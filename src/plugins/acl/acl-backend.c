/* Copyright (c) 2006-2009 Dovecot authors, see the included COPYING file */

#include "lib.h"
#include "hash.h"
#include "acl-cache.h"
#include "acl-api-private.h"

#include <stdlib.h>

extern struct acl_backend_vfuncs acl_backend_vfile;

static const char *const owner_mailbox_rights[] = {
	MAIL_ACL_LOOKUP,
	MAIL_ACL_READ,
	MAIL_ACL_WRITE,
	MAIL_ACL_WRITE_SEEN,
	MAIL_ACL_WRITE_DELETED,
	MAIL_ACL_INSERT,
	MAIL_ACL_POST,
	MAIL_ACL_EXPUNGE,
	MAIL_ACL_CREATE,
	MAIL_ACL_DELETE,
	MAIL_ACL_ADMIN,
	NULL
};

static const char *const non_owner_mailbox_rights[] = { NULL };

struct acl_backend *
acl_backend_init(const char *data, struct mailbox_list *list,
		 const char *acl_username, const char *const *groups,
		 bool owner)
{
	struct acl_backend *backend;
	unsigned int i, group_count;
	bool debug;

	debug = getenv("DEBUG") != NULL;
	if (debug) {
		i_info("acl: initializing backend with data: %s", data);
		i_info("acl: acl username = %s", acl_username);
		i_info("acl: owner = %d", owner);
	}

	group_count = str_array_length(groups);

	if (strncmp(data, "vfile:", 6) == 0)
		data += 6;
	else if (strcmp(data, "vfile") == 0)
		data = "";
	else
		i_fatal("Unknown ACL backend: %s", t_strcut(data, ':'));

	backend = acl_backend_vfile.alloc();
	backend->debug = debug;
	backend->v = acl_backend_vfile;
	backend->list = list;
	backend->username = p_strdup(backend->pool, acl_username);
	backend->owner = owner;

	if (group_count > 0) {
		backend->group_count = group_count;
		backend->groups =
			p_new(backend->pool, const char *, group_count);
		for (i = 0; i < group_count; i++)
			backend->groups[i] = p_strdup(backend->pool, groups[i]);
		qsort(backend->groups, group_count, sizeof(const char *),
		      i_strcmp_p);
	}

	T_BEGIN {
		if (acl_backend_vfile.init(backend, data) < 0)
			i_fatal("acl: backend vfile init failed with data: %s",
				data);
	} T_END;

	backend->default_rights = owner ? owner_mailbox_rights :
		non_owner_mailbox_rights;
	backend->default_aclmask =
		acl_cache_mask_init(backend->cache, backend->pool,
				    backend->default_rights);

	backend->default_aclobj = acl_object_init_from_name(backend, NULL, "");
	return backend;
}

void acl_backend_deinit(struct acl_backend **_backend)
{
	struct acl_backend *backend = *_backend;

	*_backend = NULL;

	acl_object_deinit(&backend->default_aclobj);
	acl_cache_deinit(&backend->cache);
	backend->v.deinit(backend);
}

bool acl_backend_user_is_authenticated(struct acl_backend *backend)
{
	return backend->username != NULL;
}

bool acl_backend_user_is_owner(struct acl_backend *backend)
{
	return backend->owner;
}

bool acl_backend_user_name_equals(struct acl_backend *backend,
				  const char *username)
{
	if (backend->username == NULL) {
		/* anonymous user never matches */
		return FALSE;
	}

	return strcmp(backend->username, username) == 0;
}

bool acl_backend_user_is_in_group(struct acl_backend *backend,
				  const char *group_name)
{
	return bsearch(group_name, backend->groups, backend->group_count,
		       sizeof(const char *), bsearch_strcmp) != NULL;
}

unsigned int acl_backend_lookup_right(struct acl_backend *backend,
				      const char *right)
{
	return acl_cache_right_lookup(backend->cache, right);
}

int acl_backend_get_default_rights(struct acl_backend *backend,
				   const struct acl_mask **mask_r)
{
	if (backend->v.object_refresh_cache(backend->default_aclobj) < 0)
		return -1;

	*mask_r = acl_cache_get_my_rights(backend->cache, "");
	if (*mask_r == NULL)
		*mask_r = backend->default_aclmask;
	return 0;
}
