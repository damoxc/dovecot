/* Copyright (c) 2007-2009 Dovecot authors, see the included COPYING file */

#include "lib.h"
#include "array.h"
#include "ioloop.h"
#include "hex-binary.h"
#include "randgen.h"
#include "mkdir-parents.h"
#include "unlink-directory.h"
#include "unlink-old-files.h"
#include "index-mail.h"
#include "mail-index-modseq.h"
#include "mailbox-uidvalidity.h"
#include "dbox-mail.h"
#include "dbox-save.h"
#include "sdbox-file.h"
#include "sdbox-sync.h"
#include "sdbox-storage.h"

#include <stdio.h>
#include <unistd.h>
#include <sys/stat.h>

#define SDBOX_LIST_CONTEXT(obj) \
	MODULE_CONTEXT(obj, sdbox_mailbox_list_module)

struct sdbox_mailbox_list {
	union mailbox_list_module_context module_ctx;
};

extern struct mail_storage dbox_storage;
extern struct mailbox sdbox_mailbox;
extern struct dbox_storage_vfuncs sdbox_dbox_storage_vfuncs;

static MODULE_CONTEXT_DEFINE_INIT(sdbox_mailbox_list_module,
				  &mailbox_list_module_register);

static struct mail_storage *sdbox_storage_alloc(void)
{
	struct sdbox_storage *storage;
	pool_t pool;

	pool = pool_alloconly_create("dbox storage", 512+256);
	storage = p_new(pool, struct sdbox_storage, 1);
	storage->storage.v = sdbox_dbox_storage_vfuncs;
	storage->storage.storage = dbox_storage;
	storage->storage.storage.pool = pool;
	return &storage->storage.storage;
}

struct mailbox *
sdbox_mailbox_alloc(struct mail_storage *storage, struct mailbox_list *list,
		    const char *name, struct istream *input,
		    enum mailbox_flags flags)
{
	struct sdbox_mailbox *mbox;
	pool_t pool;

	/* dbox can't work without index files */
	flags &= ~MAILBOX_FLAG_NO_INDEX_FILES;

	pool = pool_alloconly_create("dbox mailbox", 1024+512);
	mbox = p_new(pool, struct sdbox_mailbox, 1);
	mbox->ibox.box = sdbox_mailbox;
	mbox->ibox.box.pool = pool;
	mbox->ibox.box.storage = storage;
	mbox->ibox.box.list = list;
	mbox->ibox.mail_vfuncs = &sdbox_mail_vfuncs;

	mbox->ibox.save_commit_pre = sdbox_transaction_save_commit_pre;
	mbox->ibox.save_commit_post = sdbox_transaction_save_commit_post;
	mbox->ibox.save_rollback = sdbox_transaction_save_rollback;

	index_storage_mailbox_alloc(&mbox->ibox, name, input, flags,
				    DBOX_INDEX_PREFIX);
	mail_index_set_fsync_types(mbox->ibox.index,
				   MAIL_INDEX_SYNC_TYPE_APPEND |
				   MAIL_INDEX_SYNC_TYPE_EXPUNGE);

	mbox->ibox.index_flags |= MAIL_INDEX_OPEN_FLAG_KEEP_BACKUPS |
		MAIL_INDEX_OPEN_FLAG_NEVER_IN_MEMORY;

	mbox->storage = (struct sdbox_storage *)storage;
	mbox->alt_path =
		p_strconcat(pool, list->set.alt_dir, "/",
			    list->set.maildir_name, NULL);
	mbox->hdr_ext_id =
		mail_index_ext_register(mbox->ibox.index, "dbox-hdr",
					sizeof(struct sdbox_index_header), 0, 0);
	return &mbox->ibox.box;
}

int sdbox_read_header(struct sdbox_mailbox *mbox,
		      struct sdbox_index_header *hdr)
{
	const void *data;
	size_t data_size;

	mail_index_get_header_ext(mbox->ibox.view, mbox->hdr_ext_id,
				  &data, &data_size);
	if (data_size < SDBOX_INDEX_HEADER_MIN_SIZE &&
	    (!mbox->creating || data_size != 0)) {
		mail_storage_set_critical(&mbox->storage->storage.storage,
			"dbox %s: Invalid dbox header size",
			mbox->ibox.box.path);
		return -1;
	}
	memset(hdr, 0, sizeof(*hdr));
	memcpy(hdr, data, I_MIN(data_size, sizeof(*hdr)));
	return 0;
}

