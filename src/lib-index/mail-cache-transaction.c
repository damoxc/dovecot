/* Copyright (c) 2003-2012 Dovecot authors, see the included COPYING file */

#include "lib.h"
#include "ioloop.h"
#include "array.h"
#include "buffer.h"
#include "module-context.h"
#include "file-cache.h"
#include "file-set-size.h"
#include "read-full.h"
#include "write-full.h"
#include "mail-cache-private.h"
#include "mail-index-transaction-private.h"

#include <stddef.h>
#include <sys/stat.h>

#define MAIL_CACHE_INIT_WRITE_BUFFER (1024*16)
#define MAIL_CACHE_MAX_WRITE_BUFFER (1024*256)

#define CACHE_TRANS_CONTEXT(obj) \
	MODULE_CONTEXT(obj, cache_mail_index_transaction_module)

struct mail_cache_transaction_ctx {
	union mail_index_transaction_module_context module_ctx;
	struct mail_index_transaction_vfuncs super;

	struct mail_cache *cache;
	struct mail_cache_view *view;
	struct mail_index_transaction *trans;

	uint32_t cache_file_seq;
	uint32_t first_new_seq;

	buffer_t *cache_data;
	ARRAY(uint32_t) cache_data_seq;
	uint32_t prev_seq;
	size_t last_rec_pos;

	uoff_t bytes_written;

	unsigned int tried_compression:1;
	unsigned int changes:1;
};

static MODULE_CONTEXT_DEFINE_INIT(cache_mail_index_transaction_module,
				  &mail_index_module_register);

static int mail_cache_transaction_lock(struct mail_cache_transaction_ctx *ctx);
static int mail_cache_link_locked(struct mail_cache *cache,
				  uint32_t old_offset, uint32_t new_offset);

static void mail_index_transaction_cache_reset(struct mail_index_transaction *t)
{
	struct mail_cache_transaction_ctx *ctx = CACHE_TRANS_CONTEXT(t);
	struct mail_index_transaction_vfuncs super = ctx->super;

	mail_cache_transaction_reset(ctx);
	super.reset(t);
}

static int
mail_index_transaction_cache_commit(struct mail_index_transaction *t,
				    struct mail_index_transaction_commit_result *result_r)
{
	struct mail_cache_transaction_ctx *ctx = CACHE_TRANS_CONTEXT(t);
	struct mail_index_transaction_vfuncs super = ctx->super;

	/* a failed cache commit isn't important enough to fail the entire
	   index transaction, so we'll just ignore it */
	(void)mail_cache_transaction_commit(&ctx);
	return super.commit(t, result_r);
}

static void
mail_index_transaction_cache_rollback(struct mail_index_transaction *t)
{
	struct mail_cache_transaction_ctx *ctx = CACHE_TRANS_CONTEXT(t);
	struct mail_index_transaction_vfuncs super = ctx->super;

	mail_cache_transaction_rollback(&ctx);
	super.rollback(t);
}

struct mail_cache_transaction_ctx *
mail_cache_get_transaction(struct mail_cache_view *view,
			   struct mail_index_transaction *t)
{
	struct mail_cache_transaction_ctx *ctx;

	ctx = !cache_mail_index_transaction_module.id.module_id_set ? NULL :
		CACHE_TRANS_CONTEXT(t);

	if (ctx != NULL)
		return ctx;

	ctx = i_new(struct mail_cache_transaction_ctx, 1);
	ctx->cache = view->cache;
	ctx->view = view;
	ctx->trans = t;

	i_assert(view->transaction == NULL);
	view->transaction = ctx;
	view->trans_view = mail_index_transaction_open_updated_view(t);

	ctx->super = t->v;
	t->v.reset = mail_index_transaction_cache_reset;
	t->v.commit = mail_index_transaction_cache_commit;
	t->v.rollback = mail_index_transaction_cache_rollback;

	MODULE_CONTEXT_SET(t, cache_mail_index_transaction_module, ctx);
	return ctx;
}

void mail_cache_transaction_reset(struct mail_cache_transaction_ctx *ctx)
{
	ctx->cache_file_seq = MAIL_CACHE_IS_UNUSABLE(ctx->cache) ? 0 :
		ctx->cache->hdr->file_seq;
	mail_index_ext_set_reset_id(ctx->trans, ctx->cache->ext_id,
				    ctx->cache_file_seq);

	if (ctx->cache_data != NULL)
		buffer_set_used_size(ctx->cache_data, 0);
	if (array_is_created(&ctx->cache_data_seq))
		array_clear(&ctx->cache_data_seq);
	ctx->prev_seq = 0;
	ctx->last_rec_pos = 0;

	ctx->changes = FALSE;
}

