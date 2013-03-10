/* Copyright (c) 2013 Dovecot authors, see the included COPYING file */

#include "lib.h"
#include "net.h"
#include "str.h"
#include "hash.h"
#include "array.h"
#include "ioloop.h"
#include "istream.h"
#include "ostream.h"
#include "iostream-rawlog.h"
#include "iostream-ssl.h"
#include "http-response-parser.h"

#include "http-client-private.h"

/*
 * Logging
 */

static inline void
http_client_connection_debug(struct http_client_connection *conn,
	const char *format, ...) ATTR_FORMAT(2, 3);
static inline void
http_client_connection_error(struct http_client_connection *conn,
	const char *format, ...) ATTR_FORMAT(2, 3);

static inline void
http_client_connection_debug(struct http_client_connection *conn,
	const char *format, ...)
{
	va_list args;

	if (conn->client->set.debug) {

		va_start(args, format);	
		i_debug("http-client: conn %s: %s",
			http_client_connection_label(conn),	t_strdup_vprintf(format, args));
		va_end(args);
	}
}

static inline void
http_client_connection_error(struct http_client_connection *conn,
	const char *format, ...)
{
	va_list args;

	va_start(args, format);	
	i_error("http-client: conn %s: %s",
		http_client_connection_label(conn),	t_strdup_vprintf(format, args));
	va_end(args);
}


/*
 * Connection
 */

static void http_client_connection_input(struct connection *_conn);

bool http_client_connection_is_ready(struct http_client_connection *conn)
{
	unsigned int pending_count = array_count(&conn->request_wait_list);

	if (conn->pending_request != NULL)
		pending_count++;
	return (conn->connected && !conn->output_locked &&
		!conn->close_indicated &&
		pending_count < conn->client->set.max_pipelined_requests);
}

bool http_client_connection_is_idle(struct http_client_connection *conn)
{
	return (conn->to_idle != NULL);
}

static void
http_client_connection_retry_requests(struct http_client_connection *conn,
	unsigned int status, const char *error)
{
	struct http_client_request **req;

	array_foreach_modifiable(&conn->request_wait_list, req) {
		http_client_request_retry(*req, status, error);
		http_client_request_unref(req);
	}	
	array_clear(&conn->request_wait_list);
}

static void
http_client_connection_server_close(struct http_client_connection **_conn)
{
	struct http_client_connection *conn = *_conn;
	struct http_client_request **req;

	conn->connected = FALSE;
	conn->closing = TRUE;

	http_client_connection_debug(conn,
		"Server explicitly closed connection");

	array_foreach_modifiable(&conn->request_wait_list, req) {
		http_client_request_resubmit(*req);
		http_client_request_unref(req);
	}	
	array_clear(&conn->request_wait_list);

	if (conn->client->ioloop != NULL)
		io_loop_stop(conn->client->ioloop);

	http_client_connection_unref(_conn);
}

static void
http_client_connection_abort_temp_error(struct http_client_connection **_conn,
	unsigned int status, const char *error)
{
	struct http_client_connection *conn = *_conn;
	const char *sslerr;

	if (status == HTTP_CLIENT_REQUEST_ERROR_CONNECTION_LOST &&
	    conn->ssl_iostream != NULL) {
		sslerr = ssl_iostream_get_last_error(conn->ssl_iostream);
		if (sslerr != NULL) {
			error = t_strdup_printf("%s (last SSL error: %s)",
						error, sslerr);
		}
	}

	conn->connected = FALSE;
	conn->closing = TRUE;
	
	http_client_connection_retry_requests(conn, status, error);
	http_client_connection_unref(_conn);
}

static void
http_client_connection_abort_error(struct http_client_connection **_conn,
	unsigned int status, const char *error)
{
	struct http_client_connection *conn = *_conn;
	struct http_client_request **req;

	conn->connected = FALSE;
	conn->closing = TRUE;
	
	array_foreach_modifiable(&conn->request_wait_list, req) {
		http_client_request_error(*req, status, error);
		http_client_request_unref(req);
	}	
	array_clear(&conn->request_wait_list);
	http_client_connection_unref(_conn);
}

static void
http_client_connection_idle_timeout(struct http_client_connection *conn)
{
	http_client_connection_debug(conn, "Idle connection timed out");

	http_client_connection_unref(&conn);
}

