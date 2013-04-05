/* Copyright (c) 2009-2013 Dovecot authors, see the included COPYING file */

#include "lib.h"
#include "lib-signals.h"
#include "array.h"
#include "execv-const.h"
#include "fd-set-nonblock.h"
#include "istream.h"
#include "ostream.h"
#include "iostream-ssl.h"
#include "iostream-rawlog.h"
#include "write-full.h"
#include "str.h"
#include "strescape.h"
#include "var-expand.h"
#include "settings-parser.h"
#include "master-service.h"
#include "master-service-ssl-settings.h"
#include "mail-storage-service.h"
#include "mail-user.h"
#include "mail-namespace.h"
#include "mailbox-list.h"
#include "doveadm-settings.h"
#include "doveadm-mail.h"
#include "doveadm-print.h"
#include "doveadm-server.h"
#include "client-connection.h"
#include "server-connection.h"
#include "dsync-brain.h"
#include "dsync-ibc.h"
#include "doveadm-dsync.h"

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <ctype.h>
#include <sys/wait.h>

#define DSYNC_COMMON_GETOPT_ARGS "+dEfg:l:m:n:Nr:Rs:"
#define DSYNC_REMOTE_CMD_EXIT_WAIT_SECS 30

enum dsync_run_type {
	DSYNC_RUN_TYPE_LOCAL,
	DSYNC_RUN_TYPE_STREAM,
	DSYNC_RUN_TYPE_CMD
};

struct dsync_cmd_context {
	struct doveadm_mail_cmd_context ctx;
	enum dsync_brain_sync_type sync_type;
	const char *mailbox, *namespace_prefix;
	guid_128_t mailbox_guid;
	const char *state_input, *rawlog_path;

	const char *remote_name;
	const char *local_location;
	pid_t remote_pid;

	int fd_in, fd_out, fd_err;
	struct io *io_err;
	struct istream *input, *err_stream;
	struct ostream *output;

	struct ssl_iostream_context *ssl_ctx;
	struct ssl_iostream *ssl_iostream;

	enum dsync_run_type run_type;
	struct server_connection *tcp_conn;
	const char *error;

	unsigned int lock_timeout;

	unsigned int lock:1;
	unsigned int sync_visible_namespaces:1;
	unsigned int default_replica_location:1;
	unsigned int backup:1;
	unsigned int reverse_backup:1;
	unsigned int remote_user_prefix:1;
	unsigned int no_mail_sync:1;
	unsigned int local_location_from_arg:1;
};

static bool legacy_dsync = FALSE;

static void remote_error_input(struct dsync_cmd_context *ctx)
{
	const unsigned char *data;
	size_t size;
	const char *line;

	switch (i_stream_read(ctx->err_stream)) {
	case -2:
		data = i_stream_get_data(ctx->err_stream, &size);
		fprintf(stderr, "%.*s", (int)size, data);
		i_stream_skip(ctx->err_stream, size);
		break;
	case -1:
		if (ctx->io_err != NULL)
			io_remove(&ctx->io_err);
		break;
	default:
		while ((line = i_stream_next_line(ctx->err_stream)) != NULL)
			fprintf(stderr, "%s\n", line);
		break;
	}
}

static void
run_cmd(struct dsync_cmd_context *ctx, const char *const *args)
{
	int fd_in[2], fd_out[2], fd_err[2];

	if (pipe(fd_in) < 0 || pipe(fd_out) < 0 || pipe(fd_err) < 0)
		i_fatal("pipe() failed: %m");

	ctx->remote_pid = fork();
	switch (ctx->remote_pid) {
	case -1:
		i_fatal("fork() failed: %m");
	case 0:
		/* child, which will execute the proxy server. stdin/stdout
		   goes to pipes which we'll pass to proxy client. */
		if (dup2(fd_in[0], STDIN_FILENO) < 0 ||
		    dup2(fd_out[1], STDOUT_FILENO) < 0 ||
		    dup2(fd_err[1], STDERR_FILENO) < 0)
			i_fatal("dup2() failed: %m");

		i_close_fd(&fd_in[0]);
		i_close_fd(&fd_in[1]);
		i_close_fd(&fd_out[0]);
		i_close_fd(&fd_out[1]);
		i_close_fd(&fd_err[0]);
		i_close_fd(&fd_err[1]);

		execvp_const(args[0], args);
	default:
		/* parent */
		break;
	}

	i_close_fd(&fd_in[0]);
	i_close_fd(&fd_out[1]);
	i_close_fd(&fd_err[1]);
	ctx->fd_in = fd_out[0];
	ctx->fd_out = fd_in[1];
	ctx->fd_err = fd_err[0];

	if (ctx->remote_user_prefix) {
		const char *prefix =
			t_strdup_printf("%s\n", ctx->ctx.cur_username);
		if (write_full(ctx->fd_out, prefix, strlen(prefix)) < 0)
			i_fatal("write(remote out) failed: %m");
	}

	fd_set_nonblock(ctx->fd_err, TRUE);
	ctx->err_stream = i_stream_create_fd(ctx->fd_err, IO_BLOCK_SIZE, FALSE);
	i_stream_set_return_partial_line(ctx->err_stream, TRUE);
}