void mail_cache_transaction_rollback(struct mail_cache_transaction_ctx **_ctx)
{
	struct mail_cache_transaction_ctx *ctx = *_ctx;

	*_ctx = NULL;

	if (ctx->bytes_written > 0) {
		/* we already wrote to the cache file. we can't (or don't want
		   to) delete that data, so just mark it as deleted space */
		if (mail_cache_transaction_lock(ctx) > 0) {
			ctx->cache->hdr_copy.deleted_space +=
				ctx->bytes_written;
			(void)mail_cache_unlock(ctx->cache);
		}
	}

	MODULE_CONTEXT_UNSET(ctx->trans, cache_mail_index_transaction_module);

	ctx->view->transaction = NULL;
	ctx->view->trans_seq1 = ctx->view->trans_seq2 = 0;

	mail_index_view_close(&ctx->view->trans_view);
	if (ctx->cache_data != NULL)
		buffer_free(&ctx->cache_data);
	if (array_is_created(&ctx->cache_data_seq))
		array_free(&ctx->cache_data_seq);
	i_free(ctx);
}

static int
mail_cache_transaction_compress(struct mail_cache_transaction_ctx *ctx)
{
	struct mail_cache *cache = ctx->cache;
	struct mail_index_view *view;
	struct mail_index_transaction *trans;
	int ret;

	ctx->tried_compression = TRUE;

	cache->need_compress_file_seq =
		MAIL_CACHE_IS_UNUSABLE(cache) ? 0 : cache->hdr->file_seq;

	view = mail_index_view_open(cache->index);
	trans = mail_index_transaction_begin(view,
					MAIL_INDEX_TRANSACTION_FLAG_EXTERNAL);
	if (mail_cache_compress(cache, trans) < 0) {
		mail_index_transaction_rollback(&trans);
		ret = -1;
	} else {
		ret = mail_index_transaction_commit(&trans);
	}
	mail_index_view_close(&view);
	mail_cache_transaction_reset(ctx);
	return ret;
}

static void
mail_cache_transaction_open_if_needed(struct mail_cache_transaction_ctx *ctx)
{
	struct mail_cache *cache = ctx->cache;
	const struct mail_index_ext *ext;
	uint32_t idx;
	int i;

	if (!cache->opened) {
		(void)mail_cache_open_and_verify(cache);
		return;
	}

	/* see if we should try to reopen the cache file */
	for (i = 0;; i++) {
		if (MAIL_CACHE_IS_UNUSABLE(cache))
			return;

		if (!mail_index_map_get_ext_idx(cache->index->map,
						cache->ext_id, &idx)) {
			/* index doesn't have a cache extension, but the cache
			   file exists (corrupted indexes fixed?). fix it. */
			if (i == 2)
				break;
		} else {
			ext = array_idx(&cache->index->map->extensions, idx);
			if (ext->reset_id == cache->hdr->file_seq || i == 2)
				break;

			/* index offsets don't match the cache file */
			if (ext->reset_id > cache->hdr->file_seq) {
				/* the cache file appears to be too old.
				   reopening should help. */
				if (mail_cache_reopen(cache) != 0)
					break;
			}
		}

		/* cache file sequence might be broken. it's also possible
		   that it was just compressed and we just haven't yet seen
		   the changes in index. try if refreshing index helps.
		   if not, compress the cache file. */
		if (i == 0) {
			if (ctx->tried_compression)
				break;
			/* get the latest reset ID */
			if (mail_index_refresh(ctx->cache->index) < 0)
				return;
		} else {
			i_assert(i == 1);
			(void)mail_cache_transaction_compress(ctx);
		}
	}
}

