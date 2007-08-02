/* Copyright (C) 2002-2006 Timo Sirainen */

#include "lib.h"
#include "str.h"
#include "istream.h"
#include "rfc822-parser.h"
#include "message-parser.h"

/* RFC-2046 requires boundaries are max. 70 chars + "--" prefix + "--" suffix.
   We'll add a bit more just in case. */
#define BOUNDARY_END_MAX_LEN (70 + 2 + 2 + 10)

struct message_boundary {
	struct message_boundary *next;

	struct message_part *part;
	const char *boundary;
	size_t len;

	unsigned int epilogue_found:1;
};

struct message_parser_ctx {
	pool_t parser_pool, part_pool;
	struct istream *input;
	struct message_part *parts, *part;

	enum message_header_parser_flags hdr_flags;
	enum message_parser_flags flags;

	const char *last_boundary;
	struct message_boundary *boundaries;

	size_t skip;
	char last_chr;
	unsigned int want_count;

	struct message_header_parser_ctx *hdr_parser_ctx;

	int (*parse_next_block)(struct message_parser_ctx *ctx,
				struct message_block *block_r);

	unsigned int part_seen_content_type:1;
};

message_part_header_callback_t *null_message_part_header_callback = NULL;

static int parse_next_header_init(struct message_parser_ctx *ctx,
				  struct message_block *block_r);
static int parse_next_body_to_boundary(struct message_parser_ctx *ctx,
				       struct message_block *block_r);
static int parse_next_body_to_eof(struct message_parser_ctx *ctx,
				  struct message_block *block_r);
static int preparsed_parse_next_header_init(struct message_parser_ctx *ctx,
					    struct message_block *block_r);

static struct message_boundary *
boundary_find(struct message_boundary *boundaries,
	      const unsigned char *data, size_t len)
{
	/* As MIME spec says: search from latest one to oldest one so that we
	   don't break if the same boundary is used in nested parts. Also the
	   full message line doesn't have to match the boundary, only the
	   beginning. */
	while (boundaries != NULL) {
		if (boundaries->len <= len &&
		    memcmp(boundaries->boundary, data, boundaries->len) == 0)
			return boundaries;

		boundaries = boundaries->next;
	}

	return NULL;
}

static void parse_body_add_block(struct message_parser_ctx *ctx,
				 struct message_block *block)
{
	unsigned int missing_cr_count = 0;
	const unsigned char *data = block->data;
	size_t i;

	block->hdr = NULL;

	for (i = 0; i < block->size; i++) {
		if (data[i] <= '\n') {
			if (data[i] == '\n') {
				ctx->part->body_size.lines++;
				if ((i > 0 && data[i-1] != '\r') ||
				    (i == 0 && ctx->last_chr != '\r'))
					missing_cr_count++;
			} else if (data[i] == '\0')
				ctx->part->flags |= MESSAGE_PART_FLAG_HAS_NULS;
		}
	}

	ctx->part->body_size.physical_size += block->size;
	ctx->part->body_size.virtual_size += block->size + missing_cr_count;

	ctx->last_chr = data[i-1];
	ctx->skip += block->size;
}

static int message_parser_read_more(struct message_parser_ctx *ctx,
				    struct message_block *block_r)
{
	int ret;

	if (ctx->skip > 0) {
		i_stream_skip(ctx->input, ctx->skip);
		ctx->skip = 0;
	}

	ret = i_stream_read_data(ctx->input, &block_r->data,
				 &block_r->size, ctx->want_count);
	if (ret == -1)
		return -1;

	if (ret == 0) {
		if (!ctx->input->eof) {
			i_assert(!ctx->input->blocking);
			return 0;
		}
	}

	ctx->want_count = 1;
	return 1;
}

static struct message_part *
message_part_append(pool_t pool, struct message_part *parent)
{
	struct message_part *part, **list;

	part = p_new(pool, struct message_part, 1);
	part->parent = parent;

	/* set child position */
	part->physical_pos =
		parent->physical_pos +
		parent->body_size.physical_size +
		parent->header_size.physical_size;

	list = &part->parent->children;
	while (*list != NULL)
		list = &(*list)->next;

	*list = part;
	return part;
}

