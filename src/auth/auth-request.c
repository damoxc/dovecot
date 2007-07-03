/* Copyright (C) 2002-2005 Timo Sirainen */

#include "common.h"
#include "ioloop.h"
#include "buffer.h"
#include "hash.h"
#include "hex-binary.h"
#include "str.h"
#include "safe-memset.h"
#include "str-sanitize.h"
#include "strescape.h"
#include "var-expand.h"
#include "auth-cache.h"
#include "auth-request.h"
#include "auth-client-connection.h"
#include "auth-master-connection.h"
#include "passdb.h"
#include "passdb-blocking.h"
#include "userdb-blocking.h"
#include "passdb-cache.h"
#include "password-scheme.h"

#include <stdlib.h>
#include <sys/stat.h>

struct auth_request *
auth_request_new(struct auth *auth, const struct mech_module *mech,
		 mech_callback_t *callback, void *context)
{
	struct auth_request *request;

	request = mech->auth_new();
	request->state = AUTH_REQUEST_STATE_NEW;
	request->passdb = auth->passdbs;
	request->userdb = auth->userdbs;

	request->refcount = 1;
	request->last_access = ioloop_time;

	request->auth = auth;
	request->mech = mech;
	request->callback = callback;
	request->context = context;
	return request;
}

struct auth_request *auth_request_new_dummy(struct auth *auth)
{
	struct auth_request *auth_request;
	pool_t pool;

	pool = pool_alloconly_create("auth_request", 1024);
	auth_request = p_new(pool, struct auth_request, 1);
	auth_request->pool = pool;

	auth_request->refcount = 1;
	auth_request->last_access = ioloop_time;
	auth_request->auth = auth;
	auth_request->passdb = auth->passdbs;
	auth_request->userdb = auth->userdbs;

	return auth_request;
}

void auth_request_success(struct auth_request *request,
			  const void *data, size_t data_size)
{
	i_assert(request->state == AUTH_REQUEST_STATE_MECH_CONTINUE);

	if (request->passdb_failure) {
		/* password was valid, but some other check failed. */
		auth_request_fail(request);
		return;
	}

	request->state = AUTH_REQUEST_STATE_FINISHED;
	request->successful = TRUE;
	request->last_access = ioloop_time;
	request->callback(request, AUTH_CLIENT_RESULT_SUCCESS,
			  data, data_size);
}

void auth_request_fail(struct auth_request *request)
{
	i_assert(request->state == AUTH_REQUEST_STATE_MECH_CONTINUE);

	request->state = AUTH_REQUEST_STATE_FINISHED;
	request->last_access = ioloop_time;
	request->callback(request, AUTH_CLIENT_RESULT_FAILURE, NULL, 0);
}

void auth_request_internal_failure(struct auth_request *request)
{
	request->internal_failure = TRUE;
	auth_request_fail(request);
}

void auth_request_ref(struct auth_request *request)
{
	request->refcount++;
}

void auth_request_unref(struct auth_request **_request)
{
	struct auth_request *request = *_request;

	*_request = NULL;
	i_assert(request->refcount > 0);
	if (--request->refcount > 0)
		return;

	if (request->mech != NULL)
		request->mech->auth_free(request);
	else
		pool_unref(request->pool);
}

void auth_request_export(struct auth_request *request, string_t *str)
{
	str_append(str, "user=");
	str_append(str, request->user);
	str_append(str, "\tservice=");
	str_append(str, request->service);

        if (request->master_user != NULL) {
                str_append(str, "master_user=");
                str_append(str, request->master_user);
        }

	if (request->local_ip.family != 0) {
		str_append(str, "\tlip=");
		str_append(str, net_ip2addr(&request->local_ip));
	}
	if (request->remote_ip.family != 0) {
		str_append(str, "\trip=");
		str_append(str, net_ip2addr(&request->remote_ip));
	}
	if (request->secured)
		str_append(str, "\tsecured=1");
}

bool auth_request_import(struct auth_request *request,
			 const char *key, const char *value)
{
	if (strcmp(key, "user") == 0)
		request->user = p_strdup(request->pool, value);
	else if (strcmp(key, "master_user") == 0)
		request->master_user = p_strdup(request->pool, value);
	else if (strcmp(key, "cert_username") == 0) {
		if (request->auth->ssl_username_from_cert) {
			/* get username from SSL certificate. it overrides
			   the username given by the auth mechanism. */
			request->user = p_strdup(request->pool, value);
			request->cert_username = TRUE;
		}
	} else if (strcmp(key, "service") == 0)
		request->service = p_strdup(request->pool, value);
	else if (strcmp(key, "lip") == 0)
		net_addr2ip(value, &request->local_ip);
	else if (strcmp(key, "rip") == 0)
		net_addr2ip(value, &request->remote_ip);
	else if (strcmp(key, "secured") == 0)
		request->secured = TRUE;
	else
		return FALSE;

	return TRUE;
}

void auth_request_initial(struct auth_request *request,
			  const unsigned char *data, size_t data_size)
{
	i_assert(request->state == AUTH_REQUEST_STATE_NEW);

	request->state = AUTH_REQUEST_STATE_MECH_CONTINUE;
	request->mech->auth_initial(request, data, data_size);
}

