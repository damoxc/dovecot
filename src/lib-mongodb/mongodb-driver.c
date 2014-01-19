/* Copyright (c) 2004-2013 Dovecot authors, see the included COPYING file */

#include "lib.h"
#include "str.h"
#include "hash.h"
#include "istream.h"
#include "json-parser.h"

#include <mongo.h>

#include "istream.h"
#include "str.h"
#include "json-parser.h"

#include "mongodb-api-private.h"

#define MAX_KEY_LENGTH 128
#define MAX_FIELD_LENGTH 1024

static void mongodb_driver_result_debug(mongodb_result_t result);

/* JSON parser */
struct mongodb_json_iter {
	struct json_parser *parser;
	string_t *key;
	enum json_type type;
	const char *error;
};

static struct mongodb_json_iter *
mongodb_json_iter_init(const char *value)
{
	struct mongodb_json_iter *iter;
	struct istream *input;

	iter = i_new(struct mongodb_json_iter, 1);
	iter->key = str_new(default_pool, MAX_KEY_LENGTH);
	input = i_stream_create_from_data(value, strlen(value));
	iter->parser = json_parser_init(input);
	i_stream_unref(&input);
	return iter;
}

static bool mongodb_json_iter_next(struct mongodb_json_iter *iter,
				      const char **key_r, const char **value_r)
{
	enum json_type type;
	const char *value;

	if (json_parse_next(iter->parser, &type, &value) < 0) {
		return FALSE;
	}
	if (type != JSON_TYPE_OBJECT_KEY) {
		iter->error = "Object expected";
		return FALSE;
	}
	if (*value == '\0') {
		iter->error = "Empty object key";
		return FALSE;
	}
	str_truncate(iter->key, 0);
	str_append(iter->key, value);

	if (json_parse_next(iter->parser, &type, &value) < 0) {
		iter->error = "Missing value";
		return FALSE;
	}
	if (type == JSON_TYPE_OBJECT) {
		iter->error = "Nested objects not supported";
		return FALSE;
	}
	iter->type = type;
	*key_r = str_c(iter->key);
	*value_r = value;
	return TRUE;
}

static int mongodb_json_iter_deinit(struct mongodb_json_iter **_iter,
				       const char **error_r)
{
	struct mongodb_json_iter *iter = *_iter;

	*_iter = NULL;
	*error_r = iter->error;
	if (json_parser_deinit(&iter->parser, &iter->error) < 0 &&
	    *error_r == NULL)
		*error_r = iter->error;
	str_free(&iter->key);
	i_free(iter);
	return *error_r != NULL ? -1 : 0;
} /* end of JSON parser */

/* connection api */
static mongodb_conn_t mongodb_driver_conn_init(const char *connection_string)
{
	struct mongodb_conn *conn;
	pool_t pool;

	/* TODO: add error reporting */
	if (strncmp(connection_string, "mongodb://", 10) != 0)
		return NULL;

	/* TODO: add proper mongo uri parsing, maybe add to the right place (mongoc)? */
	connection_string += 10;

	pool = pool_alloconly_create("mongodb_connection", 1024);
	conn = p_new(pool, struct mongodb_conn, 1);
	conn->pool = pool;
	conn->conn = p_new(pool, mongo, 1);

	/* FIXME: replace with proper uri parsing */
	conn->uri.host.host = "localhost";
	conn->uri.host.port = 27017;
	conn->uri.database = "mail";

	mongo_client(conn->conn, "localhost", 27017);

	return (struct mongodb_conn *)conn;
}

static void mongodb_driver_conn_deinit(mongodb_conn_t *_conn) {
	struct mongodb_conn *conn = *_conn;
	pool_t pool = conn->pool;

	*_conn = NULL;

	mongo_destroy(conn->conn);

	p_free(pool, conn);
	pool_unref(&pool);
}

