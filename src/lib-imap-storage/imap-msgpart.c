/* Copyright (c) 2012 Dovecot authors, see the included COPYING file */

#include "lib.h"
#include "str.h"
#include "array.h"
#include "istream.h"
#include "istream-crlf.h"
#include "istream-nonuls.h"
#include "istream-base64.h"
#include "istream-header-filter.h"
#include "istream-qp.h"
#include "ostream.h"
#include "message-parser.h"
#include "message-decoder.h"
#include "mail-storage-private.h"
#include "mail-namespace.h"
#include "imap-bodystructure.h"
#include "imap-parser.h"
#include "imap-msgpart.h"

enum fetch_type {
	FETCH_FULL,
	FETCH_MIME,
	FETCH_MIME_BODY,
	FETCH_HEADER,
	FETCH_HEADER_FIELDS,
	FETCH_HEADER_FIELDS_NOT,
	FETCH_BODY
};

struct imap_msgpart {
	pool_t pool;

	/* "" for root, otherwise e.g. "1.2.3". the .MIME, .HEADER, etc.
	   suffix not included */
	const char *section_number;
	enum fetch_type fetch_type;
	enum mail_fetch_field wanted_fields;

	/* HEADER.FIELDS[.NOT] (list of headers) */
        struct mailbox_header_lookup_ctx *header_ctx;
	const char *const *headers;

	/* which part of the message part to fetch (default: 0..(uoff_t)-1) */
	uoff_t partial_offset, partial_size;

	unsigned int decode_cte_to_binary:1;
};

struct imap_msgpart_open_ctx {
	/* from matching message_part, set after opening: */
	uoff_t physical_pos;
	struct message_size mime_hdr_size;
	struct message_size mime_body_size;
};

static struct imap_msgpart *imap_msgpart_type(enum fetch_type fetch_type)
{
	struct imap_msgpart *msgpart;
	pool_t pool;

	pool = pool_alloconly_create("imap msgpart", sizeof(*msgpart)+32);
	msgpart = p_new(pool, struct imap_msgpart, 1);
	msgpart->pool = pool;
	msgpart->partial_size = (uoff_t)-1;
	msgpart->fetch_type = fetch_type;
	msgpart->section_number = "";
	if (fetch_type == FETCH_HEADER || fetch_type == FETCH_FULL)
		msgpart->wanted_fields |= MAIL_FETCH_STREAM_HEADER;
	if (fetch_type == FETCH_BODY || fetch_type == FETCH_FULL)
		msgpart->wanted_fields |= MAIL_FETCH_STREAM_BODY;
	return msgpart;
}

struct imap_msgpart *imap_msgpart_full(void)
{
	return imap_msgpart_type(FETCH_FULL);
}

struct imap_msgpart *imap_msgpart_header(void)
{
	return imap_msgpart_type(FETCH_HEADER);
}

struct imap_msgpart *imap_msgpart_body(void)
{
	return imap_msgpart_type(FETCH_BODY);
}

static struct message_part *
imap_msgpart_find(struct message_part *parts, const char *section)
{
	struct message_part *part = parts;
	const char *path;
	unsigned int num;

	path = section;
	while (*path >= '0' && *path <= '9' && part != NULL) {
		/* get part number, we have already verified its validity */
		num = 0;
		while (*path != '\0' && *path != '.') {
			i_assert(*path >= '0' && *path <= '9');

			num = num*10 + (*path - '0');
			path++;
		}

		if (*path == '.')
			path++;

		if ((part->flags & MESSAGE_PART_FLAG_MULTIPART) != 0) {
			/* find the part */
			part = part->children;
			for (; num > 1 && part != NULL; num--)
				part = part->next;
		} else {
			/* only 1 allowed with non-multipart messages.
			   if the child isn't message/rfc822, the path must be
			   finished after this. */
			if (num != 1)
				part = NULL;
			else if (*path != '\0' &&
				 (part->flags & MESSAGE_PART_FLAG_MESSAGE_RFC822) == 0)
				part = NULL;
		}

		if (part != NULL &&
		    (part->flags & MESSAGE_PART_FLAG_MESSAGE_RFC822) != 0 &&
		    (*path >= '0' && *path <= '9')) {
			/* if we continue inside the message/rfc822, skip this
			   body part */
			part = part->children;
		}
	}
	i_assert(part == NULL || *path == '\0');
	return part;
}

