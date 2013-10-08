/* Copyright (c) 2013 Dovecot authors, see the included COPYING file */

#include "auth-common.h"

#if defined(PASSDB_MONGODB) || defined(USERDB_MONGODB)

#include "db-mongodb.h"

#include "settings.h"
#include "auth-request.h"
#include "auth-worker-client.h"
#include "istream.h"
#include "str.h"
#include "json-parser.h"
#include "var-expand.h"
#include "mongodb-api.h"

#include <string.h>

#define DEF_STR(name) DEF_STRUCT_STR(name, mongodb_settings)
#define DEF_INT(name) DEF_STRUCT_INT(name, mongodb_settings)
#define DEF_BOOL(name) DEF_STRUCT_BOOL(name, mongodb_settings)

#define MAX_KEY_LENGTH 128
#define MAX_FIELD_LENGTH 1024

typedef HASH_TABLE(const char *, const char *) mongodb_fieldmap_t;

static struct setting_def setting_defs[] = {
	DEF_STR(connect),
	DEF_STR(database),
	DEF_STR(collection),
	DEF_STR(password_query),
	DEF_STR(password_fields),
	DEF_STR(password_defaults),
	DEF_STR(user_query),
	DEF_STR(user_fields),
	DEF_STR(user_defaults),
	DEF_STR(update_query),
	DEF_STR(iterate_query),
	DEF_STR(iterate_fields),
	DEF_STR(iterate_defaults),
	DEF_STR(default_pass_scheme),

	{ 0, NULL, 0 }
};

static struct mongodb_settings default_mongodb_settings = {
	.connect = NULL,
	.database = NULL,
	.collection = NULL,
	.password_defaults = NULL,
	.password_fields = "{\"password\": \"password\"}",
	.password_query = "{\"user\": \"%n\", \"domain\": \"%d\"}",
	.user_defaults = NULL,
	.user_fields = "{\"uid\": \"uid\", \"home\": \"home\", \"gid\": \"gid\"}",
	.user_query = "{\"user\": \"%n\", \"domain\": \"%d\"}",
	.update_query = "{\"user\": \"%n\", \"domain\": \"%d\"}",
	.iterate_query = "{}",
    .iterate_defaults = NULL,
    .iterate_fields = "{\"email\": \"user\"}",
	.default_pass_scheme = "MD5",
	.userdb_warning_disable = FALSE
};

static struct mongodb_connection *mongodb_connections = NULL;

/* Building a plugin */
extern struct passdb_module_interface passdb_mongodb_plugin;
extern struct userdb_module_interface userdb_mongodb_plugin;

static const char *parse_setting(const char *key, const char *value,
				 struct mongodb_connection *conn)
{
	return parse_setting_from_defs(conn->pool, setting_defs,
				       &conn->set, key, value);
}

static struct mongodb_connection *db_mongodb_conn_find(const char *config_path)
{
	struct mongodb_connection *conn;

	for (conn = mongodb_connections; conn != NULL; conn = conn->next) {
		if (strcmp(conn->config_path, config_path) == 0)
			return conn;
	}

	return NULL;
}

int db_mongodb_connect(struct mongodb_connection *conn)
{
	//mongo_client(conn->conn, conn->set.connect, 27017);
	return 0;
}

struct mongodb_connection *db_mongodb_init(const char *config_path, bool userdb)
{
	struct mongodb_connection *conn;
	const char *error;
	pool_t pool;

	/* see if it already exists */
	conn = db_mongodb_conn_find(config_path);
	if (conn != NULL) {
		if (userdb)
			conn->userdb_used = TRUE;
		conn->refcount++;
		return conn;
	}

	if (*config_path == '\0')
		i_fatal("mongodb: Configuration file path not given");

	pool = pool_alloconly_create("mongodb_connection", 1024);
	conn = p_new(pool, struct mongodb_connection, 1);
	conn->pool = pool;
	conn->refcount = 1;

	conn->userdb_used = userdb;
	conn->config_path = p_strdup(pool, config_path);
	conn->set = default_mongodb_settings;
	if (!settings_read_nosection(config_path, parse_setting, conn, &error))
		i_fatal("mongodb %s: %s", config_path, error);

	conn->next = mongodb_connections;
	mongodb_connections = conn;

	conn->conn = mongodb_conn_init("mongodb://localhost");

	return conn;
}

#endif
