/* Copyright (c) 2007-2009 Dovecot authors, see the included COPYING file */

#include "lib.h"
#include "array.h"
#include "fdatasync-path.h"
#include "hex-binary.h"
#include "hex-dec.h"
#include "str.h"
#include "istream.h"
#include "istream-crlf.h"
#include "ostream.h"
#include "write-full.h"
#include "index-mail.h"
#include "mail-copy.h"
#include "dbox-save.h"
#include "mdbox-storage.h"
#include "mdbox-map.h"
#include "mdbox-file.h"
#include "mdbox-sync.h"

#include <stdlib.h>

struct dbox_save_mail {
	struct dbox_file_append_context *file_append;
	uint32_t seq;
	uint32_t append_offset;
};

struct mdbox_save_context {
	struct dbox_save_context ctx;

	struct mdbox_mailbox *mbox;
	struct mdbox_sync_context *sync_ctx;

	struct dbox_file_append_context *cur_file_append;
	struct dbox_map_append_context *append_ctx;

	ARRAY_TYPE(uint32_t) copy_map_uids;
	struct dbox_map_transaction_context *map_trans;

	ARRAY_DEFINE(mails, struct dbox_save_mail);
};

struct dbox_file *
mdbox_save_file_get_file(struct mailbox_transaction_context *t,
			 uint32_t seq, uoff_t *offset_r)
{
	struct mdbox_save_context *ctx =
		(struct mdbox_save_context *)t->save_ctx;
	const struct dbox_save_mail *mails, *mail;
	unsigned int count;

	mails = array_get(&ctx->mails, &count);
	i_assert(count > 0);
	i_assert(seq >= mails[0].seq);

	mail = &mails[seq - mails[0].seq];
	i_assert(mail->seq == seq);

	if (dbox_file_append_flush(mail->file_append) < 0)
		ctx->ctx.failed = TRUE;

	*offset_r = mail->append_offset;
	return mail->file_append->file;
}

struct mail_save_context *
mdbox_save_alloc(struct mailbox_transaction_context *t)
{
	struct index_transaction_context *it =
		(struct index_transaction_context *)t;
	struct mdbox_mailbox *mbox = (struct mdbox_mailbox *)t->box;
	struct mdbox_save_context *ctx =
		(struct mdbox_save_context *)t->save_ctx;

	i_assert((t->flags & MAILBOX_TRANSACTION_FLAG_EXTERNAL) != 0);

	if (ctx != NULL) {
		/* use the existing allocated structure */
		ctx->ctx.finished = FALSE;
		return &ctx->ctx.ctx;
	}

	ctx = i_new(struct mdbox_save_context, 1);
	ctx->ctx.ctx.transaction = t;
	ctx->ctx.trans = it->trans;
	ctx->mbox = mbox;
	ctx->append_ctx = dbox_map_append_begin(mbox->storage->map);
	i_array_init(&ctx->mails, 32);
	t->save_ctx = &ctx->ctx.ctx;
	return t->save_ctx;
}

int mdbox_save_begin(struct mail_save_context *_ctx, struct istream *input)
{
	struct mdbox_save_context *ctx = (struct mdbox_save_context *)_ctx;
	struct dbox_save_mail *save_mail;
	uoff_t mail_size, append_offset;

	/* get the size of the mail to be saved, if possible */
	if (i_stream_get_size(input, TRUE, &mail_size) <= 0)
		mail_size = 0;
	if (dbox_map_append_next(ctx->append_ctx, mail_size,
				 &ctx->cur_file_append,
				 &ctx->ctx.cur_output) < 0) {
		ctx->ctx.failed = TRUE;
		return -1;
	}
	i_assert(ctx->ctx.cur_output->offset <= (uint32_t)-1);
	append_offset = ctx->ctx.cur_output->offset;

	ctx->ctx.cur_file = ctx->cur_file_append->file;
	dbox_save_begin(&ctx->ctx, input);

	save_mail = array_append_space(&ctx->mails);
	save_mail->file_append = ctx->cur_file_append;
	save_mail->seq = ctx->ctx.seq;
	save_mail->append_offset = append_offset;
	return ctx->ctx.failed ? -1 : 0;
}

static int mdbox_save_mail_write_metadata(struct mdbox_save_context *ctx,
					  struct dbox_save_mail *mail)
{
	struct dbox_file *file = mail->file_append->file;
	struct dbox_message_header dbox_msg_hdr;
	uoff_t message_size;
	uint8_t guid_128[MAIL_GUID_128_SIZE];