static int
imap_msgpart_get_header_fields(pool_t pool, const char *header_list,
			       ARRAY_TYPE(const_string) *fields)
{
	struct istream *input;
	struct imap_parser *parser;
	const struct imap_arg *args, *hdr_list;
	unsigned int list_count;
	unsigned int i;
	int result = 0;

	input = i_stream_create_from_data(header_list, strlen(header_list));
	parser = imap_parser_create(input, NULL, (size_t)-1);

	if (imap_parser_finish_line(parser, 0, 0, &args) > 0 &&
	    imap_arg_get_list_full(args, &hdr_list, &list_count) &&
	    args[1].type == IMAP_ARG_EOL &&
	    list_count > 0) {
		const char *value;
		
		p_array_init(fields, pool, list_count);
		for (i = 0; i < list_count; i++) {
			if (!imap_arg_get_astring(&hdr_list[i], &value)) {
				result = -1;
				break;
			}

			value = p_strdup(pool, t_str_ucase(value));
			array_append(fields, &value, 1);
		}
	} else {
		result = -1;
	}

	/* istream-header-filter requires headers to be sorted */
	array_sort(fields, i_strcasecmp_p);

	imap_parser_unref(&parser);
	i_stream_unref(&input);
	return result;
}

static int
imap_msgpart_parse_header_fields(struct imap_msgpart *msgpart,
				 const char *header_list)
{
	ARRAY_TYPE(const_string) fields;

	/* HEADER.FIELDS (list), HEADER.FIELDS.NOT (list) */
	if (imap_msgpart_get_header_fields(msgpart->pool, header_list,
					   &fields) < 0)
		return -1;

	array_append_zero(&fields);
	msgpart->headers = array_idx(&fields, 0);
	return 0;
}

int imap_msgpart_parse(const char *section, struct imap_msgpart **msgpart_r)
{
	struct imap_msgpart *msgpart;
	pool_t pool;
	unsigned int i;
	bool next_digit;
	int ret;

	pool = pool_alloconly_create("imap msgpart", 1024);
	msgpart = *msgpart_r = p_new(pool, struct imap_msgpart, 1);
	msgpart->pool = pool;
	msgpart->partial_size = (uoff_t)-1;

	/* get the section number */
	next_digit = TRUE;
	for (i = 0; section[i] != '\0'; i++) {
		if (section[i] >= '0' && section[i] <= '9') {
			next_digit = FALSE;
		} else if (!next_digit && section[i] == '.') {
			next_digit = TRUE;
		} else {
			break;
		}
	}
	if (i == 0) {
		/* [], [HEADER], etc. */
		msgpart->section_number = "";
	} else if (section[i] == '\0') {
		/* [1.2.3] */
		if (i > 0 && section[i-1] == '.') {
			pool_unref(&pool);
			return -1;
		}
		msgpart->section_number = p_strdup(pool, section);
		section = "";
	} else {
		/* [1.2.3.MIME], [1.2.3.HEADER], etc */
		if (section[i-1] != '.') {
			pool_unref(&pool);
			return -1;
		}
		msgpart->section_number = p_strndup(pool, section, i-1);
		section += i;
	}

	if (*section == '\0') {
		msgpart->wanted_fields |= MAIL_FETCH_STREAM_BODY;
		if (*msgpart->section_number == '\0') {
			/* BODY[] - header+body */
			msgpart->fetch_type = FETCH_FULL;
			msgpart->wanted_fields |= MAIL_FETCH_STREAM_HEADER;
		} else {
			/* BODY[1] - body only */
			msgpart->fetch_type = FETCH_MIME_BODY;
		}
		return 0;
	}
	section = t_str_ucase(section);

	if (strcmp(section, "MIME") == 0) {
		msgpart->fetch_type = FETCH_MIME;
		msgpart->wanted_fields |= MAIL_FETCH_STREAM_BODY;
	} else if (strcmp(section, "TEXT") == 0) {
		/* body (for root or for message/rfc822) */
		msgpart->fetch_type = FETCH_BODY;
		msgpart->wanted_fields |= MAIL_FETCH_STREAM_BODY;
	} else if (strncmp(section, "HEADER", 6) == 0) {
		/* header (for root or for message/rfc822) */
		if (section[6] == '\0') {
			msgpart->fetch_type = FETCH_HEADER;
			ret = 0;
		} else if (strncmp(section, "HEADER.FIELDS ", 14) == 0) {
			msgpart->fetch_type = FETCH_HEADER_FIELDS;
			ret = imap_msgpart_parse_header_fields(msgpart,
							       section+14);
		} else if (strncmp(section, "HEADER.FIELDS.NOT ", 18) == 0) {
			msgpart->fetch_type = FETCH_HEADER_FIELDS_NOT;
			ret = imap_msgpart_parse_header_fields(msgpart,
							       section+18);
		} else {
			ret = -1;
		}
		if (ret < 0) {
			imap_msgpart_free(&msgpart);
			return -1;
		}
		if (msgpart->fetch_type == FETCH_HEADER_FIELDS) {
			/* we may be able to get this from cache, don't give a
			   wanted_fields hint */
		} else if (*msgpart->section_number == '\0')
			msgpart->wanted_fields |= MAIL_FETCH_STREAM_HEADER;
		else
			msgpart->wanted_fields |= MAIL_FETCH_STREAM_BODY;
	} else {
		i_unreached();
	}
	return 0;
}

