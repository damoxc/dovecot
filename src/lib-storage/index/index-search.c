/* Copyright (C) 2002 Timo Sirainen */

#include "lib.h"
#include "ibuffer.h"
#include "obuffer.h"
#include "mmap-util.h"
#include "rfc822-tokenize.h"
#include "rfc822-date.h"
#include "message-size.h"
#include "message-header-search.h"
#include "imap-date.h"
#include "imap-envelope.h"
#include "index-storage.h"
#include "mail-index-util.h"
#include "mail-modifylog.h"
#include "mail-custom-flags.h"
#include "mail-search.h"

#include <stdlib.h>
#include <ctype.h>

#define ARG_SET_RESULT(arg, res) \
	STMT_START { \
		(arg)->result = !(arg)->not ? (res) : -(res); \
	} STMT_END

typedef struct {
	Pool hdr_pool;
	IndexMailbox *ibox;
	MailIndexRecord *rec;
	unsigned int client_seq;
	int cached;
	const char *charset;
	const char *error;
} SearchIndexContext;

typedef struct {
        SearchIndexContext *index_context;
	MailSearchArg *args;
	int custom_header;

	const char *name, *value;
	size_t name_len, value_len;
} SearchHeaderContext;

typedef struct {
	MailSearchArg *args;
	const char *msg;
	size_t size;

	size_t max_searchword_len;
} SearchTextContext;

static int msgset_contains(const char *set, unsigned int match_num,
			   unsigned int max_num)
{
	unsigned int num, num2;

	while (*set != '\0') {
		if (*set == '*') {
			set++;
			num = max_num;
		} else {
			num = 0;
			while (*set >= '0' && *set <= '9') {
				num = num*10 + (*set-'0');
				set++;
			}
		}

		if (*set == ',' || *set == '\0') {
			if (num == match_num)
				return TRUE;
			if (*set == '\0')
				return FALSE;
		} else if (*set == ':') {
			set++;

			if (*set == '*') {
				set++;

				if (match_num >= num && num <= max_num)
					return TRUE;
			} else {
				num2 = 0;
				while (*set >= '0' && *set <= '9') {
					num2 = num2*10 + (*set-'0');
					set++;
				}

				if (match_num >= num && match_num <= num2)
					return TRUE;
			}

			if (*set != ',')
				return FALSE;
		}

		set++;
	}

	return FALSE;
}

static uoff_t str_to_uoff_t(const char *str)
{
	uoff_t num;

	num = 0;
	while (*str != '\0') {
		if (*str < '0' || *str > '9')
			return 0;

		num = num*10 + (*str - '0');
		str++;
	}

	return num;
}

static int search_keyword(MailIndex *index, MailIndexRecord *rec,
			  const char *value)
{
	const char **custom_flags;
	int i;

	if ((rec->msg_flags & MAIL_CUSTOM_FLAGS_MASK) == 0)
		return FALSE;

	custom_flags = mail_custom_flags_list_get(index->custom_flags);
	for (i = 0; i < MAIL_CUSTOM_FLAGS_COUNT; i++) {
		if (custom_flags[i] != NULL &&
		    strcasecmp(custom_flags[i], value) == 0) {
			return rec->msg_flags &
				(1 << (MAIL_CUSTOM_FLAG_1_BIT+i));
		}
	}

	return FALSE;
}

/* Returns >0 = matched, 0 = not matched, -1 = unknown */
static int search_arg_match_index(IndexMailbox *ibox, MailIndexRecord *rec,
				  unsigned int client_seq,
				  MailSearchArgType type, const char *value)
{
	switch (type) {
	case SEARCH_ALL:
		return TRUE;
	case SEARCH_SET:
		return msgset_contains(value, client_seq,
				       ibox->synced_messages_count);
	case SEARCH_UID:
		return msgset_contains(value, rec->uid,
				       ibox->synced_messages_count);

	/* flags */
	case SEARCH_ANSWERED:
		return rec->msg_flags & MAIL_ANSWERED;
	case SEARCH_DELETED:
		return rec->msg_flags & MAIL_DELETED;
	case SEARCH_DRAFT:
		return rec->msg_flags & MAIL_DRAFT;
	case SEARCH_FLAGGED:
		return rec->msg_flags & MAIL_FLAGGED;
	case SEARCH_SEEN:
		return rec->msg_flags & MAIL_SEEN;
	case SEARCH_RECENT:
		return rec->uid >= ibox->index->first_recent_uid;
	case SEARCH_KEYWORD:
		return search_keyword(ibox->index, rec, value);

	default:
		return -1;
	}
}

