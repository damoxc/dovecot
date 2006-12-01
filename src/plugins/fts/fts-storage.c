/* Copyright (C) 2006 Timo Sirainen */

#include "lib.h"
#include "array.h"
#include "str.h"
#include "istream.h"
#include "message-parser.h"
#include "message-decoder.h"
#include "mail-search.h"
#include "mail-storage-private.h"
#include "fts-api-private.h"
#include "fts-plugin.h"

#include <stdlib.h>

#define FTS_CONTEXT(obj) \
	*((void **)array_idx_modifiable(&(obj)->module_contexts, \
					fts_storage_module_id))

struct fts_mailbox {
	struct mailbox_vfuncs super;
	struct fts_backend *backend;
};

struct fts_search_context {
	ARRAY_TYPE(seq_range) result;
	unsigned int result_pos;
};

struct fts_transaction_context {
	bool expunges;
};

struct fts_mail {
	struct mail_vfuncs super;
};

static unsigned int fts_storage_module_id = 0;
static bool fts_storage_module_id_set = FALSE;

static int fts_mailbox_close(struct mailbox *box)
{
	struct fts_mailbox *fbox = FTS_CONTEXT(box);
	int ret;

	fts_backend_deinit(fbox->backend);

	ret = fbox->super.close(box);
	i_free(fbox);
	return ret;
}

static int uid_range_to_seq(struct mailbox *box,
			    ARRAY_TYPE(seq_range) *uid_range,
			    ARRAY_TYPE(seq_range) *seq_range)
{
	const struct seq_range *range;
	struct seq_range new_range;
	unsigned int i, count;

	range = array_get(uid_range, &count);
	i_array_init(seq_range, count);
	for (i = 0; i < count; i++) {
		if (mailbox_get_uids(box, range[i].seq1, range[i].seq2,
				     &new_range.seq1, &new_range.seq2) < 0) {
			array_free(seq_range);
			return -1;
		}

		if (new_range.seq1 != 0)
			array_append(seq_range, &new_range, 1);
	}
	return 0;
}

struct fts_storage_build_context {
	struct fts_backend_build_context *build;
	uint32_t uid;
	string_t *headers;
	bool save_part;
};

static int fts_build_mail_flush(struct fts_storage_build_context *ctx)
{
	if (str_len(ctx->headers) == 0)
		return 1;

	if (fts_backend_build_more(ctx->build, ctx->uid, str_data(ctx->headers),
				   str_len(ctx->headers)) < 0)
		return -1;

	str_truncate(ctx->headers, 0);
	return 1;
}

static bool fts_build_update_save_part(struct fts_storage_build_context *ctx,
				       const struct message_block *block)
{
	/* we'll index only text/xxx and message/rfc822 parts for now */
	if ((block->part->flags &
	     (MESSAGE_PART_FLAG_TEXT |
	      MESSAGE_PART_FLAG_MESSAGE_RFC822)) == 0)
		return FALSE;

	ctx->save_part = TRUE;
	return TRUE;
}

static int fts_build_mail_header(struct fts_storage_build_context *ctx,
				 const struct message_block *block)
{
	const struct message_header_line *hdr = block->hdr;

	/* hdr->full_value is always set because we get the block from
	   message_decoder */
	str_append(ctx->headers, hdr->name);
	str_append_n(ctx->headers, hdr->middle, hdr->middle_len);
	str_append_n(ctx->headers, hdr->full_value, hdr->full_value_len);
	if (!hdr->no_newline)
		str_append_c(ctx->headers, '\n');

	if (!ctx->save_part) {
		if (strcasecmp(hdr->name, "Content-Type") == 0) {
			if (!fts_build_update_save_part(ctx, block))
				return 0;
		}
		return 1;
	}

	return fts_build_mail_flush(ctx);
}