void imap_msgpart_free(struct imap_msgpart **_msgpart)
{
	struct imap_msgpart *msgpart = *_msgpart;

	*_msgpart = NULL;

	imap_msgpart_close_mailbox(msgpart);
	pool_unref(&msgpart->pool);
}

void imap_msgpart_set_decode_to_binary(struct imap_msgpart *msgpart)
{
	msgpart->decode_cte_to_binary = TRUE;
}

void imap_msgpart_set_partial(struct imap_msgpart *msgpart,
			      uoff_t offset, uoff_t size)
{
	msgpart->partial_offset = offset;
	msgpart->partial_size = size;
}

uoff_t imap_msgpart_get_partial_offset(struct imap_msgpart *msgpart)
{
	return msgpart->partial_offset;
}

enum mail_fetch_field imap_msgpart_get_fetch_data(struct imap_msgpart *msgpart)
{
	return msgpart->wanted_fields;
}

static int
imap_msgpart_get_partial_header(struct mail *mail, struct istream *mail_input,
				const struct imap_msgpart *msgpart,
				struct message_size *hdr_size_r,
				struct imap_msgpart_open_result *result_r)
{
	const char *const *hdr_fields = msgpart->headers;
	unsigned int hdr_count = str_array_length(hdr_fields);
	struct istream *input;
	bool has_nuls;

	if (msgpart->fetch_type == FETCH_HEADER_FIELDS) {
		input = i_stream_create_header_filter(mail_input,
						      HEADER_FILTER_INCLUDE |
						      HEADER_FILTER_HIDE_BODY,
						      hdr_fields, hdr_count,
						      *null_header_filter_callback,
						      (void *)NULL);
	} else {
		i_assert(msgpart->fetch_type == FETCH_HEADER_FIELDS_NOT);
		input = i_stream_create_header_filter(mail_input,
						      HEADER_FILTER_EXCLUDE |
						      HEADER_FILTER_HIDE_BODY,
						      hdr_fields, hdr_count,
						      *null_header_filter_callback,
						      (void *)NULL);
	}

	if (message_get_header_size(input, hdr_size_r, &has_nuls) < 0) {
		errno = input->stream_errno;
		mail_storage_set_critical(mail->box->storage,
			"read(%s) failed: %m", i_stream_get_name(mail_input));
		i_stream_unref(&input);
		return -1;
	}
	i_stream_seek(input, 0);
	result_r->input = input;
	result_r->size = hdr_size_r->virtual_size;
	return 0;
}