static void
http_client_connection_check_idle(struct http_client_connection *conn)
{
	unsigned int timeout, count;

	if (array_count(&conn->request_wait_list) == 0 &&
		conn->incoming_payload == NULL &&
		conn->client->set.max_idle_time_msecs > 0) {

		if (conn->to_idle != NULL) {
			/* timeout already set */
			return;
		}

		if (conn->client->ioloop != NULL)
			io_loop_stop(conn->client->ioloop);

		count = array_count(&conn->peer->conns);
		i_assert(count > 0);

		/* set timeout for this connection */
		if (count > conn->client->set.max_parallel_connections) {
			/* instant death for (urgent) connections above limit */
			timeout = 0;
		} else {
			unsigned int idle_count = http_client_peer_idle_connections(conn->peer);

			/* kill duplicate connections quicker;
				 linearly based on the number of connections */
			i_assert(count >= idle_count + 1);
			timeout = (conn->client->set.max_parallel_connections - idle_count) *
				(conn->client->set.max_idle_time_msecs /
					conn->client->set.max_parallel_connections);
		}

		http_client_connection_debug(conn, 
			"No more requests queued; going idle (timeout = %u msecs)",
			timeout);

		conn->to_idle =
			timeout_add(timeout, http_client_connection_idle_timeout, conn);

	} else {
		/* there should be no idle timeout */
		i_assert(conn->to_idle == NULL);
	}
}

static void
http_client_connection_continue_timeout(struct http_client_connection *conn)
{
	struct http_client_request *const *req_idx;
	struct http_client_request *req;
	const char *error;

	if (conn->to_response != NULL)
		timeout_remove(&conn->to_response);
	conn->peer->no_payload_sync = TRUE;

	http_client_connection_debug(conn, 
		"Expected 100-continue response timed out; sending payload anyway");

	req_idx = array_idx(&conn->request_wait_list, 0);
	req = req_idx[0];

	conn->payload_continue = TRUE;
	if (http_client_request_send_more(req, &error) < 0) {
		http_client_connection_abort_temp_error(&conn,
			HTTP_CLIENT_REQUEST_ERROR_CONNECTION_LOST,
			t_strdup_printf("Failed to send request: %s", error));
	}
}

bool http_client_connection_next_request(struct http_client_connection *conn)
{
	struct http_client_request *req = NULL;
	const char *error;
	bool have_pending_requests;

	if (!http_client_connection_is_ready(conn)) {
		http_client_connection_debug(conn, "Not ready for next request");
		return FALSE;
	}

	/* claim request, but no urgent request can be second in line */
	have_pending_requests = array_count(&conn->request_wait_list) > 0 ||
		conn->pending_request != NULL;
	req = http_client_peer_claim_request(conn->peer, have_pending_requests);
	if (req == NULL) {
		http_client_connection_check_idle(conn);
		return FALSE;	
	}

	if (conn->to_idle != NULL)
		timeout_remove(&conn->to_idle);

	req->conn = conn;
	conn->payload_continue = FALSE;
	if (conn->peer->no_payload_sync)
		req->payload_sync = FALSE;

	array_append(&conn->request_wait_list, &req, 1);
	http_client_request_ref(req);

	http_client_connection_debug(conn, "Claimed request %s",
		http_client_request_label(req));

	if (http_client_request_send(req, &error) < 0) {
		http_client_connection_abort_temp_error(&conn,
			HTTP_CLIENT_REQUEST_ERROR_CONNECTION_LOST,
			t_strdup_printf("Failed to send request: %s", error));
		return FALSE;
	}

	/* https://tools.ietf.org/html/draft-ietf-httpbis-p2-semantics-21;
			Section 6.1.2.1:

	   Because of the presence of older implementations, the protocol allows
	   ambiguous situations in which a client might send "Expect: 100-continue"
	   without receiving either a 417 (Expectation Failed) or a 100 (Continue)
	   status code.  Therefore, when a client sends this header field to an
	   origin server (possibly via a proxy) from which it has never seen a 100
	   (Continue) status code, the client SHOULD NOT wait for an indefinite
	   period before sending the payload body.
	 */
	if (req->payload_sync) {
		i_assert(req->payload_chunked || req->payload_size > 0);
		i_assert(conn->to_response == NULL);
		conn->to_response =	timeout_add(HTTP_CLIENT_CONTINUE_TIMEOUT_MSECS,
			http_client_connection_continue_timeout, conn);
	}

	return TRUE;
}