static int mail_cache_transaction_lock(struct mail_cache_transaction_ctx *ctx)
{
	struct mail_cache *cache = ctx->cache;
	int ret;

	mail_cache_transaction_open_if_needed(ctx);

	if ((ret = mail_cache_lock(cache, FALSE)) <= 0) {
		if (ret < 0)
			return -1;

		if (!ctx->tried_compression && MAIL_CACHE_IS_UNUSABLE(cache)) {
			if (mail_cache_transaction_compress(ctx) < 0)
				return -1;
			return mail_cache_transaction_lock(ctx);
		} else {
			return 0;
		}
	}
	i_assert(!MAIL_CACHE_IS_UNUSABLE(cache));

	if (ctx->cache_file_seq == 0) {
		i_assert(ctx->cache_data == NULL ||
			 ctx->cache_data->used == 0);
		ctx->cache_file_seq = cache->hdr->file_seq;
	} else if (ctx->cache_file_seq != cache->hdr->file_seq) {
		if (mail_cache_unlock(cache) < 0)
			return -1;
		mail_cache_transaction_reset(ctx);
		return 0;
	}
	return 1;
}

static int
mail_cache_transaction_update_index(struct mail_cache_transaction_ctx *ctx,
				    uint32_t write_offset)
{
	struct mail_cache *cache = ctx->cache;
	const struct mail_cache_record *rec = ctx->cache_data->data;
	const uint32_t *seqs;
	uint32_t i, seq_count, old_offset;

	mail_index_ext_using_reset_id(ctx->trans, ctx->cache->ext_id,
				      ctx->cache_file_seq);

	/* write the cache_offsets to index file. records' prev_offset
	   is updated to point to old cache record when index is being
	   synced. */
	seqs = array_get(&ctx->cache_data_seq, &seq_count);
	for (i = 0; i < seq_count; i++) {
		mail_index_update_ext(ctx->trans, seqs[i], cache->ext_id,
				      &write_offset, &old_offset);
		if (old_offset != 0) {
			/* we added records for this message multiple
			   times in this same uncommitted transaction.
			   only the new one will be written to
			   transaction log, we need to do the linking
			   ourself here. */
			if (old_offset > write_offset) {
				if (mail_cache_link_locked(cache, old_offset,
							   write_offset) < 0)
					return -1;
			} else {
				/* if we're combining multiple transactions,
				   make sure the one with the smallest offset
				   is written into index. this is required for
				   non-file-mmaped cache to work properly. */
				mail_index_update_ext(ctx->trans, seqs[i],
						      cache->ext_id,
						      &old_offset, NULL);
				if (mail_cache_link_locked(cache, write_offset,
							   old_offset) < 0)
					return -1;
			}
		}

		write_offset += rec->size;
		rec = CONST_PTR_OFFSET(rec, rec->size);
	}
	return 0;
}

static int
mail_cache_transaction_flush(struct mail_cache_transaction_ctx *ctx)
{
	uint32_t write_offset;
	int ret;

	i_assert(!ctx->cache->locked);

	if (mail_cache_transaction_lock(ctx) <= 0)
		return -1;

	/* first write the actual data to cache file */
	i_assert(ctx->last_rec_pos <= ctx->cache_data->used);
	if (mail_cache_append(ctx->cache, ctx->cache_data->data,
			      ctx->last_rec_pos, &write_offset) < 0)
		ret = -1;
	else {
		/* update records' cache offsets to index */
		ctx->bytes_written += ctx->last_rec_pos;
		ret = mail_cache_transaction_update_index(ctx, write_offset);
	}
	if (mail_cache_unlock(ctx->cache) < 0)
		ret = -1;

	/* drop the written data from buffer */
	buffer_copy(ctx->cache_data, 0,
		    ctx->cache_data, ctx->last_rec_pos, (size_t)-1);
	buffer_set_used_size(ctx->cache_data,
			     ctx->cache_data->used - ctx->last_rec_pos);
	ctx->last_rec_pos = 0;

	array_clear(&ctx->cache_data_seq);
	return ret;
}

static void
mail_cache_transaction_update_last_rec(struct mail_cache_transaction_ctx *ctx)
{
	struct mail_cache_record *rec;
	void *data;
	size_t size;

	data = buffer_get_modifiable_data(ctx->cache_data, &size);
	rec = PTR_OFFSET(data, ctx->last_rec_pos);
	rec->size = size - ctx->last_rec_pos;
	i_assert(rec->size > sizeof(*rec));

	/* FIXME: here would be a good place to set prev_offset to
	   avoid doing it later, but avoid circular prev_offsets
	   when cache is updated multiple times within the same
	   transaction */

	array_append(&ctx->cache_data_seq, &ctx->prev_seq, 1);
	ctx->last_rec_pos = size;
}

