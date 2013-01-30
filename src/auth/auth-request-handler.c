/* Copyright (c) 2005-2012 Dovecot authors, see the included COPYING file */

#include "auth-common.h"
#include "ioloop.h"
#include "array.h"
#include "aqueue.h"
#include "base64.h"
#include "hash.h"
#include "net.h"
#include "str.h"
#include "strescape.h"
#include "str-sanitize.h"
#include "master-interface.h"
#include "auth-penalty.h"
#include "auth-request.h"
#include "auth-token.h"
#include "auth-master-connection.h"
#include "auth-request-handler.h"

#include <stdlib.h>

#define AUTH_FAILURE_DELAY_CHECK_MSECS 500

struct auth_request_handler {
	int refcount;
	pool_t pool;
	HASH_TABLE(void *, struct auth_request *) requests;

        unsigned int connect_uid, client_pid;

	auth_request_callback_t *callback;
	void *context;

	auth_request_callback_t *master_callback;

	unsigned int destroyed:1;
	unsigned int token_auth:1;
};

static ARRAY(struct auth_request *) auth_failures_arr;
static struct aqueue *auth_failures;
static struct timeout *to_auth_failures;

static void auth_failure_timeout(void *context) ATTR_NULL(1);

#undef auth_request_handler_create
struct auth_request_handler *
auth_request_handler_create(bool token_auth, auth_request_callback_t *callback,
			    void *context, auth_request_callback_t *master_callback)
{
	struct auth_request_handler *handler;
	pool_t pool;

	pool = pool_alloconly_create("auth request handler", 4096);

	handler = p_new(pool, struct auth_request_handler, 1);
	handler->refcount = 1;
	handler->pool = pool;
	hash_table_create_direct(&handler->requests, pool, 0);
	handler->callback = callback;
	handler->context = context;
	handler->master_callback = master_callback;
	handler->token_auth = token_auth;
	return handler;
}

unsigned int
auth_request_handler_get_request_count(struct auth_request_handler *handler)
{
	return hash_table_count(handler->requests);
}

void auth_request_handler_abort_requests(struct auth_request_handler *handler)
{
	struct hash_iterate_context *iter;
	void *key;
	struct auth_request *auth_request;

	iter = hash_table_iterate_init(handler->requests);
	while (hash_table_iterate(iter, handler->requests, &key, &auth_request)) {
		switch (auth_request->state) {
		case AUTH_REQUEST_STATE_NEW:
		case AUTH_REQUEST_STATE_MECH_CONTINUE:
		case AUTH_REQUEST_STATE_FINISHED:
			auth_request_unref(&auth_request);
			hash_table_remove(handler->requests, key);
			break;
		case AUTH_REQUEST_STATE_PASSDB:
		case AUTH_REQUEST_STATE_USERDB:
			/* can't abort a pending passdb/userdb lookup */
			break;
		case AUTH_REQUEST_STATE_MAX:
			i_unreached();
		}
	}
	hash_table_iterate_deinit(&iter);
}

void auth_request_handler_unref(struct auth_request_handler **_handler)
{
        struct auth_request_handler *handler = *_handler;

	*_handler = NULL;

	i_assert(handler->refcount > 0);
	if (--handler->refcount > 0)
		return;

	i_assert(hash_table_count(handler->requests) == 0);

	/* notify parent that we're done with all requests */
	handler->callback(NULL, handler->context);

	hash_table_destroy(&handler->requests);
	pool_unref(&handler->pool);
}

void auth_request_handler_destroy(struct auth_request_handler **_handler)
{
	struct auth_request_handler *handler = *_handler;

	*_handler = NULL;

	i_assert(!handler->destroyed);

	handler->destroyed = TRUE;
	auth_request_handler_unref(&handler);
}

void auth_request_handler_set(struct auth_request_handler *handler,
			      unsigned int connect_uid,
			      unsigned int client_pid)
{
	handler->connect_uid = connect_uid;
	handler->client_pid = client_pid;
}

static void auth_request_handler_remove(struct auth_request_handler *handler,
					struct auth_request *request)
{
	i_assert(request->handler == handler);

	if (request->removed_from_handler) {
		/* already removed it */
		return;
	}
	request->removed_from_handler = TRUE;

