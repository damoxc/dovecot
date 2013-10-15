#ifndef MONGODB_API_H
#define MONGODB_API_H

#include "var-expand.h"

#define MONGODB_QUERY_OK         0
#define MONGODB_QUERY_ERROR     -1
#define MONGODB_QUERY_NO_RESULT -2

struct mongodb_host {
    char *host;
    int   port;
};

struct mongodb_uri {
    struct mongodb_host host;
    char                *database;
};

struct mongodb_conn;
typedef struct mongodb_conn *mongodb_conn_t;

struct mongodb_query;
typedef struct mongodb_query *mongodb_query_t;

struct mongodb_result;
typedef struct mongodb_result *mongodb_result_t;

struct mongodb_result_iterate_context {
	mongodb_result_t             result;
    struct hash_iterate_context *iter;
	const char                  *error;
};

/* connection api */
mongodb_conn_t mongodb_conn_init(const char *connection_string);
void mongodb_conn_deinit(mongodb_conn_t *_conn);
char *mongodb_get_error(mongodb_conn_t conn);

/* query api */
mongodb_query_t mongodb_query_init(mongodb_conn_t conn);
void mongodb_query_deinit(mongodb_query_t *_query);

int mongodb_query_parse_defaults(mongodb_query_t query, const char *json);
int mongodb_query_parse_query(mongodb_query_t query, const char *json);
int mongodb_query_parse_fields(mongodb_query_t query, const char *json);
void mongodb_query_debug(mongodb_query_t query);

int mongodb_query_find_one(mongodb_query_t query, const char *collection, mongodb_result_t *result_r);
int mongodb_query_find(mongodb_query_t query, const char *collection);
int mongodb_query_find_next(mongodb_query_t query, mongodb_result_t *result_r);

void mongodb_result_field(mongodb_result_t result, const char *key, const char **value_r);
void mongodb_result_debug(mongodb_result_t result);
int mongodb_result_var_expand(mongodb_result_t result, struct var_expand_table *_table);
void mongodb_result_deinit(mongodb_result_t *_result);

struct mongodb_result_iterate_context *mongodb_result_iterate_init(mongodb_result_t result);
int mongodb_result_iterate(struct mongodb_result_iterate_context *ctx, const char **key_r, string_t **value_r);
void mongodb_result_iterate_deinit(struct mongodb_result_iterate_context **_ctx);


#endif

// vim: noexpandtab shiftwidth=8 tabstop=8