static char *mongodb_driver_get_error(mongodb_conn_t conn)
{
	switch (conn->conn->err) {
		case MONGO_CONN_SUCCESS:
			return "connection successful";
		case MONGO_IO_ERROR:
			return "io error";
		case MONGO_SOCKET_ERROR:
			return "socket error";
		case MONGO_READ_SIZE_ERROR:
			return "unexpected response length";
		case MONGO_COMMAND_FAILED:
			return "command failed";
		case MONGO_WRITE_ERROR:
			return "write failed";
		case MONGO_NS_INVALID:
			return "invalid namespace";
		case MONGO_BSON_INVALID:
			return "bson is invalid";
		case MONGO_BSON_NOT_FINISHED:
			return "bson not finished";
		case MONGO_BSON_TOO_LARGE:
			return "bson object exceeds max bson size";
		case MONGO_WRITE_CONCERN_INVALID:
			return "invalid write concern supplied";
		default:
			return "unknown error";
	}
}
/* end of connection api */

/* start of query api */
static mongodb_query_t mongodb_driver_query_init(mongodb_conn_t conn)
{
	mongodb_query_t query;
	pool_t pool = conn->pool;

	query = p_new(pool, struct mongodb_query, 1);
	query->pool = pool;
	query->conn = conn;
	query->cursor = NULL;
	query->other = NULL;

	hash_table_create(&query->fieldmap, conn->pool, 0, str_hash, strcmp);
	hash_table_create(&query->defaults, conn->pool, 0, str_hash, strcmp);
	return query;
}

static void mongodb_driver_query_deinit(mongodb_query_t *_query) {
	struct mongodb_query *query = *_query;

	*_query = NULL;

	bson_destroy(query->query);
	if (query->cursor != NULL) {
		mongo_cursor_destroy(query->cursor);
	}

	p_free(query->pool, query->query);

	bson_destroy(query->other);
	p_free(query->pool, query->other);

	hash_table_destroy(&query->defaults);
	hash_table_destroy(&query->fieldmap);

	p_free(query->pool, query);
}

static int mongodb_driver_query_parse_defaults(struct mongodb_query *query, const char *json)
{
	struct mongodb_json_iter *iter;
	const char *key, *value, *error;
	string_t *value_dup;

	iter = mongodb_json_iter_init(json);
	while (mongodb_json_iter_next(iter, &key, &value)) {
		if (value != NULL) {
			value_dup = str_new(query->pool, MAX_FIELD_LENGTH);
			str_append(value_dup, value);
			hash_table_insert(query->defaults,
					  (const char *)p_strdup(query->pool, key),
					  value_dup);
		}
	}

	if (mongodb_json_iter_deinit(&iter, &error) < 0) {
		return -1;
	}

	return 0;
}

static int mongodb_json_to_bson(bson *b, const char *json)
{
	struct mongodb_json_iter *iter;
	const char *key, *value, *error;
	int ret = 0;

	bson_init(b);

	iter = mongodb_json_iter_init(json);
	while (mongodb_json_iter_next(iter, &key, &value)) {
		mongodb_debug("type=%d, key=%s, value=%s", iter->type, key, value);
		switch (iter->type) {
			case JSON_TYPE_STRING:
				ret = bson_append_string(b, key, value);
				break;
			default:
				break;
		}

		if (ret < 0)
			break;
	}

	if (mongodb_json_iter_deinit(&iter, &error) < 0) {
		ret = -1;
	}

	if (ret == 0 && bson_finish(b) != BSON_OK) {
		ret = -2;
	}

	return ret;
}

static int mongodb_driver_query_parse_query(struct mongodb_query *query, const char *json)
{
	int ret;

	query->query = p_new(query->pool, bson, 1);

	ret = mongodb_json_to_bson(query->query, json);
	if (ret < 0) {
		ret = -1;
		query->error = (ret == -1) ? "failed to parse query" : "failed create query bson";
	}

	return ret;
}

static int mongodb_driver_query_parse_update(struct mongodb_query *query, const char *json)
{
	int ret;

	query->other = p_new(query->pool, bson, 1);

	ret = mongodb_json_to_bson(query->other, json);
	if (ret < 0) {
		ret = -1;
		query->error = (ret == -1) ? "failed to parse update" : "failed create update bson";
	}

	return ret;
}

static void str_trim_whitespace(char *string)
{
	char *s = string, *last_nwsc = NULL;

	while (*string != 0) {
		if (*string != ' ' && *string != '\t') {
			*s++ = *string;
			last_nwsc = s;
		} else if (last_nwsc != NULL) {
			*s++ = *string;
		}
		string++;
	}
	*last_nwsc = 0;
}

