/* Copyright (c) 2002-2011 Dovecot authors, see the included COPYING file */

#include "lib.h"
#include "ioloop.h"
#include "array.h"
#include "istream.h"
#include "utc-offset.h"
#include "str.h"
#include "time-util.h"
#include "imap-match.h"
#include "message-address.h"
#include "message-date.h"
#include "message-search.h"
#include "message-parser.h"
#include "mail-index-modseq.h"
#include "index-storage.h"
#include "index-mail.h"
#include "index-sort.h"
#include "mail-search.h"
#include "mailbox-search-result-private.h"
#include "index-search-private.h"

#include <stdlib.h>
#include <ctype.h>

#define SEARCH_NOTIFY_INTERVAL_SECS 10

#define SEARCH_COST_DENTRY 3ULL
#define SEARCH_COST_ATTR 1ULL
#define SEARCH_COST_FILES_READ 25ULL
#define SEARCH_COST_KBYTE 15ULL
#define SEARCH_COST_CACHE 1ULL

#define SEARCH_MIN_NONBLOCK_USECS 200000
#define SEARCH_MAX_NONBLOCK_USECS 250000
#define SEARCH_INITIAL_MAX_COST 30000
#define SEARCH_RECALC_MIN_USECS 50000

struct search_header_context {
        struct index_mail *imail;
	struct mail_search_arg *args;

        struct message_header_line *hdr;

	unsigned int parse_headers:1;
	unsigned int custom_header:1;
	unsigned int threading:1;
};

struct search_body_context {
        struct index_search_context *index_ctx;
	struct istream *input;
	struct message_part *part;
};

static void search_parse_msgset_args(unsigned int messages_count,
				     struct mail_search_arg *args,
				     uint32_t *seq1_r, uint32_t *seq2_r);

static void search_none(struct mail_search_arg *arg ATTR_UNUSED,
			struct search_body_context *ctx ATTR_UNUSED)
{
}

static void search_init_arg(struct mail_search_arg *arg,
			    struct index_search_context *ctx)
{
	struct mailbox_metadata metadata;
	bool match;

	switch (arg->type) {
	case SEARCH_SEQSET:
		ctx->have_seqsets = TRUE;
		break;
	case SEARCH_UIDSET:
	case SEARCH_INTHREAD:
	case SEARCH_FLAGS:
	case SEARCH_KEYWORDS:
	case SEARCH_MODSEQ:
		if (arg->type == SEARCH_MODSEQ)
			mail_index_modseq_enable(ctx->box->index);
		ctx->have_index_args = TRUE;
		break;
	case SEARCH_MAILBOX_GUID:
		if (mailbox_get_metadata(ctx->box, MAILBOX_METADATA_GUID,
					 &metadata) < 0) {
			/* result will be unknown */
			break;
		}

		match = strcmp(guid_128_to_string(metadata.guid),
			       arg->value.str) == 0;
		if (match != arg->match_not)
			arg->match_always = TRUE;
		else
			arg->nonmatch_always = TRUE;
		break;
	case SEARCH_MAILBOX:
	case SEARCH_MAILBOX_GLOB:
		ctx->have_mailbox_args = TRUE;
		break;
	case SEARCH_ALL:
		if (!arg->match_not)
			arg->match_always = TRUE;
		else
			arg->nonmatch_always = TRUE;
		break;
	default:
		break;
	}
}

static void search_seqset_arg(struct mail_search_arg *arg,
			      struct index_search_context *ctx)
{
	if (arg->type == SEARCH_SEQSET) {
		if (seq_range_exists(&arg->value.seqset, ctx->mail_ctx.seq))
			ARG_SET_RESULT(arg, 1);
		else
			ARG_SET_RESULT(arg, 0);
	}
}

static int search_arg_match_keywords(struct index_search_context *ctx,
				     struct mail_search_arg *arg)
{
	ARRAY_TYPE(keyword_indexes) keyword_indexes_arr;
	const struct mail_keywords *search_kws = arg->value.keywords;
	const unsigned int *keyword_indexes;
	unsigned int i, j, count;

	t_array_init(&keyword_indexes_arr, 128);
	mail_index_lookup_keywords(ctx->view, ctx->mail_ctx.seq,
				   &keyword_indexes_arr);
	keyword_indexes = array_get(&keyword_indexes_arr, &count);

	/* there probably aren't many keywords, so O(n*m) for now */
	for (i = 0; i < search_kws->count; i++) {
		for (j = 0; j < count; j++) {
			if (search_kws->idx[i] == keyword_indexes[j])
				break;
		}
		if (j == count)
			return 0;
	}
	return 1;
}

/* Returns >0 = matched, 0 = not matched, -1 = unknown */
static int search_arg_match_index(struct index_search_context *ctx,
				  struct mail_search_arg *arg,
				  const struct mail_index_record *rec)
{
	enum mail_flags flags;
	uint64_t modseq;
	int ret;

	switch (arg->type) {
	case SEARCH_UIDSET:
	case SEARCH_INTHREAD:
		return seq_range_exists(&arg->value.seqset, rec->uid);
	case SEARCH_FLAGS:
		/* recent flag shouldn't be set, but indexes from v1.0.x
		   may contain it. */
		flags = rec->flags & ~MAIL_RECENT;
		if ((arg->value.flags & MAIL_RECENT) != 0 &&
		    index_mailbox_is_recent(ctx->box, rec->uid))
			flags |= MAIL_RECENT;
		return (flags & arg->value.flags) == arg->value.flags;
	case SEARCH_KEYWORDS:
		T_BEGIN {
			ret = search_arg_match_keywords(ctx, arg);
		} T_END;
		return ret;
	case SEARCH_MODSEQ: {
		if (arg->value.flags != 0) {
			modseq = mail_index_modseq_lookup_flags(ctx->view,
					arg->value.flags, ctx->mail_ctx.seq);
		} else if (arg->value.keywords != NULL) {
			modseq = mail_index_modseq_lookup_keywords(ctx->view,
					arg->value.keywords, ctx->mail_ctx.seq);
		} else {
			modseq = mail_index_modseq_lookup(ctx->view,
						ctx->mail_ctx.seq);
		}
		return modseq >= arg->value.modseq->modseq;
	}
	default:
		return -1;
	}
}

static void search_index_arg(struct mail_search_arg *arg,
			     struct index_search_context *ctx)
{
	const struct mail_index_record *rec;

	rec = mail_index_lookup(ctx->view, ctx->mail_ctx.seq);
	switch (search_arg_match_index(ctx, arg, rec)) {
	case -1:
		/* unknown */
		break;
	case 0:
		ARG_SET_RESULT(arg, 0);
		break;
	default:
		ARG_SET_RESULT(arg, 1);
		break;
	}
}