	i_assert(file->msg_header_size == sizeof(dbox_msg_hdr));

	message_size = ctx->ctx.cur_output->offset -
		mail->append_offset - mail->file_append->file->msg_header_size;

	dbox_save_write_metadata(&ctx->ctx.ctx, ctx->ctx.cur_output,
				 ctx->mbox->ibox.box.name, guid_128);
	/* save the 128bit GUID to index so if the map index gets corrupted
	   we can still find the message */
	mail_index_update_ext(ctx->ctx.trans, ctx->ctx.seq,
			      ctx->mbox->guid_ext_id, guid_128, NULL);

	dbox_msg_header_fill(&dbox_msg_hdr, message_size);
	if (o_stream_pwrite(ctx->ctx.cur_output, &dbox_msg_hdr,
			    sizeof(dbox_msg_hdr), mail->append_offset) < 0) {
		dbox_file_set_syscall_error(file, "pwrite()");
		return -1;
	}
	return 0;
}

static int mdbox_save_finish_write(struct mail_save_context *_ctx)
{
	struct mdbox_save_context *ctx = (struct mdbox_save_context *)_ctx;
	struct dbox_save_mail *mails;

	ctx->ctx.finished = TRUE;
	if (ctx->ctx.cur_output == NULL)
		return -1;

	index_mail_cache_parse_deinit(_ctx->dest_mail,
				      _ctx->received_date, !ctx->ctx.failed);

	mails = array_idx_modifiable(&ctx->mails, array_count(&ctx->mails) - 1);
	if (!ctx->ctx.failed) T_BEGIN {
		if (mdbox_save_mail_write_metadata(ctx, mails) < 0)
			ctx->ctx.failed = TRUE;
		else
			dbox_map_append_finish(ctx->append_ctx);
	} T_END;

	i_stream_unref(&ctx->ctx.input);

	if (ctx->ctx.failed) {
		array_delete(&ctx->mails, array_count(&ctx->mails) - 1, 1);
		return -1;
	}
	return 0;
}

int mdbox_save_finish(struct mail_save_context *ctx)
{
	int ret;

	ret = mdbox_save_finish_write(ctx);
	index_save_context_free(ctx);
	return ret;
}

void mdbox_save_cancel(struct mail_save_context *_ctx)
{
	struct dbox_save_context *ctx = (struct dbox_save_context *)_ctx;

	ctx->failed = TRUE;
	(void)mdbox_save_finish(_ctx);
}

int mdbox_transaction_save_commit_pre(struct mail_save_context *_ctx)
{
	struct mdbox_save_context *ctx = (struct mdbox_save_context *)_ctx;
	struct mailbox_transaction_context *_t = _ctx->transaction;
	struct mdbox_mailbox *mbox = ctx->mbox;
	const struct mail_index_header *hdr;
	uint32_t first_map_uid, last_map_uid;

	i_assert(ctx->ctx.finished);

	/* lock the mailbox before map to avoid deadlocks */
	if (mdbox_sync_begin(mbox, MDBOX_SYNC_FLAG_NO_PURGE |
			     MDBOX_SYNC_FLAG_FORCE |
			     MDBOX_SYNC_FLAG_FSYNC, &ctx->sync_ctx) < 0) {
		mdbox_transaction_save_rollback(_ctx);
		return -1;
	}

	/* get map UIDs for messages saved to multi-files. they're written
	   to transaction log immediately within this function, but the map
	   is left locked. */
	if (dbox_map_append_assign_map_uids(ctx->append_ctx, &first_map_uid,
					    &last_map_uid) < 0) {
		mdbox_transaction_save_rollback(_ctx);
		return -1;
	}

	/* assign UIDs for new messages */
	hdr = mail_index_get_header(ctx->sync_ctx->sync_view);
	mail_index_append_finish_uids(ctx->ctx.trans, hdr->next_uid,
				      &_t->changes->saved_uids);

	/* add map_uids for all messages saved to multi-files */
	if (first_map_uid != 0) {
		struct mdbox_mail_index_record rec;
		const struct dbox_save_mail *mails;
		unsigned int i, count;
		uint32_t next_map_uid = first_map_uid;

		mdbox_update_header(mbox, ctx->ctx.trans, NULL);

		memset(&rec, 0, sizeof(rec));
		rec.save_date = ioloop_time;
		mails = array_get(&ctx->mails, &count);
		for (i = 0; i < count; i++) {
			rec.map_uid = next_map_uid++;
			i_assert(i == 0 ||
				 mails[i-1].append_offset != mails[i].append_offset);
			mail_index_update_ext(ctx->ctx.trans, mails[i].seq,
					      mbox->ext_id, &rec, NULL);
		}
		i_assert(next_map_uid == last_map_uid + 1);
	}

	/* increase map's refcount for copied mails */
	if (array_is_created(&ctx->copy_map_uids)) {
		ctx->map_trans =
			dbox_map_transaction_begin(mbox->storage->map, FALSE);
		if (dbox_map_update_refcounts(ctx->map_trans,
					      &ctx->copy_map_uids, 1) < 0) {
			mdbox_transaction_save_rollback(_ctx);
			return -1;
		}
	}

	if (ctx->ctx.mail != NULL)
		mail_free(&ctx->ctx.mail);

	_t->changes->uid_validity = hdr->uid_validity;
	return 0;
}