static int mongodb_driver_query_parse_fields(struct mongodb_query *query, const char *_fields)
{
	char *key, *value;
	char *field, *s1, *map, *s2, *dot, *s3, *fields = p_strdup(query->pool, _fields);

	string_t *key_parts;
	int ret = 0;

	/* initialize the fields bson */
	query->other = p_new(query->pool, bson, 1);
	bson_init(query->other);

	mongodb_debug("parsing fields '%s'", fields);
	key_parts = str_new(query->pool, 128);
	field = strtok_r(fields, ",", &s1);
	while (field != NULL) {
		str_truncate(key_parts, 0);
		map = strtok_r(field, ":", &s2);
		key = p_strdup(query->pool, map);
		str_trim_whitespace(key);

		/* handle dotted fields */
		dot = strtok_r(map, ".", &s3);
		str_append(key_parts, dot);
		hash_table_insert(query->fieldmap, (const char *)str_c(key_parts), (const char *)"\0");
		dot = strtok_r(NULL, ".", &s3);
		while (dot != NULL) {
			str_printfa(key_parts, ".%s", dot);
			dot = strtok_r(NULL, ".", &s3);
		}

		map = strtok_r(NULL, ":", &s2);
		if (map != NULL) {
			value = p_strdup(query->pool, map);
			str_trim_whitespace(value);
		} else {
			value = NULL;
		}

		mongodb_debug("fieldmap: '%s' = '%s'", key, (value == NULL) ? key : value);
		bson_append_int(query->other, key, 1);
		hash_table_insert(query->fieldmap, (const char *)key,
				  (const char *)((value == NULL) ? key : value));
		field = strtok_r(NULL, ",", &s1);
	}

	if (ret == 0) {
		ret = bson_finish(query->other);
	} else {
		bson_init_zero(query->other);
	}

	return ret;
}

static inline const char *bson_to_string(bson_iterator *iter)
{
	bson_type type = bson_iterator_type(iter);
	switch (type) {
		case BSON_BOOL:
			return bson_iterator_bool(iter) ? "y" : "n";
		case BSON_DOUBLE:
			return t_strdup_printf("%f", bson_iterator_double(iter));
		case BSON_STRING:
			return bson_iterator_string(iter);
		case BSON_INT:
			return t_strdup_printf("%d", bson_iterator_int(iter));
		case BSON_LONG:
			return t_strdup_printf("%ld", bson_iterator_long(iter));
		default:
			return NULL;
	}
}

static inline int
mongodb_driver_query_result_nested(mongodb_query_t query, bson_iterator *iter,
				   const char *parent_key, mongodb_result_t result)
{
	bson_iterator subiter[1];
	const char *key, *value, *doc_key;
	string_t *value_dup;
	int ret = 0;

	while (bson_iterator_next(iter) != BSON_EOO) {
		if (parent_key == NULL) {
			doc_key = bson_iterator_key(iter);
		} else {
			doc_key = t_strdup_printf("%s.%s", parent_key, bson_iterator_key(iter));
		}
		key = (const char *)hash_table_lookup(query->fieldmap, doc_key);
		mongodb_debug("result doc key=%s; map key=%s", doc_key, key);
		if (key == NULL)
			continue; /* most likely the _id field, but still ignore any unknown fields */

		if (*key == '\0') {
			mongodb_debug("result is nested");
			bson_iterator_subiterator(iter, subiter);
			mongodb_driver_query_result_nested(query, subiter, doc_key, result);
		} else {
			value = bson_to_string(iter);
			if (value == NULL)
				continue;

			value_dup = str_new(query->pool, MAX_FIELD_LENGTH);
			str_append_n(value_dup, value, MAX_FIELD_LENGTH);
			hash_table_update(result->fields, key, value_dup);
		}
	}

	return ret;
}

