/* Copyright (c) 2003-2008 Dovecot authors, see the included COPYING file */

#include "common.h"

#if defined(PASSDB_SQL) || defined(USERDB_SQL)

#include "settings.h"
#include "auth-request.h"
#include "db-sql.h"

#include <stddef.h>
#include <stdlib.h>

#define DEF_STR(name) DEF_STRUCT_STR(name, sql_settings)
#define DEF_INT(name) DEF_STRUCT_INT(name, sql_settings)
#define DEF_BOOL(name) DEF_STRUCT_BOOL(name, sql_settings)

static struct setting_def setting_defs[] = {
	DEF_STR(driver),
	DEF_STR(connect),
	DEF_STR(password_query),
	DEF_STR(user_query),
 	DEF_STR(update_query),
	DEF_STR(default_pass_scheme),

	{ 0, NULL, 0 }
};

struct sql_settings default_sql_settings = {
	MEMBER(driver) NULL,
	MEMBER(connect) NULL,
	MEMBER(password_query) "SELECT username, domain, password FROM users WHERE username = '%n' AND domain = '%d'",
	MEMBER(user_query) "SELECT home, uid, gid FROM users WHERE username = '%n' AND domain = '%d'",
	MEMBER(update_query) "UPDATE users SET password = '%w' WHERE username = '%n' AND domain = '%d'",
	MEMBER(default_pass_scheme) "PLAIN-MD5"
};

static struct sql_connection *connections = NULL;

static struct sql_connection *sql_conn_find(const char *config_path)
{
	struct sql_connection *conn;

	for (conn = connections; conn != NULL; conn = conn->next) {
		if (strcmp(conn->config_path, config_path) == 0)
			return conn;
	}

	return NULL;
}

static const char *parse_setting(const char *key, const char *value,
				 struct sql_connection *conn)
{
	return parse_setting_from_defs(conn->pool, setting_defs,
				       &conn->set, key, value);
}

struct sql_connection *db_sql_init(const char *config_path)
{
	struct sql_connection *conn;
	pool_t pool;

	conn = sql_conn_find(config_path);
	if (conn != NULL) {
		conn->refcount++;
		return conn;
	}

	if (*config_path == '\0')
		i_fatal("sql: Configuration file path not given");

	pool = pool_alloconly_create("sql_connection", 1024);
	conn = p_new(pool, struct sql_connection, 1);
	conn->pool = pool;

	conn->refcount = 1;

	conn->config_path = p_strdup(pool, config_path);
	conn->set = default_sql_settings;
	if (!settings_read(config_path, NULL, parse_setting,
			   null_settings_section_callback, conn))
		exit(FATAL_DEFAULT);

	if (conn->set.driver == NULL) {
		i_fatal("sql: driver not set in configuration file %s",
			config_path);
	}
	if (conn->set.connect == NULL) {
		i_fatal("sql: connect string not set in configuration file %s",
			config_path);
	}
	conn->db = sql_init(conn->set.driver, conn->set.connect);

	conn->next = connections;
	connections = conn;
	return conn;
}

void db_sql_unref(struct sql_connection **_conn)
{
        struct sql_connection *conn = *_conn;

	*_conn = NULL;
	if (--conn->refcount > 0)
		return;

	sql_deinit(&conn->db);
	pool_unref(&conn->pool);
}

#endif