void mdbox_transaction_save_commit_post(struct mail_save_context *_ctx,
					struct mail_index_transaction_commit_result *result)
{
	struct mdbox_save_context *ctx = (struct mdbox_save_context *)_ctx;

	_ctx->transaction = NULL; /* transaction is already freed */

	mail_index_sync_set_commit_result(ctx->sync_ctx->index_sync_ctx,
					  result);

	/* finish writing the mailbox APPENDs */
	if (mdbox_sync_finish(&ctx->sync_ctx, TRUE) == 0) {
		if (ctx->map_trans != NULL)
			(void)dbox_map_transaction_commit(ctx->map_trans);
		/* commit only updates the sync tail offset, everything else
		   was already written at this point. */
		(void)dbox_map_append_commit(ctx->append_ctx);
	}
	dbox_map_append_free(&ctx->append_ctx);

	if (!ctx->mbox->storage->storage.storage.set->fsync_disable) {
		if (fdatasync_path(ctx->mbox->ibox.box.path) < 0) {
			i_error("fdatasync_path(%s) failed: %m",
				ctx->mbox->ibox.box.path);
		}
	}
	mdbox_transaction_save_rollback(_ctx);
}

void mdbox_transaction_save_rollback(struct mail_save_context *_ctx)
{
	struct mdbox_save_context *ctx = (struct mdbox_save_context *)_ctx;

	if (!ctx->ctx.finished)
		mdbox_save_cancel(&ctx->ctx.ctx);
	if (ctx->append_ctx != NULL)
		dbox_map_append_free(&ctx->append_ctx);
	if (ctx->map_trans != NULL)
		dbox_map_transaction_free(&ctx->map_trans);
	if (array_is_created(&ctx->copy_map_uids))
		array_free(&ctx->copy_map_uids);

	if (ctx->sync_ctx != NULL)
		(void)mdbox_sync_finish(&ctx->sync_ctx, FALSE);

	if (ctx->ctx.mail != NULL)
		mail_free(&ctx->ctx.mail);
	array_free(&ctx->mails);
	i_free(ctx);
}

int mdbox_copy(struct mail_save_context *_ctx, struct mail *mail)
{
	struct mdbox_save_context *ctx = (struct mdbox_save_context *)_ctx;
	struct mdbox_mailbox *src_mbox;
	struct mdbox_mail_index_record rec;
	const void *data;
	bool expunged;

	ctx->ctx.finished = TRUE;

	if (mail->box->storage != _ctx->transaction->box->storage)
		return mail_storage_copy(_ctx, mail);
	src_mbox = (struct mdbox_mailbox *)mail->box;

	memset(&rec, 0, sizeof(rec));
	rec.save_date = ioloop_time;
	if (mdbox_mail_lookup(src_mbox, src_mbox->ibox.view, mail->seq,
			      &rec.map_uid) < 0)
		return -1;

	/* remember the map_uid so we can later increase its refcount */
	if (!array_is_created(&ctx->copy_map_uids))
		i_array_init(&ctx->copy_map_uids, 32);
	array_append(&ctx->copy_map_uids, &rec.map_uid, 1);

	/* add message to mailbox index */
	dbox_save_add_to_index(&ctx->ctx);
	mail_index_update_ext(ctx->ctx.trans, ctx->ctx.seq,
			      ctx->mbox->ext_id, &rec, NULL);

	mail_index_lookup_ext(src_mbox->ibox.view, mail->seq,
			      src_mbox->guid_ext_id, &data, &expunged);
	if (data != NULL) {
		mail_index_update_ext(ctx->ctx.trans, ctx->ctx.seq,
				      ctx->mbox->guid_ext_id, data, NULL);
	}
	return 0;
}