void sdbox_update_header(struct sdbox_mailbox *mbox,
			 struct mail_index_transaction *trans,
			 const struct mailbox_update *update)
{
	struct sdbox_index_header hdr, new_hdr;

	if (sdbox_read_header(mbox, &hdr) < 0)
		memset(&hdr, 0, sizeof(hdr));

	new_hdr = hdr;

	if (update != NULL && !mail_guid_128_is_empty(update->mailbox_guid)) {
		memcpy(new_hdr.mailbox_guid, update->mailbox_guid,
		       sizeof(new_hdr.mailbox_guid));
	} else if (mail_guid_128_is_empty(new_hdr.mailbox_guid)) {
		mail_generate_guid_128(new_hdr.mailbox_guid);
	}

	if (memcmp(&hdr, &new_hdr, sizeof(hdr)) != 0) {
		mail_index_update_header_ext(trans, mbox->hdr_ext_id, 0,
					     &new_hdr, sizeof(new_hdr));
	}
}

static int sdbox_write_index_header(struct mailbox *box,
				    const struct mailbox_update *update)
{
	struct sdbox_mailbox *mbox = (struct sdbox_mailbox *)box;
	struct mail_index_transaction *trans;
	const struct mail_index_header *hdr;
	uint32_t uid_validity, uid_next;

	hdr = mail_index_get_header(mbox->ibox.view);
	trans = mail_index_transaction_begin(mbox->ibox.view, 0);
	sdbox_update_header(mbox, trans, update);

	if (update != NULL && update->uid_validity != 0)
		uid_validity = update->uid_validity;
	else if (hdr->uid_validity == 0) {
		/* set uidvalidity */
		uid_validity = dbox_get_uidvalidity_next(box->list);
	}

	if (hdr->uid_validity != uid_validity) {
		mail_index_update_header(trans,
			offsetof(struct mail_index_header, uid_validity),
			&uid_validity, sizeof(uid_validity), TRUE);
	}
	if (update != NULL && hdr->next_uid < update->min_next_uid) {
		uid_next = update->min_next_uid;
		mail_index_update_header(trans,
			offsetof(struct mail_index_header, next_uid),
			&uid_next, sizeof(uid_next), TRUE);
	}
	if (update != NULL && update->min_highest_modseq != 0 &&
	    mail_index_modseq_get_highest(mbox->ibox.view) <
	    					update->min_highest_modseq) {
		mail_index_update_highest_modseq(trans,
						 update->min_highest_modseq);
	}

	if (mail_index_transaction_commit(&trans) < 0) {
		mail_storage_set_internal_error(box->storage);
		mail_index_reset_error(mbox->ibox.index);
		return -1;
	}
	return 0;
}

static int sdbox_mailbox_create_indexes(struct mailbox *box,
					const struct mailbox_update *update)
{
	struct sdbox_mailbox *mbox = (struct sdbox_mailbox *)box;
	const char *origin;
	mode_t mode;
	gid_t gid;
	int ret;

	mailbox_list_get_dir_permissions(box->list, NULL, &mode, &gid, &origin);
	if (mkdir_parents_chgrp(box->path, mode, gid, origin) == 0) {
		/* create indexes immediately with the dbox header */
		if (index_storage_mailbox_open(box) < 0)
			return -1;
		mbox->creating = TRUE;
		ret = sdbox_write_index_header(box, update);
		mbox->creating = FALSE;
		if (ret < 0)
			return -1;
	} else if (errno != EEXIST) {
		if (!mail_storage_set_error_from_errno(box->storage)) {
			mail_storage_set_critical(box->storage,
				"mkdir(%s) failed: %m", box->path);
		}
		return -1;
	}
	return 0;
}

static void sdbox_storage_get_status_guid(struct mailbox *box,
					  struct mailbox_status *status_r)
{
	struct sdbox_mailbox *mbox = (struct sdbox_mailbox *)box;
	struct sdbox_index_header hdr;

	if (sdbox_read_header(mbox, &hdr) < 0)
		memset(&hdr, 0, sizeof(hdr));

	if (mail_guid_128_is_empty(hdr.mailbox_guid)) {
		/* regenerate it */
		if (sdbox_write_index_header(box, NULL) < 0 ||
		    sdbox_read_header(mbox, &hdr) < 0)
			return;
	}
	memcpy(status_r->mailbox_guid, hdr.mailbox_guid,
	       sizeof(status_r->mailbox_guid));
}

static void
dbox_storage_get_status(struct mailbox *box, enum mailbox_status_items items,
			struct mailbox_status *status_r)
{
	index_storage_get_status(box, items, status_r);

