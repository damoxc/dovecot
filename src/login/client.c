/* Copyright (C) 2002 Timo Sirainen */

#include "common.h"
#include "buffer.h"
#include "hash.h"
#include "ioloop.h"
#include "istream.h"
#include "ostream.h"
#include "process-title.h"
#include "safe-memset.h"
#include "strescape.h"
#include "client.h"
#include "client-authenticate.h"
#include "ssl-proxy.h"

#include <syslog.h>

/* Disconnect client after idling this many seconds */
#define CLIENT_LOGIN_IDLE_TIMEOUT 60

/* When max. number of simultaneous connections is reached, few of the
   oldest connections are disconnected. Since we have to go through the whole
   client hash, it's faster if we disconnect multiple clients. */
#define CLIENT_DESTROY_OLDEST_COUNT 16

static HashTable *clients;
static Timeout to_idle;

static void client_set_title(Client *client)
{
	const char *host;

	if (!verbose_proctitle || !process_per_connection)
		return;

	host = net_ip2host(&client->ip);
	if (host == NULL)
		host = "??";

	process_title_set(t_strdup_printf(client->tls ? "[%s TLS]" : "[%s]",
					  host));
}

static int cmd_capability(Client *client)
{
	const char *capability;

	capability = t_strconcat("* CAPABILITY " CAPABILITY_STRING,
				 ssl_initialized ? " STARTTLS" : "",
				 disable_plaintext_auth && !client->tls ?
				 " LOGINDISABLED" : "",
				 client_authenticate_get_capabilities(),
				 NULL);
	client_send_line(client, capability);
	client_send_tagline(client, "OK Capability completed.");
	return TRUE;
}

static int cmd_starttls(Client *client)
{
	int fd_ssl;

	if (client->tls) {
		client_send_tagline(client, "BAD TLS is already active.");
		return TRUE;
	}

	if (!ssl_initialized) {
		client_send_tagline(client, "BAD TLS support isn't enabled.");
		return TRUE;
	}

	client_send_tagline(client, "OK Begin TLS negotiation now.");
	o_stream_flush(client->output);

	/* must be removed before ssl_proxy_new(), since it may
	   io_add() the same fd. */
	if (client->io != NULL) {
		io_remove(client->io);
		client->io = NULL;
	}

	fd_ssl = ssl_proxy_new(client->fd);
	if (fd_ssl != -1) {
		client->tls = TRUE;
                client_set_title(client);

		client->fd = fd_ssl;

		i_stream_unref(client->input);
		o_stream_unref(client->output);

		client->input = i_stream_create_file(fd_ssl, default_pool,
						     8192, FALSE);
		client->output = o_stream_create_file(fd_ssl, default_pool,
						      1024, IO_PRIORITY_DEFAULT,
						      FALSE);
	} else {
		client_send_line(client, " * BYE TLS handehake failed.");
		client_destroy(client, "TLS handshake failed");
	}

	client->io = io_add(client->fd, IO_READ, client_input, client);
	return TRUE;
}

static int cmd_noop(Client *client)
{
	client_send_tagline(client, "OK NOOP completed.");
	return TRUE;
}

static int cmd_logout(Client *client)
{
	client_send_line(client, "* BYE Logging out");
	client_send_tagline(client, "OK Logout completed.");
	client_destroy(client, "Aborted login");
	return TRUE;
}

int client_read(Client *client)
{
	switch (i_stream_read(client->input)) {
	case -2:
		/* buffer full */
		client_send_line(client, "* BYE Input buffer full, aborting");
		client_destroy(client, "Disconnected: Input buffer full");
		return FALSE;
	case -1:
		/* disconnected */
		client_destroy(client, "Disconnected");
		return FALSE;
	default:
		/* something was read */
		return TRUE;
	}
}

static char *get_next_arg(char **linep)
{
	char *line, *start;
	int quoted;

	line = *linep;
	while (*line == ' ') line++;

	/* @UNSAFE: get next argument, unescape arg if it's quoted */
	if (*line == '"') {
		quoted = TRUE;
		line++;

		start = line;
		while (*line != '\0' && *line != '"') {
			if (*line == '\\' && line[1] != '\0')
				line++;
			line++;
		}

		if (*line == '"')
			*line++ = '\0';
		str_unescape(start);
	} else {
		start = line;
		while (*line != '\0' && *line != ' ')
			line++;

		if (*line == ' ')
			*line++ = '\0';
	}

	*linep = line;
	return start;
}

static int client_command_execute(Client *client, char *line)
{
	char *cmd;
	int ret;

	cmd = get_next_arg(&line);
	str_ucase(cmd);

	if (strcmp(cmd, "LOGIN") == 0) {
		char *user, *pass;

		user = get_next_arg(&line);
		pass = get_next_arg(&line);
		ret = cmd_login(client, user, pass);

		safe_memset(pass, 0, strlen(pass));
		return ret;
	}
	if (strcmp(cmd, "AUTHENTICATE") == 0)
		return cmd_authenticate(client, get_next_arg(&line));
	if (strcmp(cmd, "CAPABILITY") == 0)
		return cmd_capability(client);
	if (strcmp(cmd, "STARTTLS") == 0)
		return cmd_starttls(client);
	if (strcmp(cmd, "NOOP") == 0)
		return cmd_noop(client);
	if (strcmp(cmd, "LOGOUT") == 0)
		return cmd_logout(client);

	return FALSE;
}

