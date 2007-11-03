/* Copyright (c) 2007 Dovecot authors, see the included COPYING file */

#include "lib.h"
#include "array.h"
#include "dbox-storage.h"
#include "../maildir/maildir-uidlist.h"
#include "../maildir/maildir-keywords.h"
#include "dbox-index.h"
#include "dbox-file.h"
#include "dbox-sync.h"

#include <stdlib.h>
#include <dirent.h>

struct dbox_sync_rebuild_context {
	struct dbox_mailbox *mbox;
	struct dbox_index_append_context *append_ctx;
	struct mail_index_transaction *trans;

	struct maildir_uidlist *maildir_uidlist;
	struct maildir_keywords *mk;
};

static int dbox_sync_set_uidvalidity(struct dbox_sync_rebuild_context *ctx)
{
	uint32_t uid_validity;

	if (dbox_index_get_uid_validity(ctx->mbox->dbox_index,
					&uid_validity) < 0)
		return -1;

	mail_index_update_header(ctx->trans,
		offsetof(struct mail_index_header, uid_validity),
		&uid_validity, sizeof(uid_validity), TRUE);
	return 0;
}

static void dbox_sync_index_metadata(struct dbox_sync_rebuild_context *ctx,
				     struct dbox_file *file, uint32_t seq)
{
	const char *value;
	struct mail_keywords *keywords;
	enum mail_flags flags = 0;
	unsigned int i;

	value = dbox_file_metadata_get(file, DBOX_METADATA_FLAGS);
	if (value != NULL) {
		for (i = 0; value[i] != '\0'; i++) {
			if (value[i] != '0' && i < DBOX_METADATA_FLAGS_COUNT)
				flags |= dbox_mail_flags_map[i];
		}
		mail_index_update_flags(ctx->trans, seq, MODIFY_REPLACE, flags);
	}

	value = dbox_file_metadata_get(file, DBOX_METADATA_KEYWORDS);
	if (value != NULL) {
		t_push();
		keywords = mail_index_keywords_create(ctx->mbox->ibox.index,
						t_strsplit_spaces(value, " "));
		mail_index_update_keywords(ctx->trans, seq, MODIFY_REPLACE,
					   keywords);
		mail_index_keywords_free(&keywords);
		t_pop();
	}
}

static int dbox_sync_index_file_next(struct dbox_sync_rebuild_context *ctx,
				     struct dbox_file *file, uoff_t *offset)
{
	uint32_t seq, uid;
	uoff_t metadata_offset, physical_size;
	const char *path;
	bool expunged;
	int ret;

	path = dbox_file_get_path(file);
	ret = dbox_file_seek_next(file, offset, &uid, &physical_size);
	if (ret <= 0) {
		if (ret < 0)
			return -1;

		if (uid == 0 && (file->file_id & DBOX_FILE_ID_FLAG_UID) != 0) {
			/* EOF */
			return 0;
		}

		i_warning("%s: Ignoring broken file (header)", path);
		return 0;
	}
	if ((file->file_id & DBOX_FILE_ID_FLAG_UID) != 0 &&
	    uid != (file->file_id & ~DBOX_FILE_ID_FLAG_UID)) {
		i_warning("%s: Header contains wrong UID %u", path, uid);
		return 0;
	}
	if (file->maildir_file) {
		i_assert(uid == 0);
		if (!maildir_uidlist_get_uid(ctx->maildir_uidlist, file->fname,
					     &uid)) {
			/* FIXME: not in uidlist, give it an uid */
			return 0;
		}
		file->append_count = 1;
		file->last_append_uid = uid;
	}

	metadata_offset =
		dbox_file_get_metadata_offset(file, *offset, physical_size);
	ret = dbox_file_metadata_seek(file, metadata_offset, &expunged);
	if (ret <= 0) {
		if (ret < 0)
			return -1;
		i_warning("%s: Ignoring broken file (metadata)", path);
		return 0;
	}
	if (!expunged) {
		mail_index_append(ctx->trans, uid, &seq);
		file->maildir_append_seq = seq;
		dbox_sync_index_metadata(ctx, file, seq);
	}
	return 1;
}

static int
dbox_sync_index_uid_file(struct dbox_sync_rebuild_context *ctx,
			 const char *dir, const char *fname)
{
	struct dbox_file *file;
	unsigned long uid;
	char *p;
	uoff_t offset = 0;
	int ret;

	fname += sizeof(DBOX_MAIL_FILE_MULTI_PREFIX)-1;
	uid = strtoul(fname, &p, 10);
	if (*p != '\0' || uid == 0 || uid >= (uint32_t)-1) {
		i_warning("dbox %s: Ignoring invalid filename %s",
			  ctx->mbox->path, fname);
		return 0;
	}

	file = dbox_file_init(ctx->mbox, uid | DBOX_FILE_ID_FLAG_UID);
	file->current_path = i_strdup_printf("%s/%s", dir, fname);

	ret = dbox_sync_index_file_next(ctx, file, &offset) < 0 ? -1 : 0;
	dbox_file_unref(&file);
	return ret;
}

static int
dbox_sync_index_multi_file(struct dbox_sync_rebuild_context *ctx,
			   const char *dir, const char *fname)
{
	/* FIXME */
	return 0;
}

