/* Copyright (c) 2002-2011 Dovecot authors, see the included COPYING file */

#include "lib.h"
#include "buffer.h"
#include "istream.h"
#include "str.h"
#include "str-find.h"
#include "charset-utf8.h"
#include "rfc822-parser.h"
#include "message-decoder.h"
#include "message-parser.h"
#include "message-search.h"

struct message_search_context {
	char *key;
	char *key_charset;
	unsigned int key_len;

	enum message_search_flags flags;
	struct str_find_context *str_find_ctx;
	struct message_part *prev_part;

	struct message_decoder_context *decoder;
	unsigned int content_type_text:1; /* text/any or message/any */
};

static int
message_search_init_real(const char *key, const char *charset,
			 enum message_search_flags flags,
			 struct message_search_context **ctx_r)
{
	struct message_search_context *ctx;
	string_t *key_utf8;
	enum charset_result result;
	int ret;

	key_utf8 = t_str_new(128);
	if (charset_to_utf8_str(charset, CHARSET_FLAG_DECOMP_TITLECASE,
				key, key_utf8, &result) < 0)
		ret = 0;
	else if (result != CHARSET_RET_OK)
		ret = -1;
	else {
		ctx = *ctx_r = i_new(struct message_search_context, 1);
		ctx->key = i_strdup(str_c(key_utf8));
		ctx->key_len = str_len(key_utf8);
		ctx->key_charset = i_strdup(charset);
		ctx->flags = flags;
		ctx->decoder = message_decoder_init(MESSAGE_DECODER_FLAG_DTCASE);
		ctx->str_find_ctx = str_find_init(default_pool, ctx->key);
		ret = 1;
	}
	return ret;
}

int message_search_init(const char *key, const char *charset,
			enum message_search_flags flags,
			struct message_search_context **ctx_r)
{
	int ret;

	T_BEGIN {
		ret = message_search_init_real(key, charset, flags, ctx_r);
	} T_END;
	return ret;
}

void message_search_deinit(struct message_search_context **_ctx)
{
	struct message_search_context *ctx = *_ctx;

	*_ctx = NULL;
	str_find_deinit(&ctx->str_find_ctx);
	message_decoder_deinit(&ctx->decoder);
	i_free(ctx->key);
	i_free(ctx->key_charset);
	i_free(ctx);
}

static void parse_content_type(struct message_search_context *ctx,
			       struct message_header_line *hdr)
{
	struct rfc822_parser_context parser;
	string_t *content_type;

	rfc822_parser_init(&parser, hdr->full_value, hdr->full_value_len, NULL);
	(void)rfc822_skip_lwsp(&parser);

	content_type = t_str_new(64);
	if (rfc822_parse_content_type(&parser, content_type) >= 0) {
		ctx->content_type_text =
			strncasecmp(str_c(content_type), "text/", 5) == 0 ||
			strncasecmp(str_c(content_type), "message/", 8) == 0;
	}
}

static void handle_header(struct message_search_context *ctx,
			  struct message_header_line *hdr)
{
	if (hdr->name_len == 12 &&
	    strcasecmp(hdr->name, "Content-Type") == 0) {
		if (hdr->continues) {
			hdr->use_full_value = TRUE;
			return;
		}
		T_BEGIN {
			parse_content_type(ctx, hdr);
		} T_END;
	}
}

static bool search_header(struct message_search_context *ctx,
			  const struct message_header_line *hdr)
{
	static const unsigned char crlf[2] = { '\r', '\n' };

	return str_find_more(ctx->str_find_ctx,
			     (const unsigned char *)hdr->name, hdr->name_len) ||
		str_find_more(ctx->str_find_ctx,
			      hdr->middle, hdr->middle_len) ||
		str_find_more(ctx->str_find_ctx, hdr->full_value,
			      hdr->full_value_len) ||
		(!hdr->no_newline &&
		 str_find_more(ctx->str_find_ctx, crlf, 2));
}

