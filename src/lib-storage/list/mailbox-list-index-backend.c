/* Copyright (c) 2013 Dovecot authors, see the included COPYING file */

#include "lib.h"
#include "hostpid.h"
#include "mail-index.h"
#include "subscription-file.h"
#include "mailbox-list-delete.h"
#include "mailbox-list-subscriptions.h"
#include "mailbox-list-index-storage.h"
#include "mailbox-list-index-sync.h"

#include <stdio.h>

#define GLOBAL_TEMP_PREFIX ".temp."

struct index_mailbox_list {
	struct mailbox_list list;
	const char *temp_prefix;
};

extern struct mailbox_list index_mailbox_list;

static struct mailbox_list *index_list_alloc(void)
{
	struct index_mailbox_list *list;
	pool_t pool;

	pool = pool_alloconly_create("index list", 2048);

	list = p_new(pool, struct index_mailbox_list, 1);
	list->list = index_mailbox_list;
	list->list.pool = pool;

	list->temp_prefix = p_strconcat(pool, GLOBAL_TEMP_PREFIX,
					my_hostname, ".", my_pid, ".", NULL);
	return &list->list;
}

static int index_list_init(struct mailbox_list *_list, const char **error_r)
{
	const char *dir;

	if (!_list->mail_set->mailbox_list_index) {
		*error_r = "LAYOUT=index requires mailbox_list_index=yes";
		return -1;
	}
	if (mailbox_list_get_root_path(_list, MAILBOX_LIST_PATH_TYPE_INDEX, &dir) &&
	    mailbox_list_mkdir_root(_list, dir, MAILBOX_LIST_PATH_TYPE_INDEX) < 0) {
		*error_r = t_strdup_printf("Failed to create the index root directory: %s",
					   mailbox_list_get_last_error(_list, NULL));
		return -1;
	}
	return 0;
}

static void index_list_deinit(struct mailbox_list *_list)
{
	struct index_mailbox_list *list = (struct index_mailbox_list *)_list;

	pool_unref(&list->list.pool);
}

static char index_list_get_hierarchy_sep(struct mailbox_list *list ATTR_UNUSED)
{
	return MAILBOX_LIST_INDEX_HIERARHCY_SEP;
}

static int
index_list_get_node(struct index_mailbox_list *list, const char *name,
		    struct mailbox_list_index_node **node_r)
{
	struct mailbox_list_index_node *node;

	if (mailbox_list_index_refresh(&list->list) < 0)
		return -1;

	node = mailbox_list_index_lookup(&list->list, name);
	if (node == NULL)
		return 0;
	*node_r = node;
	return 1;
}

static const char *
index_get_guid_path(struct mailbox_list *_list, const char *root_dir,
		    const guid_128_t mailbox_guid)
{
	if (_list->set.mailbox_dir_name == '\0') {
		return t_strconcat(root_dir, "/",
				   guid_128_to_string(mailbox_guid), NULL);
	} else {
		return t_strdup_printf("%s/%s%s", root_dir,
				       _list->set.mailbox_dir_name,
				       guid_128_to_string(mailbox_guid));
	}
}

static int
index_list_get_path(struct mailbox_list *_list, const char *name,
		    enum mailbox_list_path_type type, const char **path_r)
{
	struct index_mailbox_list *list = (struct index_mailbox_list *)_list;
	struct mailbox_list_index *ilist = INDEX_LIST_CONTEXT(_list);
	struct mail_index_view *view;
	struct mailbox_list_index_node *node;
	struct mailbox_status status;
	guid_128_t mailbox_guid;
	const char *root_dir;
	uint32_t seq;
	int ret;

	if (name == NULL) {
		/* return root directories */
		return mailbox_list_set_get_root_path(&_list->set, type,
						      path_r) ? 1 : 0;
	}
	/* consistently use mailbox_dir_name as part of all mailbox
	   directories (index/control/etc) */
	switch (type) {
	case MAILBOX_LIST_PATH_TYPE_MAILBOX:
		type = MAILBOX_LIST_PATH_TYPE_DIR;
		break;
	case MAILBOX_LIST_PATH_TYPE_ALT_MAILBOX:
		type = MAILBOX_LIST_PATH_TYPE_ALT_DIR;
		break;
	default:
		break;
	}
	if (!mailbox_list_set_get_root_path(&_list->set, type, &root_dir))
		return 0;

