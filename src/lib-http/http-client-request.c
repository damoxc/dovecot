/* Copyright (c) 2013 Dovecot authors, see the included COPYING file */

#include "lib.h"
#include "net.h"
#include "str.h"
#include "hash.h"
#include "array.h"
#include "istream.h"
#include "ostream.h"
#include "http-url.h"
#include "http-response-parser.h"
#include "http-transfer.h"

#include "http-client-private.h"

/*
 * Logging
 */

static inline void
http_client_request_debug(struct http_client_request *req,
	const char *format, ...) ATTR_FORMAT(2, 3);

static inline void
http_client_request_debug(struct http_client_request *req,
	const char *format, ...)
{
	va_list args;

	if (req->client->set.debug) {
		va_start(args, format);	
		i_debug("http-client: request %s: %s",
			http_client_request_label(req), t_strdup_vprintf(format, args));
		va_end(args);
	}
}

/*
 * Request
 */

#undef http_client_request
struct http_client_request *
http_client_request(struct http_client *client,
		    const char *method, const char *host, const char *target,
		    http_client_request_callback_t *callback, void *context)
{
	pool_t pool;
	struct http_client_request *req;

	pool = pool_alloconly_create("http client request", 2048);
	req = p_new(pool, struct http_client_request, 1);
	req->pool = pool;
	req->refcount = 1;
	req->client = client;
	req->method = p_strdup(pool, method);
	req->hostname = p_strdup(pool, host);
	req->port = HTTP_DEFAULT_PORT;
	req->target = p_strdup(pool, target);
	req->callback = callback;
	req->context = context;
	req->headers = str_new(default_pool, 256);

	req->state = HTTP_REQUEST_STATE_NEW;
	return req;
}

void http_client_request_ref(struct http_client_request *req)
{
	req->refcount++;
}

void http_client_request_unref(struct http_client_request **_req)
{
	struct http_client_request *req = *_req;
	struct http_client *client = req->client;

	i_assert(req->refcount > 0);

	if (--req->refcount > 0)
		return;

	/* only decrease pending request counter if this request was submitted */
	if (req->state > HTTP_REQUEST_STATE_NEW)
		req->client->pending_requests--;

	http_client_request_debug(req, "Destroy (requests left=%d)",
		client->pending_requests);

	if (client->pending_requests == 0 && client->ioloop != NULL)
		io_loop_stop(client->ioloop);

	if (req->payload_input != NULL)
		i_stream_unref(&req->payload_input);
	if (req->payload_output != NULL)
		o_stream_unref(&req->payload_output);
	str_free(&req->headers);
	pool_unref(&req->pool);
	*_req = NULL;
}

void http_client_request_set_port(struct http_client_request *req,
	unsigned int port)
{
	i_assert(req->state == HTTP_REQUEST_STATE_NEW);
	req->port = port;
}

void http_client_request_set_ssl(struct http_client_request *req,
	bool ssl)
{
	i_assert(req->state == HTTP_REQUEST_STATE_NEW);
	if (ssl) {
		if (!req->ssl && req->port == HTTP_DEFAULT_PORT)
			req->port = HTTPS_DEFAULT_PORT;
	} else {
		if (req->ssl && req->port == HTTPS_DEFAULT_PORT)
			req->port = HTTP_DEFAULT_PORT;
	}
	req->ssl = ssl;
}

void http_client_request_set_urgent(struct http_client_request *req)
{
	i_assert(req->state == HTTP_REQUEST_STATE_NEW);
	req->urgent = TRUE;
}

void http_client_request_add_header(struct http_client_request *req,
				    const char *key, const char *value)
{
	i_assert(req->state == HTTP_REQUEST_STATE_NEW);
	str_printfa(req->headers, "%s: %s\r\n", key, value);
}

void http_client_request_set_payload(struct http_client_request *req,
				     struct istream *input, bool sync)
{
	int ret;

	i_assert(req->state == HTTP_REQUEST_STATE_NEW);
	i_assert(req->payload_input == NULL);

