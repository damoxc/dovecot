/* Copyright (c) 2006-2007 Dovecot authors, see the included COPYING file */

#include "lib.h"
#include "array.h"
#include "seq-range-array.h"
#include "mail-search.h"
#include "mail-storage-private.h"
#include "fts-api-private.h"
#include "fts-storage.h"

static void
uid_range_to_seqs(struct mailbox *box, const ARRAY_TYPE(seq_range) *uid_range,
		  ARRAY_TYPE(seq_range) *seq_range)
{
	const struct seq_range *range;
	struct seq_range new_range;
	unsigned int i, count;

	range = array_get(uid_range, &count);
	i_array_init(seq_range, count);
	for (i = 0; i < count; i++) {
		mailbox_get_uids(box, range[i].seq1, range[i].seq2,
				 &new_range.seq1, &new_range.seq2);
		if (new_range.seq1 != 0)
			array_append(seq_range, &new_range, 1);
	}
}

static void fts_uid_results_to_seq(struct fts_search_context *fctx)
{
	ARRAY_TYPE(seq_range) uid_range;

	uid_range = fctx->definite_seqs;
	i_array_init(&fctx->definite_seqs, array_count(&uid_range));
	uid_range_to_seqs(fctx->t->box, &uid_range, &fctx->definite_seqs);
	array_free(&uid_range);

	uid_range = fctx->maybe_seqs;
	i_array_init(&fctx->maybe_seqs, array_count(&uid_range));
	uid_range_to_seqs(fctx->t->box, &uid_range, &fctx->maybe_seqs);
	array_free(&uid_range);
}

static int fts_search_lookup_arg(struct fts_search_context *fctx,
				 struct mail_search_arg *arg, bool filter)
{
	struct fts_backend *backend;
	enum fts_lookup_flags flags = 0;
	const char *key;

	switch (arg->type) {
	case SEARCH_HEADER:
		/* we can filter out messages that don't have the header,
		   but we can't trust definite results list. */
		flags = FTS_LOOKUP_FLAG_HEADER;
		backend = fctx->fbox->backend_substr;
		key = arg->value.str;
		if (*key == '\0') {
			/* we're only checking the existence
			   of the header. */
			key = arg->hdr_field_name;
		}
		break;
	case SEARCH_TEXT:
	case SEARCH_TEXT_FAST:
		flags = FTS_LOOKUP_FLAG_HEADER;
	case SEARCH_BODY:
	case SEARCH_BODY_FAST:
		flags |= FTS_LOOKUP_FLAG_BODY;
		key = arg->value.str;
		backend = fctx->fbox->backend_fast != NULL &&
			(arg->type == SEARCH_TEXT_FAST ||
			 arg->type == SEARCH_BODY_FAST) ?
			fctx->fbox->backend_fast : fctx->fbox->backend_substr;
		break;
	default:
		/* can't filter this */
		i_assert(filter);
		return 0;
	}
	if (arg->not)
		flags |= FTS_LOOKUP_FLAG_INVERT;

	if (!backend->locked) {
		if (fts_backend_lock(backend) <= 0)
			return -1;
	}

	if (!filter) {
		return fts_backend_lookup(backend, key, flags,
					  &fctx->definite_seqs,
					  &fctx->maybe_seqs);
	} else {
		return fts_backend_filter(backend, key, flags,
					  &fctx->definite_seqs,
					  &fctx->maybe_seqs);
	}
}

void fts_search_lookup(struct fts_search_context *fctx)
{
	struct mail_search_arg *arg;
	int ret;

	if (fctx->best_arg == NULL)
		return;

	i_array_init(&fctx->definite_seqs, 64);
	i_array_init(&fctx->maybe_seqs, 64);

	/* start filtering with the best arg */
	ret = fts_search_lookup_arg(fctx, fctx->best_arg, FALSE);
	/* filter the rest */
	for (arg = fctx->args; arg != NULL && ret == 0; arg = arg->next) {
		if (arg != fctx->best_arg)
			ret = fts_search_lookup_arg(fctx, arg, TRUE);
	}

	if (fctx->fbox->backend_fast != NULL &&
	    fctx->fbox->backend_fast->locked)
		fts_backend_unlock(fctx->fbox->backend_fast);
	if (fctx->fbox->backend_substr != NULL &&
	    fctx->fbox->backend_substr->locked)
		fts_backend_unlock(fctx->fbox->backend_substr);

	if (ret == 0) {
		fctx->seqs_set = TRUE;
		fts_uid_results_to_seq(fctx);
	}
}

static bool arg_is_better(const struct mail_search_arg *new_arg,
			  const struct mail_search_arg *old_arg)
{
	if (old_arg == NULL)
		return TRUE;
	if (new_arg == NULL)
		return FALSE;

	/* avoid NOTs */
	if (old_arg->not && !new_arg->not)
		return TRUE;
	if (!old_arg->not && new_arg->not)
		return FALSE;

	/* prefer not to use headers. they have a larger possibility of
	   having lots of identical strings */
	if (old_arg->type == SEARCH_HEADER)
		return TRUE;
	else if (new_arg->type == SEARCH_HEADER)
		return FALSE;

	return strlen(new_arg->value.str) > strlen(old_arg->value.str);
}

static void
fts_search_args_find_best(struct mail_search_arg *args,
			  struct mail_search_arg **best_fast_arg,
			  struct mail_search_arg **best_substr_arg)
{
	for (; args != NULL; args = args->next) {
		switch (args->type) {
		case SEARCH_BODY_FAST:
		case SEARCH_TEXT_FAST:
			if (arg_is_better(args, *best_fast_arg))
				*best_fast_arg = args;
			break;
		case SEARCH_BODY:
		case SEARCH_TEXT:
		case SEARCH_HEADER:
			if (arg_is_better(args, *best_substr_arg))
				*best_substr_arg = args;
			break;
		default:
			break;
		}
	}
}

void fts_search_analyze(struct fts_search_context *fctx)
{
	struct mail_search_arg *best_fast_arg = NULL, *best_substr_arg = NULL;

	fts_search_args_find_best(fctx->args, &best_fast_arg, &best_substr_arg);

	if (best_fast_arg != NULL && fctx->fbox->backend_fast != NULL) {
		/* use fast backend whenever possible */
		fctx->best_arg = best_fast_arg;
		fctx->build_backend = fctx->fbox->backend_fast;
	} else if (best_fast_arg != NULL || best_substr_arg != NULL) {
		fctx->build_backend = fctx->fbox->backend_substr;
		fctx->best_arg = arg_is_better(best_substr_arg, best_fast_arg) ?
			best_substr_arg : best_fast_arg;
	}
}