static void http_client_connection_destroy(struct connection *_conn)
{
	struct http_client_connection *conn =
		(struct http_client_connection *)_conn;
	const char *error;

	conn->closing = TRUE;
	conn->connected = FALSE;

	switch (_conn->disconnect_reason) {
	case CONNECTION_DISCONNECT_CONNECT_TIMEOUT:
		http_client_peer_connection_failure(conn->peer);
		break;
	case CONNECTION_DISCONNECT_CONN_CLOSED:
		/* retry pending requests if possible */
		error = _conn->input == NULL ? "Connection lost" :
			t_strdup_printf("Connection lost: %s",
					strerror(_conn->input->stream_errno));
		http_client_connection_retry_requests(conn,
			HTTP_CLIENT_REQUEST_ERROR_CONNECTION_LOST, error);
	default:
		break;
	}

	http_client_connection_unref(&conn);
}

static void http_client_payload_finished(struct http_client_connection *conn)
{
	timeout_remove(&conn->to_input);
	conn->conn.io = io_add(conn->conn.fd_in, IO_READ,
			       http_client_connection_input, &conn->conn);
}

static void
http_client_payload_destroyed_timeout(struct http_client_connection *conn)
{
	if (conn->close_indicated) {
		http_client_connection_server_close(&conn);
		return;
	}
	http_client_connection_input(&conn->conn);
}

static void http_client_payload_destroyed(struct http_client_request *req)
{
	struct http_client_connection *conn = req->conn;

	i_assert(conn->pending_request == req);
	i_assert(conn->incoming_payload != NULL);
	i_assert(conn->conn.io == NULL);

	http_client_connection_debug(conn, "Response payload stream destroyed");

	/* caller is allowed to change the socket fd to blocking while reading
	   the payload. make sure here that it's switched back. */
	net_set_nonblock(conn->conn.fd_in, TRUE);

	conn->incoming_payload = NULL;

	http_client_request_finish(&req);
	conn->pending_request = NULL;

	/* input stream may have pending input. make sure input handler
	   gets called (but don't do it directly, since we get get here
	   somewhere from the API user's code, which we can't really know what
	   state it is in). this call also triggers sending a new request if
	   necessary. */
	conn->to_input =
		timeout_add_short(0, http_client_payload_destroyed_timeout, conn);
}

static bool
http_client_connection_return_response(struct http_client_connection *conn,
	struct http_client_request *req, struct http_response *response)
{
	struct istream *payload;

	i_assert(conn->incoming_payload == NULL);
	i_assert(conn->pending_request == NULL);

	req->state = HTTP_REQUEST_STATE_GOT_RESPONSE;

	if (response->payload != NULL) {
		/* wrap the stream to capture the destroy event without destroying the
		   actual payload stream. */
		conn->incoming_payload = response->payload =
			i_stream_create_limit(response->payload, (uoff_t)-1);
		i_stream_set_destroy_callback(response->payload,
					      http_client_payload_destroyed,
					      req);
		/* the callback may add its own I/O, so we need to remove
		   our one before calling it */
		io_remove(&conn->conn.io);
	}

	http_client_request_callback(req, response);

	// FIXME: conn may be freed at this point..

	if (response->payload != NULL) {
		req->state = HTTP_REQUEST_STATE_PAYLOAD_IN;
		payload = response->payload;
		response->payload = NULL;
		conn->pending_request = req;
		i_stream_unref(&payload);
		if (conn->to_input != NULL) {
			/* already finished reading the payload */
			http_client_payload_finished(conn);
		}
	} else {
		http_client_request_finish(&req);
	}

	if (conn->incoming_payload == NULL) {
		i_assert(conn->conn.io != NULL);
		return TRUE;
	}

	return FALSE;
}

