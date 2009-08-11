/* Copyright (c) 2002-2009 Dovecot authors, see the included COPYING file */

#include "common.h"
#include "hostpid.h"
#include "llist.h"
#include "istream.h"
#include "ostream.h"
#include "process-title.h"
#include "str.h"
#include "str-sanitize.h"
#include "safe-memset.h"
#include "var-expand.h"
#include "master-service.h"
#include "master-auth.h"
#include "auth-client.h"
#include "login-proxy.h"
#include "ssl-proxy.h"
#include "client-common.h"

#include <stdlib.h>

/* When max. number of simultaneous connections is reached, few of the
   oldest connections are disconnected. Since we have to go through all of the
   clients, it's faster if we disconnect multiple clients. */
#define CLIENT_DESTROY_OLDEST_COUNT 16

struct client *clients = NULL;
static unsigned int clients_count = 0;

static void client_idle_disconnect_timeout(struct client *client)
{
	client_send_line(client, CLIENT_CMD_REPLY_BAD,
			 "Disconnected for inactivity.");
	client_destroy(client, "Disconnected: Inactivity");
}

static void client_open_streams(struct client *client)
{
	client->input =
		i_stream_create_fd(client->fd, LOGIN_MAX_INBUF_SIZE, FALSE);
	client->output =
		o_stream_create_fd(client->fd, LOGIN_MAX_OUTBUF_SIZE, FALSE);
}

struct client *client_create(int fd, bool ssl, pool_t pool,
			     const struct login_settings *set,
			     const struct ip_addr *local_ip,
			     const struct ip_addr *remote_ip)
{
	struct client *client;

	i_assert(fd != -1);

	if (clients_get_count() >= set->login_max_connections) {
		/* reached max. users count, kill few of the
		   oldest connections */
		client_destroy_oldest();
	}

	/* always use nonblocking I/O */
	net_set_nonblock(fd, TRUE);

	client = client_vfuncs.alloc(pool);
	client->v = client_vfuncs;
	client->created = ioloop_time;
	client->refcount = 1;

	client->pool = pool;
	client->set = set;
	client->local_ip = *local_ip;
	client->ip = *remote_ip;
	client->fd = fd;
	client->tls = ssl;
	client->trusted = client_is_trusted(client);
	client->secured = ssl || client->trusted ||
		net_ip_compare(remote_ip, local_ip);

	DLLIST_PREPEND(&clients, client);
	clients_count++;

	client_set_title(client);

	client->to_idle_disconnect =
		timeout_add(CLIENT_LOGIN_IDLE_TIMEOUT_MSECS,
			    client_idle_disconnect_timeout, client);
	client_open_streams(client);

	client->v.create(client);

	if (auth_client_is_connected(auth_client))
		client->v.send_greeting(client);
	else
		client_set_auth_waiting(client);
	return client;
}

void client_destroy(struct client *client, const char *reason)
{
	if (client->destroyed)
		return;
	client->destroyed = TRUE;

	if (!client->login_success && reason != NULL) {
		reason = t_strconcat(reason, " ",
			client_get_extra_disconnect_reason(client), NULL);
	}
	if (reason != NULL)
		client_log(client, reason);

	i_assert(clients_count > 0);
	clients_count--;
	DLLIST_REMOVE(&clients, client);

	if (client->input != NULL)
		i_stream_close(client->input);
	if (client->output != NULL)
		o_stream_close(client->output);

	if (client->master_tag != 0) {
		i_assert(client->auth_request == NULL);
		i_assert(client->authenticating);
		master_auth_request_abort(master_service, client->master_tag);
	} else if (client->auth_request != NULL) {
		i_assert(client->authenticating);
		sasl_server_auth_abort(client);
	} else {
		i_assert(!client->authenticating);
	}

	if (client->io != NULL)
		io_remove(&client->io);
	if (client->to_idle_disconnect != NULL)
		timeout_remove(&client->to_idle_disconnect);
	if (client->to_auth_waiting != NULL)
		timeout_remove(&client->to_auth_waiting);
	if (client->to_authfail_delay != NULL)
		timeout_remove(&client->to_authfail_delay);

	if (client->fd != -1) {
		net_disconnect(client->fd);
		client->fd = -1;
	}

	if (client->proxy_password != NULL) {
		safe_memset(client->proxy_password, 0,
			    strlen(client->proxy_password));
		i_free_and_null(client->proxy_password);
	}

	i_free_and_null(client->proxy_user);
	i_free_and_null(client->proxy_master_user);

	if (client->login_proxy != NULL)
		login_proxy_free(&client->login_proxy);
	if (client->ssl_proxy != NULL)
		ssl_proxy_free(&client->ssl_proxy);
	client->v.destroy(client);
	client_unref(client);
}

