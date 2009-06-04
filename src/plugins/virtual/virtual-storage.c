/* Copyright (c) 2008-2009 Dovecot authors, see the included COPYING file */

#include "lib.h"
#include "array.h"
#include "ioloop.h"
#include "str.h"
#include "mkdir-parents.h"
#include "unlink-directory.h"
#include "index-mail.h"
#include "mail-copy.h"
#include "mail-search.h"
#include "virtual-plugin.h"
#include "virtual-transaction.h"
#include "virtual-storage.h"

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <dirent.h>
#include <sys/stat.h>

#define VIRTUAL_LIST_CONTEXT(obj) \
	MODULE_CONTEXT(obj, virtual_mailbox_list_module)

struct virtual_mailbox_list {
	union mailbox_list_module_context module_ctx;
};

extern struct mail_storage virtual_storage;
extern struct mailbox virtual_mailbox;

struct virtual_storage_module virtual_storage_module =
	MODULE_CONTEXT_INIT(&mail_storage_module_register);
static MODULE_CONTEXT_DEFINE_INIT(virtual_mailbox_list_module,
				  &mailbox_list_module_register);

void virtual_box_copy_error(struct mailbox *dest, struct mailbox *src)
{
	const char *str;
	enum mail_error error;

	str = mail_storage_get_last_error(src->storage, &error);
	if ((src->list->ns->flags & NAMESPACE_FLAG_HIDDEN) != 0)
		str = t_strdup_printf("%s (mailbox %s)", str, src->name);
	else {
		str = t_strdup_printf("%s (mailbox %s%s)", str,
				      src->list->ns->prefix, src->name);
	}
	mail_storage_set_error(dest->storage, error, str);
}

static struct mail_storage *virtual_storage_alloc(void)
{
	struct virtual_storage *storage;
	pool_t pool;

	pool = pool_alloconly_create("virtual storage", 512+256);
	storage = p_new(pool, struct virtual_storage, 1);
	storage->storage = virtual_storage;
	storage->storage.pool = pool;
	p_array_init(&storage->open_stack, pool, 8);
	return &storage->storage;
}

static void
virtual_storage_get_list_settings(const struct mail_namespace *ns ATTR_UNUSED,
				  struct mailbox_list_settings *set)
{
	if (set->layout == NULL)
		set->layout = MAILBOX_LIST_NAME_FS;
	if (set->subscription_fname == NULL)
		set->subscription_fname = VIRTUAL_SUBSCRIPTION_FILE_NAME;
}

struct virtual_backend_box *
virtual_backend_box_lookup_name(struct virtual_mailbox *mbox, const char *name)
{
	struct virtual_backend_box *const *bboxes;
	unsigned int i, count;

	bboxes = array_get(&mbox->backend_boxes, &count);
	for (i = 0; i < count; i++) {
		if (strcmp(bboxes[i]->name, name) == 0)
			return bboxes[i];
	}
	return NULL;
}

struct virtual_backend_box *
virtual_backend_box_lookup(struct virtual_mailbox *mbox, uint32_t mailbox_id)
{
	struct virtual_backend_box *const *bboxes;
	unsigned int i, count;

	if (mailbox_id == 0)
		return NULL;

	bboxes = array_get(&mbox->backend_boxes, &count);
	for (i = 0; i < count; i++) {
		if (bboxes[i]->mailbox_id == mailbox_id)
			return bboxes[i];
	}
	return NULL;
}

static bool virtual_mailbox_is_in_open_stack(struct virtual_storage *storage,
					     const char *name)
{
	const char *const *names;
	unsigned int i, count;

	names = array_get(&storage->open_stack, &count);
	for (i = 0; i < count; i++) {
		if (strcmp(names[i], name) == 0)
			return TRUE;
	}
	return FALSE;
}

static int virtual_mailboxes_open(struct virtual_mailbox *mbox,
				  enum mailbox_open_flags open_flags)
{
	struct mail_user *user = mbox->storage->storage.user;
	struct virtual_backend_box *const *bboxes;
	struct mail_namespace *ns;
	unsigned int i, count;
	enum mail_error error;
	const char *str, *mailbox;

	open_flags |= MAILBOX_OPEN_KEEP_RECENT;