/* Returns >0 = matched, 0 = not matched, -1 = unknown */
static int search_arg_match_mailbox(struct index_search_context *ctx,
				    struct mail_search_arg *arg)
{
	const char *str;

	switch (arg->type) {
	case SEARCH_MAILBOX:
		if (mail_get_special(ctx->cur_mail, MAIL_FETCH_MAILBOX_NAME,
				     &str) < 0)
			return -1;

		if (strcasecmp(str, "INBOX") == 0)
			return strcasecmp(arg->value.str, "INBOX") == 0;
		return strcmp(str, arg->value.str) == 0;
	case SEARCH_MAILBOX_GLOB:
		if (mail_get_special(ctx->cur_mail, MAIL_FETCH_MAILBOX_NAME,
				     &str) < 0)
			return -1;
		return imap_match(arg->value.mailbox_glob, str) == IMAP_MATCH_YES;
	default:
		return -1;
	}
}

static void search_mailbox_arg(struct mail_search_arg *arg,
			       struct index_search_context *ctx)
{
	switch (search_arg_match_mailbox(ctx, arg)) {
	case -1:
		/* unknown */
		break;
	case 0:
		ARG_SET_RESULT(arg, 0);
		break;
	default:
		ARG_SET_RESULT(arg, 1);
		break;
	}
}

/* Returns >0 = matched, 0 = not matched, -1 = unknown */
static int search_arg_match_cached(struct index_search_context *ctx,
				   struct mail_search_arg *arg)
{
	const char *str;
	struct tm *tm;
	uoff_t virtual_size;
	time_t date;
	int tz_offset;
	bool have_tz_offset;

	switch (arg->type) {
	/* internal dates */
	case SEARCH_BEFORE:
	case SEARCH_ON:
	case SEARCH_SINCE:
		have_tz_offset = FALSE; tz_offset = 0; date = (time_t)-1;
		switch (arg->value.date_type) {
		case MAIL_SEARCH_DATE_TYPE_SENT:
			if (mail_get_date(ctx->cur_mail, &date, &tz_offset) < 0)
				return -1;
			have_tz_offset = TRUE;
			break;
		case MAIL_SEARCH_DATE_TYPE_RECEIVED:
			if (mail_get_received_date(ctx->cur_mail, &date) < 0)
				return -1;
			break;
		case MAIL_SEARCH_DATE_TYPE_SAVED:
			if (mail_get_save_date(ctx->cur_mail, &date) < 0)
				return -1;
			break;
		}

		if ((arg->value.search_flags &
		     MAIL_SEARCH_ARG_FLAG_USE_TZ) == 0) {
			if (!have_tz_offset) {
				tm = localtime(&date);
				tz_offset = utc_offset(tm, date);
			}
			date += tz_offset * 60;
		}

		switch (arg->type) {
		case SEARCH_BEFORE:
			return date < arg->value.time;
		case SEARCH_ON:
			return date >= arg->value.time &&
				date < arg->value.time + 3600*24;
		case SEARCH_SINCE:
			return date >= arg->value.time;
		default:
			/* unreachable */
			break;
		}

	/* sizes */
	case SEARCH_SMALLER:
	case SEARCH_LARGER:
		if (mail_get_virtual_size(ctx->cur_mail, &virtual_size) < 0)
			return -1;

		if (arg->type == SEARCH_SMALLER)
			return virtual_size < arg->value.size;
		else
			return virtual_size > arg->value.size;

	case SEARCH_GUID:
		if (mail_get_special(ctx->cur_mail, MAIL_FETCH_GUID, &str) < 0)
			return -1;
		return strcmp(str, arg->value.str) == 0;
	default:
		return -1;
	}
}

static void search_cached_arg(struct mail_search_arg *arg,
			      struct index_search_context *ctx)
{
	switch (search_arg_match_cached(ctx, arg)) {
	case -1:
		/* unknown */
		break;
	case 0:
		ARG_SET_RESULT(arg, 0);
		break;
	default:
		ARG_SET_RESULT(arg, 1);
		break;
	}
}

static int search_sent(enum mail_search_arg_type type, time_t search_time,
		       const unsigned char *sent_value, size_t sent_value_len)
{
	time_t sent_time;
	int timezone_offset;

	if (sent_value == NULL)
		return 0;

	/* NOTE: RFC-3501 specifies that timezone is ignored
	   in searches. sent_time is returned as UTC, so change it. */
	if (!message_date_parse(sent_value, sent_value_len,
				&sent_time, &timezone_offset))
		return 0;
	sent_time += timezone_offset * 60;

	switch (type) {
	case SEARCH_BEFORE:
		return sent_time < search_time;
	case SEARCH_ON:
		return sent_time >= search_time &&
			sent_time < search_time + 3600*24;
	case SEARCH_SINCE:
		return sent_time >= search_time;
	default:
                i_unreached();
	}
}

static struct message_search_context *
msg_search_arg_context(struct mail_search_arg *arg)
{
	enum message_search_flags flags;

	if (arg->context == NULL) {
		flags = arg->type == SEARCH_BODY ?
			MESSAGE_SEARCH_FLAG_SKIP_HEADERS : 0;
		arg->context = message_search_init(arg->value.str, flags);
	}
	return arg->context;
}

static void compress_lwsp(string_t *dest, const unsigned char *src,
			  unsigned int src_len)
{
	unsigned int i;
	bool prev_lwsp = TRUE;

	for (i = 0; i < src_len; i++) {
		if (IS_LWSP(src[i])) {
			if (!prev_lwsp) {
				prev_lwsp = TRUE;
				str_append_c(dest, ' ');
			}
		} else {
			prev_lwsp = FALSE;
			str_append_c(dest, src[i]);
		}
	}
}

static void search_header_arg(struct mail_search_arg *arg,
			      struct search_header_context *ctx)
{
        struct message_search_context *msg_search_ctx;
	struct message_block block;
	struct message_header_line hdr;
	int ret;

	/* first check that the field name matches to argument. */
	switch (arg->type) {
	case SEARCH_BEFORE:
	case SEARCH_ON:
	case SEARCH_SINCE:
		if (arg->value.date_type != MAIL_SEARCH_DATE_TYPE_SENT)
			return;

		/* date is handled differently than others */
		if (strcasecmp(ctx->hdr->name, "Date") == 0) {
			if (ctx->hdr->continues) {
				ctx->hdr->use_full_value = TRUE;
				return;
			}
			ret = search_sent(arg->type, arg->value.time,
					  ctx->hdr->full_value,
					  ctx->hdr->full_value_len);
			ARG_SET_RESULT(arg, ret);
		}
		return;

	case SEARCH_HEADER:
	case SEARCH_HEADER_ADDRESS:
	case SEARCH_HEADER_COMPRESS_LWSP:
		ctx->custom_header = TRUE;

		if (strcasecmp(ctx->hdr->name, arg->hdr_field_name) != 0)
			return;
		break;
	default:
		return;
	}