static void parse_next_body_multipart_init(struct message_parser_ctx *ctx)
{
	struct message_boundary *b;

	b = p_new(ctx->parser_pool, struct message_boundary, 1);
	b->part = ctx->part;
	b->boundary = ctx->last_boundary;
	b->len = strlen(b->boundary);

	b->next = ctx->boundaries;
	ctx->boundaries = b;

	ctx->last_boundary = NULL;
}

static void parse_next_body_message_rfc822_init(struct message_parser_ctx *ctx)
{
	ctx->part = message_part_append(ctx->part_pool, ctx->part);
}

static int
boundary_line_find(struct message_parser_ctx *ctx,
		   const unsigned char *data, size_t size, bool full,
		   struct message_boundary **boundary_r)
{
	size_t i;

	*boundary_r = NULL;

	if (size < 2) {
		i_assert(!full);

		if (ctx->input->eof)
			return -1;
		ctx->want_count = 2;
		return 0;
	}

	if (data[0] != '-' || data[1] != '-') {
		/* not a boundary, just skip this line */
		return -1;
	}

	/* need to find the end of line */
	for (i = 2; i < size; i++) {
		if (data[i] == '\n')
			break;
	}
	if (i == size && i < BOUNDARY_END_MAX_LEN &&
	    !ctx->input->eof && !full) {
		/* no LF found */
		ctx->want_count = BOUNDARY_END_MAX_LEN;
		return 0;
	}

	data += 2;
	size -= 2;

	*boundary_r = boundary_find(ctx->boundaries, data, size);
	if (*boundary_r == NULL)
		return -1;

	(*boundary_r)->epilogue_found =
		size >= (*boundary_r)->len + 2 &&
		memcmp(data + (*boundary_r)->len, "--", 2) == 0;
	return 1;
}

static int parse_next_body_skip_boundary_line(struct message_parser_ctx *ctx,
					      struct message_block *block_r)
{
	size_t i;
	int ret;

	if ((ret = message_parser_read_more(ctx, block_r)) <= 0)
		return ret;

	for (i = 0; i < block_r->size; i++) {
		if (block_r->data[i] == '\n')
			break;
	}

	if (i == block_r->size) {
		parse_body_add_block(ctx, block_r);
		return 1;
	}

	/* found the LF */
	block_r->size = i + 1;
	parse_body_add_block(ctx, block_r);

	/* a new MIME part begins */
	ctx->part = message_part_append(ctx->part_pool, ctx->part);
	ctx->part->flags |= MESSAGE_PART_FLAG_IS_MIME;

	ctx->parse_next_block = parse_next_header_init;
	return parse_next_header_init(ctx, block_r);
}

static int parse_part_finish(struct message_parser_ctx *ctx,
			     struct message_boundary *boundary,
			     struct message_block *block_r, bool first_line)
{
	struct message_part *part;

	if (boundary == NULL) {
		/* message ended unexpectedly */
		return -1;
	}

	/* get back to parent MIME part, summing the child MIME part sizes
	   into parent's body sizes */
	for (part = ctx->part; part != boundary->part; part = part->parent) {
		message_size_add(&part->parent->body_size, &part->body_size);
		message_size_add(&part->parent->body_size, &part->header_size);
	}
	ctx->part = part;

	if (boundary->epilogue_found) {
		/* this boundary isn't needed anymore */
		ctx->boundaries = boundary->next;

		if (ctx->boundaries != NULL)
			ctx->parse_next_block = parse_next_body_to_boundary;
		else
			ctx->parse_next_block = parse_next_body_to_eof;
		return ctx->parse_next_block(ctx, block_r);
	}

	/* forget about the boundaries we possibly skipped */
	ctx->boundaries = boundary;

	/* the boundary itself should already be in buffer. add that. */
	block_r->data = i_stream_get_data(ctx->input, &block_r->size);
	i_assert(block_r->size >= ctx->skip + 2 + boundary->len +
		 (first_line ? 0 : 1));
	block_r->data += ctx->skip;
	/* [\n]--<boundary> */
	block_r->size = (first_line ? 0 : 1) + 2 + boundary->len;
	parse_body_add_block(ctx, block_r);

	ctx->parse_next_block = parse_next_body_skip_boundary_line;
	return 1;
}

