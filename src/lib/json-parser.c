/* Copyright (c) 2013 Dovecot authors, see the included COPYING file */

#include "lib.h"
#include "array.h"
#include "str.h"
#include "istream.h"
#include "hex-dec.h"
#include "unichar.h"
#include "istream-jsonstr.h"
#include "json-parser.h"

enum json_state {
	JSON_STATE_ROOT = 0,
	JSON_STATE_OBJECT_OPEN,
	JSON_STATE_OBJECT_KEY,
	JSON_STATE_OBJECT_COLON,
	JSON_STATE_OBJECT_VALUE,
	JSON_STATE_OBJECT_SKIP_STRING,
	JSON_STATE_OBJECT_NEXT,
	JSON_STATE_ARRAY_OPEN,
	JSON_STATE_ARRAY_VALUE,
	JSON_STATE_ARRAY_SKIP_STRING,
	JSON_STATE_ARRAY_NEXT,
	JSON_STATE_DONE
};

struct json_parser {
	struct istream *input;
	uoff_t highwater_offset;

	const unsigned char *start, *end, *data;
	const char *error;
	string_t *value;
	struct istream *strinput;

	enum json_state state;
	ARRAY(enum json_state) nesting;
	unsigned int nested_skip_count;
	bool skipping;
};

static int json_parser_read_more(struct json_parser *parser)
{
	uoff_t cur_highwater = parser->input->v_offset +
		i_stream_get_data_size(parser->input);
	size_t size;
	ssize_t ret;

	i_assert(parser->highwater_offset <= cur_highwater);

	if (parser->error != NULL)
		return -1;

	if (parser->highwater_offset == cur_highwater) {
		ret = i_stream_read(parser->input);
		if (ret == -2) {
			parser->error = "Token too large";
			return -1;
		}
		if (ret <= 0)
			return ret;

		cur_highwater = parser->input->v_offset +
			i_stream_get_data_size(parser->input);
		i_assert(parser->highwater_offset < cur_highwater);
		parser->highwater_offset = cur_highwater;
	}

	parser->start = parser->data = i_stream_get_data(parser->input, &size);
	parser->end = parser->start + size;
	i_assert(size > 0);
	return 1;
}

static void json_parser_update_input_pos(struct json_parser *parser)
{
	size_t size;

	if (parser->data == parser->start)
		return;

	i_stream_skip(parser->input, parser->data - parser->start);
	parser->start = parser->data = i_stream_get_data(parser->input, &size);
	parser->end = parser->start + size;
	if (size > 0) {
		/* we skipped over some data and there's still data left.
		   no need to read() the next time. */
		parser->highwater_offset = 0;
	} else {
		parser->highwater_offset = parser->input->v_offset;
	}
}

struct json_parser *json_parser_init(struct istream *input)
{
	struct json_parser *parser;

	parser = i_new(struct json_parser, 1);
	parser->input = input;
	parser->value = str_new(default_pool, 128);
	i_array_init(&parser->nesting, 8);
	i_stream_ref(input);
	return parser;
}

int json_parser_deinit(struct json_parser **_parser, const char **error_r)
{
	struct json_parser *parser = *_parser;

	*_parser = NULL;

	if (parser->error != NULL) {
		/* actual parser error */
		*error_r = parser->error;
	} else if (parser->input->stream_errno != 0) {
		*error_r = t_strdup_printf("read(%s) failed: %m",
					   i_stream_get_name(parser->input));
	} else if (parser->data == parser->end &&
		   !i_stream_have_bytes_left(parser->input) &&
		   parser->state != JSON_STATE_DONE) {
		*error_r = "Missing '}'";
	} else {
		*error_r = NULL;
	}
	
	i_stream_unref(&parser->input);
	array_free(&parser->nesting);
	str_free(&parser->value);
	i_free(parser);
	return *error_r != NULL ? -1 : 0;
}

