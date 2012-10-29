/* Copyright (c) 2002-2012 Dovecot authors, see the included COPYING file */

#include "lib.h"
#include "str.h"
#include "imap-arg.h"
#include "imap-quote.h"

/* If we have quoted-specials (<">, <\>) in a string, the minimum quoted-string
   overhead is 3 bytes ("\") while the minimum literal overhead is 5 bytes
   ("{n}\r\n"). But the literal overhead also depends on the string size. If
   the string length is less than 10, literal catches up to quoted-string after
   3 quoted-specials. If the string length is 10..99, it catches up after 4
   quoted-specials, and so on. We'll assume that the string lengths are usually
   in double digits, so we'll switch to literals after seeing 4
   quoted-specials. */
#define QUOTED_MAX_ESCAPE_CHARS 4

void imap_append_string(string_t *dest, const char *src)
{
	i_assert(src != NULL);

	imap_append_nstring(dest, src);
}

void imap_append_astring(string_t *dest, const char *src)
{
	unsigned int i;

	i_assert(src != NULL);

	for (i = 0; src[i] != '\0'; i++) {
		if (!IS_ASTRING_CHAR(src[i])) {
			imap_append_string(dest, src);
			return;
		}
	}
	if (i == 0)
		imap_append_string(dest, src);
	else
		str_append(dest, src);
}

static void
imap_append_literal(string_t *dest, const char *src, unsigned int pos)
{
	unsigned int full_len = pos + strlen(src+pos);

	str_printfa(dest, "{%u}\r\n", full_len);
	buffer_append(dest, src, full_len);
}

void imap_append_nstring(string_t *dest, const char *src)
{
	unsigned int i, escape_count = 0;

	if (src == NULL) {
		str_append(dest, "NIL");
		return;
	}

	/* first check if we can (or want to) write this as quoted or
	   as literal.

	   quoted-specials = DQUOTE / "\"
	   QUOTED-CHAR     = <any TEXT-CHAR except quoted-specials> /
	                     "\" quoted-specials
	   TEXT-CHAR       = <any CHAR except CR and LF>
	*/
	for (i = 0; src[i] != '\0'; i++) {
		switch (src[i]) {
		case '"':
		case '\\':
			if (escape_count++ < QUOTED_MAX_ESCAPE_CHARS)
				break;
			/* fall through */
		case 13:
		case 10:
			imap_append_literal(dest, src, i);
			return;
		default:
			if ((unsigned char)src[i] >= 0x80) {
				imap_append_literal(dest, src, i);
				return;
			}
			break;
		}
	}
	imap_append_quoted(dest, src);
}

void imap_append_quoted(string_t *dest, const char *src)
{
	str_append_c(dest, '"');
	for (; *src != '\0'; src++) {
		switch (*src) {
		case 13:
		case 10:
			/* not allowed */
			break;
		case '"':
		case '\\':
			str_append_c(dest, '\\');
			str_append_c(dest, *src);
			break;
		default:
			if ((unsigned char)*src >= 0x80) {
				/* 8bit input not allowed in dquotes */
				break;
			}

			str_append_c(dest, *src);
			break;
		}
	}
	str_append_c(dest, '"');
}

void imap_append_string_for_humans(string_t *dest,
				   const unsigned char *src, size_t size)
{
	size_t i, pos, remove_count = 0;
	bool last_lwsp = TRUE, modify = FALSE;

	/* first check if there is anything to change */
	for (i = 0; i < size; i++) {
		switch (src[i]) {
		case 0:
			/* convert NUL to #0x80 */
			last_lwsp = FALSE;
			modify = TRUE;
			break;
		case '\t':
			modify = TRUE;
			/* fall through */
		case ' ':
			if (last_lwsp) {
				modify = TRUE;
				remove_count++;
			}
			last_lwsp = TRUE;
			break;
		case 13:
		case 10:
			remove_count++;
			modify = TRUE;
			break;
		case '"':
		case '\\':
			modify = TRUE;
			last_lwsp = FALSE;
			break;
		default:
			if ((src[i] & 0x80) != 0)
				modify = TRUE;
			last_lwsp = FALSE;
			break;
		}
	}
	if (last_lwsp) {
		modify = TRUE;
		remove_count++;
	}
	if (!modify) {
		/* fast path: we can simply write it as quoted string
		   without any escaping */
		str_append_c(dest, '"');
		str_append_n(dest, src, size);
		str_append_c(dest, '"');
		return;
	}

	str_printfa(dest, "{%"PRIuSIZE_T"}\r\n", size - remove_count);
	pos = str_len(dest);

	last_lwsp = TRUE;
	for (i = 0; i < size; i++) {
		switch (src[i]) {
		case 0:
			str_append_c(dest, 128);
			last_lwsp = FALSE;
			break;
		case '\t':
		case ' ':
			if (!last_lwsp)
				str_append_c(dest, ' ');
			last_lwsp = TRUE;
			break;
		case 13:
		case 10:
			break;
		default:
			last_lwsp = FALSE;
			str_append_c(dest, src[i]);
			break;
		}
	}
	if (last_lwsp)
		str_truncate(dest, str_len(dest)-1);
	i_assert(str_len(dest) - pos == size - remove_count);
}
