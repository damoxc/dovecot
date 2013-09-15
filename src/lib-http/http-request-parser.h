#ifndef HTTP_REQUEST_PARSER_H
#define HTTP_REQUEST_PARSER_H

#include "http-request.h"
 
enum http_request_parse_error {
	HTTP_REQUEST_PARSE_ERROR_NONE = 0,           /* no error */
	HTTP_REQUEST_PARSE_ERROR_BROKEN_STREAM,      /* stream error */
	HTTP_REQUEST_PARSE_ERROR_BROKEN_REQUEST,     /* unrecoverable generic error */
	HTTP_REQUEST_PARSE_ERROR_BAD_REQUEST,        /* recoverable generic error */
	HTTP_REQUEST_PARSE_ERROR_NOT_IMPLEMENTED,    /* used unimplemented feature
	                                                (recoverable) */
	HTTP_REQUEST_PARSE_ERROR_EXPECTATION_FAILED, /* unknown item in Expect:
	                                                header (recoverable) */
	HTTP_REQUEST_PARSE_ERROR_METHOD_TOO_LONG,    /* method too long (fatal) */
	HTTP_REQUEST_PARSE_ERROR_TARGET_TOO_LONG,    /* target too long (fatal) */
	HTTP_REQUEST_PARSE_ERROR_PAYLOAD_TOO_LARGE   /* payload too large (fatal) */
};

struct http_request_parser *
http_request_parser_init(struct istream *input,
	const struct http_request_limits *limits) ATTR_NULL(2);
void http_request_parser_deinit(struct http_request_parser **_parser);

int http_request_parse_next(struct http_request_parser *parser,
			    pool_t pool, struct http_request *request,
			    enum http_request_parse_error *error_code_r, const char **error_r);

bool http_request_parser_pending_payload(struct http_request_parser *parser);

#endif
