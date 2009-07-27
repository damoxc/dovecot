/* Copyright (c) 2002-2009 Dovecot authors, see the included COPYING file */

#include "lib.h"
#include "str.h"
#include "imap-match.h"
#include "mailbox-tree.h"
#include "mailbox-list-subscriptions.h"
#include "mailbox-list-maildir.h"

#include <dirent.h>

struct maildir_list_iterate_context {
	struct mailbox_list_iterate_context ctx;
	pool_t pool;

	const char *dir;

        struct mailbox_tree_context *tree_ctx;
	struct mailbox_tree_iterate_context *tree_iter;

	struct mailbox_info info;
};

static void node_fix_parents(struct mailbox_node *node)
{
	/* Fix parent nodes' children states. also if we happened to create any
	   of the parents, we need to mark them nonexistent. */
	node = node->parent;
	for (; node != NULL; node = node->parent) {
		if ((node->flags & MAILBOX_MATCHED) == 0)
			node->flags |= MAILBOX_NONEXISTENT;

		node->flags |= MAILBOX_CHILDREN;
		node->flags &= ~MAILBOX_NOCHILDREN;
	}
}

static void
maildir_fill_parents(struct maildir_list_iterate_context *ctx,
		     struct imap_match_glob *glob, bool update_only,
		     string_t *mailbox, enum mailbox_info_flags flags)
{
	struct mail_namespace *ns = ctx->ctx.list->ns;
	struct mailbox_node *node;
	const char *p, *mailbox_c;
	char hierarchy_sep;
	bool created;
	unsigned int prefix_len;

	if ((ctx->ctx.flags & MAILBOX_LIST_ITER_VIRTUAL_NAMES) != 0) {
		hierarchy_sep = ns->sep;
		prefix_len = ns->prefix_len;
	} else {
		hierarchy_sep = ns->real_sep;
		prefix_len = 0;
	}

	mailbox_c = str_c(mailbox);
	while ((p = strrchr(mailbox_c, hierarchy_sep)) != NULL) {
		str_truncate(mailbox, (size_t) (p-mailbox_c));
		mailbox_c = str_c(mailbox);
		if (imap_match(glob, mailbox_c) != IMAP_MATCH_YES)
			continue;

		if (prefix_len > 0 && str_len(mailbox) == prefix_len-1 &&
		    strncmp(mailbox_c, ns->prefix, prefix_len - 1) == 0 &&
		    mailbox_c[prefix_len-1] == hierarchy_sep) {
			/* don't return matches to namespace prefix itself */
			continue;
		}

		created = FALSE;
		node = update_only ?
			mailbox_tree_lookup(ctx->tree_ctx, mailbox_c) :
			mailbox_tree_get(ctx->tree_ctx, mailbox_c, &created);
		if (node != NULL) {
			if (created) {
				/* we haven't yet seen this mailbox,
				   but we might see it later */
				node->flags = MAILBOX_NONEXISTENT;
			}
			if (!update_only)
				node->flags |= MAILBOX_MATCHED;
			node->flags |= MAILBOX_CHILDREN | flags;
			node->flags &= ~MAILBOX_NOCHILDREN;
			node_fix_parents(node);
		}
	}
}

static void maildir_set_children(struct maildir_list_iterate_context *ctx,
				 string_t *mailbox)
{
	struct mailbox_node *node;
	const char *p, *mailbox_c;
	char hierarchy_sep;

	if ((ctx->ctx.flags & MAILBOX_LIST_ITER_VIRTUAL_NAMES) != 0)
		hierarchy_sep = ctx->ctx.list->ns->sep;
	else
		hierarchy_sep = ctx->ctx.list->ns->real_sep;

	/* mark the first existing parent as containing children */
	mailbox_c = str_c(mailbox);
	while ((p = strrchr(mailbox_c, hierarchy_sep)) != NULL) {
		str_truncate(mailbox, (size_t) (p-mailbox_c));
		mailbox_c = str_c(mailbox);

		node = mailbox_tree_lookup(ctx->tree_ctx, mailbox_c);
		if (node != NULL) {
			node->flags &= ~MAILBOX_NOCHILDREN;
			node->flags |= MAILBOX_CHILDREN;
			break;
		}
	}
}