	if ((ret = index_list_get_node(list, name, &node)) <= 0) {
		if (ret == 0) {
			mailbox_list_set_error(_list, MAIL_ERROR_NOTFOUND,
				T_MAIL_ERR_MAILBOX_NOT_FOUND(name));
		}
		return -1;
	}
	view = mail_index_view_open(ilist->index);
	if (!mail_index_lookup_seq(view, node->uid, &seq))
		i_panic("mailbox list index: lost uid=%u", node->uid);
	if (!mailbox_list_index_status(_list, view, seq, 0,
				       &status, mailbox_guid) ||
	    guid_128_is_empty(mailbox_guid)) {
		mailbox_list_set_error(_list, MAIL_ERROR_NOTFOUND,
				       T_MAIL_ERR_MAILBOX_NOT_FOUND(name));
		ret = -1;
	} else {
		*path_r = index_get_guid_path(_list, root_dir, mailbox_guid);
		ret = 1;
	}
	mail_index_view_close(&view);
	return ret;
}

static const char *
index_list_get_temp_prefix(struct mailbox_list *_list, bool global)
{
	struct index_mailbox_list *list = (struct index_mailbox_list *)_list;

	return global ? GLOBAL_TEMP_PREFIX : list->temp_prefix;
}

static int index_list_set_subscribed(struct mailbox_list *_list,
				     const char *name, bool set)
{
	struct index_mailbox_list *list = (struct index_mailbox_list *)_list;
	const char *path;

	path = t_strconcat(_list->set.control_dir != NULL ?
			   _list->set.control_dir : _list->set.root_dir,
			   "/", _list->set.subscription_fname, NULL);
	return subsfile_set_subscribed(_list, path, list->temp_prefix,
				       name, set);
}

static int
index_list_node_exists(struct index_mailbox_list *list, const char *name,
		       enum mailbox_existence *existence_r)
{
	struct mailbox_list_index_node *node;
	int ret;

	*existence_r = MAILBOX_EXISTENCE_NONE;

	if ((ret = index_list_get_node(list, name, &node)) < 0)
		return -1;
	if (ret == 0)
		return 0;

	if ((node->flags & (MAILBOX_LIST_INDEX_FLAG_NONEXISTENT |
			    MAILBOX_LIST_INDEX_FLAG_NOSELECT)) == 0) {
		/* selectable */
		*existence_r = MAILBOX_EXISTENCE_SELECT;
	} else {
		/* non-selectable */
		*existence_r = MAILBOX_EXISTENCE_NOSELECT;
	}
	return 0;
}

static int
index_list_mailbox_create_dir(struct index_mailbox_list *list, const char *name)
{
	struct mailbox_list_index_sync_context *sync_ctx;
	struct mailbox_list_index_node *node;
	uint32_t seq;
	bool created;
	int ret;

	if (mailbox_list_index_sync_begin(&list->list, &sync_ctx) < 0)
		return -1;

	seq = mailbox_list_index_sync_name(sync_ctx, name, &node, &created);
	if (created || (node->flags & MAILBOX_LIST_INDEX_FLAG_NONEXISTENT) != 0) {
		/* didn't already exist */
		node->flags = MAILBOX_LIST_INDEX_FLAG_NOSELECT;
		mail_index_update_flags(sync_ctx->trans, seq, MODIFY_REPLACE,
					(enum mail_flags)node->flags);
		ret = 1;
	} else {
		/* already existed */
		ret = 0;
	}
	if (mailbox_list_index_sync_end(&sync_ctx, TRUE) < 0)
		ret = -1;
	return ret;
}