static bool json_parse_whitespace(struct json_parser *parser)
{
	for (; parser->data != parser->end; parser->data++) {
		switch (*parser->data) {
		case ' ':
		case '\t':
		case '\r':
		case '\n':
			break;
		default:
			json_parser_update_input_pos(parser);
			return TRUE;
		}
	}
	json_parser_update_input_pos(parser);
	return FALSE;
}

static int json_skip_string(struct json_parser *parser)
{
	for (; parser->data != parser->end; parser->data++) {
		if (*parser->data == '"') {
			parser->data++;
			json_parser_update_input_pos(parser);
			return 1;
		}
		if (*parser->data == '\\') {
			switch (*++parser->data) {
			case '"':
			case '\\':
			case '/':
			case 'b':
			case 'f':
			case 'n':
			case 'r':
			case 't':
				break;
			case 'u':
				if (parser->end - parser->data < 4)
					return -1;
				parser->data += 3;
				break;
			default:
				return -1;
			}
		}
	}
	json_parser_update_input_pos(parser);
	return 0;
}

static int json_parse_string(struct json_parser *parser, bool allow_skip,
			     const char **value_r)
{
	if (*parser->data != '"')
		return -1;
	parser->data++;

	if (parser->skipping && allow_skip) {
		*value_r = NULL;
		return json_skip_string(parser);
	}

	str_truncate(parser->value, 0);
	for (; parser->data != parser->end; parser->data++) {
		if (*parser->data == '"') {
			parser->data++;
			*value_r = str_c(parser->value);
			return 1;
		}
		if (*parser->data != '\\')
			str_append_c(parser->value, *parser->data);
		else {
			switch (*++parser->data) {
			case '"':
			case '\\':
			case '/':
				str_append_c(parser->value, *parser->data);
				break;
			case 'b':
				str_append_c(parser->value, '\b');
				break;
			case 'f':
				str_append_c(parser->value, '\f');
				break;
			case 'n':
				str_append_c(parser->value, '\n');
				break;
			case 'r':
				str_append_c(parser->value, '\r');
				break;
			case 't':
				str_append_c(parser->value, '\t');
				break;
			case 'u':
				parser->data++;
				if (parser->end - parser->data < 4) {
					/* wait for more data */
					parser->data = parser->end;
					return 0;
				}
				uni_ucs4_to_utf8_c(hex2dec(parser->data, 4),
						   parser->value);
				parser->data += 3;
				break;
			default:
				return -1;
			}
		}
	}
	return 0;
}

static int
json_parse_digits(struct json_parser *parser)
{
	if (parser->data == parser->end)
		return 0;
	if (*parser->data < '0' || *parser->data > '9')
		return -1;

	while (parser->data != parser->end &&
	       *parser->data >= '0' && *parser->data <= '9')
		str_append_c(parser->value, *parser->data++);
	return 1;
}

static int json_parse_int(struct json_parser *parser)
{
	int ret;

	if (*parser->data == '-') {
		str_append_c(parser->value, *parser->data++);
		if (parser->data == parser->end)
			return 0;
	}
	if (*parser->data == '0')
		str_append_c(parser->value, *parser->data++);
	else {
		if ((ret = json_parse_digits(parser)) <= 0)
			return ret;
	}
	return 1;
}

static int json_parse_number(struct json_parser *parser, const char **value_r)
{
	int ret;

	str_truncate(parser->value, 0);
	if ((ret = json_parse_int(parser)) <= 0)
		return ret;
	if (parser->data != parser->end && *parser->data == '.') {
		/* frac */
		str_append_c(parser->value, *parser->data++);
		if ((ret = json_parse_digits(parser)) <= 0)
			return ret;
	}
	if (parser->data != parser->end &&
	    (*parser->data == 'e' || *parser->data == 'E')) {
		/* exp */
		str_append_c(parser->value, *parser->data++);
		if (parser->data == parser->end)
			return 0;
		if (*parser->data == '+' || *parser->data == '-')
			str_append_c(parser->value, *parser->data++);
		if ((ret = json_parse_digits(parser)) <= 0)
			return ret;
	}
	if (parser->data == parser->end && !parser->input->eof)
		return 0;
	*value_r = str_c(parser->value);
	return 1;
}