	if (arg->value.str[0] == '\0') {
		/* we're just testing existence of the field. always matches. */
		ARG_SET_RESULT(arg, 1);
		return;
	}

	if (ctx->hdr->continues) {
		ctx->hdr->use_full_value = TRUE;
		return;
	}

	memset(&block, 0, sizeof(block));

	/* We're searching only for values, so drop header name and middle
	   parts. We use header searching so that MIME words will be decoded. */
	hdr = *ctx->hdr;
	hdr.name = ""; hdr.name_len = 0;
	hdr.middle_len = 0;
	block.hdr = &hdr;

	msg_search_ctx = msg_search_arg_context(arg);
	if (msg_search_ctx == NULL)
		return;

	T_BEGIN {
		struct message_address *addr;
		string_t *str;

		switch (arg->type) {
		case SEARCH_HEADER:
			/* simple match */
			break;
		case SEARCH_HEADER_ADDRESS:
			/* we have to match against normalized address */
			addr = message_address_parse(pool_datastack_create(),
						     ctx->hdr->full_value,
						     ctx->hdr->full_value_len,
						     (unsigned int)-1, TRUE);
			str = t_str_new(ctx->hdr->value_len);
			message_address_write(str, addr);
			hdr.value = hdr.full_value = str_data(str);
			hdr.value_len = hdr.full_value_len = str_len(str);
			break;
		case SEARCH_HEADER_COMPRESS_LWSP:
			/* convert LWSP to single spaces */
			str = t_str_new(hdr.full_value_len);
			compress_lwsp(str, hdr.full_value, hdr.full_value_len);
			hdr.value = hdr.full_value = str_data(str);
			hdr.value_len = hdr.full_value_len = str_len(str);
			break;
		default:
			i_unreached();
		}
		ret = message_search_more(msg_search_ctx, &block) ? 1 : 0;
	} T_END;

	/* there may be multiple headers. don't mark this failed yet. */
	if (ret > 0)
		ARG_SET_RESULT(arg, 1);
}

static void search_header_unmatch(struct mail_search_arg *arg,
				  void *context ATTR_UNUSED)
{
	switch (arg->type) {
	case SEARCH_BEFORE:
	case SEARCH_ON:
	case SEARCH_SINCE:
		if (arg->value.date_type != MAIL_SEARCH_DATE_TYPE_SENT)
			break;

		if (arg->match_not) {
			/* date header not found, so we match only for
			   NOT searches */
			ARG_SET_RESULT(arg, 0);
		}
		break;
	case SEARCH_HEADER:
	case SEARCH_HEADER_ADDRESS:
	case SEARCH_HEADER_COMPRESS_LWSP:
		ARG_SET_RESULT(arg, 0);
		break;
	default:
		break;
	}
}

static void search_header(struct message_header_line *hdr,
			  struct search_header_context *ctx)
{
	if (hdr == NULL) {
		/* end of headers, mark all unknown SEARCH_HEADERs unmatched */
		mail_search_args_foreach(ctx->args, search_header_unmatch, ctx);
		return;
	}

	if (hdr->eoh)
		return;

	if (ctx->parse_headers)
		index_mail_parse_header(NULL, hdr, ctx->imail);

	if (ctx->custom_header || strcasecmp(hdr->name, "Date") == 0) {
		ctx->hdr = hdr;

		ctx->custom_header = FALSE;
		mail_search_args_foreach(ctx->args, search_header_arg, ctx);
	}
}

static void search_body(struct mail_search_arg *arg,
			struct search_body_context *ctx)
{
	struct message_search_context *msg_search_ctx;
	int ret;

	switch (arg->type) {
	case SEARCH_BODY:
	case SEARCH_TEXT:
		break;
	default:
		return;
	}

	msg_search_ctx = msg_search_arg_context(arg);
	if (msg_search_ctx == NULL) {
		ARG_SET_RESULT(arg, 0);
		return;
	}

	i_stream_seek(ctx->input, 0);
	ret = message_search_msg(msg_search_ctx, ctx->input, ctx->part);
	if (ret < 0 && ctx->input->stream_errno == 0) {
		/* try again without cached parts */
		mail_set_cache_corrupted(ctx->index_ctx->cur_mail,
					 MAIL_FETCH_MESSAGE_PARTS);

		i_stream_seek(ctx->input, 0);
		ret = message_search_msg(msg_search_ctx, ctx->input, NULL);
		i_assert(ret >= 0 || ctx->input->stream_errno != 0);
	}

	ARG_SET_RESULT(arg, ret);
}

static int search_arg_match_text(struct mail_search_arg *args,
				 struct index_search_context *ctx)
{
	const enum message_header_parser_flags hdr_parser_flags =
		MESSAGE_HEADER_PARSER_FLAG_CLEAN_ONELINE;
	struct index_mail *imail = (struct index_mail *)ctx->cur_mail;
	struct istream *input = NULL;
	struct mailbox_header_lookup_ctx *headers_ctx;
	struct search_header_context hdr_ctx;
	struct search_body_context body_ctx;
	const char *const *headers;
	bool have_headers, have_body, failed = FALSE;
	int ret;

	/* first check what we need to use */
	headers = mail_search_args_analyze(args, &have_headers, &have_body);
	if (!have_headers && !have_body)
		return -1;

	memset(&hdr_ctx, 0, sizeof(hdr_ctx));
	/* hdr_ctx.imail is different from imail for mails in
	   virtual mailboxes */
	hdr_ctx.imail = (struct index_mail *)mail_get_real_mail(ctx->cur_mail);
	hdr_ctx.custom_header = TRUE;
	hdr_ctx.args = args;

	headers_ctx = headers == NULL ? NULL :
		mailbox_header_lookup_init(ctx->box, headers);
	if (headers != NULL &&
	    (!have_body ||
	     ctx->cur_mail->lookup_abort == MAIL_LOOKUP_ABORT_NEVER)) {
		/* try to look up the specified headers from cache */
		i_assert(*headers != NULL);

		if (mail_get_header_stream(ctx->cur_mail, headers_ctx,
					   &input) < 0)
			failed = TRUE;
		else {
			message_parse_header(input, NULL, hdr_parser_flags,
					     search_header, &hdr_ctx);
		}
		input = NULL;
	} else if (have_headers) {
		/* we need to read the entire header */
		if (mail_get_stream(ctx->cur_mail, NULL, NULL, &input) < 0)
			failed = TRUE;
		else {
			hdr_ctx.parse_headers =
				index_mail_want_parse_headers(hdr_ctx.imail);
			if (hdr_ctx.parse_headers) {
				index_mail_parse_header_init(hdr_ctx.imail,
							     headers_ctx);
			}
			message_parse_header(input, NULL, hdr_parser_flags,
					     search_header, &hdr_ctx);
		}
	}
	if (headers_ctx != NULL)
		mailbox_header_lookup_unref(&headers_ctx);

