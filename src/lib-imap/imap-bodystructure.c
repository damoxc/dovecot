/* Copyright (C) 2002 Timo Sirainen */

#include "lib.h"
#include "buffer.h"
#include "istream.h"
#include "str.h"
#include "message-parser.h"
#include "rfc822-parser.h"
#include "imap-parser.h"
#include "imap-quote.h"
#include "imap-envelope.h"
#include "imap-bodystructure.h"

#define DEFAULT_CHARSET \
	"\"charset\" \"us-ascii\""

#define EMPTY_BODYSTRUCTURE \
        "(\"text\" \"plain\" ("DEFAULT_CHARSET") NIL NIL \"7bit\" 0 0)"

struct message_part_body_data {
	pool_t pool;
	const char *content_type, *content_subtype;
	const char *content_type_params;
	const char *content_transfer_encoding;
	const char *content_id;
	const char *content_description;
	const char *content_disposition;
	const char *content_disposition_params;
	const char *content_md5;
	const char *content_language;

	struct message_part_envelope_data *envelope;
};

static void parse_content_type(struct message_part_body_data *data,
			       struct message_header_line *hdr)
{
	struct rfc822_parser_context parser;
	const char *key, *value;
	string_t *str;
	unsigned int i;
	bool charset_found = FALSE;

	rfc822_parser_init(&parser, hdr->full_value, hdr->full_value_len, NULL);
	(void)rfc822_skip_lwsp(&parser);

	str = t_str_new(256);
	if (rfc822_parse_content_type(&parser, str) < 0)
		return;

	/* Save content type and subtype */
	value = str_c(str);
	for (i = 0; value[i] != '\0'; i++) {
		if (value[i] == '/') {
			data->content_subtype =
				imap_quote(data->pool, str_data(str) + i + 1,
					   str_len(str) - (i + 1));
			break;
		}
	}
	data->content_type =
		imap_quote(data->pool, str_data(str), i);

	/* parse parameters and save them */
	str_truncate(str, 0);
	while (rfc822_parse_content_param(&parser, &key, &value) > 0) {
		if (strcasecmp(key, "charset") == 0)
			charset_found = TRUE;

		str_append_c(str, ' ');
		imap_quote_append_string(str, key, TRUE);
		str_append_c(str, ' ');
		imap_quote_append_string(str, value, TRUE);
	}

	if (!charset_found &&
	    strcasecmp(data->content_type, "\"text\"") == 0) {
		/* set a default charset */
		str_append_c(str, ' ');
		str_append(str, DEFAULT_CHARSET);
	}
	if (str_len(str) > 0) {
		data->content_type_params =
			p_strdup(data->pool, str_c(str) + 1);
	}
}

static void parse_content_transfer_encoding(struct message_part_body_data *data,
					    struct message_header_line *hdr)
{
	struct rfc822_parser_context parser;
	string_t *str;

	rfc822_parser_init(&parser, hdr->full_value, hdr->full_value_len, NULL);
	(void)rfc822_skip_lwsp(&parser);

	t_push();
	str = t_str_new(256);
	if (rfc822_parse_mime_token(&parser, str) >= 0) {
		data->content_transfer_encoding =
			imap_quote(data->pool, str_data(str), str_len(str));
	}
	t_pop();
}

static void parse_content_disposition(struct message_part_body_data *data,
				      struct message_header_line *hdr)
{
	struct rfc822_parser_context parser;
	const char *key, *value;
	string_t *str;

	rfc822_parser_init(&parser, hdr->full_value, hdr->full_value_len, NULL);
	(void)rfc822_skip_lwsp(&parser);

	t_push();
	str = t_str_new(256);
	if (rfc822_parse_mime_token(&parser, str) < 0) {
		t_pop();
		return;
	}
	data->content_disposition =
		imap_quote(data->pool, str_data(str), str_len(str));

	/* parse parameters and save them */
	str_truncate(str, 0);
	while (rfc822_parse_content_param(&parser, &key, &value) > 0) {
		str_append_c(str, ' ');
		imap_quote_append_string(str, key, TRUE);
		str_append_c(str, ' ');
		imap_quote_append_string(str, value, TRUE);
	}
	if (str_len(str) > 0) {
		data->content_disposition_params =
			p_strdup(data->pool, str_c(str) + 1);
	}
	t_pop();
}