static void
skip_using_parts(struct mail *mail, struct istream *input,
		 uoff_t physical_start, uoff_t *virtual_skip)
{
	enum mail_lookup_abort old_lookup_abort;
	struct message_part *parts, *part;
	uoff_t vpos;
	int ret;

	old_lookup_abort = mail->lookup_abort;
	mail->lookup_abort = MAIL_LOOKUP_ABORT_NOT_IN_CACHE;
	ret = mail_get_parts(mail, &parts);
	mail->lookup_abort = old_lookup_abort;
	if (ret < 0)
		return;

	for (part = parts, vpos = 0; part != NULL; ) {
		if (vpos + part->header_size.virtual_size > *virtual_skip)
			break;
		/* skip header */
		vpos += part->header_size.virtual_size;
		*virtual_skip -= part->header_size.virtual_size;
		i_stream_seek(input, physical_start + part->physical_pos +
			      part->header_size.physical_size);

		if (vpos + part->body_size.virtual_size <= *virtual_skip) {
			/* skip body */
			vpos += part->body_size.virtual_size;
			*virtual_skip -= part->body_size.virtual_size;
			i_stream_seek(input, physical_start +
				      part->physical_pos +
				      part->header_size.physical_size +
				      part->body_size.physical_size);
			part = part->next;
		} else {
			/* maybe we have a child and can skip using it? */
			part = part->children;
		}
	}
}

static struct istream *
imap_msgpart_crlf_seek(struct mail *mail, struct istream *input,
		       const struct imap_msgpart *msgpart)
{
	struct mail_msgpart_partial_cache *cache = &mail->box->partial_cache;
	struct istream *crlf_input, *errinput;
	uoff_t physical_start = input->v_offset;
	uoff_t virtual_skip = msgpart->partial_offset;
	bool cr_skipped;

	i_assert(msgpart->headers == NULL); /* HEADER.FIELDS returns CRLFs */

	if (virtual_skip == 0) {
		/* no need to seek */
	} else if (cache->uid == mail->uid &&
		   cache->physical_start == physical_start &&
		   cache->virtual_pos < virtual_skip) {
		/* use cache */
		i_stream_seek(input, physical_start + cache->physical_pos);
		virtual_skip -= cache->virtual_pos;
	} else {
		/* can't use cache, but maybe we can skip faster using the
		   message parts. */
		skip_using_parts(mail, input, physical_start, &virtual_skip);
	}
	if (message_skip_virtual(input, virtual_skip, &cr_skipped) < 0) {
		errinput = i_stream_create_error(errno);
		i_stream_set_name(errinput, i_stream_get_name(input));
		return errinput;
	}

	if ((msgpart->partial_offset != 0 ||
	     msgpart->partial_size != (uoff_t)-1) && !input->eof) {
		/* update cache */
		cache->uid = mail->uid;
		cache->physical_start = physical_start;
		cache->physical_pos = input->v_offset - physical_start;
		cache->virtual_pos = msgpart->partial_offset;
		if (cr_skipped) {
			/* the physical_pos points to virtual CRLF, but
			   virtual_pos already skipped CR. that can't work,
			   so seek back the virtual CR */
			cache->virtual_pos--;
		}
	}
	crlf_input = i_stream_create_crlf(input);
	if (cr_skipped)
		i_stream_skip(crlf_input, 1);
	i_stream_unref(&input);
	return crlf_input;
}

static void
imap_msgpart_get_partial(struct mail *mail, const struct imap_msgpart *msgpart,
			 bool convert_nuls, bool use_partial_cache,
			 const struct message_size *part_size,
			 struct imap_msgpart_open_result *result)
{
	struct istream *input2;
	uoff_t bytes_left;

	/* input is already seeked to the beginning of the wanted data */

	if (msgpart->partial_offset >= part_size->virtual_size) {
		/* can't seek past the MIME part */
		i_stream_unref(&result->input);
		result->input = i_stream_create_from_data("", 0);
		result->size = 0;
		return;
	}