static void http_client_connection_input(struct connection *_conn)
{
	struct http_client_connection *conn =
		(struct http_client_connection *)_conn;
	struct http_response *response;
	struct http_client_request *const *req_idx;
	struct http_client_request *req = NULL;
	int finished = 0, ret;
	const char *error;
	bool no_payload = FALSE;

	i_assert(conn->incoming_payload == NULL);

	if (conn->to_input != NULL) {
		/* We came here from a timeout added by
		   http_client_payload_destroyed(). The IO couldn't be added
		   back immediately in there, because the HTTP API user may
		   still have had its own IO pointed to the same fd. It should
		   be removed by now, so we can add it back. */
		http_client_payload_finished(conn);
		finished++;
	}

	/* get first waiting request */
	if (array_count(&conn->request_wait_list) > 0) {
		req_idx = array_idx(&conn->request_wait_list, 0);
		req = req_idx[0];

		/* https://tools.ietf.org/html/draft-ietf-httpbis-p1-messaging-21
		     Section 3.3.2:

		   A server MAY send a Content-Length header field in a response to a
		   HEAD request [...]
		 */
		no_payload = (strcmp(req->method, "HEAD") == 0);
	}

	// FIXME: handle somehow if server replies before request->input is at EOF
	while ((ret=http_response_parse_next
		(conn->http_parser, no_payload, &response, &error)) > 0) {
		bool aborted;

		if (req == NULL) {
			/* server sent response without any requests in the wait list */
			http_client_connection_error(conn, "Got unexpected input from server");
			http_client_connection_unref(&conn);
			return;
		}

		/* Got some response; cancel response timeout */
		if (conn->to_response != NULL)
			timeout_remove(&conn->to_response);

		/* https://tools.ietf.org/html/draft-ietf-httpbis-p2-semantics-21
		     Section 7.2:

		   A client MUST be prepared to accept one or more 1xx status responses
		   prior to a regular response, even if the client does not expect a 100
		   (Continue) status message.  Unexpected 1xx status responses MAY be
		   ignored by a user agent.
		 */
		if (req->payload_sync && response->status == 100) {
			conn->payload_continue = TRUE;
			http_client_connection_debug(conn,
				"Got expected 100-continue response");
			if (http_client_request_send_more(req, &error) < 0) {
				http_client_connection_abort_temp_error(&conn,
					HTTP_CLIENT_REQUEST_ERROR_CONNECTION_LOST,
					t_strdup_printf("Failed to send request: %s", error));
			}
			return;
		} else if (response->status / 100 == 1) {
			/* ignore them for now */
			http_client_connection_debug(conn,
				"Got unexpected %u response; ignoring", response->status);
			/* restart timeout */
			conn->to_response =	timeout_add(HTTP_CLIENT_CONTINUE_TIMEOUT_MSECS,
				http_client_connection_continue_timeout, conn);
			continue;
		} 

		http_client_connection_debug(conn,
			"Got %u response for request %s",
			response->status, http_client_request_label(req));

		/* remove request from queue */
		array_delete(&conn->request_wait_list, 0, 1);
		aborted = (req->state == HTTP_REQUEST_STATE_ABORTED);
		http_client_request_unref(&req);
		
		conn->close_indicated = response->connection_close;

		if (!aborted) {
			if (response->status == 417 && req->payload_sync) {
				/* drop Expect: continue */
				req->payload_sync = FALSE;
				conn->peer->no_payload_sync = TRUE;
				http_client_request_retry(req, response->status, response->reason);
				return;								
			} else if (response->status / 100 == 3) {
				/* redirect */
				http_client_request_redirect(req, response->status, response->location);
			} else {
				/* response for application */
				if (!http_client_connection_return_response(conn, req, response))
					return;
			}

			finished++;
		}

		/* server closing connection? */
		if (conn->close_indicated) {
			http_client_connection_server_close(&conn);
			return;
		}

		/* get next waiting request */
		if (array_count(&conn->request_wait_list) > 0) {
			req_idx = array_idx(&conn->request_wait_list, 0);
			req = req_idx[0];
			no_payload = (strcmp(req->method, "HEAD") == 0);
		} else {
			req = NULL;
			no_payload = FALSE;
		}
	}

	if (ret <= 0 &&
	    (conn->conn.input->eof || conn->conn.input->stream_errno != 0)) {
		int stream_errno = conn->conn.input->stream_errno;
		http_client_connection_abort_temp_error(&conn,
			HTTP_CLIENT_REQUEST_ERROR_CONNECTION_LOST,
			t_strdup_printf("Connection lost: read(%s) failed: %s",
					i_stream_get_name(conn->conn.input),
					stream_errno != 0 ?
					strerror(stream_errno) : "EOF"));
		return;
	}

	if (ret < 0) {
		http_client_connection_abort_error(&conn,
			HTTP_CLIENT_REQUEST_ERROR_BAD_RESPONSE, error);
		return;
	}

	if (finished > 0) {
		/* room for new requests */
		http_client_peer_handle_requests(conn->peer);
		http_client_connection_check_idle(conn);
	}
}

