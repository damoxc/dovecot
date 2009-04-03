/* Copyright (c) 2006-2009 Dovecot authors, see the included COPYING file */

#include "lib.h"
#include "array.h"
#include "str.h"
#include "imap-match.h"
#include "mailbox-tree.h"
#include "mail-namespace.h"
#include "mailbox-list-private.h"
#include "acl-api-private.h"
#include "acl-cache.h"
#include "acl-shared-storage.h"
#include "acl-plugin.h"

#define ACL_LIST_CONTEXT(obj) \
	MODULE_CONTEXT(obj, acl_mailbox_list_module)

#define MAILBOX_FLAG_MATCHED 0x40000000

struct acl_mailbox_list {
	union mailbox_list_module_context module_ctx;

	struct acl_storage_rights_context rights;
};

struct acl_mailbox_list_iterate_context {
	struct mailbox_list_iterate_context ctx;
	struct mailbox_list_iterate_context *super_ctx;

	struct mailbox_tree_context *lookup_boxes;
	struct mailbox_info info;

	struct imap_match_glob *glob;
	char sep;
	unsigned int simple_star_glob:1;
};

static MODULE_CONTEXT_DEFINE_INIT(acl_mailbox_list_module,
				  &mailbox_list_module_register);

struct acl_backend *acl_mailbox_list_get_backend(struct mailbox_list *list)
{
	struct acl_mailbox_list *alist = ACL_LIST_CONTEXT(list);

	return alist->rights.backend;
}

static int
acl_mailbox_list_have_right(struct mailbox_list *list, const char *name,
			    unsigned int acl_storage_right_idx, bool *can_see_r)
{
	struct acl_mailbox_list *alist = ACL_LIST_CONTEXT(list);
	int ret;

	ret = acl_storage_rights_ctx_have_right(&alist->rights, name, FALSE,
						acl_storage_right_idx,
						can_see_r);
	if (ret < 0)
		mailbox_list_set_internal_error(list);
	return ret;
}

static void
acl_mailbox_try_list_fast(struct acl_mailbox_list_iterate_context *ctx)
{
	struct acl_mailbox_list *alist = ACL_LIST_CONTEXT(ctx->ctx.list);
	struct acl_backend *backend = alist->rights.backend;
	const unsigned int *idxp;
	const struct acl_mask *acl_mask;
	struct acl_mailbox_list_context *nonowner_list_ctx;
	struct mail_namespace *ns = ctx->ctx.list->ns;
	struct mailbox_list_iter_update_context update_ctx;
	const char *name;
	string_t *vname = NULL;
	int ret;

	if ((ctx->ctx.flags & (MAILBOX_LIST_ITER_RAW_LIST |
			       MAILBOX_LIST_ITER_SELECT_SUBSCRIBED)) != 0)
		return;

	/* if this namespace's default rights contain LOOKUP, we'll need to
	   go through all mailboxes in any case. */
	idxp = alist->rights.acl_storage_right_idx + ACL_STORAGE_RIGHT_LOOKUP;
	if (acl_backend_get_default_rights(backend, &acl_mask) < 0 ||
	    acl_cache_mask_isset(acl_mask, *idxp))
		return;

	/* no LOOKUP right by default, we can optimize this */
	if ((ctx->ctx.flags & MAILBOX_LIST_ITER_VIRTUAL_NAMES) != 0)
		vname = t_str_new(256);

	memset(&update_ctx, 0, sizeof(update_ctx));
	update_ctx.iter_ctx = &ctx->ctx;
	update_ctx.glob =
		imap_match_init(pool_datastack_create(), "*",
				(ns->flags & NAMESPACE_FLAG_INBOX) != 0,
				ctx->sep);
	update_ctx.match_parents = TRUE;
	update_ctx.tree_ctx = mailbox_tree_init(ctx->sep);

	nonowner_list_ctx = acl_backend_nonowner_lookups_iter_init(backend);
	while ((ret = acl_backend_nonowner_lookups_iter_next(nonowner_list_ctx,
							     &name)) > 0) {
		if (vname != NULL)
			name = mail_namespace_get_vname(ns, vname, name);
		mailbox_list_iter_update(&update_ctx, name);
	}
	acl_backend_nonowner_lookups_iter_deinit(&nonowner_list_ctx);

	if (ret == 0)
		ctx->lookup_boxes = update_ctx.tree_ctx;
	else
		mailbox_tree_deinit(&update_ctx.tree_ctx);
}