	i_stream_ref(input);
	req->payload_input = input;
	if ((ret = i_stream_get_size(input, TRUE, &req->payload_size)) <= 0) {
		if (ret < 0) {
			i_error("i_stream_get_size(%s) failed: %m",
				i_stream_get_name(input));
		}
		req->payload_size = 0;
		req->payload_chunked = TRUE;
	}
	req->payload_offset = input->v_offset;

	/* prepare request payload sync using 100 Continue response from server */
	if ((req->payload_chunked || req->payload_size > 0) && sync)
		req->payload_sync = TRUE;
}

static void http_client_request_do_submit(struct http_client_request *req)
{
	struct http_client_host *host;

	i_assert(req->state == HTTP_REQUEST_STATE_NEW);
	
	host = http_client_host_get(req->client, req->hostname);
	req->state = HTTP_REQUEST_STATE_QUEUED;

	http_client_host_submit_request(host, req);
}

void http_client_request_submit(struct http_client_request *req)
{
	http_client_request_debug(req, "Submitted");
	req->client->pending_requests++;

	http_client_request_do_submit(req);
}

static void
http_client_request_finish_payload_out(struct http_client_request *req)
{
	if (req->payload_output != NULL) {
		o_stream_unref(&req->payload_output);
		req->payload_output = NULL;
	}
	req->state = HTTP_REQUEST_STATE_WAITING;
	req->conn->output_locked = FALSE;
	http_client_request_debug(req, "Sent all payload");
}

static int
http_client_request_continue_payload(struct http_client_request **_req,
	const unsigned char *data, size_t size)
{
	struct ioloop *prev_ioloop = current_ioloop;
	struct http_client_request *req = *_req;
	struct http_client_connection *conn = req->conn;
	struct http_client *client = req->client;
	int ret;

	i_assert(req->state == HTTP_REQUEST_STATE_NEW ||
		req->state == HTTP_REQUEST_STATE_PAYLOAD_OUT);
	i_assert(req->payload_input == NULL);

	if (conn != NULL)
		http_client_connection_ref(conn);
	http_client_request_ref(req);
	req->payload_wait = TRUE;

	if (data == NULL) {
		req->payload_input = NULL;
		if (req->state == HTTP_REQUEST_STATE_PAYLOAD_OUT)
			http_client_request_finish_payload_out(req);
	} else { 
		req->payload_input = i_stream_create_from_data(data, size);
		i_stream_set_name(req->payload_input, "<HTTP request payload>");
	}
	req->payload_size = 0;
	req->payload_chunked = TRUE;

	if (req->state == HTTP_REQUEST_STATE_NEW)
		http_client_request_submit(req);

	/* Wait for payload data to be written */

	i_assert(client->ioloop == NULL);
	client->ioloop = io_loop_create();
	http_client_switch_ioloop(client);

	while (req->state < HTTP_REQUEST_STATE_FINISHED) {
		http_client_request_debug(req, "Waiting for request to finish");
		
		if (req->state == HTTP_REQUEST_STATE_PAYLOAD_OUT)
			o_stream_set_flush_pending(req->payload_output, TRUE);
		io_loop_run(client->ioloop);

		if (req->state == HTTP_REQUEST_STATE_PAYLOAD_OUT &&
			req->payload_input->eof) {
			i_stream_unref(&req->payload_input);
			req->payload_input = NULL;
			break;
		}
	}

	current_ioloop = prev_ioloop;
	http_client_switch_ioloop(client);
	current_ioloop = client->ioloop;
	io_loop_destroy(&client->ioloop);

	if (req->state == HTTP_REQUEST_STATE_FINISHED)
		ret = 1;
	else
		ret = (req->state == HTTP_REQUEST_STATE_ABORTED ? -1 : 0);

	req->payload_wait = FALSE;
	http_client_request_unref(_req);
	if (conn != NULL)
		http_client_connection_unref(&conn);

	/* Return status */
	return ret;
}

int http_client_request_send_payload(struct http_client_request **_req,
	const unsigned char *data, size_t size)
{
	i_assert(data != NULL);

	return http_client_request_continue_payload(_req, data, size);
}

