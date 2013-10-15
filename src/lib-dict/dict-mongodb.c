/* Copyright (c) 2008-2013 Dovecot authors, see the included COPYING file */

#include "lib.h"

#ifdef HAVE_MONGODB

#include "array.h"
#include "dict-private.h"
#include "dict-transaction-memory.h"
#include "mongodb-api.h"
#include "dict-mongodb-settings.h"

struct mongodb_dict {
	struct dict dict;

	pool_t pool;
	mongodb_conn_t conn;
	const char *username;
	const char *db;

	const struct dict_mongodb_settings *set;
	unsigned int prev_map_match_idx;
};

struct mongodb_dict_iterate_context {
	struct dict_iterate_context ctx;
	pool_t pool;

	enum dict_iterate_flags flags;
	const char **paths;

	string_t *key;

	unsigned int key_prefix_len, pattern_prefix_len, next_map_idx;
};

typedef enum mongodb_dict_transaction_op_t {
	MONGODB_DICT_TRANSACTION_SET,
	MONGODB_DICT_TRANSACTION_UNSET,
	MONGODB_DICT_TRANSACTION_APPEND,
	MONGODB_DICT_TRANSACTION_INC
} mongodb_dict_transaction_op_t;

struct mongodb_dict_transaction_op {
	mongodb_dict_transaction_op_t op;
	const struct dict_mongodb_map *map;

	const char *key;
	const char *value;
	long long diff;

	struct mongodb_dict_transaction_op *next;
};

struct mongodb_dict_transaction_context {
	struct dict_transaction_context ctx;
	pool_t pool;

	struct mongodb_dict_transaction_op *ops;
	unsigned int failed:1;
};

static bool
dict_mongodb_map_match(const struct dict_mongodb_map *map, const char *path,
		       ARRAY_TYPE(const_string) *values, unsigned int *pat_len_r,
		       unsigned int *path_len_r, bool partial_ok)
{
	const char *path_start = path;
	const char *pat, *field, *p;
	unsigned int len;

	array_clear(values);
	pat = map->pattern;
	while (*pat != '\0' && *path != '\0') {
		if (*pat == '$') {
			/* variable */
			pat++;
			if (*pat == '\0') {
				/* pattern ended with this variable,
				   it'll match the rest of the path */
				len = strlen(path);
				if (partial_ok) {
					/* iterating - the last field never
					   matches fully. if there's a trailing
					   '/', drop it. */
					pat--;
					if (path[len-1] == '/') {
						field = t_strndup(path, len-1);
						array_append(values, &field, 1);
					} else {
						array_append(values, &path, 1);
					}
				} else {
					array_append(values, &path, 1);
					path += len;
				}
				*path_len_r = path - path_start;
				*pat_len_r = pat - map->pattern;
				return TRUE;
			}
			/* pattern matches until the next '/' in path */
			p = strchr(path, '/');
			if (p != NULL) {
				field = t_strdup_until(path, p);
				array_append(values, &field, 1);
				path = p;
			} else {
				/* no '/' anymore, but it'll still match a
				   partial */
				array_append(values, &path, 1);
				path += strlen(path);
				pat++;
			}
		} else if (*pat == *path) {
			pat++;
			path++;
		} else {
			return FALSE;
		}
	}

	*path_len_r = path - path_start;
	*pat_len_r = pat - map->pattern;

	if (*pat == '\0')
		return *path == '\0';
	else if (!partial_ok)
		return FALSE;
	else {
		/* partial matches must end with '/' */
		return pat == map->pattern || pat[-1] == '/';
	}
}

static const struct dict_mongodb_map *
mongodb_dict_find_map(struct mongodb_dict *dict, const char *path,
		      ARRAY_TYPE(const_string) *values)
{
	const struct dict_mongodb_map *maps;
	unsigned int i, idx, count, len;

	t_array_init(values, dict->set->max_field_count);
	maps = array_get(&dict->set->maps, &count);
	for (i = 0; i < count; i++) {
		/* start matching from the previously successful match */
		idx = (dict->prev_map_match_idx + i) % count;
		if (dict_mongodb_map_match(&maps[idx], path, values,
					   &len, &len, FALSE)) {
			dict->prev_map_match_idx = idx;
			return &maps[idx];
		}
	}
	return NULL;
}

static inline void mongodb_dict_free(struct mongodb_dict **_dict)
{
	struct mongodb_dict *dict = *_dict;

	if (dict == NULL)
		return;

	if (dict->conn != NULL) {
		mongodb_conn_deinit(&dict->conn);
	}

	pool_unref(&(dict->pool));
};

static int mongodb_dict_init(struct dict *driver, const char *uri,
			     enum dict_data_type value_type ATTR_UNUSED,
			     const char *username, const char *base_dir ATTR_UNUSED,
			     struct dict **dict_r, const char **error_r)
{
	struct mongodb_dict *dict;
	const char *const *args;
	int ret = 0;
	pool_t pool;

	pool = pool_alloconly_create("mongodb dict", 2048);

	dict = p_new(pool, struct mongodb_dict, 1);
	dict->pool = pool;
	dict->username = p_strdup(pool, username);
	dict->dict = *driver;
	dict->db = "mail";

	dict->set = dict_mongodb_settings_read(pool, uri, error_r);
	if (dict->set == NULL) {
		mongodb_dict_free(&dict);
		return -1;
	}

	dict->conn = mongodb_conn_init(dict->set->uri);
	*dict_r = &dict->dict;

