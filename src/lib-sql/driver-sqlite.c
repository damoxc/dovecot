/* Copyright (c) 2006-2013 Dovecot authors, see the included COPYING file */

#include "lib.h"
#include "array.h"
#include "str.h"
#include "sql-api-private.h"

#ifdef BUILD_SQLITE
#include <sqlite3.h>

/* retry time if db is busy (in ms) */
static const int sqlite_busy_timeout = 1000;

struct sqlite_db {
	struct sql_db api;

	pool_t pool;
	const char *dbfile;
	sqlite3 *sqlite;
	unsigned int connected:1;
	int rc;
};

struct sqlite_result {
	struct sql_result api;
	sqlite3_stmt *stmt;
	unsigned int cols;
	const char **row;
};

struct sqlite_transaction_context {
	struct sql_transaction_context ctx;
	unsigned int failed:1;
};

extern const struct sql_db driver_sqlite_db;
extern const struct sql_result driver_sqlite_result;
extern const struct sql_result driver_sqlite_error_result;

static int driver_sqlite_connect(struct sql_db *_db)
{
 	struct sqlite_db *db = (struct sqlite_db *)_db;
  
	if (db->connected)
		return 1;

	db->rc = sqlite3_open(db->dbfile, &db->sqlite);
	
	if (db->rc == SQLITE_OK) {
		db->connected = TRUE;
		sqlite3_busy_timeout(db->sqlite, sqlite_busy_timeout);
		return 1;
	} else {
		i_error("sqlite: open(%s) failed: %s", db->dbfile,
			sqlite3_errmsg(db->sqlite));
		sqlite3_close(db->sqlite);
		db->sqlite = NULL;
		return -1;
	}
}

static void driver_sqlite_disconnect(struct sql_db *_db)
{
 	struct sqlite_db *db = (struct sqlite_db *)_db;

	sqlite3_close(db->sqlite);
	db->sqlite = NULL;
}

static struct sql_db *driver_sqlite_init_v(const char *connect_string)
{
	struct sqlite_db *db;
	pool_t pool;

	i_assert(connect_string != NULL);

	pool = pool_alloconly_create("sqlite driver", 512);
	db = p_new(pool, struct sqlite_db, 1);
	db->pool = pool;
	db->api = driver_sqlite_db;
	db->dbfile = p_strdup(db->pool, connect_string);
	db->connected = FALSE;

	return &db->api;
}

static void driver_sqlite_deinit_v(struct sql_db *_db)
{
	struct sqlite_db *db = (struct sqlite_db *)_db;

	_db->no_reconnect = TRUE;
	sql_db_set_state(&db->api, SQL_DB_STATE_DISCONNECTED);

	sqlite3_close(db->sqlite);
	array_free(&_db->module_contexts);
	pool_unref(&db->pool);
}

static const char *
driver_sqlite_escape_string(struct sql_db *_db ATTR_UNUSED,
			    const char *string)
{
	const char *p;
	char *dest, *destbegin;

	/* find the first ' */
	for (p = string; *p != '\''; p++) {
		if (*p == '\0')
			return t_strdup_noconst(string);
	}

	/* @UNSAFE: escape ' with '' */
	dest = destbegin = t_buffer_get((p - string) + strlen(string) * 2 + 1);

	memcpy(dest, string, p - string);
	dest += p - string;

	for (; *p != '\0'; p++) {
		*dest++ = *p;
		if (*p == '\'')
			*dest++ = *p;
	}
	*dest++ = '\0';
	t_buffer_alloc(dest - destbegin);

	return destbegin;
}

static void driver_sqlite_exec(struct sql_db *_db, const char *query)
{
	struct sqlite_db *db = (struct sqlite_db *)_db;

	if (driver_sqlite_connect(_db) < 0)
		return;

	db->rc = sqlite3_exec(db->sqlite, query, NULL, 0, NULL);
	if (db->rc != SQLITE_OK) {
		i_error("sqlite: exec(%s) failed: %s (%d)",
			query, sqlite3_errmsg(db->sqlite), db->rc);
	}
}

static void driver_sqlite_query(struct sql_db *db, const char *query,
				sql_query_callback_t *callback, void *context)
{
	struct sql_result *result;

	result = sql_query_s(db, query);
	result->callback = TRUE;
	callback(result, context);
	result->callback = FALSE;
	sql_result_unref(result);
}

