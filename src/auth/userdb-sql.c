/* Copyright (c) 2004-2011 Dovecot authors, see the included COPYING file */

#include "auth-common.h"
#include "userdb.h"

#ifdef USERDB_SQL

#include "str.h"
#include "strescape.h"
#include "var-expand.h"
#include "auth-cache.h"
#include "db-sql.h"

#include <stdlib.h>
#include <string.h>

struct sql_userdb_module {
	struct userdb_module module;

	struct sql_connection *conn;
};

struct userdb_sql_request {
	struct auth_request *auth_request;
	userdb_callback_t *callback;
};

struct sql_userdb_iterate_context {
	struct userdb_iterate_context ctx;
	struct sql_result *result;
	unsigned int freed:1;
	unsigned int call_iter:1;
};

static void userdb_sql_iterate_next(struct userdb_iterate_context *_ctx);
static int userdb_sql_iterate_deinit(struct userdb_iterate_context *_ctx);

static void
sql_query_get_result(struct sql_result *result,
		     struct auth_request *auth_request)
{
	const char *name, *value;
	unsigned int i, fields_count;

	auth_request_init_userdb_reply(auth_request);

	fields_count = sql_result_get_fields_count(result);
	for (i = 0; i < fields_count; i++) {
		name = sql_result_get_field_name(result, i);
		value = sql_result_get_field_value(result, i);

		if (*name != '\0' && value != NULL) {
			auth_request_set_userdb_field(auth_request,
						      name, value);
		}
	}
}

static void sql_query_callback(struct sql_result *sql_result,
			       struct userdb_sql_request *sql_request)
{
	struct auth_request *auth_request = sql_request->auth_request;
	struct userdb_module *_module = auth_request->userdb->userdb;
	struct sql_userdb_module *module =
		(struct sql_userdb_module *)_module;
	enum userdb_result result = USERDB_RESULT_INTERNAL_FAILURE;
	int ret;

	ret = sql_result_next_row(sql_result);
	if (ret < 0) {
		if (!module->conn->default_user_query) {
			auth_request_log_error(auth_request, "sql",
				"User query failed: %s",
				sql_result_get_error(sql_result));
		} else {
			auth_request_log_error(auth_request, "sql",
				"User query failed: %s "
				"(using built-in default user_query: %s)",
				sql_result_get_error(sql_result),
				module->conn->set.user_query);
		}
	} else if (ret == 0) {
		result = USERDB_RESULT_USER_UNKNOWN;
		auth_request_log_info(auth_request, "sql", "Unknown user");
	} else {
		sql_query_get_result(sql_result, auth_request);
		result = USERDB_RESULT_OK;
	}

	sql_request->callback(result, auth_request);
	auth_request_unref(&auth_request);
	i_free(sql_request);
}

static const char *
userdb_sql_escape(const char *str, const struct auth_request *auth_request)
{
	struct userdb_module *_module = auth_request->userdb->userdb;
	struct sql_userdb_module *module =
		(struct sql_userdb_module *)_module;

	return sql_escape_string(module->conn->db, str);
}

static void userdb_sql_lookup(struct auth_request *auth_request,
			      userdb_callback_t *callback)
{
	struct userdb_module *_module = auth_request->userdb->userdb;
	struct sql_userdb_module *module =
		(struct sql_userdb_module *)_module;
	struct userdb_sql_request *sql_request;
	string_t *query;

	query = t_str_new(512);
	var_expand(query, module->conn->set.user_query,
		   auth_request_get_var_expand_table(auth_request,
						     userdb_sql_escape));

	auth_request_ref(auth_request);
	sql_request = i_new(struct userdb_sql_request, 1);
	sql_request->callback = callback;
	sql_request->auth_request = auth_request;

	auth_request_log_debug(auth_request, "sql", "%s", str_c(query));

	sql_query(module->conn->db, str_c(query),
		  sql_query_callback, sql_request);
}

static void sql_iter_query_callback(struct sql_result *sql_result,
				    struct sql_userdb_iterate_context *ctx)
{
	ctx->result = sql_result;
	sql_result_ref(sql_result);

	if (ctx->freed)
		userdb_sql_iterate_deinit(&ctx->ctx);
	else if (ctx->call_iter)
		userdb_sql_iterate_next(&ctx->ctx);
}