	if (part_size->virtual_size == part_size->physical_size) {
		/* input has CRLF linefeeds, we can quickly seek to
		   wanted position */
		i_stream_skip(result->input, msgpart->partial_offset);
	} else {
		/* input has LF linefeeds. it can be slow to seek to wanted
		   position, so try to do caching whenever possible */
		i_assert(use_partial_cache);
		result->input = imap_msgpart_crlf_seek(mail, result->input,
						       msgpart);
	}

	bytes_left = part_size->virtual_size - msgpart->partial_offset;
	if (msgpart->partial_size <= bytes_left) {
		/* limit output to specified number of bytes */
		result->size = msgpart->partial_size;
	} else {
		/* send all bytes */
		result->size = bytes_left;
	}

	if (!mail->has_no_nuls && convert_nuls) {
		/* IMAP literals must not contain NULs. change them to
		   0x80 characters. */
		input2 = i_stream_create_nonuls(result->input, 0x80);
		i_stream_unref(&result->input);
		result->input = input2;
	}
	input2 = i_stream_create_limit(result->input, result->size);
	i_stream_unref(&result->input);
	result->input = input2;
}

static int
imap_msgpart_find_part(struct mail *mail, const struct imap_msgpart *msgpart,
		       struct message_part **part_r)
{
	struct message_part *parts, *part = NULL;

	if (*msgpart->section_number == '\0') {
		*part_r = NULL;
		return 1;
	}

	if (mail_get_parts(mail, &parts) < 0)
		return -1;
	part = imap_msgpart_find(parts, msgpart->section_number);
	if (part == NULL) {
		/* MIME part not found. */
		*part_r = NULL;
		return 0;
	}

	switch (msgpart->fetch_type) {
	case FETCH_MIME:
		/* What to do if this is a message/rfc822? Does it have
		   MIME headers or not? Possibilities are: a) no, return
		   empty string (UW-IMAP does this), b) return the same as
		   HEADER. Dovecot has done b) for a long time and it's not
		   very clear which one is correct, so we'll just continue
		   with b) */
	case FETCH_FULL:
	case FETCH_MIME_BODY:
		break;
	case FETCH_HEADER:
	case FETCH_HEADER_FIELDS:
	case FETCH_HEADER_FIELDS_NOT:
	case FETCH_BODY:
		/* fetching message/rfc822 part's header/body */
		if ((part->flags & MESSAGE_PART_FLAG_MESSAGE_RFC822) == 0) {
			*part_r = NULL;
			return 0;
		}
		i_assert(part->children != NULL &&
			 part->children->next == NULL);
		part = part->children;
		break;
	}
	*part_r = part;
	return 1;
}

static int
imap_msgpart_open_normal(struct mail *mail, struct imap_msgpart *msgpart,
			 const struct message_part *part,
			 struct message_size *part_size_r,
			 struct imap_msgpart_open_result *result_r)
{
	struct message_size hdr_size, body_size;
	struct istream *input = NULL;

	memset(&hdr_size, 0, sizeof(hdr_size));
	memset(&body_size, 0, sizeof(body_size));
	memset(part_size_r, 0, sizeof(*part_size_r));

	if (*msgpart->section_number != '\0') {
		/* find the MIME part */
		if (mail_get_stream(mail, NULL, NULL, &input) < 0)
			return -1;

		i_stream_seek(input, part->physical_pos);
		hdr_size = part->header_size;
		body_size = part->body_size;
	} else switch (msgpart->fetch_type) {
	case FETCH_FULL:
		/* fetch the whole message */
		if (mail_get_stream(mail, NULL, NULL, &input) < 0 ||
		    mail_get_virtual_size(mail, &body_size.virtual_size) < 0 ||
		    mail_get_physical_size(mail, &body_size.physical_size) < 0)
			return -1;
		result_r->size_field = MAIL_FETCH_VIRTUAL_SIZE;
		break;
	case FETCH_MIME:
	case FETCH_MIME_BODY:
		i_unreached();
	case FETCH_HEADER:
	case FETCH_HEADER_FIELDS_NOT:
		/* fetch the message's header */
		if (mail_get_hdr_stream(mail, &hdr_size, &input) < 0)
			return -1;
		result_r->size_field = MAIL_FETCH_MESSAGE_PARTS;
		break;
	case FETCH_HEADER_FIELDS:
		/* try to lookup the headers from cache */
		if (msgpart->header_ctx == NULL) {
			msgpart->header_ctx =
				mailbox_header_lookup_init(mail->box,
							   msgpart->headers);
		}
		if (mail_get_header_stream(mail, msgpart->header_ctx,
					   &input) < 0)
			return -1;
		result_r->size_field = 0;
		break;
	case FETCH_BODY:
		/* fetch the message's body */
		if (mail_get_stream(mail, &hdr_size, &body_size, &input) < 0)
			return -1;
		result_r->size_field = MAIL_FETCH_MESSAGE_PARTS;
		break;
	}