	/* if db lookup is stuck, this call doesn't actually free the auth
	   request, so make sure we don't get back here. */
	timeout_remove(&request->to_abort);

	hash_table_remove(handler->requests, POINTER_CAST(request->id));
	auth_request_unref(&request);
}

static void
auth_str_add_keyvalue(string_t *dest, const char *key, const char *value)
{
	str_append_c(dest, '\t');
	str_append(dest, key);
	str_append_c(dest, '=');
	str_append_tabescaped(dest, value);
}

static void
auth_str_append_extra_fields(struct auth_request *request, string_t *dest)
{
	if (auth_stream_is_empty(request->extra_fields))
		return;

	str_append_c(dest, '\t');
	auth_stream_reply_append(request->extra_fields, dest, FALSE);

	if (request->proxy && !request->auth_only) {
		/* we're proxying */
		if (!auth_stream_reply_exists(request->extra_fields, "pass") &&
		    request->mech_password != NULL) {
			/* send back the password that was sent by user
			   (not the password in passdb). */
			auth_str_add_keyvalue(dest, "pass",
					      request->mech_password);
		}
		if (request->master_user != NULL &&
		    !auth_stream_reply_exists(request->extra_fields, "master")) {
			/* the master username needs to be forwarded */
			auth_str_add_keyvalue(dest, "master",
					      request->master_user);
		}
	}
}

static void
auth_request_handle_failure(struct auth_request *request, const char *reply)
{
        struct auth_request_handler *handler = request->handler;

	if (request->delayed_failure) {
		/* we came here from flush_failures() */
		handler->callback(reply, handler->context);
		return;
	}

	/* remove the request from requests-list */
	auth_request_ref(request);
	auth_request_handler_remove(handler, request);

	if (request->no_failure_delay) {
		/* passdb specifically requested not to delay the reply. */
		handler->callback(reply, handler->context);
		auth_request_unref(&request);
		return;
	}

	/* failure. don't announce it immediately to avoid
	   a) timing attacks, b) flooding */
	request->delayed_failure = TRUE;
	handler->refcount++;

	if (auth_penalty != NULL) {
		auth_penalty_update(auth_penalty, request,
				    request->last_penalty + 1);
	}

	auth_request_refresh_last_access(request);
	aqueue_append(auth_failures, &request);
	if (to_auth_failures == NULL) {
		to_auth_failures =
			timeout_add_short(AUTH_FAILURE_DELAY_CHECK_MSECS,
					  auth_failure_timeout, (void *)NULL);
	}
}

static void
auth_request_handler_reply_success_finish(struct auth_request *request)
{
        struct auth_request_handler *handler = request->handler;
	string_t *str = t_str_new(128);

	if (request->last_penalty != 0 && auth_penalty != NULL) {
		/* reset penalty */
		auth_penalty_update(auth_penalty, request, 0);
	}

	str_printfa(str, "OK\t%u\tuser=", request->id);
	str_append_tabescaped(str, request->user);
	auth_str_append_extra_fields(request, str);
	if (request->no_login || handler->master_callback == NULL) {
		/* this request doesn't have to wait for master
		   process to pick it up. delete it */
		auth_request_handler_remove(handler, request);
	}
	handler->callback(str_c(str), handler->context);
}

static void
auth_request_handler_reply_failure_finish(struct auth_request *request)
{
	string_t *str = t_str_new(128);

	str_printfa(str, "FAIL\t%u", request->id);
	if (request->user != NULL)
		auth_str_add_keyvalue(str, "user", request->user);
	else if (request->original_username != NULL) {
		auth_str_add_keyvalue(str, "user", 
				      request->original_username);
	}

	if (request->internal_failure)
		str_append(str, "\ttemp");
	else if (request->master_user != NULL) {
		/* authentication succeeded, but we can't log in
		   as the wanted user */
		str_append(str, "\tauthz");
	}
	if (request->no_failure_delay)
		str_append(str, "\tnodelay");
	auth_str_append_extra_fields(request, str);

	switch (request->passdb_result) {
	case PASSDB_RESULT_INTERNAL_FAILURE:
	case PASSDB_RESULT_SCHEME_NOT_AVAILABLE:
	case PASSDB_RESULT_USER_UNKNOWN:
	case PASSDB_RESULT_PASSWORD_MISMATCH:
	case PASSDB_RESULT_OK:
		break;
	case PASSDB_RESULT_USER_DISABLED:
		str_append(str, "\tuser_disabled");
		break;
	case PASSDB_RESULT_PASS_EXPIRED:
		str_append(str, "\tpass_expired");
		break;
	}

	auth_request_handle_failure(request, str_c(str));
}