static void parse_content_language(const unsigned char *value, size_t value_len,
				   struct message_part_body_data *data)
{
	struct rfc822_parser_context parser;
	string_t *str;

	/* Language-Header = "Content-Language" ":" 1#Language-tag
	   Language-Tag = Primary-tag *( "-" Subtag )
	   Primary-tag = 1*8ALPHA
	   Subtag = 1*8ALPHA */

	rfc822_parser_init(&parser, value, value_len, NULL);

	t_push();
	str = t_str_new(128);
	str_append_c(str, '"');

	(void)rfc822_skip_lwsp(&parser);
	while (rfc822_parse_atom(&parser, str) >= 0) {
		str_append(str, "\" \"");

		if (parser.data == parser.end || *parser.data != ',')
			break;
		parser.data++;
		(void)rfc822_skip_lwsp(&parser);
	}

	if (str_len(str) > 1) {
		str_truncate(str, str_len(str) - 2);
		data->content_language = p_strdup(data->pool, str_c(str));
	}

	t_pop();
}

static void parse_content_header(struct message_part_body_data *d,
				 struct message_header_line *hdr,
				 pool_t pool)
{
	const char *name = hdr->name;
	const unsigned char *value;
	size_t value_len;

	if (strncasecmp(name, "Content-", 8) != 0)
		return;
	name += 8;

	if (hdr->continues) {
		hdr->use_full_value = TRUE;
		return;
	}

	value = hdr->full_value;
	value_len = hdr->full_value_len;

	switch (*name) {
	case 'i':
	case 'I':
		if (strcasecmp(name, "ID") == 0 && d->content_id == NULL)
			d->content_id = imap_quote(pool, value, value_len);
		break;

	case 'm':
	case 'M':
		if (strcasecmp(name, "MD5") == 0 && d->content_md5 == NULL)
			d->content_md5 = imap_quote(pool, value, value_len);
		break;

	case 't':
	case 'T':
		if (strcasecmp(name, "Type") == 0 && d->content_type == NULL) {
			t_push();
			parse_content_type(d, hdr);
			t_pop();
		}
		if (strcasecmp(name, "Transfer-Encoding") == 0 &&
		    d->content_transfer_encoding == NULL)
			parse_content_transfer_encoding(d, hdr);
		break;

	case 'l':
	case 'L':
		if (strcasecmp(name, "Language") == 0 &&
		    d->content_language == NULL)
			parse_content_language(value, value_len, d);
		break;

	case 'd':
	case 'D':
		if (strcasecmp(name, "Description") == 0 &&
		    d->content_description == NULL) {
			d->content_description =
				imap_quote(pool, value, value_len);
		}
		if (strcasecmp(name, "Disposition") == 0 &&
		    d->content_disposition_params == NULL)
			parse_content_disposition(d, hdr);
		break;
	}
}

void imap_bodystructure_parse_header(pool_t pool, struct message_part *part,
				     struct message_header_line *hdr)
{
	struct message_part_body_data *part_data;
	struct message_part_envelope_data *envelope;
	bool parent_rfc822;

	if (hdr == NULL) {
		/* If there was no Mime-Version, forget all the Content-stuff */
		if ((part->flags & MESSAGE_PART_FLAG_IS_MIME) == 0 &&
		    part->context != NULL) {
			part_data = part->context;
			envelope = part_data->envelope;

			memset(part_data, 0, sizeof(*part_data));
			part_data->pool = pool;
			part_data->envelope = envelope;
		}
		return;
	}

	if (hdr->eoh)
		return;

	parent_rfc822 = part->parent != NULL &&
		(part->parent->flags & MESSAGE_PART_FLAG_MESSAGE_RFC822) != 0;
	if (!parent_rfc822 && strncasecmp(hdr->name, "Content-", 8) != 0)
		return;

	if (part->context == NULL) {
		/* initialize message part data */
		part->context = part_data =
			p_new(pool, struct message_part_body_data, 1);
		part_data->pool = pool;
	}
	part_data = part->context;

	t_push();

	parse_content_header(part_data, hdr, pool);

	if (parent_rfc822) {
		/* message/rfc822, we need the envelope */
		imap_envelope_parse_header(pool, &part_data->envelope, hdr);
	}
	t_pop();
}