void auth_request_continue(struct auth_request *request,
			   const unsigned char *data, size_t data_size)
{
	i_assert(request->state == AUTH_REQUEST_STATE_MECH_CONTINUE);

	request->last_access = ioloop_time;
	request->mech->auth_continue(request, data, data_size);
}

static void auth_request_save_cache(struct auth_request *request,
				    enum passdb_result result)
{
	struct passdb_module *passdb = request->passdb->passdb;
	const char *extra_fields;
	string_t *str;

	switch (result) {
	case PASSDB_RESULT_USER_UNKNOWN:
	case PASSDB_RESULT_PASSWORD_MISMATCH:
	case PASSDB_RESULT_OK:
	case PASSDB_RESULT_SCHEME_NOT_AVAILABLE:
		/* can be cached */
		break;
	case PASSDB_RESULT_USER_DISABLED:
	case PASSDB_RESULT_PASS_EXPIRED:
		/* FIXME: we can't cache this now, or cache lookup would
		   return success. */
		return;
	case PASSDB_RESULT_INTERNAL_FAILURE:
		i_unreached();
	}

	extra_fields = request->extra_fields == NULL ? NULL :
		auth_stream_reply_export(request->extra_fields);

	if (passdb_cache == NULL)
		return;

	if (passdb->cache_key == NULL)
		return;

	if (result < 0) {
		/* lookup failed. */
		if (result == PASSDB_RESULT_USER_UNKNOWN) {
			auth_cache_insert(passdb_cache, request,
					  passdb->cache_key, "", FALSE);
		}
		return;
	}

	if (!request->no_password && request->passdb_password == NULL) {
		/* passdb didn't provide the correct password */
		if (result != PASSDB_RESULT_OK ||
		    request->mech_password == NULL)
			return;

		/* we can still cache valid password lookups though.
		   strdup() it so that mech_password doesn't get
		   cleared too early. */
		request->passdb_password =
			p_strconcat(request->pool, "{plain}",
				    request->mech_password, NULL);
	}

	/* save all except the currently given password in cache */
	str = t_str_new(256);
	if (request->passdb_password != NULL) {
		if (*request->passdb_password != '{') {
			/* cached passwords must have a known scheme */
			str_append_c(str, '{');
			str_append(str, passdb->default_pass_scheme);
			str_append_c(str, '}');
		}
		if (strchr(request->passdb_password, '\t') != NULL)
			i_panic("%s: Password contains TAB", request->user);
		if (strchr(request->passdb_password, '\n') != NULL)
			i_panic("%s: Password contains LF", request->user);
		str_append(str, request->passdb_password);
	}

	if (extra_fields != NULL && *extra_fields != '\0') {
		str_append_c(str, '\t');
		str_append(str, extra_fields);
	}
	if (request->extra_cache_fields != NULL) {
		extra_fields =
			auth_stream_reply_export(request->extra_cache_fields);
		if (*extra_fields != '\0') {
			str_append_c(str, '\t');
			str_append(str, extra_fields);
		}
	}
	auth_cache_insert(passdb_cache, request, passdb->cache_key, str_c(str),
			  result == PASSDB_RESULT_OK);
}

static bool auth_request_master_lookup_finish(struct auth_request *request)
{
	if (request->passdb_failure)
		return TRUE;

	/* master login successful. update user and master_user variables. */
	auth_request_log_info(request, "passdb", "Master user logging in as %s",
			      request->requested_login_user);

	request->master_user = request->user;
	request->user = request->requested_login_user;
	request->requested_login_user = NULL;

	request->skip_password_check = TRUE;
	request->passdb_password = NULL;

	if (!request->passdb->pass) {
		/* skip the passdb lookup, we're authenticated now. */
		return TRUE;
	}

	/* the authentication continues with passdb lookup for the
	   requested_login_user. */
	request->passdb = request->auth->passdbs;
	return FALSE;
}

static bool
auth_request_handle_passdb_callback(enum passdb_result *result,
				    struct auth_request *request)
{
	if (request->passdb_password != NULL) {
		safe_memset(request->passdb_password, 0,
			    strlen(request->passdb_password));
	}

	if (request->passdb->deny && *result != PASSDB_RESULT_USER_UNKNOWN) {
		/* deny passdb. we can get through this step only if the
		   lookup returned that user doesn't exist in it. internal
		   errors are fatal here. */
		if (*result != PASSDB_RESULT_INTERNAL_FAILURE) {
			auth_request_log_info(request, "passdb",
					      "User found from deny passdb");
			*result = PASSDB_RESULT_USER_DISABLED;
		}
	} else if (*result == PASSDB_RESULT_OK) {
		/* success */
		if (request->requested_login_user != NULL) {
			/* this was a master user lookup. */
			if (!auth_request_master_lookup_finish(request))
				return FALSE;
		} else {
			if (request->passdb->pass) {
				/* this wasn't the final passdb lookup,
				   continue to next passdb */
				request->passdb = request->passdb->next;
				request->passdb_password = NULL;
				return FALSE;
			}
		}
	} else if (*result == PASSDB_RESULT_PASS_EXPIRED) {
	        if (request->extra_fields == NULL)
		        request->extra_fields = auth_stream_reply_init(request);
	        auth_stream_reply_add(request->extra_fields, "reason",
				      "Password expired");
	} else if (request->passdb->next != NULL &&
		   *result != PASSDB_RESULT_USER_DISABLED) {
		/* try next passdb. */
                request->passdb = request->passdb->next;
		request->passdb_password = NULL;

                if (*result == PASSDB_RESULT_INTERNAL_FAILURE) {
			/* remember that we have had an internal failure. at
			   the end return internal failure if we couldn't
			   successfully login. */
			request->passdb_internal_failure = TRUE;
		}
		if (request->extra_fields != NULL)
			auth_stream_reply_reset(request->extra_fields);

		return FALSE;
	} else if (request->passdb_internal_failure) {
		/* last passdb lookup returned internal failure. it may have
		   had the correct password, so return internal failure
		   instead of plain failure. */
		*result = PASSDB_RESULT_INTERNAL_FAILURE;
	}