static int
fts_build_mail(struct fts_storage_build_context *ctx, struct mail *mail)
{
	struct istream *input;
	struct message_parser_ctx *parser;
	struct message_decoder_context *decoder;
	struct message_block raw_block, block;
	struct message_part *prev_part, *skip_part;
	int ret;

	ctx->uid = mail->uid;

	input = mail_get_stream(mail, NULL, NULL);
	if (input == NULL)
		return -1;

	prev_part = skip_part = NULL;
	parser = message_parser_init(pool_datastack_create(), input);
	decoder = message_decoder_init();
	for (;;) {
		ret = message_parser_parse_next_block(parser, &raw_block);
		i_assert(ret != 0);
		if (ret < 0) {
			if (input->stream_errno == 0)
				ret = 0;
			break;
		}
		if (raw_block.part == skip_part)
			continue;

		if (!message_decoder_decode_next_block(decoder, &raw_block,
						       &block))
			continue;

		if (block.part != prev_part &&
		    (block.hdr != NULL || block.size != 0)) {
			str_truncate(ctx->headers, 0);
			ctx->save_part = FALSE;
			prev_part = block.part;
			skip_part = NULL;
		}

		if (block.hdr != NULL) {
			ret = fts_build_mail_header(ctx, &block);
			if (ret < 0)
				break;
			if (ret == 0)
				skip_part = raw_block.part;
		} else if (block.size == 0) {
			/* end of headers */
			if (fts_build_update_save_part(ctx, &block)) {
				ret = fts_build_mail_flush(ctx);
				if (ret < 0)
					break;
			}
		} else {
			if (fts_backend_build_more(ctx->build, mail->uid,
						   block.data,
						   block.size) < 0) {
				ret = -1;
				break;
			}
		}
	}
	(void)message_parser_deinit(&parser);
	message_decoder_deinit(&decoder);
	return ret;
}

static int fts_build_new(struct mailbox_transaction_context *t)
{
	struct fts_mailbox *fbox = FTS_CONTEXT(t->box);
	struct fts_storage_build_context ctx;
	struct mail_search_context *search_ctx;
	struct mail_search_seqset seqset;
	struct mail_search_arg search_arg;
	struct mail *mail;
	uint32_t last_uid, last_uid_locked;
	int ret = 0;

	if (fts_backend_get_last_uid(fbox->backend, &last_uid) < 0)
		return -1;

	memset(&seqset, 0, sizeof(seqset));
	if (mailbox_get_uids(t->box, last_uid+1, (uint32_t)-1,
			     &seqset.seq1, &seqset.seq2) < 0)
		return -1;
	if (seqset.seq1 == 0) {
		/* no new messages */
		return 0;
	}

	memset(&ctx, 0, sizeof(ctx));
	ctx.build = fts_backend_build_init(fbox->backend, &last_uid_locked);
	if (last_uid != last_uid_locked) {
		/* changed, need to get again the sequences */
		i_assert(last_uid < last_uid_locked);

		last_uid = last_uid_locked;
		if (mailbox_get_uids(t->box, last_uid+1, (uint32_t)-1,
				     &seqset.seq1, &seqset.seq2) < 0) {
			(void)fts_backend_build_deinit(ctx.build);
			return -1;
		}
		if (seqset.seq1 == 0) {
			/* no new messages */
			(void)fts_backend_build_deinit(ctx.build);
			return 0;
		}
	}

	memset(&search_arg, 0, sizeof(search_arg));
	search_arg.type = SEARCH_SEQSET;
	search_arg.value.seqset = &seqset;

	ctx.headers = str_new(default_pool, 512);
	mail = mail_alloc(t, 0, NULL);
	search_ctx = mailbox_search_init(t, NULL, &search_arg, NULL);
	while (mailbox_search_next(search_ctx, mail) > 0) {
		if (fts_build_mail(&ctx, mail) < 0) {
			ret = -1;
			break;
		}
	}
	if (mailbox_search_deinit(&search_ctx) < 0)
		ret = -1;
	mail_free(&mail);

	if (fts_backend_build_deinit(ctx.build) < 0)
		ret = -1;
	str_free(&ctx.headers);
	return ret;
}