static void part_write_body_multipart(const struct message_part *part,
				      string_t *str, bool extended)
{
	struct message_part_body_data *data = part->context;

	if (data == NULL) {
		/* there was no content headers, use an empty structure */
		data = t_new(struct message_part_body_data, 1);
	}

	if (part->children != NULL)
		imap_bodystructure_write(part->children, str, extended);
	else {
		/* no parts in multipart message,
		   that's not allowed. write a single
		   0-length text/plain structure */
		str_append(str, EMPTY_BODYSTRUCTURE);
	}

	str_append_c(str, ' ');
	if (data->content_subtype != NULL)
		str_append(str, data->content_subtype);
	else
		str_append(str, "\"x-unknown\"");

	if (!extended)
		return;

	/* BODYSTRUCTURE data */
	str_append_c(str, ' ');
	if (data->content_type_params == NULL)
		str_append(str, "NIL");
	else {
		str_append_c(str, '(');
		str_append(str, data->content_type_params);
		str_append_c(str, ')');
	}

	str_append_c(str, ' ');
	if (data->content_disposition == NULL)
		str_append(str, "NIL");
	else {
		str_append_c(str, '(');
		str_append(str, data->content_disposition);
		str_append_c(str, ' ');

		if (data->content_disposition_params == NULL)
			str_append(str, "NIL");
		else {
			str_append_c(str, '(');
			str_append(str, data->content_disposition_params);
			str_append_c(str, ')');
		}
		str_append_c(str, ')');
	}

	str_append_c(str, ' ');
	if (data->content_language == NULL)
		str_append(str, "NIL");
	else {
		str_append_c(str, '(');
		str_append(str, data->content_language);
		str_append_c(str, ')');
	}
}

static void part_write_body(const struct message_part *part,
			    string_t *str, bool extended)
{
	struct message_part_body_data *data = part->context;
	bool text;

	if (data == NULL) {
		/* there was no content headers, use an empty structure */
		data = t_new(struct message_part_body_data, 1);
	}

	if (part->flags & MESSAGE_PART_FLAG_MESSAGE_RFC822) {
		str_append(str, "\"message\" \"rfc822\"");
		text = FALSE;
	} else {
		/* "content type" "subtype" */
		text = data->content_type == NULL ||
			strcasecmp(data->content_type, "\"text\"") == 0;
		str_append(str, NVL(data->content_type, "\"text\""));
		str_append_c(str, ' ');

		if (data->content_subtype != NULL)
			str_append(str, data->content_subtype);
		else {
			if (text)
				str_append(str, "\"plain\"");
			else
				str_append(str, "\"unknown\"");

		}
	}

	/* ("content type param key" "value" ...) */
	str_append_c(str, ' ');
	if (data->content_type_params == NULL) {
		if (!text)
			str_append(str, "NIL");
		else
			str_append(str, "("DEFAULT_CHARSET")");
	} else {
		str_append_c(str, '(');
		str_append(str, data->content_type_params);
		str_append_c(str, ')');
	}

	str_printfa(str, " %s %s %s %"PRIuUOFF_T,
		    NVL(data->content_id, "NIL"),
		    NVL(data->content_description, "NIL"),
		    NVL(data->content_transfer_encoding, "\"7bit\""),
		    part->body_size.virtual_size);

	if (text) {
		/* text/.. contains line count */
		str_printfa(str, " %u", part->body_size.lines);
	} else if (part->flags & MESSAGE_PART_FLAG_MESSAGE_RFC822) {
		/* message/rfc822 contains envelope + body + line count */
		struct message_part_body_data *child_data;
                struct message_part_envelope_data *env_data;

		i_assert(part->children != NULL);
		i_assert(part->children->next == NULL);

                child_data = part->children->context;
		env_data = child_data != NULL ? child_data->envelope : NULL;

		str_append(str, " (");
		imap_envelope_write_part_data(env_data, str);
		str_append(str, ") ");

		imap_bodystructure_write(part->children, str, extended);
		str_printfa(str, " %u", part->body_size.lines);
	}

	if (!extended)
		return;

	/* BODYSTRUCTURE data */

	/* "md5" ("content disposition" ("disposition" "params"))
	   ("body" "language" "params") */
	str_append_c(str, ' ');
	str_append(str, NVL(data->content_md5, "NIL"));

	str_append_c(str, ' ');
	if (data->content_disposition == NULL)
		str_append(str, "NIL");
	else {
		str_append_c(str, '(');
		str_append(str, data->content_disposition);
		str_append_c(str, ' ');

		if (data->content_disposition_params == NULL)
			str_append(str, "NIL");
		else {
			str_append_c(str, '(');
			str_append(str, data->content_disposition_params);
			str_append_c(str, ')');
		}

		str_append_c(str, ')');
	}

	str_append_c(str, ' ');
	if (data->content_language == NULL)
		str_append(str, "NIL");
	else {
		str_append_c(str, '(');
		str_append(str, data->content_language);
		str_append_c(str, ')');
	}
}