	if (failed) {
		/* opening mail failed. maybe because of lookup_abort.
		   update access_parts for prefetching */
		if (have_body)
			imail->data.access_part |= READ_HDR | READ_BODY;
		else 
			imail->data.access_part |= READ_HDR;
		return -1;
	}

	if (have_headers) {
		/* see if the header search succeeded in finishing the search */
		ret = mail_search_args_foreach(args, search_none, NULL);
		if (ret >= 0 || !have_body)
			return ret;
	}

	i_assert(have_body);

	if (ctx->cur_mail->lookup_abort != MAIL_LOOKUP_ABORT_NEVER) {
		imail->data.access_part |= READ_HDR | READ_BODY;
		return -1;
	}

	if (input == NULL) {
		/* we didn't search headers. */
		struct message_size hdr_size;

		if (mail_get_stream(ctx->cur_mail, &hdr_size, NULL, &input) < 0)
			return -1;
		i_stream_seek(input, hdr_size.physical_size);
	}

	memset(&body_ctx, 0, sizeof(body_ctx));
	body_ctx.index_ctx = ctx;
	body_ctx.input = input;
	(void)mail_get_parts(ctx->cur_mail, &body_ctx.part);

	return mail_search_args_foreach(args, search_body, &body_ctx);
}

static bool
search_msgset_fix_limits(unsigned int messages_count,
			 ARRAY_TYPE(seq_range) *seqset, bool match_not)
{
	struct seq_range *range;
	unsigned int count;

	i_assert(messages_count > 0);

	range = array_get_modifiable(seqset, &count);
	if (count > 0) {
		i_assert(range[0].seq1 != 0);
		if (range[count-1].seq2 == (uint32_t)-1) {
			/* "*" used, make sure the last message is in the range
			   (e.g. with count+1:* we still want to include it) */
			seq_range_array_add(seqset, 0, messages_count);
		}
		/* remove all nonexistent messages */
		seq_range_array_remove_range(seqset, messages_count + 1,
					     (uint32_t)-1);
	}
	if (!match_not)
		return array_count(seqset) > 0;
	else {
		/* if all messages are in the range, it can't match */
		range = array_get_modifiable(seqset, &count);
		return count == 0 || range[0].seq1 != 1 ||
			range[count-1].seq2 != messages_count;
	}
}

static void
search_msgset_fix(unsigned int messages_count,
		  ARRAY_TYPE(seq_range) *seqset,
		  uint32_t *seq1_r, uint32_t *seq2_r, bool match_not)
{
	const struct seq_range *range;
	unsigned int count;
	uint32_t min_seq, max_seq;

	if (!search_msgset_fix_limits(messages_count, seqset, match_not)) {
		*seq1_r = (uint32_t)-1;
		*seq2_r = 0;
		return;
	}

	range = array_get(seqset, &count);
	if (!match_not) {
		min_seq = range[0].seq1;
		max_seq = range[count-1].seq2;
	} else if (count == 0) {
		/* matches all messages */
		min_seq = 1;
		max_seq = messages_count;
	} else {
		min_seq = range[0].seq1 > 1 ? 1 : range[0].seq2 + 1;
		max_seq = range[count-1].seq2 < messages_count ?
			messages_count : range[count-1].seq1 - 1;
		if (min_seq > max_seq) {
			*seq1_r = (uint32_t)-1;
			*seq2_r = 0;
			return;
		}
	}

	if (*seq1_r < min_seq || *seq1_r == 0)
		*seq1_r = min_seq;
	if (*seq2_r > max_seq)
		*seq2_r = max_seq;
}

static void search_or_parse_msgset_args(unsigned int messages_count,
					struct mail_search_arg *args,
					uint32_t *seq1_r, uint32_t *seq2_r)
{
	uint32_t seq1, seq2, min_seq1 = 0, max_seq2 = 0;

	for (; args != NULL; args = args->next) {
		seq1 = 1; seq2 = messages_count;

		switch (args->type) {
		case SEARCH_SUB:
			i_assert(!args->match_not);
			search_parse_msgset_args(messages_count,
						 args->value.subargs,
						 &seq1, &seq2);
			break;
		case SEARCH_OR:
			i_assert(!args->match_not);
			search_or_parse_msgset_args(messages_count,
						    args->value.subargs,
						    &seq1, &seq2);
			break;
		case SEARCH_SEQSET:
			search_msgset_fix(messages_count, &args->value.seqset,
					  &seq1, &seq2, args->match_not);
			break;
		default:
			break;
		}

		if (min_seq1 == 0) {
			min_seq1 = seq1;
			max_seq2 = seq2;
		} else {
			if (seq1 < min_seq1)
				min_seq1 = seq1;
			if (seq2 > max_seq2)
				max_seq2 = seq2;
		}
	}
	i_assert(min_seq1 != 0);

	if (min_seq1 > *seq1_r)
		*seq1_r = min_seq1;
	if (max_seq2 < *seq2_r)
		*seq2_r = max_seq2;
}

static void search_parse_msgset_args(unsigned int messages_count,
				     struct mail_search_arg *args,
				     uint32_t *seq1_r, uint32_t *seq2_r)
{
	for (; args != NULL; args = args->next) {
		switch (args->type) {
		case SEARCH_SUB:
			i_assert(!args->match_not);
			search_parse_msgset_args(messages_count,
						 args->value.subargs,
						 seq1_r, seq2_r);
			break;
		case SEARCH_OR:
			/* go through our children and use the widest seqset
			   range */
			i_assert(!args->match_not);
			search_or_parse_msgset_args(messages_count,
						    args->value.subargs,
						    seq1_r, seq2_r);
			break;
		case SEARCH_SEQSET:
			search_msgset_fix(messages_count, &args->value.seqset,
					  seq1_r, seq2_r, args->match_not);
			break;
		default:
			break;
		}
	}
}

static void search_limit_lowwater(struct index_search_context *ctx,
				  uint32_t uid_lowwater, uint32_t *first_seq)
{
	uint32_t seq1, seq2;

	if (uid_lowwater == 0)
		return;

	mail_index_lookup_seq_range(ctx->view, uid_lowwater, (uint32_t)-1,
				    &seq1, &seq2);
	if (*first_seq < seq1)
		*first_seq = seq1;
}