void client_destroy_success(struct client *client, const char *reason)
{
	client->login_success = TRUE;
	client_destroy(client, reason);
}

void client_destroy_internal_failure(struct client *client)
{
	client_send_line(client, CLIENT_CMD_REPLY_AUTH_FAIL_TEMP,
			 "Internal login failure. "
			 "Refer to server log for more information.");
	client_destroy(client, "Internal login failure");
}

void client_ref(struct client *client)
{
	client->refcount++;
}

bool client_unref(struct client *client)
{
	i_assert(client->refcount > 0);
	if (--client->refcount > 0)
		return TRUE;

	i_assert(client->destroyed);

	if (client->input != NULL)
		i_stream_unref(&client->input);
	if (client->output != NULL)
		o_stream_unref(&client->output);

	if (!client->proxying) {
		i_assert(client->ssl_proxy == NULL);
		master_service_client_connection_destroyed(master_service);
	}

	i_free(client->virtual_user);
	i_free(client->auth_mech_name);
	pool_unref(&client->pool);
	return FALSE;
}

void client_destroy_oldest(void)
{
	unsigned int max_connections =
		global_login_settings->login_max_connections;
	struct client *client;
	struct client *destroy_buf[CLIENT_DESTROY_OLDEST_COUNT];
	unsigned int i, destroy_count;

	/* find the oldest clients and put them to destroy-buffer */
	memset(destroy_buf, 0, sizeof(destroy_buf));

	destroy_count = max_connections > CLIENT_DESTROY_OLDEST_COUNT*2 ?
		CLIENT_DESTROY_OLDEST_COUNT : I_MIN(max_connections/2, 1);
	for (client = clients; client != NULL; client = client->next) {
		for (i = 0; i < destroy_count; i++) {
			if (destroy_buf[i] == NULL ||
			    destroy_buf[i]->created > client->created) {
				/* @UNSAFE */
				memmove(destroy_buf+i+1, destroy_buf+i,
					sizeof(destroy_buf) -
					(i+1) * sizeof(destroy_buf[0]));
				destroy_buf[i] = client;
				break;
			}
		}
	}

	/* then kill them */
	for (i = 0; i < destroy_count; i++) {
		if (destroy_buf[i] == NULL)
			break;

		client_destroy(destroy_buf[i],
			       "Disconnected: Connection queue full");
	}
}

void clients_destroy_all(void)
{
	struct client *client, *next;

	for (client = clients; client != NULL; client = next) {
		next = client->next;
		client_destroy(client, "Disconnected: Shutting down");
	}
}

static void client_start_tls(struct client *client)
{
	int fd_ssl;

	client_ref(client);
	if (!client_unref(client) || client->destroyed)
		return;

	fd_ssl = ssl_proxy_new(client->fd, &client->ip,
			       client->set, &client->ssl_proxy);
	if (fd_ssl == -1) {
		client_send_line(client, CLIENT_CMD_REPLY_BYE,
				 "TLS initialization failed.");
		client_destroy(client,
			       "Disconnected: TLS initialization failed.");
		return;
	}

	client->starttls = TRUE;
	client->proxying = TRUE;
	client->tls = TRUE;
	client->secured = TRUE;
	client_set_title(client);

	client->fd = fd_ssl;
	client->io = io_add(client->fd, IO_READ, client_input, client);
	i_stream_unref(&client->input);
	o_stream_unref(&client->output);
	client_open_streams(client);

	client->v.starttls(client);
}

static int client_output_starttls(struct client *client)
{
	int ret;

	if ((ret = o_stream_flush(client->output)) < 0) {
		client_destroy(client, "Disconnected");
		return 1;
	}

	if (ret > 0) {
		o_stream_unset_flush_callback(client->output);
		client_start_tls(client);
	}
	return 1;
}

