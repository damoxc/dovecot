/* Copyright (c) 2005-2008 Dovecot authors, see the included COPYING file */

#include "common.h"
#include "base64.h"
#include "ioloop.h"
#include "network.h"
#include "istream.h"
#include "ostream.h"
#include "str.h"
#include "auth-request.h"
#include "auth-worker-client.h"

#include <stdlib.h>

#define OUTBUF_THROTTLE_SIZE (1024*10)

struct auth_worker_client {
	int refcount;

        struct auth *auth;
	int fd;
	struct io *io;
	struct istream *input;
	struct ostream *output;
};

static void
auth_worker_client_check_throttle(struct auth_worker_client *client)
{
	if (o_stream_get_buffer_used_size(client->output) >=
	    OUTBUF_THROTTLE_SIZE) {
		/* stop reading new requests until client has read the pending
		   replies. */
		if (client->io != NULL)
			io_remove(&client->io);
	}
}

static struct auth_request *
worker_auth_request_new(struct auth_worker_client *client, unsigned int id,
			const char *args)
{
	struct auth_request *auth_request;
	const char *key, *value, *const *tmp;

	auth_request = auth_request_new_dummy(client->auth);

	client->refcount++;
	auth_request->context = client;
	auth_request->id = id;

	if (args != NULL) {
		for (tmp = t_strsplit(args, "\t"); *tmp != NULL; tmp++) {
			value = strchr(*tmp, '=');
			if (value == NULL)
				continue;

			key = t_strdup_until(*tmp, value);
			value++;

			(void)auth_request_import(auth_request, key, value);
		}
	}

	return auth_request;
}

static void
add_userdb_replies(struct auth_stream_reply *reply,
		   struct auth_stream_reply *userdb_reply)
{
	const char *const *tmp;

	tmp = auth_stream_split(userdb_reply);
	i_assert(*tmp != NULL);
	/* first field is the user name */
	tmp++;
	for (; *tmp != NULL; tmp++) {
		auth_stream_reply_import(reply,
					 t_strconcat("userdb_", *tmp, NULL));
	}
}

static void verify_plain_callback(enum passdb_result result,
				  struct auth_request *request)
{
	struct auth_worker_client *client = request->context;
	struct auth_stream_reply *reply;
	string_t *str;

	if (request->passdb_failure && result == PASSDB_RESULT_OK)
		result = PASSDB_RESULT_PASSWORD_MISMATCH;

	reply = auth_stream_reply_init(pool_datastack_create());
	auth_stream_reply_add(reply, NULL, dec2str(request->id));

	if (result == PASSDB_RESULT_OK)
		auth_stream_reply_add(reply, "OK", NULL);
	else {
		auth_stream_reply_add(reply, "FAIL", NULL);
		auth_stream_reply_add(reply, NULL,
				      t_strdup_printf("%d", result));
	}
	if (result != PASSDB_RESULT_INTERNAL_FAILURE) {
		auth_stream_reply_add(reply, NULL, request->user);
		auth_stream_reply_add(reply, NULL,
				      request->passdb_password == NULL ? "" :
				      request->passdb_password);
		if (request->no_password)
			auth_stream_reply_add(reply, "nopassword", NULL);
		if (request->userdb_reply != NULL)
			add_userdb_replies(reply, request->userdb_reply);
		if (request->extra_fields != NULL) {
			const char *fields =
				auth_stream_reply_export(request->extra_fields);
			auth_stream_reply_import(reply, fields);
		}
	}
	str = auth_stream_reply_get_str(reply);
	str_append_c(str, '\n');
	o_stream_send(client->output, str_data(str), str_len(str));

	auth_request_unref(&request);
	auth_worker_client_check_throttle(client);
	auth_worker_client_unref(&client);
}

