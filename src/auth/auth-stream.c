/* Copyright (c) 2005-2008 Dovecot authors, see the included COPYING file */

#include "common.h"
#include "str.h"
#include "ostream.h"
#include "auth-request.h"
#include "auth-stream.h"

struct auth_stream_reply {
	string_t *str;
};

struct auth_stream_reply *auth_stream_reply_init(struct auth_request *request)
{
	struct auth_stream_reply *reply;

	reply = p_new(request->pool, struct auth_stream_reply, 1);
	reply->str = str_new(request->pool, 256);
	return reply;
}

void auth_stream_reply_add(struct auth_stream_reply *reply,
			   const char *key, const char *value)
{
	if (str_len(reply->str) > 0)
		str_append_c(reply->str, '\t');
	if (key != NULL) {
		i_assert(*key != '\0');
		i_assert(strchr(key, '\t') == NULL &&
			 strchr(key, '\n') == NULL);

		str_append(reply->str, key);
		if (value != NULL)
			str_append_c(reply->str, '=');
	}
	if (value != NULL) {
		/* escape dangerous characters in the value */
		for (; *value != '\0'; value++) {
			switch (*value) {
			case '\001':
				str_append_c(reply->str, '\001');
				str_append_c(reply->str, '1');
				break;
			case '\t':
				str_append_c(reply->str, '\001');
				str_append_c(reply->str, 't');
				break;
			case '\n':
				str_append_c(reply->str, '\001');
				str_append_c(reply->str, 'n');
				break;
			default:
				str_append_c(reply->str, *value);
				break;
			}
		}
	}
}

void auth_stream_reply_remove(struct auth_stream_reply *reply, const char *key)
{
	const char *str = str_c(reply->str);
	unsigned int i, start, key_len = strlen(key);

	i = 0;
	while (str[i] != '\0') {
		start = i;
		for (; str[i] != '\0'; i++) {
			if (str[i] == '\t') {
				i++;
				break;
			}
		}

		if (strncmp(str+start, key, key_len) == 0 &&
		    (str[start+key_len] == '=' ||
		     str[start+key_len] == '\t' ||
		     str[start+key_len] == '\0')) {
			str_delete(reply->str, start, i-start);
			if (str_len(reply->str) == start && start > 0)
				str_delete(reply->str, start - 1, 1);
			break;
		}
	}
}

void auth_stream_reply_reset(struct auth_stream_reply *reply)
{
	str_truncate(reply->str, 0);
}

void auth_stream_reply_import(struct auth_stream_reply *reply, const char *str)
{
	if (str_len(reply->str) > 0)
		str_append_c(reply->str, '\t');
	str_append(reply->str, str);
}

const char *auth_stream_reply_export(struct auth_stream_reply *reply)
{
	return str_c(reply->str);
}

bool auth_stream_is_empty(struct auth_stream_reply *reply)
{
	return reply == NULL || str_len(reply->str) == 0;
}

const char *const *auth_stream_split(struct auth_stream_reply *reply)
{
	return t_strsplit(str_c(reply->str), "\t");
}