int http_client_request_finish_payload(struct http_client_request **_req)
{
	return http_client_request_continue_payload(_req, NULL, 0);
}

int http_client_request_send_more(struct http_client_request *req,
				  const char **error_r)
{
	struct http_client_connection *conn = req->conn;
	struct ostream *output = req->payload_output;
	off_t ret;

	i_assert(req->payload_input != NULL);

	/* chunked ostream needs to write to the parent stream's buffer */
	o_stream_set_max_buffer_size(output, IO_BLOCK_SIZE);
	ret = o_stream_send_istream(output, req->payload_input);
	o_stream_set_max_buffer_size(output, (size_t)-1);

	if (req->payload_input->stream_errno != 0) {
		errno = req->payload_input->stream_errno;
		*error_r = t_strdup_printf("read(%s) failed: %m",
					   i_stream_get_name(req->payload_input));
	} else if (output->stream_errno != 0) {
		errno = output->stream_errno;
		*error_r = t_strdup_printf("write(%s) failed: %m",
					   o_stream_get_name(output));
	} else {
		i_assert(ret >= 0);
	}

	if (!i_stream_have_bytes_left(req->payload_input)) {
		if (!req->payload_chunked &&
			req->payload_input->v_offset - req->payload_offset != req->payload_size) {
			i_error("stream input size changed"); //FIXME
			return -1;
		}

		if (req->payload_wait) {
			conn->output_locked = TRUE;
			if (req->client->ioloop != NULL)
				io_loop_stop(req->client->ioloop);
		} else {
			http_client_request_finish_payload_out(req);
		}

	} else {
		conn->output_locked = TRUE;
		o_stream_set_flush_pending(output, TRUE);
		http_client_request_debug(req, "Partially sent payload");
	}
	return ret < 0 ? -1 : 0;
}

int http_client_request_send(struct http_client_request *req,
			     const char **error_r)
{
	struct http_client_connection *conn = req->conn;
	struct ostream *output = conn->conn.output;
	string_t *rtext = t_str_new(256);
	struct const_iovec iov[3];
	int ret = 0;

	i_assert(!req->conn->output_locked);

	str_append(rtext, req->method);
	str_append(rtext, " ");
	str_append(rtext, req->target);
	str_append(rtext, " HTTP/1.1\r\n");
	str_append(rtext, "Host: ");
	str_append(rtext, req->hostname);
	if ((!req->ssl &&req->port != HTTP_DEFAULT_PORT) ||
		(req->ssl && req->port != HTTPS_DEFAULT_PORT)) {
		str_printfa(rtext, ":%u", req->port);
	}
	str_append(rtext, "\r\n");
	if (req->payload_sync) {
		str_append(rtext, "Expect: 100-continue\r\n");
	}
	if (req->payload_chunked) {
		str_append(rtext, "Transfer-Encoding: chunked\r\n");
		req->payload_output =
			http_transfer_chunked_ostream_create(output);
	} else if (req->payload_input != NULL) {
		/* send Content-Length if we have specified a payload,
		   even if it's 0 bytes. */
		str_printfa(rtext, "Content-Length: %"PRIuUOFF_T"\r\n",
			    req->payload_size);
		req->payload_output = output;
		o_stream_ref(output);
	}

	iov[0].iov_base = str_data(rtext);
	iov[0].iov_len = str_len(rtext);
	iov[1].iov_base = str_data(req->headers);
	iov[1].iov_len = str_len(req->headers);
	iov[2].iov_base = "\r\n";
	iov[2].iov_len = 2;

	req->state = HTTP_REQUEST_STATE_PAYLOAD_OUT;
	o_stream_cork(output);
	if (o_stream_sendv(output, iov, N_ELEMENTS(iov)) < 0) {
		*error_r = t_strdup_printf("write(%s) failed: %m",
					   o_stream_get_name(output));
		ret = -1;
	}

	http_client_request_debug(req, "Sent header");

	if (ret >= 0 && req->payload_output != NULL) {
		if (!req->payload_sync) {
			if (http_client_request_send_more(req, error_r) < 0)
				ret = -1;
		} else {
			http_client_request_debug(req, "Waiting for 100-continue");
		}
	} else {
		req->state = HTTP_REQUEST_STATE_WAITING;
		conn->output_locked = FALSE;
	}
	o_stream_uncork(output);
	return ret;
}

