#ifndef HTTP_RESPONSE_H
#define HTTP_RESPONSE_H

#include "array.h"

#include "http-header.h"

#define http_response_header http_header_field /* FIXME: remove in v2.3 */

struct http_response {
	unsigned char version_major;
	unsigned char version_minor;

	unsigned int status;

	const char *reason;
	const char *location;

	time_t date;
	const struct http_header *header;
	struct istream *payload;

	/* FIXME: remove in v2.3 */
	ARRAY_TYPE(http_header_field) headers;

	ARRAY_TYPE(const_string) connection_options;

	unsigned int connection_close:1;
};

static inline void
http_response_init(struct http_response *resp,
	unsigned int status, const char *reason)
{
	memset(resp, 0, sizeof(*resp));
	resp->status = status;
	resp->reason = reason;
}

static inline const struct http_header_field *
http_response_header_find(const struct http_response *resp, const char *name)
{
	if (resp->header == NULL)
		return NULL;
	return http_header_field_find(resp->header, name);
}

static inline const char *
http_response_header_get(const struct http_response *resp, const char *name)
{
	if (resp->header == NULL)
		return NULL;
	return http_header_field_get(resp->header, name);
}

static inline const ARRAY_TYPE(http_header_field) *
http_response_header_get_fields(const struct http_response *resp)
{
	if (resp->header == NULL)
		return NULL;
	return http_header_get_fields(resp->header);
}

bool http_response_has_connection_option(const struct http_response *resp,
	const char *option);
int http_response_get_payload_size(const struct http_response *resp,
	uoff_t *size_r);

#endif
