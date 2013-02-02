* Copyright (c) 2005-2013 Dovecot authors, see the included COPYING file */

#include "auth-common.h"
#include "str.h"
#include "auth-worker-server.h"
#include "userdb.h"
#include "userdb-blocking.h"

#include <stdlib.h>

struct blocking_userdb_iterate_context {
	struct userdb_iterate_context ctx;
	struct auth_worker_connection *conn;
	bool next;
	bool destroyed;
};

static bool user_callback(const char *reply, void *context)
{
	struct auth_request *request = context;
	enum userdb_result result;
	const char *args;

	if (strncmp(reply, "FAIL\t", 5) == 0) {
		result = USERDB_RESULT_INTERNAL_FAILURE;
		args = reply + 5;
	} else if (strncmp(reply, "NOTFOUND\t", 9) == 0) {
		result = USERDB_RESULT_USER_UNKNOWN;
		args = reply + 9;
	} else if (strncmp(reply, "OK\t", 3) == 0) {
		result = USERDB_RESULT_OK;
		args = reply + 3;
	} else {
		result = USERDB_RESULT_INTERNAL_FAILURE;
		i_error("BUG: auth-worker sent invalid user reply");
		args = "";
	}

	if (*args != '\0') {
		request->userdb_reply = auth_fields_init(request->pool);
		auth_fields_import(request->userdb_reply, args, 0);
		if (auth_fields_exists(request->userdb_reply, "tempfail"))
			request->userdb_lookup_failed = TRUE;
	}

        auth_request_userdb_callback(result, request);
	auth_request_unref(&request);
	return TRUE;
}

void userdb_blocking_lookup(struct auth_request *request)
{
	string_t *str;

	str = t_str_new(128);
	str_printfa(str, "USER\t%u\t", request->userdb->userdb->id);
	auth_request_export(request, str);

	auth_request_ref(request);
	auth_worker_call(request->pool, str_c(str), user_callback, request);
}

static bool iter_callback(const char *reply, void *context)
{
	struct blocking_userdb_iterate_context *ctx = context;

	if (strncmp(reply, "*\t", 2) == 0) {
		ctx->next = FALSE;
		ctx->ctx.callback(reply + 2, ctx->ctx.context);
		return ctx->next;
	}

	if (strcmp(reply, "OK") != 0)
		ctx->ctx.failed = TRUE;
	if (!ctx->destroyed)
		ctx->ctx.callback(NULL, ctx->ctx.context);
	auth_request_unref(&ctx->ctx.auth_request);
	return TRUE;
}

struct userdb_iterate_context *
userdb_blocking_iter_init(struct auth_request *request,
			  userdb_iter_callback_t *callback, void *context)
{
	struct blocking_userdb_iterate_context *ctx;
	string_t *str;

	str = t_str_new(128);
	str_printfa(str, "LIST\t%u\t", request->userdb->userdb->id);
	auth_request_export(request, str);

	ctx = p_new(request->pool, struct blocking_userdb_iterate_context, 1);
	ctx->ctx.auth_request = request;
	ctx->ctx.callback = callback;
	ctx->ctx.context = context;

	auth_request_ref(request);
	ctx->conn = auth_worker_call(request->pool, str_c(str), iter_callback, ctx);
	return &ctx->ctx;
}

void userdb_blocking_iter_next(struct userdb_iterate_context *_ctx)
{
	struct blocking_userdb_iterate_context *ctx =
		(struct blocking_userdb_iterate_context *)_ctx;

	ctx->next = TRUE;
	auth_worker_server_resume_input(ctx->conn);
}

int userdb_blocking_iter_deinit(struct userdb_iterate_context **_ctx)
{
	struct blocking_userdb_iterate_context *ctx =
		(struct blocking_userdb_iterate_context *)*_ctx;
	int ret = ctx->ctx.failed ? -1 : 0;

	*_ctx = NULL;

	/* iter_callback() may still be called */
	ctx->destroyed = TRUE;
	return ret;
}