static bool search_limit_by_flags(struct index_search_context *ctx,
				  struct mail_search_arg *args,
				  uint32_t *seq1, uint32_t *seq2)
{
	const struct mail_index_header *hdr;

	hdr = mail_index_get_header(ctx->view);
	for (; args != NULL; args = args->next) {
		if (args->type != SEARCH_FLAGS) {
			if (args->type == SEARCH_ALL) {
				if (args->match_not)
					return FALSE;
			}
			continue;
		}
		if ((args->value.flags & MAIL_SEEN) != 0) {
			/* SEEN with 0 seen? */
			if (!args->match_not && hdr->seen_messages_count == 0)
				return FALSE;

			if (hdr->seen_messages_count == hdr->messages_count) {
				/* UNSEEN with all seen? */
				if (args->match_not)
					return FALSE;

				/* SEEN with all seen */
				args->match_always = TRUE;
			} else if (args->match_not) {
				/* UNSEEN with lowwater limiting */
				search_limit_lowwater(ctx,
                                	hdr->first_unseen_uid_lowwater, seq1);
			}
		}
		if ((args->value.flags & MAIL_DELETED) != 0) {
			/* DELETED with 0 deleted? */
			if (!args->match_not &&
			    hdr->deleted_messages_count == 0)
				return FALSE;

			if (hdr->deleted_messages_count ==
			    hdr->messages_count) {
				/* UNDELETED with all deleted? */
				if (args->match_not)
					return FALSE;

				/* DELETED with all deleted */
				args->match_always = TRUE;
			} else if (!args->match_not) {
				/* DELETED with lowwater limiting */
				search_limit_lowwater(ctx,
                                	hdr->first_deleted_uid_lowwater, seq1);
			}
		}
	}

	return *seq1 <= *seq2;
}

static void search_get_seqset(struct index_search_context *ctx,
			      unsigned int messages_count,
			      struct mail_search_arg *args)
{
	if (messages_count == 0) {
		/* no messages, don't check sequence ranges. although we could
		   give error message then for FETCH, we shouldn't do it for
		   UID FETCH. */
		ctx->seq1 = 1;
		ctx->seq2 = 0;
		return;
	}

	ctx->seq1 = 1;
	ctx->seq2 = messages_count;

	search_parse_msgset_args(messages_count, args, &ctx->seq1, &ctx->seq2);
	if (ctx->seq1 == 0) {
		ctx->seq1 = 1;
		ctx->seq2 = messages_count;
	}
	if (ctx->seq1 > ctx->seq2) {
		/* no matches */
		return;
	}

	/* UNSEEN and DELETED in root search level may limit the range */
	if (!search_limit_by_flags(ctx, args, &ctx->seq1, &ctx->seq2)) {
		/* no matches */
		ctx->seq1 = 1;
		ctx->seq2 = 0;
	}
}

static int search_build_subthread(struct mail_thread_iterate_context *iter,
				  ARRAY_TYPE(seq_range) *uids)
{
	struct mail_thread_iterate_context *child_iter;
	const struct mail_thread_child_node *node;
	int ret = 0;

	while ((node = mail_thread_iterate_next(iter, &child_iter)) != NULL) {
		if (child_iter != NULL) {
			if (search_build_subthread(child_iter, uids) < 0)
				ret = -1;
		}
		seq_range_array_add(uids, 0, node->uid);
	}
	if (mail_thread_iterate_deinit(&iter) < 0)
		ret = -1;
	return ret;
}

static int search_build_inthread_result(struct index_search_context *ctx,
					struct mail_search_arg *arg)
{
	struct mail_thread_iterate_context *iter, *child_iter;
	const struct mail_thread_child_node *node;
	const ARRAY_TYPE(seq_range) *search_uids;
	ARRAY_TYPE(seq_range) thread_uids;
	int ret = 0;

	/* mail_search_args_init() must have been called by now */
	i_assert(arg->value.search_args != NULL);

	p_array_init(&arg->value.seqset, ctx->mail_ctx.args->pool, 64);
	if (mailbox_search_result_build(ctx->mail_ctx.transaction,
					arg->value.search_args,
					MAILBOX_SEARCH_RESULT_FLAG_UPDATE |
					MAILBOX_SEARCH_RESULT_FLAG_QUEUE_SYNC,
					&arg->value.search_result) < 0)
		return -1;
	if (ctx->thread_ctx == NULL) {
		/* failed earlier */
		return -1;
	}

	search_uids = mailbox_search_result_get(arg->value.search_result);
	if (array_count(search_uids) == 0) {
		/* search found nothing - no threads can match */
		return 0;
	}

	t_array_init(&thread_uids, 128);
	iter = mail_thread_iterate_init(ctx->thread_ctx,
					arg->value.thread_type, FALSE);
	while ((node = mail_thread_iterate_next(iter, &child_iter)) != NULL) {
		seq_range_array_add(&thread_uids, 0, node->uid);
		if (child_iter != NULL) {
			if (search_build_subthread(child_iter,
						   &thread_uids) < 0)
				ret = -1;
		}
		if (seq_range_array_have_common(&thread_uids, search_uids)) {
			/* yes, we want this thread */
			seq_range_array_merge(&arg->value.seqset, &thread_uids);
		}
		array_clear(&thread_uids);
	}
	if (mail_thread_iterate_deinit(&iter) < 0)
		ret = -1;
	return ret;
}

static int search_build_inthreads(struct index_search_context *ctx,
				  struct mail_search_arg *arg)
{
	int ret = 0;

	for (; arg != NULL; arg = arg->next) {
		switch (arg->type) {
		case SEARCH_OR:
		case SEARCH_SUB:
			if (search_build_inthreads(ctx, arg->value.subargs) < 0)
				ret = -1;
			break;
		case SEARCH_INTHREAD:
			if (search_build_inthread_result(ctx, arg) < 0)
				ret = -1;
			break;
		default:
			break;
		}
	}
	return ret;
}

static void
wanted_sort_fields_get(struct mailbox *box,
		       const enum mail_sort_type *sort_program,
		       struct mailbox_header_lookup_ctx *wanted_headers,
		       enum mail_fetch_field *wanted_fields_r,
		       struct mailbox_header_lookup_ctx **headers_ctx_r)
{
	ARRAY_TYPE(const_string) headers;
	const char *header;
	unsigned int i;

	*wanted_fields_r = 0;
	*headers_ctx_r = NULL;