static void
auth_request_handler_proxy_callback(bool success, struct auth_request *request)
{
        struct auth_request_handler *handler = request->handler;

	if (success)
		auth_request_handler_reply_success_finish(request);
	else
		auth_request_handler_reply_failure_finish(request);
        auth_request_handler_unref(&handler);
}

void auth_request_handler_reply(struct auth_request *request,
				enum auth_client_result result,
				const void *auth_reply, size_t reply_size)
{
        struct auth_request_handler *handler = request->handler;
	string_t *str;
	int ret;

	if (handler->destroyed) {
		/* the client connection was already closed. we can't do
		   anything but abort this request */
		request->internal_failure = TRUE;
		result = AUTH_CLIENT_RESULT_FAILURE;
		/* make sure this request is set to finished state
		   (it's not with result=continue) */
		auth_request_set_state(request, AUTH_REQUEST_STATE_FINISHED);
	}

	switch (result) {
	case AUTH_CLIENT_RESULT_CONTINUE:
		str = t_str_new(16 + MAX_BASE64_ENCODED_SIZE(reply_size));
		str_printfa(str, "CONT\t%u\t", request->id);
		base64_encode(auth_reply, reply_size, str);

		request->accept_input = TRUE;
		handler->callback(str_c(str), handler->context);
		break;
	case AUTH_CLIENT_RESULT_SUCCESS:
		if (reply_size > 0) {
			str = t_str_new(MAX_BASE64_ENCODED_SIZE(reply_size));
			base64_encode(auth_reply, reply_size, str);
			auth_stream_reply_add(request->extra_fields, "resp",
					      str_c(str), 0);
		}
		ret = auth_request_proxy_finish(request,
				auth_request_handler_proxy_callback);
		if (ret < 0)
			auth_request_handler_reply_failure_finish(request);
		else if (ret > 0)
			auth_request_handler_reply_success_finish(request);
		else
			return;
		break;
	case AUTH_CLIENT_RESULT_FAILURE:
		auth_request_proxy_finish_failure(request);
		auth_request_handler_reply_failure_finish(request);
		break;
	}
	/* NOTE: request may be destroyed now */

        auth_request_handler_unref(&handler);
}

void auth_request_handler_reply_continue(struct auth_request *request,
					 const void *reply, size_t reply_size)
{
	auth_request_handler_reply(request, AUTH_CLIENT_RESULT_CONTINUE,
				   reply, reply_size);
}

static void auth_request_handler_auth_fail(struct auth_request_handler *handler,
					   struct auth_request *request,
					   const char *reason)
{
	string_t *str = t_str_new(128);

	auth_request_log_info(request, request->mech->mech_name, "%s", reason);

	str_printfa(str, "FAIL\t%u\treason=", request->id);
	str_append_tabescaped(str, reason);

	handler->callback(str_c(str), handler->context);
	auth_request_handler_remove(handler, request);
}

static void auth_request_timeout(struct auth_request *request)
{
	unsigned int secs = (unsigned int)(time(NULL) - request->last_access);

	if (request->state != AUTH_REQUEST_STATE_MECH_CONTINUE) {
		/* client's fault */
		auth_request_log_error(request, request->mech->mech_name,
			"Request %u.%u timed out after %u secs, state=%d",
			request->handler->client_pid, request->id,
			secs, request->state);
	} else if (request->set->verbose) {
		auth_request_log_info(request, request->mech->mech_name,
			"Request timed out waiting for client to continue authentication "
			"(%u secs)", secs);
	}
	auth_request_handler_remove(request->handler, request);
}

static void auth_request_penalty_finish(struct auth_request *request)
{
	timeout_remove(&request->to_penalty);
	auth_request_initial(request);
}

static void
auth_penalty_callback(unsigned int penalty, struct auth_request *request)
{
	unsigned int secs;

	request->last_penalty = penalty;

	if (penalty == 0)
		auth_request_initial(request);
	else {
		secs = auth_penalty_to_secs(penalty);
		request->to_penalty = timeout_add(secs * 1000,
						  auth_request_penalty_finish,
						  request);
	}
}

