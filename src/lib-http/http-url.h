#ifndef HTTP_URL_H
#define HTTP_URL_H

#include "net.h"

struct http_request_target;

struct http_url {
	/* server */
	const char *host_name;
	struct ip_addr host_ip;
	in_port_t port;

	/* userinfo (not parsed by default) */
	const char *user;
	const char *password;

	/* path */
	const char *path;

	/* ?query (still encoded) */
	const char *enc_query;

	/* #fragment (still encoded) */
	const char *enc_fragment;

	unsigned int have_host_ip:1; /* URL uses IP address */
	unsigned int have_port:1;
	unsigned int have_ssl:1;
};

/*
 * HTTP URL parsing
 */

enum http_url_parse_flags {
	/* Scheme part 'http:' is already parsed externally. This implies that
	   this is an absolute HTTP URL. */
	HTTP_URL_PARSE_SCHEME_EXTERNAL	= 0x01,
	/* Allow '#fragment' part in HTTP URL */
	HTTP_URL_ALLOW_FRAGMENT_PART = 0x02,
	/* Allow 'user:password@' part in HTTP URL */
	HTTP_URL_ALLOW_USERINFO_PART = 0x04
};

int http_url_parse(const char *url, struct http_url *base,
		   enum http_url_parse_flags flags, pool_t pool,
		   struct http_url **url_r, const char **error_r);

int http_url_request_target_parse(const char *request_target,
	const char *host_header, pool_t pool,
	struct http_request_target *target, const char **error_r);

/*
 * HTTP URL construction
 */

const char *http_url_create(const struct http_url *url);

const char *http_url_create_target(const struct http_url *url);

void http_url_escape_param(string_t *out, const char *data);

#endif