static struct sql_result *
driver_sqlite_query_s(struct sql_db *_db, const char *query)
{
	struct sqlite_db *db = (struct sqlite_db *)_db;
	struct sqlite_result *result;
	int rc;

	result = i_new(struct sqlite_result, 1);

	if (driver_sqlite_connect(_db) < 0) {
		result->api = driver_sqlite_error_result;
		result->stmt = NULL;
		result->cols = 0;
	} else {
		rc = sqlite3_prepare(db->sqlite, query, -1, &result->stmt, NULL);
		if (rc == SQLITE_OK) {
			result->api = driver_sqlite_result;
			result->cols = sqlite3_column_count(result->stmt);
			result->row = i_new(const char *, result->cols);
		} else {
			result->api = driver_sqlite_error_result;
			result->stmt = NULL;
			result->cols = 0;
		}
	}
	result->api.db = _db;
	result->api.refcount = 1;

	return &result->api;
}

static void driver_sqlite_result_free(struct sql_result *_result)
{
	struct sqlite_result *result = (struct sqlite_result *)_result;
	struct sqlite_db *db = (struct sqlite_db *)	result->api.db;
	int rc;

	if (_result->callback)
		return;

	if (result->stmt != NULL) {
		if ((rc = sqlite3_finalize(result->stmt)) != SQLITE_OK) {
			i_warning("sqlite: finalize failed: %s (%d)",
				  sqlite3_errmsg(db->sqlite), rc);
		}
		i_free(result->row);
	}
	i_free(result);
}

static int driver_sqlite_result_next_row(struct sql_result *_result)
{
	struct sqlite_result *result = (struct sqlite_result *)_result;

	switch (sqlite3_step(result->stmt)) {
	case SQLITE_ROW:
		return 1;
	case SQLITE_DONE:
		return 0;
	default:
		return -1;
	}
}

static unsigned int
driver_sqlite_result_get_fields_count(struct sql_result *_result)
{
	struct sqlite_result *result = (struct sqlite_result *)_result;

	return result->cols;
}

static const char *
driver_sqlite_result_get_field_name(struct sql_result *_result,
				    unsigned int idx)
{
	struct sqlite_result *result = (struct sqlite_result *)_result;

	return sqlite3_column_name(result->stmt, idx);
}

static int driver_sqlite_result_find_field(struct sql_result *_result,
					   const char *field_name)
{
	struct sqlite_result *result = (struct sqlite_result *)_result;
	unsigned int i;

	for (i = 0; i < result->cols; ++i) {
		const char *col = sqlite3_column_name(result->stmt, i);

		if (strcmp(col, field_name) == 0)
			return i;
	}
	
	return -1;
}

static const char *
driver_sqlite_result_get_field_value(struct sql_result *_result,
				     unsigned int idx)
{
	struct sqlite_result *result = (struct sqlite_result *)_result;

	return (const char*)sqlite3_column_text(result->stmt, idx);
}

static const unsigned char *
driver_sqlite_result_get_field_value_binary(struct sql_result *_result,
					    unsigned int idx, size_t *size_r)
{
	struct sqlite_result *result = (struct sqlite_result *)_result;

	*size_r = sqlite3_column_bytes(result->stmt, idx);
	return sqlite3_column_blob(result->stmt, idx);
}

static const char *
driver_sqlite_result_find_field_value(struct sql_result *result,
				     const char *field_name)
{
	int idx;

	idx = driver_sqlite_result_find_field(result, field_name);
	if (idx < 0)
		return NULL;
	return driver_sqlite_result_get_field_value(result, idx);
}

static const char *const *
driver_sqlite_result_get_values(struct sql_result *_result)
{
	struct sqlite_result *result = (struct sqlite_result *)_result;
	unsigned int i;

	for (i = 0; i < result->cols; ++i) {
		result->row[i] =
			driver_sqlite_result_get_field_value(_result, i);
	}

	return (const char *const *)result->row;
}

static const char *driver_sqlite_result_get_error(struct sql_result *_result)
{
	struct sqlite_result *result = (struct sqlite_result *)_result;
	struct sqlite_db *db = (struct sqlite_db *)result->api.db;

	return sqlite3_errmsg(db->sqlite);
}

static struct sql_transaction_context *
driver_sqlite_transaction_begin(struct sql_db *_db)
{
	struct sqlite_transaction_context *ctx;
	struct sqlite_db *db = (struct sqlite_db *)_db;

	ctx = i_new(struct sqlite_transaction_context, 1);
	ctx->ctx.db = _db;

	sql_exec(_db, "BEGIN TRANSACTION");
	if (db->rc != SQLITE_OK)
		ctx->failed = TRUE;

	return &ctx->ctx;
}