static void
auth_worker_handle_passv(struct auth_worker_client *client,
			 unsigned int id, const char *args)
{
	/* verify plaintext password */
	struct auth_request *auth_request;
        struct auth_passdb *passdb;
	const char *password;
	unsigned int passdb_id;

	passdb_id = atoi(t_strcut(args, '\t'));
	args = strchr(args, '\t');
	if (args == NULL) {
		i_error("BUG: Auth worker server sent us invalid PASSV");
		return;
	}
	args++;

	password = t_strcut(args, '\t');
	args = strchr(args, '\t');
	if (args != NULL) args++;

	auth_request = worker_auth_request_new(client, id, args);
	auth_request->mech_password =
		p_strdup(auth_request->pool, password);

	if (auth_request->user == NULL || auth_request->service == NULL) {
		i_error("BUG: PASSV had missing parameters");
		auth_request_unref(&auth_request);
		return;
	}

	passdb = auth_request->passdb;
	while (passdb != NULL && passdb->id != passdb_id)
		passdb = passdb->next;

	if (passdb == NULL) {
		/* could be a masterdb */
		passdb = auth_request->auth->masterdbs;
		while (passdb != NULL && passdb->id != passdb_id)
			passdb = passdb->next;

		if (passdb == NULL) {
			i_error("BUG: PASSV had invalid passdb ID");
			auth_request_unref(&auth_request);
			return;
		}
	}

	auth_request->passdb = passdb;
	passdb->passdb->iface.
		verify_plain(auth_request, password, verify_plain_callback);
}

static void
lookup_credentials_callback(enum passdb_result result,
			    const unsigned char *credentials, size_t size,
			    struct auth_request *request)
{
	struct auth_worker_client *client = request->context;
	struct auth_stream_reply *reply;
	string_t *str;

	if (request->passdb_failure && result == PASSDB_RESULT_OK)
		result = PASSDB_RESULT_PASSWORD_MISMATCH;

	reply = auth_stream_reply_init(pool_datastack_create());
	auth_stream_reply_add(reply, NULL, dec2str(request->id));

	if (result != PASSDB_RESULT_OK) {
		auth_stream_reply_add(reply, "FAIL", NULL);
		auth_stream_reply_add(reply, NULL,
				      t_strdup_printf("%d", result));
	} else {
		auth_stream_reply_add(reply, "OK", NULL);
		auth_stream_reply_add(reply, NULL, request->user);

		str = t_str_new(64);
		str_printfa(str, "{%s.b64}", request->credentials_scheme);
		base64_encode(credentials, size, str);
		auth_stream_reply_add(reply, NULL, str_c(str));

		if (request->extra_fields != NULL) {
			const char *fields =
				auth_stream_reply_export(request->extra_fields);
			auth_stream_reply_import(reply, fields);
		}
		if (request->userdb_reply != NULL)
			add_userdb_replies(reply, request->userdb_reply);
	}
	str = auth_stream_reply_get_str(reply);
	str_append_c(str, '\n');
	o_stream_send(client->output, str_data(str), str_len(str));

	auth_request_unref(&request);
	auth_worker_client_check_throttle(client);
	auth_worker_client_unref(&client);
}

static void
auth_worker_handle_passl(struct auth_worker_client *client,
			 unsigned int id, const char *args)
{
	/* lookup credentials */
	struct auth_request *auth_request;
	const char *scheme;
	unsigned int passdb_id;

	passdb_id = atoi(t_strcut(args, '\t'));
	args = strchr(args, '\t');
	if (args == NULL) {
		i_error("BUG: Auth worker server sent us invalid PASSL");
		return;
	}
	args++;

	scheme = t_strcut(args, '\t');
	args = strchr(args, '\t');
	if (args != NULL) args++;

	auth_request = worker_auth_request_new(client, id, args);
	auth_request->credentials_scheme = p_strdup(auth_request->pool, scheme);

	if (auth_request->user == NULL || auth_request->service == NULL) {
		i_error("BUG: PASSL had missing parameters");
		auth_request_unref(&auth_request);
		return;
	}

	while (auth_request->passdb->id != passdb_id) {
		auth_request->passdb = auth_request->passdb->next;
		if (auth_request->passdb == NULL) {
			i_error("BUG: PASSL had invalid passdb ID");
			auth_request_unref(&auth_request);
			return;
		}
	}

	if (auth_request->passdb->passdb->iface.lookup_credentials == NULL) {
		i_error("BUG: PASSL lookup not supported by given passdb");
		auth_request_unref(&auth_request);
		return;
	}

	auth_request->passdb->passdb->iface.
		lookup_credentials(auth_request, lookup_credentials_callback);
}