static struct mail_search_context *
fts_mailbox_search_init(struct mailbox_transaction_context *t,
			const char *charset, struct mail_search_arg *args,
			const enum mail_sort_type *sort_program)
{
	struct fts_mailbox *fbox = FTS_CONTEXT(t->box);
	struct mail_search_context *ctx;
	struct fts_search_context *fctx;
	ARRAY_TYPE(seq_range) uid_result;

	ctx = fbox->super.search_init(t, charset, args, sort_program);

	fctx = i_new(struct fts_search_context, 1);
	array_idx_set(&ctx->module_contexts, fts_storage_module_id, &fctx);

	/* FIXME: handle AND/OR. Maybe also header lookups? */
	while (args != NULL &&
	       args->type != SEARCH_BODY &&
	       args->type != SEARCH_TEXT)
		args = args->next;

	if (args != NULL) {
		if (fts_build_new(t) < 0)
			return ctx;

		i_array_init(&uid_result, 64);
		if (fts_backend_lookup(fbox->backend, args->value.str,
				       &uid_result) < 0) {
			/* failed, fallback to reading everything */
			array_free(&uid_result);
		}

		if (fbox->backend->definite_lookups) {
			args->match_always = TRUE;
			args->result = 1;
		}
		args = args->next;
		while (args != NULL) {
			if (args->type == SEARCH_BODY ||
			    args->type == SEARCH_TEXT) {
				if (fbox->backend->definite_lookups) {
					args->match_always = TRUE;
					args->result = 1;
				}
				if (fts_backend_filter(fbox->backend,
						       args->value.str,
						       &uid_result) < 0) {
					/* failed, but we already have limited
					   the search, so just ignore this */
					break;
				}
			}
			args = args->next;
		}

		if (array_is_created(&uid_result)) {
			(void)uid_range_to_seq(t->box, &uid_result,
					       &fctx->result);
			array_free(&uid_result);
		}
	}
	return ctx;
}

static int fts_mailbox_search_next_update_seq(struct mail_search_context *ctx)
{
	struct fts_mailbox *fbox = FTS_CONTEXT(ctx->transaction->box);
	struct fts_search_context *fctx = FTS_CONTEXT(ctx);
	struct seq_range *range;
	unsigned int count;
	uint32_t wanted_seq;
	int ret;

	if (!array_is_created(&fctx->result))
		return fbox->super.search_next_update_seq(ctx);

	do {
		range = array_get_modifiable(&fctx->result, &count);
		while (fctx->result_pos < count &&
		       ctx->seq > range[fctx->result_pos].seq2)
			fctx->result_pos++;

		if (fctx->result_pos == count)
			return 0;

		if (ctx->seq > range[fctx->result_pos].seq1)
			range[fctx->result_pos].seq1 = ctx->seq+1;
		else {
			ctx->seq = range[fctx->result_pos].seq1 - 1;

			if (fctx->result_pos < count &&
			    ctx->seq + 1 == range[fctx->result_pos].seq2)
				fctx->result_pos++;
			else
				range[fctx->result_pos].seq1++;
		}

		wanted_seq = ctx->seq + 1;
		ret = fbox->super.search_next_update_seq(ctx);
	} while (ret > 0 && wanted_seq != ctx->seq);

	return ret;
}

static int fts_mailbox_search_deinit(struct mail_search_context *ctx)
{
	struct fts_mailbox *fbox = FTS_CONTEXT(ctx->transaction->box);
	struct fts_search_context *fctx = FTS_CONTEXT(ctx);

	if (array_is_created(&fctx->result))
		array_free(&fctx->result);
	i_free(fctx);
	return fbox->super.search_deinit(ctx);
}