	bboxes = array_get(&mbox->backend_boxes, &count);
	for (i = 0; i < count; ) {
		mailbox = bboxes[i]->name;
		ns = mail_namespace_find(user->namespaces, &mailbox);
		bboxes[i]->box = mailbox_open(ns->list, mailbox,
					      NULL, open_flags);

		if (bboxes[i]->box == NULL) {
			str = mailbox_list_get_last_error(ns->list, &error);
			if (bboxes[i]->wildcard &&
			    (error == MAIL_ERROR_PERM ||
			     error == MAIL_ERROR_NOTFOUND)) {
				/* this mailbox wasn't explicitly specified.
				   just skip it. */
				mail_search_args_unref(&bboxes[i]->search_args);
				array_delete(&mbox->backend_boxes, i, 1);
				bboxes = array_get(&mbox->backend_boxes, &count);
				continue;
			}
			if (ns->list != mbox->ibox.box.list) {
				/* copy the error */
				mailbox_list_set_error(mbox->ibox.box.list,
						       error, str);
			}
			break;
		}
		i_array_init(&bboxes[i]->uids, 64);
		i_array_init(&bboxes[i]->sync_pending_removes, 64);
		mail_search_args_init(bboxes[i]->search_args, bboxes[i]->box,
				      FALSE, NULL);
		i++;
	}
	if (i == count)
		return 0;
	else {
		/* failed */
		for (; i > 0; i--) {
			(void)mailbox_close(&bboxes[i-1]->box);
			array_free(&bboxes[i-1]->uids);
		}
		return -1;
	}
}

static struct mailbox *
virtual_open(struct virtual_storage *storage, struct mailbox_list *list,
	     const char *name, enum mailbox_open_flags flags)
{
	struct mail_storage *_storage = &storage->storage;
	struct virtual_mailbox *mbox;
	struct mail_index *index;
	const char *path;
	pool_t pool;
	bool failed;

	if (virtual_mailbox_is_in_open_stack(storage, name)) {
		mail_storage_set_critical(_storage,
					  "Virtual mailbox loops: %s", name);
		return NULL;
	}

	path = mailbox_list_get_path(list, name,
				     MAILBOX_LIST_PATH_TYPE_MAILBOX);
	index = index_storage_alloc(list, name, flags, VIRTUAL_INDEX_PREFIX);

	pool = pool_alloconly_create("virtual mailbox", 1024+512);
	mbox = p_new(pool, struct virtual_mailbox, 1);
	mbox->ibox.box = virtual_mailbox;
	mbox->ibox.box.pool = pool;
	mbox->ibox.box.storage = _storage;
	mbox->ibox.mail_vfuncs = &virtual_mail_vfuncs;
	mbox->ibox.index = index;

	mbox->storage = storage;
	mbox->path = p_strdup(pool, path);
	mbox->vseq_lookup_prev_mailbox = i_strdup("");

	mbox->virtual_ext_id =
		mail_index_ext_register(index, "virtual", 0,
			sizeof(struct virtual_mail_index_record),
			sizeof(uint32_t));

	array_append(&storage->open_stack, &name, 1);
	failed = virtual_config_read(mbox) < 0 ||
		virtual_mailboxes_open(mbox, flags) < 0;
	array_delete(&storage->open_stack,
		     array_count(&storage->open_stack)-1, 1);
	if (failed) {
		virtual_config_free(mbox);
		index_storage_mailbox_close(&mbox->ibox.box);
		return NULL;
	}

	index_storage_mailbox_init(&mbox->ibox, name, flags, FALSE);
	return &mbox->ibox.box;
}

static struct mailbox *
virtual_mailbox_open(struct mail_storage *_storage, struct mailbox_list *list,
		     const char *name, struct istream *input,
		     enum mailbox_open_flags flags)
{
	struct virtual_storage *storage = (struct virtual_storage *)_storage;
	const char *path;
	struct stat st;

	if (input != NULL) {
		mailbox_list_set_critical(list,
			"virtual doesn't support streamed mailboxes");
		return NULL;
	}

	path = mailbox_list_get_path(list, name,
				     MAILBOX_LIST_PATH_TYPE_MAILBOX);
	if (stat(path, &st) == 0)
		return virtual_open(storage, list, name, flags);
	else if (errno == ENOENT) {
		mailbox_list_set_error(list, MAIL_ERROR_NOTFOUND,
			T_MAIL_ERR_MAILBOX_NOT_FOUND(name));
	} else if (errno == EACCES) {
		mailbox_list_set_critical(list, "%s",
			mail_error_eacces_msg("stat", path));
	} else {
		mailbox_list_set_critical(list, "stat(%s) failed: %m", path);
	}
	return NULL;
}