static void
mirror_get_remote_cmd_line(const char *const *argv,
			   const char *const **cmd_args_r)
{
	ARRAY_TYPE(const_string) cmd_args;
	unsigned int i;
	const char *p;

	i_assert(argv[0] != NULL);

	t_array_init(&cmd_args, 16);
	for (i = 0; argv[i] != NULL; i++) {
		p = argv[i];
		array_append(&cmd_args, &p, 1);
	}

	if (legacy_dsync) {
		/* we're executing dsync */
		p = "server";
	} else {
		/* we're executing doveadm */
		p = "dsync-server";
	}
	array_append(&cmd_args, &p, 1);
	array_append_zero(&cmd_args);
	*cmd_args_r = array_idx(&cmd_args, 0);
}

static const char *const *
get_ssh_cmd_args(const char *host, const char *login, const char *mail_user)
{
	static struct var_expand_table static_tab[] = {
		{ 'u', NULL, "user" },
		{ '\0', NULL, "login" },
		{ '\0', NULL, "host" },
		{ '\0', NULL, NULL }
	};
	struct var_expand_table *tab;
	ARRAY_TYPE(const_string) cmd_args;
	string_t *str, *str2;
	const char *value, *const *args;

	tab = t_malloc(sizeof(static_tab));
	memcpy(tab, static_tab, sizeof(static_tab));

	tab[0].value = mail_user;
	tab[1].value = login;
	tab[2].value = host;

	t_array_init(&cmd_args, 8);
	str = t_str_new(128);
	str2 = t_str_new(128);
	args = t_strsplit(doveadm_settings->dsync_remote_cmd, " ");
	for (; *args != NULL; args++) {
		if (strchr(*args, '%') == NULL)
			value = *args;
		else {
			/* some automation: if parameter's all %variables
			   expand to empty, but the %variable isn't the only
			   text in the parameter, skip it. */
			str_truncate(str, 0);
			str_truncate(str2, 0);
			var_expand(str, *args, tab);
			var_expand(str2, *args, static_tab);
			if (strcmp(str_c(str), str_c(str2)) == 0 &&
			    str_len(str) > 0)
				continue;
			value = t_strdup(str_c(str));
		}
		array_append(&cmd_args, &value, 1);
	}
	array_append_zero(&cmd_args);
	return array_idx(&cmd_args, 0);
}

static bool mirror_get_remote_cmd(struct dsync_cmd_context *ctx,
				  const char *user,
				  const char *const **cmd_args_r)
{
	const char *p, *host, *const *argv = ctx->ctx.args;

	if (argv[1] != NULL) {
		/* more than one parameter, so it contains a full command
		   (e.g. ssh host dsync) */
		mirror_get_remote_cmd_line(argv, cmd_args_r);
		return TRUE;
	}

	/* if it begins with /[a-z0-9]+:/, it's a mail location
	   (e.g. mdbox:~/mail) */
	for (p = argv[0]; *p != '\0'; p++) {
		if (!i_isalnum(*p)) {
			if (*p == ':')
				return FALSE;
			break;
		}
	}

	if (strchr(argv[0], ' ') != NULL || strchr(argv[0], '/') != NULL) {
		/* a) the whole command is in one string. this is mainly for
		      backwards compatibility.
		   b) script/path */
		mirror_get_remote_cmd_line(t_strsplit(argv[0], " "),
					   cmd_args_r);
		return TRUE;
	}

	/* [user@]host */
	host = strchr(argv[0], '@');
	if (host != NULL)
		user = t_strdup_until(argv[0], host++);
	else
		host = argv[0];

	/* we'll assume virtual users, so in user@host it really means not to
	   give ssh a username, but to give dsync -u user parameter. */
	*cmd_args_r = get_ssh_cmd_args(host, "", user);
	return TRUE;
}

static bool paths_are_equal(struct mail_user *user1, struct mail_user *user2,
			    enum mailbox_list_path_type type)
{
	const char *path1, *path2;

	return mailbox_list_get_root_path(user1->namespaces->list, type, &path1) &&
		mailbox_list_get_root_path(user2->namespaces->list, type, &path2) &&
		strcmp(path1, path2) == 0;
}

static int
cmd_dsync_run_local(struct dsync_cmd_context *ctx, struct mail_user *user,
		    struct dsync_brain *brain, struct dsync_ibc *ibc2)
{
	struct dsync_brain *brain2;
	struct mail_user *user2;
	struct setting_parser_context *set_parser;
	const char *set_line, *location;
	bool brain1_running, brain2_running, changed1, changed2;
	int ret;