	if ((items & STATUS_GUID) != 0)
		sdbox_storage_get_status_guid(box, status_r);
}

static int
dbox_mailbox_update(struct mailbox *box, const struct mailbox_update *update)
{
	if (!box->opened) {
		if (index_storage_mailbox_open(box) < 0)
			return -1;
	}
	return sdbox_write_index_header(box, update);
}

static int
sdbox_list_delete_mailbox(struct mailbox_list *list, const char *name)
{
	struct sdbox_mailbox_list *mlist = SDBOX_LIST_CONTEXT(list);
	const char *trash_dest;
	int ret;

	/* Make sure the indexes are closed before trying to delete the
	   directory that contains them. It can still fail with some NFS
	   implementations if indexes are opened by another session, but
	   that can't really be helped. */
	index_storage_destroy_unrefed();

	/* delete the index and control directories */
	if (mlist->module_ctx.super.delete_mailbox(list, name) < 0)
		return -1;

	if ((ret = dbox_list_delete_mailbox1(list, name, &trash_dest)) < 0)
		return -1;
	return dbox_list_delete_mailbox2(list, name, ret, trash_dest);
}

static int
sdbox_list_rename_mailbox(struct mailbox_list *oldlist, const char *oldname,
			  struct mailbox_list *newlist, const char *newname,
			  bool rename_children)
{
	struct sdbox_mailbox_list *oldmlist = SDBOX_LIST_CONTEXT(oldlist);

	if (oldmlist->module_ctx.super.
	    		rename_mailbox(oldlist, oldname, newlist, newname,
				       rename_children) < 0)
		return -1;
	return dbox_list_rename_mailbox(oldlist, oldname, newlist, newname,
					rename_children);
}

static void sdbox_storage_add_list(struct mail_storage *storage ATTR_UNUSED,
				   struct mailbox_list *list)
{
	struct sdbox_mailbox_list *mlist;

	mlist = p_new(list->pool, struct sdbox_mailbox_list, 1);
	mlist->module_ctx.super = list->v;

	list->v.iter_is_mailbox = dbox_list_iter_is_mailbox;
	list->v.delete_mailbox = sdbox_list_delete_mailbox;
	list->v.rename_mailbox = sdbox_list_rename_mailbox;
	list->v.rename_mailbox_pre = dbox_list_rename_mailbox_pre;

	MODULE_CONTEXT_SET(list, sdbox_mailbox_list_module, mlist);
}

struct mail_storage dbox_storage = {
	.name = SDBOX_STORAGE_NAME,
	.class_flags = 0,

	.v = {
                NULL,
		sdbox_storage_alloc,
		NULL,
		index_storage_destroy,
		sdbox_storage_add_list,
		dbox_storage_get_list_settings,
		NULL,
		sdbox_mailbox_alloc,
		NULL
	}
};

struct mailbox sdbox_mailbox = {
	.v = {
		index_storage_is_readonly,
		index_storage_allow_new_keywords,
		index_storage_mailbox_enable,
		dbox_mailbox_open,
		index_storage_mailbox_close,
		dbox_mailbox_create,
		dbox_mailbox_update,
		dbox_storage_get_status,
		NULL,
		NULL,
		sdbox_storage_sync_init,
		index_mailbox_sync_next,
		index_mailbox_sync_deinit,
		NULL,
		dbox_notify_changes,
		index_transaction_begin,
		index_transaction_commit,
		index_transaction_rollback,
		index_transaction_set_max_modseq,
		index_keywords_create,
		index_keywords_create_from_indexes,
		index_keywords_ref,
		index_keywords_unref,
		index_keyword_is_valid,
		index_storage_get_seq_range,
		index_storage_get_uid_range,
		index_storage_get_expunges,
		NULL,
		NULL,
		NULL,
		dbox_mail_alloc,
		index_header_lookup_init,
		index_header_lookup_deinit,
		index_storage_search_init,
		index_storage_search_deinit,
		index_storage_search_next_nonblock,
		index_storage_search_next_update_seq,
		sdbox_save_alloc,
		sdbox_save_begin,
		dbox_save_continue,
		sdbox_save_finish,
		sdbox_save_cancel,
		sdbox_copy,
		index_storage_is_inconsistent
	}
};

struct dbox_storage_vfuncs sdbox_dbox_storage_vfuncs = {
	dbox_file_free,
	sdbox_file_create_fd,
	sdbox_mail_open,
	sdbox_mailbox_create_indexes
};