void client_input(void *context, int fd __attr_unused__,
		  IO io __attr_unused__)
{
	Client *client = context;
	char *line;

	client->last_input = ioloop_time;

	i_free(client->tag);
	client->tag = i_strdup("*");

	if (!client_read(client))
		return;

	client_ref(client);
	o_stream_cork(client->output);

	while ((line = i_stream_next_line(client->input)) != NULL) {
		/* split the arguments, make sure we have at
		   least tag + command */
		i_free(client->tag);
		client->tag = i_strdup(get_next_arg(&line));

		if (*client->tag == '\0' ||
		    !client_command_execute(client, line)) {
			/* error */
			client_send_tagline(client, "BAD Error in IMAP command "
					    "received by server.");
		}
	}

	if (client_unref(client))
		o_stream_flush(client->output);
}

static void client_hash_destroy_oldest(void *key, void *value __attr_unused__,
				       void *context)
{
	Client *client = key;
	Buffer *destroy_buf = context;
	Client *const *destroy_clients;
	size_t i, count;

	destroy_clients = buffer_get_data(destroy_buf, &count);
	count /= sizeof(Client *);

	for (i = 0; i < count; i++) {
		if (destroy_clients[i]->created > client->created) {
			buffer_insert(destroy_buf, i * sizeof(Client *),
				      &client, sizeof(client));
			break;
		}
	}
}

static void client_destroy_oldest(void)
{
	Buffer *destroy_buf;
	Client *const *destroy_clients;
	size_t i, count;

	/* find the oldest clients and put them to destroy-buffer */
	destroy_buf = buffer_create_static_hard(data_stack_pool,
						sizeof(Client *) *
						CLIENT_DESTROY_OLDEST_COUNT);
	hash_foreach(clients, client_hash_destroy_oldest, destroy_buf);

	/* then kill them */
	destroy_clients = buffer_get_data(destroy_buf, &count);
	count /= sizeof(Client *);

	for (i = 0; i < count; i++) {
		client_destroy(destroy_clients[i],
			       "Disconnected: Connection queue full");
	}
}

Client *client_create(int fd, IPADDR *ip, int imaps)
{
	Client *client;

	if (max_logging_users > CLIENT_DESTROY_OLDEST_COUNT &&
	    hash_size(clients) >= max_logging_users) {
		/* reached max. users count, kill few of the
		   oldest connections */
		client_destroy_oldest();
	}

	/* always use nonblocking I/O */
	net_set_nonblock(fd, TRUE);

	client = i_new(Client, 1);
	client->created = ioloop_time;
	client->refcount = 1;
	client->tls = imaps;

	memcpy(&client->ip, ip, sizeof(IPADDR));
	client->fd = fd;
	client->io = io_add(fd, IO_READ, client_input, client);
	client->input = i_stream_create_file(fd, default_pool, 8192, FALSE);
	client->output = o_stream_create_file(fd, default_pool, 1024,
					      IO_PRIORITY_DEFAULT, FALSE);
	client->plain_login = buffer_create_dynamic(system_pool, 128, 8192);
	client->last_input = ioloop_time;
	hash_insert(clients, client, client);

	main_ref();

	client_send_line(client, "* OK " PACKAGE " ready.");
	client_set_title(client);
	return client;
}

void client_destroy(Client *client, const char *reason)
{
	if (reason != NULL)
		client_syslog(client, reason);

	hash_remove(clients, client);

	i_stream_close(client->input);
	o_stream_close(client->output);

	if (client->io != NULL) {
		io_remove(client->io);
		client->io = NULL;
	}

	net_disconnect(client->fd);
	client->fd = -1;

	client_unref(client);
}

void client_ref(Client *client)
{
	client->refcount++;
}

int client_unref(Client *client)
{
	if (--client->refcount > 0)
		return TRUE;

	i_stream_unref(client->input);
	o_stream_unref(client->output);

	i_free(client->tag);
	buffer_free(client->plain_login);
	i_free(client);

	main_unref();
	return FALSE;
}

void client_send_line(Client *client, const char *line)
{
	o_stream_send_str(client->output, line);
	o_stream_send(client->output, "\r\n", 2);
}

void client_send_tagline(Client *client, const char *line)
{
	client_send_line(client, t_strconcat(client->tag, " ", line, NULL));
}

void client_syslog(Client *client, const char *text)
{
	const char *host;

	host = net_ip2host(&client->ip);
	if (host == NULL)
		host = "??";

	i_info("%s [%s]", text, host);
}

static void client_hash_check_idle(void *key, void *value __attr_unused__,
				   void *context __attr_unused__)
{
	Client *client = key;

	if (ioloop_time - client->last_input >= CLIENT_LOGIN_IDLE_TIMEOUT) {
		client_send_line(client, "* BYE Disconnected for inactivity.");
		client_destroy(client, "Disconnected: Inactivity");
	}
}

static void idle_timeout(void *context __attr_unused__,
			 Timeout timeout __attr_unused__)
{
	hash_foreach(clients, client_hash_check_idle, NULL);
}

unsigned int clients_get_count(void)
{
	return hash_size(clients);
}

static void client_hash_destroy(void *key, void *value __attr_unused__,
				void *context __attr_unused__)
{
	client_destroy(key, NULL);
}

void clients_destroy_all(void)
{
	hash_foreach(clients, client_hash_destroy, NULL);
}

void clients_init(void)
{
	clients = hash_create(default_pool, 128, NULL, NULL);
	to_idle = timeout_add(1000, idle_timeout, NULL);
}

void clients_deinit(void)
{
	clients_destroy_all();
	hash_destroy(clients);

	timeout_remove(to_idle);
}