static void
mail_cache_transaction_switch_seq(struct mail_cache_transaction_ctx *ctx)
{
	struct mail_cache_record new_rec;

	if (ctx->prev_seq != 0) {
		/* update previously added cache record's size */
		mail_cache_transaction_update_last_rec(ctx);
	} else if (ctx->cache_data == NULL) {
		ctx->cache_data =
			buffer_create_dynamic(default_pool,
					      MAIL_CACHE_INIT_WRITE_BUFFER);
		i_array_init(&ctx->cache_data_seq, 64);
	}

	memset(&new_rec, 0, sizeof(new_rec));
	buffer_append(ctx->cache_data, &new_rec, sizeof(new_rec));

	ctx->prev_seq = 0;
	ctx->changes = TRUE;
}

int mail_cache_transaction_commit(struct mail_cache_transaction_ctx **_ctx)
{
	struct mail_cache_transaction_ctx *ctx = *_ctx;
	int ret = 0;

	if (ctx->changes) {
		if (ctx->prev_seq != 0)
			mail_cache_transaction_update_last_rec(ctx);
		if (mail_cache_transaction_flush(ctx) < 0)
			ret = -1;
		else {
			/* successfully wrote everything */
			ctx->bytes_written = 0;
		}
		/* Here would be a good place to do fdatasync() to make sure
		   everything is written before offsets are updated to index.
		   However it slows down I/O unneededly and we're pretty good
		   at catching and fixing cache corruption, so we no longer do
		   it. */
	}
	mail_cache_transaction_rollback(_ctx);
	return ret;
}

static int
mail_cache_header_fields_write(struct mail_cache_transaction_ctx *ctx,
			       const buffer_t *buffer)
{
	struct mail_cache *cache = ctx->cache;
	uint32_t offset, hdr_offset;

	i_assert(cache->locked);

	if (mail_cache_append(cache, buffer->data, buffer->used, &offset) < 0)
		return -1;

	if (cache->index->fsync_mode == FSYNC_MODE_ALWAYS) {
		if (fdatasync(cache->fd) < 0) {
			mail_cache_set_syscall_error(cache, "fdatasync()");
			return -1;
		}
	}
	/* find offset to the previous header's "next_offset" field */
	if (mail_cache_header_fields_get_next_offset(cache, &hdr_offset) < 0)
		return -1;

	/* update the next_offset offset, so our new header will be found */
	offset = mail_index_uint32_to_offset(offset);
	if (mail_cache_write(cache, &offset, sizeof(offset), hdr_offset) < 0)
		return -1;

	if (hdr_offset == offsetof(struct mail_cache_header,
				   field_header_offset)) {
		/* we're adding the first field. hdr_copy needs to be kept
		   in sync so unlocking won't overwrite it. */
		cache->hdr_copy.field_header_offset = hdr_offset;
		cache->hdr_ro_copy.field_header_offset = hdr_offset;
	}
	return 0;
}

static void mail_cache_mark_adding(struct mail_cache *cache, bool set)
{
	unsigned int i;

	/* we want to avoid adding all the fields one by one to the cache file,
	   so just add all of them at once in here. the unused ones get dropped
	   later when compressing. */
	for (i = 0; i < cache->fields_count; i++) {
		if (set)
			cache->fields[i].used = TRUE;
		cache->fields[i].adding = set;
	}
}

static int mail_cache_header_add_field(struct mail_cache_transaction_ctx *ctx,
				       unsigned int field_idx)
{
	struct mail_cache *cache = ctx->cache;
	int ret;

	if (mail_cache_transaction_lock(ctx) <= 0) {
		if (MAIL_CACHE_IS_UNUSABLE(cache))
			return -1;

		/* if we compressed the cache, the field should be there now.
		   it's however possible that someone else just compressed it
		   and we only reopened the cache file. */
		if (cache->field_file_map[field_idx] != (uint32_t)-1)
			return 0;

		/* need to add it */
		if (mail_cache_transaction_lock(ctx) <= 0)
			return -1;
	}

	/* re-read header to make sure we don't lose any fields. */
	if (mail_cache_header_fields_read(cache) < 0) {
		(void)mail_cache_unlock(cache);
		return -1;
	}

	if (cache->field_file_map[field_idx] != (uint32_t)-1) {
		/* it was already added */
		if (mail_cache_unlock(cache) < 0)
			return -1;
		return 0;
	}

	T_BEGIN {
		buffer_t *buffer;

		buffer = buffer_create_dynamic(pool_datastack_create(), 256);
		mail_cache_header_fields_get(cache, buffer);
		ret = mail_cache_header_fields_write(ctx, buffer);
	} T_END;

	if (ret == 0) {
		/* we wrote all the headers, so there are no pending changes */
		cache->field_header_write_pending = FALSE;
		ret = mail_cache_header_fields_read(cache);
	}
	if (ret == 0 && cache->field_file_map[field_idx] == (uint32_t)-1) {
		mail_index_set_error(cache->index,
				     "Cache file %s: Newly added field got "
				     "lost unexpectedly", cache->filepath);
		ret = -1;
	}

	if (mail_cache_unlock(cache) < 0)
		ret = -1;
	return ret;
}