static int virtual_storage_mailbox_close(struct mailbox *box)
{
	struct virtual_mailbox *mbox = (struct virtual_mailbox *)box;
	struct mail_storage *storage;
	struct virtual_backend_box **bboxes;
	unsigned int i, count;
	int ret = 0;

	virtual_config_free(mbox);

	bboxes = array_get_modifiable(&mbox->backend_boxes, &count);
	for (i = 0; i < count; i++) {
		if (bboxes[i]->search_result != NULL)
			mailbox_search_result_free(&bboxes[i]->search_result);

		storage = bboxes[i]->box->storage;
		if (mailbox_close(&bboxes[i]->box) < 0) {
			const char *str;
			enum mail_error error;

			str = mail_storage_get_last_error(storage, &error);
			mail_storage_set_error(box->storage, error, str);
			ret = -1;
		}
		if (array_is_created(&bboxes[i]->sync_outside_expunges))
			array_free(&bboxes[i]->sync_outside_expunges);
		array_free(&bboxes[i]->sync_pending_removes);
		array_free(&bboxes[i]->uids);
	}
	array_free(&mbox->backend_boxes);
	i_free(mbox->vseq_lookup_prev_mailbox);

	return index_storage_mailbox_close(box) < 0 ? -1 : ret;
}

static int virtual_mailbox_create(struct mail_storage *_storage,
				  struct mailbox_list *list ATTR_UNUSED,
				  const char *name ATTR_UNUSED,
				  bool directory ATTR_UNUSED)
{
	mail_storage_set_error(_storage, MAIL_ERROR_NOTPOSSIBLE,
			       "Can't create virtual mailboxes");
	return -1;
}

static int
virtual_delete_nonrecursive(struct mailbox_list *list, const char *path,
			    const char *name)
{
	DIR *dir;
	struct dirent *d;
	string_t *full_path;
	unsigned int dir_len;
	bool unlinked_something = FALSE;

	dir = opendir(path);
	if (dir == NULL) {
		if (!mailbox_list_set_error_from_errno(list)) {
			mailbox_list_set_critical(list,
				"opendir(%s) failed: %m", path);
		}
		return -1;
	}

	full_path = t_str_new(256);
	str_append(full_path, path);
	str_append_c(full_path, '/');
	dir_len = str_len(full_path);

	errno = 0;
	while ((d = readdir(dir)) != NULL) {
		if (d->d_name[0] == '.') {
			/* skip . and .. */
			if (d->d_name[1] == '\0')
				continue;
			if (d->d_name[1] == '.' && d->d_name[2] == '\0')
				continue;
		}

		str_truncate(full_path, dir_len);
		str_append(full_path, d->d_name);

		/* trying to unlink() a directory gives either EPERM or EISDIR
		   (non-POSIX). it doesn't really work anywhere in practise,
		   so don't bother stat()ing the file first */
		if (unlink(str_c(full_path)) == 0)
			unlinked_something = TRUE;
		else if (errno != ENOENT && errno != EISDIR && errno != EPERM) {
			mailbox_list_set_critical(list,
				"unlink(%s) failed: %m",
				str_c(full_path));
		}
	}

	if (closedir(dir) < 0) {
		mailbox_list_set_critical(list, "closedir(%s) failed: %m",
					  path);
	}

	if (rmdir(path) == 0)
		unlinked_something = TRUE;
	else if (errno != ENOENT && errno != ENOTEMPTY) {
		mailbox_list_set_critical(list, "rmdir(%s) failed: %m", path);
		return -1;
	}

	if (!unlinked_something) {
		mailbox_list_set_error(list, MAIL_ERROR_NOTPOSSIBLE,
			t_strdup_printf("Directory %s isn't empty, "
					"can't delete it.", name));
		return -1;
	}
	return 0;
}