bool auth_request_handler_auth_begin(struct auth_request_handler *handler,
				     const char *args)
{
	const struct mech_module *mech;
	struct auth_request *request;
	const char *const *list, *name, *arg, *initial_resp;
	void *initial_resp_data;
	unsigned int id;
	buffer_t *buf;

	i_assert(!handler->destroyed);

	/* <id> <mechanism> [...] */
	list = t_strsplit_tab(args);
	if (list[0] == NULL || list[1] == NULL ||
	    str_to_uint(list[0], &id) < 0) {
		i_error("BUG: Authentication client %u "
			"sent broken AUTH request", handler->client_pid);
		return FALSE;
	}

	if (handler->token_auth) {
		mech = &mech_dovecot_token;
		if (strcmp(list[1], mech->mech_name) != 0) {
			/* unsupported mechanism */
			i_error("BUG: Authentication client %u requested invalid "
				"authentication mechanism %s (DOVECOT-TOKEN required)",
				handler->client_pid, str_sanitize(list[1], MAX_MECH_NAME_LEN));
			return FALSE;
		}
	} else {		 
		mech = mech_module_find(list[1]);
		if (mech == NULL) {
			/* unsupported mechanism */
			i_error("BUG: Authentication client %u requested unsupported "
				"authentication mechanism %s", handler->client_pid,
				str_sanitize(list[1], MAX_MECH_NAME_LEN));
			return FALSE;
		}
	}

	request = auth_request_new(mech);
	request->handler = handler;
	request->connect_uid = handler->connect_uid;
	request->client_pid = handler->client_pid;
	request->id = id;
	request->auth_only = handler->master_callback == NULL;

	/* parse optional parameters */
	initial_resp = NULL;
	for (list += 2; *list != NULL; list++) {
		arg = strchr(*list, '=');
		if (arg == NULL) {
			name = *list;
			arg = "";
		} else {
			name = t_strdup_until(*list, arg);
			arg++;
		}

		if (auth_request_import_auth(request, name, arg))
			;
		else if (strcmp(name, "resp") == 0) {
			initial_resp = arg;
			/* this must be the last parameter */
			list++;
			break;
		}
	}

	if (*list != NULL) {
		i_error("BUG: Authentication client %u "
			"sent AUTH parameters after 'resp'",
			handler->client_pid);
		auth_request_unref(&request);
		return FALSE;
	}

	if (request->service == NULL) {
		i_error("BUG: Authentication client %u "
			"didn't specify service in request",
			handler->client_pid);
		auth_request_unref(&request);
		return FALSE;
	}
	if (hash_table_lookup(handler->requests, POINTER_CAST(id)) != NULL) {
		i_error("BUG: Authentication client %u "
			"sent a duplicate ID %u", handler->client_pid, id);
		auth_request_unref(&request);
		return FALSE;
	}
	auth_request_init(request);

	request->to_abort = timeout_add(MASTER_AUTH_SERVER_TIMEOUT_SECS * 1000,
					auth_request_timeout, request);
	hash_table_insert(handler->requests, POINTER_CAST(id), request);

	if (request->set->ssl_require_client_cert &&
	    !request->valid_client_cert) {
		/* we fail without valid certificate */
                auth_request_handler_auth_fail(handler, request,
			"Client didn't present valid SSL certificate");
		return TRUE;
	}

	/* Empty initial response is a "=" base64 string. Completely empty
	   string shouldn't really be sent, but at least Exim does it,
	   so just allow it for backwards compatibility.. */
	if (initial_resp != NULL && *initial_resp != '\0') {
		size_t len = strlen(initial_resp);

		buf = buffer_create_dynamic(pool_datastack_create(),
					    MAX_BASE64_DECODED_SIZE(len));
		if (base64_decode(initial_resp, len, NULL, buf) < 0) {
                        auth_request_handler_auth_fail(handler, request,
				"Invalid base64 data in initial response");
			return TRUE;
		}
		initial_resp_data =
			p_malloc(request->pool, I_MAX(buf->used, 1));
		memcpy(initial_resp_data, buf->data, buf->used);
		request->initial_response = initial_resp_data;
		request->initial_response_len = buf->used;
	}

	/* handler is referenced until auth_request_handler_reply()
	   is called. */
	handler->refcount++;

	/* before we start authenticating, see if we need to wait first */
	auth_penalty_lookup(auth_penalty, request, auth_penalty_callback);
	return TRUE;
}

