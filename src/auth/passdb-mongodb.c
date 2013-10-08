/* Copyright (c) 2013 Dovecot authors, see the included COPYING file */

#include "auth-common.h"
#include "passdb.h"

#ifdef PASSDB_MONGODB

#include "str.h"
#include "strescape.h"
#include "var-expand.h"
#include "safe-memset.h"
#include "password-scheme.h"
#include "auth-cache.h"
#include "db-mongodb.h"

struct mongodb_passdb_module {
	struct passdb_module module;

	struct mongodb_connection *conn;
};

struct passdb_mongodb_request {
	struct auth_request *auth_request;
	union {
		verify_plain_callback_t *verify_plain;
		lookup_credentials_callback_t *lookup_credentials;
		set_credentials_callback_t *set_credentials;
	} callback;
};

static void mongodb_query_save_results(mongodb_result_t result,
				       struct passdb_mongodb_request *mongodb_request)
{
	struct auth_request *auth_request = mongodb_request->auth_request;
	struct passdb_module *_module = auth_request->passdb->passdb;
	struct mongodb_passdb_module *module = (struct mongodb_passdb_module *)_module;

	struct mongodb_result_iterate_context *iter;
	const char *key;
	string_t *value;

	iter = mongodb_result_iterate_init(result);
	while (mongodb_result_iterate(iter, &key, &value)) {
		if (*key != '\0' && value != NULL) {
			auth_request_set_field(auth_request, key, str_c(value),
				module->conn->set.default_pass_scheme);
		}
	}
	mongodb_result_iterate_deinit(&iter);
}

static void mongodb_lookup_pass(struct passdb_mongodb_request *mongodb_request)
{
	struct auth_request *auth_request = mongodb_request->auth_request;
	struct passdb_module *_module = auth_request->passdb->passdb;
	struct mongodb_passdb_module *module = (struct mongodb_passdb_module *)_module;
	mongodb_conn_t mongodb_conn = module->conn->conn;
	mongodb_query_t mongodb_query;
	mongodb_result_t result;

	enum passdb_result passdb_result;
	const char *password, *scheme;
	string_t *query;
	int ret;

	passdb_result = PASSDB_RESULT_INTERNAL_FAILURE;
	password = NULL;

	auth_request_ref(mongodb_request->auth_request);

 	query = t_str_new(512);
	var_expand(query, module->conn->set.password_query,
		   auth_request_get_var_expand_table(auth_request,
						     NULL));

	auth_request_log_debug(auth_request, "mongodb",
			       "query: %s", str_c(query));

	mongodb_query = mongodb_query_init(mongodb_conn);
	if (mongodb_query_parse_query(mongodb_query, str_c(query)) < 0) {
		auth_request_log_error(auth_request, "mongodb",
				       "Query failed: %s", mongodb_get_error(mongodb_conn));
		goto cleanup;
	}

	if (module->conn->set.password_defaults != NULL)
		if (mongodb_query_parse_defaults(mongodb_query,
						 module->conn->set.password_defaults) < 0) {
			auth_request_log_error(auth_request, "mongodb",
					       "Query failed: %s", mongodb_get_error(mongodb_conn));
			goto cleanup;
		}

	if (mongodb_query_parse_fields(mongodb_query, module->conn->set.password_fields) < 0) {
		auth_request_log_error(auth_request, "mongodb",
				       "Query failed: %s", mongodb_get_error(mongodb_conn));
		goto cleanup;
	}

	ret = mongodb_query_find_one(mongodb_query, module->conn->set.collection, &result);

	//mongodb_query_debug("db_mongodb_find return=%d", ret);
	if (ret != MONGODB_QUERY_OK) {
		if (ret == MONGODB_QUERY_NO_RESULT) {
			auth_request_log_info(auth_request, "mongodb", "unknown user");
			passdb_result = PASSDB_RESULT_USER_UNKNOWN;
		} else {
			auth_request_log_error(auth_request, "mongodb",
					       "Query failed: %s", mongodb_get_error(mongodb_conn));
		}
	} else {
		mongodb_query_save_results(result, mongodb_request);

		if (auth_request->passdb_password == NULL &&
		    !auth_fields_exists(auth_request->extra_fields, "nopassword")) {
		    	auth_request_log_info(auth_request, "mongodb",
		    			      "Empty password returned without nopassword");
		    	passdb_result = PASSDB_RESULT_PASSWORD_MISMATCH;
		} else {
			password = t_strdup(auth_request->passdb_password);
			passdb_result = PASSDB_RESULT_OK;
		}
	}

	scheme = password_get_scheme(&password);
	/* auth_request_set_field() sets scheme */
	i_assert(password == NULL || scheme != NULL);

	if (auth_request->credentials_scheme != NULL) {
		passdb_handle_credentials(passdb_result, password, scheme,
			mongodb_request->callback.lookup_credentials,
			auth_request);
		goto cleanup;
	}

	/* verify plain */
	if (password == NULL) {
		mongodb_request->callback.verify_plain(passdb_result, auth_request);
		goto cleanup;
	}

	ret = auth_request_password_verify(auth_request,
					   auth_request->mech_password,
					   password, scheme, "mongodb");

	mongodb_request->callback.verify_plain(ret > 0 ? PASSDB_RESULT_OK :
					       PASSDB_RESULT_PASSWORD_MISMATCH,
					       auth_request);

cleanup:
	mongodb_query_deinit(&mongodb_query);
	auth_request_unref(&auth_request);
}