static int
virtual_list_delete_mailbox(struct mailbox_list *list, const char *name)
{
	struct virtual_mailbox_list *mlist = VIRTUAL_LIST_CONTEXT(list);
	struct stat st;
	const char *src;

	/* Make sure the indexes are closed before trying to delete the
	   directory that contains them. It can still fail with some NFS
	   implementations if indexes are opened by another session, but
	   that can't really be helped. */
	index_storage_destroy_unrefed();

	/* delete the index and control directories */
	if (mlist->module_ctx.super.delete_mailbox(list, name) < 0)
		return -1;

	/* check if the mailbox actually exists */
	src = mailbox_list_get_path(list, name, MAILBOX_LIST_PATH_TYPE_MAILBOX);
	if (stat(src, &st) != 0 && errno == ENOENT) {
		mailbox_list_set_error(list, MAIL_ERROR_NOTFOUND,
			T_MAIL_ERR_MAILBOX_NOT_FOUND(name));
		return -1;
	}

	return virtual_delete_nonrecursive(list, src, name);
}

static void virtual_notify_changes(struct mailbox *box)
{
	struct virtual_mailbox *mbox = (struct virtual_mailbox *)box;

	// FIXME
}

static int
virtual_list_iter_is_mailbox(struct mailbox_list_iterate_context *ctx
			     	ATTR_UNUSED,
			     const char *dir, const char *fname,
			     const char *mailbox_name ATTR_UNUSED,
			     enum mailbox_list_file_type type,
			     enum mailbox_info_flags *flags)
{
	const char *path, *maildir_path;
	struct stat st;
	int ret = 1;

	/* try to avoid stat() with these checks */
	if (type != MAILBOX_LIST_FILE_TYPE_DIR &&
	    type != MAILBOX_LIST_FILE_TYPE_SYMLINK &&
	    type != MAILBOX_LIST_FILE_TYPE_UNKNOWN) {
		/* it's a file */
		*flags |= MAILBOX_NOSELECT | MAILBOX_NOINFERIORS;
		return 0;
	}

	/* need to stat() then */
	path = t_strconcat(dir, "/", fname, NULL);
	if (stat(path, &st) == 0) {
		if (!S_ISDIR(st.st_mode)) {
			/* non-directory */
			*flags |= MAILBOX_NOSELECT | MAILBOX_NOINFERIORS;
			ret = 0;
		} else if (st.st_nlink == 2) {
			/* no subdirectories */
			*flags |= MAILBOX_NOCHILDREN;
		} else if (*ctx->list->set.maildir_name != '\0') {
			/* non-default configuration: we have one directory
			   containing the mailboxes. if there are 3 links,
			   either this is a selectable mailbox without children
			   or non-selectable mailbox with children */
			if (st.st_nlink > 3)
				*flags |= MAILBOX_CHILDREN;
		} else {
			/* default configuration: all subdirectories are
			   child mailboxes. */
			if (st.st_nlink > 2)
				*flags |= MAILBOX_CHILDREN;
		}
	} else {
		/* non-selectable. probably either access denied, or symlink
		   destination not found. don't bother logging errors. */
		*flags |= MAILBOX_NOSELECT;
	}
	if ((*flags & MAILBOX_NOSELECT) == 0) {
		/* make sure it's a selectable mailbox */
		maildir_path = t_strconcat(path, "/"VIRTUAL_CONFIG_FNAME, NULL);
		if (stat(maildir_path, &st) < 0)
			*flags |= MAILBOX_NOSELECT;
	}
	return ret;
}

static int virtual_backend_uidmap_cmp(const void *key, const void *data)
{
	const uint32_t *uid = key;
	const struct virtual_backend_uidmap *map = data;

	return *uid < map->real_uid ? -1 :
		*uid > map->real_uid ? 1 : 0;
}

static bool
virtual_get_virtual_uid(struct mailbox *box, const char *backend_mailbox,
			uint32_t backend_uidvalidity,
			uint32_t backend_uid, uint32_t *uid_r)
{
	struct virtual_mailbox *mbox = (struct virtual_mailbox *)box;
	struct virtual_backend_box *bbox;
	struct mailbox_status status;
	const struct virtual_backend_uidmap *uids;
	unsigned int count;

	if (strcmp(mbox->vseq_lookup_prev_mailbox, backend_mailbox) == 0)
		bbox = mbox->vseq_lookup_prev_bbox;
	else {
		i_free(mbox->vseq_lookup_prev_mailbox);
		mbox->vseq_lookup_prev_mailbox = i_strdup(backend_mailbox);

		bbox = virtual_backend_box_lookup_name(mbox, backend_mailbox);
		mbox->vseq_lookup_prev_bbox = bbox;
	}
	if (bbox == NULL)
		return FALSE;

	mailbox_get_status(bbox->box, STATUS_UIDVALIDITY, &status);
	if (status.uidvalidity != backend_uidvalidity)
		return FALSE;

	uids = array_get(&bbox->uids, &count);
	uids = bsearch(&backend_uid, uids, count, sizeof(*uids),
		       virtual_backend_uidmap_cmp);
	if (uids == NULL)
		return FALSE;

	*uid_r = uids->virtual_uid;
	return TRUE;
}