static int
index_list_mailbox_create_selectable(struct index_mailbox_list *list,
				     const char *name, guid_128_t mailbox_guid)
{
	struct mailbox_list_index_sync_context *sync_ctx;
	struct mailbox_list_index_record rec;
	struct mailbox_list_index_node *node;
	const void *data;
	bool expunged, created;
	uint32_t seq;

	if (mailbox_list_index_sync_begin(&list->list, &sync_ctx) < 0)
		return -1;

	seq = mailbox_list_index_sync_name(sync_ctx, name, &node, &created);
	if (!created &&
	    (node->flags & (MAILBOX_LIST_INDEX_FLAG_NONEXISTENT |
			    MAILBOX_LIST_INDEX_FLAG_NOSELECT)) == 0) {
		/* already selectable */
		(void)mailbox_list_index_sync_end(&sync_ctx, FALSE);
		return 0;
	}

	mail_index_lookup_ext(sync_ctx->view, seq, sync_ctx->ilist->ext_id,
			      &data, &expunged);
	i_assert(data != NULL && !expunged);
	memcpy(&rec, data, sizeof(rec));
	i_assert(guid_128_is_empty(rec.guid));

	/* make it selectable */
	node->flags = 0;
	mail_index_update_flags(sync_ctx->trans, seq, MODIFY_REPLACE, 0);

	memcpy(rec.guid, mailbox_guid, sizeof(rec.guid));
	mail_index_update_ext(sync_ctx->trans, seq, sync_ctx->ilist->ext_id,
			      &rec, NULL);

	if (mailbox_list_index_sync_end(&sync_ctx, TRUE) < 0)
		return -1;
	return 1;
}

static int
index_list_mailbox_create(struct mailbox *box,
			  const struct mailbox_update *update, bool directory)
{
	struct index_list_mailbox *ibox = INDEX_LIST_STORAGE_CONTEXT(box);
	struct index_mailbox_list *list =
		(struct index_mailbox_list *)box->list;
	struct mailbox_update new_update;
	enum mailbox_existence existence;
	int ret;

	/* first do a quick check that it doesn't exist */
	if ((ret = index_list_node_exists(list, box->name, &existence)) < 0) {
		mail_storage_copy_list_error(box->storage, box->list);
		return -1;
	}
	if (existence == MAILBOX_EXISTENCE_NONE && directory) {
		/* now add the directory to index locked */
		if ((ret = index_list_mailbox_create_dir(list, box->name)) < 0) {
			mail_storage_copy_list_error(box->storage, box->list);
			return -1;
		}
	} else if (existence != MAILBOX_EXISTENCE_SELECT && !directory) {
		/* if no GUID is requested, generate it ourself. set
		   UIDVALIDITY to index sometimes later. */
		if (update == NULL)
			memset(&new_update, 0, sizeof(new_update));
		else
			new_update = *update;
		if (guid_128_is_empty(new_update.mailbox_guid))
			guid_128_generate(new_update.mailbox_guid);
		ret = index_list_mailbox_create_selectable(list, box->name,
							   new_update.mailbox_guid);
		if (ret < 0) {
			mail_storage_copy_list_error(box->storage, box->list);
			return -1;
		}
		/* the storage backend needs to use the same GUID */
		update = &new_update;
	} else {
		ret = 0;
	}

	if (ret == 0) {
		mail_storage_set_error(box->storage, MAIL_ERROR_EXISTS,
				       "Mailbox already exists");
		return -1;
	}
	return directory ? 0 :
		ibox->module_ctx.super.create_box(box, update, directory);
}

static int
index_list_mailbox_update(struct mailbox *box,
			  const struct mailbox_update *update)
{
	struct index_list_mailbox *ibox = INDEX_LIST_STORAGE_CONTEXT(box);
	const char *root_dir, *old_path, *new_path;

	if (mailbox_list_get_path(box->list, box->name,
				  MAILBOX_LIST_PATH_TYPE_MAILBOX,
				  &old_path) <= 0)
		old_path = NULL;

	if (ibox->module_ctx.super.update_box(box, update) < 0)
		return -1;

	/* rename the directory */
	if (!guid_128_is_empty(update->mailbox_guid) && old_path != NULL &&
	    mailbox_list_set_get_root_path(&box->list->set,
					   MAILBOX_LIST_PATH_TYPE_MAILBOX,
					   &root_dir)) {
		new_path = index_get_guid_path(box->list, root_dir,
					       update->mailbox_guid);
		if (strcmp(old_path, new_path) == 0)
			;
		else if (rename(old_path, new_path) == 0)
			;
		else if (errno == ENOENT) {
			mail_storage_set_error(box->storage, MAIL_ERROR_NOTFOUND,
				T_MAIL_ERR_MAILBOX_NOT_FOUND(box->name));
			return -1;
		} else {
			mail_storage_set_critical(box->storage,
						  "rename(%s, %s) failed: %m",
						  old_path, new_path);
			return -1;
		}
	}

	mailbox_list_index_update_mailbox_index(box, update);
	return 0;
}