	if (ctx->local_location_from_arg)
		location = ctx->ctx.args[0];
	else {
		i_assert(ctx->local_location != NULL);
		location = ctx->local_location;
	}

	i_set_failure_prefix("dsync(%s): ", user->username);

	/* update mail_location and create another user for the
	   second location. */
	set_parser = mail_storage_service_user_get_settings_parser(ctx->ctx.cur_service_user);
	set_line = t_strconcat("mail_location=", location, NULL);
	if (settings_parse_line(set_parser, set_line) < 0)
		i_unreached();
	ret = mail_storage_service_next(ctx->ctx.storage_service,
					ctx->ctx.cur_service_user, &user2);
	if (ret < 0) {
		ctx->ctx.exit_code = ret == -1 ? EX_TEMPFAIL : EX_CONFIG;
		return -1;
	}
	user2->admin = TRUE;

	if (mail_namespaces_get_root_sep(user->namespaces) !=
	    mail_namespaces_get_root_sep(user2->namespaces)) {
		i_error("Mail locations must use the same "
			"virtual mailbox hierarchy separator "
			"(specify separator for the default namespace)");
		ctx->ctx.exit_code = EX_CONFIG;
		mail_user_unref(&user2);
		return -1;
	}
	if (paths_are_equal(user, user2, MAILBOX_LIST_PATH_TYPE_MAILBOX) &&
	    paths_are_equal(user, user2, MAILBOX_LIST_PATH_TYPE_INDEX)) {
		i_error("Both source and destination mail_location "
			"points to same directory: %s",
			mailbox_list_get_root_forced(user->namespaces->list,
						     MAILBOX_LIST_PATH_TYPE_MAILBOX));
		ctx->ctx.exit_code = EX_CONFIG;
		mail_user_unref(&user2);
		return -1;
	}

	brain2 = dsync_brain_slave_init(user2, ibc2);

	brain1_running = brain2_running = TRUE;
	changed1 = changed2 = TRUE;
	while (brain1_running || brain2_running) {
		if (dsync_brain_has_failed(brain) ||
		    dsync_brain_has_failed(brain2))
			break;

		i_assert(changed1 || changed2);
		brain1_running = dsync_brain_run(brain, &changed1);
		brain2_running = dsync_brain_run(brain2, &changed2);
	}
	mail_user_unref(&user2);
	if (dsync_brain_deinit(&brain2) < 0) {
		ctx->ctx.exit_code = EX_TEMPFAIL;
		return -1;
	}
	return 0;
}

static void cmd_dsync_wait_remote(struct dsync_cmd_context *ctx,
				  int *status_r)
{
	/* wait for the remote command to finish to see any final errors.
	   don't wait very long though. */
	alarm(DSYNC_REMOTE_CMD_EXIT_WAIT_SECS);
	if (waitpid(ctx->remote_pid, status_r, 0) == -1) {
		if (errno != EINTR) {
			i_error("waitpid(%ld) failed: %m",
				(long)ctx->remote_pid);
		} else {
			i_error("Remote command process isn't dying, killing it");
			if (kill(ctx->remote_pid, SIGKILL) < 0 && errno != ESRCH) {
				i_error("kill(%ld, SIGKILL) failed: %m",
					(long)ctx->remote_pid);
			}
		}
		*status_r = -1;
	}
}

static void cmd_dsync_log_remote_status(int status, bool remote_errors_logged)
{
	if (status == -1)
		;
	else if (WIFSIGNALED(status))
		i_error("Remote command died with signal %d", WTERMSIG(status));
	else if (!WIFEXITED(status))
		i_error("Remote command failed with status %d", status);
	else if (WEXITSTATUS(status) == EX_TEMPFAIL && remote_errors_logged) {
		/* remote most likely already logged the error.
		   don't bother logging another line about it */
	} else if (WEXITSTATUS(status) != 0) {
		i_error("Remote command returned error %d", WEXITSTATUS(status));
	}
}

static void cmd_dsync_run_remote(struct mail_user *user)
{
	i_set_failure_prefix("dsync-local(%s): ", user->username);
	io_loop_run(current_ioloop);
}

static const char *const *
parse_ssh_location(const char *location, const char *username)
{
	const char *host, *login;

	host = strchr(location, '@');
	if (host != NULL)
		login = t_strdup_until(location, host++);
	else {
		host = location;
		login = "";
	}
	return get_ssh_cmd_args(host, login, username);
}

static struct dsync_ibc *
cmd_dsync_icb_stream_init(struct dsync_cmd_context *ctx,
			  const char *name, const char *temp_prefix)
{
	if (ctx->input == NULL) {
		fd_set_nonblock(ctx->fd_in, TRUE);
		fd_set_nonblock(ctx->fd_out, TRUE);
		ctx->input = i_stream_create_fd(ctx->fd_in, (size_t)-1, FALSE);
		ctx->output = o_stream_create_fd(ctx->fd_out, (size_t)-1, FALSE);
	} else {
		i_stream_ref(ctx->input);
		o_stream_ref(ctx->output);
	}
	if (ctx->rawlog_path != NULL) {
		iostream_rawlog_create_path(ctx->rawlog_path,
					    &ctx->input, &ctx->output);
	}
	return dsync_ibc_init_stream(ctx->input, ctx->output,
				     name, temp_prefix);
}