static struct userdb_iterate_context *
userdb_sql_iterate_init(struct userdb_module *userdb,
			userdb_iter_callback_t *callback, void *context)
{
	static struct var_expand_table static_tab[] = {
		/* nothing for now, but e.g. %{hostname} can be used */
		{ '\0', NULL, NULL }
	};
	struct sql_userdb_module *module =
		(struct sql_userdb_module *)userdb;
	struct sql_userdb_iterate_context *ctx;
	string_t *query;

	query = t_str_new(512);
	var_expand(query, module->conn->set.iterate_query, static_tab);

	ctx = i_new(struct sql_userdb_iterate_context, 1);
	ctx->ctx.userdb = userdb;
	ctx->ctx.callback = callback;
	ctx->ctx.context = context;

	sql_query(module->conn->db, str_c(query),
		  sql_iter_query_callback, ctx);
	return &ctx->ctx;
}

static int userdb_sql_iterate_get_user(struct sql_userdb_iterate_context *ctx,
				       const char **user_r)
{
	const char *domain;
	int idx;

	/* try user first */
	idx = sql_result_find_field(ctx->result, "user");
	if (idx == 0) {
		*user_r = sql_result_get_field_value(ctx->result, idx);
		return 0;
	}

	/* username [+ domain]? */
	idx = sql_result_find_field(ctx->result, "username");
	if (idx < 0) {
		/* no user or username, fail */
		return -1;
	}

	*user_r = sql_result_get_field_value(ctx->result, idx);
	if (*user_r == NULL)
		return 0;

	domain = sql_result_find_field_value(ctx->result, "domain");
	if (domain != NULL)
		*user_r = t_strconcat(*user_r, "@", domain, NULL);
	return 0;
}

static void userdb_sql_iterate_next(struct userdb_iterate_context *_ctx)
{
	struct sql_userdb_iterate_context *ctx =
		(struct sql_userdb_iterate_context *)_ctx;
	struct userdb_module *_module = _ctx->userdb;
	struct sql_userdb_module *module = (struct sql_userdb_module *)_module;
	const char *user;
	int ret;

	if (ctx->result == NULL) {
		/* query not finished yet */
		ctx->call_iter = TRUE;
		return;
	}

	ret = sql_result_next_row(ctx->result);
	if (ret > 0) {
		if (userdb_sql_iterate_get_user(ctx, &user) < 0)
			i_error("sql: Iterate query didn't return 'user' field");
		else if (user == NULL)
			i_error("sql: Iterate query returned NULL user");
		else {
			_ctx->callback(user, _ctx->context);
			return;
		}
		_ctx->failed = TRUE;
	} else if (ret < 0) {
		if (!module->conn->default_iterate_query) {
			i_error("sql: Iterate query failed: %s",
				sql_result_get_error(ctx->result));
		} else {
			i_error("sql: Iterate query failed: %s "
				"(using built-in default iterate_query: %s)",
				sql_result_get_error(ctx->result),
				module->conn->set.iterate_query);
		}
		_ctx->failed = TRUE;
	}
	_ctx->callback(NULL, _ctx->context);
}

static int userdb_sql_iterate_deinit(struct userdb_iterate_context *_ctx)
{
	struct sql_userdb_iterate_context *ctx =
		(struct sql_userdb_iterate_context *)_ctx;
	int ret = _ctx->failed ? -1 : 0;

	if (ctx->result == NULL) {
		/* sql query hasn't finished yet */
		ctx->freed = TRUE;
	} else {
		if (ctx->result != NULL)
			sql_result_unref(ctx->result);
		i_free(ctx);
	}
	return ret;
}

static struct userdb_module *
userdb_sql_preinit(pool_t pool, const char *args)
{
	struct sql_userdb_module *module;

	module = p_new(pool, struct sql_userdb_module, 1);
	module->conn = db_sql_init(args);

	module->module.cache_key =
		auth_cache_parse_key(pool, module->conn->set.user_query);
	return &module->module;
}

static void userdb_sql_init(struct userdb_module *_module)
{
	struct sql_userdb_module *module =
		(struct sql_userdb_module *)_module;
	enum sql_db_flags flags;

	flags = sql_get_flags(module->conn->db);
	_module->blocking = (flags & SQL_DB_FLAG_BLOCKING) != 0;

	if (!_module->blocking || worker)
		sql_connect(module->conn->db);
}

static void userdb_sql_deinit(struct userdb_module *_module)
{
	struct sql_userdb_module *module =
		(struct sql_userdb_module *)_module;

	db_sql_unref(&module->conn);
}

struct userdb_module_interface userdb_sql = {
	"sql",

	userdb_sql_preinit,
	userdb_sql_init,
	userdb_sql_deinit,

	userdb_sql_lookup,

	userdb_sql_iterate_init,
	userdb_sql_iterate_next,
	userdb_sql_iterate_deinit
};
#else
struct userdb_module_interface userdb_sql = {
	.name = "sql"
};
#endif