bool imap_bodystructure_is_plain_7bit(struct message_part *part)
{
	struct message_part_body_data *data = part->context;

	i_assert(part->parent == NULL);

	if (data == NULL) {
		/* no bodystructure headers found */
		return TRUE;
	}

	/* if content-type is text/xxx we don't have to check any
	   multipart stuff */
	if ((part->flags & MESSAGE_PART_FLAG_TEXT) == 0)
		return FALSE;
	if (part->next != NULL || part->children != NULL)
		return FALSE; /* shouldn't happen normally.. */

	/* must be text/plain */
	if (data->content_subtype != NULL &&
	    strcasecmp(data->content_subtype, "\"plain\"") != 0)
		return FALSE;

	/* only allowed parameter is charset=us-ascii, which is also default */
	if (data->content_type_params != NULL &&
	    strcasecmp(data->content_type_params, DEFAULT_CHARSET) != 0)
		return FALSE;

	if (data->content_id != NULL ||
	    data->content_description != NULL)
		return FALSE;

	if (data->content_transfer_encoding != NULL &&
	    strcasecmp(data->content_transfer_encoding, "\"7bit\"") != 0)
		return FALSE;

	/* BODYSTRUCTURE checks: */
	if (data->content_md5 != NULL ||
	    data->content_disposition != NULL ||
	    data->content_language != NULL)
		return FALSE;

	return TRUE;
}

void imap_bodystructure_write(const struct message_part *part,
			      string_t *dest, bool extended)
{
	i_assert(part->parent != NULL || part->next == NULL);

	while (part != NULL) {
		if (part->parent != NULL)
			str_append_c(dest, '(');

		if (part->flags & MESSAGE_PART_FLAG_MULTIPART)
			part_write_body_multipart(part, dest, extended);
		else
			part_write_body(part, dest, extended);

		if (part->parent != NULL)
			str_append_c(dest, ')');

		part = part->next;
	}
}

static bool str_append_imap_arg(string_t *str, const struct imap_arg *arg)
{
	switch (arg->type) {
	case IMAP_ARG_NIL:
		str_append(str, "NIL");
		break;
	case IMAP_ARG_ATOM:
		str_append(str, IMAP_ARG_STR(arg));
		break;
	case IMAP_ARG_STRING:
		str_append_c(str, '"');
		str_append(str, IMAP_ARG_STR(arg));
		str_append_c(str, '"');
		break;
	case IMAP_ARG_LITERAL: {
		const char *argstr = IMAP_ARG_STR(arg);

		str_printfa(str, "{%"PRIuSIZE_T"}\r\n", strlen(argstr));
		str_append(str, argstr);
		break;
	}
	default:
		return FALSE;
	}

	return TRUE;
}

static bool imap_write_list(const struct imap_arg *args, string_t *str)
{
	/* don't do any typechecking, just write it out */
	str_append_c(str, '(');
	while (args->type != IMAP_ARG_EOL) {
		if (!str_append_imap_arg(str, args)) {
			if (args->type != IMAP_ARG_LIST)
				return FALSE;

			if (!imap_write_list(IMAP_ARG_LIST_ARGS(args), str))
				return FALSE;
		}
		args++;

		if (args->type != IMAP_ARG_EOL)
			str_append_c(str, ' ');
	}
	str_append_c(str, ')');
	return TRUE;
}

