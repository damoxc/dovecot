/* Copyright (c) 2004-2013 Dovecot authors, see the included COPYING file */

#include "lib.h"

#ifdef HAVE_MONGODB
#include "module-dir.h"
#include "mongodb-api.h"
#include "mongodb-api-private.h"

static bool mongodb_driver_loaded = FALSE;
static struct module *mongodb_driver = NULL;
static const struct mongodb_driver_vfuncs *mongodb_vfuncs = NULL;

static void mongodb_driver_unload(void)
{
	module_dir_unload(&mongodb_driver);
}

static int mongodb_driver_load(void)
{
	const char *plugin_name = "driver_mongodb";
	struct module_dir_load_settings mod_set;

	memset(&mod_set, 0, sizeof(mod_set));
	mod_set.abi_version = DOVECOT_ABI_VERSION;
	mod_set.setting_name = "<built-in driver-mongodb>";
	mongodb_driver = module_dir_load(MODULE_DIR, plugin_name, &mod_set);

	mongodb_vfuncs = module_get_symbol(mongodb_driver, "mongodb_vfuncs");
	if (mongodb_vfuncs == NULL) {
		i_error("%s: mongodb_vfuncs symbol not found", plugin_name);
		return -1; 
	}

	atexit(mongodb_driver_unload);
	mongodb_driver_loaded = TRUE;
	return 0;
}

/* connection api */
mongodb_conn_t mongodb_conn_init(const char *connection_string)
{
	if (!mongodb_driver_loaded)
		if (mongodb_driver_load() < 0)
			return NULL;

	return mongodb_vfuncs->conn_init(connection_string);
}

void mongodb_conn_deinit(mongodb_conn_t *_conn)
{
	mongodb_vfuncs->conn_deinit(_conn);
}

char *mongodb_get_error(mongodb_conn_t conn)
{
	return mongodb_vfuncs->get_error(conn);
}

/* query api */
mongodb_query_t mongodb_query_init(mongodb_conn_t conn)
{
	return mongodb_vfuncs->query_init(conn);
}

void mongodb_query_deinit(mongodb_query_t *_query)
{
	return mongodb_vfuncs->query_deinit(_query);
}

int mongodb_query_parse_defaults(mongodb_query_t query, const char *json)
{
	return mongodb_vfuncs->query_parse_defaults(query, json);
}

int mongodb_query_parse_query(mongodb_query_t query, const char *json)
{
	return mongodb_vfuncs->query_parse_query(query, json);
}

int mongodb_query_parse_fields(mongodb_query_t query, const char *json)
{
	return mongodb_vfuncs->query_parse_fields(query, json);
}

void mongodb_query_debug(mongodb_query_t query)
{
	return mongodb_vfuncs->query_debug(query);
}

int mongodb_query_find_one(mongodb_query_t query, const char *collection,
			   mongodb_result_t *result_r)
{
	return mongodb_vfuncs->query_find_one(query, collection, result_r);
}

int mongodb_query_find(mongodb_query_t query, const char *collection)
{
	return mongodb_vfuncs->query_find(query, collection);
}

int mongodb_query_find_next(mongodb_query_t query)
{
	return mongodb_vfuncs->query_find_next(query);
}

int mongodb_result_var_expand(mongodb_result_t result, struct var_expand_table *_table)
{
	return mongodb_vfuncs->result_var_expand(result, _table);
}

void mongodb_result_debug(mongodb_result_t result)
{
	mongodb_vfuncs->result_debug(result);
}

struct mongodb_result_iterate_context *
mongodb_result_iterate_init(mongodb_result_t result)
{
	return mongodb_vfuncs->result_iterate_init(result);
}

int mongodb_result_iterate(struct mongodb_result_iterate_context *ctx,
			   const char **key_r, string_t **value_r)
{
	return mongodb_vfuncs->result_iterate(ctx, key_r, value_r);
}

void mongodb_result_iterate_deinit(struct mongodb_result_iterate_context **_ctx)
{
	mongodb_vfuncs->result_iterate_deinit(_ctx);
}

#endif