static void
virtual_get_virtual_backend_boxes(struct mailbox *box,
				  ARRAY_TYPE(mailboxes) *mailboxes,
				  bool only_with_msgs)
{
	struct virtual_mailbox *mbox = (struct virtual_mailbox *)box;
	struct virtual_backend_box *const *bboxes;
	unsigned int i, count;

	bboxes = array_get(&mbox->backend_boxes, &count);
	for (i = 0; i < count; i++) {
		if (!only_with_msgs || array_count(&bboxes[i]->uids) > 0)
			array_append(mailboxes, &bboxes[i]->box, 1);
	}
}

static void
virtual_get_virtual_box_patterns(struct mailbox *box,
				 ARRAY_TYPE(mailbox_virtual_patterns) *includes,
				 ARRAY_TYPE(mailbox_virtual_patterns) *excludes)
{
	struct virtual_mailbox *mbox = (struct virtual_mailbox *)box;

	array_append_array(includes, &mbox->list_include_patterns);
	array_append_array(excludes, &mbox->list_exclude_patterns);
}

static void virtual_class_init(void)
{
	virtual_transaction_class_init();
}

static void virtual_class_deinit(void)
{
	virtual_transaction_class_deinit();
}

static void virtual_storage_add_list(struct mail_storage *storage ATTR_UNUSED,
				     struct mailbox_list *list)
{
	struct virtual_mailbox_list *mlist;

	mlist = p_new(list->pool, struct virtual_mailbox_list, 1);
	mlist->module_ctx.super = list->v;

	list->ns->flags |= NAMESPACE_FLAG_NOQUOTA;

	list->v.iter_is_mailbox = virtual_list_iter_is_mailbox;
	list->v.delete_mailbox = virtual_list_delete_mailbox;

	MODULE_CONTEXT_SET(list, virtual_mailbox_list_module, mlist);
}

struct mail_storage virtual_storage = {
	MEMBER(name) VIRTUAL_STORAGE_NAME,
	MEMBER(class_flags) 0,

	{
		NULL,
		virtual_class_init,
		virtual_class_deinit,
		virtual_storage_alloc,
		NULL,
		index_storage_destroy,
		virtual_storage_add_list,
		virtual_storage_get_list_settings,
		NULL,
		virtual_mailbox_open,
		virtual_mailbox_create,
		NULL
	}
};

struct mailbox virtual_mailbox = {
	MEMBER(name) NULL, 
	MEMBER(storage) NULL, 
	MEMBER(list) NULL,

	{
		index_storage_is_readonly,
		index_storage_allow_new_keywords,
		index_storage_mailbox_enable,
		virtual_storage_mailbox_close,
		index_storage_get_status,
		NULL,
		NULL,
		virtual_storage_sync_init,
		index_mailbox_sync_next,
		index_mailbox_sync_deinit,
		NULL,
		virtual_notify_changes,
		index_transaction_begin,
		index_transaction_commit,
		index_transaction_rollback,
		index_transaction_set_max_modseq,
		index_keywords_create,
		index_keywords_free,
		index_keyword_is_valid,
		index_storage_get_seq_range,
		index_storage_get_uid_range,
		index_storage_get_expunged_uids,
		virtual_get_virtual_uid,
		virtual_get_virtual_backend_boxes,
		virtual_get_virtual_box_patterns,
		virtual_mail_alloc,
		index_header_lookup_init,
		index_header_lookup_deinit,
		virtual_search_init,
		virtual_search_deinit,
		virtual_search_next_nonblock,
		virtual_search_next_update_seq,
		virtual_save_alloc,
		virtual_save_begin,
		virtual_save_continue,
		virtual_save_finish,
		virtual_save_cancel,
		mail_storage_copy,
		index_storage_is_inconsistent
	}
};