static int parse_next_body_to_boundary(struct message_parser_ctx *ctx,
				       struct message_block *block_r)
{
	struct message_boundary *boundary = NULL;
	const unsigned char *data;
	size_t i, boundary_start;
	int ret;
	bool eof, full;

	if ((ret = message_parser_read_more(ctx, block_r)) == 0 ||
	    block_r->size == 0)
		return ret;
	eof = ret == -1;
	full = ret == -2;

	data = block_r->data;
	if (ctx->last_chr == '\n') {
		/* handle boundary in first line of message. alternatively
		   it's an empty line. */
		ret = boundary_line_find(ctx, block_r->data,
					 block_r->size, full, &boundary);
		if (ret >= 0) {
			if (ret == 0)
				return 0;

			return parse_part_finish(ctx, boundary, block_r, TRUE);
		}
	}

	for (i = boundary_start = 0; i < block_r->size; i++) {
		/* skip to beginning of the next line. the first line was
		   handled already. */
		size_t next_line_idx = block_r->size;

		for (; i < block_r->size; i++) {
			if (data[i] == '\n') {
				boundary_start = i;
				if (i > 0 && data[i-1] == '\r')
					boundary_start--;
				next_line_idx = i + 1;
				break;
			}
		}
		if (boundary_start != 0) {
			/* we can skip the first lines. input buffer can't be
			   full anymore. */
			full = FALSE;
		}

		ret = boundary_line_find(ctx, block_r->data + next_line_idx,
					 block_r->size - next_line_idx, full,
					 &boundary);
		if (ret >= 0) {
			/* found / need more data */
			if (ret == 0 && boundary_start == 0)
				ctx->want_count += next_line_idx;
			break;
		}
	}

	if (i >= block_r->size) {
		/* the boundary wasn't found from this data block,
		   we'll need more data. */
		if (eof)
			ret = -1;
		else {
			ret = 0;
			ctx->want_count = (block_r->size - boundary_start) + 1;
		}
	}
	i_assert(!(ret == 0 && full));

	if (ret >= 0) {
		/* leave CR+LF + last line to buffer */
		block_r->size = boundary_start;
	}
	if (block_r->size != 0)
		parse_body_add_block(ctx, block_r);
	return ret <= 0 ? ret :
		parse_part_finish(ctx, boundary, block_r, FALSE);
}

static int parse_next_body_to_eof(struct message_parser_ctx *ctx,
				  struct message_block *block_r)
{
	int ret;

	if ((ret = message_parser_read_more(ctx, block_r)) <= 0)
		return ret;

	parse_body_add_block(ctx, block_r);
	return 1;
}

static void parse_content_type(struct message_parser_ctx *ctx,
			       struct message_header_line *hdr)
{
	struct rfc822_parser_context parser;
	const char *key, *value;
	string_t *content_type;

	if (ctx->part_seen_content_type)
		return;
	ctx->part_seen_content_type = TRUE;

	rfc822_parser_init(&parser, hdr->full_value, hdr->full_value_len, NULL);
	(void)rfc822_skip_lwsp(&parser);

	content_type = t_str_new(64);
	if (rfc822_parse_content_type(&parser, content_type) < 0)
		return;

	if (strcasecmp(str_c(content_type), "message/rfc822") == 0)
		ctx->part->flags |= MESSAGE_PART_FLAG_MESSAGE_RFC822;
	else if (strncasecmp(str_c(content_type), "text", 4) == 0 &&
		 (str_len(content_type) == 4 ||
		  str_data(content_type)[4] == '/'))
		ctx->part->flags |= MESSAGE_PART_FLAG_TEXT;
	else if (strncasecmp(str_c(content_type), "multipart/", 10) == 0) {
		ctx->part->flags |= MESSAGE_PART_FLAG_MULTIPART;

		if (strcasecmp(str_c(content_type)+10, "digest") == 0)
			ctx->part->flags |= MESSAGE_PART_FLAG_MULTIPART_DIGEST;
	}

	if ((ctx->part->flags & MESSAGE_PART_FLAG_MULTIPART) == 0 ||
	    ctx->last_boundary != NULL)
		return;

	while (rfc822_parse_content_param(&parser, &key, &value) > 0) {
		if (strcasecmp(key, "boundary") == 0) {
			ctx->last_boundary = p_strdup(ctx->parser_pool, value);
			break;
		}
	}
}