static void search_index_arg(MailSearchArg *arg, void *context)
{
	SearchIndexContext *ctx = context;

	switch (search_arg_match_index(ctx->ibox, ctx->rec, ctx->client_seq,
				       arg->type, arg->value.str)) {
	case -1:
		/* unknown */
		break;
	case 0:
		ARG_SET_RESULT(arg, -1);
		break;
	default:
		ARG_SET_RESULT(arg, 1);
		break;
	}
}

static ImapMessageCache *search_open_cache(SearchIndexContext *ctx)
{
	if (!ctx->cached) {
		(void)index_msgcache_open(ctx->ibox->cache,
					  ctx->ibox->index, ctx->rec, 0);
		ctx->cached = TRUE;
	}
	return ctx->ibox->cache;
}

/* Returns >0 = matched, 0 = not matched, -1 = unknown */
static int search_arg_match_cached(SearchIndexContext *ctx,
				   MailSearchArgType type, const char *value)
{
	time_t internal_date, search_time;
	uoff_t virtual_size, search_size;

	switch (type) {
	/* internal dates */
	case SEARCH_BEFORE:
	case SEARCH_ON:
	case SEARCH_SINCE:
		internal_date = imap_msgcache_get_internal_date(
					search_open_cache(ctx));
		if (internal_date == (time_t)-1)
			return -1;

		if (!imap_parse_date(value, &search_time))
			return 0;

		switch (type) {
		case SEARCH_BEFORE:
			return internal_date < search_time;
		case SEARCH_ON:
			return internal_date >= search_time &&
				internal_date < search_time + 3600*24;
		case SEARCH_SINCE:
			return internal_date >= search_time;
		default:
			/* unreachable */
			break;
		}

	/* sizes */
	case SEARCH_SMALLER:
	case SEARCH_LARGER:
		virtual_size = imap_msgcache_get_virtual_size(
					search_open_cache(ctx));
		if (virtual_size == (uoff_t)-1)
			return -1;

		search_size = str_to_uoff_t(value);
		if (type == SEARCH_SMALLER)
			return virtual_size < search_size;
		else
			return virtual_size > search_size;

	default:
		return -1;
	}
}

static void search_cached_arg(MailSearchArg *arg, void *context)
{
	SearchIndexContext *ctx = context;

	switch (search_arg_match_cached(ctx, arg->type,
					arg->value.str)) {
	case -1:
		/* unknown */
		break;
	case 0:
		ARG_SET_RESULT(arg, -1);
		break;
	default:
		ARG_SET_RESULT(arg, 1);
		break;
	}
}

static int search_sent(MailSearchArgType type, const char *value,
		       const char *sent_value)
{
	time_t search_time, sent_time;
	int timezone_offset;

	if (!imap_parse_date(value, &search_time))
		return 0;

	/* NOTE: RFC2060 doesn't specify if timezones should affect
	   matching, so we ignore them. */
	if (!rfc822_parse_date(sent_value, &sent_time, &timezone_offset))
		return 0;

	switch (type) {
	case SEARCH_SENTBEFORE:
		return sent_time < search_time;
	case SEARCH_SENTON:
		return sent_time >= search_time &&
			sent_time < search_time + 3600*24;
	case SEARCH_SENTSINCE:
		return sent_time >= search_time;
	default:
                i_unreached();
	}
}

static HeaderSearchContext *search_header_context(SearchIndexContext *ctx,
						  MailSearchArg *arg)
{
	int unknown_charset;

	if (arg->context != NULL) {
                message_header_search_reset(arg->context);
		return arg->context;
	}

	if (ctx->hdr_pool == NULL) {
		ctx->hdr_pool = pool_create("message_header_search",
					    8192, FALSE);
	}

	arg->context = message_header_search_init(ctx->hdr_pool, arg->value.str,
						  ctx->charset,
						  &unknown_charset);
	if (arg->context == NULL) {
		ctx->error = unknown_charset ?
			"Unknown charset" : "Invalid search key";
	}

	return arg->context;
}