static bool imap_parse_bodystructure_args(const struct imap_arg *args,
					  string_t *str)
{
	const struct imap_arg *subargs;
	const struct imap_arg *list_args;
	bool multipart, text, message_rfc822;
	int i;

	multipart = FALSE;
	while (args->type == IMAP_ARG_LIST) {
		str_append_c(str, '(');
		list_args = IMAP_ARG_LIST_ARGS(args);
		if (!imap_parse_bodystructure_args(list_args, str))
			return FALSE;
		str_append_c(str, ')');

		multipart = TRUE;
		args++;
	}

	if (multipart) {
		/* next is subtype of Content-Type. rest is skipped. */
		str_append_c(str, ' ');
		return str_append_imap_arg(str, args);
	}

	/* "content type" "subtype" */
	if (args[0].type == IMAP_ARG_NIL || args[1].type == IMAP_ARG_NIL)
		return FALSE;

	if (!str_append_imap_arg(str, &args[0]))
		return FALSE;
	str_append_c(str, ' ');
	if (!str_append_imap_arg(str, &args[1]))
		return FALSE;

	text = strcasecmp(IMAP_ARG_STR_NONULL(&args[0]), "text") == 0;
	message_rfc822 =
		strcasecmp(IMAP_ARG_STR_NONULL(&args[0]), "message") == 0 &&
		strcasecmp(IMAP_ARG_STR_NONULL(&args[1]), "rfc822") == 0;

	args += 2;

	/* ("content type param key" "value" ...) | NIL */
	if (args->type == IMAP_ARG_LIST) {
		str_append(str, " (");
                subargs = IMAP_ARG_LIST_ARGS(args);
		for (; subargs->type != IMAP_ARG_EOL; ) {
			if (!str_append_imap_arg(str, &subargs[0]))
				return FALSE;
			str_append_c(str, ' ');
			if (!str_append_imap_arg(str, &subargs[1]))
				return FALSE;

			subargs += 2;
			if (subargs->type == IMAP_ARG_EOL)
				break;
			str_append_c(str, ' ');
		}
		str_append(str, ")");
	} else if (args->type == IMAP_ARG_NIL) {
		str_append(str, " NIL");
	} else {
		return FALSE;
	}
	args++;

	/* "content id" "content description" "transfer encoding" size */
	for (i = 0; i < 4; i++, args++) {
		str_append_c(str, ' ');

		if (!str_append_imap_arg(str, args))
			return FALSE;
	}

	if (text) {
		/* text/xxx - text lines */
		if (args->type != IMAP_ARG_ATOM)
			return FALSE;

		str_append_c(str, ' ');
		str_append(str, IMAP_ARG_STR(args));
	} else if (message_rfc822) {
		/* message/rfc822 - envelope + bodystructure + text lines */
		if (args[0].type != IMAP_ARG_LIST ||
		    args[1].type != IMAP_ARG_LIST ||
		    args[2].type != IMAP_ARG_ATOM)
			return FALSE;

		str_append_c(str, ' ');

		list_args = IMAP_ARG_LIST_ARGS(&args[0]);
		if (!imap_write_list(list_args, str))
			return FALSE;

		str_append(str, " (");

		list_args = IMAP_ARG_LIST_ARGS(&args[1]);
		if (!imap_parse_bodystructure_args(list_args, str))
			return FALSE;

		str_append(str, ") ");
		str_append(str, IMAP_ARG_STR(&args[2]));
	}

	return TRUE;
}

bool imap_body_parse_from_bodystructure(const char *bodystructure,
					string_t *dest)
{
	struct istream *input;
	struct imap_parser *parser;
	const struct imap_arg *args;
	int ret;

	input = i_stream_create_from_data(bodystructure, strlen(bodystructure));
	(void)i_stream_read(input);

	parser = imap_parser_create(input, NULL, (size_t)-1);
	ret = imap_parser_finish_line(parser, 0, IMAP_PARSE_FLAG_NO_UNESCAPE |
				      IMAP_PARSE_FLAG_LITERAL_TYPE, &args);
	ret = ret > 0 && imap_parse_bodystructure_args(args, dest);

	if (!ret)
		i_error("Error parsing IMAP bodystructure: %s", bodystructure);

	imap_parser_destroy(&parser);
	i_stream_destroy(&input);
	return ret;
}