static int json_parse_atom(struct json_parser *parser, const char *atom)
{
	unsigned int avail, len = strlen(atom);

	avail = parser->end - parser->data;
	if (avail < len) {
		if (memcmp(parser->data, atom, avail) != 0)
			return -1;

		/* everything matches so far, but we need more data */
		parser->data += avail;
		return 0;
	}
	if (memcmp(parser->data, atom, len) != 0)
		return -1;
	parser->data += len;
	return 1;
}

static int json_parse_denest(struct json_parser *parser)
{
	const enum json_state *nested_states;
	unsigned count;

	parser->data++;
	json_parser_update_input_pos(parser);

	nested_states = array_get(&parser->nesting, &count);
	if (count == 0) {
		/* closing root */
		parser->state = JSON_STATE_DONE;
		return 0;
	}

	/* closing a nested object */
	if (count == 1) {
		/* we're back to root */
		parser->state = JSON_STATE_OBJECT_NEXT;
	} else {
		/* back to previous nested object */
		parser->state = nested_states[count-2] == JSON_STATE_OBJECT_OPEN ?
			JSON_STATE_OBJECT_NEXT : JSON_STATE_ARRAY_NEXT;
	}
	array_delete(&parser->nesting, count-1, 1);

	if (parser->nested_skip_count > 0) {
		parser->nested_skip_count--;
		return 0;
	}
	return 1;
}

static int
json_parse_close_object(struct json_parser *parser, enum json_type *type_r)
{
	if (json_parse_denest(parser) == 0)
		return 0;
	*type_r = JSON_TYPE_OBJECT_END;
	return 1;
}

static int
json_parse_close_array(struct json_parser *parser, enum json_type *type_r)
{
	if (json_parse_denest(parser) == 0)
		return 0;
	*type_r = JSON_TYPE_ARRAY_END;
	return 1;
}