	return 0;
}

static void mongodb_dict_deinit(struct dict *_dict)
{
	struct mongodb_dict *dict = (struct mongodb_dict *)_dict;
	i_debug("dict/mongodb: deinit");

	mongodb_dict_free(&dict);
}

static int mongodb_dict_lookup(struct  dict *_dict, pool_t pool,
			       const char *key, const char **value_r)
{
	struct mongodb_dict *dict = (struct mongodb_dict *)_dict;
	const struct dict_mongodb_map *map;
	ARRAY_TYPE(const_string) values;
	mongodb_result_t result;
	mongodb_query_t query;
	const char *json, *value;
	int ret;

	map = mongodb_dict_find_map(dict, key, &values);
	if (map == NULL) {
		i_error("mongodb dict lookup: Invalid/unmapped key: %s", key);
		*value_r = NULL;
		return 0;
	}

	json = t_strdup_printf("{\"%s\": \"%s\"}", map->username_field, dict->username);
	i_debug("dict/mongodb: query = %s", json);

	query = mongodb_query_init(dict->conn);
	mongodb_query_parse_query(query, json);
	mongodb_query_parse_fields(query, map->value_field);

	*value_r = NULL;
	ret = mongodb_query_find_one(query, map->collection, &result);
	if (ret != MONGODB_QUERY_OK) {
		ret = -1;
	} else {
		mongodb_result_field(result,  map->value_field, &value);
		if (value != NULL) {
			*value_r = p_strdup(pool, value);
			i_debug("dict/mongodb: value=%s", *value_r);
			ret = 1;
		} else {
			ret = 0;
		}
	}

	mongodb_query_deinit(&query);
	return ret;
}

static struct dict_transaction_context *
mongodb_dict_transaction_init(struct dict *_dict)
{
	struct dict_transaction_memory_context *ctx;
	pool_t pool;
	i_debug("dict/mongodb: transaction_init");

	pool = pool_alloconly_create("mongodb dict transaction", 2048);
	ctx = p_new(pool, struct dict_transaction_memory_context, 1);
	dict_transaction_memory_init(ctx, _dict, pool);
	return &ctx->ctx;
}

static void mongodb_dict_transaction_rollback(struct dict_transaction_context *_ctx)
{
	i_debug("dict/mongodb: transaction_rollback");
}

static int
mongodb_dict_transaction_commit(struct dict_transaction_context *_ctx,
				bool async,
				dict_transaction_commit_callback_t *callback,
				void *context)
{
	struct dict_transaction_memory_context *ctx =
		(struct dict_transaction_memory_context *)_ctx;
	struct mongodb_dict *dict = (struct mongodb_dict *)_ctx->dict;
	int ret = 1;
	i_debug("dict/mongodb: transaction_commit");

	if (callback != NULL)
		callback(ret, context);
	pool_unref(&ctx->pool);
	return ret;
}

static void mongodb_dict_set(struct dict_transaction_context *_ctx,
			     const char *key, const char *value)
{
	struct mongodb_dict_transaction_context *ctx =
		(struct mongodb_dict_transaction_context *)_ctx;
	struct mongodb_dict *dict = (struct mongodb_dict *)_ctx->dict;
	const struct dict_mongodb_map *map;
	ARRAY_TYPE(const_string) values;

	i_debug("dict/mongodb: set '%s' = '%s'", key, value);
}

static void mongodb_dict_unset(struct dict_transaction_context *_ctx,
			       const char *key)
{
	struct mongodb_dict_transaction_context *ctx =
		(struct mongodb_dict_transaction_context *)_ctx;
	struct mongodb_dict *dict = (struct mongodb_dict *)_ctx->dict;
	const struct dict_mongodb_map *map;
	ARRAY_TYPE(const_string) values;

	map = mongodb_dict_find_map(dict, key, &values);
	if (map == NULL) {
		i_error("mongodb dict unset: Invalid/unmapped key: %s", key);
		ctx->failed = TRUE;
		return;
	}

	T_BEGIN {
		mongodb_query_t query;
		const char *json;

		json = t_strdup_printf("{\"%s\": \"%s\"}", map->username_field, dict->username);
		i_debug("dict/mongodb: query = %s", json);
	} T_END;

}

static void mongodb_dict_append(struct dict_transaction_context *_ctx,
				const char *key, const char *value)
{
	i_debug("dict/mongodb: append '%s' = '%s'", key, value);
}

static void mongodb_dict_atomic_inc(struct dict_transaction_context *_ctx,
				    const char *key, long long diff)
{
	i_debug("dict/mongodb: atomic_inc '%s' by %lld", key, diff);
}


struct dict dict_driver_mongodb = {
	.name = "mongodb",
	{
		mongodb_dict_init,
		mongodb_dict_deinit,
		NULL,
		mongodb_dict_lookup,
		NULL,
		NULL,
		NULL,
		mongodb_dict_transaction_init,
		mongodb_dict_transaction_commit,
#if 1
		mongodb_dict_transaction_rollback,
		mongodb_dict_set,
		mongodb_dict_unset,
		mongodb_dict_append,
		mongodb_dict_atomic_inc
#else
		dict_transaction_memory_rollback,
		dict_transaction_memory_set,
		dict_transaction_memory_unset,
		dict_transaction_memory_append,
		dict_transaction_memory_atomic_inc
#endif
	}
};
#endif

// vim: noexpandtab shiftwidth=8 tabstop=8