static int
cmd_dsync_run(struct doveadm_mail_cmd_context *_ctx, struct mail_user *user)
{
	struct dsync_cmd_context *ctx = (struct dsync_cmd_context *)_ctx;
	struct dsync_ibc *ibc, *ibc2 = NULL;
	struct dsync_brain *brain;
	struct mail_namespace *sync_ns = NULL;
	enum dsync_brain_flags brain_flags;
	bool remote_errors_logged = FALSE;
	int status = 0, ret = 0;

	user->admin = TRUE;
	user->dsyncing = TRUE;

	if (ctx->namespace_prefix != NULL) {
		sync_ns = mail_namespace_find(user->namespaces,
					      ctx->namespace_prefix);
	}

	if (ctx->run_type == DSYNC_RUN_TYPE_LOCAL)
		dsync_ibc_init_pipe(&ibc, &ibc2);
	else {
		string_t *temp_prefix = t_str_new(64);
		mail_user_set_get_temp_prefix(temp_prefix, user->set);
		ibc = cmd_dsync_icb_stream_init(ctx, ctx->remote_name,
						str_c(temp_prefix));
		if (ctx->fd_err != -1) {
			ctx->io_err = io_add(ctx->fd_err, IO_READ,
					     remote_error_input, ctx);
		}
	}

	brain_flags = DSYNC_BRAIN_FLAG_SEND_MAIL_REQUESTS;
	if (ctx->sync_visible_namespaces)
		brain_flags |= DSYNC_BRAIN_FLAG_SYNC_VISIBLE_NAMESPACES;

	if (ctx->reverse_backup)
		brain_flags |= DSYNC_BRAIN_FLAG_BACKUP_RECV;
	else if (ctx->backup)
		brain_flags |= DSYNC_BRAIN_FLAG_BACKUP_SEND;

	if (ctx->no_mail_sync)
		brain_flags |= DSYNC_BRAIN_FLAG_NO_MAIL_SYNC;
	if (doveadm_debug)
		brain_flags |= DSYNC_BRAIN_FLAG_DEBUG;
	brain = dsync_brain_master_init(user, ibc, sync_ns, ctx->mailbox,
					ctx->mailbox_guid,
					ctx->sync_type, brain_flags,
					ctx->lock_timeout,
					ctx->state_input == NULL ? "" :
					ctx->state_input);

	if (ctx->run_type == DSYNC_RUN_TYPE_LOCAL) {
		if (cmd_dsync_run_local(ctx, user, brain, ibc2) < 0)
			ret = -1;
	} else {
		cmd_dsync_run_remote(user);
	}

	if (ctx->state_input != NULL) {
		string_t *str = t_str_new(128);
		dsync_brain_get_state(brain, str);
		doveadm_print(str_c(str));
	}

	if (dsync_brain_deinit(&brain) < 0)
		ctx->ctx.exit_code = EX_TEMPFAIL;
	dsync_ibc_deinit(&ibc);
	if (ibc2 != NULL)
		dsync_ibc_deinit(&ibc2);
	if (ctx->ssl_iostream != NULL)
		ssl_iostream_destroy(&ctx->ssl_iostream);
	if (ctx->ssl_ctx != NULL)
		ssl_iostream_context_deinit(&ctx->ssl_ctx);
	if (ctx->fd_in != -1) {
		if (ctx->fd_out != ctx->fd_in)
			i_close_fd(&ctx->fd_out);
		i_close_fd(&ctx->fd_in);
	}
	if (ctx->run_type == DSYNC_RUN_TYPE_CMD)
		cmd_dsync_wait_remote(ctx, &status);

	/* print any final errors after the process has died. not closing
	   stdin/stdout before wait() may cause the process to hang, but stderr
	   shouldn't (at least with ssh) and we need stderr to be open to be
	   able to print the final errors */
	if (ctx->err_stream != NULL) {
		remote_error_input(ctx);
		remote_errors_logged = ctx->err_stream->v_offset > 0;
		i_stream_destroy(&ctx->err_stream);
	}
	if (ctx->run_type == DSYNC_RUN_TYPE_CMD)
		cmd_dsync_log_remote_status(status, remote_errors_logged);
	if (ctx->io_err != NULL)
		io_remove(&ctx->io_err);
	if (ctx->fd_err != -1)
		i_close_fd(&ctx->fd_err);
	ctx->input = NULL;
	ctx->output = NULL;
	return ret;
}