static int
maildir_fill_inbox(struct maildir_list_iterate_context *ctx,
		   const char *mailbox_name, struct imap_match_glob *glob,
		   bool update_only)
{
	const struct mailbox_list_settings *set = &ctx->ctx.list->set;
	struct mailbox_node *node;
	enum mailbox_info_flags flags;
	enum imap_match_result match;
	bool created;
	int ret;

	if ((ctx->ctx.flags & MAILBOX_LIST_ITER_NO_AUTO_INBOX) == 0) {
		/* always show INBOX */
	} else if (set->inbox_path != NULL &&
		   strcmp(set->inbox_path, set->root_dir) != 0) {
		/* INBOX doesn't exist, since it wasn't listed */
		update_only = TRUE;
	} else {
		/* INBOX is in Maildir root. show it only if it has already
		   been created */
		ret = ctx->ctx.list->v.
			iter_is_mailbox(&ctx->ctx, ctx->dir, "", mailbox_name,
					MAILBOX_LIST_FILE_TYPE_UNKNOWN, &flags);
		if (ret < 0)
			return -1;
		if (ret == 0)
			update_only = TRUE;
	}

	created = FALSE;
	node = update_only ?
		mailbox_tree_lookup(ctx->tree_ctx, mailbox_name) :
		mailbox_tree_get(ctx->tree_ctx, mailbox_name, &created);
	if (created)
		node->flags = MAILBOX_NOCHILDREN;
	else if (node != NULL)
		node->flags &= ~MAILBOX_NONEXISTENT;

	match = imap_match(glob, mailbox_name);
	if ((match & (IMAP_MATCH_YES | IMAP_MATCH_PARENT)) != 0) {
		if (!update_only)
			node->flags |= MAILBOX_MATCHED;
	}
	return 0;
}

static int
maildir_fill_readdir(struct maildir_list_iterate_context *ctx,
		     struct imap_match_glob *glob, bool update_only)
{
	struct mail_namespace *ns = ctx->ctx.list->ns;
	DIR *dirp;
	struct dirent *d;
	const char *mailbox_name;
	string_t *mailbox;
	enum mailbox_info_flags flags;
	enum imap_match_result match;
	struct mailbox_node *node;
	bool created, virtual_names;
	char prefix_char;
	int ret;

	dirp = opendir(ctx->dir);
	if (dirp == NULL) {
		if (errno == EACCES) {
			mailbox_list_set_critical(ctx->ctx.list, "%s",
				mail_error_eacces_msg("opendir", ctx->dir));
		} else if (errno != ENOENT) {
			mailbox_list_set_critical(ctx->ctx.list,
				"opendir(%s) failed: %m", ctx->dir);
			return -1;
		}
		return 0;
	}

	virtual_names = (ctx->ctx.flags & MAILBOX_LIST_ITER_VIRTUAL_NAMES) != 0;
	prefix_char =
		strcmp(ctx->ctx.list->name, MAILBOX_LIST_NAME_IMAPDIR) != 0 ?
		ctx->ctx.list->hierarchy_sep : '\0';

	mailbox = t_str_new(PATH_MAX);
	while ((d = readdir(dirp)) != NULL) {
		const char *fname = d->d_name;

		if (fname[0] == prefix_char)
			mailbox_name = fname + 1;
		else {
			if (prefix_char != '\0' || fname[0] == '.')
				continue;
			mailbox_name = fname;
		}

		/* skip . and .. */
		if (fname[0] == '.' &&
		    (fname[1] == '\0' || (fname[1] == '.' && fname[2] == '\0')))
			continue;

		if (!virtual_names) {
			str_truncate(mailbox, 0);
			str_append(mailbox, mailbox_name);
			mailbox_name = str_c(mailbox);
		} else {
			mailbox_name = mail_namespace_get_vname(ns, mailbox,
								mailbox_name);
		}

		/* make sure the pattern matches */
		match = imap_match(glob, mailbox_name);
		if ((match & (IMAP_MATCH_YES | IMAP_MATCH_PARENT)) == 0)
			continue;

		/* check if this is an actual mailbox */
		flags = 0;
		T_BEGIN {
			ret = ctx->ctx.list->v.
				iter_is_mailbox(&ctx->ctx, ctx->dir, fname,
					mailbox_name,
					mailbox_list_get_file_type(d), &flags);
		} T_END;
		if (ret <= 0) {
			if (ret < 0)
				return -1;
			continue;
		}

		/* we know the children flags ourself, so ignore if any of
		   them were set. */
		flags &= ~(MAILBOX_NOINFERIORS |
			   MAILBOX_CHILDREN | MAILBOX_NOCHILDREN);

		if ((match & IMAP_MATCH_PARENT) != 0) {
			T_BEGIN {
				maildir_fill_parents(ctx, glob, update_only,
						     mailbox, flags);
			} T_END;
		} else {
			created = FALSE;
			node = update_only ?
				mailbox_tree_lookup(ctx->tree_ctx,
						    mailbox_name) :
				mailbox_tree_get(ctx->tree_ctx,
						 mailbox_name, &created);

			if (node != NULL) {
				if (created)
					node->flags = MAILBOX_NOCHILDREN;
				else
					node->flags &= ~MAILBOX_NONEXISTENT;
				if (!update_only)
					node->flags |= MAILBOX_MATCHED;
				node->flags |= flags;
				node_fix_parents(node);
			} else {
				i_assert(update_only);
				maildir_set_children(ctx, mailbox);
			}
		}
	}