void client_cmd_starttls(struct client *client)
{
	if (client->tls) {
		client_send_line(client, CLIENT_CMD_REPLY_BAD,
				 "TLS is already active.");
		return;
	}

	if (!ssl_initialized) {
		client_send_line(client, CLIENT_CMD_REPLY_BAD,
				 "TLS support isn't enabled.");
		return;
	}

	/* remove input handler, SSL proxy gives us a new fd. we also have to
	   remove it in case we have to wait for buffer to be flushed */
	if (client->io != NULL)
		io_remove(&client->io);

	client_send_line(client, CLIENT_CMD_REPLY_OK,
			 "Begin TLS negotiation now.");

	/* uncork the old fd */
	o_stream_uncork(client->output);

	if (o_stream_flush(client->output) <= 0) {
		/* the buffer has to be flushed */
		o_stream_set_flush_pending(client->output, TRUE);
		o_stream_set_flush_callback(client->output,
					    client_output_starttls, client);
	} else {
		client_start_tls(client);
	}
}

unsigned int clients_get_count(void)
{
	return clients_count;
}

void client_set_title(struct client *client)
{
	const char *addr;

	if (!client->set->verbose_proctitle ||
	    !client->set->login_process_per_connection)
		return;

	addr = net_ip2addr(&client->ip);
	if (addr == NULL)
		addr = "??";

	process_title_set(t_strdup_printf(client->tls ?
					  "[%s TLS]" : "[%s]", addr));
}

static const struct var_expand_table *
get_var_expand_table(struct client *client)
{
	static struct var_expand_table static_tab[] = {
		{ 'u', NULL, "user" },
		{ 'n', NULL, "username" },
		{ 'd', NULL, "domain" },
		{ 's', NULL, "service" },
		{ 'h', NULL, "home" },
		{ 'l', NULL, "lip" },
		{ 'r', NULL, "rip" },
		{ 'p', NULL, "pid" },
		{ 'm', NULL, "mech" },
		{ 'a', NULL, "lport" },
		{ 'b', NULL, "rport" },
		{ 'c', NULL, "secured" },
		{ 'k', NULL, "ssl_security" },
		{ 'e', NULL, "mail_pid" },
		{ '\0', NULL, NULL }
	};
	struct var_expand_table *tab;
	unsigned int i;

	tab = t_malloc(sizeof(static_tab));
	memcpy(tab, static_tab, sizeof(static_tab));

	if (client->virtual_user != NULL) {
		tab[0].value = client->virtual_user;
		tab[1].value = t_strcut(client->virtual_user, '@');
		tab[2].value = strchr(client->virtual_user, '@');
		if (tab[2].value != NULL) tab[2].value++;

		for (i = 0; i < 3; i++)
			tab[i].value = str_sanitize(tab[i].value, 80);
	}
	tab[3].value = login_protocol;
	tab[4].value = getenv("HOME");
	tab[5].value = net_ip2addr(&client->local_ip);
	tab[6].value = net_ip2addr(&client->ip);
	tab[7].value = my_pid;
	tab[8].value = client->auth_mech_name == NULL ? NULL :
		str_sanitize(client->auth_mech_name, MAX_MECH_NAME);
	tab[9].value = dec2str(client->local_port);
	tab[10].value = dec2str(client->remote_port);
	if (!client->tls) {
		tab[11].value = client->secured ? "secured" : NULL;
		tab[12].value = "";
	} else {
		const char *ssl_state =
			ssl_proxy_is_handshaked(client->ssl_proxy) ?
			"TLS" : "TLS handshaking";
		const char *ssl_error =
			ssl_proxy_get_last_error(client->ssl_proxy);

		tab[11].value = ssl_error == NULL ? ssl_state :
			t_strdup_printf("%s: %s", ssl_state, ssl_error);
		tab[12].value =
			ssl_proxy_get_security_string(client->ssl_proxy);
	}
	tab[13].value = dec2str(client->mail_pid);
	return tab;
}

static bool have_key(const struct var_expand_table *table, const char *str)
{
	char key;
	unsigned int i;

	key = var_get_key(str);
	for (i = 0; table[i].key != '\0'; i++) {
		if (table[i].key == key) {
			return table[i].value != NULL &&
				table[i].value[0] != '\0';
		}
	}
	return FALSE;
}

