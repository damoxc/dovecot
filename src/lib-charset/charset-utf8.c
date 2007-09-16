/* Copyright (c) 2002-2007 Dovecot authors, see the included COPYING file */

#include "lib.h"
#include "buffer.h"
#include "unichar.h"
#include "charset-utf8.h"

#include <ctype.h>

bool charset_is_utf8(const char *charset)
{
	return strcasecmp(charset, "us-ascii") == 0 ||
		strcasecmp(charset, "ascii") == 0 ||
		strcasecmp(charset, "UTF-8") == 0 ||
		strcasecmp(charset, "UTF8") == 0;
}

#ifndef HAVE_ICONV

struct charset_translation {
	enum charset_flags flags;
};

static struct charset_translation raw_translation = { 0 };
static struct charset_translation tc_translation = {
	CHARSET_FLAG_DECOMP_TITLECASE
};

int charset_to_utf8_begin(const char *charset, enum charset_flags flags,
			  struct charset_translation **t_r)
{
	if (charset_is_utf8(charset)) {
		if ((flags & CHARSET_FLAG_DECOMP_TITLECASE) != 0)
			*t_r = &tc_translation;
		else
			*t_r = &raw_translation;
		return 0;
	}

	/* no support for charsets that need translation */
	return -1;
}

void charset_to_utf8_end(struct charset_translation **t ATTR_UNUSED)
{
}

void charset_to_utf8_reset(struct charset_translation *t ATTR_UNUSED)
{
}

enum charset_result
charset_to_utf8(struct charset_translation *t,
		const unsigned char *src, size_t *src_size, buffer_t *dest)
{
	if ((t->flags & CHARSET_FLAG_DECOMP_TITLECASE) == 0)
		buffer_append(dest, src, *src_size);
	else {
		if (uni_utf8_to_decomposed_titlecase(src, *src_size, dest) < 0)
			return CHARSET_RET_INVALID_INPUT;
	}
	return CHARSET_RET_OK;
}

#endif