	return TRUE;
}

static void
auth_request_verify_plain_callback_finish(enum passdb_result result,
					  struct auth_request *request)
{
	if (!auth_request_handle_passdb_callback(&result, request)) {
		/* try next passdb */
		auth_request_verify_plain(request, request->mech_password,
			request->private_callback.verify_plain);
	} else {
		auth_request_ref(request);
		request->private_callback.verify_plain(result, request);
		safe_memset(request->mech_password, 0,
			    strlen(request->mech_password));
		auth_request_unref(&request);
	}
}

void auth_request_verify_plain_callback(enum passdb_result result,
					struct auth_request *request)
{
	i_assert(request->state == AUTH_REQUEST_STATE_PASSDB);

	request->state = AUTH_REQUEST_STATE_MECH_CONTINUE;

	if (result != PASSDB_RESULT_INTERNAL_FAILURE)
		auth_request_save_cache(request, result);
	else {
		/* lookup failed. if we're looking here only because the
		   request was expired in cache, fallback to using cached
		   expired record. */
		const char *cache_key = request->passdb->passdb->cache_key;

		if (passdb_cache_verify_plain(request, cache_key,
					      request->mech_password,
					      &result, TRUE)) {
			auth_request_log_info(request, "passdb",
				"Fallbacking to expired data from cache");
		}
	}

	auth_request_verify_plain_callback_finish(result, request);
}

void auth_request_verify_plain(struct auth_request *request,
			       const char *password,
			       verify_plain_callback_t *callback)
{
	struct passdb_module *passdb;
	enum passdb_result result;
	const char *cache_key;

	i_assert(request->state == AUTH_REQUEST_STATE_MECH_CONTINUE);

        if (request->passdb == NULL) {
                /* no masterdbs, master logins not supported */
                i_assert(request->requested_login_user != NULL);
		auth_request_log_info(request, "passdb",
			"Attempted master login with no master passdbs");
		callback(PASSDB_RESULT_USER_UNKNOWN, request);
		return;
        }

        passdb = request->passdb->passdb;
	if (request->mech_password == NULL)
		request->mech_password = p_strdup(request->pool, password);
	else
		i_assert(request->mech_password == password);
	request->private_callback.verify_plain = callback;

	cache_key = passdb_cache == NULL ? NULL : passdb->cache_key;
	if (passdb_cache_verify_plain(request, cache_key, password,
				      &result, FALSE)) {
		auth_request_verify_plain_callback_finish(result, request);
		return;
	}

	request->state = AUTH_REQUEST_STATE_PASSDB;
	request->credentials_scheme = NULL;

	if (passdb->blocking)
		passdb_blocking_verify_plain(request);
	else {
		passdb->iface.verify_plain(request, password,
					   auth_request_verify_plain_callback);
	}
}

static void
auth_request_lookup_credentials_finish(enum passdb_result result,
				       const unsigned char *credentials,
				       size_t size,
				       struct auth_request *request)
{
	if (!auth_request_handle_passdb_callback(&result, request)) {
		/* try next passdb */
		auth_request_lookup_credentials(request,
			request->credentials_scheme,
                	request->private_callback.lookup_credentials);
	} else {
		if (request->auth->verbose_debug_passwords &&
		    result == PASSDB_RESULT_OK) {
			auth_request_log_debug(request, "password",
				"Credentials: %s",
				binary_to_hex(credentials, size));
		}
		request->private_callback.
			lookup_credentials(result, credentials, size, request);
	}
}

void auth_request_lookup_credentials_callback(enum passdb_result result,
					      const unsigned char *credentials,
					      size_t size,
					      struct auth_request *request)
{
	const char *cache_cred, *cache_scheme;

	i_assert(request->state == AUTH_REQUEST_STATE_PASSDB);

	request->state = AUTH_REQUEST_STATE_MECH_CONTINUE;

	if (result != PASSDB_RESULT_INTERNAL_FAILURE)
		auth_request_save_cache(request, result);
	else {
		/* lookup failed. if we're looking here only because the
		   request was expired in cache, fallback to using cached
		   expired record. */
		const char *cache_key = request->passdb->passdb->cache_key;

		if (passdb_cache_lookup_credentials(request, cache_key,
						    &cache_cred, &cache_scheme,
						    &result, TRUE)) {
			auth_request_log_info(request, "passdb",
				"Fallbacking to expired data from cache");
		}
		if (result == PASSDB_RESULT_OK) {
			if (!passdb_get_credentials(request, cache_cred,
						    cache_scheme,
						    &credentials, &size))
				result = PASSDB_RESULT_SCHEME_NOT_AVAILABLE;
		}
	}