	t_array_init(&headers, 8);
	for (i = 0; sort_program[i] != MAIL_SORT_END; i++) {
		header = NULL;

		switch (sort_program[i] & MAIL_SORT_MASK) {
		case MAIL_SORT_ARRIVAL:
			*wanted_fields_r |= MAIL_FETCH_RECEIVED_DATE;
			break;
		case MAIL_SORT_CC:
			header = "Cc";
			break;
		case MAIL_SORT_DATE:
			*wanted_fields_r |= MAIL_FETCH_DATE;
			break;
		case MAIL_SORT_FROM:
			header = "From";
			break;
		case MAIL_SORT_SIZE:
			*wanted_fields_r |= MAIL_FETCH_VIRTUAL_SIZE;
			break;
		case MAIL_SORT_SUBJECT:
			header = "Subject";
			break;
		case MAIL_SORT_TO:
			header = "To";
			break;
		}
		if (header != NULL)
			array_append(&headers, &header, 1);
	}

	if (wanted_headers != NULL) {
		for (i = 0; wanted_headers->name[i] != NULL; i++)
			array_append(&headers, &wanted_headers->name[i], 1);
	}

	if (array_count(&headers) > 0) {
		(void)array_append_space(&headers);
		*headers_ctx_r = mailbox_header_lookup_init(box,
							array_idx(&headers, 0));
	}
}

struct mail_search_context *
index_storage_search_init(struct mailbox_transaction_context *t,
			  struct mail_search_args *args,
			  const enum mail_sort_type *sort_program,
			  enum mail_fetch_field wanted_fields,
			  struct mailbox_header_lookup_ctx *wanted_headers)
{
	struct index_search_context *ctx;
	struct mailbox_status status;

	ctx = i_new(struct index_search_context, 1);
	ctx->mail_ctx.transaction = t;
	ctx->box = t->box;
	ctx->view = t->view;
	ctx->mail_ctx.args = args;
	ctx->mail_ctx.sort_program = index_sort_program_init(t, sort_program);

	ctx->max_mails = t->box->storage->set->mail_prefetch_count + 1;
	if (ctx->max_mails == 0)
		ctx->max_mails = -1U;
	ctx->next_time_check_cost = SEARCH_INITIAL_MAX_COST;
	if (gettimeofday(&ctx->last_nonblock_timeval, NULL) < 0)
		i_fatal("gettimeofday() failed: %m");

	mailbox_get_open_status(t->box, STATUS_MESSAGES, &status);
	ctx->mail_ctx.progress_max = status.messages;

	i_array_init(&ctx->mail_ctx.results, 5);
	array_create(&ctx->mail_ctx.module_contexts, default_pool,
		     sizeof(void *), 5);
	i_array_init(&ctx->mails, ctx->max_mails);

	mail_search_args_reset(ctx->mail_ctx.args->args, TRUE);
	if (args->have_inthreads) {
		if (mail_thread_init(t->box, NULL, &ctx->thread_ctx) < 0)
			ctx->failed = TRUE;
		if (search_build_inthreads(ctx, args->args) < 0)
			ctx->failed = TRUE;
	}

	if (sort_program != NULL) {
		wanted_sort_fields_get(ctx->box, sort_program, wanted_headers,
				       &ctx->mail_ctx.wanted_fields,
				       &ctx->mail_ctx.wanted_headers);
	} else if (wanted_headers != NULL) {
		ctx->mail_ctx.wanted_headers = wanted_headers;
		mailbox_header_lookup_ref(wanted_headers);
	}
	ctx->mail_ctx.wanted_fields |= wanted_fields;

	search_get_seqset(ctx, status.messages, args->args);
	(void)mail_search_args_foreach(args->args, search_init_arg, ctx);

	/* Need to reset results for match_always cases */
	mail_search_args_reset(ctx->mail_ctx.args->args, FALSE);
	return &ctx->mail_ctx;
}

static void search_arg_deinit(struct mail_search_arg *arg,
			      void *context ATTR_UNUSED)
{
	struct message_search_context *search_ctx = arg->context;

	if (search_ctx != NULL) {
		message_search_deinit(&search_ctx);
		arg->context = NULL;
	}
}

int index_storage_search_deinit(struct mail_search_context *_ctx)
{
        struct index_search_context *ctx = (struct index_search_context *)_ctx;
	struct mail **mailp;
	int ret;

	ret = ctx->failed ? -1 : 0;

	mail_search_args_reset(ctx->mail_ctx.args->args, FALSE);
	(void)mail_search_args_foreach(ctx->mail_ctx.args->args,
				       search_arg_deinit, NULL);

	if (ctx->mail_ctx.wanted_headers != NULL)
		mailbox_header_lookup_unref(&ctx->mail_ctx.wanted_headers);
	if (ctx->mail_ctx.sort_program != NULL)
		index_sort_program_deinit(&ctx->mail_ctx.sort_program);
	if (ctx->thread_ctx != NULL)
		mail_thread_deinit(&ctx->thread_ctx);
	array_free(&ctx->mail_ctx.results);
	array_free(&ctx->mail_ctx.module_contexts);

	array_foreach_modifiable(&ctx->mails, mailp) {
		struct index_mail *imail = (struct index_mail *)*mailp;

		imail->search_mail = FALSE;
		mail_free(mailp);
	}
	array_free(&ctx->mails);
	i_free(ctx);
	return ret;
}

static unsigned long long
search_get_cost(struct mailbox_transaction_context *trans)
{
	return trans->stats.open_lookup_count * SEARCH_COST_DENTRY +
		trans->stats.stat_lookup_count * SEARCH_COST_DENTRY +
		trans->stats.fstat_lookup_count * SEARCH_COST_ATTR +
		trans->stats.cache_hit_count * SEARCH_COST_CACHE +
		trans->stats.files_read_count * SEARCH_COST_FILES_READ +
		(trans->stats.files_read_bytes/1024) * SEARCH_COST_KBYTE;
}

static int search_match_once(struct index_search_context *ctx)
{
	unsigned long long cost1, cost2;
	int ret;

	cost1 = search_get_cost(ctx->cur_mail->transaction);
	ret = mail_search_args_foreach(ctx->mail_ctx.args->args,
				       search_cached_arg, ctx);
	if (ret < 0)
		ret = search_arg_match_text(ctx->mail_ctx.args->args, ctx);

	cost2 = search_get_cost(ctx->cur_mail->transaction);
	ctx->cost += cost2 - cost1;
	return ret;
}