bool auth_request_handler_auth_continue(struct auth_request_handler *handler,
					const char *args)
{
	struct auth_request *request;
	const char *data;
	size_t data_len;
	buffer_t *buf;
	unsigned int id;

	data = strchr(args, '\t');
	if (data == NULL || str_to_uint(t_strdup_until(args, data), &id) < 0) {
		i_error("BUG: Authentication client sent broken CONT request");
		return FALSE;
	}
	data++;

	request = hash_table_lookup(handler->requests, POINTER_CAST(id));
	if (request == NULL) {
		const char *reply = t_strdup_printf(
			"FAIL\t%u\treason=Authentication request timed out", id);
		handler->callback(reply, handler->context);
		return TRUE;
	}

	/* accept input only once after mechanism has sent a CONT reply */
	if (!request->accept_input) {
		auth_request_handler_auth_fail(handler, request,
					       "Unexpected continuation");
		return TRUE;
	}
	request->accept_input = FALSE;

	data_len = strlen(data);
	buf = buffer_create_dynamic(pool_datastack_create(),
				    MAX_BASE64_DECODED_SIZE(data_len));
	if (base64_decode(data, data_len, NULL, buf) < 0) {
		auth_request_handler_auth_fail(handler, request,
			"Invalid base64 data in continued response");
		return TRUE;
	}

	/* handler is referenced until auth_request_handler_reply()
	   is called. */
	handler->refcount++;
	auth_request_continue(request, buf->data, buf->used);
	return TRUE;
}

static void userdb_callback(enum userdb_result result,
			    struct auth_request *request)
{
        struct auth_request_handler *handler = request->handler;
	string_t *str;
	const char *value;

	i_assert(request->state == AUTH_REQUEST_STATE_USERDB);

	auth_request_set_state(request, AUTH_REQUEST_STATE_FINISHED);

	if (request->userdb_lookup_failed)
		result = USERDB_RESULT_INTERNAL_FAILURE;

	str = t_str_new(128);
	switch (result) {
	case USERDB_RESULT_INTERNAL_FAILURE:
		str_printfa(str, "FAIL\t%u", request->id);
		if (request->userdb_lookup_failed) {
			value = auth_stream_reply_find(request->userdb_reply,
						       "reason");
			if (value != NULL)
				auth_str_add_keyvalue(str, "reason", value);
		}
		break;
	case USERDB_RESULT_USER_UNKNOWN:
		str_printfa(str, "NOTFOUND\t%u", request->id);
		break;
	case USERDB_RESULT_OK:
		str_printfa(str, "USER\t%u\t", request->id);
		str_append_tabescaped(str, request->user);
		auth_stream_reply_append(request->userdb_reply, str, FALSE);

		if (request->master_user != NULL &&
		    !auth_stream_reply_exists(request->userdb_reply,
					      "master_user")) {
			auth_str_add_keyvalue(str, "master_user",
					      request->master_user);
		}
		if (*request->set->anonymous_username != '\0' &&
		    strcmp(request->user,
			   request->set->anonymous_username) == 0) {
			/* this is an anonymous login, either via ANONYMOUS
			   SASL mechanism or simply logging in as the anonymous
			   user via another mechanism */
			str_append(str, "\tanonymous");
		}
		/* generate auth_token when master service provided session_pid */
		if (request->session_pid != (pid_t)-1) {
			const char *auth_token =
				auth_token_get(request->service,
					       dec2str(request->session_pid),
					       request->user,
					       request->session_id);
			auth_str_add_keyvalue(str, "auth_token", auth_token);
		}
		break;
	}
	handler->master_callback(str_c(str), request->master);

	auth_master_connection_unref(&request->master);
	auth_request_unref(&request);
        auth_request_handler_unref(&handler);
}

static bool
auth_master_request_failed(struct auth_request_handler *handler,
			   struct auth_master_connection *master,
			   unsigned int id)
{
	if (handler->master_callback == NULL)
		return FALSE;
	handler->master_callback(t_strdup_printf("FAIL\t%u", id), master);
	return TRUE;
}