	auth_request_lookup_credentials_finish(result, credentials, size,
					       request);
}

void auth_request_lookup_credentials(struct auth_request *request,
				     const char *scheme,
				     lookup_credentials_callback_t *callback)
{
	struct passdb_module *passdb = request->passdb->passdb;
	const char *cache_key, *cache_cred, *cache_scheme;
	const unsigned char *credentials;
	size_t size;
	enum passdb_result result;

	i_assert(request->state == AUTH_REQUEST_STATE_MECH_CONTINUE);

	request->credentials_scheme = p_strdup(request->pool, scheme);
	request->private_callback.lookup_credentials = callback;

	cache_key = passdb_cache == NULL ? NULL : passdb->cache_key;
	if (cache_key != NULL) {
		if (passdb_cache_lookup_credentials(request, cache_key,
						    &cache_cred, &cache_scheme,
						    &result, FALSE)) {
			if (result == PASSDB_RESULT_OK &&
			    !passdb_get_credentials(request, cache_cred,
						    cache_scheme,
						    &credentials, &size))
				result = PASSDB_RESULT_SCHEME_NOT_AVAILABLE;
			auth_request_lookup_credentials_finish(
				result, credentials, size, request);
			return;
		}
	}

	request->state = AUTH_REQUEST_STATE_PASSDB;

	if (passdb->blocking)
		passdb_blocking_lookup_credentials(request);
	else if (passdb->iface.lookup_credentials != NULL) {
		passdb->iface.lookup_credentials(request,
			auth_request_lookup_credentials_callback);
	} else {
		/* this passdb doesn't support credentials */
		auth_request_lookup_credentials_callback(
			PASSDB_RESULT_SCHEME_NOT_AVAILABLE, NULL, 0, request);
	}
}

void auth_request_set_credentials(struct auth_request *request,
				  const char *scheme, const char *data,
				  set_credentials_callback_t *callback)
{
	struct passdb_module *passdb = request->passdb->passdb;
	const char *cache_key, *new_credentials;

	cache_key = passdb_cache == NULL ? NULL : passdb->cache_key;
	if (cache_key != NULL)
		auth_cache_remove(passdb_cache, request, cache_key);

	request->private_callback.set_credentials = callback;

	new_credentials = t_strdup_printf("{%s}%s", scheme, data);
	if (passdb->blocking)
		passdb_blocking_set_credentials(request, new_credentials);
	else if (passdb->iface.set_credentials != NULL) {
		passdb->iface.set_credentials(request, new_credentials,
					      callback);
	} else {
		/* this passdb doesn't support credentials update */
		callback(PASSDB_RESULT_INTERNAL_FAILURE, request);
	}
}

static void auth_request_userdb_save_cache(struct auth_request *request,
					   enum userdb_result result)
{
	struct userdb_module *userdb = request->userdb->userdb;
	const char *str;

	if (passdb_cache == NULL || userdb->cache_key == NULL)
		return;

	str = result == USERDB_RESULT_USER_UNKNOWN ? "" :
		auth_stream_reply_export(request->userdb_reply);
	/* last_success has no meaning with userdb */
	auth_cache_insert(passdb_cache, request, userdb->cache_key, str, FALSE);
}

static bool auth_request_lookup_user_cache(struct auth_request *request,
					   const char *key,
					   struct auth_stream_reply **reply_r,
					   enum userdb_result *result_r,
					   bool use_expired)
{
	const char *value;
	struct auth_cache_node *node;
	bool expired;

	value = auth_cache_lookup(passdb_cache, request, key, &node,
				  &expired);
	if (value == NULL || (expired && !use_expired))
		return FALSE;

	if (*value == '\0') {
		/* negative cache entry */
		*result_r = USERDB_RESULT_USER_UNKNOWN;
		*reply_r = auth_stream_reply_init(request);
		return TRUE;
	}

	*result_r = USERDB_RESULT_OK;
	*reply_r = auth_stream_reply_init(request);
	auth_stream_reply_import(*reply_r, value);
	return TRUE;
}

void auth_request_userdb_callback(enum userdb_result result,
				  struct auth_request *request)
{
	struct userdb_module *userdb = request->userdb->userdb;

	if (result != USERDB_RESULT_OK && request->userdb->next != NULL) {
		/* try next userdb. */
		if (result == USERDB_RESULT_INTERNAL_FAILURE)
			request->userdb_internal_failure = TRUE;

		request->userdb = request->userdb->next;
		auth_request_lookup_user(request,
					 request->private_callback.userdb);
		return;
	}

	if (request->userdb_internal_failure && result != USERDB_RESULT_OK) {
		/* one of the userdb lookups failed. the user might have been
		   in there, so this is an internal failure */
		result = USERDB_RESULT_INTERNAL_FAILURE;
	} else if (result == USERDB_RESULT_USER_UNKNOWN &&
		   request->client_pid != 0) {
		/* this was an actual login attempt, the user should
		   have been found. */
		auth_request_log_error(request, "userdb",
				       "user not found from userdb");
	}