static void dsync_connected_callback(enum server_cmd_reply reply, void *context)
{
	struct dsync_cmd_context *ctx = context;

	switch (reply) {
	case SERVER_CMD_REPLY_UNKNOWN_USER:
		ctx->error = "Unknown user in remote";
		ctx->ctx.exit_code = EX_NOUSER;
		break;
	case SERVER_CMD_REPLY_FAIL:
		ctx->error = "Failed to start dsync-server command";
		break;
	case SERVER_CMD_REPLY_OK:
		server_connection_extract(ctx->tcp_conn, &ctx->input,
					  &ctx->output, &ctx->ssl_iostream);
		break;
	case SERVER_CMD_REPLY_INTERNAL_FAILURE:
		ctx->error = "Disconnected from remote";
		break;
	default:
		i_unreached();
	}
	io_loop_stop(current_ioloop);
}

static int dsync_init_ssl_ctx(struct dsync_cmd_context *ctx,
			      const struct mail_storage_settings *mail_set,
			      const char **error_r)
{
	struct ssl_iostream_settings ssl_set;

	if (ctx->ssl_ctx != NULL)
		return 0;

	memset(&ssl_set, 0, sizeof(ssl_set));
	ssl_set.ca_dir = mail_set->ssl_client_ca_dir;
	ssl_set.verify_remote_cert = TRUE;
	ssl_set.crypto_device = mail_set->ssl_crypto_device;

	return ssl_iostream_context_init_client(&ssl_set, &ctx->ssl_ctx, error_r);
}

static int
dsync_connect_tcp(struct dsync_cmd_context *ctx,
		  const struct mail_storage_settings *mail_set,
		  const char *target, bool ssl, const char **error_r)
{
	struct doveadm_server *server;
	struct server_connection *conn;
	struct ioloop *ioloop;
	string_t *cmd;
	const char *error;

	server = p_new(ctx->ctx.pool, struct doveadm_server, 1);
	server->name = p_strdup(ctx->ctx.pool, target);
	if (ssl) {
		if (dsync_init_ssl_ctx(ctx, mail_set, &error) < 0) {
			*error_r = t_strdup_printf(
				"Couldn't initialize SSL context: %s", error);
			return -1;
		}
		server->ssl_ctx = ctx->ssl_ctx;
	}
	p_array_init(&server->connections, ctx->ctx.pool, 1);
	p_array_init(&server->queue, ctx->ctx.pool, 1);

	ioloop = io_loop_create();

	if (server_connection_create(server, &conn) < 0) {
		*error_r = "Couldn't create server connection";
		return -1;
	}

	/* <flags> <username> <command> [<args>] */
	cmd = t_str_new(256);
	if (doveadm_debug)
		str_append_c(cmd, 'D');
	str_append_c(cmd, '\t');
	str_append_tabescaped(cmd, ctx->ctx.cur_username);
	str_append(cmd, "\tdsync-server\t-u");
	str_append_tabescaped(cmd, ctx->ctx.cur_username);
	str_append_c(cmd, '\n');

	ctx->tcp_conn = conn;
	server_connection_cmd(conn, str_c(cmd),
			      dsync_connected_callback, ctx);
	io_loop_run(ioloop);
	ctx->tcp_conn = NULL;

	if (array_count(&server->connections) > 0)
		server_connection_destroy(&conn);
	io_loop_destroy(&ioloop);

	if (ctx->error != NULL) {
		*error_r = ctx->error;
		ctx->error = NULL;
		return -1;
	}
	ctx->run_type = DSYNC_RUN_TYPE_STREAM;
	return 0;
}

static int
parse_location(struct dsync_cmd_context *ctx,
	       const struct mail_storage_settings *mail_set,
	       const char *location,
	       const char *const **remote_cmd_args_r, const char **error_r)
{
	if (strncmp(location, "tcp:", 4) == 0) {
		/* TCP connection to remote dsync */
		ctx->remote_name = location+4;
		return dsync_connect_tcp(ctx, mail_set, ctx->remote_name,
					 FALSE, error_r);
	}
	if (strncmp(location, "tcps:", 5) == 0) {
		/* TCP+SSL connection to remote dsync */
		ctx->remote_name = location+5;
		return dsync_connect_tcp(ctx, mail_set, ctx->remote_name,
					 TRUE, error_r);
	}

	if (strncmp(location, "remote:", 7) == 0) {
		/* this is a remote (ssh) command */
		ctx->remote_name = location+7;
	} else if (strncmp(location, "remoteprefix:", 13) == 0) {
		/* this is a remote (ssh) command with a "user\n"
		   prefix sent before dsync actually starts */
		ctx->remote_name = location+13;
		ctx->remote_user_prefix = TRUE;
	} else {
		/* local with e.g. maildir:path */
		ctx->remote_name = NULL;
		return 0;
	}
	*remote_cmd_args_r =
		parse_ssh_location(ctx->remote_name, ctx->ctx.cur_username);
	return 0;
}