static bool search_arg_is_static(struct mail_search_arg *arg)
{
	struct mail_search_arg *subarg;

	switch (arg->type) {
	case SEARCH_OR:
	case SEARCH_SUB:
		/* they're static only if all subargs are static */
		subarg = arg->value.subargs;
		for (; subarg != NULL; subarg = subarg->next) {
			if (!search_arg_is_static(subarg))
				return FALSE;
		}
		return TRUE;
	case SEARCH_SEQSET:
		/* changes between syncs, but we can't really handle this
		   currently. seqsets should be converted to uidsets first. */
	case SEARCH_FLAGS:
	case SEARCH_KEYWORDS:
	case SEARCH_MODSEQ:
	case SEARCH_INTHREAD:
		break;
	case SEARCH_ALL:
	case SEARCH_UIDSET:
	case SEARCH_BEFORE:
	case SEARCH_ON:
	case SEARCH_SINCE:
	case SEARCH_SMALLER:
	case SEARCH_LARGER:
	case SEARCH_HEADER:
	case SEARCH_HEADER_ADDRESS:
	case SEARCH_HEADER_COMPRESS_LWSP:
	case SEARCH_BODY:
	case SEARCH_TEXT:
	case SEARCH_GUID:
	case SEARCH_MAILBOX:
	case SEARCH_MAILBOX_GUID:
	case SEARCH_MAILBOX_GLOB:
		return TRUE;
	}
	return FALSE;
}

static void search_set_static_matches(struct mail_search_arg *arg)
{
	for (; arg != NULL; arg = arg->next) {
		if (search_arg_is_static(arg))
			arg->result = 1;
	}
}

static bool search_has_static_nonmatches(struct mail_search_arg *arg)
{
	for (; arg != NULL; arg = arg->next) {
		if (arg->result == 0 && search_arg_is_static(arg))
			return TRUE;
	}
	return FALSE;
}

static void search_match_finish(struct index_search_context *ctx, int match)
{
	if (ctx->cur_mail->expunged)
		ctx->mail_ctx.seen_lost_data = TRUE;

	if (match == 0 &&
	    search_has_static_nonmatches(ctx->mail_ctx.args->args)) {
		/* if there are saved search results remember
		   that this message never matches */
		mailbox_search_results_never(&ctx->mail_ctx,
					     ctx->cur_mail->uid);
	}
}

static int search_match_next(struct index_search_context *ctx)
{
	static enum mail_lookup_abort cache_lookups[] = {
		MAIL_LOOKUP_ABORT_NOT_IN_CACHE,
		MAIL_LOOKUP_ABORT_READ_MAIL,
		MAIL_LOOKUP_ABORT_NEVER
	};
	unsigned int i, n = N_ELEMENTS(cache_lookups);
	int ret = -1;

	if (ctx->have_mailbox_args) {
		/* check that the mailbox name matches.
		   this makes sense only with virtual mailboxes. */
		ret = mail_search_args_foreach(ctx->mail_ctx.args->args,
					       search_mailbox_arg, ctx);
	}

	/* avoid doing extra work for as long as possible */
	if (ctx->max_mails > 1) {
		/* we're doing prefetching. if we have to read the mail,
		   do a prefetch first and the final search later */
		n--;
	}
	for (i = 0; i < n && ret < 0; i++) {
		ctx->cur_mail->lookup_abort = cache_lookups[i];
		ret = search_match_once(ctx);
	}
	ctx->cur_mail->lookup_abort = MAIL_LOOKUP_ABORT_NEVER;
	search_match_finish(ctx, ret);
	return ret;
}

static void index_storage_search_notify(struct mailbox *box,
					struct index_search_context *ctx)
{
	float percentage;
	unsigned int msecs, secs;

	if (ctx->last_notify.tv_sec == 0) {
		/* set the search time in here, in case a plugin
		   already spent some time indexing the mailbox */
		ctx->search_start_time = ioloop_timeval;
	} else if (box->storage->callbacks.notify_ok != NULL &&
		   !ctx->mail_ctx.progress_hidden) {
		percentage = ctx->mail_ctx.progress_cur * 100.0 /
			ctx->mail_ctx.progress_max;
		msecs = timeval_diff_msecs(&ioloop_timeval,
					   &ctx->search_start_time);
		secs = (msecs / (percentage / 100.0) - msecs) / 1000;

		T_BEGIN {
			const char *text;

			text = t_strdup_printf("Searched %d%% of the mailbox, "
					       "ETA %d:%02d", (int)percentage,
					       secs/60, secs%60);
			box->storage->callbacks.
				notify_ok(box, text,
					  box->storage->callback_context);
		} T_END;
	}
	ctx->last_notify = ioloop_timeval;
}

static bool search_would_block(struct index_search_context *ctx)
{
	struct timeval now;
	unsigned long long guess_cost;
	long long usecs;
	bool ret;

	if (ctx->cost < ctx->next_time_check_cost)
		return FALSE;

	if (gettimeofday(&now, NULL) < 0)
		i_fatal("gettimeofday() failed: %m");

	usecs = timeval_diff_usecs(&now, &ctx->last_nonblock_timeval);
	if (usecs < 0) {
		/* clock moved backwards. */
		ctx->last_nonblock_timeval = now;
		ctx->next_time_check_cost = SEARCH_INITIAL_MAX_COST;
		return TRUE;
	} else if (usecs < SEARCH_MIN_NONBLOCK_USECS) {
		/* not finished yet. estimate the next time lookup */
		ret = FALSE;
	} else {
		/* done, or close enough anyway */
		ctx->last_nonblock_timeval = now;
		ret = TRUE;
	}
	guess_cost = ctx->cost *
		(SEARCH_MAX_NONBLOCK_USECS / (double)usecs);
	if (usecs < SEARCH_RECALC_MIN_USECS) {
		/* the estimate may not be very good since we spent
		   so little time doing this search. don't allow huge changes
		   to the guess, but allow anyway large enough so that we can
		   move to right direction. */
		if (guess_cost > ctx->next_time_check_cost*3)
			guess_cost = ctx->next_time_check_cost*3;
		else if (guess_cost < ctx->next_time_check_cost/3)
			guess_cost = ctx->next_time_check_cost/3;
	}
	if (ret)
		ctx->cost = 0;
	ctx->next_time_check_cost = guess_cost;
	return ret;
}

static int search_more_with_mail(struct index_search_context *ctx,
				 struct mail *mail)
{
	struct mail_search_context *_ctx = &ctx->mail_ctx;
	struct mailbox *box = _ctx->transaction->box;
	struct index_mail *imail = (struct index_mail *)mail;
	int match;

	if (search_would_block(ctx)) {
		/* this lookup is useful when a large number of
		   messages match */
		return 0;
	}

	if (ioloop_time - ctx->last_notify.tv_sec >=
	    SEARCH_NOTIFY_INTERVAL_SECS)
		index_storage_search_notify(box, ctx);

	while (box->v.search_next_update_seq(_ctx)) {
		mail_set_seq(mail, _ctx->seq);

		ctx->cur_mail = mail;
		T_BEGIN {
			match = search_match_next(ctx);
		} T_END;
		ctx->cur_mail = NULL;

		i_assert(imail->data.search_results == NULL);
		if (match < 0) {
			/* result isn't known yet, do a prefetch and
			   finish later */
			imail->data.search_results =
				buffer_create_dynamic(imail->data_pool, 64);
			mail_search_args_result_serialize(_ctx->args,
				imail->data.search_results);
		}

		mail_search_args_reset(_ctx->args->args, FALSE);

		if (match != 0)
			return 1;
		if (search_would_block(ctx))
			return 0;
	}
	return -1;
}