	if (result != USERDB_RESULT_INTERNAL_FAILURE)
		auth_request_userdb_save_cache(request, result);
	else if (passdb_cache != NULL && userdb->cache_key != NULL) {
		/* lookup failed. if we're looking here only because the
		   request was expired in cache, fallback to using cached
		   expired record. */
		const char *cache_key = userdb->cache_key;
		struct auth_stream_reply *reply;

		if (auth_request_lookup_user_cache(request, cache_key, &reply,
						   &result, TRUE)) {
			request->userdb_reply = reply;
			auth_request_log_info(request, "userdb",
				"Fallbacking to expired data from cache");
		}
	}

        request->private_callback.userdb(result, request);
}

void auth_request_lookup_user(struct auth_request *request,
			      userdb_callback_t *callback)
{
	struct userdb_module *userdb = request->userdb->userdb;
	const char *cache_key;

	request->private_callback.userdb = callback;
	request->userdb_lookup = TRUE;

	/* (for now) auth_cache is shared between passdb and userdb */
	cache_key = passdb_cache == NULL ? NULL : userdb->cache_key;
	if (cache_key != NULL) {
		struct auth_stream_reply *reply;
		enum userdb_result result;

		if (auth_request_lookup_user_cache(request, cache_key, &reply,
						   &result, FALSE)) {
			request->userdb_reply = reply;
			request->private_callback.userdb(result, request);
			return;
		}
	}

	if (userdb->blocking)
		userdb_blocking_lookup(request);
	else
		userdb->iface->lookup(request, auth_request_userdb_callback);
}

static char *
auth_request_fix_username(struct auth_request *request, const char *username,
                          const char **error_r)
{
	unsigned char *p;
        char *user;

	if (strchr(username, '@') == NULL &&
	    request->auth->default_realm != NULL) {
		user = p_strconcat(request->pool, username, "@",
                                   request->auth->default_realm, NULL);
	} else {
		user = p_strdup(request->pool, username);
	}

        for (p = (unsigned char *)user; *p != '\0'; p++) {
		if (request->auth->username_translation[*p & 0xff] != 0)
			*p = request->auth->username_translation[*p & 0xff];
		if (request->auth->username_chars[*p & 0xff] == 0) {
			*error_r = t_strdup_printf(
				"Username contains disallowed character: "
				"0x%02x", *p);
			return NULL;
		}
	}

	if (request->auth->username_format != NULL) {
		/* username format given, put it through variable expansion.
		   we'll have to temporarily replace request->user to get
		   %u to be the wanted username */
		const struct var_expand_table *table;
		char *old_username;
		string_t *dest;

		old_username = request->user;
		request->user = user;

		t_push();
		dest = t_str_new(256);
		table = auth_request_get_var_expand_table(request,
						auth_request_str_escape);
		var_expand(dest, request->auth->username_format, table);
		user = p_strdup(request->pool, str_c(dest));
		t_pop();

		request->user = old_username;
	}

        return user;
}

bool auth_request_set_username(struct auth_request *request,
			       const char *username, const char **error_r)
{
	const char *p, *login_username = NULL;

	if (request->original_username == NULL) {
		/* the username may change later, but we need to use this
		   username when verifying at least DIGEST-MD5 password */
		request->original_username = p_strdup(request->pool, username);
	}
	if (request->cert_username) {
		/* cert_username overrides the username given by
		   authentication mechanism. */
		return TRUE;
	}

	if (request->auth->master_user_separator != '\0' &&
	    !request->userdb_lookup) {
		/* check if the username contains a master user */
		p = strchr(username, request->auth->master_user_separator);
		if (p != NULL) {
			/* it does, set it. */
			login_username = t_strdup_until(username, p);

			/* username is the master user */
			username = p + 1;
		}
	}

	if (*username == '\0') {
		/* Some PAM plugins go nuts with empty usernames */
		*error_r = "Empty username";
		return FALSE;
	}

        request->user = auth_request_fix_username(request, username, error_r);
	if (request->user == NULL) {
		auth_request_log_debug(request, "auth",
			"Invalid username: %s", str_sanitize(username, 128));
		return FALSE;
	}

	if (login_username != NULL) {
		if (!auth_request_set_login_username(request,
						     login_username,
						     error_r))
			return FALSE;
	}
	return TRUE;
}

bool auth_request_set_login_username(struct auth_request *request,
                                     const char *username,
                                     const char **error_r)
{
        i_assert(*username != '\0');

	if (strcmp(username, request->user) == 0) {
		/* The usernames are the same, we don't really wish to log
		   in as someone else */
		return TRUE;
	}

        /* lookup request->user from masterdb first */
        request->passdb = request->auth->masterdbs;

        request->requested_login_user =
                auth_request_fix_username(request, username, error_r);
	return request->requested_login_user != NULL;
}

