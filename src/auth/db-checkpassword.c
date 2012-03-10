/* Copyright (c) 2004-2012 Dovecot authors, see the included COPYING file */

#include "auth-common.h"

#if defined(PASSDB_CHECKPASSWORD) || defined(USERDB_CHECKPASSWORD)

#include "lib-signals.h"
#include "buffer.h"
#include "str.h"
#include "ioloop.h"
#include "hash.h"
#include "execv-const.h"
#include "env-util.h"
#include "safe-memset.h"
#include "strescape.h"
#include "child-wait.h"
#include "var-expand.h"
#include "db-checkpassword.h"

#include <stdlib.h>
#include <unistd.h>

#define CHECKPASSWORD_MAX_REQUEST_LEN 512

struct chkpw_auth_request {
	struct db_checkpassword *db;
	struct auth_request *request;
	char *auth_password;

	db_checkpassword_callback_t *callback;
	void *context;

	pid_t pid;
	int fd_out, fd_in;
	struct io *io_out, *io_in;

	string_t *input_buf;
	unsigned int output_pos, output_len;

	int exit_status;
	unsigned int exited:1;
};

struct db_checkpassword {
	char *checkpassword_path, *checkpassword_reply_path;

	struct hash_table *clients;
	struct child_wait *child_wait;
};

static void env_put_extra_fields(const char *extra_fields)
{
	const char *const *tmp;
	const char *key, *p;

	for (tmp = t_strsplit(extra_fields, "\t"); *tmp != NULL; tmp++) {
		key = t_str_ucase(t_strcut(*tmp, '='));
		p = strchr(*tmp, '=');
		if (p == NULL)
			env_put(t_strconcat(key, "=1", NULL));
		else
			env_put(t_strconcat(key, p, NULL));
	}
}

static void checkpassword_request_close(struct chkpw_auth_request *request)
{
	if (request->io_in != NULL)
		io_remove(&request->io_in);
	if (request->io_out != NULL)
		io_remove(&request->io_out);

	if (request->fd_in != -1) {
		if (close(request->fd_in) < 0)
			i_error("checkpassword: close() failed: %m");
		request->fd_in = -1;
	}
	if (request->fd_out != -1) {
		if (close(request->fd_out) < 0)
			i_error("checkpassword: close() failed: %m");
	}
}

static void checkpassword_request_free(struct chkpw_auth_request **_request)
{
	struct chkpw_auth_request *request = *_request;

	*_request = NULL;

	if (!request->exited) {
		hash_table_remove(request->db->clients,
				  POINTER_CAST(request->pid));
		child_wait_remove_pid(request->db->child_wait, request->pid);
	}
	checkpassword_request_close(request);

	if (request->auth_password != NULL) {
		safe_memset(request->auth_password, 0,
			    strlen(request->auth_password));
		i_free(request->auth_password);
	}
	auth_request_unref(&request->request);
	str_free(&request->input_buf);
	i_free(request);
}

static void checkpassword_finish(struct chkpw_auth_request **_request,
				 enum db_checkpassword_status status)
{
	struct chkpw_auth_request *request = *_request;
	const char *const *extra_fields;

	*_request = NULL;

	extra_fields = t_strsplit_tabescaped(str_c(request->input_buf));
	request->callback(request->request, status, extra_fields,
			  request->context);
	checkpassword_request_free(&request);
}

static void checkpassword_internal_failure(struct chkpw_auth_request **request)
{
	checkpassword_finish(request, DB_CHECKPASSWORD_STATUS_INTERNAL_FAILURE);
}

static void
checkpassword_request_finish_auth(struct chkpw_auth_request *request)
{
	switch (request->exit_status) {
	/* vpopmail exit codes: */
	case 3:		/* password fail / vpopmail user not found */
	case 12: 	/* null user name given */
	case 13:	/* null password given */
	case 15:	/* user has no password */
	case 20:	/* invalid user/domain characters */
	case 21:	/* system user not found */
	case 22:	/* system user shadow entry not found */
	case 23:	/* system password fail */

	/* standard checkpassword exit codes: */
	case 1:
		/* (1 is additionally defined in vpopmail for
		   "pop/smtp/webmal/ imap/access denied") */
		auth_request_log_info(request->request, "checkpassword",
				      "Login failed (status=%d)",
				      request->exit_status);
		checkpassword_finish(&request, DB_CHECKPASSWORD_STATUS_FAILURE);
		break;
	case 0:
		if (request->input_buf->used == 0) {
			auth_request_log_error(request->request, "checkpassword",
					       "Received no input");
			checkpassword_internal_failure(&request);
			break;
		}
		checkpassword_finish(&request, DB_CHECKPASSWORD_STATUS_OK);
		break;
	case 2:
		/* checkpassword is called with wrong parameters? unlikely */
		auth_request_log_error(request->request, "checkpassword",
			"Child %s exited with status 2 (tried to use "
			"userdb-only checkpassword program for passdb?)",
			dec2str(request->pid));
		checkpassword_internal_failure(&request);
		break;
	case 111:
		/* temporary problem, treat as internal error */
	default:
		/* whatever error.. */
		auth_request_log_error(request->request, "checkpassword",
			"Child %s exited with status %d",
			dec2str(request->pid), request->exit_status);
		checkpassword_internal_failure(&request);
		break;
	}
}