static void
driver_sqlite_transaction_rollback(struct sql_transaction_context *_ctx)
{
	struct sqlite_transaction_context *ctx =
		(struct sqlite_transaction_context *)_ctx;

	sql_exec(_ctx->db, "ROLLBACK");
	i_free(ctx);
}

static void
driver_sqlite_transaction_commit(struct sql_transaction_context *_ctx,
				 sql_commit_callback_t *callback, void *context)
{
	struct sqlite_transaction_context *ctx =
		(struct sqlite_transaction_context *)_ctx;
	struct sqlite_db *db = (struct sqlite_db *)ctx->ctx.db;
	const char *errmsg;

	if (!ctx->failed) {
		sql_exec(_ctx->db, "COMMIT");
		if (db->rc != SQLITE_OK)
			ctx->failed = TRUE;
	}

	if (ctx->failed) {
		errmsg = sqlite3_errmsg(db->sqlite);
		callback(errmsg, context);
                /* also does i_free(ctx) */
		driver_sqlite_transaction_rollback(_ctx);
	} else {
		callback(NULL, context);
		i_free(ctx);
	}
}

static int
driver_sqlite_transaction_commit_s(struct sql_transaction_context *_ctx,
				   const char **error_r)
{
	struct sqlite_transaction_context *ctx =
		(struct sqlite_transaction_context *)_ctx;
	struct sqlite_db *db = (struct sqlite_db *) ctx->ctx.db;

	if (ctx->failed) {
                /* also does i_free(ctx) */
		driver_sqlite_transaction_rollback(_ctx);
		return -1;
	}

	sql_exec(_ctx->db, "COMMIT");
	*error_r = sqlite3_errmsg(db->sqlite);
	i_free(ctx);
	return 0;
}

static void
driver_sqlite_update(struct sql_transaction_context *_ctx, const char *query,
		     unsigned int *affected_rows)
{
	struct sqlite_transaction_context *ctx =
		(struct sqlite_transaction_context *)_ctx;
	struct sqlite_db *db = (struct sqlite_db *)ctx->ctx.db;

	if (ctx->failed)
		return;

	sql_exec(_ctx->db, query);
	if (db->rc != SQLITE_OK)
		ctx->failed = TRUE;
	else if (affected_rows != NULL)
		*affected_rows = sqlite3_changes(db->sqlite);
}

const struct sql_db driver_sqlite_db = {
	.name = "sqlite",
	.flags = SQL_DB_FLAG_BLOCKING,

	.v = {
		driver_sqlite_init_v,
		driver_sqlite_deinit_v,
		driver_sqlite_connect,
		driver_sqlite_disconnect,
		driver_sqlite_escape_string,
		driver_sqlite_exec,
		driver_sqlite_query,
		driver_sqlite_query_s,

		driver_sqlite_transaction_begin,
		driver_sqlite_transaction_commit,
		driver_sqlite_transaction_commit_s,
		driver_sqlite_transaction_rollback,
		driver_sqlite_update
	}
};

const struct sql_result driver_sqlite_result = {
	.v = {
		driver_sqlite_result_free,
		driver_sqlite_result_next_row,
		driver_sqlite_result_get_fields_count,
		driver_sqlite_result_get_field_name,
		driver_sqlite_result_find_field,
		driver_sqlite_result_get_field_value,
		driver_sqlite_result_get_field_value_binary,
		driver_sqlite_result_find_field_value,
		driver_sqlite_result_get_values,
		driver_sqlite_result_get_error
	}
};

static int
driver_sqlite_result_error_next_row(struct sql_result *result ATTR_UNUSED)
{
	return -1;
}

const struct sql_result driver_sqlite_error_result = {
	.v = {
		driver_sqlite_result_free,
		driver_sqlite_result_error_next_row,
		NULL, NULL, NULL, NULL, NULL, NULL, NULL,
		driver_sqlite_result_get_error
	}
};

const char *driver_sqlite_version = DOVECOT_ABI_VERSION;

void driver_sqlite_init(void);
void driver_sqlite_deinit(void);

void driver_sqlite_init(void)
{
	sql_driver_register(&driver_sqlite_db);
}

void driver_sqlite_deinit(void)
{
	sql_driver_unregister(&driver_sqlite_db);
}

#endif