static int cmd_dsync_prerun(struct doveadm_mail_cmd_context *_ctx,
			    struct mail_storage_service_user *service_user,
			    const char **error_r)
{
	struct dsync_cmd_context *ctx = (struct dsync_cmd_context *)_ctx;
	const char *const *remote_cmd_args = NULL;
	const struct mail_user_settings *user_set;
	const struct mail_storage_settings *mail_set;
	const char *username = "";

	user_set = mail_storage_service_user_get_set(service_user)[0];
	mail_set = mail_storage_service_user_get_mail_set(service_user);

	ctx->fd_in = -1;
	ctx->fd_out = -1;
	ctx->fd_err = -1;
	ctx->run_type = DSYNC_RUN_TYPE_LOCAL;
	ctx->remote_name = "remote";

	if (ctx->default_replica_location) {
		ctx->local_location =
			mail_user_set_plugin_getenv(user_set, "mail_replica");
		if (ctx->local_location == NULL ||
		    *ctx->local_location == '\0') {
			*error_r = "User has no mail_replica in userdb";
			_ctx->exit_code = DOVEADM_EX_NOTFOUND;
			return -1;
		}
	} else {
		/* if we're executing remotely, give -u parameter if we also
		   did a userdb lookup. */
		if ((_ctx->service_flags & MAIL_STORAGE_SERVICE_FLAG_USERDB_LOOKUP) != 0)
			username = _ctx->cur_username;

		if (!mirror_get_remote_cmd(ctx, username, &remote_cmd_args)) {
			/* it's a mail_location */
			if (_ctx->args[1] != NULL)
				doveadm_mail_help_name(_ctx->cmd->name);
			ctx->local_location = _ctx->args[0];
			ctx->local_location_from_arg = TRUE;
		}
	}

	if (remote_cmd_args == NULL && ctx->local_location != NULL) {
		if (parse_location(ctx, mail_set, ctx->local_location,
				   &remote_cmd_args, error_r) < 0)
			return -1;
	}

	if (remote_cmd_args != NULL) {
		/* do this before mail_storage_service_next() in case it
		   drops process privileges */
		run_cmd(ctx, remote_cmd_args);
		ctx->run_type = DSYNC_RUN_TYPE_CMD;
	}

	if (ctx->sync_visible_namespaces &&
	    ctx->run_type == DSYNC_RUN_TYPE_LOCAL)
		i_fatal("-N parameter requires syncing with remote host");
	return 0;
}

static void cmd_dsync_init(struct doveadm_mail_cmd_context *_ctx,
			   const char *const args[])
{
	struct dsync_cmd_context *ctx = (struct dsync_cmd_context *)_ctx;

	if (ctx->default_replica_location) {
		if (args[0] != NULL)
			i_error("Don't give mail location with -d parameter");
	} else {
		if (args[0] == NULL)
			doveadm_mail_help_name(_ctx->cmd->name);
	}

	lib_signals_ignore(SIGHUP, TRUE);
}

static void cmd_dsync_preinit(struct doveadm_mail_cmd_context *ctx)
{
	if ((ctx->service_flags & MAIL_STORAGE_SERVICE_FLAG_USERDB_LOOKUP) == 0)
		ctx->service_flags |= MAIL_STORAGE_SERVICE_FLAG_NO_CHDIR;
}

static bool
cmd_mailbox_dsync_parse_arg(struct doveadm_mail_cmd_context *_ctx, int c)
{
	struct dsync_cmd_context *ctx = (struct dsync_cmd_context *)_ctx;

	switch (c) {
	case 'd':
		ctx->default_replica_location = TRUE;
		break;
	case 'E':
		/* dsync wrapper detection flag */
		legacy_dsync = TRUE;
		break;
	case 'f':
		ctx->sync_type = DSYNC_BRAIN_SYNC_TYPE_FULL;
		break;
	case 'g':
		if (optarg[0] == '\0')
			ctx->no_mail_sync = TRUE;
		else if (guid_128_from_string(optarg, ctx->mailbox_guid) < 0 ||
			 guid_128_is_empty(ctx->mailbox_guid))
			i_fatal("Invalid -g parameter: %s", optarg);
		break;
	case 'l':
		ctx->lock = TRUE;
		if (str_to_uint(optarg, &ctx->lock_timeout) < 0)
			i_fatal("Invalid -l parameter: %s", optarg);
		break;
	case 'm':
		if (optarg[0] == '\0')
			ctx->no_mail_sync = TRUE;
		else
			ctx->mailbox = optarg;
		break;
	case 'n':
		ctx->namespace_prefix = optarg;
		break;
	case 'N':
		ctx->sync_visible_namespaces = TRUE;
		break;
	case 'r':
		ctx->rawlog_path = optarg;
		break;
	case 'R':
		if (!ctx->backup)
			return FALSE;
		ctx->reverse_backup = TRUE;
		break;
	case 's':
		if (ctx->sync_type != DSYNC_BRAIN_SYNC_TYPE_FULL &&
		    *optarg != '\0')
			ctx->sync_type = DSYNC_BRAIN_SYNC_TYPE_STATE;
		ctx->state_input = optarg;
		break;
	default:
		return FALSE;
	}
	return TRUE;
}