static void
set_credentials_callback(bool success, struct auth_request *request)
{
	struct auth_worker_client *client = request->context;

	string_t *str;

	str = t_str_new(64);
	str_printfa(str, "%u\t%s\n", request->id, success ? "OK" : "FAIL");
	o_stream_send(client->output, str_data(str), str_len(str));

	auth_request_unref(&request);
	auth_worker_client_check_throttle(client);
	auth_worker_client_unref(&client);
}

static void
auth_worker_handle_setcred(struct auth_worker_client *client,
			   unsigned int id, const char *args)
{
	struct auth_request *auth_request;
	unsigned int passdb_id;
	const char *data;

	passdb_id = atoi(t_strcut(args, '\t'));
	args = strchr(args, '\t');
	if (args == NULL) {
		i_error("BUG: Auth worker server sent us invalid SETCRED");
		return;
	}
	args++;

	data = t_strcut(args, '\t');
	args = strchr(args, '\t');
	if (args != NULL) args++;

	auth_request = worker_auth_request_new(client, id, args);

	if (auth_request->user == NULL || auth_request->service == NULL) {
		i_error("BUG: SETCRED had missing parameters");
		auth_request_unref(&auth_request);
		return;
	}

	while (auth_request->passdb->id != passdb_id) {
		auth_request->passdb = auth_request->passdb->next;
		if (auth_request->passdb == NULL) {
			i_error("BUG: SETCRED had invalid passdb ID");
			auth_request_unref(&auth_request);
			return;
		}
	}

	auth_request->passdb->passdb->iface.
		set_credentials(auth_request, data, set_credentials_callback);
}

static void
lookup_user_callback(enum userdb_result result,
		     struct auth_request *auth_request)
{
	struct auth_worker_client *client = auth_request->context;
	struct auth_stream_reply *reply = auth_request->userdb_reply;
	string_t *str;

	if (auth_request->userdb_lookup_failed)
		result = USERDB_RESULT_INTERNAL_FAILURE;

	str = t_str_new(128);
	str_printfa(str, "%u\t", auth_request->id);
	switch (result) {
	case USERDB_RESULT_INTERNAL_FAILURE:
		str_append(str, "FAIL\t");
		break;
	case USERDB_RESULT_USER_UNKNOWN:
		str_append(str, "NOTFOUND\t");
		break;
	case USERDB_RESULT_OK:
		str_append(str, "OK\t");
		str_append(str, auth_stream_reply_export(reply));
		break;
	}
	str_append_c(str, '\n');

	o_stream_send(client->output, str_data(str), str_len(str));

	auth_request_unref(&auth_request);
	auth_worker_client_check_throttle(client);
	auth_worker_client_unref(&client);
}

static void
auth_worker_handle_user(struct auth_worker_client *client,
			unsigned int id, const char *args)
{
	/* lookup user */
	struct auth_request *auth_request;
	unsigned int num;

	num = atoi(t_strcut(args, '\t'));
	args = strchr(args, '\t');
	if (args != NULL) args++;

	auth_request = worker_auth_request_new(client, id, args);

	if (auth_request->user == NULL || auth_request->service == NULL) {
		i_error("BUG: USER had missing parameters");
		auth_request_unref(&auth_request);
		return;
	}

	for (; num > 0; num--) {
		auth_request->userdb = auth_request->userdb->next;
		if (auth_request->userdb == NULL) {
			i_error("BUG: USER had invalid userdb num");
			auth_request_unref(&auth_request);
			return;
		}
	}

	auth_request->userdb->userdb->iface->
		lookup(auth_request, lookup_user_callback);
}