static const char *
client_get_log_str(struct client *client, const char *msg)
{
	static struct var_expand_table static_tab[3] = {
		{ 's', NULL, NULL },
		{ '$', NULL, NULL },
		{ '\0', NULL, NULL }
	};
	const struct var_expand_table *var_expand_table;
	struct var_expand_table *tab;
	const char *p;
	char *const *e;
	string_t *str;

	var_expand_table = get_var_expand_table(client);

	tab = t_malloc(sizeof(static_tab));
	memcpy(tab, static_tab, sizeof(static_tab));

	str = t_str_new(256);
	for (e = client->set->log_format_elements_split; *e != NULL; e++) {
		for (p = *e; *p != '\0'; p++) {
			if (*p != '%' || p[1] == '\0')
				continue;

			p++;
			if (have_key(var_expand_table, p)) {
				if (str_len(str) > 0)
					str_append(str, ", ");
				var_expand(str, *e, var_expand_table);
				break;
			}
		}
	}

	tab[0].value = t_strdup(str_c(str));
	tab[1].value = msg;
	str_truncate(str, 0);

	var_expand(str, client->set->login_log_format, tab);
	return str_c(str);
}

void client_log(struct client *client, const char *msg)
{
	T_BEGIN {
		i_info("%s", client_get_log_str(client, msg));
	} T_END;
}

void client_log_err(struct client *client, const char *msg)
{
	T_BEGIN {
		i_error("%s", client_get_log_str(client, msg));
	} T_END;
}

bool client_is_trusted(struct client *client)
{
	const char *const *net;
	struct ip_addr net_ip;
	unsigned int bits;

	if (client->set->login_trusted_networks == NULL)
		return FALSE;

	net = t_strsplit_spaces(client->set->login_trusted_networks, ", ");
	for (; *net != NULL; net++) {
		if (net_parse_range(*net, &net_ip, &bits) < 0) {
			i_error("login_trusted_networks: "
				"Invalid network '%s'", *net);
			break;
		}

		if (net_is_in_network(&client->ip, &net_ip, bits))
			return TRUE;
	}
	return FALSE;
}

const char *client_get_extra_disconnect_reason(struct client *client)
{
	if (client->set->ssl_require_client_cert && client->ssl_proxy != NULL) {
		if (ssl_proxy_has_broken_client_cert(client->ssl_proxy))
			return "(client sent an invalid cert)";
		if (!ssl_proxy_has_valid_client_cert(client->ssl_proxy))
			return "(client didn't send a cert)";
	}

	if (client->auth_attempts == 0)
		return "(no auth attempts)";

	/* some auth attempts without SSL/TLS */
	if (client->auth_tried_disabled_plaintext)
		return "(tried to use disabled plaintext auth)";
	if (client->set->ssl_require_client_cert)
		return "(cert required, client didn't start TLS)";

	return t_strdup_printf("(auth failed, %u attempts)",
			       client->auth_attempts);
}

void client_send_line(struct client *client, enum client_cmd_reply reply,
		      const char *text)
{
	client->v.send_line(client, reply, text);
}

void client_send_raw_data(struct client *client, const void *data, size_t size)
{
	ssize_t ret;

	ret = o_stream_send(client->output, data, size);
	if (ret < 0 || (size_t)ret != size) {
		/* either disconnection or buffer full. in either case we want
		   this connection destroyed. however destroying it here might
		   break things if client is still tried to be accessed without
		   being referenced.. */
		i_stream_close(client->input);
	}
}

void client_send_raw(struct client *client, const char *data)
{
	client_send_raw_data(client, data, strlen(data));
}

bool client_read(struct client *client)
{
	switch (i_stream_read(client->input)) {
	case -2:
		/* buffer full */
		client_send_line(client, CLIENT_CMD_REPLY_BYE,
				 "Input buffer full, aborting");
		client_destroy(client, "Disconnected: Input buffer full");
		return FALSE;
	case -1:
		/* disconnected */
		client_destroy(client, "Disconnected");
		return FALSE;
	case 0:
		/* nothing new read */
		return TRUE;
	default:
		/* something was read */
		timeout_reset(client->to_idle_disconnect);
		return TRUE;
	}
}

void client_input(struct client *client)
{
	client->v.input(client);
}