/* Returns >0 = matched, 0 = not matched, -1 = unknown */
static int search_arg_match_envelope(SearchIndexContext *ctx,
				     MailSearchArg *arg)
{
	MailIndex *index = ctx->ibox->index;
	ImapEnvelopeField env_field;
        HeaderSearchContext *hdr_search_ctx;
	const char *envelope, *field;
	size_t size;
	int ret;

	switch (arg->type) {
	case SEARCH_SENTBEFORE:
	case SEARCH_SENTON:
	case SEARCH_SENTSINCE:
                env_field = IMAP_ENVELOPE_DATE;
		break;

	case SEARCH_FROM:
                env_field = IMAP_ENVELOPE_FROM;
		break;
	case SEARCH_TO:
                env_field = IMAP_ENVELOPE_TO;
		break;
	case SEARCH_CC:
                env_field = IMAP_ENVELOPE_CC;
		break;
	case SEARCH_BCC:
                env_field = IMAP_ENVELOPE_BCC;
		break;
	case SEARCH_SUBJECT:
                env_field = IMAP_ENVELOPE_SUBJECT;
		break;

	case SEARCH_IN_REPLY_TO:
                env_field = IMAP_ENVELOPE_IN_REPLY_TO;
		break;
	case SEARCH_MESSAGE_ID:
                env_field = IMAP_ENVELOPE_MESSAGE_ID;
		break;
	default:
		return -1;
	}

	t_push();

	/* get field from hopefully cached envelope */
	envelope = index->lookup_field(index, ctx->rec, DATA_FIELD_ENVELOPE);
	if (envelope != NULL)
		field = imap_envelope_parse(envelope, env_field);
	else {
		index->cache_fields_later(index, DATA_FIELD_ENVELOPE);
		field = NULL;
	}

	if (field == NULL)
		ret = -1;
	else {
		switch (arg->type) {
		case SEARCH_SENTBEFORE:
		case SEARCH_SENTON:
		case SEARCH_SENTSINCE:
			ret = search_sent(arg->type, arg->value.str, field);
		default:
			hdr_search_ctx = search_header_context(ctx, arg);
			if (hdr_search_ctx == NULL) {
				ret = 0;
				break;
			}

			size = strlen(field);
			ret = message_header_search(field, &size,
						    hdr_search_ctx) ? 1 : 0;
		}
	}
	t_pop();
	return ret;
}

static void search_envelope_arg(MailSearchArg *arg, void *context)
{
	SearchIndexContext *ctx = context;

	switch (search_arg_match_envelope(ctx, arg)) {
	case -1:
		/* unknown */
		break;
	case 0:
		ARG_SET_RESULT(arg, -1);
		break;
	default:
		ARG_SET_RESULT(arg, 1);
		break;
	}
}

static void search_header_arg(MailSearchArg *arg, void *context)
{
	SearchHeaderContext *ctx = context;
        HeaderSearchContext *hdr_search_ctx;
	size_t len;
	int ret;

	/* first check that the field name matches to argument. */
	switch (arg->type) {
	case SEARCH_SENTBEFORE:
	case SEARCH_SENTON:
	case SEARCH_SENTSINCE:
		/* date is handled differently than others */
		if (ctx->name_len == 4 &&
		    strncasecmp(ctx->name, "Date", 4) == 0) {
			search_sent(arg->type, arg->value.str,
				    t_strndup(ctx->value, ctx->value_len));
		}
		return;

	case SEARCH_FROM:
		if (ctx->name_len != 4 ||
		    strncasecmp(ctx->name, "From", 4) != 0)
			return;
		break;
	case SEARCH_TO:
		if (ctx->name_len != 2 ||
		    strncasecmp(ctx->name, "To", 2) != 0)
			return;
		break;
	case SEARCH_CC:
		if (ctx->name_len != 2 ||
		    strncasecmp(ctx->name, "Cc", 2) != 0)
			return;
		break;
	case SEARCH_BCC:
		if (ctx->name_len != 3 ||
		    strncasecmp(ctx->name, "Bcc", 3) != 0)
			return;
		break;
	case SEARCH_SUBJECT:
		if (ctx->name_len != 7 ||
		    strncasecmp(ctx->name, "Subject", 7) != 0)
			return;
		break;
	case SEARCH_HEADER:
		ctx->custom_header = TRUE;

		len = strlen(arg->hdr_field_name);
		if (ctx->name_len != len ||
		    strncasecmp(ctx->name, arg->hdr_field_name, len) != 0)
			return;
	case SEARCH_TEXT:
		/* TEXT goes through all headers */
		ctx->custom_header = TRUE;
		break;
	default:
		return;
	}

	/* then check if the value matches */
	hdr_search_ctx = search_header_context(ctx->index_context, arg);
	if (hdr_search_ctx == NULL)
		ret = 0;
	else {
		len = ctx->value_len;
		ret = message_header_search(ctx->value, &len,
					    hdr_search_ctx) ? 1 : 0;
	}

        ARG_SET_RESULT(arg, ret);
}

