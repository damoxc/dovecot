/* Copyright (c) 2004-2009 Dovecot authors, see the included COPYING file */

#include "common.h"
#include "password-scheme.h"
#include "passdb.h"
#include "passdb-cache.h"

#include <stdlib.h>

struct auth_cache *passdb_cache = NULL;

static void
passdb_cache_log_hit(struct auth_request *request, const char *value)
{
	const char *p;

	if (!request->auth->verbose_debug_passwords &&
	    *value != '\0' && *value != '\t') {
		/* hide the password */
		p = strchr(value, '\t');
		value = t_strconcat("<hidden>", p, NULL);
	}
	auth_request_log_debug(request, "cache", "hit: %s", value);
}

bool passdb_cache_verify_plain(struct auth_request *request, const char *key,
			       const char *password,
			       enum passdb_result *result_r, int use_expired)
{
	const char *value, *cached_pw, *scheme, *const *list;
	struct auth_cache_node *node;
	int ret;
	bool expired;

	if (passdb_cache == NULL || key == NULL)
		return FALSE;

	/* value = password \t ... */
	value = auth_cache_lookup(passdb_cache, request, key, &node, &expired);
	if (value == NULL || (expired && !use_expired)) {
		auth_request_log_debug(request, "cache",
				       value == NULL ? "miss" : "expired");
		return FALSE;
	}
	passdb_cache_log_hit(request, value);

	if (*value == '\0') {
		/* negative cache entry */
		auth_request_log_info(request, "cache", "User unknown");
		*result_r = PASSDB_RESULT_USER_UNKNOWN;
		return TRUE;
	}

	list = t_strsplit(value, "\t");

	cached_pw = list[0];
	if (*cached_pw == '\0') {
		/* NULL password */
		auth_request_log_info(request, "cache", "NULL password access");
		ret = 1;
	} else {
		scheme = password_get_scheme(&cached_pw);
		i_assert(scheme != NULL);

		ret = auth_request_password_verify(request, password, cached_pw,
						   scheme, "cache");

		if (ret == 0 && node->last_success) {
			/* the last authentication was successful. assume that
			   the password was changed and cache is expired. */
			node->last_success = FALSE;
			return FALSE;
		}
	}
	node->last_success = ret > 0;

	/* save the extra_fields only after we know we're using the
	   cached data */
	auth_request_set_fields(request, list + 1, NULL);

	*result_r = ret > 0 ? PASSDB_RESULT_OK :
		PASSDB_RESULT_PASSWORD_MISMATCH;
	return TRUE;
}

bool passdb_cache_lookup_credentials(struct auth_request *request,
				     const char *key, const char **password_r,
				     const char **scheme_r,
				     enum passdb_result *result_r,
				     bool use_expired)
{
	const char *value, *const *list;
	struct auth_cache_node *node;
	bool expired;

	if (passdb_cache == NULL)
		return FALSE;

	value = auth_cache_lookup(passdb_cache, request, key, &node, &expired);
	if (value == NULL || (expired && !use_expired)) {
		auth_request_log_debug(request, "cache",
				       value == NULL ? "miss" : "expired");
		return FALSE;
	}
	passdb_cache_log_hit(request, value);

	if (*value == '\0') {
		/* negative cache entry */
		*result_r = PASSDB_RESULT_USER_UNKNOWN;
		*password_r = NULL;
		*scheme_r = NULL;
		return TRUE;
	}

	list = t_strsplit(value, "\t");
	auth_request_set_fields(request, list + 1, NULL);

	*result_r = PASSDB_RESULT_OK;
	*password_r = *list[0] == '\0' ? NULL : list[0];
	*scheme_r = password_get_scheme(password_r);
	i_assert(*scheme_r != NULL || *password_r == NULL);
	return TRUE;
}

void passdb_cache_init(void)
{
	const char *env;
	size_t max_size;
	unsigned int cache_ttl, neg_cache_ttl;

	env = getenv("CACHE_SIZE");
	if (env == NULL)
		return;

	max_size = (size_t)strtoul(env, NULL, 10) * 1024;
	if (max_size == 0)
		return;

	env = getenv("CACHE_TTL");
	if (env == NULL)
		return;

	cache_ttl = (unsigned int)strtoul(env, NULL, 10);
	if (cache_ttl == 0)
		return;

	env = getenv("CACHE_NEGATIVE_TTL");
	neg_cache_ttl = env == NULL ? 0 : (unsigned int)strtoul(env, NULL, 10);
	passdb_cache = auth_cache_new(max_size, cache_ttl, neg_cache_ttl);
}

void passdb_cache_deinit(void)
{
	if (passdb_cache != NULL)
		auth_cache_free(&passdb_cache);
}