static void mongodb_verify_plain(struct auth_request *request,
				 const char *password ATTR_UNUSED,
				 verify_plain_callback_t *callback)
{
	struct passdb_mongodb_request *mongodb_request;


	mongodb_request = p_new(request->pool, struct passdb_mongodb_request, 1);
	mongodb_request->auth_request = request;
	mongodb_request->callback.verify_plain = callback;

	mongodb_lookup_pass(mongodb_request);
}

static void mongodb_lookup_credentials(struct auth_request *request,
				       lookup_credentials_callback_t *callback)
{
	struct passdb_mongodb_request *mongodb_request;

	mongodb_request = p_new(request->pool, struct passdb_mongodb_request, 1);
	mongodb_request->auth_request = request;
	mongodb_request->callback.lookup_credentials = callback;

	mongodb_lookup_pass(mongodb_request);
}

static int mongodb_set_credentials(struct auth_request *request,
				   const char *new_credentials,
				   set_credentials_callback_t *callback)
{
	return -1;
}

static struct passdb_module *
passdb_mongodb_preinit(pool_t pool, const char *args)
{
	struct mongodb_passdb_module *module;
	struct mongodb_connection *conn;

	module = p_new(pool, struct mongodb_passdb_module, 1);
	module->conn = conn = db_mongodb_init(args, FALSE);

	module->module.cache_key =
		auth_cache_parse_key(pool, conn->set.password_query);
	module->module.default_pass_scheme = conn->set.default_pass_scheme;
	return &module->module;
}

static void passdb_mongodb_init(struct passdb_module *_module)
{
	struct mongodb_passdb_module *module =
		(struct mongodb_passdb_module *)_module;

	(void)db_mongodb_connect(module->conn);
#if 0
	if (module->conn->set.auth_bind) {
		/* Credential lookups can't be done with authentication binds */
		_module->iface.lookup_credentials = NULL;
	}
#endif
}

static void passdb_mongodb_deinit(struct passdb_module *_module)
{
	struct mongodb_passdb_module *module =
		(struct mongodb_passdb_module *)_module;

	mongodb_conn_deinit(&module->conn->conn);
}

struct passdb_module_interface passdb_mongodb = {
	"mongodb",

	passdb_mongodb_preinit,
	passdb_mongodb_init,
	passdb_mongodb_deinit,

	mongodb_verify_plain,
	mongodb_lookup_credentials,
	mongodb_set_credentials
};
#else
struct passdb_module_interface passdb_mongodb = {
	.name = "mongodb"
};
#endif

// vim: noexpandtab shiftwidth=8 tabstop=8