	if (msgpart->headers != NULL) {
		/* return specific headers */
		return imap_msgpart_get_partial_header(mail, input, msgpart,
						       part_size_r, result_r);
	}

	switch (msgpart->fetch_type) {
	case FETCH_FULL:
		part_size_r->physical_size += body_size.physical_size;
		part_size_r->virtual_size += body_size.virtual_size;
		/* fall through */
	case FETCH_MIME:
	case FETCH_HEADER:
		part_size_r->physical_size += hdr_size.physical_size;
		part_size_r->virtual_size += hdr_size.virtual_size;
		break;
	case FETCH_HEADER_FIELDS:
	case FETCH_HEADER_FIELDS_NOT:
		i_unreached();
	case FETCH_BODY:
	case FETCH_MIME_BODY:
		i_stream_skip(input, hdr_size.physical_size);
		part_size_r->physical_size += body_size.physical_size;
		part_size_r->virtual_size += body_size.virtual_size;
		break;
	}

	result_r->input = input;
	i_stream_ref(input);
	return 0;
}

int imap_msgpart_open(struct mail *mail, struct imap_msgpart *msgpart,
		      struct imap_msgpart_open_result *result_r)
{
	struct message_part *part;
	struct message_size part_size;
	uoff_t size;
	bool include_hdr, binary, use_partial_cache;
	int ret;

	memset(result_r, 0, sizeof(*result_r));

	if ((ret = imap_msgpart_find_part(mail, msgpart, &part)) < 0)
		return -1;
	if (ret == 0) {
		/* MIME part not found. return an empty part. */
		result_r->input = i_stream_create_from_data("", 0);
		return 0;
	}

	if (msgpart->decode_cte_to_binary &&
	    (msgpart->fetch_type == FETCH_FULL ||
	     msgpart->fetch_type == FETCH_BODY ||
	     msgpart->fetch_type == FETCH_MIME_BODY)) {
		/* binary fetch */
		include_hdr = msgpart->fetch_type == FETCH_FULL;
		if (part == NULL) {
			if (mail_get_parts(mail, &part) < 0)
				return -1;
		}
		if (mail_get_binary_stream(mail, part, include_hdr, &size,
					   &binary, &result_r->input) < 0)
			return -1;
		part_size.virtual_size = size;
		part_size.physical_size = size;
		use_partial_cache = FALSE;
	} else {
		if (imap_msgpart_open_normal(mail, msgpart, part, &part_size,
					     result_r) < 0)
			return -1;
		binary = FALSE;
		use_partial_cache = TRUE;
	}

	if (binary && msgpart->decode_cte_to_binary)
		result_r->binary_decoded_input_has_nuls = TRUE;

	imap_msgpart_get_partial(mail, msgpart, !binary, use_partial_cache,
				 &part_size, result_r);
	return 0;
}