#define MUTEX_FLAGS \
	(MESSAGE_PART_FLAG_MESSAGE_RFC822 | MESSAGE_PART_FLAG_MULTIPART)

static int parse_next_header(struct message_parser_ctx *ctx,
			     struct message_block *block_r)
{
	struct message_part *part = ctx->part;
	struct message_header_line *hdr;
	int ret;

	if (ctx->skip > 0) {
		i_stream_skip(ctx->input, ctx->skip);
		ctx->skip = 0;
	}

	ret = message_parse_header_next(ctx->hdr_parser_ctx, &hdr);
	if (ret == 0 || (ret < 0 && ctx->input->stream_errno != 0))
		return ret;

	if (hdr != NULL) {
		if (hdr->eoh)
			;
		else if (strcasecmp(hdr->name, "Mime-Version") == 0) {
			/* it's MIME. Content-* headers are valid */
			part->flags |= MESSAGE_PART_FLAG_IS_MIME;
		} else if (strcasecmp(hdr->name, "Content-Type") == 0) {
			if ((ctx->flags &
			     MESSAGE_PARSER_FLAG_MIME_VERSION_STRICT) == 0)
				part->flags |= MESSAGE_PART_FLAG_IS_MIME;

			if (hdr->continues)
				hdr->use_full_value = TRUE;
			else {
				t_push();
				parse_content_type(ctx, hdr);
				t_pop();
			}
		}

		block_r->hdr = hdr;
		block_r->size = 0;
		return 1;
	}

	/* end of headers */
	if ((part->flags & MESSAGE_PART_FLAG_MULTIPART) != 0 &&
	    ctx->last_boundary == NULL) {
		/* multipart type but no message boundary */
		part->flags = 0;
	}
	if ((part->flags & MESSAGE_PART_FLAG_IS_MIME) == 0) {
		/* It's not MIME. Reset everything we found from
		   Content-Type. */
		part->flags = 0;
		ctx->last_boundary = NULL;
	}

	if (!ctx->part_seen_content_type ||
	    (part->flags & MESSAGE_PART_FLAG_IS_MIME) == 0) {
		if (part->parent != NULL &&
		    (part->parent->flags &
		     MESSAGE_PART_FLAG_MULTIPART_DIGEST) != 0) {
			/* when there's no content-type specified and we're
			   below multipart/digest, assume message/rfc822
			   content-type */
			part->flags |= MESSAGE_PART_FLAG_MESSAGE_RFC822;
		} else {
			/* otherwise we default to text/plain */
			part->flags |= MESSAGE_PART_FLAG_TEXT;
		}
	}

	if (message_parse_header_has_nuls(ctx->hdr_parser_ctx))
		part->flags |= MESSAGE_PART_FLAG_HAS_NULS;
	message_parse_header_deinit(&ctx->hdr_parser_ctx);

	i_assert((part->flags & MUTEX_FLAGS) != MUTEX_FLAGS);

	ctx->last_chr = '\n';
	if (ctx->last_boundary != NULL) {
		parse_next_body_multipart_init(ctx);
		ctx->parse_next_block = parse_next_body_to_boundary;
	} else if (part->flags & MESSAGE_PART_FLAG_MESSAGE_RFC822) {
		parse_next_body_message_rfc822_init(ctx);
		ctx->parse_next_block = parse_next_header_init;
	} else if (ctx->boundaries != NULL)
		ctx->parse_next_block = parse_next_body_to_boundary;
	else
		ctx->parse_next_block = parse_next_body_to_eof;

	ctx->want_count = 1;

	/* return empty block as end of headers */
	block_r->hdr = NULL;
	block_r->size = 0;
	return 1;
}

static int parse_next_header_init(struct message_parser_ctx *ctx,
				  struct message_block *block_r)
{
	i_assert(ctx->hdr_parser_ctx == NULL);

	ctx->hdr_parser_ctx =
		message_parse_header_init(ctx->input, &ctx->part->header_size,
					  ctx->hdr_flags);
	ctx->part_seen_content_type = FALSE;

	ctx->parse_next_block = parse_next_header;
	return parse_next_header(ctx, block_r);
}

static int preparsed_parse_eof(struct message_parser_ctx *ctx __attr_unused__,
			       struct message_block *block_r __attr_unused__)
{
	return -1;
}