static int
index_list_mailbox_exists(struct mailbox *box, bool auto_boxes ATTR_UNUSED,
			  enum mailbox_existence *existence_r)
{
	struct index_mailbox_list *list =
		(struct index_mailbox_list *)box->list;

	if (index_list_node_exists(list, box->name, existence_r) < 0) {
		mail_storage_copy_list_error(box->storage, box->list);
		return -1;
	}
	return 0;
}

static void
index_list_try_delete(struct index_mailbox_list *list, const char *name,
		      enum mailbox_list_path_type type)
{
	struct mailbox_list *_list = &list->list;
	const char *mailbox_path, *path;

	if (mailbox_list_get_path(_list, name, MAILBOX_LIST_PATH_TYPE_MAILBOX,
				  &mailbox_path) <= 0 ||
	    mailbox_list_get_path(_list, name, type, &path) <= 0 ||
	    strcmp(path, mailbox_path) == 0)
		return;

	if (*_list->set.maildir_name == '\0' &&
	    (_list->flags & MAILBOX_LIST_FLAG_MAILBOX_FILES) == 0) {
		/* this directory may contain also child mailboxes' data.
		   we don't want to delete that. */
		bool rmdir_path = *_list->set.maildir_name != '\0';
		if (mailbox_list_delete_mailbox_nonrecursive(_list, name, path,
							     rmdir_path) < 0)
			return;
	} else {
		if (mailbox_list_delete_trash(path) < 0 &&
		    errno != ENOENT && errno != ENOTEMPTY) {
			mailbox_list_set_critical(_list,
				"unlink_directory(%s) failed: %m", path);
		}
	}

	/* avoid leaving empty directories lying around */
	mailbox_list_delete_until_root(_list, path, type);
}

static void
index_list_delete_finish(struct index_mailbox_list *list, const char *name)
{
	index_list_try_delete(list, name, MAILBOX_LIST_PATH_TYPE_INDEX);
	index_list_try_delete(list, name, MAILBOX_LIST_PATH_TYPE_CONTROL);
	index_list_try_delete(list, name, MAILBOX_LIST_PATH_TYPE_ALT_MAILBOX);
}

static int
index_list_delete_entry(struct index_mailbox_list *list, const char *name,
			bool delete_selectable)
{
	struct mailbox_list_index_sync_context *sync_ctx;
	struct mailbox_list_index_record rec;
	struct mailbox_list_index_node *node;
	const void *data;
	bool expunged;
	uint32_t seq;
	int ret;

	if (mailbox_list_index_sync_begin(&list->list, &sync_ctx) < 0)
		return -1;

	if ((ret = index_list_get_node(list, name, &node)) < 0) {
		(void)mailbox_list_index_sync_end(&sync_ctx, FALSE);
		return -1;
	}
	if (ret == 0) {
		(void)mailbox_list_index_sync_end(&sync_ctx, FALSE);
		mailbox_list_set_error(&list->list, MAIL_ERROR_NOTFOUND,
				       T_MAIL_ERR_MAILBOX_NOT_FOUND(name));
		return -1;
	}
	if (!mail_index_lookup_seq(sync_ctx->view, node->uid, &seq))
		i_panic("mailbox list index: lost uid=%u", node->uid);
	if (delete_selectable) {
		/* make it at least non-selectable */
		node->flags = MAILBOX_LIST_INDEX_FLAG_NOSELECT;
		mail_index_update_flags(sync_ctx->trans, seq, MODIFY_REPLACE,
					(enum mail_flags)node->flags);

		mail_index_lookup_ext(sync_ctx->view, seq,
				      sync_ctx->ilist->ext_id,
				      &data, &expunged);
		i_assert(data != NULL && !expunged);
		memcpy(&rec, data, sizeof(rec));
		rec.uid_validity = 0;
		memset(&rec.guid, 0, sizeof(rec.guid));
		mail_index_update_ext(sync_ctx->trans, seq,
				      sync_ctx->ilist->ext_id, &rec, NULL);
	}
	if (node->children != NULL) {
		/* can't delete this directory before its children,
		   but we may have made it non-selectable already */
		if (mailbox_list_index_sync_end(&sync_ctx, TRUE) < 0)
			return -1;
		return 0;
	}

	/* we can remove the entire node */
	mail_index_expunge(sync_ctx->trans, seq);
	mailbox_list_index_node_unlink(sync_ctx->ilist, node);

	if (mailbox_list_index_sync_end(&sync_ctx, TRUE) < 0)
		return -1;
	return 1;
}