static int is_ip_in_network(const char *network, const struct ip_addr *ip)
{
	const uint32_t *ip1, *ip2;
	struct ip_addr src_ip, net_ip;
	const char *p;
	unsigned int max_bits, bits, pos, i;
	uint32_t mask;

	if (net_ipv6_mapped_ipv4_convert(ip, &src_ip) == 0)
		ip = &src_ip;

	max_bits = IPADDR_IS_V4(ip) ? 32 : 128;
	p = strchr(network, '/');
	if (p == NULL) {
		/* full IP address must match */
		bits = max_bits;
	} else {
		/* get the network mask */
		network = t_strdup_until(network, p);
		bits = strtoul(p+1, NULL, 10);
		if (bits > max_bits)
			bits = max_bits;
	}

	if (net_addr2ip(network, &net_ip) < 0)
		return -1;

	if (IPADDR_IS_V4(ip) != IPADDR_IS_V4(&net_ip)) {
		/* one is IPv6 and one is IPv4 */
		return 0;
	}
	i_assert(IPADDR_IS_V6(ip) == IPADDR_IS_V6(&net_ip));

	ip1 = (const uint32_t *)&ip->ip;
	ip2 = (const uint32_t *)&net_ip.ip;

	/* check first the full 32bit ints */
	for (pos = 0, i = 0; pos + 32 <= bits; pos += 32, i++) {
		if (ip1[i] != ip2[i])
			return 0;
	}

	/* check the last full bytes */
	for (mask = 0xff; pos + 8 <= bits; pos += 8, mask <<= 8) {
		if ((ip1[i] & mask) != (ip2[i] & mask))
			return 0;
	}

	/* check the last bits, they're reversed in bytes */
	bits -= pos;
	for (mask = 0x80 << (pos % 32); bits > 0; bits--, mask >>= 1) {
		if ((ip1[i] & mask) != (ip2[i] & mask))
			return 0;
	}
	return 1;
}

static void auth_request_validate_networks(struct auth_request *request,
					   const char *networks)
{
	const char *const *net;
	bool found = FALSE;

	if (request->remote_ip.family == 0) {
		/* IP not known */
		auth_request_log_info(request, "passdb",
			"allow_nets check failed: Remote IP not known");
		request->passdb_failure = TRUE;
		return;
	}

	t_push();
	for (net = t_strsplit_spaces(networks, ", "); *net != NULL; net++) {
		auth_request_log_debug(request, "auth",
			"allow_nets: Matching for network %s", *net);
		switch (is_ip_in_network(*net, &request->remote_ip)) {
		case 1:
			found = TRUE;
			break;
		case -1:
			auth_request_log_info(request, "passdb",
				"allow_nets: Invalid network '%s'", *net);
			break;
		default:
			break;
		}
	}
	t_pop();

	if (!found) {
		auth_request_log_info(request, "passdb",
			"allow_nets check failed: IP not in allowed networks");
	}
	request->passdb_failure = !found;
}

void auth_request_set_field(struct auth_request *request,
			    const char *name, const char *value,
			    const char *default_scheme)
{
	i_assert(*name != '\0');
	i_assert(value != NULL);

	if (strcmp(name, "password") == 0) {
		if (request->passdb_password != NULL) {
			auth_request_log_error(request,
				request->passdb->passdb->iface.name,
				"Multiple password values not supported");
			return;
		}

		if (*value == '{') {
			request->passdb_password =
				p_strdup(request->pool, value);
		} else {
			i_assert(default_scheme != NULL);
			request->passdb_password =
				p_strdup_printf(request->pool, "{%s}%s",
						default_scheme, value);
		}
		return;
	}

	if (strcmp(name, "user") == 0) {
		/* update username to be exactly as it's in database */
		if (strcmp(request->user, value) != 0) {
			/* remember the original username for cache */
			if (request->original_username == NULL) {
				request->original_username =
					p_strdup(request->pool, request->user);
			}

			auth_request_log_debug(request, "auth",
				"username changed %s -> %s",
				request->user, value);
			request->user = p_strdup(request->pool, value);
		}
	} else if (strcmp(name, "nodelay") == 0) {
		/* don't delay replying to client of the failure */
		request->no_failure_delay = TRUE;
	} else if (strcmp(name, "nopassword") == 0) {
		/* NULL password - anything goes */
		const char *password = request->passdb_password;

		if (password != NULL) {
			(void)password_get_scheme(&password);
			if (*password != '\0') {
				auth_request_log_error(request,
					request->passdb->passdb->iface.name,
					"nopassword set but password is "
					"non-empty");
				return;
			}
		}
		request->no_password = TRUE;
		request->passdb_password = NULL;
	} else if (strcmp(name, "allow_nets") == 0) {
		auth_request_validate_networks(request, value);
	} else if (strncmp(name, "userdb_", 7) == 0) {
		/* for prefetch userdb */
		if (request->userdb_reply == NULL)
			auth_request_init_userdb_reply(request);
		auth_request_set_userdb_field(request, name + 7, value);
	} else {
		if (strcmp(name, "nologin") == 0) {
			/* user can't actually login - don't keep this
			   reply for master */
			request->no_login = TRUE;
			value = NULL;
		} else if (strcmp(name, "proxy") == 0) {
			/* we're proxying authentication for this user. send
			   password back if using plaintext authentication. */
			request->proxy = TRUE;
			request->no_login = TRUE;
			value = NULL;
		}

		if (request->extra_fields == NULL)
			request->extra_fields = auth_stream_reply_init(request);
		auth_stream_reply_add(request->extra_fields, name, value);
		return;
	}

	if (passdb_cache != NULL &&
	    request->passdb->passdb->cache_key != NULL) {
		/* we'll need to get this field stored into cache */
		if (request->extra_cache_fields == NULL) {
			request->extra_cache_fields =
				auth_stream_reply_init(request);
		}
		auth_stream_reply_add(request->extra_cache_fields, name, value);
	}
}

