/* Copyright (c) 2005-2010 Dovecot authors, see the included COPYING file */

#include "auth-common.h"
#include "network.h"
#include "array.h"
#include "str.h"
#include "env-util.h"
#include "mech.h"
#include "userdb.h"
#include "passdb.h"
#include "passdb-cache.h"
#include "auth.h"
#include "auth-request-handler.h"

#include <stdlib.h>
#include <unistd.h>

struct auth_userdb_settings userdb_dummy_set = {
	.driver = "static",
	.args = ""
};

static void
auth_passdb_preinit(struct auth *auth, const struct auth_passdb_settings *set)
{
	struct auth_passdb *auth_passdb, **dest;

	auth_passdb = p_new(auth->pool, struct auth_passdb, 1);
	auth_passdb->set = set;

	for (dest = &auth->passdbs; *dest != NULL; dest = &(*dest)->next) ;
	*dest = auth_passdb;

	auth_passdb->passdb =
		passdb_preinit(auth->pool, set->driver, set->args);
}

static void
auth_userdb_preinit(struct auth *auth, const struct auth_userdb_settings *set)
{
        struct auth_userdb *auth_userdb, **dest;

	auth_userdb = p_new(auth->pool, struct auth_userdb, 1);
	auth_userdb->set = set;

	for (dest = &auth->userdbs; *dest != NULL; dest = &(*dest)->next) ;
	*dest = auth_userdb;

	auth_userdb->userdb =
		userdb_preinit(auth->pool, set->driver, set->args);
}

struct auth *auth_preinit(struct auth_settings *set)
{
	struct auth_passdb_settings *const *passdbs;
	struct auth_userdb_settings *const *userdbs;
	struct auth *auth;
	pool_t pool;
	unsigned int i, count, db_count, passdb_count, last_passdb = 0;

	pool = pool_alloconly_create("auth", 2048);
	auth = p_new(pool, struct auth, 1);
	auth->pool = pool;
	auth->set = set;

	if (array_is_created(&set->passdbs))
		passdbs = array_get(&set->passdbs, &db_count);
	else {
		passdbs = NULL;
		db_count = 0;
	}

	/* initialize passdbs first and count them */
	for (passdb_count = 0, i = 0; i < db_count; i++) {
		if (passdbs[i]->master)
			continue;

		auth_passdb_preinit(auth, passdbs[i]);
		passdb_count++;
		last_passdb = i;
	}
	if (passdb_count != 0 && passdbs[last_passdb]->pass)
		i_fatal("Last passdb can't have pass=yes");

	for (i = 0; i < db_count; i++) {
		if (!passdbs[i]->master)
			continue;

		if (passdbs[i]->deny)
			i_fatal("Master passdb can't have deny=yes");
		if (passdbs[i]->pass && passdb_count == 0) {
			i_fatal("Master passdb can't have pass=yes "
				"if there are no passdbs");
		}
		auth_passdb_preinit(auth, passdbs[i]);
	}

	if (array_is_created(&set->userdbs)) {
		userdbs = array_get(&set->userdbs, &count);
		for (i = 0; i < count; i++)
			auth_userdb_preinit(auth, userdbs[i]);
	}

	if (auth->userdbs == NULL) {
		/* use a dummy userdb static. */
		auth_userdb_preinit(auth, &userdb_dummy_set);
	}
	return auth;
}

const string_t *auth_mechanisms_get_list(struct auth *auth)
{
	struct mech_module_list *list;
	string_t *str;

	str = t_str_new(128);
	for (list = auth->mech_modules; list != NULL; list = list->next)
		str_append(str, list->module.mech_name);

	return str;
}

static void auth_mech_register(struct auth *auth, const struct mech_module *mech)
{
	struct mech_module_list *list;

	list = p_new(auth->pool, struct mech_module_list, 1);
	list->module = *mech;

	str_printfa(auth->mech_handshake, "MECH\t%s", mech->mech_name);
	if ((mech->flags & MECH_SEC_PRIVATE) != 0)
		str_append(auth->mech_handshake, "\tprivate");
	if ((mech->flags & MECH_SEC_ANONYMOUS) != 0)
		str_append(auth->mech_handshake, "\tanonymous");
	if ((mech->flags & MECH_SEC_PLAINTEXT) != 0)
		str_append(auth->mech_handshake, "\tplaintext");
	if ((mech->flags & MECH_SEC_DICTIONARY) != 0)
		str_append(auth->mech_handshake, "\tdictionary");
	if ((mech->flags & MECH_SEC_ACTIVE) != 0)
		str_append(auth->mech_handshake, "\tactive");
	if ((mech->flags & MECH_SEC_FORWARD_SECRECY) != 0)
		str_append(auth->mech_handshake, "\tforward-secrecy");
	if ((mech->flags & MECH_SEC_MUTUAL_AUTH) != 0)
		str_append(auth->mech_handshake, "\tmutual-auth");
	str_append_c(auth->mech_handshake, '\n');

	list->next = auth->mech_modules;
	auth->mech_modules = list;
}