static void
checkpassword_request_finish_lookup(struct chkpw_auth_request *request)
{
	switch (request->exit_status) {
	case 3:
		/* User does not exist. */
		auth_request_log_info(request->request, "userdb-checkpassword",
				      "User unknown");
		checkpassword_finish(&request, DB_CHECKPASSWORD_STATUS_FAILURE);
		break;
	case 2:
		/* This is intentionally not 0. checkpassword-reply exits with
		   2 on success when AUTHORIZED is set. */
		if (request->input_buf->used == 0) {
			auth_request_log_error(request->request, "checkpassword",
					       "Received no input");
			checkpassword_internal_failure(&request);
			break;
		}
		checkpassword_finish(&request, DB_CHECKPASSWORD_STATUS_OK);
		break;
	default:
		/* whatever error... */
		auth_request_log_error(request->request, "userdb-checkpassword",
			"Child %s exited with status %d",
			dec2str(request->pid), request->exit_status);
		checkpassword_internal_failure(&request);
		break;
	}
}

static void
checkpassword_request_half_finish(struct chkpw_auth_request *request)
{
	/* the process must have exited, and the input fd must have closed */
	if (!request->exited || request->fd_in != -1)
		return;

	if (request->auth_password != NULL)
		checkpassword_request_finish_auth(request);
	else
		checkpassword_request_finish_lookup(request);
}

static void env_put_auth_vars(struct auth_request *request)
{
	const struct var_expand_table *tab;
	unsigned int i;

	tab = auth_request_get_var_expand_table(request, NULL);
	for (i = 0; tab[i].key != '\0' || tab[i].long_key != NULL; i++) {
		if (tab[i].long_key != NULL && tab[i].value != NULL) {
			env_put(t_strdup_printf("AUTH_%s=%s",
						t_str_ucase(tab[i].long_key),
						tab[i].value));
		}
	}
}

static void checkpassword_setup_env(struct auth_request *request)
{
	/* Besides passing the standard username and password in a
	   pipe, also pass some other possibly interesting information
	   via environment. Use UCSPI names for local/remote IPs. */
	env_put("PROTO=TCP"); /* UCSPI */
	env_put(t_strconcat("SERVICE=", request->service, NULL));
	if (request->local_ip.family != 0) {
		env_put(t_strconcat("TCPLOCALIP=",
				    net_ip2addr(&request->local_ip), NULL));
		/* FIXME: for backwards compatibility only,
		   remove some day */
		env_put(t_strconcat("LOCAL_IP=",
				    net_ip2addr(&request->local_ip), NULL));
	}
	if (request->remote_ip.family != 0) {
		env_put(t_strconcat("TCPREMOTEIP=",
				    net_ip2addr(&request->remote_ip), NULL));
		/* FIXME: for backwards compatibility only,
		   remove some day */
		env_put(t_strconcat("REMOTE_IP=",
				    net_ip2addr(&request->remote_ip), NULL));
	}
	if (request->local_port != 0) {
		env_put(t_strdup_printf("TCPLOCALPORT=%u",
					request->local_port));
	}
	if (request->remote_port != 0) {
		env_put(t_strdup_printf("TCPREMOTEPORT=%u",
					request->remote_port));
	}
	if (request->master_user != NULL) {
		env_put(t_strconcat("MASTER_USER=",
				    request->master_user, NULL));
	}
	if (request->extra_fields != NULL) {
		const char *fields =
			auth_stream_reply_export(request->extra_fields);

		/* extra fields could come from master db */
		env_put_extra_fields(fields);
	}
	env_put_auth_vars(request);
}

static const char *
checkpassword_get_cmd(struct auth_request *request, const char *args,
		      const char *checkpassword_reply_path)
{
	string_t *str;

	str = t_str_new(256);
	var_expand(str, args,
		   auth_request_get_var_expand_table(request, NULL));
	return t_strconcat(str_c(str), " ", checkpassword_reply_path, NULL);
}