static void search_header(MessagePart *part __attr_unused__,
			  const char *name, size_t name_len,
			  const char *value, size_t value_len,
			  void *context)
{
	SearchHeaderContext *ctx = context;

	if ((name_len > 0 && ctx->custom_header) ||
	    (name_len == 4 && strncasecmp(name, "Date", 4) == 0) ||
	    (name_len == 4 && strncasecmp(name, "From", 4) == 0) ||
	    (name_len == 2 && strncasecmp(name, "To", 2) == 0) ||
	    (name_len == 2 && strncasecmp(name, "Cc", 2) == 0) ||
	    (name_len == 3 && strncasecmp(name, "Bcc", 3) == 0) ||
	    (name_len == 7 && strncasecmp(name, "Subject", 7) == 0)) {
		ctx->name = name;
		ctx->value = value;
		ctx->name_len = name_len;
		ctx->value_len = value_len;

		ctx->custom_header = FALSE;
		mail_search_args_foreach(ctx->args, search_header_arg, ctx);
	}
}

static void search_text(MailSearchArg *arg, SearchTextContext *ctx)
{
	const char *p;
	size_t i, len, max;

	if (arg->result != 0)
		return;

	len = strlen(arg->value.str);
	if (len > ctx->max_searchword_len)
		ctx->max_searchword_len = len;

	if (ctx->size >= len) {
		max = ctx->size-len;
		for (i = 0, p = ctx->msg; i <= max; i++, p++) {
			if (i_toupper(*p) == arg->value.str[0] &&
			    strncasecmp(p, arg->value.str, len) == 0) {
				/* match */
				ARG_SET_RESULT(arg, 1);
				return;
			}
		}
	}
}

static void search_text_body(MailSearchArg *arg, void *context)
{
	SearchTextContext *ctx = context;

	if (arg->type == SEARCH_TEXT || arg->type == SEARCH_BODY)
		search_text(arg, ctx);
}

static void search_arg_match_data(IBuffer *inbuf, MailSearchArg *args,
				  MailSearchForeachFunc search_func)
{
	SearchTextContext ctx;
	const unsigned char *data;
	size_t size, max_searchword_len;

	memset(&ctx, 0, sizeof(ctx));
	ctx.args = args;

	/* first get the max. search keyword length */
	mail_search_args_foreach(args, search_func, &ctx);
        max_searchword_len = ctx.max_searchword_len;

	/* do this in blocks: read data, compare it for all search words, skip
	   for block size - (strlen(largest_searchword)-1) and continue. */
	while (i_buffer_read_data(inbuf, &data, &size,
				  max_searchword_len-1) > 0) {
		ctx.msg = (const char *) data;
		ctx.size = size;
		mail_search_args_foreach(args, search_func, &ctx);
		i_buffer_skip(inbuf, size - (max_searchword_len-1));
	}

	if (size > 0) {
		/* last block */
		ctx.msg = (const char *) data;
		ctx.size = size;
		mail_search_args_foreach(args, search_func, &ctx);
		i_buffer_skip(inbuf, size);
	}

	i_buffer_set_read_limit(inbuf, 0);
}

static int search_arg_match_text(MailSearchArg *args, SearchIndexContext *ctx)
{
	IBuffer *inbuf;
	MessageSize hdr_size;
	int have_headers, have_body, have_text;

	/* first check what we need to use */
	mail_search_args_analyze(args, &have_headers, &have_body, &have_text);
	if (!have_headers && !have_body && !have_text)
		return TRUE;

	if (!imap_msgcache_get_rfc822(search_open_cache(ctx), &inbuf,
				      have_headers ? NULL : &hdr_size, NULL))
		return FALSE;

	if (have_headers || have_text) {
		SearchHeaderContext hdr_ctx;

		memset(&hdr_ctx, 0, sizeof(hdr_ctx));

		/* header checks */
		hdr_ctx.index_context = ctx;
		hdr_ctx.custom_header = TRUE;
		hdr_ctx.args = args;
		message_parse_header(NULL, inbuf, &hdr_size,
				     search_header, &hdr_ctx);
	}

	if (have_text || have_body) {
		if (inbuf->v_offset == 0) {
			/* skip over headers */
			i_buffer_skip(inbuf, hdr_size.physical_size);
		}

		search_arg_match_data(inbuf, args, search_text_body);
	}
	return TRUE;
}