static int http_client_connection_output(struct http_client_connection *conn)
{
	struct http_client_request *const *req_idx, *req;
	struct ostream *output = conn->conn.output;
	const char *error;
	int ret;

	if ((ret = o_stream_flush(output)) <= 0) {
		if (ret < 0) {
			http_client_connection_abort_temp_error(&conn,
				HTTP_CLIENT_REQUEST_ERROR_CONNECTION_LOST,
				t_strdup_printf("Connection lost: write(%s) failed: %m",
						o_stream_get_name(output)));
		}
		return ret;
	}

	if (array_count(&conn->request_wait_list) > 0 && conn->output_locked) {
		req_idx = array_idx(&conn->request_wait_list, 0);
		req = req_idx[0];

		if (!req->payload_sync || conn->payload_continue) {
			if (http_client_request_send_more(req, &error) < 0) {
				http_client_connection_abort_temp_error(&conn,
					HTTP_CLIENT_REQUEST_ERROR_CONNECTION_LOST,
					t_strdup_printf("Connection lost: %s", error));
				return -1;
			}
			if (!conn->output_locked) {
				/* room for new requests */
				http_client_peer_handle_requests(conn->peer);
				http_client_connection_check_idle(conn);
			}
		}
	}
	return 1;
}

static void 
http_client_connection_ready(struct http_client_connection *conn)
{
	struct stat st;

	conn->connected = TRUE;
	conn->peer->last_connect_failed = FALSE;

	if (conn->client->set.rawlog_dir != NULL &&
		stat(conn->client->set.rawlog_dir, &st) == 0) {
		iostream_rawlog_create(conn->client->set.rawlog_dir,
				       &conn->conn.input, &conn->conn.output);
	}

	conn->http_parser = http_response_parser_init(conn->conn.input);
	o_stream_set_flush_callback(conn->conn.output,
    http_client_connection_output, conn);

	/* we never pipeline before the first response */
	(void)http_client_connection_next_request(conn);
}

#ifdef HTTP_BUILD_SSL
static int http_client_connection_ssl_handshaked(void *context)
{
	struct http_client_connection *conn = context;

	if (!conn->client->set.ssl_verify) {
		/* skip certificate checks */
		http_client_connection_debug(conn, "SSL handshake successful");
		return 0;
	} else if (!ssl_iostream_has_valid_client_cert(conn->ssl_iostream)) {
		if (!ssl_iostream_has_broken_client_cert(conn->ssl_iostream)) {
			http_client_connection_error(conn, "SSL certificate not received");
		} else {
			http_client_connection_error(conn, "Received invalid SSL certificate");
		}
	} else {
		const char *host = http_client_peer_get_hostname(conn->peer);

		i_assert(host != NULL);

		if (ssl_iostream_cert_match_name(conn->ssl_iostream, host) < 0) {
			http_client_connection_error(conn, 
				"SSL certificate doesn't match host name");
		} else {
			http_client_connection_debug(conn, "SSL handshake successful");
			return 0;
		}
	}
	i_stream_close(conn->conn.input);
	return -1;
}

static int 
http_client_connection_ssl_init(struct http_client_connection *conn)
{
	struct ssl_iostream_settings ssl_set;
	const char *source;

	if (conn->peer->ssl_ctx == NULL) {
		http_client_connection_error(conn, "No SSL context");
		return -1;
	}

	memset(&ssl_set, 0, sizeof(ssl_set));
	if (conn->client->set.ssl_verify) {
		ssl_set.verbose_invalid_cert = TRUE;
		ssl_set.verify_remote_cert = TRUE;
		ssl_set.require_valid_cert = TRUE;
	}

	if (conn->client->set.debug)
		http_client_connection_debug(conn, "Starting SSL handshake");

	source = t_strdup_printf
		("connection %s: ", http_client_connection_label(conn));
	if (io_stream_create_ssl(conn->peer->ssl_ctx, source, &ssl_set,
				 &conn->conn.input, &conn->conn.output, &conn->ssl_iostream) < 0) {
		http_client_connection_error(conn, "Couldn't initialize SSL client");
		return -1;
	}
	ssl_iostream_set_handshake_callback(conn->ssl_iostream,
					    http_client_connection_ssl_handshaked, conn);
	if (ssl_iostream_handshake(conn->ssl_iostream) < 0) {
		http_client_connection_error(conn, "SSL handshake failed: %s",
			ssl_iostream_get_last_error(conn->ssl_iostream));
		return -1;
	}

	http_client_connection_ready(conn);
	return 0;
}
#endif

