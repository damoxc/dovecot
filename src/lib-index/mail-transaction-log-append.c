/* Copyright (c) 2003-2009 Dovecot authors, see the included COPYING file */

#include "lib.h"
#include "array.h"
#include "write-full.h"
#include "mail-index-private.h"
#include "mail-transaction-log-private.h"

void mail_transaction_log_append_add(struct mail_transaction_log_append_ctx *ctx,
				     enum mail_transaction_type type,
				     const void *data, size_t size)
{
	struct mail_transaction_header hdr;

	i_assert((type & MAIL_TRANSACTION_TYPE_MASK) != 0);
	i_assert((size % 4) == 0);

	if (size == 0)
		return;

	memset(&hdr, 0, sizeof(hdr));
	hdr.type = type;
	if (type == MAIL_TRANSACTION_EXPUNGE)
		hdr.type |= MAIL_TRANSACTION_EXPUNGE_PROT;
	if (ctx->external)
		hdr.type |= MAIL_TRANSACTION_EXTERNAL;
	hdr.size = sizeof(hdr) + size;
	hdr.size = mail_index_uint32_to_offset(hdr.size);

	buffer_append(ctx->output, &hdr, sizeof(hdr));
	buffer_append(ctx->output, data, size);

	if (mail_transaction_header_has_modseq(data,
					CONST_PTR_OFFSET(data, sizeof(hdr)),
					ctx->new_highest_modseq))
		ctx->new_highest_modseq++;
}

static int
log_buffer_move_to_memory(struct mail_transaction_log_append_ctx *ctx)
{
	struct mail_transaction_log_file *file = ctx->log->head;

	/* first we need to truncate this latest write so that log syncing
	   doesn't break */
	if (ftruncate(file->fd, file->sync_offset) < 0) {
		mail_index_file_set_syscall_error(file->log->index,
						  file->filepath,
						  "ftruncate()");
	}

	if (mail_index_move_to_memory(file->log->index) < 0)
		return -1;
	i_assert(MAIL_TRANSACTION_LOG_FILE_IN_MEMORY(file));

	i_assert(file->buffer_offset + file->buffer->used ==
		 file->sync_offset);
	buffer_append_buf(file->buffer, ctx->output, 0, (size_t)-1);
	file->sync_offset = file->buffer_offset + file->buffer->used;
	return 0;
}

static int log_buffer_write(struct mail_transaction_log_append_ctx *ctx)
{
	struct mail_transaction_log_file *file = ctx->log->head;
	struct mail_transaction_header *hdr;
	uint32_t first_size;

	if (MAIL_TRANSACTION_LOG_FILE_IN_MEMORY(file)) {
		if (file->buffer == NULL) {
			file->buffer = buffer_create_dynamic(default_pool, 4096);
			file->buffer_offset = sizeof(file->hdr);
		}
		buffer_append_buf(file->buffer, ctx->output, 0, (size_t)-1);
		file->sync_offset = file->buffer_offset + file->buffer->used;
		return 0;
	}

	/* size will be written later once everything is in disk */
	hdr = buffer_get_space_unsafe(ctx->output, 0, sizeof(*hdr));
	first_size = hdr->size;
	i_assert(first_size != 0);
	hdr->size = 0;

	if (pwrite_full(file->fd, ctx->output->data, ctx->output->used,
			file->sync_offset) < 0) {
		/* write failure, fallback to in-memory indexes. */
		hdr->size = first_size;
		mail_index_file_set_syscall_error(file->log->index,
						  file->filepath,
						  "pwrite_full()");
		return log_buffer_move_to_memory(ctx);
	}

	i_assert(!ctx->sync_includes_this ||
		 file->sync_offset + ctx->output->used ==
		 file->max_tail_offset);

	/* now that the whole transaction has been written, rewrite the first
	   record's size so the transaction becomes visible */
	if (pwrite_full(file->fd, &first_size, sizeof(uint32_t),
			file->sync_offset +
			offsetof(struct mail_transaction_header, size)) < 0) {
		mail_index_file_set_syscall_error(file->log->index,
						  file->filepath,
						  "pwrite_full()");
		return log_buffer_move_to_memory(ctx);
	}

	if ((ctx->want_fsync && !file->log->index->fsync_disable) ||
	    file->log->index->nfs_flush) {
		if (fdatasync(file->fd) < 0) {
			mail_index_file_set_syscall_error(file->log->index,
							  file->filepath,
							  "fdatasync()");
			return log_buffer_move_to_memory(ctx);
		}
	}

	/* FIXME: when we're relying on O_APPEND and someone else wrote a
	   transaction, we'll need to wait for it to commit its transaction.
	   if it crashes before doing that, we'll need to overwrite it with
	   a dummy record */

	file->sync_offset += ctx->output->used;
	return 0;
}