static bool
auth_worker_handle_line(struct auth_worker_client *client, const char *line)
{
	const char *p;
	unsigned int id;

	p = strchr(line, '\t');
	if (p == NULL)
		return FALSE;

	id = (unsigned int)strtoul(t_strdup_until(line, p), NULL, 10);
	line = p + 1;

	if (strncmp(line, "PASSV\t", 6) == 0)
		auth_worker_handle_passv(client, id, line + 6);
	else if (strncmp(line, "PASSL\t", 6) == 0)
		auth_worker_handle_passl(client, id, line + 6);
	else if (strncmp(line, "SETCRED\t", 8) == 0)
		auth_worker_handle_setcred(client, id, line + 8);
	else if (strncmp(line, "USER\t", 5) == 0)
		auth_worker_handle_user(client, id, line + 5);

        return TRUE;
}

static void auth_worker_input(struct auth_worker_client *client)
{
	char *line;
	bool ret;

	switch (i_stream_read(client->input)) {
	case 0:
		return;
	case -1:
		/* disconnected */
		auth_worker_client_destroy(&client);
		return;
	case -2:
		/* buffer full */
		i_error("BUG: Auth worker server sent us more than %d bytes",
			(int)AUTH_WORKER_MAX_LINE_LENGTH);
		auth_worker_client_destroy(&client);
		return;
	}

        client->refcount++;
	while ((line = i_stream_next_line(client->input)) != NULL) {
		T_BEGIN {
			ret = auth_worker_handle_line(client, line);
		} T_END;

		if (!ret) {
			auth_worker_client_destroy(&client);
			break;
		}
	}
	auth_worker_client_unref(&client);
}

static int auth_worker_output(struct auth_worker_client *client)
{
	if (o_stream_flush(client->output) < 0) {
		auth_worker_client_destroy(&client);
		return 1;
	}

	if (o_stream_get_buffer_used_size(client->output) <=
	    OUTBUF_THROTTLE_SIZE/3 && client->io == NULL) {
		/* allow input again */
		client->io = io_add(client->fd, IO_READ,
				    auth_worker_input, client);
	}
	return 1;
}

struct auth_worker_client *
auth_worker_client_create(struct auth *auth, int fd)
{
        struct auth_worker_client *client;

	client = i_new(struct auth_worker_client, 1);
	client->refcount = 1;

	client->auth = auth;
	client->fd = fd;
	client->input = i_stream_create_fd(fd, AUTH_WORKER_MAX_LINE_LENGTH,
					   FALSE);
	client->output = o_stream_create_fd(fd, (size_t)-1, FALSE);
	o_stream_set_flush_callback(client->output, auth_worker_output, client);
	client->io = io_add(fd, IO_READ, auth_worker_input, client);
	return client;
}

void auth_worker_client_destroy(struct auth_worker_client **_client)
{
	struct auth_worker_client *client = *_client;

	*_client = NULL;
	if (client->fd == -1)
		return;

	i_stream_close(client->input);
	o_stream_close(client->output);

	if (client->io != NULL)
		io_remove(&client->io);

	net_disconnect(client->fd);
	client->fd = -1;

	io_loop_stop(ioloop);
}

void auth_worker_client_unref(struct auth_worker_client **_client)
{
	struct auth_worker_client *client = *_client;

	if (--client->refcount > 0) {
		*_client = NULL;
		return;
	}

	if (client->fd != -1)
		auth_worker_client_destroy(_client);

	i_stream_unref(&client->input);
	o_stream_unref(&client->output);
	i_free(client);
}