static int mongodb_driver_query_result(mongodb_query_t query, bson *_result, mongodb_result_t *result_r)
{
	mongodb_result_t result;
	bson_iterator iter[1];
	int ret = 0;

	/* create the result struct */
	result = p_new(query->pool, struct mongodb_result, 1);
	result->query = query;
	hash_table_create(&(result->fields), query->pool, 0, str_hash, strcmp);
	hash_table_copy(result->fields, query->defaults);
	*result_r = result;

	/* grab the parts of the BSON result we are interested in */
	bson_iterator_init(iter, _result);
	ret = mongodb_driver_query_result_nested(query, iter, NULL, result);
	mongodb_driver_result_debug(result);

	return ret;
}

static int mongodb_driver_query_find_one(mongodb_query_t query, const char *collection, mongodb_result_t *result_r)
{
	struct mongodb_conn *conn = query->conn;

	const char *ns;
	bson *_result;
	int ret = -1;

	/* execute the query on the mongo database */
	ns = t_strdup_printf("%s.%s", conn->uri.database,
			     collection);
	mongodb_debug("ns=%s", ns);

	_result = p_new(conn->pool, bson, 1);

	bson_debug(conn, query->query, 0);
	bson_debug(conn, query->other, 0);
	ret = mongo_find_one(conn->conn, ns, query->query, query->other, _result);

	/* if we have a matching document, convert the fields as-per the field map */
	if (ret != MONGO_OK) {
		mongodb_debug("converting result");
		if (conn->conn->err == MONGO_CONN_SUCCESS) {
			ret = MONGODB_QUERY_ERROR;
		} else {
			ret = MONGODB_QUERY_NO_RESULT;
		}
	} else {
		ret = mongodb_driver_query_result(query, _result, result_r);
	}

	return ret;
}

static int mongodb_driver_query_find(mongodb_query_t query, const char *collection)
{
	struct mongodb_conn *conn = query->conn;
	const char *ns;

	ns = t_strdup_printf("%s.%s", conn->uri.database,
			     collection);
	mongodb_debug("ns=%s", ns);
	bson_debug(conn, query->query, 0);
	bson_debug(conn, query->other, 0);

	/* execute the query on the mongo database */
	query->cursor = mongo_find(conn->conn, ns, query->query, query->other, 0, 0, 0);

	if (query->cursor == NULL) {
		return MONGODB_QUERY_ERROR;
	}

	return MONGO_OK;
}

static int mongodb_driver_query_find_next(mongodb_query_t query, mongodb_result_t *result_r)
{

	int ret;
	bson *_result;

	_result = p_new(query->pool, bson, 1);
	ret = mongo_cursor_next(query->cursor);
	if (ret == MONGO_OK) {
		ret = bson_copy(_result, &query->cursor->current);
		if (ret == MONGO_OK) {
			ret = mongodb_driver_query_result(query, _result, result_r);
		}
	} else if (ret == MONGO_ERROR && query->cursor->err == MONGO_CURSOR_EXHAUSTED) {
		ret = MONGODB_QUERY_NO_RESULT;
	} else {
		ret = MONGODB_QUERY_ERROR;
	}

	return ret;
}

static int mongodb_driver_query_update(mongodb_query_t query, const char *collection, bool multi)
{
	struct mongodb_conn *conn = query->conn;
	const char *ns;
	int flags = 0, ret;

	ns = t_strdup_printf("%s.%s", conn->uri.database,
			     collection);
	mongodb_debug("ns=%s", ns);
	bson_debug(conn, query->query, 0);
	bson_debug(conn, query->other, 0);

	/* execute the query on the mongo database */
	if (mongo_update(conn->conn, ns, query->query, query->other, flags, 0) != MONGO_OK)
		ret = MONGODB_QUERY_ERROR;
	else
		ret = MONGODB_QUERY_OK;

	if (query->cursor == NULL) {
		return MONGODB_QUERY_ERROR;
	}

	return MONGO_OK;
}

static void mongodb_driver_query_debug(mongodb_query_t query)
{
	struct hash_iterate_context *iter;
	const char *key;
	string_t *value;

	mongodb_debug("printing query structure");

// FIXME: replace with query dumping
#if 0
	iter = hash_table_iterate_init(query->result);
	while (hash_table_iterate(iter, query->result, &key, &value)) {
		mongodb_debug("query-result; key=%s, value=%s", key, str_c(value));
	}
#endif
}
/* end of query api */

