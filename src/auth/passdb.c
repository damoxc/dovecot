/* Copyright (c) 2002-2009 Dovecot authors, see the included COPYING file */

#include "common.h"
#include "array.h"
#include "password-scheme.h"
#include "auth-worker-server.h"
#include "passdb.h"

#include <stdlib.h>

static ARRAY_DEFINE(passdb_interfaces, struct passdb_module_interface *);

static struct passdb_module_interface *passdb_interface_find(const char *name)
{
	struct passdb_module_interface *const *ifaces;
	unsigned int i, count;

	ifaces = array_get(&passdb_interfaces, &count);
	for (i = 0; i < count; i++) {
		if (strcmp(ifaces[i]->name, name) == 0)
			return ifaces[i];
	}
	return NULL;
}

void passdb_register_module(struct passdb_module_interface *iface)
{
	if (passdb_interface_find(iface->name) != NULL) {
		i_panic("passdb_register_module(%s): Already registered",
			iface->name);
	}
	array_append(&passdb_interfaces, &iface, 1);
}

void passdb_unregister_module(struct passdb_module_interface *iface)
{
	struct passdb_module_interface *const *ifaces;
	unsigned int i, count;

	ifaces = array_get(&passdb_interfaces, &count);
	for (i = 0; i < count; i++) {
		if (ifaces[i] == iface) {
			array_delete(&passdb_interfaces, i, 1);
			return;
		}
	}
	i_panic("passdb_unregister_module(%s): Not registered", iface->name);
}

bool passdb_get_credentials(struct auth_request *auth_request,
			    const char *input, const char *input_scheme,
			    const unsigned char **credentials_r, size_t *size_r)
{
	const char *wanted_scheme = auth_request->credentials_scheme;
	const char *plaintext;
	int ret;

	ret = password_decode(input, input_scheme, credentials_r, size_r);
	if (ret <= 0) {
		if (ret < 0) {
			auth_request_log_error(auth_request, "password",
				"Invalid password format for scheme %s",
				input_scheme);
		} else {
			auth_request_log_error(auth_request, "password",
				"Unknown scheme %s", input_scheme);
		}
		return FALSE;
	}

	if (*wanted_scheme == '\0') {
		/* anything goes. change the credentials_scheme to what we
		   actually got, so blocking passdbs work. */
		auth_request->credentials_scheme =
			p_strdup(auth_request->pool, input_scheme);
		return TRUE;
	}

	if (!password_scheme_is_alias(input_scheme, wanted_scheme)) {
		if (!password_scheme_is_alias(input_scheme, "PLAIN")) {
			const char *error = t_strdup_printf(
				"Requested %s scheme, but we have only %s",
				wanted_scheme, input_scheme);
			if (auth_request->auth->verbose_debug_passwords) {
				error = t_strdup_printf("%s (input: %s)",
							error, input);
			}
			auth_request_log_info(auth_request, "password",
					      "%s", error);
			return FALSE;
		}

		/* we can generate anything out of plaintext passwords */
		plaintext = t_strndup(*credentials_r, *size_r);
		if (auth_request->auth->verbose_debug_passwords) {
			auth_request_log_info(auth_request, "password",
				"Generating %s from user %s password %s",
				wanted_scheme, auth_request->original_username,
				plaintext);
		}
		if (!password_generate(plaintext,
				       auth_request->original_username,
				       wanted_scheme, credentials_r, size_r)) {
			auth_request_log_error(auth_request, "password",
				"Requested unknown scheme %s", wanted_scheme);
			return FALSE;
		}
	}

	return TRUE;
}

void passdb_handle_credentials(enum passdb_result result,
			       const char *password, const char *scheme,
			       lookup_credentials_callback_t *callback,
                               struct auth_request *auth_request)
{
	const unsigned char *credentials;
	size_t size = 0;

	if (result != PASSDB_RESULT_OK) {
		callback(result, NULL, 0, auth_request);
		return;
	}

	if (password == NULL) {
		auth_request_log_info(auth_request, "password",
			"Requested %s scheme, but we have a NULL password",
			auth_request->credentials_scheme);
		result = PASSDB_RESULT_SCHEME_NOT_AVAILABLE;
	} else if (!passdb_get_credentials(auth_request, password, scheme,
					   &credentials, &size)) {
		result = PASSDB_RESULT_SCHEME_NOT_AVAILABLE;
	}

	callback(result, credentials, size, auth_request);
}

struct auth_passdb *passdb_preinit(struct auth *auth, const char *driver,
				   const char *args, unsigned int id)
{
	struct passdb_module_interface *iface;
        struct auth_passdb *auth_passdb;

	if (args == NULL) args = "";

	auth_passdb = p_new(auth->pool, struct auth_passdb, 1);
	auth_passdb->auth = auth;
        auth_passdb->args = p_strdup(auth->pool, args);
        auth_passdb->id = id;

	iface = passdb_interface_find(driver);
	if (iface == NULL)
		i_fatal("Unknown passdb driver '%s'", driver);
	if (iface->verify_plain == NULL) {
		i_fatal("Support not compiled in for passdb driver '%s'",
			driver);
	}

	if (iface->preinit == NULL && iface->init == NULL &&
	    *auth_passdb->args != '\0') {
		i_fatal("passdb %s: No args are supported: %s",
			driver, auth_passdb->args);
	}

	if (iface->preinit == NULL) {
		auth_passdb->passdb =
			p_new(auth->pool, struct passdb_module, 1);
	} else {
		auth_passdb->passdb =
			iface->preinit(auth_passdb, auth_passdb->args);
	}
	auth_passdb->passdb->iface = *iface;
	return auth_passdb;
}

void passdb_init(struct auth_passdb *passdb)
{
	if (passdb->passdb->iface.init != NULL)
		passdb->passdb->iface.init(passdb->passdb, passdb->args);

	i_assert(passdb->passdb->default_pass_scheme != NULL ||
		 passdb->passdb->cache_key == NULL);

	if (passdb->passdb->blocking && !worker) {
		/* blocking passdb - we need an auth server */
		auth_worker_server_init();
	}
}

void passdb_deinit(struct auth_passdb *passdb)
{
	if (passdb->passdb->iface.deinit != NULL)
		passdb->passdb->iface.deinit(passdb->passdb);
}

extern struct passdb_module_interface passdb_passwd;
extern struct passdb_module_interface passdb_bsdauth;
extern struct passdb_module_interface passdb_shadow;
extern struct passdb_module_interface passdb_passwd_file;
extern struct passdb_module_interface passdb_pam;
extern struct passdb_module_interface passdb_checkpassword;
extern struct passdb_module_interface passdb_vpopmail;
extern struct passdb_module_interface passdb_ldap;
extern struct passdb_module_interface passdb_sql;
extern struct passdb_module_interface passdb_sia;

void passdbs_init(void)
{
	i_array_init(&passdb_interfaces, 16);
	passdb_register_module(&passdb_passwd);
	passdb_register_module(&passdb_bsdauth);
	passdb_register_module(&passdb_passwd_file);
	passdb_register_module(&passdb_pam);
	passdb_register_module(&passdb_checkpassword);
	passdb_register_module(&passdb_shadow);
	passdb_register_module(&passdb_vpopmail);
	passdb_register_module(&passdb_ldap);
	passdb_register_module(&passdb_sql);
	passdb_register_module(&passdb_sia);
}

void passdbs_deinit(void)
{
	array_free(&passdb_interfaces);
}