static void seq_update(const char *set, unsigned int *first_seq,
		       unsigned int *last_seq, unsigned int max_value)
{
	unsigned int seq;

	while (*set != '\0') {
		if (*set == '*') {
			seq = max_value;
			set++;
		} else {
			seq = 0;
			while (*set >= '0' && *set <= '9') {
				seq = seq*10 + (*set-'0');
				set++;
			}
		}

		if (seq != 0) {
			if (*first_seq == 0 || seq < *first_seq)
				*first_seq = seq;
			if (*last_seq == 0 || seq > *last_seq)
				*last_seq = seq;
		}

		set++;
	}
}

static void search_get_sequid(IndexMailbox *ibox, MailSearchArg *args,
			      unsigned int *first_seq, unsigned int *last_seq,
			      unsigned int *first_uid, unsigned int *last_uid)
{
	for (; args != NULL; args = args->next) {
		if (args->type == SEARCH_OR || args->type == SEARCH_SUB) {
			search_get_sequid(ibox, args->value.subargs,
					  first_seq, last_seq,
					  first_uid, last_uid);
		} if (args->type == SEARCH_SET) {
			seq_update(args->value.str, first_seq, last_seq,
				   ibox->synced_messages_count);
		} else if (args->type == SEARCH_UID) {
			seq_update(args->value.str, first_uid, last_uid,
				   ibox->index->header->next_uid-1);
		} else if (args->type == SEARCH_ALL) {
			/* go through everything */
			*first_seq = 1;
			*last_seq = ibox->synced_messages_count;
			return;
		}
	}
}

static int search_limit_by_flags(IndexMailbox *ibox, MailSearchArg *args,
				 unsigned int *first_uid,
				 unsigned int *last_uid)
{
	MailIndexHeader *hdr;
	unsigned int uid;

	hdr = ibox->index->header;
	for (; args != NULL; args = args->next) {
		if (args->type == SEARCH_SEEN) {
			/* SEEN with 0 seen? */
			if (!args->not && hdr->seen_messages_count == 0)
				return FALSE;

			/* UNSEEN with all seen? */
			if (args->not &&
			    hdr->seen_messages_count == hdr->messages_count)
				return FALSE;

			/* UNSEEN with lowwater limiting */
			uid = hdr->first_unseen_uid_lowwater;
			if (args->not && *first_uid < uid)
				*first_uid = uid;
		}

		if (args->type == SEARCH_DELETED) {
			/* DELETED with 0 deleted? */
			if (!args->not && hdr->deleted_messages_count == 0)
				return FALSE;

			/* UNDELETED with all deleted? */
			if (!args->not &&
			    hdr->deleted_messages_count == hdr->messages_count)
				return FALSE;

			/* DELETED with lowwater limiting */
			uid = hdr->first_deleted_uid_lowwater;
			if (!args->not && *first_uid < uid)
				*first_uid = uid;
		}

		if (args->type == SEARCH_RECENT) {
			uid = ibox->index->first_recent_uid;
			if (!args->not && *first_uid < uid)
				*first_uid = ibox->index->first_recent_uid;
			else if (args->not && *last_uid >= uid)
				*last_uid = uid-1;
		}
	}

	return *first_uid <= *last_uid;
}

static unsigned int client_seq_to_uid(MailIndex *index, unsigned int seq)
{
	MailIndexRecord *rec;
	unsigned int expunges_before;

	(void)mail_modifylog_seq_get_expunges(index->modifylog, seq, seq,
					      &expunges_before);
	seq -= expunges_before;

	rec = index->lookup(index, seq);
	return rec == NULL ? 0 : rec->uid;
}