static void preparsed_skip_to_next(struct message_parser_ctx *ctx)
{
	ctx->parse_next_block = preparsed_parse_next_header_init;
	while (ctx->part != NULL) {
		if (ctx->part->next != NULL) {
			ctx->part = ctx->part->next;
			break;
		}
		ctx->part = ctx->part->parent;
	}
	if (ctx->part == NULL)
		ctx->parse_next_block = preparsed_parse_eof;
}

static int preparsed_parse_body_finish(struct message_parser_ctx *ctx,
				       struct message_block *block_r)
{
	i_stream_skip(ctx->input, ctx->skip);
	ctx->skip = 0;

	preparsed_skip_to_next(ctx);
	return ctx->parse_next_block(ctx, block_r);
}

static int preparsed_parse_body_more(struct message_parser_ctx *ctx,
				     struct message_block *block_r)
{
	uoff_t end_offset = ctx->part->physical_pos +
		ctx->part->header_size.physical_size +
		ctx->part->body_size.physical_size;
	int ret;

	ret = message_parser_read_more(ctx, block_r);
	if (ret <= 0)
		return ret;

	if (ctx->input->v_offset + block_r->size >= end_offset) {
		block_r->size = end_offset - ctx->input->v_offset;
		ctx->parse_next_block = preparsed_parse_body_finish;
	}
	ctx->skip = block_r->size;
	return 1;
}

static int preparsed_parse_body_init(struct message_parser_ctx *ctx,
				     struct message_block *block_r)
{
	uoff_t offset = ctx->part->physical_pos +
		ctx->part->header_size.physical_size;

	i_assert(offset >= ctx->input->v_offset);
	i_stream_skip(ctx->input, offset - ctx->input->v_offset);

	ctx->parse_next_block = preparsed_parse_body_more;
	return preparsed_parse_body_more(ctx, block_r);
}

static int preparsed_parse_finish_header(struct message_parser_ctx *ctx,
					 struct message_block *block_r)
{
	if (ctx->part->children != NULL) {
		ctx->parse_next_block = preparsed_parse_next_header_init;
		ctx->part = ctx->part->children;
	} else if ((ctx->flags & MESSAGE_PARSER_FLAG_SKIP_BODY_BLOCK) == 0) {
		ctx->parse_next_block = preparsed_parse_body_init;
	} else {
		preparsed_skip_to_next(ctx);
	}
	return ctx->parse_next_block(ctx, block_r);
}

static int preparsed_parse_next_header(struct message_parser_ctx *ctx,
				       struct message_block *block_r)
{
	struct message_header_line *hdr;
	int ret;

	ret = message_parse_header_next(ctx->hdr_parser_ctx, &hdr);
	if (ret == 0 || (ret < 0 && ctx->input->stream_errno != 0))
		return ret;

	if (hdr != NULL) {
		block_r->hdr = hdr;
		block_r->size = 0;
		return 1;
	}
	message_parse_header_deinit(&ctx->hdr_parser_ctx);

	ctx->parse_next_block = preparsed_parse_finish_header;

	/* return empty block as end of headers */
	block_r->hdr = NULL;
	block_r->size = 0;
	return 1;
}

static int preparsed_parse_next_header_init(struct message_parser_ctx *ctx,
					    struct message_block *block_r)
{
	i_assert(ctx->hdr_parser_ctx == NULL);

	i_assert(ctx->part->physical_pos >= ctx->input->v_offset);
	i_stream_skip(ctx->input, ctx->part->physical_pos -
		      ctx->input->v_offset);

	ctx->hdr_parser_ctx =
		message_parse_header_init(ctx->input, NULL, ctx->hdr_flags);

	ctx->parse_next_block = preparsed_parse_next_header;
	return preparsed_parse_next_header(ctx, block_r);
}

struct message_parser_ctx *
message_parser_init(pool_t part_pool, struct istream *input,
		    enum message_header_parser_flags hdr_flags,
		    enum message_parser_flags flags)
{
	struct message_parser_ctx *ctx;
	pool_t pool;