static int
json_try_parse_next(struct json_parser *parser, enum json_type *type_r,
		    const char **value_r)
{
	bool skipping = parser->skipping;
	int ret;

	if (!json_parse_whitespace(parser))
		return -1;

	switch (parser->state) {
	case JSON_STATE_ROOT:
		if (*parser->data != '{') {
			parser->error = "Object doesn't begin with '{'";
			return -1;
		}
		parser->data++;
		parser->state = JSON_STATE_OBJECT_OPEN;
		json_parser_update_input_pos(parser);
		return 0;
	case JSON_STATE_OBJECT_VALUE:
	case JSON_STATE_ARRAY_VALUE:
		if (*parser->data == '{') {
			parser->data++;
			parser->state = JSON_STATE_OBJECT_OPEN;
			array_append(&parser->nesting, &parser->state, 1);
			json_parser_update_input_pos(parser);

			if (parser->skipping) {
				parser->nested_skip_count++;
				return 0;
			}
			*type_r = JSON_TYPE_OBJECT;
			return 1;
		} else if (*parser->data == '[') {
			parser->data++;
			parser->state = JSON_STATE_ARRAY_OPEN;
			array_append(&parser->nesting, &parser->state, 1);
			json_parser_update_input_pos(parser);

			if (parser->skipping) {
				parser->nested_skip_count++;
				return 0;
			}
			*type_r = JSON_TYPE_ARRAY;
			return 1;
		}

		if ((ret = json_parse_string(parser, TRUE, value_r)) >= 0) {
			*type_r = JSON_TYPE_STRING;
		} else if ((ret = json_parse_number(parser, value_r)) >= 0) {
			*type_r = JSON_TYPE_NUMBER;
		} else if ((ret = json_parse_atom(parser, "true")) >= 0) {
			*type_r = JSON_TYPE_TRUE;
			*value_r = "true";
		} else if ((ret = json_parse_atom(parser, "false")) >= 0) {
			*type_r = JSON_TYPE_FALSE;
			*value_r = "false";
		} else if ((ret = json_parse_atom(parser, "null")) >= 0) {
			*type_r = JSON_TYPE_NULL;
			*value_r = NULL;
		} else {
			parser->error = "Invalid data as value";
			return -1;
		}
		if (ret == 0) {
			i_assert(parser->data == parser->end);
			if (parser->skipping && *type_r == JSON_TYPE_STRING) {
				/* a large string that we want to skip over. */
				json_parser_update_input_pos(parser);
				parser->state = parser->state == JSON_STATE_OBJECT_VALUE ?
					JSON_STATE_OBJECT_SKIP_STRING :
					JSON_STATE_ARRAY_SKIP_STRING;
				return 0;
			}
			return -1;
		}
		parser->state = parser->state == JSON_STATE_OBJECT_VALUE ?
			JSON_STATE_OBJECT_NEXT : JSON_STATE_ARRAY_NEXT;
		break;
	case JSON_STATE_OBJECT_OPEN:
		if (*parser->data == '}')
			return json_parse_close_object(parser, type_r);
		parser->state = JSON_STATE_OBJECT_KEY;
		/* fall through */
	case JSON_STATE_OBJECT_KEY:
		if (json_parse_string(parser, FALSE, value_r) <= 0) {
			parser->error = "Expected string as object key";
			return -1;
		}
		*type_r = JSON_TYPE_OBJECT_KEY;
		parser->state = JSON_STATE_OBJECT_COLON;
		break;
	case JSON_STATE_OBJECT_COLON:
		if (*parser->data != ':') {
			parser->error = "Expected ':' after key";
			return -1;
		}
		parser->data++;
		parser->state = JSON_STATE_OBJECT_VALUE;
		json_parser_update_input_pos(parser);
		return 0;
	case JSON_STATE_OBJECT_NEXT:
		if (parser->skipping && parser->nested_skip_count == 0) {
			/* we skipped over the previous value */
			parser->skipping = FALSE;
		}
		if (*parser->data == '}')
			return json_parse_close_object(parser, type_r);
		if (*parser->data != ',') {
			parser->error = "Expected ',' or '}' after object value";
			return -1;
		}
		parser->state = JSON_STATE_OBJECT_KEY;
		parser->data++;
		json_parser_update_input_pos(parser);
		return 0;
	case JSON_STATE_ARRAY_OPEN:
		if (*parser->data == ']')
			return json_parse_close_array(parser, type_r);
		parser->state = JSON_STATE_ARRAY_VALUE;
		return 0;
	case JSON_STATE_ARRAY_NEXT:
		if (parser->skipping && parser->nested_skip_count == 0) {
			/* we skipped over the previous value */
			parser->skipping = FALSE;
		}
		if (*parser->data == ']')
			return json_parse_close_array(parser, type_r);
		if (*parser->data != ',') {
			parser->error = "Expected ',' or '}' after array value";
			return -1;
		}
		parser->state = JSON_STATE_ARRAY_VALUE;
		parser->data++;
		json_parser_update_input_pos(parser);
		return 0;
	case JSON_STATE_OBJECT_SKIP_STRING:
	case JSON_STATE_ARRAY_SKIP_STRING:
		if (json_skip_string(parser) <= 0)
			return -1;
		parser->state = parser->state == JSON_STATE_OBJECT_SKIP_STRING ?
			JSON_STATE_OBJECT_NEXT : JSON_STATE_ARRAY_NEXT;
		return 0;
	case JSON_STATE_DONE:
		parser->error = "Unexpected data at the end";
		return -1;
	}
	json_parser_update_input_pos(parser);
	return skipping ? 0 : 1;
}