static struct doveadm_mail_cmd_context *cmd_dsync_alloc(void)
{
	struct dsync_cmd_context *ctx;

	ctx = doveadm_mail_cmd_alloc(struct dsync_cmd_context);
	ctx->ctx.getopt_args = DSYNC_COMMON_GETOPT_ARGS;
	ctx->ctx.v.parse_arg = cmd_mailbox_dsync_parse_arg;
	ctx->ctx.v.preinit = cmd_dsync_preinit;
	ctx->ctx.v.init = cmd_dsync_init;
	ctx->ctx.v.prerun = cmd_dsync_prerun;
	ctx->ctx.v.run = cmd_dsync_run;
	ctx->sync_type = DSYNC_BRAIN_SYNC_TYPE_CHANGED;
	doveadm_print_init(DOVEADM_PRINT_TYPE_FLOW);
	doveadm_print_header("state", "state",
			     DOVEADM_PRINT_HEADER_FLAG_HIDE_TITLE);
	return &ctx->ctx;
}

static struct doveadm_mail_cmd_context *cmd_dsync_backup_alloc(void)
{
	struct doveadm_mail_cmd_context *_ctx;
	struct dsync_cmd_context *ctx;

	_ctx = cmd_dsync_alloc();
	_ctx->getopt_args = DSYNC_COMMON_GETOPT_ARGS"R";
	ctx = (struct dsync_cmd_context *)_ctx;
	ctx->backup = TRUE;
	return _ctx;
}

static int
cmd_dsync_server_run(struct doveadm_mail_cmd_context *_ctx,
		     struct mail_user *user)
{
	struct dsync_cmd_context *ctx = (struct dsync_cmd_context *)_ctx;
	struct dsync_ibc *ibc;
	struct dsync_brain *brain;
	string_t *temp_prefix;

	if (_ctx->conn != NULL) {
		/* doveadm-server connection. start with a success reply.
		   after that follows the regular dsync protocol. */
		ctx->fd_in = ctx->fd_out = -1;
		ctx->input = _ctx->conn->input;
		ctx->output = _ctx->conn->output;
		o_stream_nsend(ctx->output, "\n+\n", 3);
	}

	user->admin = TRUE;
	user->dsyncing = TRUE;

	i_set_failure_prefix("dsync-remote(%s): ", user->username);

	temp_prefix = t_str_new(64);
	mail_user_set_get_temp_prefix(temp_prefix, user->set);

	ibc = cmd_dsync_icb_stream_init(ctx, "local", str_c(temp_prefix));
	brain = dsync_brain_slave_init(user, ibc);

	io_loop_run(current_ioloop);

	if (dsync_brain_deinit(&brain) < 0)
		_ctx->exit_code = EX_TEMPFAIL;
	dsync_ibc_deinit(&ibc);

	if (_ctx->conn != NULL) {
		/* make sure nothing more is written by the generic doveadm
		   connection code */
		o_stream_close(_ctx->conn->output);
	}

	return _ctx->exit_code == 0 ? 0 : -1;
}

static bool
cmd_mailbox_dsync_server_parse_arg(struct doveadm_mail_cmd_context *_ctx, int c)
{
	struct dsync_cmd_context *ctx = (struct dsync_cmd_context *)_ctx;

	switch (c) {
	case 'E':
		/* dsync wrapper detection flag */
		legacy_dsync = TRUE;
		break;
	case 'r':
		ctx->rawlog_path = optarg;
		break;
	default:
		return FALSE;
	}
	return TRUE;
}

static struct doveadm_mail_cmd_context *cmd_dsync_server_alloc(void)
{
	struct dsync_cmd_context *ctx;

	ctx = doveadm_mail_cmd_alloc(struct dsync_cmd_context);
	ctx->ctx.getopt_args = "Er:";
	ctx->ctx.v.parse_arg = cmd_mailbox_dsync_server_parse_arg;
	ctx->ctx.v.run = cmd_dsync_server_run;
	ctx->sync_type = DSYNC_BRAIN_SYNC_TYPE_CHANGED;
	ctx->fd_in = STDIN_FILENO;
	ctx->fd_out = STDOUT_FILENO;
	return &ctx->ctx;
}