void auth_request_set_fields(struct auth_request *request,
			     const char *const *fields,
			     const char *default_scheme)
{
	const char *key, *value;

	t_push();
	for (; *fields != NULL; fields++) {
		if (**fields == '\0')
			continue;

		value = strchr(*fields, '=');
		if (value == NULL) {
			key = *fields;
			value = "";
		} else {
			key = t_strdup_until(*fields, value);
			value++;
		}
		auth_request_set_field(request, key, value, default_scheme);
	}
	t_pop();
}

void auth_request_init_userdb_reply(struct auth_request *request)
{
	request->userdb_reply = auth_stream_reply_init(request);
	auth_stream_reply_add(request->userdb_reply, NULL, request->user);
}

static void
auth_request_change_userdb_user(struct auth_request *request, const char *user)
{
	const char *str;

	/* replace the username in userdb_reply if it changed */
	if (strcmp(user, request->user) == 0)
		return;

	t_push();
	str = t_strdup(auth_stream_reply_export(request->userdb_reply));

	/* reset the reply and add the new username */
	auth_request_set_field(request, "user", user, NULL);
	auth_stream_reply_reset(request->userdb_reply);
	auth_stream_reply_add(request->userdb_reply,
			      NULL, request->user);

	/* add the rest */
	str = strchr(str, '\t');
	i_assert(str != NULL);
	auth_stream_reply_import(request->userdb_reply, str + 1);
	t_pop();
}

static void auth_request_set_uidgid_file(struct auth_request *request,
					 const char *path_template)
{
	string_t *path;
	struct stat st;

	t_push();
	path = t_str_new(256);
	var_expand(path, path_template,
		   auth_request_get_var_expand_table(request, NULL));
	if (stat(str_c(path), &st) < 0) {
		auth_request_log_error(request, "uidgid_file",
				       "stat(%s) failed: %m", str_c(path));
	} else {
		auth_stream_reply_add(request->userdb_reply,
				      "uid", dec2str(st.st_uid));
		auth_stream_reply_add(request->userdb_reply,
				      "gid", dec2str(st.st_gid));
	}
	t_pop();
}

void auth_request_set_userdb_field(struct auth_request *request,
				   const char *name, const char *value)
{
	uid_t uid;
	gid_t gid;

	if (strcmp(name, "uid") == 0) {
		uid = userdb_parse_uid(request, value);
		if (uid == (uid_t)-1) {
			request->userdb_lookup_failed = TRUE;
			return;
		}
		value = dec2str(uid);
	} else if (strcmp(name, "gid") == 0) {
		gid = userdb_parse_gid(request, value);
		if (gid == (gid_t)-1) {
			request->userdb_lookup_failed = TRUE;
			return;
		}
		value = dec2str(gid);
	} else if (strcmp(name, "user") == 0) {
		auth_request_change_userdb_user(request, value);
		return;
	} else if (strcmp(name, "uidgid_file") == 0) {
		auth_request_set_uidgid_file(request, value);
		return;
	}

	auth_stream_reply_add(request->userdb_reply, name, value);
}

void auth_request_set_userdb_field_values(struct auth_request *request,
					  const char *name,
					  const char *const *values)
{
	if (*values == NULL)
		return;

	if (strcmp(name, "uid") == 0) {
		/* there can be only one. use the first one. */
		auth_request_set_userdb_field(request, name, *values);
	} else if (strcmp(name, "gid") == 0) {
		/* convert gids to comma separated list */
		string_t *value;
		gid_t gid;

		t_push();
		value = t_str_new(128);
		for (; *values != NULL; values++) {
			gid = userdb_parse_gid(request, *values);
			if (gid == (gid_t)-1) {
				request->userdb_lookup_failed = TRUE;
				t_pop();
				return;
			}

			if (str_len(value) > 0)
				str_append_c(value, ',');
			str_append(value, dec2str(gid));
		}
		auth_stream_reply_add(request->userdb_reply, name,
				      str_c(value));
		t_pop();
	} else {
		/* add only one */
		auth_request_set_userdb_field(request, name, *values);
	}
}