static int fts_mail_expunge(struct mail *_mail)
{
	struct mail_private *mail = (struct mail_private *)_mail;
	struct fts_mail *fmail = FTS_CONTEXT(mail);
	struct fts_mailbox *fbox = FTS_CONTEXT(_mail->box);

	if (fmail->super.expunge(_mail) < 0)
		return -1;

	fts_backend_expunge(fbox->backend, _mail);
	return 0;
}

static struct mail *
fts_mail_alloc(struct mailbox_transaction_context *t,
	       enum mail_fetch_field wanted_fields,
	       struct mailbox_header_lookup_ctx *wanted_headers)
{
	struct fts_mailbox *fbox = FTS_CONTEXT(t->box);
	struct fts_transaction_context *ft = FTS_CONTEXT(t);
	struct fts_mail *fmail;
	struct mail *_mail;
	struct mail_private *mail;

	_mail = fbox->super.mail_alloc(t, wanted_fields, wanted_headers);
	mail = (struct mail_private *)_mail;

	ft->expunges = TRUE;

	fmail = p_new(mail->pool, struct fts_mail, 1);
	fmail->super = mail->v;

	mail->v.expunge = fts_mail_expunge;
	array_idx_set(&mail->module_contexts, fts_storage_module_id, &fmail);
	return _mail;
}

static struct mailbox_transaction_context *
fts_transaction_begin(struct mailbox *box,
		      enum mailbox_transaction_flags flags)
{
	struct fts_mailbox *fbox = FTS_CONTEXT(box);
	struct mailbox_transaction_context *t;
	struct fts_transaction_context *ft;

	ft = i_new(struct fts_transaction_context, 1);

	t = fbox->super.transaction_begin(box, flags);
	array_idx_set(&t->module_contexts, fts_storage_module_id, &ft);
	return t;
}

static void fts_transaction_rollback(struct mailbox_transaction_context *t)
{
	struct mailbox *box = t->box;
	struct fts_mailbox *fbox = FTS_CONTEXT(box);
	struct fts_transaction_context *ft = FTS_CONTEXT(t);

	fbox->super.transaction_rollback(t);
	if (ft->expunges)
		fts_backend_expunge_finish(fbox->backend, box, FALSE);
	i_free(ft);
}

static int fts_transaction_commit(struct mailbox_transaction_context *t,
				  enum mailbox_sync_flags flags)
{
	struct mailbox *box = t->box;
	struct fts_mailbox *fbox = FTS_CONTEXT(box);
	struct fts_transaction_context *ft = FTS_CONTEXT(t);
	int ret;

	ret = fbox->super.transaction_commit(t, flags);
	if (ft->expunges)
		fts_backend_expunge_finish(fbox->backend, box, ret == 0);
	i_free(ft);
	return ret;
}

void fts_mailbox_opened(struct mailbox *box)
{
	struct fts_mailbox *fbox;
	struct fts_backend *backend;
	const char *env;

	if (fts_next_hook_mailbox_opened != NULL)
		fts_next_hook_mailbox_opened(box);

	env = getenv("FTS");
	if (env == NULL)
		return;

	backend = fts_backend_init(env, box);
	if (backend == NULL)
		return;

	fbox = i_new(struct fts_mailbox, 1);
	fbox->super = box->v;
	fbox->backend = backend;
	box->v.close = fts_mailbox_close;
	box->v.search_init = fts_mailbox_search_init;
	box->v.search_next_update_seq = fts_mailbox_search_next_update_seq;
	box->v.search_deinit = fts_mailbox_search_deinit;
	box->v.mail_alloc = fts_mail_alloc;
	box->v.transaction_begin = fts_transaction_begin;
	box->v.transaction_rollback = fts_transaction_rollback;
	box->v.transaction_commit = fts_transaction_commit;

	if (!fts_storage_module_id_set) {
		fts_storage_module_id = mail_storage_module_id++;
		fts_storage_module_id_set = TRUE;
	}

	array_idx_set(&box->module_contexts, fts_storage_module_id, &fbox);
}
