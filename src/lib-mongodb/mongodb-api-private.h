#ifndef MONGODB_API_PRIVATE_H
#define MONGODB_API_PRIVATE_H

#include "mongodb-api.h"
#include <mongo.h>
#include <hash.h>

#define MONGODB_DEBUG

struct mongodb_conn {
	pool_t pool;
	struct mongodb_uri uri;
	mongo *conn;
};

struct mongodb_query {
	pool_t pool;
	mongodb_conn_t conn;
	mongo_cursor *cursor;

	char *error;

	bson *query;
	bson *other;

	HASH_TABLE(const char *, const char *) fieldmap;
	HASH_TABLE(const char *, string_t *) defaults;
};

struct mongodb_result {
	mongodb_query_t query;

	HASH_TABLE(const char *, string_t *) fields;
};

struct mongodb_driver_vfuncs {
	/* connection api */
	mongodb_conn_t (*conn_init)(const char *connection_string);
	void (*conn_deinit)(mongodb_conn_t *_conn);
	char *(*get_error)(mongodb_conn_t conn);

	/* query api */
	mongodb_query_t (*query_init)(mongodb_conn_t conn);
	void (*query_deinit)(mongodb_query_t *_query);

	int (*query_parse_defaults)(mongodb_query_t query, const char *json);
	int (*query_parse_query)(mongodb_query_t query, const char *json);
	int (*query_parse_fields)(mongodb_query_t query, const char *fields);
	int (*query_parse_update)(mongodb_query_t query, const char *json);
	void (*query_debug)(mongodb_query_t query);

	int (*query_find_one)(mongodb_query_t query, const char *collection, mongodb_result_t *result_r);
	int (*query_find)(mongodb_query_t query, const char *collection);
	int (*query_find_next)(mongodb_query_t query, mongodb_result_t *result);
	int (*query_update)(mongodb_query_t query, const char *collection, bool multi);

	/* result api */
	int (*result_var_expand)(mongodb_result_t result, struct var_expand_table *_table);
	void (*result_debug)(mongodb_result_t result);
	void (*result_field)(mongodb_result_t result, const char *key, const char **value_r);
	struct mongodb_result_iterate_context *(*result_iterate_init)(mongodb_result_t result);
	int (*result_iterate)(struct mongodb_result_iterate_context *ctx, const char **key_r, string_t **value_r);
	void (*result_iterate_deinit)(struct mongodb_result_iterate_context **_ctx);
};

// static char[][] mongodb_error_text = {
// 	"connection successful",
// 	"no socket",
// 	"unable to connect",
// 	"unknown address",
// 	"not master",
// 	"invalid replica set name",
// 	"no primary in replica set",
// 	"io error",
// 	"socket error",
// 	"read size error",
// 	"command failed",
// 	"write error",
// 	"invalid namespace",
// 	"bson is invalid",
// 	"bson not finished",
// 	"bson object exceeds max bson size",
// 	"invalid write concern supplied",
// 	NULL
// }

#ifdef MONGODB_DEBUG
#define mongodb_debug(format, ...) i_debug("mongodb: " format, ## __VA_ARGS__)
static inline void bson_debug(mongodb_conn_t conn, bson *b, int depth) {
    bson_iterator *i;
    const char *key;
    bool first = TRUE;
    bson_timestamp_t ts;
    char oidhex[25];
    string_t *out;
    bson scope;

    i = p_new(conn->pool, bson_iterator, 1);
    out = str_new(conn->pool, 1024);
    bson_iterator_from_buffer(i, b->data);

    str_printfa(out, "{");
    while (bson_iterator_next(i)) {
	bson_type t = bson_iterator_type(i);
	if (t == 0)
	    break;
	if (!first)
		str_printfa(out, ", ");
	key = bson_iterator_key(i);

	str_printfa(out, "\"%s\": ", key);
	switch (t) {
	case BSON_DOUBLE:
	    str_printfa(out, "%f", bson_iterator_double(i));
	    break;
	case BSON_STRING:
	    str_printfa(out, "\"%s\"", bson_iterator_string(i));
	    break;
	case BSON_SYMBOL:
	    str_printfa(out, "SYMBOL: %s", bson_iterator_string(i));
	    break;
	case BSON_OID:
	    bson_oid_to_string(bson_iterator_oid(i), oidhex);
	    str_printfa(out, "%s", oidhex);
	    break;
	case BSON_BOOL:
	    str_printfa(out, "%s", bson_iterator_bool(i) ? "true" : "false");
	    break;
	case BSON_DATE:
	    str_printfa(out, "%ld", (long int)bson_iterator_date(i));
	    break;
	case BSON_BINDATA:
	    str_printfa(out, "BSON_BINDATA");
	    break;
	case BSON_UNDEFINED:
	    str_printfa(out, "BSON_UNDEFINED");
	    break;
	case BSON_NULL:
	    str_printfa(out, "BSON_NULL");
	    break;
	case BSON_REGEX:
	    str_printfa(out, "BSON_REGEX: %s", bson_iterator_regex(i));
	    break;
	case BSON_CODE:
	    str_printfa(out, "BSON_CODE: %s", bson_iterator_code(i));
	    break;
	case BSON_CODEWSCOPE:
	    str_printfa(out, "BSON_CODE_W_SCOPE: %s", bson_iterator_code(i));
	    bson_iterator_code_scope_init(i, &scope, 0);
	    str_printfa(out, "\n\t SCOPE: ");
	    bson_print(&scope);
	    bson_destroy(&scope);
	    break;
	case BSON_INT:
	    str_printfa(out, "%d", bson_iterator_int(i));
	    break;
	case BSON_LONG:
	    str_printfa(out, "%lu", (uint64_t)bson_iterator_long(i));
	    break;
	case BSON_TIMESTAMP:
	    ts = bson_iterator_timestamp(i);
	    str_printfa(out, "i: %d, t: %d", ts.i, ts.t);
	    break;
	case BSON_OBJECT:
	case BSON_ARRAY:
	    str_printfa(out, "\n");
	    bson_print_raw(bson_iterator_value(i) , depth + 1);
	    break;
	default:
	    bson_errprintf("can't print type : %d\n", t);
	}

	if (first)
		first = FALSE;
    }
    str_printfa(out, "}");

    mongodb_debug("bson=%s", str_c(out));
    str_free(&out);
}
#else
#define mongodb_debug(format, ...)
#define bson_debug(conn, bson, depth)
#endif

#endif

// vim: noexpandtab shiftwidth=8 tabstop=8