static struct mailbox_list_iterate_context *
acl_mailbox_list_iter_init_shared(struct mailbox_list *list,
				  const char *const *patterns,
				  enum mailbox_list_iter_flags flags)
{
	struct acl_mailbox_list *alist = ACL_LIST_CONTEXT(list);
	struct mailbox_list_iterate_context *ctx;
	int ret;

	/* before listing anything add namespaces for all users
	   who may have visible mailboxes */
	ret = acl_shared_namespaces_add(list->ns);

	ctx = alist->module_ctx.super.iter_init(list, patterns, flags);
	if (ret < 0)
		ctx->failed = TRUE;
	return ctx;
}

static struct mailbox_list_iterate_context *
acl_mailbox_list_iter_init(struct mailbox_list *list,
			   const char *const *patterns,
			   enum mailbox_list_iter_flags flags)
{
	struct acl_mailbox_list *alist = ACL_LIST_CONTEXT(list);
	struct acl_mailbox_list_iterate_context *ctx;
	const char *p;
	unsigned int i;
	bool inboxcase;

	ctx = i_new(struct acl_mailbox_list_iterate_context, 1);
	ctx->ctx.list = list;
	ctx->ctx.flags = flags;

	inboxcase = (list->ns->flags & NAMESPACE_FLAG_INBOX) != 0;
	ctx->sep = (ctx->ctx.flags & MAILBOX_LIST_ITER_VIRTUAL_NAMES) != 0 ?
		list->ns->sep : list->ns->real_sep;
	ctx->glob = imap_match_init_multiple(default_pool, patterns,
					     inboxcase, ctx->sep);
	/* see if all patterns have only a single '*' and it's at the end.
	   we can use it to do some optimizations. */
	ctx->simple_star_glob = TRUE;
	for (i = 0; patterns[i] != NULL; i++) {
		p = strchr(patterns[i], '*');
		if (p == NULL || p[1] != '\0') {
			ctx->simple_star_glob = FALSE;
			break;
		}
	}

	/* Try to avoid reading ACLs from all mailboxes by getting a smaller
	   list of mailboxes that have even potential to be visible. If we
	   couldn't get such a list, we'll go through all mailboxes. */
	T_BEGIN {
		acl_mailbox_try_list_fast(ctx);
	} T_END;
	ctx->super_ctx = alist->module_ctx.super.
		iter_init(list, patterns, flags);
	return &ctx->ctx;
}

static const struct mailbox_info *
acl_mailbox_list_iter_next_info(struct acl_mailbox_list_iterate_context *ctx)
{
	struct acl_mailbox_list *alist = ACL_LIST_CONTEXT(ctx->ctx.list);
	const struct mailbox_info *info;

	do {
		info = alist->module_ctx.super.iter_next(ctx->super_ctx);
		if (info == NULL)
			return NULL;
		/* if we've a list of mailboxes with LOOKUP rights, skip the
		   mailboxes not in the list (since we know they can't be
		   visible to us). */
	} while (ctx->lookup_boxes != NULL &&
		 mailbox_tree_lookup(ctx->lookup_boxes, info->name) == NULL);

	return info;
}

static const char *
acl_mailbox_list_iter_get_name(struct mailbox_list_iterate_context *ctx,
			       const char *name)
{
	struct mail_namespace *ns = ctx->list->ns;

	if ((ctx->flags & MAILBOX_LIST_ITER_VIRTUAL_NAMES) == 0)
		return name;

	/* Mailbox names contain namespace prefix,
	   except when listing INBOX. */
	if (strncmp(name, ns->prefix, ns->prefix_len) == 0)
		name += ns->prefix_len;
	return mail_namespace_fix_sep(ns, name);
}

static bool
iter_is_listing_all_children(struct acl_mailbox_list_iterate_context *ctx)
{
	const char *child;

	/* If all patterns (with '.' separator) are in "name*", "name.*" or
	   "%.*" style format, simple_star_glob=TRUE and we can easily test
	   this by simply checking if name/child mailbox matches. */
	child = t_strdup_printf("%s%cx", ctx->info.name, ctx->sep);
	return ctx->simple_star_glob &&
		imap_match(ctx->glob, child) == IMAP_MATCH_YES;
}