static int
index_list_delete_mailbox(struct mailbox_list *_list, const char *name)
{
	struct index_mailbox_list *list = (struct index_mailbox_list *)_list;
	const char *path;
	int ret;

	/* first delete the mailbox files */
	ret = mailbox_list_get_path(_list, name, MAILBOX_LIST_PATH_TYPE_MAILBOX,
				    &path);
	if (ret <= 0)
		return ret;

	if ((_list->flags & MAILBOX_LIST_FLAG_NO_MAIL_FILES) != 0) {
		ret = 0;
	} else if ((_list->flags & MAILBOX_LIST_FLAG_MAILBOX_FILES) != 0) {
		ret = mailbox_list_delete_mailbox_file(_list, name, path);
	} else {
		ret = mailbox_list_delete_mailbox_nonrecursive(_list, name,
							       path, TRUE);
	}

	if (ret == 0 || (_list->props & MAILBOX_LIST_PROP_AUTOCREATE_DIRS) != 0)
		index_list_delete_finish(list, name);
	if (ret == 0) {
		if (index_list_delete_entry(list, name, TRUE) < 0)
			return -1;
	}
	return ret;
}

static int
index_list_delete_dir(struct mailbox_list *_list, const char *name)
{
	struct index_mailbox_list *list = (struct index_mailbox_list *)_list;
	int ret;

	if ((ret = index_list_delete_entry(list, name, FALSE)) < 0)
		return -1;
	if (ret == 0) {
		mailbox_list_set_error(_list, MAIL_ERROR_EXISTS,
			"Mailbox has children, delete them first");
		return -1;
	}
	return 0;
}

static int
index_list_delete_symlink(struct mailbox_list *_list,
			  const char *name ATTR_UNUSED)
{
	mailbox_list_set_error(_list, MAIL_ERROR_NOTPOSSIBLE,
			       "Symlinks not supported");
	return -1;
}

static int
index_list_rename_mailbox(struct mailbox_list *_oldlist, const char *oldname,
			  struct mailbox_list *_newlist, const char *newname)
{
	struct index_mailbox_list *list = (struct index_mailbox_list *)_oldlist;
	struct mailbox_list_index_sync_context *sync_ctx;
	struct mailbox_list_index_record oldrec, newrec;
	struct mailbox_list_index_node *oldnode, *newnode, *child;
	const void *data;
	bool created, expunged;
	uint32_t oldseq, newseq;
	int ret;

	if (_oldlist != _newlist) {
		mailbox_list_set_error(_oldlist, MAIL_ERROR_NOTPOSSIBLE,
			"Renaming not supported across namespaces.");
		return -1;
	}

	if (mailbox_list_index_sync_begin(&list->list, &sync_ctx) < 0)
		return -1;

	if ((ret = index_list_get_node(list, oldname, &oldnode)) < 0) {
		(void)mailbox_list_index_sync_end(&sync_ctx, FALSE);
		return -1;
	}
	if (ret == 0) {
		(void)mailbox_list_index_sync_end(&sync_ctx, FALSE);
		mailbox_list_set_error(&list->list, MAIL_ERROR_NOTFOUND,
			T_MAIL_ERR_MAILBOX_NOT_FOUND(oldname));
		return -1;
	}
	if (!mail_index_lookup_seq(sync_ctx->view, oldnode->uid, &oldseq))
		i_panic("mailbox list index: lost uid=%u", oldnode->uid);

	newseq = mailbox_list_index_sync_name(sync_ctx, newname,
					      &newnode, &created);
	if (!created) {
		(void)mailbox_list_index_sync_end(&sync_ctx, FALSE);
		mailbox_list_set_error(&list->list, MAIL_ERROR_EXISTS,
				       "Target mailbox already exists");
		return -1;
	}
	i_assert(oldnode != newnode);

