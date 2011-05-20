/* Copyright (c) 2010-2011 Dovecot authors, see the included COPYING file */

#include "lib.h"
#include "base64.h"
#include "ioloop.h"
#include "istream.h"
#include "ostream.h"
#include "strescape.h"
#include "settings-parser.h"
#include "master-service.h"
#include "master-service-settings.h"
#include "mail-storage-service.h"
#include "doveadm-util.h"
#include "doveadm-server.h"
#include "doveadm-mail.h"
#include "doveadm-print.h"
#include "doveadm-settings.h"
#include "client-connection.h"

#include <unistd.h>

#define MAX_INBUF_SIZE 1024

struct client_connection {
	pool_t pool;

	int fd;
	struct io *io;
	struct istream *input;
	struct ostream *output;
	struct ip_addr local_ip, remote_ip;
	const struct doveadm_settings *set;

	unsigned int handshaked:1;
	unsigned int authenticated:1;
};

static bool
doveadm_mail_cmd_server(const char *cmd_name,
			const struct doveadm_settings *set,
			const struct mail_storage_service_input *input,
			int argc, char *argv[])
{
	enum mail_storage_service_flags service_flags =
		MAIL_STORAGE_SERVICE_FLAG_NO_LOG_INIT |
		MAIL_STORAGE_SERVICE_FLAG_USERDB_LOOKUP;
	struct doveadm_mail_cmd_context *ctx;
	const struct doveadm_mail_cmd *cmd;
	const char *getopt_args;
	bool add_username_header = FALSE;
	int c;

	cmd = doveadm_mail_cmd_find(cmd_name);
	if (cmd == NULL) {
		i_error("doveadm: Client sent unknown command: %s", cmd_name);
		return FALSE;
	}

	if (doveadm_debug)
		service_flags |= MAIL_STORAGE_SERVICE_FLAG_DEBUG;

	ctx = doveadm_mail_cmd_init(cmd, set);
	getopt_args = t_strconcat("Au:", ctx->getopt_args, NULL);
	while ((c = getopt(argc, argv, getopt_args)) > 0) {
		switch (c) {
		case 'A':
			add_username_header = TRUE;
			break;
		case 'u':
			if (strchr(optarg, '*') != NULL ||
			    strchr(optarg, '?') != NULL)
				add_username_header = TRUE;
			break;
		default:
			if ((ctx->v.parse_arg == NULL ||
			     !ctx->v.parse_arg(ctx, c))) {
				i_error("doveadm %s: "
					"Client sent unknown parameter: %c",
					cmd->name, c);
				ctx->v.deinit(ctx);
				return FALSE;
			}
		}
	}

	argv += optind-1;
	optind = 1;

	if (argv[0] != NULL && cmd->usage_args == NULL) {
		i_error("doveadm %s: Client sent unknown parameter: %s",
			cmd->name, argv[0]);
		ctx->v.deinit(ctx);
		return FALSE;
	}

	if (doveadm_print_is_initialized() && add_username_header) {
		doveadm_print_header("username", "Username",
				     DOVEADM_PRINT_HEADER_FLAG_STICKY |
				     DOVEADM_PRINT_HEADER_FLAG_HIDE_TITLE);
		doveadm_print_sticky("username", input->username);
	}

	doveadm_mail_single_user(ctx, argv, input, service_flags);
	ctx->v.deinit(ctx);
	doveadm_print_flush();
	return !ctx->failed;
}

static bool client_handle_command(struct client_connection *conn, char **args)
{
	struct mail_storage_service_input input;
	const char *flags, *cmd_name;
	unsigned int argc;
	bool ret;

	memset(&input, 0, sizeof(input));
	input.service = "doveadm";

	for (argc = 0; args[argc] != NULL; argc++)
		args[argc] = str_tabunescape(args[argc]);

	if (argc < 3) {
		i_error("doveadm client: No command given");
		return FALSE;
	}
	flags = args[0];
	input.username = args[1];
	cmd_name = args[2];
	args += 3;
	argc -= 3;

	doveadm_debug = FALSE;
	doveadm_verbose = FALSE;

	for (; *flags != '\0'; flags++) {
		switch (*flags) {
		case 'D':
			doveadm_debug = TRUE;
			doveadm_verbose = TRUE;
			break;
		case 'v':
			doveadm_verbose = TRUE;
			break;
		default:
			i_error("doveadm client: Unknown flag: %c", *flags);
			return FALSE;
		}
	}

	o_stream_cork(conn->output);
	ret = doveadm_mail_cmd_server(cmd_name, conn->set, &input, argc, args);
	if (ret)
		o_stream_send(conn->output, "\n+\n", 3);
	else
		o_stream_send(conn->output, "\n-\n", 3);
	o_stream_uncork(conn->output);

	/* flush the output and disconnect */
	net_set_nonblock(conn->fd, FALSE);
	(void)o_stream_flush(conn->output);
	net_set_nonblock(conn->fd, TRUE);
	return TRUE;
}