	pool = pool_alloconly_create("Message Parser", 1024);
	ctx = p_new(pool, struct message_parser_ctx, 1);
	ctx->parser_pool = pool;
	ctx->part_pool = part_pool;
	ctx->hdr_flags = hdr_flags;
	ctx->flags = flags;
	ctx->input = input;
	ctx->parts = ctx->part = part_pool == NULL ? NULL :
		p_new(part_pool, struct message_part, 1);
	ctx->parse_next_block = parse_next_header_init;
	i_stream_ref(input);
	return ctx;
}

struct message_parser_ctx *
message_parser_init_from_parts(struct message_part *parts,
			       struct istream *input,
			       enum message_header_parser_flags hdr_flags,
			       enum message_parser_flags flags)
{
	struct message_parser_ctx *ctx;

	ctx = message_parser_init(NULL, input, hdr_flags, flags);
	ctx->parts = ctx->part = parts;
	ctx->parse_next_block = preparsed_parse_next_header_init;
	return ctx;
}

struct message_part *message_parser_deinit(struct message_parser_ctx **_ctx)
{
        struct message_parser_ctx *ctx = *_ctx;
	struct message_part *parts = ctx->parts;

	*_ctx = NULL;
	i_stream_unref(&ctx->input);
	pool_unref(ctx->parser_pool);
	return parts;
}

int message_parser_parse_next_block(struct message_parser_ctx *ctx,
				    struct message_block *block_r)
{
	int ret;
	bool eof = FALSE;

	while ((ret = ctx->parse_next_block(ctx, block_r)) == 0) {
		ret = message_parser_read_more(ctx, block_r);
		if (ret <= 0) {
			i_assert(ret != -2);

			if (ret == 0) {
				i_assert(!ctx->input->blocking);
				return 0;
			}
			if (ret < 0) {
				i_assert(!eof);
				eof = TRUE;
			}
		}
	}

	block_r->part = ctx->part;

	if (ret < 0 && ctx->part != NULL) {
		i_assert(ctx->input->eof);
		while (ctx->part->parent != NULL) {
			message_size_add(&ctx->part->parent->body_size,
					 &ctx->part->body_size);
			message_size_add(&ctx->part->parent->body_size,
					 &ctx->part->header_size);
			ctx->part = ctx->part->parent;
		}
	}

	return ret;
}

#undef message_parser_parse_header
void message_parser_parse_header(struct message_parser_ctx *ctx,
				 struct message_size *hdr_size,
				 message_part_header_callback_t *callback,
				 void *context)
{
	struct message_block block;
	int ret;

	while ((ret = message_parser_parse_next_block(ctx, &block)) > 0) {
		callback(block.part, block.hdr, context);

		if (block.hdr == NULL)
			break;
	}
	i_assert(ret != 0);

	if (ret < 0) {
		/* well, can't return error so fake end of headers */
		callback(ctx->part, NULL, context);
	}

        *hdr_size = ctx->part->header_size;
}

#undef message_parser_parse_body
void message_parser_parse_body(struct message_parser_ctx *ctx,
			       message_part_header_callback_t *hdr_callback,
			       void *context)
{
	struct message_block block;
	int ret;

	while ((ret = message_parser_parse_next_block(ctx, &block)) > 0) {
		if (block.size == 0 && hdr_callback != NULL)
			hdr_callback(block.part, block.hdr, context);
	}
	i_assert(ret != 0);
}

static void
message_parser_set_crlfs_diff(struct message_part *parts, bool use_crlf,
			      off_t diff)
{
	while (parts != NULL) {
		parts->physical_pos += diff;

		if (use_crlf) {
			parts->header_size.physical_size =
				parts->header_size.virtual_size;
			parts->body_size.physical_size =
				parts->body_size.virtual_size;
		} else {
			parts->header_size.physical_size =
				parts->header_size.virtual_size -
				parts->header_size.lines;
			parts->body_size.physical_size =
				parts->body_size.virtual_size -
				parts->body_size.lines;

			diff -= parts->header_size.lines;
		}

		if (parts->children != NULL) {
			message_parser_set_crlfs_diff(parts->children,
						      use_crlf, diff);
		}

		if (!use_crlf)
			diff -= parts->body_size.lines;

		parts = parts->next;
	}
}

void message_parser_set_crlfs(struct message_part *parts, bool use_crlf)
{
	message_parser_set_crlfs_diff(parts, use_crlf, 0);
}
