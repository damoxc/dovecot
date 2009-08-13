/* Copyright (c) 2002-2009 Dovecot authors, see the included COPYING file */

#include "common.h"
#include "base64.h"
#include "buffer.h"
#include "ioloop.h"
#include "istream.h"
#include "ostream.h"
#include "randgen.h"
#include "safe-memset.h"
#include "str.h"
#include "strescape.h"
#include "client.h"
#include "client-authenticate.h"
#include "auth-client.h"
#include "ssl-proxy.h"
#include "pop3-proxy.h"
#include "hostpid.h"

/* Disconnect client when it sends too many bad commands */
#define CLIENT_MAX_BAD_COMMANDS 10

const char *login_protocol = "pop3";
const char *login_process_name = "pop3-login";
unsigned int login_default_port = 110;

static bool cmd_stls(struct pop3_client *client)
{
	client_cmd_starttls(&client->common);
	return TRUE;
}

static bool cmd_quit(struct pop3_client *client)
{
	client_send_line(&client->common, CLIENT_CMD_REPLY_OK, "Logging out");
	client_destroy(&client->common, "Aborted login");
	return TRUE;
}

static bool client_command_execute(struct pop3_client *client, const char *cmd,
				   const char *args)
{
	cmd = t_str_ucase(cmd);
	if (strcmp(cmd, "CAPA") == 0)
		return cmd_capa(client, args);
	if (strcmp(cmd, "USER") == 0)
		return cmd_user(client, args);
	if (strcmp(cmd, "PASS") == 0)
		return cmd_pass(client, args);
	if (strcmp(cmd, "AUTH") == 0)
		return cmd_auth(client, args);
	if (strcmp(cmd, "APOP") == 0)
		return cmd_apop(client, args);
	if (strcmp(cmd, "STLS") == 0)
		return cmd_stls(client);
	if (strcmp(cmd, "QUIT") == 0)
		return cmd_quit(client);

	client_send_line(&client->common, CLIENT_CMD_REPLY_BAD,
			 "Unknown command.");
	return FALSE;
}

static void pop3_client_input(struct client *client)
{
	struct pop3_client *pop3_client = (struct pop3_client *)client;
	char *line, *args;

	i_assert(!client->authenticating);

	if (!client_read(client))
		return;

	client_ref(client);

	o_stream_cork(client->output);
	/* if a command starts an authentication, stop processing further
	   commands until the authentication is finished. */
	while (!client->output->closed && !client->authenticating &&
	       (line = i_stream_next_line(client->input)) != NULL) {
		args = strchr(line, ' ');
		if (args != NULL)
			*args++ = '\0';

		if (client_command_execute(pop3_client, line,
					   args != NULL ? args : ""))
			client->bad_counter = 0;
		else if (++client->bad_counter > CLIENT_MAX_BAD_COMMANDS) {
			client_send_line(client, CLIENT_CMD_REPLY_BYE,
				"Too many invalid bad commands.");
			client_destroy(client,
				       "Disconnected: Too many bad commands");
		}
	}

	if (client_unref(client))
		o_stream_uncork(client->output);
}

static struct client *pop3_client_alloc(pool_t pool)
{
	struct pop3_client *pop3_client;

	pop3_client = p_new(pool, struct pop3_client, 1);
	return &pop3_client->common;
}

static void pop3_client_create(struct client *client ATTR_UNUSED)
{
}

static void pop3_client_destroy(struct client *client)
{
	struct pop3_client *pop3_client = (struct pop3_client *)client;

	i_free_and_null(pop3_client->last_user);
	i_free_and_null(pop3_client->apop_challenge);
}

static char *get_apop_challenge(struct pop3_client *client)
{
	struct auth_connect_id *id = &client->auth_id;
	unsigned char buffer[16];
        buffer_t *buf;

	if (!auth_client_reserve_connection(auth_client, "APOP", id))
		return NULL;

	random_fill(buffer, sizeof(buffer));
	buf = buffer_create_static_hard(pool_datastack_create(),
			MAX_BASE64_ENCODED_SIZE(sizeof(buffer)) + 1);
	base64_encode(buffer, sizeof(buffer), buf);
	buffer_append_c(buf, '\0');

	return i_strdup_printf("<%x.%x.%lx.%s@%s>",
			       id->server_pid, id->connect_uid,
			       (unsigned long)ioloop_time,
			       (const char *)buf->data, my_hostname);
}

static void pop3_client_send_greeting(struct client *client)
{
	struct pop3_client *pop3_client = (struct pop3_client *)client;

	client->io = io_add(client->fd, IO_READ, client_input, client);

	pop3_client->apop_challenge = get_apop_challenge(pop3_client);
	if (pop3_client->apop_challenge == NULL) {
		client_send_line(client, CLIENT_CMD_REPLY_OK,
				 client->set->login_greeting);
	} else {
		client_send_line(client, CLIENT_CMD_REPLY_OK,
			t_strconcat(client->set->login_greeting, " ",
				    pop3_client->apop_challenge, NULL));
	}
	client->greeting_sent = TRUE;
}

static void pop3_client_starttls(struct client *client ATTR_UNUSED)
{
}

static void
pop3_client_send_line(struct client *client, enum client_cmd_reply reply,
		      const char *text)
{
	const char *prefix = "-ERR";

	switch (reply) {
	case CLIENT_CMD_REPLY_OK:
		prefix = "+OK";
		break;
	case CLIENT_CMD_REPLY_AUTH_FAIL_TEMP:
		prefix = "-ERR [IN-USE]";
		break;
	case CLIENT_CMD_REPLY_AUTH_FAILED:
	case CLIENT_CMD_REPLY_AUTHZ_FAILED:
	case CLIENT_CMD_REPLY_AUTH_FAIL_REASON:
	case CLIENT_CMD_REPLY_AUTH_FAIL_NOSSL:
	case CLIENT_CMD_REPLY_BAD:
	case CLIENT_CMD_REPLY_BYE:
		break;
	case CLIENT_CMD_REPLY_STATUS:
	case CLIENT_CMD_REPLY_STATUS_BAD:
		/* can't send status notifications */
		return;
	}

	T_BEGIN {
		string_t *line = t_str_new(256);

		str_append(line, prefix);
		str_append_c(line, ' ');
		str_append(line, text);
		str_append(line, "\r\n");

		client_send_raw_data(client, str_data(line),
				     str_len(line));
	} T_END;
}


void clients_init(void)
{
	/* Nothing to initialize for POP3 */
}

void clients_deinit(void)
{
	clients_destroy_all();
}

struct client_vfuncs client_vfuncs = {
	pop3_client_alloc,
	pop3_client_create,
	pop3_client_destroy,
	pop3_client_send_greeting,
	pop3_client_starttls,
	pop3_client_input,
	pop3_client_send_line,
	pop3_client_auth_handle_reply,
	NULL,
	NULL,
	pop3_proxy_reset,
	pop3_proxy_parse_line
};