/* result api */
static int mongodb_driver_result_var_expand(mongodb_result_t result, struct var_expand_table *_table)
{
	struct var_expand_table *table;
	struct hash_iterate_context *iter;
	const char *key, *tmp;
	string_t *value;
	int field_count, i;

	field_count = hash_table_count(result->fields);
	mongodb_debug("creating var_expand_table for %d element(s)", field_count);

	table = p_new(result->query->pool, struct var_expand_table, field_count);

	/* loop once to fill the var_expand_table */
	i = 0;
	iter = hash_table_iterate_init(result->fields);
	while (hash_table_iterate(iter, result->fields, &key, &value)) {
		table[i].key = '\0';
		table[i].long_key = key;
		table[i].value = str_c(value);
		i++;
	}
	hash_table_iterate_deinit(&iter);

	/* loop round creating the new values */
	iter = hash_table_iterate_init(result->fields);
	while (hash_table_iterate(iter, result->fields, &key, &value)) {
		tmp = t_strdup(str_c(value));
		str_truncate(value, 0);
		var_expand(value, tmp, table);
		mongodb_debug("key=%s, old-value=%s, new-value=%s", key, tmp, str_c(value));
	}
	hash_table_iterate_deinit(&iter);

	p_free(result->query->pool, table);

	return -1;
}

static void mongodb_driver_result_debug(mongodb_result_t result)
{
	struct hash_iterate_context *iter;
	const char *key;
	string_t *value;

	mongodb_debug("result=0x%p, fields=0x%p", result, result->fields);
	iter = hash_table_iterate_init(result->fields);
	while (hash_table_iterate(iter, result->fields, &key, &value)) {
		mongodb_debug("query-result; key=%s, value=%s", key, str_c(value));
	}
	hash_table_iterate_deinit(&iter);
}

static void mongodb_driver_result_field(mongodb_result_t result, const char *key,
				       const char **value_r)
{
	string_t *val;
	val = hash_table_lookup(result->fields, key);
	if (val == NULL) {
		*value_r = NULL;
	} else {
		mongodb_debug("field: key=%s, value=%s", key, str_c(val));
		*value_r = str_c(val);
	}
}

static struct mongodb_result_iterate_context *
mongodb_driver_result_iterate_init(mongodb_result_t result)
{
	struct mongodb_result_iterate_context *ctx;
	ctx = p_new(result->query->pool, struct mongodb_result_iterate_context, 1);
	ctx->iter = hash_table_iterate_init(result->fields);
	ctx->result = result;
	return ctx;
}

static int mongodb_driver_result_iterate(struct mongodb_result_iterate_context *ctx,
					 const char **key_r, string_t **value_r)
{
	return hash_table_iterate(ctx->iter, ctx->result->fields, key_r, value_r);
}

static void mongodb_driver_result_iterate_deinit(struct mongodb_result_iterate_context **_ctx)
{
	struct mongodb_result_iterate_context *ctx = *_ctx;
	struct mongodb_result *result = ctx->result;
	*_ctx = NULL;

	hash_table_iterate_deinit(&ctx->iter);
	p_free(result->query->pool, ctx);
}
/* end of result api */


const struct mongodb_driver_vfuncs mongodb_vfuncs = {

	/* connection api */
	mongodb_driver_conn_init,
	mongodb_driver_conn_deinit,
	mongodb_driver_get_error,

	/* query api */
	mongodb_driver_query_init,
	mongodb_driver_query_deinit,
	mongodb_driver_query_parse_defaults,
	mongodb_driver_query_parse_query,
	mongodb_driver_query_parse_fields,
	mongodb_driver_query_parse_update,
	mongodb_driver_query_debug,
	mongodb_driver_query_find_one,
	mongodb_driver_query_find,
	mongodb_driver_query_find_next,
	mongodb_driver_query_update,

	/* result api */
	mongodb_driver_result_var_expand,
	mongodb_driver_result_debug,
	mongodb_driver_result_field,
	mongodb_driver_result_iterate_init,
	mongodb_driver_result_iterate,
	mongodb_driver_result_iterate_deinit,
};

// vim: noexpandtab shiftwidth=8 tabstop=8