void mail_cache_add(struct mail_cache_transaction_ctx *ctx, uint32_t seq,
		    unsigned int field_idx, const void *data, size_t data_size)
{
	uint32_t file_field, data_size32;
	unsigned int fixed_size;
	size_t full_size;
	int ret;

	i_assert(field_idx < ctx->cache->fields_count);
	i_assert(data_size < (uint32_t)-1);

	if (ctx->cache->fields[field_idx].field.decision ==
	    (MAIL_CACHE_DECISION_NO | MAIL_CACHE_DECISION_FORCED))
		return;

	if (ctx->cache_file_seq == 0) {
		mail_cache_transaction_open_if_needed(ctx);
		if (!MAIL_CACHE_IS_UNUSABLE(ctx->cache))
			ctx->cache_file_seq = ctx->cache->hdr->file_seq;
	} else if (!MAIL_CACHE_IS_UNUSABLE(ctx->cache) &&
		   ctx->cache_file_seq != ctx->cache->hdr->file_seq) {
		/* cache was compressed within this transaction */
		mail_cache_transaction_reset(ctx);
	}

	file_field = ctx->cache->field_file_map[field_idx];
	if (MAIL_CACHE_IS_UNUSABLE(ctx->cache) || file_field == (uint32_t)-1) {
		/* we'll have to add this field to headers */
		mail_cache_mark_adding(ctx->cache, TRUE);
		ret = mail_cache_header_add_field(ctx, field_idx);
		mail_cache_mark_adding(ctx->cache, FALSE);
		if (ret < 0)
			return;

		if (ctx->cache_file_seq == 0)
			ctx->cache_file_seq = ctx->cache->hdr->file_seq;

		file_field = ctx->cache->field_file_map[field_idx];
		i_assert(file_field != (uint32_t)-1);
	}
	i_assert(ctx->cache_file_seq != 0);

	mail_cache_decision_add(ctx->view, seq, field_idx);

	fixed_size = ctx->cache->fields[field_idx].field.field_size;
	i_assert(fixed_size == (unsigned int)-1 || fixed_size == data_size);

	data_size32 = (uint32_t)data_size;

	if (ctx->prev_seq != seq) {
		mail_cache_transaction_switch_seq(ctx);
		ctx->prev_seq = seq;

		/* remember roughly what we have modified, so cache lookups can
		   look into transactions to see changes. */
		if (seq < ctx->view->trans_seq1 || ctx->view->trans_seq1 == 0)
			ctx->view->trans_seq1 = seq;
		if (seq > ctx->view->trans_seq2)
			ctx->view->trans_seq2 = seq;
	}

	/* remember that this value exists, in case we try to look it up */
	buffer_write(ctx->view->cached_exists_buf, field_idx,
		     &ctx->view->cached_exists_value, 1);

	full_size = (data_size + 3) & ~3;
	if (fixed_size == (unsigned int)-1)
		full_size += sizeof(data_size32);

	if (ctx->cache_data->used + full_size > MAIL_CACHE_MAX_WRITE_BUFFER &&
	    ctx->last_rec_pos > 0) {
		/* time to flush our buffer. if flushing fails because the
		   cache file had been compressed and was reopened, return
		   without adding the cached data since cache_data buffer
		   doesn't contain the cache_rec anymore. */
		if (mail_cache_transaction_flush(ctx) < 0) {
			/* make sure the transaction is reset, so we don't
			   constantly try to flush for each call to this
			   function */
			mail_cache_transaction_reset(ctx);
			return;
		}
	}

	buffer_append(ctx->cache_data, &file_field, sizeof(file_field));
	if (fixed_size == -1U) {
		buffer_append(ctx->cache_data, &data_size32,
			      sizeof(data_size32));
	}

	buffer_append(ctx->cache_data, data, data_size);
	if ((data_size & 3) != 0)
                buffer_append_zero(ctx->cache_data, 4 - (data_size & 3));
}