static int
dbox_sync_index_maildir_file(struct dbox_sync_rebuild_context *ctx,
			     const char *fname)
{
	struct dbox_file *file;
	uoff_t offset = 0;
	int ret;

	if (ctx->mbox->maildir_sync_keywords == NULL) {
		ctx->maildir_uidlist =
			maildir_uidlist_init_readonly(&ctx->mbox->ibox);
		ctx->mk = maildir_keywords_init_readonly(&ctx->mbox->ibox.box);
		ctx->mbox->maildir_sync_keywords =
			maildir_keywords_sync_init(ctx->mk,
						   ctx->mbox->ibox.index);

		if (maildir_uidlist_refresh(ctx->maildir_uidlist) < 0)
			return -1;
	}

	file = dbox_file_init_new_maildir(ctx->mbox, fname);
	if ((ret = dbox_sync_index_file_next(ctx, file, &offset)) > 0)
		dbox_index_append_file(ctx->append_ctx, file);
	dbox_file_unref(&file);
	return ret < 0 ? -1 : 0;
}

static int dbox_sync_index_rebuild_dir(struct dbox_sync_rebuild_context *ctx,
				       const char *path, bool primary)
{
	struct mail_storage *storage = ctx->mbox->ibox.box.storage;
	DIR *dir;
	struct dirent *d;
	int ret = 0;

	dir = opendir(path);
	if (dir == NULL) {
		if (errno == ENOENT) {
			ctx->mbox->ibox.mailbox_deleted = TRUE;
			return -1;
		}
		mail_storage_set_critical(storage,
			"opendir(%s) failed: %m", path);
		return -1;
	}
	errno = 0;
	for (; ret == 0 && (d = readdir(dir)) != NULL; errno = 0) {
		if (strncmp(d->d_name, DBOX_MAIL_FILE_UID_PREFIX,
			    sizeof(DBOX_MAIL_FILE_UID_PREFIX)-1) == 0)
			ret = dbox_sync_index_uid_file(ctx, path, d->d_name);
		else if (strncmp(d->d_name, DBOX_MAIL_FILE_MULTI_PREFIX,
				 sizeof(DBOX_MAIL_FILE_MULTI_PREFIX)-1) == 0)
			ret = dbox_sync_index_multi_file(ctx, path, d->d_name);
		else if (primary && strstr(d->d_name, ":2,") != NULL)
			ret = dbox_sync_index_maildir_file(ctx, d->d_name);
	}
	if (errno != 0) {
		mail_storage_set_critical(storage,
			"readdir(%s) failed: %m", path);
		ret = -1;
	}

	if (closedir(dir) < 0) {
		mail_storage_set_critical(storage,
			"closedir(%s) failed: %m", path);
		ret = -1;
	}
	return ret;
}

static int dbox_sync_index_rebuild_ctx(struct dbox_sync_rebuild_context *ctx)
{
	int ret;

	if (dbox_sync_set_uidvalidity(ctx) < 0)
		return -1;

	ret = dbox_sync_index_rebuild_dir(ctx, ctx->mbox->path, TRUE);
	if (ret < 0 || ctx->mbox->alt_path == NULL)
		return ret;

	return dbox_sync_index_rebuild_dir(ctx, ctx->mbox->alt_path, FALSE);
}

static void dbox_sync_update_maildir_ids(struct dbox_sync_rebuild_context *ctx)
{
	struct dbox_mail_index_record rec;
	struct dbox_file *const *files;
	unsigned int i, count;

	memset(&rec, 0, sizeof(rec));
	files = array_get(&ctx->mbox->open_files, &count);
	for (i = 0; i < count; i++) {
		if (!files[i]->maildir_file)
			continue;

		i_assert(files[i]->file_id != 0);
		rec.file_id = files[i]->file_id;
		mail_index_update_ext(ctx->trans, files[i]->maildir_append_seq,
				      ctx->mbox->dbox_ext_id, &rec, NULL);
	}
}

int dbox_sync_index_rebuild(struct dbox_mailbox *mbox)
{
	struct dbox_sync_rebuild_context ctx;
	struct mail_index_view *view;
	uint32_t seq;
	uoff_t offset;
	int ret;

	memset(&ctx, 0, sizeof(ctx));
	ctx.mbox = mbox;
	ctx.append_ctx = dbox_index_append_begin(mbox->dbox_index);
	view = mail_index_view_open(mbox->ibox.index);
	ctx.trans = mail_index_transaction_begin(view,
					MAIL_INDEX_TRANSACTION_FLAG_EXTERNAL);
	mail_index_reset(ctx.trans);

	if ((ret = dbox_sync_index_rebuild_ctx(&ctx)) < 0)
		mail_index_transaction_rollback(&ctx.trans);
	else {
		ret = dbox_index_append_assign_file_ids(ctx.append_ctx);
		if (ret == 0) {
			dbox_sync_update_maildir_ids(&ctx);
			ret = mail_index_transaction_commit(&ctx.trans,
							    &seq, &offset);
		}
	}
	mail_index_view_close(&view);

	if (ret == 0)
		ret = dbox_index_append_commit(&ctx.append_ctx);
	else
		dbox_index_append_rollback(&ctx.append_ctx);

	if (mbox->maildir_sync_keywords != NULL)
		maildir_keywords_sync_deinit(&mbox->maildir_sync_keywords);
	if (ctx.mk != NULL)
		maildir_keywords_deinit(&ctx.mk);
	if (ctx.maildir_uidlist != NULL)
		maildir_uidlist_deinit(&ctx.maildir_uidlist);
	return ret;
}