static bool message_search_more_decoded2(struct message_search_context *ctx,
					 struct message_block *block)
{
	if (block->hdr != NULL) {
		if (search_header(ctx, block->hdr))
			return TRUE;
	} else {
		if (str_find_more(ctx->str_find_ctx, block->data, block->size))
			return TRUE;
	}
	return FALSE;
}

bool message_search_more(struct message_search_context *ctx,
			 struct message_block *raw_block)
{
	struct message_header_line *hdr = raw_block->hdr;
	struct message_block block;

	if (raw_block->part != ctx->prev_part) {
		/* part changes. we must change this before looking at
		   content type */
		message_search_reset(ctx);
		ctx->prev_part = raw_block->part;

		if (hdr == NULL) {
			/* we're returning to a multipart message. */
			ctx->content_type_text = FALSE;
		}
	}

	if (hdr != NULL) {
		handle_header(ctx, hdr);
		if ((ctx->flags & MESSAGE_SEARCH_FLAG_SKIP_HEADERS) != 0) {
			/* we want to search only message bodies, but
			   but decoder needs some headers so that it can
			   decode the body properly. */
			if (hdr->name_len != 12 && hdr->name_len != 25)
				return FALSE;
			if (strcasecmp(hdr->name, "Content-Type") != 0 &&
			    strcasecmp(hdr->name,
				       "Content-Transfer-Encoding") != 0)
				return FALSE;
		}
	} else {
		/* body */
		if (!ctx->content_type_text)
			return FALSE;
	}
	if (!message_decoder_decode_next_block(ctx->decoder, raw_block, &block))
		return FALSE;

	if (block.hdr != NULL &&
	    (ctx->flags & MESSAGE_SEARCH_FLAG_SKIP_HEADERS) != 0) {
		/* Content-* header */
		return FALSE;
	}

	return message_search_more_decoded2(ctx, &block);
}

bool message_search_more_decoded(struct message_search_context *ctx,
				 struct message_block *block)
{
	if (block->part != ctx->prev_part) {
		/* part changes */
		message_search_reset(ctx);
		ctx->prev_part = block->part;
	}

	return message_search_more_decoded2(ctx, block);
}

void message_search_reset(struct message_search_context *ctx)
{
	/* Content-Type defaults to text/plain */
	ctx->content_type_text = TRUE;

	ctx->prev_part = NULL;
	str_find_reset(ctx->str_find_ctx);
	message_decoder_decode_reset(ctx->decoder);
}

static int
message_search_msg_real(struct message_search_context *ctx,
			struct istream *input, struct message_part *parts)
{
	const enum message_header_parser_flags hdr_parser_flags =
		MESSAGE_HEADER_PARSER_FLAG_CLEAN_ONELINE;
	struct message_parser_ctx *parser_ctx;
	struct message_block raw_block;
	struct message_part *new_parts;
	int ret;

	message_search_reset(ctx);

	if (parts != NULL) {
		parser_ctx = message_parser_init_from_parts(parts,
						input, hdr_parser_flags, 0);
	} else {
		parser_ctx = message_parser_init(pool_datastack_create(),
						 input, hdr_parser_flags, 0);
	}

	while ((ret = message_parser_parse_next_block(parser_ctx,
						      &raw_block)) > 0) {
		if (message_search_more(ctx, &raw_block)) {
			ret = 1;
			break;
		}
	}
	i_assert(ret != 0);
	if (ret < 0 && input->stream_errno == 0) {
		/* normal exit */
		ret = 0;
	}
	if (message_parser_deinit(&parser_ctx, &new_parts) < 0) {
		/* broken parts */
		input->stream_errno = 0;
		ret = -1;
	}
	return ret;
}

int message_search_msg(struct message_search_context *ctx,
		       struct istream *input, struct message_part *parts)
{
	int ret;

	T_BEGIN {
		ret = message_search_msg_real(ctx, input, parts);
	} T_END;
	return ret;
}