bool mail_cache_field_want_add(struct mail_cache_transaction_ctx *ctx,
			       uint32_t seq, unsigned int field_idx)
{
	enum mail_cache_decision_type decision;

	mail_cache_transaction_open_if_needed(ctx);

	decision = mail_cache_field_get_decision(ctx->view->cache, field_idx);
	decision &= ~MAIL_CACHE_DECISION_FORCED;
	switch (decision) {
	case MAIL_CACHE_DECISION_NO:
		return FALSE;
	case MAIL_CACHE_DECISION_TEMP:
		/* add it only if it's newer than what we would drop when
		   compressing */
		if (ctx->first_new_seq == 0) {
			ctx->first_new_seq =
				mail_cache_get_first_new_seq(ctx->view->view);
		}
		if (seq < ctx->first_new_seq)
			return FALSE;
		break;
	default:
		break;
	}

	return mail_cache_field_exists(ctx->view, seq, field_idx) == 0;
}

bool mail_cache_field_can_add(struct mail_cache_transaction_ctx *ctx,
			      uint32_t seq, unsigned int field_idx)
{
	enum mail_cache_decision_type decision;

	mail_cache_transaction_open_if_needed(ctx);

	decision = mail_cache_field_get_decision(ctx->view->cache, field_idx);
	if (decision == (MAIL_CACHE_DECISION_FORCED | MAIL_CACHE_DECISION_NO))
		return FALSE;

	return mail_cache_field_exists(ctx->view, seq, field_idx) == 0;
}

static int mail_cache_link_locked(struct mail_cache *cache,
				  uint32_t old_offset, uint32_t new_offset)
{
	new_offset += offsetof(struct mail_cache_record, prev_offset);
	return mail_cache_write(cache, &old_offset, sizeof(old_offset),
				new_offset);
}

int mail_cache_link(struct mail_cache *cache, uint32_t old_offset,
		    uint32_t new_offset)
{
	const struct mail_cache_record *rec;

	i_assert(cache->locked);

	if (MAIL_CACHE_IS_UNUSABLE(cache))
		return -1;

	/* this function is called for each added cache record (or cache
	   extension record update actually) with new_offset pointing to the
	   new record and old_offset pointing to the previous record.

	   we want to keep the old and new records linked so both old and new
	   cached data is found. normally they are already linked correctly.
	   the problem only comes when multiple processes are adding cache
	   records at the same time. we'd rather not lose those additions, so
	   force the linking order to be new_offset -> old_offset if it isn't
	   already. */
	if (mail_cache_map(cache, new_offset, sizeof(*rec)) < 0)
		return -1;
	if (new_offset + sizeof(*rec) > cache->mmap_length) {
		mail_cache_set_corrupted(cache,
			"Cache record offset %u points outside file",
			new_offset);
		return -1;
	}
	rec = CACHE_RECORD(cache, new_offset);
	if (rec->prev_offset == old_offset) {
		/* link is already correct */
		return 0;
	}

	if (mail_cache_link_locked(cache, old_offset, new_offset) < 0)
		return -1;

	cache->hdr_copy.continued_record_count++;
	cache->hdr_modified = TRUE;
	return 0;
}

static int mail_cache_delete_real(struct mail_cache *cache, uint32_t offset)
{
	const struct mail_cache_record *rec;
	struct mail_cache_loop_track loop_track;
	int ret = 0;

	i_assert(cache->locked);

	/* we'll only update the deleted_space in header. we can't really
	   do any actual deleting as other processes might still be using
	   the data. also it's actually useful as some index views are still
	   able to ask cached data from messages that have already been
	   expunged. */
	memset(&loop_track, 0, sizeof(loop_track));
	while (offset != 0 &&
	       (ret = mail_cache_get_record(cache, offset, &rec)) == 0) {
		if (mail_cache_track_loops(&loop_track, offset, rec->size)) {
			mail_cache_set_corrupted(cache,
						 "record list is circular");
			return -1;
		}

		cache->hdr_copy.deleted_space += rec->size;
		offset = rec->prev_offset;
	}
	return ret;
}

int mail_cache_delete(struct mail_cache *cache, uint32_t offset)
{
	int ret;

	i_assert(cache->locked);
	T_BEGIN {
		ret = mail_cache_delete_real(cache, offset);
	} T_END;
	cache->hdr_modified = TRUE;
	return ret;
}