struct mail *index_search_get_mail(struct index_search_context *ctx)
{
	struct index_mail *imail;
	struct mail *const *mails, *mail;
	unsigned int count;

	if (ctx->unused_mail_idx == ctx->max_mails)
		return NULL;

	mails = array_get(&ctx->mails, &count);
	if (ctx->unused_mail_idx < count)
		return mails[ctx->unused_mail_idx];

	mail = mail_alloc(ctx->mail_ctx.transaction,
			  ctx->mail_ctx.wanted_fields,
			  ctx->mail_ctx.wanted_headers);
	imail = (struct index_mail *)mail;
	imail->search_mail = TRUE;
	ctx->mail_ctx.transaction->stats_track = TRUE;

	array_append(&ctx->mails, &mail, 1);
	return mail;
}

static int search_more_with_prefetching(struct index_search_context *ctx,
					struct mail **mail_r)
{
	struct mail *mail, *const *mails;
	unsigned int count;
	int ret = 0;

	while ((mail = index_search_get_mail(ctx)) != NULL) {
		ret = search_more_with_mail(ctx, mail);
		if (ret <= 0)
			break;
		if (mail_prefetch(mail) && ctx->unused_mail_idx == 0) {
			/* no prefetching done, return it immediately */
			*mail_r = mail;
			return 1;
		}
		ctx->unused_mail_idx++;
	}

	if (mail != NULL) {
		if (ret == 0) {
			/* wait */
			return 0;
		}
		i_assert(ret < 0);
		if (ctx->unused_mail_idx == 0) {
			/* finished */
			return -1;
		}
	} else {
		/* prefetch buffer is full. */
	}

	/* return the next message */
	i_assert(ctx->unused_mail_idx > 0);

	mails = array_get(&ctx->mails, &count);
	*mail_r = mails[0];
	if (--ctx->unused_mail_idx > 0) {
		array_delete(&ctx->mails, 0, 1);
		array_append(&ctx->mails, mail_r, 1);
	}
	return 1;
}

static bool search_finish_prefetch(struct index_search_context *ctx,
				   struct index_mail *imail)
{
	int ret;

	i_assert(imail->mail.mail.lookup_abort == MAIL_LOOKUP_ABORT_NEVER);

	ctx->cur_mail = &imail->mail.mail;
	mail_search_args_result_deserialize(ctx->mail_ctx.args,
					    imail->data.search_results->data,
					    imail->data.search_results->used);
	ret = search_match_once(ctx);
	search_match_finish(ctx, ret);
	ctx->cur_mail = NULL;
	return ret > 0;
}

static int search_more(struct index_search_context *ctx,
		       struct mail **mail_r)
{
	struct index_mail *imail;
	int ret;

	while ((ret = search_more_with_prefetching(ctx, mail_r)) > 0) {
		imail = (struct index_mail *)*mail_r;
		if (imail->data.search_results == NULL)
			break;

		/* searching wasn't finished yet */
		if (search_finish_prefetch(ctx, imail))
			break;
		/* search finished as non-match */
	}
	return ret;
}

bool index_storage_search_next_nonblock(struct mail_search_context *_ctx,
					struct mail **mail_r, bool *tryagain_r)
{
        struct index_search_context *ctx = (struct index_search_context *)_ctx;
	struct mail *mail, *const *mailp;
	uint32_t seq;
	int ret;

	*tryagain_r = FALSE;

	if (_ctx->sort_program == NULL) {
		ret = search_more(ctx, &mail);
		if (ret == 0) {
			*tryagain_r = TRUE;
			return FALSE;
		}
		if (ret < 0)
			return FALSE;
		*mail_r = mail;
		return TRUE;
	}

	if (!ctx->sorted) {
		while ((ret = search_more(ctx, &mail)) > 0)
			index_sort_list_add(_ctx->sort_program, mail);

		if (ret == 0) {
			*tryagain_r = TRUE;
			return FALSE;
		}
		/* finished searching the messages. now sort them and start
		   returning the messages. */
		ctx->sorted = TRUE;
		index_sort_list_finish(_ctx->sort_program);
		if (ctx->failed)
			return FALSE;
	}

	/* everything searched at this point already. just returning
	   matches from sort list */
	if (!index_sort_list_next(_ctx->sort_program, &seq))
		return FALSE;

	mailp = array_idx(&ctx->mails, 0);
	mail_set_seq(*mailp, seq);
	*mail_r = *mailp;
	return TRUE;
}

bool index_storage_search_next_update_seq(struct mail_search_context *_ctx)
{
        struct index_search_context *ctx = (struct index_search_context *)_ctx;
	uint32_t uid;
	int ret;

	if (_ctx->seq == 0) {
		/* first time */
		_ctx->seq = ctx->seq1;
	} else {
		_ctx->seq++;
	}

	if (!ctx->have_seqsets && !ctx->have_index_args &&
	    _ctx->update_result == NULL) {
		_ctx->progress_cur = _ctx->seq;
		return _ctx->seq <= ctx->seq2;
	}

	ret = 0;
	while (_ctx->seq <= ctx->seq2) {
		/* check if the sequence matches */
		ret = mail_search_args_foreach(ctx->mail_ctx.args->args,
					       search_seqset_arg, ctx);
		if (ret != 0 && ctx->have_index_args) {
			/* check if flags/keywords match before anything else
			   is done. mail_set_seq() can be a bit slow. */
			ret = mail_search_args_foreach(ctx->mail_ctx.args->args,
						       search_index_arg, ctx);
		}
		if (ret != 0 && _ctx->update_result != NULL) {
			/* see if this message never matches */
			mail_index_lookup_uid(ctx->view, _ctx->seq, &uid);
			if (seq_range_exists(&_ctx->update_result->never_uids,
					     uid))
				ret = 0;
		}
		if (ret != 0)
			break;

		/* doesn't, try next one */
		_ctx->seq++;
		mail_search_args_reset(ctx->mail_ctx.args->args, FALSE);
	}

	if (ret != 0 && _ctx->update_result != NULL) {
		mail_index_lookup_uid(ctx->view, _ctx->seq, &uid);
		if (seq_range_exists(&_ctx->update_result->uids, uid)) {
			/* we already know that the static data
			   matches. mark it as such. */
			search_set_static_matches(_ctx->args->args);
		}
	}
	ctx->mail_ctx.progress_cur = _ctx->seq;
	return ret != 0;
}
