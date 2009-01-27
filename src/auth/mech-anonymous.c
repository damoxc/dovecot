/* Copyright (c) 2002-2009 Dovecot authors, see the included COPYING file */

#include "common.h"
#include "mech.h"

static void
mech_anonymous_auth_continue(struct auth_request *request,
			     const unsigned char *data, size_t data_size)
{
	i_assert(*request->auth->set->anonymous_username != '\0');

	if (request->auth->set->verbose) {
		/* temporarily set the user to the one that was given,
		   so that the log message goes right */
		request->user =
			p_strndup(pool_datastack_create(), data, data_size);
		auth_request_log_info(request, "anonymous", "login");
	}

	request->user = p_strdup(request->pool,
				 request->auth->set->anonymous_username);

	auth_request_success(request, NULL, 0);
}

static struct auth_request *mech_anonymous_auth_new(void)
{
        struct auth_request *request;
	pool_t pool;

	pool = pool_alloconly_create("anonymous_auth_request", 512);
	request = p_new(pool, struct auth_request, 1);
	request->pool = pool;
	return request;
}

const struct mech_module mech_anonymous = {
	"ANONYMOUS",

	MEMBER(flags) MECH_SEC_ANONYMOUS,
	MEMBER(passdb_need) MECH_PASSDB_NEED_NOTHING,

	mech_anonymous_auth_new,
	mech_generic_auth_initial,
	mech_anonymous_auth_continue,
	mech_generic_auth_free
};