static void
log_append_sync_offset_if_needed(struct mail_transaction_log_append_ctx *ctx)
{
	struct mail_transaction_log_file *file = ctx->log->head;
	struct mail_transaction_header_update *u;
	struct mail_transaction_header *hdr;
	buffer_t *buf;
	uint32_t offset;

	if (file->max_tail_offset == file->sync_offset) {
		/* FIXME: when we remove exclusive log locking, we
		   can't rely on this. then write non-changed offset + check
		   real offset + rewrite the new offset if other transactions
		   weren't written in the middle */
		file->max_tail_offset += ctx->output->used +
			sizeof(*hdr) + sizeof(*u) + sizeof(offset);
		ctx->sync_includes_this = TRUE;
	}
	offset = file->max_tail_offset;

	if (file->saved_tail_offset == offset)
		return;
	i_assert(offset > file->saved_tail_offset);

	buf = buffer_create_static_hard(pool_datastack_create(),
					sizeof(*u) + sizeof(offset));
	u = buffer_append_space_unsafe(buf, sizeof(*u));
	u->offset = offsetof(struct mail_index_header, log_file_tail_offset);
	u->size = sizeof(offset);
	buffer_append(buf, &offset, sizeof(offset));

	mail_transaction_log_append_add(ctx, MAIL_TRANSACTION_HEADER_UPDATE,
					buf->data, buf->used);
}

static int
mail_transaction_log_append_locked(struct mail_transaction_log_append_ctx *ctx)
{
	struct mail_transaction_log_file *file = ctx->log->head;

	if (file->sync_offset < file->last_size) {
		/* there is some garbage at the end of the transaction log
		   (eg. previous write failed). remove it so reader doesn't
		   break because of it. */
		buffer_set_used_size(file->buffer,
				     file->sync_offset - file->buffer_offset);
		if (!MAIL_TRANSACTION_LOG_FILE_IN_MEMORY(file)) {
			if (ftruncate(file->fd, file->sync_offset) < 0) {
				mail_index_file_set_syscall_error(ctx->log->index,
					file->filepath, "ftruncate()");
			}
		}
	}

	if (ctx->append_sync_offset)
		log_append_sync_offset_if_needed(ctx);

	if (log_buffer_write(ctx) < 0)
		return -1;
	file->sync_highest_modseq = ctx->new_highest_modseq;
	return 0;
}

int mail_transaction_log_append_begin(struct mail_index *index, bool external,
				      struct mail_transaction_log_append_ctx **ctx_r)
{
	struct mail_transaction_log_append_ctx *ctx;

	if (!index->log_locked) {
		if (mail_transaction_log_lock_head(index->log) < 0)
			return -1;
	}
	ctx = i_new(struct mail_transaction_log_append_ctx, 1);
	ctx->log = index->log;
	ctx->output = buffer_create_dynamic(default_pool, 1024);
	ctx->external = external;

	*ctx_r = ctx;
	return 0;
}

int mail_transaction_log_append_commit(struct mail_transaction_log_append_ctx **_ctx)
{
	struct mail_transaction_log_append_ctx *ctx = *_ctx;
	struct mail_index *index = ctx->log->index;
	int ret = 0;

	*_ctx = NULL;

	if (ctx->output->used > 0)
		ret = mail_transaction_log_append_locked(ctx);

	if (!index->log_locked)
		mail_transaction_log_file_unlock(index->log->head);

	buffer_free(&ctx->output);
	i_free(ctx);
	return ret;
}