static bool
iter_mailbox_has_visible_children(struct acl_mailbox_list_iterate_context *ctx,
				  bool only_nonpatterns)
{
	struct mailbox_list_iterate_context *iter;
	const struct mailbox_info *info;
	enum mailbox_list_iter_flags flags;
	string_t *pattern;
	const char *prefix;
	unsigned int i, prefix_len;
	bool stars = FALSE, ret = FALSE;

	/* do we have child mailboxes with LOOKUP right that don't match
	   the list pattern? */
	if (ctx->lookup_boxes != NULL) {
		/* we have a list of mailboxes with LOOKUP rights. before
		   starting the slow list iteration, check check first
		   if there even are any children with LOOKUP rights. */
		struct mailbox_node *node;

		node = mailbox_tree_lookup(ctx->lookup_boxes, ctx->info.name);
		i_assert(node != NULL);
		if (node->children == NULL)
			return FALSE;
	}

	/* if mailbox name has '*' characters in it, they'll conflict with the
	   LIST wildcard. replace then with '%' and verify later that all
	   results have the correct prefix. */
	pattern = t_str_new(128);
	for (i = 0; ctx->info.name[i] != '\0'; i++) {
		if (ctx->info.name[i] != '*')
			str_append_c(pattern, ctx->info.name[i]);
		else {
			stars = TRUE;
			str_append_c(pattern, '%');
		}
	}
	str_append_c(pattern, ctx->sep);
	str_append_c(pattern, '*');
	prefix = str_c(pattern);
	prefix_len = str_len(pattern) - 1;

	flags = (ctx->ctx.flags & MAILBOX_LIST_ITER_VIRTUAL_NAMES) |
		MAILBOX_LIST_ITER_RETURN_NO_FLAGS;
	iter = mailbox_list_iter_init(ctx->ctx.list, str_c(pattern), flags);
	while ((info = mailbox_list_iter_next(iter)) != NULL) {
		if (only_nonpatterns &&
		    imap_match(ctx->glob, info->name) == IMAP_MATCH_YES) {
			/* at least one child matches also the original list
			   patterns. we don't need to show this mailbox. */
			ret = FALSE;
			break;
		}
		if (!stars || strncmp(info->name, prefix, prefix_len) == 0)
			ret = TRUE;
	}
	(void)mailbox_list_iter_deinit(&iter);
	return ret;
}

static int
acl_mailbox_list_info_is_visible(struct acl_mailbox_list_iterate_context *ctx)
{
#define PRESERVE_MAILBOX_FLAGS (MAILBOX_SUBSCRIBED | MAILBOX_CHILD_SUBSCRIBED)
	struct mailbox_info *info = &ctx->info;
	const char *acl_name;
	int ret;

	if ((ctx->ctx.flags & MAILBOX_LIST_ITER_RAW_LIST) != 0) {
		/* skip ACL checks. */
		return 1;
	}

	acl_name = acl_mailbox_list_iter_get_name(&ctx->ctx, info->name);
	ret = acl_mailbox_list_have_right(ctx->ctx.list, acl_name,
					  ACL_STORAGE_RIGHT_LOOKUP,
					  NULL);
	if (ret != 0) {
		if ((info->flags & MAILBOX_CHILDREN) != 0 &&
		    !iter_mailbox_has_visible_children(ctx, FALSE)) {
			info->flags &= ~MAILBOX_CHILDREN;
			info->flags |= MAILBOX_NOCHILDREN;
		}
		return ret;
	}

	/* no permission to see this mailbox */
	if ((ctx->ctx.flags & MAILBOX_LIST_ITER_SELECT_SUBSCRIBED) != 0) {
		/* we're listing subscribed mailboxes. this one or its child
		   is subscribed, so we'll need to list it. but since we don't
		   have LOOKUP right, we'll need to show it as nonexistent. */
		i_assert((info->flags & PRESERVE_MAILBOX_FLAGS) != 0);
		info->flags = MAILBOX_NONEXISTENT |
			(info->flags & PRESERVE_MAILBOX_FLAGS);
		return 1;
	}

	if (!iter_is_listing_all_children(ctx) &&
	    iter_mailbox_has_visible_children(ctx, TRUE)) {
		/* no child mailboxes match the list pattern(s), but mailbox
		   has visible children. we'll need to show this as
		   non-existent. */
		info->flags = MAILBOX_NONEXISTENT | MAILBOX_CHILDREN |
			(info->flags & PRESERVE_MAILBOX_FLAGS);
		return 1;
	}
	return 0;
}