static void checkpassword_child_input(struct chkpw_auth_request *request)
{
	unsigned char buf[1024];
	ssize_t ret;

	ret = read(request->fd_in, buf, sizeof(buf));
	if (ret > 0) {
		str_append_n(request->input_buf, buf, ret);
		return;
	}

	if (ret < 0) {
		auth_request_log_error(request->request,
			"checkpassword", "read() failed: %m");
		checkpassword_internal_failure(&request);
	} else if (strchr(str_c(request->input_buf), '\n') != NULL) {
		auth_request_log_error(request->request, "checkpassword",
				       "LF characters in checkpassword reply");
		checkpassword_internal_failure(&request);
	} else {
		auth_request_log_debug(request->request, "checkpassword",
			"Received input: %s", str_c(request->input_buf));
		checkpassword_request_close(request);
		checkpassword_request_half_finish(request);
	}
}

static void checkpassword_child_output(struct chkpw_auth_request *request)
{
	/* Send: username \0 password \0 timestamp \0.
	   Must be 512 bytes or less. The "timestamp" parameter is actually
	   useful only for APOP authentication. We don't support it, so
	   keep it empty */
	struct auth_request *auth_request = request->request;
	buffer_t *buf;
	const unsigned char *data;
	size_t size;
	ssize_t ret;

	buf = buffer_create_dynamic(pool_datastack_create(),
				    CHECKPASSWORD_MAX_REQUEST_LEN);
	buffer_append(buf, auth_request->user, strlen(auth_request->user)+1);
	if (request->auth_password != NULL) {
		buffer_append(buf, request->auth_password,
			      strlen(request->auth_password)+1);
	} else {
		buffer_append_c(buf, '\0');
	}
	buffer_append_c(buf, '\0');
	data = buffer_get_data(buf, &size);

	i_assert(size == request->output_len);
	/* already checked this */
	i_assert(size <= CHECKPASSWORD_MAX_REQUEST_LEN);

	ret = write(request->fd_out, data + request->output_pos,
		    size - request->output_pos);
	if (ret <= 0) {
		if (ret < 0) {
			auth_request_log_error(request->request,
				"checkpassword", "write() failed: %m");
		} else {
			auth_request_log_error(request->request,
				"checkpassword", "write() returned 0");
		}
		checkpassword_internal_failure(&request);
		return;
	}

	request->output_pos += ret;
	if (request->output_pos < size)
		return;

	/* finished sending the data */
	io_remove(&request->io_out);

	if (close(request->fd_out) < 0)
		i_error("checkpassword: close() failed: %m");
	request->fd_out = -1;
}

static void ATTR_NORETURN
checkpassword_exec(struct db_checkpassword *db, struct auth_request *request,
		   int fd_in, int fd_out, bool authenticate)
{
	const char *cmd, *const *args;

	/* fd 3 is used to send the username+password for the script
	   fd 4 is used to communicate with checkpassword-reply */
	if (dup2(fd_out, 3) < 0 || dup2(fd_in, 4) < 0) {
		auth_request_log_error(request, "checkpassword",
				       "dup2() failed: %m");
		exit(111);
	}

	if (!authenticate) {
		/* We want to retrieve passdb/userdb data and don't do
		   authorization, so we need to signalize the
		   checkpassword program that the password shall be
		   ignored by setting AUTHORIZED.  This needs a
		   special checkpassword program which knows how to
		   handle this. */
		env_put("AUTHORIZED=1");
	}
	checkpassword_setup_env(request);
	cmd = checkpassword_get_cmd(request, db->checkpassword_path,
				    db->checkpassword_reply_path);
	auth_request_log_debug(request, "checkpassword", "execute: %s", cmd);

	/* very simple argument splitting. */
	args = t_strsplit(cmd, " ");
	execv_const(args[0], args);
}

static void sigchld_handler(const struct child_wait_status *status,
			    struct db_checkpassword *db)
{
	struct chkpw_auth_request *request = 
		hash_table_lookup(db->clients, POINTER_CAST(status->pid));

	i_assert(request != NULL);

	hash_table_remove(db->clients, POINTER_CAST(status->pid));
	request->exited = TRUE;

	if (WIFSIGNALED(status->status)) {
		auth_request_log_error(request->request, "checkpassword",
			"Child %s died with signal %d",
			dec2str(status->pid), WTERMSIG(status->status));
		checkpassword_internal_failure(&request);
	} else if (WIFEXITED(status->status)) {
		request->exit_status = WEXITSTATUS(status->status);

		auth_request_log_debug(request->request,
				       "checkpassword", "exit_status=%d",
				       request->exit_status);
		checkpassword_request_half_finish(request);
	} else {
		/* shouldn't happen */
		auth_request_log_debug(request->request, "checkpassword",
				       "Child %s exited with status=%d",
				       dec2str(status->pid), status->status);
		checkpassword_internal_failure(&request);
	}
}