bool auth_request_handler_master_request(struct auth_request_handler *handler,
					 struct auth_master_connection *master,
					 unsigned int id, unsigned int client_id,
					 const char *const *params)
{
	struct auth_request *request;
	struct net_unix_cred cred;

	request = hash_table_lookup(handler->requests, POINTER_CAST(client_id));
	if (request == NULL) {
		i_error("Master request %u.%u not found",
			handler->client_pid, client_id);
		return auth_master_request_failed(handler, master, id);
	}

	auth_request_ref(request);
	auth_request_handler_remove(handler, request);

	for (; *params != NULL; params++) {
		const char *name, *param = strchr(*params, '=');

		if (param == NULL) {
			name = *params;
			param = "";
		} else {
			name = t_strdup_until(*params, param);
			param++;
		}

		(void)auth_request_import_master(request, name, param);
	}

	/* verify session pid if specified and possible */
	if (request->session_pid != (pid_t)-1 &&
	    net_getunixcred(master->fd, &cred) == 0 &&
	    cred.pid != (pid_t)-1 && request->session_pid != cred.pid) {
		i_error("Session pid %ld provided by master for request %u.%u "
			"did not match peer credentials (pid=%ld, uid=%ld)",
			(long)request->session_pid,
			handler->client_pid, client_id,
			(long)cred.pid, (long)cred.uid);
		return auth_master_request_failed(handler, master, id);
	}

	if (request->state != AUTH_REQUEST_STATE_FINISHED ||
	    !request->successful) {
		i_error("Master requested unfinished authentication request "
			"%u.%u", handler->client_pid, client_id);
		handler->master_callback(t_strdup_printf("FAIL\t%u", id),
					 master);
		auth_request_unref(&request);
	} else {
		/* the request isn't being referenced anywhere anymore,
		   so we can do a bit of kludging.. replace the request's
		   old client_id with master's id. */
		auth_request_set_state(request, AUTH_REQUEST_STATE_USERDB);
		request->id = id;
		request->master = master;

		/* master and handler are referenced until userdb_callback i
		   s called. */
		auth_master_connection_ref(master);
		handler->refcount++;
		auth_request_lookup_user(request, userdb_callback);
	}
	return TRUE;
}

void auth_request_handler_cancel_request(struct auth_request_handler *handler,
					 unsigned int client_id)
{
	struct auth_request *request;

	request = hash_table_lookup(handler->requests, POINTER_CAST(client_id));
	if (request != NULL)
		auth_request_handler_remove(handler, request);
}

void auth_request_handler_flush_failures(bool flush_all)
{
	struct auth_request **auth_requests, *auth_request;
	unsigned int i, count;
	time_t diff;

	count = aqueue_count(auth_failures);
	if (count == 0) {
		if (to_auth_failures != NULL)
			timeout_remove(&to_auth_failures);
		return;
	}

	auth_requests = array_idx_modifiable(&auth_failures_arr, 0);
	for (i = 0; i < count; i++) {
		auth_request = auth_requests[aqueue_idx(auth_failures, 0)];

		/* FIXME: assumess that failure_delay is always the same. */
		diff = ioloop_time - auth_request->last_access;
		if (diff < (time_t)auth_request->set->failure_delay &&
		    !flush_all)
			break;

		aqueue_delete_tail(auth_failures);

		i_assert(auth_request->state == AUTH_REQUEST_STATE_FINISHED);
		auth_request_handler_reply(auth_request,
					   AUTH_CLIENT_RESULT_FAILURE,
					   &uchar_nul, 0);
		auth_request_unref(&auth_request);
	}
}

static void auth_failure_timeout(void *context ATTR_UNUSED)
{
	auth_request_handler_flush_failures(FALSE);
}

void auth_request_handler_init(void)
{
	i_array_init(&auth_failures_arr, 128);
	auth_failures = aqueue_init(&auth_failures_arr.arr);
}

void auth_request_handler_deinit(void)
{
	auth_request_handler_flush_failures(TRUE);
	array_free(&auth_failures_arr);
	aqueue_deinit(&auth_failures);

	if (to_auth_failures != NULL)
		timeout_remove(&to_auth_failures);
}