int imap_msgpart_size(struct mail *mail, struct imap_msgpart *msgpart,
		      uoff_t *size_r)
{
	struct imap_msgpart_open_result result;
	struct message_part *part;
	bool include_hdr;
	unsigned int lines;
	int ret;

	if (!msgpart->decode_cte_to_binary ||
	    (msgpart->fetch_type != FETCH_FULL &&
	     msgpart->fetch_type != FETCH_BODY &&
	     msgpart->fetch_type != FETCH_MIME_BODY)) {
		/* generic implementation */
		if (imap_msgpart_open(mail, msgpart, &result) < 0)
			return -1;
		i_stream_unref(&result.input);
		*size_r = result.size;
		return 0;
	}

	/* binary-optimized implementation: */
	if ((ret = imap_msgpart_find_part(mail, msgpart, &part)) < 0)
		return -1;
	if (ret == 0) {
		/* MIME part not found. return an empty part. */
		*size_r = 0;
		return 0;
	}
	if (part == NULL) {
		if (mail_get_parts(mail, &part) < 0)
			return -1;
	}
	include_hdr = msgpart->fetch_type == FETCH_FULL;
	return mail_get_binary_size(mail, part, include_hdr, size_r, &lines);
}

static int
imap_msgpart_parse_bodystructure(struct mail *mail,
				 struct message_part *all_parts)
{
	struct mail_private *pmail = (struct mail_private *)mail;
	const char *bodystructure, *error;

	if (mail_get_special(mail, MAIL_FETCH_IMAP_BODYSTRUCTURE,
			     &bodystructure) < 0)
		return -1;
	if (all_parts->context != NULL) {
		/* we just parsed the bodystructure */
		return 0;
	}

	if (imap_bodystructure_parse(bodystructure, pmail->data_pool,
				     all_parts, &error) < 0) {
		mail_storage_set_critical(mail->box->storage,
			"Invalid message_part/BODYSTRUCTURE %s: %s",
			bodystructure, error);
		mail_set_cache_corrupted(mail, MAIL_FETCH_MESSAGE_PARTS);
		mail_set_cache_corrupted(mail, MAIL_FETCH_IMAP_BODYSTRUCTURE);
		return -1;
	}
	return 0;
}

static int
imap_msgpart_vsizes_to_binary(struct mail *mail, const struct message_part *part,
			      struct message_part **binpart_r)
{
	struct message_part **pos;
	uoff_t size;
	unsigned int lines;

	if (mail_get_binary_size(mail, part, FALSE, &size, &lines) < 0)
		return -1;

	*binpart_r = t_new(struct message_part, 1);
	**binpart_r = *part;
	(*binpart_r)->body_size.virtual_size = size;
	(*binpart_r)->body_size.lines = lines;

	pos = &(*binpart_r)->children;
	for (part = part->children; part != NULL; part = part->next) {
		if (imap_msgpart_vsizes_to_binary(mail, part, pos) < 0)
			return -1;
		pos = &(*pos)->next;
	}
	return 0;
}

int imap_msgpart_bodypartstructure(struct mail *mail,
				   struct imap_msgpart *msgpart,
				   const char **bpstruct_r)
{
	struct message_part *all_parts, *part;
	string_t *bpstruct;
	int ret;

	/* if we start parsing the body in here, make sure we also parse the
	   BODYSTRUCTURE */
	mail_add_temp_wanted_fields(mail, MAIL_FETCH_IMAP_BODYSTRUCTURE, NULL);

	if ((ret = imap_msgpart_find_part(mail, msgpart, &part)) < 0)
		return -1;
	if (ret == 0) {
		/* MIME part not found. */
		*bpstruct_r = NULL;
		return 0;
	}

	if (mail_get_parts(mail, &all_parts) < 0)
		return -1;
	if (all_parts->context == NULL) {
		if (imap_msgpart_parse_bodystructure(mail, all_parts) < 0)
			return -1;
	}
	if (part == NULL)
		part = all_parts;

	if (msgpart->decode_cte_to_binary)
		ret = imap_msgpart_vsizes_to_binary(mail, part, &part);

	if (ret >= 0) {
		bpstruct = t_str_new(256);
		imap_bodystructure_write(part, bpstruct, TRUE);
		*bpstruct_r = str_c(bpstruct);
	}
	return ret < 0 ? -1 : 1;
}


void imap_msgpart_close_mailbox(struct imap_msgpart *msgpart)
{
	if (msgpart->header_ctx != NULL)
		mailbox_header_lookup_unref(&msgpart->header_ctx);
}