static const struct mailbox_info *
acl_mailbox_list_iter_next(struct mailbox_list_iterate_context *_ctx)
{
	struct acl_mailbox_list_iterate_context *ctx =
		(struct acl_mailbox_list_iterate_context *)_ctx;
	const struct mailbox_info *info;
	int ret;

	while ((info = acl_mailbox_list_iter_next_info(ctx)) != NULL) {
		ctx->info = *info;
		T_BEGIN {
			ret = acl_mailbox_list_info_is_visible(ctx);
		} T_END;
		if (ret > 0)
			break;
		if (ret < 0) {
			ctx->ctx.failed = TRUE;
			return NULL;
		}
		/* skip to next one */
	}
	return info == NULL ? NULL : &ctx->info;
}

static int
acl_mailbox_list_iter_deinit(struct mailbox_list_iterate_context *_ctx)
{
	struct acl_mailbox_list_iterate_context *ctx =
		(struct acl_mailbox_list_iterate_context *)_ctx;
	struct acl_mailbox_list *alist = ACL_LIST_CONTEXT(_ctx->list);
	int ret = ctx->ctx.failed ? -1 : 0;

	if (alist->module_ctx.super.iter_deinit(ctx->super_ctx) < 0)
		ret = -1;
	if (ctx->lookup_boxes != NULL)
		mailbox_tree_deinit(&ctx->lookup_boxes);
	if (ctx->glob != NULL)
		imap_match_deinit(&ctx->glob);
	i_free(ctx);
	return ret;
}

static int acl_get_mailbox_name_status(struct mailbox_list *list,
				       const char *name,
				       enum mailbox_name_status *status)
{
	struct acl_mailbox_list *alist = ACL_LIST_CONTEXT(list);
	int ret;

	ret = acl_mailbox_list_have_right(list, name, ACL_STORAGE_RIGHT_LOOKUP,
					  NULL);
	if (ret < 0)
		return -1;
	if (ret == 0) {
		/* If we have INSERT right for the mailbox, we'll need to
		   reveal its existence so that APPEND and COPY works. */
		ret = acl_mailbox_list_have_right(list, name,
						  ACL_STORAGE_RIGHT_INSERT,
						  NULL);
		if (ret < 0)
			return -1;
	}

	if (alist->module_ctx.super.get_mailbox_name_status(list, name,
							    status) < 0)
		return -1;
	if (ret > 0)
		return 0;

	/* we shouldn't reveal this mailbox's existance */
	switch (*status) {
	case MAILBOX_NAME_EXISTS:
		*status = MAILBOX_NAME_VALID;
		break;
	case MAILBOX_NAME_VALID:
	case MAILBOX_NAME_INVALID:
		break;
	case MAILBOX_NAME_NOINFERIORS:
		/* have to check if we are allowed to see the parent */
		T_BEGIN {
			ret = acl_storage_rights_ctx_have_right(&alist->rights, name,
					TRUE, ACL_STORAGE_RIGHT_LOOKUP, NULL);
		} T_END;

		if (ret < 0) {
			mailbox_list_set_internal_error(list);
			return -1;
		}
		if (ret == 0) {
			/* no permission to see the parent */
			*status = MAILBOX_NAME_VALID;
		}
		break;
	}
	return 0;
}

static int
acl_mailbox_list_delete(struct mailbox_list *list, const char *name)
{
	struct acl_mailbox_list *alist = ACL_LIST_CONTEXT(list);
	bool can_see;
	int ret;

	ret = acl_mailbox_list_have_right(list, name, ACL_STORAGE_RIGHT_DELETE,
					  &can_see);
	if (ret <= 0) {
		if (ret < 0)
			return -1;
		if (can_see) {
			mailbox_list_set_error(list, MAIL_ERROR_PERM,
					       MAIL_ERRSTR_NO_PERMISSION);
		} else {
			mailbox_list_set_error(list, MAIL_ERROR_NOTFOUND,
				T_MAIL_ERR_MAILBOX_NOT_FOUND(name));
		}
		return -1;
	}

	return alist->module_ctx.super.delete_mailbox(list, name);
}

