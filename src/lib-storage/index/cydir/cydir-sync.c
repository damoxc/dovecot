/* Copyright (c) 2007-2007 Dovecot authors, see the included COPYING file */

#include "lib.h"
#include "ioloop.h"
#include "str.h"
#include "cydir-storage.h"
#include "cydir-sync.h"

static void cydir_sync_set_uidvalidity(struct cydir_sync_context *ctx)
{
	uint32_t uid_validity = ioloop_time;

	mail_index_update_header(ctx->trans,
		offsetof(struct mail_index_header, uid_validity),
		&uid_validity, sizeof(uid_validity), TRUE);
	ctx->uid_validity = uid_validity;
}

static string_t *cydir_get_path_prefix(struct cydir_mailbox *mbox)
{
	string_t *path = str_new(default_pool, 256);
	const char *dir;

	dir = mailbox_list_get_path(mbox->storage->storage.list,
				    mbox->ibox.box.name,
				    MAILBOX_LIST_PATH_TYPE_MAILBOX);
	str_append(path, dir);
	str_append_c(path, '/');
	return path;
}

static void
cydir_sync_expunge(struct cydir_sync_context *ctx, uint32_t seq1, uint32_t seq2)
{
	struct mailbox *box = &ctx->mbox->ibox.box;
	uint32_t uid;

	if (ctx->path == NULL) {
		ctx->path = cydir_get_path_prefix(ctx->mbox);
		ctx->path_dir_prefix_len = str_len(ctx->path);
	}

	for (; seq1 <= seq2; seq1++) {
		mail_index_lookup_uid(ctx->sync_view, seq1, &uid);

		str_truncate(ctx->path, ctx->path_dir_prefix_len);
		str_printfa(ctx->path, "%u.", uid);
		if (unlink(str_c(ctx->path)) == 0) {
			if (box->v.sync_notify != NULL) {
				box->v.sync_notify(box, uid,
						   MAILBOX_SYNC_TYPE_EXPUNGE);
			}
			mail_index_expunge(ctx->trans, seq1);
		} else if (errno != ENOENT) {
			mail_storage_set_critical(&ctx->mbox->storage->storage,
				"unlink(%s) failed: %m", str_c(ctx->path));
			/* continue anyway */
		}
	}
}

static void cydir_sync_index(struct cydir_sync_context *ctx)
{
	struct mailbox *box = &ctx->mbox->ibox.box;
	const struct mail_index_header *hdr;
	struct mail_index_sync_rec sync_rec;
	uint32_t seq1, seq2;

	hdr = mail_index_get_header(ctx->sync_view);
	if (hdr->uid_validity != 0)
		ctx->uid_validity = hdr->uid_validity;
	else
		cydir_sync_set_uidvalidity(ctx);

	/* mark the newly seen messages as recent */
	mail_index_lookup_uid_range(ctx->sync_view, hdr->first_recent_uid,
				    hdr->next_uid, &seq1, &seq2);
	if (seq1 != 0) {
		index_mailbox_set_recent_seq(&ctx->mbox->ibox, ctx->sync_view,
					     seq1, seq2);
	}

	while (mail_index_sync_next(ctx->index_sync_ctx, &sync_rec)) {
		mail_index_lookup_uid_range(ctx->sync_view,
					    sync_rec.uid1, sync_rec.uid2,
					    &seq1, &seq2);
		if (seq1 == 0) {
			/* already expunged, nothing to do. */
			continue;
		}

		switch (sync_rec.type) {
		case MAIL_INDEX_SYNC_TYPE_APPEND:
			/* don't care */
			break;
		case MAIL_INDEX_SYNC_TYPE_EXPUNGE:
			cydir_sync_expunge(ctx, seq1, seq2);
			break;
		case MAIL_INDEX_SYNC_TYPE_FLAGS:
		case MAIL_INDEX_SYNC_TYPE_KEYWORD_ADD:
		case MAIL_INDEX_SYNC_TYPE_KEYWORD_REMOVE:
		case MAIL_INDEX_SYNC_TYPE_KEYWORD_RESET:
			/* FIXME: should be bother calling sync_notify()? */
			break;
		}
	}

	if (box->v.sync_notify != NULL)
		box->v.sync_notify(box, 0, 0);
}

int cydir_sync_begin(struct cydir_mailbox *mbox,
		     struct cydir_sync_context **ctx_r)
{
	struct cydir_sync_context *ctx;
	enum mail_index_sync_flags sync_flags;

	ctx = i_new(struct cydir_sync_context, 1);
	ctx->mbox = mbox;

	sync_flags = MAIL_INDEX_SYNC_FLAG_FLUSH_DIRTY;
	if (!mbox->ibox.keep_recent)
		sync_flags |= MAIL_INDEX_SYNC_FLAG_DROP_RECENT;

	if (mail_index_sync_begin(mbox->ibox.index, &ctx->index_sync_ctx,
				  &ctx->sync_view, &ctx->trans,
				  sync_flags) < 0) {
		mail_storage_set_index_error(&mbox->ibox);
		i_free(ctx);
		return -1;
	}

	cydir_sync_index(ctx);
	*ctx_r = ctx;
	return 0;
}

int cydir_sync_finish(struct cydir_sync_context **_ctx, bool success)
{
	struct cydir_sync_context *ctx = *_ctx;
	int ret = success ? 0 : -1;

	*_ctx = NULL;
	if (success) {
		if (mail_index_sync_commit(&ctx->index_sync_ctx) < 0) {
			mail_storage_set_index_error(&ctx->mbox->ibox);
			ret = -1;
		}
	} else {
		mail_index_sync_rollback(&ctx->index_sync_ctx);
	}
	if (ctx->path != NULL)
		str_free(&ctx->path);
	i_free(ctx);
	return 0;
}

int cydir_sync(struct cydir_mailbox *mbox)
{
	struct cydir_sync_context *sync_ctx;

	if (cydir_sync_begin(mbox, &sync_ctx) < 0)
		return -1;

	return cydir_sync_finish(&sync_ctx, TRUE);
}

struct mailbox_sync_context *
cydir_storage_sync_init(struct mailbox *box, enum mailbox_sync_flags flags)
{
	struct cydir_mailbox *mbox = (struct cydir_mailbox *)box;
	int ret = 0;

	if (!box->opened)
		index_storage_mailbox_open(&mbox->ibox);

	if ((flags & MAILBOX_SYNC_FLAG_FAST) == 0 ||
	    mbox->ibox.sync_last_check + MAILBOX_FULL_SYNC_INTERVAL <=
	    ioloop_time)
		ret = cydir_sync(mbox);

	return index_mailbox_sync_init(box, flags, ret < 0);
}