struct doveadm_mail_cmd cmd_dsync_mirror = {
	cmd_dsync_alloc, "sync",
	"[-dfR] [-l <secs>] [-m <mailbox>] [-n <namespace>] [-s <state>] <dest>"
};
struct doveadm_mail_cmd cmd_dsync_backup = {
	cmd_dsync_backup_alloc, "backup",
	"[-dfR] [-l <secs>] [-m <mailbox>] [-n <namespace>] [-s <state>] <dest>"
};
struct doveadm_mail_cmd cmd_dsync_server = {
	cmd_dsync_server_alloc, "dsync-server", &doveadm_mail_cmd_hide
};

void doveadm_dsync_main(int *_argc, char **_argv[])
{
	int argc = *_argc;
	const char *getopt_str;
	char **argv = *_argv;
	char **new_argv, *mailbox = NULL, *alt_char = NULL, *username = NULL;
	char *p, *dup, new_flags[6];
	int max_argc, src, dest, i, j;
	bool flag_f = FALSE, flag_R = FALSE, flag_m, flag_u, flag_C, has_arg;
	bool dsync_server = FALSE;

	p = strrchr(argv[0], '/');
	if (p == NULL) p = argv[0];
	if (strstr(p, "dsync") == NULL)
		return;

	/* @UNSAFE: this is called when the "doveadm" binary is called as
	   "dsync" (for backwards compatibility) */
	max_argc = argc + 7;
	new_argv = t_new(char *, max_argc);
	new_argv[0] = argv[0];
	dest = 1;
	getopt_str = master_service_getopt_string();

	/* add global doveadm flags */
	for (src = 1; src < argc; src++) {
		if (argv[src][0] != '-')
			break;

		flag_m = FALSE; flag_C = FALSE; has_arg = FALSE; flag_u = FALSE;
		dup = t_strdup_noconst(argv[src]);
		for (i = j = 1; argv[src][i] != '\0'; i++) {
			switch (argv[src][i]) {
			case 'C':
				flag_C = TRUE;
				break;
			case 'f':
				flag_f = TRUE;
				break;
			case 'R':
				flag_R = TRUE;
				break;
			case 'm':
				flag_m = TRUE;
				break;
			case 'u':
				flag_u = TRUE;
				break;
			default:
				p = strchr(getopt_str, argv[src][i]);
				if (p != NULL && p[1] == ':')
					has_arg = TRUE;
				dup[j++] = argv[src][i];
				break;
			}
		}
		if (j > 1) {
			dup[j++] = '\0';
			new_argv[dest++] = dup;
			if (has_arg && src+1 < argc)
				new_argv[dest++] = argv[++src];
		}
		if (flag_m) {
			if (src+1 == argc)
				i_fatal("-m missing parameter");
			mailbox = argv[++src];
		}
		if (flag_u) {
			if (src+1 == argc)
				i_fatal("-u missing parameter");
			username = argv[++src];
		}
		if (flag_C) {
			if (src+1 == argc)
				i_fatal("-C missing parameter");
			alt_char = argv[++src];
		}
	}
	if (alt_char != NULL) {
		new_argv[dest++] = "-o";
		new_argv[dest++] =
			p_strconcat(pool_datastack_create(),
				    "dsync_alt_char=", alt_char, NULL);
	}

	/* mirror|backup|server */
	if (src == argc)
		i_fatal("Missing mirror or backup parameter");
	if (strcmp(argv[src], "sync") == 0 ||
	    strcmp(argv[src], "dsync-server") == 0) {
		/* we're re-executing dsync due to doveconf.
		   "backup" re-exec detection is later. */
		return;
	}
	if (strcmp(argv[src], "mirror") == 0)
		new_argv[dest] = "sync";
	else if (strcmp(argv[src], "backup") == 0)
		new_argv[dest] = "backup";
	else if (strcmp(argv[src], "server") == 0) {
		new_argv[dest] = "dsync-server";
		dsync_server = TRUE;
	} else
		i_fatal("Invalid parameter: %s", argv[src]);
	src++; dest++;

	if (src < argc && strncmp(argv[src], "-E", 2) == 0) {
		/* we're re-executing dsync due to doveconf */
		return;
	}

	/* dsync flags */
	new_flags[0] = '-';
	new_flags[1] = 'E'; i = 2;
	if (!dsync_server) {
		if (flag_f)
			new_flags[i++] = 'f';
		if (flag_R)
			new_flags[i++] = 'R';
		if (mailbox != NULL)
			new_flags[i++] = 'm';
	}
	i_assert((unsigned int)i < sizeof(new_flags));
	new_flags[i] = '\0';

	if (i > 1) {
		new_argv[dest++] = strdup(new_flags);
		if (mailbox != NULL)
			new_argv[dest++] = mailbox;
	}
	if (username != NULL) {
		new_argv[dest++] = "-u";
		new_argv[dest++] = username;
	}

	/* rest of the parameters */
	for (; src < argc; src++)
		new_argv[dest++] = argv[src];
	i_assert(dest < max_argc);
	new_argv[dest] = NULL;

	legacy_dsync = TRUE;
	*_argc = dest;
	*_argv = new_argv;
	optind = 1;
}
