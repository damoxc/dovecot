#ifndef __CHARSET_UTF8_H
#define __CHARSET_UTF8_H

enum charset_result {
	CHARSET_RET_OK = 1,
	CHARSET_RET_OUTPUT_FULL = 0,
	CHARSET_RET_INCOMPLETE_INPUT = -1,
	CHARSET_RET_INVALID_INPUT = -2
};

/* Begin translation to UTF-8. */
struct charset_translation *charset_to_utf8_begin(const char *charset,
						  bool *unknown_charset);

void charset_to_utf8_end(struct charset_translation *t);

void charset_to_utf8_reset(struct charset_translation *t);

/* Translate src to UTF-8. If src_size is updated to contain the number of
   characters actually translated from src. */
enum charset_result
charset_to_ucase_utf8(struct charset_translation *t,
		      const unsigned char *src, size_t *src_size,
		      buffer_t *dest);

/* Simple wrappers for above functions. If utf8_size is non-NULL, it's set
   to same as strlen(returned data). */
const char *
charset_to_utf8_string(const char *charset, bool *unknown_charset,
		       const unsigned char *data, size_t size,
		       size_t *utf8_size_r);
const char *
charset_to_ucase_utf8_string(const char *charset, bool *unknown_charset,
			     const unsigned char *data, size_t size,
			     size_t *utf8_size_r);

void _charset_utf8_ucase(const unsigned char *src, size_t src_size,
			 buffer_t *dest, size_t destpos);
const char *_charset_utf8_ucase_strdup(const unsigned char *data, size_t size,
				       size_t *utf8_size_r);

#endif
