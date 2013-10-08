#ifndef DB_MONGODB_H
#define DB_MONGODB_H

#include "str.h"
#include "hash.h"
#include "hash2.h"
#include "mongodb-api.h"

struct mongodb_settings {
	const char *connect;
	const char *database;
	const char *collection;

	const char *user_query;
	const char *user_fields;
	const char *user_defaults;

	const char *password_query;
	const char *password_fields;
	const char *password_defaults;

	const char *update_query;

	const char *iterate_query;
	const char *iterate_fields;
	const char *iterate_defaults;

	const char *default_pass_scheme;
	bool userdb_warning_disable;
};

struct mongodb_connection {
	struct mongodb_connection *next;

	pool_t pool;
	int refcount;

    char *config_path;
    struct mongodb_settings set;
	mongodb_conn_t conn;

	unsigned int default_password_query:1;
	unsigned int default_user_query:1;
	unsigned int default_update_query:1;
	unsigned int default_iterate_query:1;
	unsigned int userdb_used:1;
};

struct mongodb_connection *db_mongodb_init(const char *config_path, bool userdb);
void db_mongodb_unref(struct mongodb_connection **conn);

int db_mongodb_connect(struct mongodb_connection *conn);
void db_mongodb_success(struct mongodb_connection *conn);

void db_mongodb_check_userdb_warning(struct mongodb_connection *conn);

#endif