int json_parse_next(struct json_parser *parser, enum json_type *type_r,
		    const char **value_r)
{
	int ret;

	i_assert(parser->strinput == NULL);

	*value_r = NULL;

	while ((ret = json_parser_read_more(parser)) > 0) {
		while ((ret = json_try_parse_next(parser, type_r, value_r)) == 0)
			;
		if (ret > 0)
			break;
		if (parser->data != parser->end)
			return -1;
		/* parsing probably failed because there wasn't enough input.
		   reset the error and try reading more. */
		parser->error = NULL;
		parser->highwater_offset = parser->input->v_offset +
			i_stream_get_data_size(parser->input);
	}
	return ret;
}

void json_parse_skip_next(struct json_parser *parser)
{
	i_assert(!parser->skipping);
	i_assert(parser->strinput == NULL);
	i_assert(parser->state == JSON_STATE_OBJECT_COLON ||
		 parser->state == JSON_STATE_OBJECT_VALUE ||
		 parser->state == JSON_STATE_ARRAY_VALUE);

	parser->skipping = TRUE;
}

static void json_strinput_destroyed(struct json_parser *parser)
{
	i_assert(parser->strinput != NULL);

	parser->strinput = NULL;
}

static int
json_try_parse_stream_start(struct json_parser *parser,
			    struct istream **input_r)
{
	if (!json_parse_whitespace(parser))
		return -1;

	if (parser->state == JSON_STATE_OBJECT_COLON) {
		if (*parser->data != ':') {
			parser->error = "Expected ':' after key";
			return -1;
		}
		parser->data++;
		parser->state = JSON_STATE_OBJECT_VALUE;
		if (!json_parse_whitespace(parser))
			return -1;
	}

	if (*parser->data != '"')
		return -1;
	parser->data++;
	json_parser_update_input_pos(parser);

	parser->state = parser->state == JSON_STATE_OBJECT_VALUE ?
		JSON_STATE_OBJECT_SKIP_STRING : JSON_STATE_ARRAY_SKIP_STRING;
	parser->strinput = i_stream_create_jsonstr(parser->input);
	i_stream_add_destroy_callback(parser->strinput,
				      json_strinput_destroyed, parser);

	*input_r = parser->strinput;
	return 1;
}

int json_parse_next_stream(struct json_parser *parser,
			   struct istream **input_r)
{
	int ret;

	i_assert(!parser->skipping);
	i_assert(parser->strinput == NULL);
	i_assert(parser->state == JSON_STATE_OBJECT_COLON ||
		 parser->state == JSON_STATE_OBJECT_VALUE ||
		 parser->state == JSON_STATE_ARRAY_VALUE);

	*input_r = NULL;

	while ((ret = json_parser_read_more(parser)) > 0) {
		if (json_try_parse_stream_start(parser, input_r) == 0)
			break;
		if (parser->data != parser->end)
			return -1;
		/* parsing probably failed because there wasn't enough input.
		   reset the error and try reading more. */
		parser->error = NULL;
		parser->highwater_offset = parser->input->v_offset +
			i_stream_get_data_size(parser->input);
	}
	return ret;
}

void json_append_escaped(string_t *dest, const char *src)
{
	for (; *src != '\0'; src++) {
		switch (*src) {
		case '\b':
			str_append(dest, "\\b");
			break;
		case '\f':
			str_append(dest, "\\f");
			break;
		case '\n':
			str_append(dest, "\\n");
			break;
		case '\r':
			str_append(dest, "\\r");
			break;
		case '\t':
			str_append(dest, "\\t");
			break;
		case '"':
			str_append(dest, "\\\"");
			break;
		case '\\':
			str_append(dest, "\\\\");
			break;
		default:
			if (*src < 32)
				str_printfa(dest, "\\u%04x", *src);
			else
				str_append_c(dest, *src);
			break;
		}
	}
}