int auth_request_password_verify(struct auth_request *request,
				 const char *plain_password,
				 const char *crypted_password,
				 const char *scheme, const char *subsystem)
{
	const unsigned char *raw_password;
	size_t raw_password_size;
	const char *user;
	int ret;

	if (request->skip_password_check) {
		/* currently this can happen only with master logins */
		i_assert(request->master_user != NULL);
		return 1;
	}

	if (request->passdb->deny) {
		/* this is a deny database, we don't care about the password */
		return 0;
	}

	if (request->no_password) {
		auth_request_log_info(request, subsystem, "No password");
		return 1;
	}

	ret = password_decode(crypted_password, scheme,
			      &raw_password, &raw_password_size);
	if (ret <= 0) {
		if (ret < 0) {
			auth_request_log_error(request, subsystem,
				"Invalid password format for scheme %s",
				scheme);
		} else {
			auth_request_log_error(request, subsystem,
					       "Unknown scheme %s", scheme);
		}
		return -1;
	}

	/* If original_username is set, use it. It may be important for some
	   password schemes (eg. digest-md5). Otherwise the username is used
	   only for logging purposes. */
	user = request->original_username != NULL ?
		request->original_username : request->user;
	ret = password_verify(plain_password, user, scheme,
			      raw_password, raw_password_size);
	i_assert(ret >= 0);
	if (ret == 0) {
		auth_request_log_info(request, subsystem,
				      "Password mismatch");
		if (request->auth->verbose_debug_passwords) {
			auth_request_log_debug(request, subsystem,
					       "%s(%s) != '%s'", scheme,
					       plain_password,
					       crypted_password);
		}
	}
	return ret;
}

static const char *
escape_none(const char *string,
	    const struct auth_request *request __attr_unused__)
{
	return string;
}

const char *
auth_request_str_escape(const char *string,
			const struct auth_request *request __attr_unused__)
{
	return str_escape(string);
}

const struct var_expand_table *
auth_request_get_var_expand_table(const struct auth_request *auth_request,
				  auth_request_escape_func_t *escape_func)
{
	static struct var_expand_table static_tab[] = {
		{ 'u', NULL },
		{ 'n', NULL },
		{ 'd', NULL },
		{ 's', NULL },
		{ 'h', NULL },
		{ 'l', NULL },
		{ 'r', NULL },
		{ 'p', NULL },
		{ 'w', NULL },
		{ '!', NULL },
		{ 'm', NULL },
		{ 'c', NULL },
		{ '\0', NULL }
	};
	struct var_expand_table *tab;

	if (escape_func == NULL)
		escape_func = escape_none;

	tab = t_malloc(sizeof(static_tab));
	memcpy(tab, static_tab, sizeof(static_tab));

	tab[0].value = escape_func(auth_request->user, auth_request);
	tab[1].value = escape_func(t_strcut(auth_request->user, '@'),
				   auth_request);
	tab[2].value = strchr(auth_request->user, '@');
	if (tab[2].value != NULL)
		tab[2].value = escape_func(tab[2].value+1, auth_request);
	tab[3].value = auth_request->service;
	/* tab[4] = we have no home dir */
	if (auth_request->local_ip.family != 0)
		tab[5].value = net_ip2addr(&auth_request->local_ip);
	if (auth_request->remote_ip.family != 0)
		tab[6].value = net_ip2addr(&auth_request->remote_ip);
	tab[7].value = dec2str(auth_request->client_pid);
	if (auth_request->mech_password != NULL) {
		tab[8].value = escape_func(auth_request->mech_password,
					   auth_request);
	}
	if (auth_request->userdb_lookup) {
		tab[9].value = auth_request->userdb == NULL ? "" :
			dec2str(auth_request->userdb->num);
	} else {
		tab[9].value = auth_request->passdb == NULL ? "" :
			dec2str(auth_request->passdb->id);
	}
	tab[10].value = auth_request->mech == NULL ? "" :
		auth_request->mech->mech_name;
	tab[11].value = auth_request->secured ? "secured" : "";
	return tab;
}

static const char * __attr_format__(3, 0)
get_log_str(struct auth_request *auth_request, const char *subsystem,
	    const char *format, va_list va)
{
#define MAX_LOG_USERNAME_LEN 64
	const char *ip;
	string_t *str;

	str = t_str_new(128);
	str_append(str, subsystem);
	str_append_c(str, '(');

	if (auth_request->user == NULL)
		str_append(str, "?");
	else {
		str_sanitize_append(str, auth_request->user,
				    MAX_LOG_USERNAME_LEN);
	}

	ip = net_ip2addr(&auth_request->remote_ip);
	if (ip != NULL) {
		str_append_c(str, ',');
		str_append(str, ip);
	}
	if (auth_request->requested_login_user != NULL)
		str_append(str, ",master");
	str_append(str, "): ");
	str_vprintfa(str, format, va);
	return str_c(str);
}

void auth_request_log_debug(struct auth_request *auth_request,
			    const char *subsystem,
			    const char *format, ...)
{
	va_list va;

	if (!auth_request->auth->verbose_debug)
		return;

	va_start(va, format);
	t_push();
	i_info("%s", get_log_str(auth_request, subsystem, format, va));
	t_pop();
	va_end(va);
}

void auth_request_log_info(struct auth_request *auth_request,
			   const char *subsystem,
			   const char *format, ...)
{
	va_list va;

	if (!auth_request->auth->verbose)
		return;

	va_start(va, format);
	t_push();
	i_info("%s", get_log_str(auth_request, subsystem, format, va));
	t_pop();
	va_end(va);
}

void auth_request_log_error(struct auth_request *auth_request,
			    const char *subsystem,
			    const char *format, ...)
{
	va_list va;

	va_start(va, format);
	t_push();
	i_error("%s", get_log_str(auth_request, subsystem, format, va));
	t_pop();
	va_end(va);
}
