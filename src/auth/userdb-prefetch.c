/* Copyright (c) 2004-2011 Dovecot authors, see the included COPYING file */

#include "auth-common.h"
#include "userdb.h"

#ifdef USERDB_PREFETCH

#include "str.h"
#include "var-expand.h"

#include <stdlib.h>

static void prefetch_lookup(struct auth_request *auth_request,
			    userdb_callback_t *callback)
{
	/* auth_request_set_field() should have already placed the userdb_*
	   values to userdb_reply. */
	if (auth_request->userdb_reply == NULL) {
		if (auth_request_get_auth(auth_request)->userdbs->next == NULL) {
			/* no other userdbs */
			if (auth_request->userdb_lookup) {
				auth_request_log_error(auth_request, "prefetch",
					"userdb lookup not possible with only userdb prefetch");
			} else {
				auth_request_log_error(auth_request, "prefetch",
					"passdb didn't return userdb entries");
			}
		} else if (!auth_request->userdb_lookup ||
			   auth_request->set->debug) {
			/* more userdbs, they may know the user */
			auth_request_log_debug(auth_request, "prefetch",
				"passdb didn't return userdb entries, "
				"trying the next userdb");
		}
		callback(USERDB_RESULT_USER_UNKNOWN, auth_request);
		return;
	}

	auth_request_log_debug(auth_request, "prefetch", "success");
	callback(USERDB_RESULT_OK, auth_request);
}

struct userdb_module_interface userdb_prefetch = {
	"prefetch",

	NULL,
	NULL,
	NULL,

	prefetch_lookup,

	NULL,
	NULL,
	NULL
};
#else
struct userdb_module_interface userdb_prefetch = {
	.name = "prefetch"
};
#endif