static void 
http_client_connection_connected(struct connection *_conn, bool success)
{
	struct http_client_connection *conn =
		(struct http_client_connection *)_conn;

	if (!success) {
		http_client_connection_error(conn, "Connect failed: %m");
		http_client_peer_connection_failure(conn->peer);

	} else {
		http_client_connection_debug(conn, "Connected");
#ifdef HTTP_BUILD_SSL
		if (conn->peer->addr.ssl) {
			if (http_client_connection_ssl_init(conn) < 0)
				http_client_peer_connection_failure(conn->peer);
			return;
		}
#endif
		http_client_connection_ready(conn);
	}
}

static const struct connection_settings http_client_connection_set = {
	.input_max_size = (size_t)-1,
	.output_max_size = (size_t)-1,
	.client = TRUE
};

static const struct connection_vfuncs http_client_connection_vfuncs = {
	.destroy = http_client_connection_destroy,
	.input = http_client_connection_input,
	.client_connected = http_client_connection_connected
};

struct connection_list *
http_client_connection_list_init(void)
{
	return connection_list_init
		(&http_client_connection_set, &http_client_connection_vfuncs);
}

static int http_client_connection_connect(struct http_client_connection *conn)
{
	if (conn->conn.fd_in == -1) {
		if (connection_client_connect(&conn->conn) < 0) {
			http_client_connection_error(conn, "Could not connect");
			return -1;
		}
	}

	return 0;
}

struct http_client_connection *
http_client_connection_create(struct http_client_peer *peer)
{
	struct http_client_connection *conn;
	static unsigned int id = 0;

	conn = i_new(struct http_client_connection, 1);
	conn->refcount = 1;
	conn->client = peer->client;
	conn->peer = peer;
	i_array_init(&conn->request_wait_list, 16);

	connection_init_client_ip
		(peer->client->conn_list, &conn->conn, &peer->addr.ip, peer->addr.port);

	if (http_client_connection_connect(conn) < 0) {
		http_client_connection_unref(&conn);
		return NULL;
	}

	conn->id = id++;
	array_append(&peer->conns, &conn, 1);

	http_client_connection_debug(conn,
		"Connection created (%d parallel connections exist)",
		array_count(&peer->conns));
	return conn;
}

void http_client_connection_ref(struct http_client_connection *conn)
{
	conn->refcount++;
}

void http_client_connection_unref(struct http_client_connection **_conn)
{
	struct http_client_connection *conn = *_conn;
	struct http_client_connection *const *conn_idx;
	struct http_client_peer *peer = conn->peer;
	struct http_client_request **req;

	i_assert(conn->refcount > 0);

	if (--conn->refcount > 0)
		return;

	http_client_connection_debug(conn, "Connection destroy");

	conn->closing = TRUE;
	conn->connected = FALSE;

#ifdef HTTP_BUILD_SSL
	if (conn->ssl_iostream != NULL)
		ssl_iostream_unref(&conn->ssl_iostream);
#endif

	connection_deinit(&conn->conn);

	/* abort all pending requests */
	array_foreach_modifiable(&conn->request_wait_list, req) {
		http_client_request_error(*req, HTTP_CLIENT_REQUEST_ERROR_ABORTED,
			"Aborting");
		http_client_request_unref(req);
	}
	array_free(&conn->request_wait_list);

	if (conn->to_input != NULL)
		timeout_remove(&conn->to_input);
	if (conn->to_idle != NULL)
		timeout_remove(&conn->to_idle);
	if (conn->to_response != NULL)
		timeout_remove(&conn->to_response);
	
	/* remove this connection from the list */
	array_foreach(&conn->peer->conns, conn_idx) {
		if (*conn_idx == conn) {
			array_delete(&conn->peer->conns,
				array_foreach_idx(&conn->peer->conns, conn_idx), 1);
			break;
		}
	}

	if (conn->http_parser != NULL)
		http_response_parser_deinit(&conn->http_parser);

	i_free(conn);
	*_conn = NULL;

	http_client_peer_connection_lost(peer);
}

void http_client_connection_switch_ioloop(struct http_client_connection *conn)
{
	if (conn->to_input != NULL)
		conn->to_input = io_loop_move_timeout(&conn->to_input);
	if (conn->to_idle != NULL)
		conn->to_idle = io_loop_move_timeout(&conn->to_idle);
	if (conn->to_response != NULL)
		conn->to_response = io_loop_move_timeout(&conn->to_response);
	connection_switch_ioloop(&conn->conn);
}