	/* copy all the data from old node to new node */
	newnode->uid = oldnode->uid;
	newnode->flags = oldnode->flags;
	newnode->children = oldnode->children; oldnode->children = NULL;
	for (child = newnode->children; child != NULL; child = child->next)
		child->parent = newnode;

	/* remove the old node from existence */
	mailbox_list_index_node_unlink(sync_ctx->ilist, oldnode);

	/* update the old index record to contain the new name_id/parent_uid,
	   then expunge the added index record */
	mail_index_lookup_ext(sync_ctx->view, oldseq, sync_ctx->ilist->ext_id,
			      &data, &expunged);
	i_assert(data != NULL && !expunged);
	memcpy(&oldrec, data, sizeof(oldrec));

	mail_index_lookup_ext(sync_ctx->view, newseq, sync_ctx->ilist->ext_id,
			      &data, &expunged);
	i_assert(data != NULL && !expunged);
	memcpy(&newrec, data, sizeof(newrec));

	oldrec.name_id = newrec.name_id;
	oldrec.parent_uid = newrec.parent_uid;

	mail_index_update_ext(sync_ctx->trans, oldseq,
			      sync_ctx->ilist->ext_id, &oldrec, NULL);
	mail_index_expunge(sync_ctx->trans, newseq);

	return mailbox_list_index_sync_end(&sync_ctx, TRUE);
}

static struct mailbox_list_iterate_context *
index_list_iter_init(struct mailbox_list *list,
		     const char *const *patterns,
		     enum mailbox_list_iter_flags flags)
{
	struct mailbox_list_iterate_context *ctx;
	pool_t pool;

	if ((flags & MAILBOX_LIST_ITER_SELECT_SUBSCRIBED) != 0) {
		return mailbox_list_subscriptions_iter_init(list, patterns,
							    flags);
	}

	pool = pool_alloconly_create("mailbox list index backend iter", 1024);
	ctx = p_new(pool, struct mailbox_list_iterate_context, 1);
	ctx->pool = pool;
	ctx->list = list;
	ctx->flags = flags;
	array_create(&ctx->module_contexts, pool, sizeof(void *), 5);
	return ctx;
}

static const struct mailbox_info *
index_list_iter_next(struct mailbox_list_iterate_context *ctx)
{
	if ((ctx->flags & MAILBOX_LIST_ITER_SELECT_SUBSCRIBED) != 0)
		return mailbox_list_subscriptions_iter_next(ctx);
	return NULL;
}

static int index_list_iter_deinit(struct mailbox_list_iterate_context *ctx)
{
	if ((ctx->flags & MAILBOX_LIST_ITER_SELECT_SUBSCRIBED) != 0)
		return mailbox_list_subscriptions_iter_deinit(ctx);
	pool_unref(&ctx->pool);
	return 0;
}

struct mailbox_list index_mailbox_list = {
	.name = MAILBOX_LIST_NAME_INDEX,
	.props = MAILBOX_LIST_PROP_NO_ROOT,
	.mailbox_name_max_length = MAILBOX_LIST_NAME_MAX_LENGTH,

	{
		index_list_alloc,
		index_list_init,
		index_list_deinit,
		NULL,
		index_list_get_hierarchy_sep,
		mailbox_list_default_get_vname,
		mailbox_list_default_get_storage_name,
		index_list_get_path,
		index_list_get_temp_prefix,
		NULL,
		index_list_iter_init,
		index_list_iter_next,
		index_list_iter_deinit,
		NULL,
		NULL,
		mailbox_list_subscriptions_refresh,
		index_list_set_subscribed,
		index_list_delete_mailbox,
		index_list_delete_dir,
		index_list_delete_symlink,
		index_list_rename_mailbox,
		NULL, NULL, NULL, NULL
	}
};

void mailbox_list_index_backend_init_mailbox(struct mailbox *box)
{
	if (strcmp(box->list->name, MAILBOX_LIST_NAME_INDEX) != 0)
		return;
	box->v.create_box = index_list_mailbox_create;
	box->v.update_box = index_list_mailbox_update;
	box->v.exists = index_list_mailbox_exists;

	box->v.list_index_has_changed = NULL;
	box->v.list_index_update_sync = NULL;
}