void http_client_request_callback(struct http_client_request *req,
			     struct http_response *response)
{
	http_client_request_callback_t *callback = req->callback;

	req->state = HTTP_REQUEST_STATE_GOT_RESPONSE;

	req->callback = NULL;
	if (callback != NULL)
		callback(response, req->context);
}

static void
http_client_request_send_error(struct http_client_request *req,
			       unsigned int status, const char *error)
{
	http_client_request_callback_t *callback;

	req->state = HTTP_REQUEST_STATE_ABORTED;

	callback = req->callback;
	req->callback = NULL;
	if (callback != NULL) {
		struct http_response response;

		memset(&response, 0, sizeof(response));
		response.status = status;
		response.reason = error;
		(void)callback(&response, req->context);
	}
}

void http_client_request_error(struct http_client_request *req,
	unsigned int status, const char *error)
{
	http_client_request_send_error(req, status, error);
	http_client_request_unref(&req);
}

void http_client_request_abort(struct http_client_request **_req)
{
	struct http_client_request *req = *_req;

	if (req->state >= HTTP_REQUEST_STATE_FINISHED)
		return;
	req->callback = NULL;
	req->state = HTTP_REQUEST_STATE_ABORTED;
	if (req->host != NULL)
		http_client_host_drop_request(req->host, req);
	http_client_request_unref(_req);
}

void http_client_request_finish(struct http_client_request **_req)
{
	struct http_client_request *req = *_req;

	if (req->state >= HTTP_REQUEST_STATE_FINISHED)
		return;

	http_client_request_debug(req, "Finished");

	req->callback = NULL;
	req->state = HTTP_REQUEST_STATE_FINISHED;

	if (req->destroy_callback != NULL)
		req->destroy_callback(req->destroy_context);

	if (req->payload_wait && req->client->ioloop != NULL)
		io_loop_stop(req->client->ioloop);
	http_client_request_unref(_req);
}