static int
client_connection_authenticate(struct client_connection *conn)
{
	const char *line, *pass;
	buffer_t *plain;
	const unsigned char *data;
	size_t size;

	if ((line = i_stream_read_next_line(conn->input)) == NULL)
		return 0;

	if (*conn->set->doveadm_password == '\0') {
		i_error("doveadm_password not set, "
			"remote authentication disabled");
		return -1;
	}

	/* FIXME: some day we should probably let auth process do this and
	   support all kinds of authentication */
	if (strncmp(line, "PLAIN\t", 6) != 0) {
		i_error("doveadm client attempted non-PLAIN authentication");
		return -1;
	}

	plain = buffer_create_dynamic(pool_datastack_create(), 128);
	if (base64_decode(line + 6, strlen(line + 6), NULL, plain) < 0) {
		i_error("doveadm client sent invalid base64 auth PLAIN data");
		return -1;
	}
	data = plain->data;
	size = plain->used;

	if (size < 10 || data[0] != '\0' ||
	    memcmp(data+1, "doveadm", 7) != 0 || data[8] != '\0') {
		i_error("doveadm client didn't authenticate as 'doveadm'");
		return -1;
	}
	pass = t_strndup(data + 9, size - 9);
	if (strcmp(pass, conn->set->doveadm_password) != 0) {
		i_error("doveadm client authenticated with wrong password");
		return -1;
	}
	return 1;
}

static void client_connection_input(struct client_connection *conn)
{
	const char *line;
	bool ok = TRUE;
	int ret;

	if (!conn->handshaked) {
		if ((line = i_stream_read_next_line(conn->input)) == NULL) {
			if (conn->input->eof || conn->input->stream_errno != 0)
				client_connection_destroy(&conn);
			return;
		}

		if (!version_string_verify(line, "doveadm-server",
				DOVEADM_SERVER_PROTOCOL_VERSION_MAJOR)) {
			i_error("doveadm client not compatible with this server "
				"(mixed old and new binaries?)");
			client_connection_destroy(&conn);
			return;
		}
		conn->handshaked = TRUE;
	}
	if (!conn->authenticated) {
		if ((ret = client_connection_authenticate(conn)) <= 0) {
			if (ret < 0)
				client_connection_destroy(&conn);
			return;
		}
		conn->authenticated = TRUE;
	}

	while (ok && (line = i_stream_read_next_line(conn->input)) != NULL) {
		T_BEGIN {
			char **args;

			args = p_strsplit(pool_datastack_create(), line, "\t");
			ok = client_handle_command(conn, args);
		} T_END;
	}
	if (conn->input->eof || conn->input->stream_errno != 0 || !ok)
		client_connection_destroy(&conn);
}

static int client_connection_read_settings(struct client_connection *conn)
{
	const struct setting_parser_info *set_roots[] = {
		&doveadm_setting_parser_info,
		NULL
	};
	struct master_service_settings_input input;
	struct master_service_settings_output output;
	const char *error;
	void *set;

	memset(&input, 0, sizeof(input));
	input.roots = set_roots;
	input.service = "doveadm";
	input.local_ip = conn->local_ip;
	input.remote_ip = conn->remote_ip;

	if (master_service_settings_read(master_service, &input,
					 &output, &error) < 0) {
		i_error("Error reading configuration: %s", error);
		return -1;
	}
	set = master_service_settings_get_others(master_service)[0];
	conn->set = settings_dup(&doveadm_setting_parser_info, set, conn->pool);
	return 0;
}

struct client_connection *client_connection_create(int fd, int listen_fd)
{
	struct client_connection *conn;
	struct stat st;
	const char *listen_path;
	unsigned int port;
	pool_t pool;

	pool = pool_alloconly_create("doveadm client", 1024*16);
	conn = p_new(pool, struct client_connection, 1);
	conn->pool = pool;
	conn->fd = fd;
	conn->io = io_add(fd, IO_READ, client_connection_input, conn);
	conn->input = i_stream_create_fd(fd, MAX_INBUF_SIZE, FALSE);
	conn->output = o_stream_create_fd(fd, (size_t)-1, FALSE);

	(void)net_getsockname(fd, &conn->local_ip, &port);
	(void)net_getpeername(fd, &conn->remote_ip, &port);

	/* we'll have to do this with stat(), because at least in Linux
	   fstat() always returns mode as 0777 */
	if (net_getunixname(listen_fd, &listen_path) == 0 &&
	    stat(listen_path, &st) == 0 && S_ISSOCK(st.st_mode) &&
	    (st.st_mode & 0777) == 0600 && st.st_uid == geteuid()) {
		/* no need for client to authenticate */
		conn->authenticated = TRUE;
		o_stream_send(conn->output, "+\n", 2);
	} else {
		o_stream_send(conn->output, "-\n", 2);
	}
	if (client_connection_read_settings(conn) < 0)
		client_connection_destroy(&conn);
	return conn;
}

void client_connection_destroy(struct client_connection **_conn)
{
	struct client_connection *conn = *_conn;

	*_conn = NULL;

	i_stream_destroy(&conn->input);
	o_stream_destroy(&conn->output);
	io_remove(&conn->io);
	if (close(conn->fd) < 0)
		i_error("close(client) failed: %m");
	pool_unref(&conn->pool);

	doveadm_client = NULL;
	master_service_client_connection_destroyed(master_service);
}

struct ostream *client_connection_get_output(struct client_connection *conn)
{
	return conn->output;
}
