/* Copyright (c) 2013 Dovecot authors, see the included COPYING file */

#include "auth-common.h"
#include "userdb.h"

#ifdef USERDB_MONGODB

#include "str.h"
#include "hash.h"
#include "auth-cache.h"
#include "db-mongodb.h"

struct mongodb_userdb_module {
	struct userdb_module module;

	struct mongodb_connection *conn;
};

struct mongodb_userdb_iterate_context {
	struct userdb_iterate_context ctx;
	struct mongodb_result *result;
	unsigned int freed:1;
	unsigned int call_iter:1;
};

static void mongodb_query_get_result(struct mongodb_result *result,
				     struct auth_request *auth_request)
{
	struct mongodb_result_iterate_context *iter;
	const char *key;
	string_t *value;

    mongodb_result_var_expand(result, NULL);
	auth_request_init_userdb_reply(auth_request);

	iter = mongodb_result_iterate_init(result);
	while (mongodb_result_iterate(iter, &key, &value)) {
		if (*key != '\0' && value != NULL) {
			auth_request_set_userdb_field(auth_request, key, str_c(value));
		}
	}
	mongodb_result_iterate_deinit(&iter);
}

static void userdb_mongodb_lookup(struct auth_request *auth_request,
				  userdb_callback_t *callback)
{
	struct userdb_module *_module = auth_request->userdb->userdb;
	struct mongodb_userdb_module *module =
		(struct mongodb_userdb_module *)_module;
	enum userdb_result userdb_result = USERDB_RESULT_INTERNAL_FAILURE;
	mongodb_query_t mongodb_query;
	mongodb_result_t mongodb_result;

	string_t *query;
	int ret;

 	query = t_str_new(512);
	var_expand(query, module->conn->set.user_query,
		   auth_request_get_var_expand_table(auth_request,
						     NULL));

	auth_request_log_debug(auth_request, "mongodb",
			       "query: %s", str_c(query));

	mongodb_query = mongodb_query_init(module->conn->conn);
	mongodb_query_parse_query(mongodb_query, str_c(query));

	if (module->conn->set.user_defaults != NULL)
		mongodb_query_parse_defaults(mongodb_query,
					     module->conn->set.user_defaults);

	mongodb_query_parse_fields(mongodb_query, module->conn->set.user_fields);

	ret = mongodb_query_find_one(mongodb_query, module->conn->set.collection, &mongodb_result);
	if (ret != MONGODB_QUERY_OK) {
		if (ret == MONGODB_QUERY_NO_RESULT) {
			auth_request_log_info(auth_request, "mongodb", "unknown user");
			userdb_result = USERDB_RESULT_USER_UNKNOWN;
		} else {
			auth_request_log_error(auth_request, "mongodb",
				       "Query failed: %s", mongodb_get_error(module->conn->conn));
		}
	} else {
		mongodb_query_get_result(mongodb_result, auth_request);
		userdb_result = USERDB_RESULT_OK;
	}

	callback(userdb_result, auth_request);
}

static struct userdb_iterate_context *
userdb_mongodb_iterate_init(struct auth_request *auth_request,
			    userdb_iter_callback_t *callback, void *context)
{
#if 0
	struct userdb_module *_module = auth_request->userdb->userdb;
	struct mongodb_userdb_module *module =
		(struct mongodb_userdb_module *)_module;
	struct mongodb_userdb_iterate_context *ctx;
	string_t *query;

	query = t_str_new(512);
	var_expand(query, module->conn->set.iterate_query,
		   auth_request_get_var_expand_table(auth_request,
						     NULL));

	ctx = i_new(struct mongodb_userdb_iterate_context, 1);
	ctx->ctx.auth_request = auth_request;
	ctx->ctx.callback = callback;
	ctx->ctx.context = context;
	auth_request_ref(auth_request);
#endif

	return NULL;
}

static void userdb_mongodb_iterate_next(struct userdb_iterate_context *_ctx)
{
#if 0
	struct mongodb_userdb_iterate_context *ctx =
		(struct mongodb_userdb_iterate_context *)_ctx;
	struct userdb_module *_module = _ctx->auth_request->userdb->userdb;
	struct sql_userdb_module *module = (struct sql_userdb_module *)_module;
	const char *user;
	int ret;

	if (ctx->result == NULL) {
		ctx->call_iter = TRUE;
		return;
	}
#endif

}

static int userdb_mongodb_iterate_deinit(struct userdb_iterate_context *_ctx)
{
#if 0
	struct mongodb_userdb_iterate_context *ctx =
		(struct mongodb_userdb_iterate_context *)_ctx;
	int ret = _ctx->failed ? -1 : 0;

	auth_request_unref(&_ctx->auth_request);
	if (ctx->result == NULL) {
		/* mongodb query hasn't finished yet */
		ctx->freed = TRUE;
	} else {
		if (ctx->result != NULL)
			ctx->result = NULL;
			//sql_result_unref(ctx->result);
		i_free(ctx);
	}
	return ret;
#endif
	return -1;
}

static struct userdb_module *
userdb_mongodb_preinit(pool_t pool, const char *args)
{
	struct mongodb_userdb_module *module;
	struct mongodb_connection *conn;

	module = p_new(pool, struct mongodb_userdb_module, 1);
	module->conn = conn = db_mongodb_init(args, TRUE);

	module->module.cache_key =
		auth_cache_parse_key(pool, conn->set.password_query);
	return &module->module;
}

static void userdb_mongodb_init(struct userdb_module *_module)
{
	struct mongodb_userdb_module *module =
		(struct mongodb_userdb_module *)_module;

	db_mongodb_connect(module->conn);
}

static void userdb_mongodb_deinit(struct userdb_module *_module)
{
	struct mongodb_userdb_module *module =
		(struct mongodb_userdb_module *)_module;
}

struct userdb_module_interface userdb_mongodb = {
	"mongodb",

	userdb_mongodb_preinit,
	userdb_mongodb_init,
	userdb_mongodb_deinit,

	userdb_mongodb_lookup,

	userdb_mongodb_iterate_init,
	userdb_mongodb_iterate_next,
	userdb_mongodb_iterate_deinit
};

static void userdb_mongodb_iterate_next(struct userdb_iterate_context *_ctx);
static int userdb_mongodb_iterate_deinit(struct userdb_iterate_context *_ctx);

#else
struct userdb_module_interface userdb_mongodb = {
	.name = "mongodb"
};
#endif