static int
acl_mailbox_list_rename(struct mailbox_list *list,
			const char *oldname, const char *newname)
{
	struct acl_mailbox_list *alist = ACL_LIST_CONTEXT(list);
	bool can_see;
	int ret;

	/* renaming requires rights to delete the old mailbox */
	ret = acl_mailbox_list_have_right(list, oldname,
					  ACL_STORAGE_RIGHT_DELETE, &can_see);
	if (ret <= 0) {
		if (ret < 0)
			return -1;
		if (can_see) {
			mailbox_list_set_error(list, MAIL_ERROR_PERM,
					       MAIL_ERRSTR_NO_PERMISSION);
		} else {
			mailbox_list_set_error(list, MAIL_ERROR_NOTFOUND,
				T_MAIL_ERR_MAILBOX_NOT_FOUND(oldname));
		}
		return 0;
	}

	/* and create the new one under the parent mailbox */
	T_BEGIN {
		ret = acl_storage_rights_ctx_have_right(&alist->rights, newname,
				TRUE, ACL_STORAGE_RIGHT_CREATE, NULL);
	} T_END;

	if (ret <= 0) {
		if (ret == 0) {
			/* Note that if the mailbox didn't have LOOKUP
			   permission, this not reveals to user the mailbox's
			   existence. Can't help it. */
			mailbox_list_set_error(list, MAIL_ERROR_PERM,
					       MAIL_ERRSTR_NO_PERMISSION);
		} else {
			mailbox_list_set_internal_error(list);
		}
		return -1;
	}

	return alist->module_ctx.super.rename_mailbox(list, oldname, newname);
}

static void acl_mailbox_list_init_shared(struct mailbox_list *list)
{
	struct acl_mailbox_list *alist;

	alist = p_new(list->pool, struct acl_mailbox_list, 1);
	alist->module_ctx.super = list->v;
	list->v.iter_init = acl_mailbox_list_iter_init_shared;

	MODULE_CONTEXT_SET(list, acl_mailbox_list_module, alist);
}

static void acl_mailbox_list_init_default(struct mailbox_list *list)
{
	struct acl_user *auser = ACL_USER_CONTEXT(list->ns->user);
	struct acl_mailbox_list *alist;
	struct acl_backend *backend;
	struct mail_namespace *ns;
	enum mailbox_list_flags flags;
	const char *current_username, *owner_username;
	bool owner = TRUE;

	owner_username = list->ns->user->username;
	current_username = auser->master_user;
	if (current_username == NULL)
		current_username = owner_username;
	else
		owner = strcmp(current_username, owner_username) == 0;

	/* We don't care about the username for non-private mailboxes.
	   It's used only when checking if we're the mailbox owner. We never
	   are for shared/public mailboxes. */
	ns = mailbox_list_get_namespace(list);
	if (ns->type != NAMESPACE_PRIVATE)
		owner = FALSE;

	backend = acl_backend_init(auser->acl_env, list, current_username,
				   auser->groups, owner);
	if (backend == NULL)
		i_fatal("ACL backend initialization failed");

	flags = mailbox_list_get_flags(list);
	if (list->mail_set->mail_full_filesystem_access) {
		/* not necessarily, but safer to do this for now. */
		i_fatal("mail_full_filesystem_access=yes is "
			"incompatible with ACLs");
	}

	alist = p_new(list->pool, struct acl_mailbox_list, 1);
	alist->module_ctx.super = list->v;
	list->v.iter_init = acl_mailbox_list_iter_init;
	list->v.iter_next = acl_mailbox_list_iter_next;
	list->v.iter_deinit = acl_mailbox_list_iter_deinit;
	list->v.get_mailbox_name_status = acl_get_mailbox_name_status;
	list->v.delete_mailbox = acl_mailbox_list_delete;
	list->v.rename_mailbox = acl_mailbox_list_rename;

	acl_storage_rights_ctx_init(&alist->rights, backend);
	MODULE_CONTEXT_SET(list, acl_mailbox_list_module, alist);
}

void acl_mailbox_list_created(struct mailbox_list *list)
{
	struct acl_user *auser = ACL_USER_CONTEXT(list->ns->user);

	if (auser == NULL) {
		/* ACLs disabled for this user */
	} else if ((list->ns->flags & NAMESPACE_FLAG_INTERNAL) != 0) {
		/* no ACL checks for internal namespaces (deliver, shared) */
		if (list->ns->type == NAMESPACE_SHARED)
			acl_mailbox_list_init_shared(list);
	} else {
		acl_mailbox_list_init_default(list);
	}

	if (acl_next_hook_mailbox_list_created != NULL)
		acl_next_hook_mailbox_list_created(list);
}