static int search_get_uid_range(IndexMailbox *ibox, MailSearchArg *args,
				unsigned int *first_uid,
				unsigned int *last_uid)
{
	unsigned int first_seq, last_seq, uid;

	*first_uid = *last_uid = 0;
	first_seq = last_seq = 0;

	search_get_sequid(ibox, args, &first_seq, &last_seq,
			  first_uid, last_uid);

	/* seq_update() should make sure that these can't happen */
	i_assert(first_seq <= last_seq);
	i_assert(*first_uid <= *last_uid);

	if (first_seq > 1) {
		uid = client_seq_to_uid(ibox->index, first_seq);
		if (uid == 0)
			return FALSE;

		if (*first_uid == 0 || uid < *first_uid)
			*first_uid = uid;
	}

	if (last_seq > 1 && last_seq != ibox->synced_messages_count) {
		uid = client_seq_to_uid(ibox->index, last_seq);
		if (uid == 0)
			return FALSE;

		if (uid > *last_uid)
			*last_uid = uid;
	}

	if (*first_uid == 0)
		*first_uid = 1;
	if (*last_uid == 0)
		*last_uid = ibox->index->header->next_uid-1;

	/* UNSEEN and DELETED in root search level may limit the range */
	if (!search_limit_by_flags(ibox, args, first_uid, last_uid))
		return FALSE;

	i_assert(*first_uid <= *last_uid);
	return TRUE;
}

static int search_messages(IndexMailbox *ibox, const char *charset,
			   MailSearchArg *args, OBuffer *outbuf, int uid_result)
{
	SearchIndexContext ctx;
	MailIndexRecord *rec;
        MailSearchArg *arg;
	const ModifyLogExpunge *expunges;
	unsigned int first_uid, last_uid, client_seq, expunges_before;
	char num[MAX_LARGEST_T_STRLEN+10];
	int found, failed;

	if (ibox->synced_messages_count == 0)
		return TRUE;

	/* see if we can limit the records we look at */
	if (!search_get_uid_range(ibox, args, &first_uid, &last_uid))
		return TRUE;

	rec = ibox->index->lookup_uid_range(ibox->index, first_uid, last_uid,
					    &client_seq);
	if (rec == NULL)
		return TRUE;

	expunges = mail_modifylog_uid_get_expunges(ibox->index->modifylog,
						   rec->uid, last_uid,
						   &expunges_before);
	client_seq += expunges_before;

	memset(&ctx, 0, sizeof(ctx));
	ctx.ibox = ibox;
	ctx.charset = charset;

	for (; rec != NULL && rec->uid <= last_uid; client_seq++) {
		while (expunges->uid1 != 0 && expunges->uid1 < rec->uid) {
			i_assert(expunges->uid2 < rec->uid);

			expunges++;
			client_seq += expunges->seq_count;
		}
		i_assert(!(expunges->uid1 <= rec->uid &&
			   expunges->uid2 >= rec->uid));

		ctx.rec = rec;
		ctx.client_seq = client_seq;
		ctx.cached = FALSE;

		mail_search_args_reset(args);

		t_push();
		mail_search_args_foreach(args, search_index_arg, &ctx);
		mail_search_args_foreach(args, search_cached_arg, &ctx);
		mail_search_args_foreach(args, search_envelope_arg, &ctx);
		failed = !search_arg_match_text(args, &ctx);
                imap_msgcache_close(ibox->cache);
		t_pop();

		if (ctx.error != NULL)
			break;

		if (!failed) {
			found = TRUE;
			for (arg = args; arg != NULL; arg = arg->next) {
				if (arg->result != 1) {
					found = FALSE;
					break;
				}
			}

			if (found) {
				i_snprintf(num, sizeof(num), " %u",
					   uid_result ? rec->uid : client_seq);
				o_buffer_send(outbuf, num, strlen(num));
			}
		}

		rec = ibox->index->next(ibox->index, rec);
	}

	if (ctx.hdr_pool != NULL)
		pool_unref(ctx.hdr_pool);

	if (ctx.error != NULL)
		mail_storage_set_error(ibox->box.storage, "%s", ctx.error);
	return ctx.error == NULL;
}

int index_storage_search(Mailbox *box, const char *charset, MailSearchArg *args,
			 OBuffer *outbuf, int uid_result)
{
	IndexMailbox *ibox = (IndexMailbox *) box;
	int failed;

	if (!index_storage_sync_and_lock(ibox, TRUE, MAIL_LOCK_SHARED))
		return FALSE;

	o_buffer_send(outbuf, "* SEARCH", 8);
	failed = !search_messages(ibox, charset, args, outbuf, uid_result);
	o_buffer_send(outbuf, "\r\n", 2);

	if (!ibox->index->set_lock(ibox->index, MAIL_LOCK_UNLOCK))
		return mail_storage_set_index_error(ibox);

	return !failed;
}