void db_checkpassword_call(struct db_checkpassword *db,
			   struct auth_request *request,
			   const char *auth_password,
			   db_checkpassword_callback_t *callback,
			   void *context)
{
	struct chkpw_auth_request *chkpw_auth_request;
	unsigned int output_len;
	int fd_in[2], fd_out[2];
	pid_t pid;

	/* <username> \0 <password> \0 timestamp \0 */
	output_len = strlen(request->user) + 3;
	if (auth_password != NULL)
		output_len += strlen(auth_password);
	if (output_len > CHECKPASSWORD_MAX_REQUEST_LEN) {
		auth_request_log_info(request, "checkpassword",
			"Username+password combination too long (%u bytes)",
			output_len);
		callback(request, DB_CHECKPASSWORD_STATUS_FAILURE,
			 NULL, context);
		return;
	}

	fd_in[0] = -1;
	if (pipe(fd_in) < 0 || pipe(fd_out) < 0) {
		auth_request_log_error(request, "checkpassword",
				       "pipe() failed: %m");
		if (fd_in[0] != -1) {
			(void)close(fd_in[0]);
			(void)close(fd_in[1]);
		}
		callback(request, DB_CHECKPASSWORD_STATUS_INTERNAL_FAILURE,
			 NULL, context);
		return;
	}

	pid = fork();
	if (pid == -1) {
		auth_request_log_error(request, "checkpassword",
				       "fork() failed: %m");
		(void)close(fd_in[0]);
		(void)close(fd_in[1]);
		(void)close(fd_out[0]);
		(void)close(fd_out[1]);
		callback(request, DB_CHECKPASSWORD_STATUS_INTERNAL_FAILURE,
			 NULL, context);
		return;
	}

	if (pid == 0) {
		/* child */
		(void)close(fd_in[0]);
		(void)close(fd_out[1]);
		checkpassword_exec(db, request, fd_in[1], fd_out[0],
				   auth_password != NULL);
		/* not reached */
	}

	if (close(fd_in[1]) < 0) {
		auth_request_log_error(request, "checkpassword",
				       "close(fd_in[1]) failed: %m");
	}
	if (close(fd_out[0]) < 0) {
		auth_request_log_error(request, "checkpassword",
				       "close(fd_out[0]) failed: %m");
	}

	auth_request_ref(request);
	chkpw_auth_request = i_new(struct chkpw_auth_request, 1);
	chkpw_auth_request->db = db;
	chkpw_auth_request->pid = pid;
	chkpw_auth_request->fd_in = fd_in[0];
	chkpw_auth_request->fd_out = fd_out[1];
	chkpw_auth_request->auth_password = i_strdup(auth_password);
	chkpw_auth_request->request = request;
	chkpw_auth_request->output_len = output_len;
	chkpw_auth_request->input_buf = str_new(default_pool, 256);
	chkpw_auth_request->callback = callback;
	chkpw_auth_request->context = context;

	chkpw_auth_request->io_in =
		io_add(fd_in[0], IO_READ, checkpassword_child_input,
		       chkpw_auth_request);
	chkpw_auth_request->io_out =
		io_add(fd_out[1], IO_WRITE, checkpassword_child_output,
		       chkpw_auth_request);

	hash_table_insert(db->clients, POINTER_CAST(pid), chkpw_auth_request);
	child_wait_add_pid(db->child_wait, pid);
}

struct db_checkpassword *
db_checkpassword_init(const char *checkpassword_path,
		      const char *checkpassword_reply_path)
{
	struct db_checkpassword *db;

	db = i_new(struct db_checkpassword, 1);
	db->checkpassword_path = i_strdup(checkpassword_path);
	db->checkpassword_reply_path = i_strdup(checkpassword_reply_path);
	db->clients = hash_table_create(default_pool, default_pool, 0,
					NULL, NULL);
	db->child_wait =
		child_wait_new_with_pid((pid_t)-1, sigchld_handler, db);
	return db;
}

void db_checkpassword_deinit(struct db_checkpassword **_db)
{
	struct db_checkpassword *db = *_db;
	struct hash_iterate_context *iter;
	void *key, *value;

	*_db = NULL;

	iter = hash_table_iterate_init(db->clients);
	while (hash_table_iterate(iter, &key, &value)) {
		struct chkpw_auth_request *request = value;
		checkpassword_internal_failure(&request);
	}
	hash_table_iterate_deinit(&iter);

	child_wait_free(&db->child_wait);
	hash_table_destroy(&db->clients);
	i_free(db->checkpassword_reply_path);
	i_free(db->checkpassword_path);
	i_free(db);
}

#endif