void http_client_request_redirect(struct http_client_request *req,
	unsigned int status, const char *location)
{
	struct http_url *url;
	const char *error;
	unsigned int newport;

	/* parse URL */
	if (http_url_parse(location, NULL, 0,
			   pool_datastack_create(), &url, &error) < 0) {
		http_client_request_error(req, HTTP_CLIENT_REQUEST_ERROR_INVALID_REDIRECT,
			t_strdup_printf("Invalid redirect location: %s", error));
		return;
	}

	if (++req->redirects > req->client->set.max_redirects) {
		if (req->client->set.max_redirects > 0) {
			http_client_request_error(req,
				HTTP_CLIENT_REQUEST_ERROR_INVALID_REDIRECT,
				t_strdup_printf("Redirected more than %d times",
					req->client->set.max_redirects));
		} else {
			http_client_request_error(req,
				HTTP_CLIENT_REQUEST_ERROR_INVALID_REDIRECT,
					"Redirect refused");
		}
		return;
	}

	/* rewind payload stream */
	if (req->payload_input != NULL && req->payload_size > 0 && status != 303) {
		if (req->payload_input->v_offset != req->payload_offset &&
			!req->payload_input->seekable) {
			http_client_request_error(req,
				HTTP_CLIENT_REQUEST_ERROR_ABORTED,
				"Redirect failed: Cannot resend payload; stream is not seekable");
			return;
		} else {
			i_stream_seek(req->payload_input, req->payload_offset);
		}
	}

	/* rewind payload stream */
	if (req->payload_input != NULL && req->payload_size > 0 && status != 303) {
		if (req->payload_input->v_offset != req->payload_offset &&
			!req->payload_input->seekable) {
			http_client_request_error(req,
				HTTP_CLIENT_REQUEST_ERROR_ABORTED,
				"Redirect failed: Cannot resend payload; stream is not seekable");
			return;
		} else {
			i_stream_seek(req->payload_input, req->payload_offset);
		}
	}

	newport = (url->have_port ? url->port : (url->have_ssl ? 443 : 80));

	http_client_request_debug(req, "Redirecting to http://%s:%u%s",
		url->host_name, newport, url->path);

	// FIXME: handle literal IP specially (avoid duplicate parsing)
	req->host = NULL;
	req->conn = NULL;
	req->hostname = p_strdup(req->pool, url->host_name);
	req->port = newport;
	req->target = p_strdup(req->pool, url->path);
	req->ssl = url->have_ssl;

	/* https://tools.ietf.org/html/draft-ietf-httpbis-p2-semantics-21
	      Section-7.4.4
	
	   -> A 303 `See Other' redirect status response is handled a bit differently.
	   Basically, the response content is located elsewhere, but the original
	   (POST) request is handled already.
	 */
	if (status == 303 && strcasecmp(req->method, "HEAD") != 0 &&
		strcasecmp(req->method, "GET") != 0) {
		// FIXME: should we provide the means to skip this step? The original
		// request was already handled at this point.
		req->method = p_strdup(req->pool, "GET");

		/* drop payload */
		if (req->payload_input != NULL)
			i_stream_unref(&req->payload_input);
		req->payload_size = 0;
		req->payload_offset = 0;
	}

	/* https://tools.ietf.org/html/draft-ietf-httpbis-p2-semantics-21
	      Section-7.4.4
	
	   -> A 303 `See Other' redirect status response is handled a bit differently.
	   Basically, the response content is located elsewhere, but the original
	   (POST) request is handled already.
	 */
	if (status == 303 && strcasecmp(req->method, "HEAD") != 0 &&
		strcasecmp(req->method, "GET") != 0) {
		// FIXME: should we provide the means to skip this step? The original
		// request was already handled at this point.
		req->method = p_strdup(req->pool, "GET");

		/* drop payload */
		if (req->payload_input != NULL)
			i_stream_unref(&req->payload_input);
		req->payload_size = 0;
		req->payload_offset = 0;
	}

	/* resubmit */
	req->state = HTTP_REQUEST_STATE_NEW;
	http_client_request_do_submit(req);
}

void http_client_request_resubmit(struct http_client_request *req)
{
	http_client_request_debug(req, "Resubmitting request");

	/* rewind payload stream */
	if (req->payload_input != NULL && req->payload_size > 0) {
		if (req->payload_input->v_offset != req->payload_offset &&
			!req->payload_input->seekable) {
			http_client_request_error(req,
				HTTP_CLIENT_REQUEST_ERROR_ABORTED,
				"Resubmission failed: Cannot resend payload; stream is not seekable");
			return;
		} else {
			i_stream_seek(req->payload_input, req->payload_offset);
		}
	}

	/* rewind payload stream */
	if (req->payload_input != NULL && req->payload_size > 0) {
		if (req->payload_input->v_offset != req->payload_offset &&
			!req->payload_input->seekable) {
			http_client_request_error(req,
				HTTP_CLIENT_REQUEST_ERROR_ABORTED,
				"Resubmission failed: Cannot resend payload; stream is not seekable");
			return;
		} else {
			i_stream_seek(req->payload_input, req->payload_offset);
		}
	}

	req->conn = NULL;
	req->peer = NULL;
	req->state = HTTP_REQUEST_STATE_QUEUED;
	http_client_host_submit_request(req->host, req);
}

void http_client_request_retry(struct http_client_request *req,
	unsigned int status, const char *error)
{
	/* limit the number of attempts for each request */
	if (++req->attempts >= req->client->set.max_attempts) {
		/* return error */
		http_client_request_error(req, status, error);
		return;
	}

	http_client_request_debug(req, "Retrying (attempts=%d)", req->attempts);

	/* resubmit */
	http_client_request_resubmit(req);
}

void http_client_request_set_destroy_callback(struct http_client_request *req,
					      void (*callback)(void *),
					      void *context)
{
	req->destroy_callback = callback;
	req->destroy_context = context;
}