	if (closedir(dirp) < 0) {
		mailbox_list_set_critical(ctx->ctx.list,
					  "readdir(%s) failed: %m", ctx->dir);
		return -1;
	}

	if ((ns->flags & NAMESPACE_FLAG_INBOX) == 0)
		return 0;
	else {
		/* make sure INBOX is listed */
		mailbox_name = !virtual_names ? "INBOX" :
			mail_namespace_get_vname(ns, mailbox, "INBOX");
		return maildir_fill_inbox(ctx, mailbox_name, glob, update_only);
	}
}

struct mailbox_list_iterate_context *
maildir_list_iter_init(struct mailbox_list *_list, const char *const *patterns,
		       enum mailbox_list_iter_flags flags)
{
	struct maildir_list_iterate_context *ctx;
        struct imap_match_glob *glob;
	char sep;
	pool_t pool;
	int ret;

	sep = (flags & MAILBOX_LIST_ITER_VIRTUAL_NAMES) != 0 ?
		_list->ns->sep : _list->ns->real_sep;

	pool = pool_alloconly_create("maildir_list", 1024);
	ctx = p_new(pool, struct maildir_list_iterate_context, 1);
	ctx->ctx.list = _list;
	ctx->ctx.flags = flags;
	ctx->pool = pool;
	ctx->tree_ctx = mailbox_tree_init(sep);
	ctx->info.ns = _list->ns;

	glob = imap_match_init_multiple(pool, patterns, TRUE, sep);

	ctx->dir = _list->set.root_dir;

	if ((flags & MAILBOX_LIST_ITER_SELECT_SUBSCRIBED) != 0) {
		/* Listing only subscribed mailboxes.
		   Flags are set later if needed. */
		if (mailbox_list_subscriptions_fill(&ctx->ctx, ctx->tree_ctx,
						    glob, FALSE) < 0) {
			ctx->ctx.failed = TRUE;
			return &ctx->ctx;
		}
	}

	if ((flags & MAILBOX_LIST_ITER_SELECT_SUBSCRIBED) == 0 ||
	    (flags & MAILBOX_LIST_ITER_RETURN_NO_FLAGS) == 0) {
		/* Add/update mailbox list with flags */
		bool update_only =
			(flags & MAILBOX_LIST_ITER_SELECT_SUBSCRIBED) != 0;

		T_BEGIN {
			ret = maildir_fill_readdir(ctx, glob, update_only);
		} T_END;
		if (ret < 0) {
			ctx->ctx.failed = TRUE;
			return &ctx->ctx;
		}
	}

	if ((flags & MAILBOX_LIST_ITER_RETURN_SUBSCRIBED) != 0 &&
	    (flags & MAILBOX_LIST_ITER_SELECT_SUBSCRIBED) == 0) {
		/* we're listing all mailboxes but we want to know
		   \Subscribed flags */
		if (mailbox_list_subscriptions_fill(&ctx->ctx, ctx->tree_ctx,
						    glob, TRUE) < 0) {
			ctx->ctx.failed = TRUE;
			return &ctx->ctx;
		}
	}

	ctx->tree_iter = mailbox_tree_iterate_init(ctx->tree_ctx, NULL,
						   MAILBOX_MATCHED);
	return &ctx->ctx;
}

int maildir_list_iter_deinit(struct mailbox_list_iterate_context *_ctx)
{
	struct maildir_list_iterate_context *ctx =
		(struct maildir_list_iterate_context *)_ctx;
	int ret = ctx->ctx.failed ? -1 : 0;

	if (ctx->tree_iter != NULL)
		mailbox_tree_iterate_deinit(&ctx->tree_iter);
	mailbox_tree_deinit(&ctx->tree_ctx);
	pool_unref(&ctx->pool);
	return ret;
}

const struct mailbox_info *
maildir_list_iter_next(struct mailbox_list_iterate_context *_ctx)
{
	struct maildir_list_iterate_context *ctx =
		(struct maildir_list_iterate_context *)_ctx;
	struct mailbox_node *node;

	if (ctx->ctx.failed)
		return NULL;

	node = mailbox_tree_iterate_next(ctx->tree_iter, &ctx->info.name);
	if (node == NULL)
		return NULL;

	ctx->info.flags = node->flags;
	return &ctx->info;
}