static bool auth_passdb_list_have_verify_plain(struct auth *auth)
{
	struct auth_passdb *passdb;

	for (passdb = auth->passdbs; passdb != NULL; passdb = passdb->next) {
		if (passdb->passdb->iface.verify_plain != NULL)
			return TRUE;
	}
	return FALSE;
}

static bool auth_passdb_list_have_lookup_credentials(struct auth *auth)
{
	struct auth_passdb *passdb;

	for (passdb = auth->passdbs; passdb != NULL; passdb = passdb->next) {
		if (passdb->passdb->iface.lookup_credentials != NULL)
			return TRUE;
	}
	return FALSE;
}

static int auth_passdb_list_have_set_credentials(struct auth *auth)
{
	struct auth_passdb *passdb;

	for (passdb = auth->passdbs; passdb != NULL; passdb = passdb->next) {
		if (passdb->passdb->iface.set_credentials != NULL)
			return TRUE;
	}
	return FALSE;
}

static bool
auth_mech_verify_passdb(struct auth *auth, struct mech_module_list *list)
{
	switch (list->module.passdb_need) {
	case MECH_PASSDB_NEED_NOTHING:
		break;
	case MECH_PASSDB_NEED_VERIFY_PLAIN:
		if (!auth_passdb_list_have_verify_plain(auth))
			return FALSE;
		break;
	case MECH_PASSDB_NEED_VERIFY_RESPONSE:
	case MECH_PASSDB_NEED_LOOKUP_CREDENTIALS:
		if (!auth_passdb_list_have_lookup_credentials(auth))
			return FALSE;
		break;
	case MECH_PASSDB_NEED_SET_CREDENTIALS:
		if (!auth_passdb_list_have_lookup_credentials(auth))
			return FALSE;
		if (!auth_passdb_list_have_set_credentials(auth))
			return FALSE;
		break;
	}
	return TRUE;
}

static void auth_mech_list_verify_passdb(struct auth *auth)
{
	struct mech_module_list *list;

	for (list = auth->mech_modules; list != NULL; list = list->next) {
		if (!auth_mech_verify_passdb(auth, list))
			break;
	}

	if (list != NULL) {
		if (auth->passdbs == NULL) {
			i_fatal("No passdbs specified in configuration file. "
				"%s mechanism needs one",
				list->module.mech_name);
		}
		i_fatal("%s mechanism can't be supported with given passdbs",
			list->module.mech_name);
	}
}

void auth_init(struct auth *auth)
{
	struct auth_passdb *passdb;
	struct auth_userdb *userdb;
	const struct mech_module *mech;
	const char *const *mechanisms;

	for (passdb = auth->masterdbs; passdb != NULL; passdb = passdb->next)
		passdb_init(passdb->passdb);
	for (passdb = auth->passdbs; passdb != NULL; passdb = passdb->next)
		passdb_init(passdb->passdb);
	for (userdb = auth->userdbs; userdb != NULL; userdb = userdb->next)
		userdb_init(userdb->userdb);

	/* caching is handled only by the main auth process */
	if (!worker)
		passdb_cache_init(auth->set);

	auth->mech_handshake = str_new(auth->pool, 512);

	/* register wanted mechanisms */
	mechanisms = t_strsplit_spaces(auth->set->mechanisms, " ");
	while (*mechanisms != NULL) {
		if (strcasecmp(*mechanisms, "ANONYMOUS") == 0) {
			if (*auth->set->anonymous_username == '\0') {
				i_fatal("ANONYMOUS listed in mechanisms, "
					"but anonymous_username not set");
			}
		}
		mech = mech_module_find(*mechanisms);
		if (mech == NULL) {
			i_fatal("Unknown authentication mechanism '%s'",
				*mechanisms);
		}
		auth_mech_register(auth, mech);

		mechanisms++;
	}

	if (auth->mech_modules == NULL)
		i_fatal("No authentication mechanisms configured");
	auth_mech_list_verify_passdb(auth);
}

void auth_deinit(struct auth **_auth)
{
        struct auth *auth = *_auth;
	struct auth_passdb *passdb;
	struct auth_userdb *userdb;

	*_auth = NULL;

	for (passdb = auth->masterdbs; passdb != NULL; passdb = passdb->next)
		passdb_deinit(passdb->passdb);
	for (passdb = auth->passdbs; passdb != NULL; passdb = passdb->next)
		passdb_deinit(passdb->passdb);
	for (userdb = auth->userdbs; userdb != NULL; userdb = userdb->next)
		userdb_deinit(userdb->userdb);

	auth_request_handler_deinit();
	passdb_cache_deinit();

	pool_unref(&auth->pool);
}
