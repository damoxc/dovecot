/* Copyright (c) 2005-2009 Dovecot authors, see the included COPYING file */

#include "common.h"
#include "ioloop.h"
#include "array.h"
#include "aqueue.h"
#include "network.h"
#include "istream.h"
#include "ostream.h"
#include "auth-request.h"
#include "auth-worker-client.h"
#include "auth-worker-server.h"

#include <stdlib.h>
#include <unistd.h>

#define AUTH_WORKER_LOOKUP_TIMEOUT_SECS 60
#define AUTH_WORKER_MAX_IDLE_SECS (60*30)
#define AUTH_WORKER_DELAY_WARN_SECS 3
#define AUTH_WORKER_DELAY_WARN_MIN_INTERVAL_SECS 300

struct auth_worker_request {
	unsigned int id;
	time_t created;
	const char *data_str;
	struct auth_request *auth_request;
        auth_worker_callback_t *callback;
};

struct auth_worker_connection {
	int fd;

	struct io *io;
	struct istream *input;
	struct ostream *output;
	struct timeout *to;

	struct auth_worker_request *request;
	unsigned int id_counter;

	unsigned int shutdown:1;
};

static ARRAY_DEFINE(connections, struct auth_worker_connection *) = ARRAY_INIT;
static unsigned int idle_count;
static unsigned int auth_workers_max;

static ARRAY_DEFINE(worker_request_array, struct auth_worker_request *);
static struct aqueue *worker_request_queue;
static time_t auth_worker_last_warn;

static char *worker_socket_path;

static void worker_input(struct auth_worker_connection *conn);
static void auth_worker_destroy(struct auth_worker_connection **conn,
				const char *reason, bool restart);

static void auth_worker_idle_timeout(struct auth_worker_connection *conn)
{
	i_assert(conn->request == NULL);

	if (idle_count > 1)
		auth_worker_destroy(&conn, NULL, FALSE);
	else
		timeout_reset(conn->to);
}

static void auth_worker_call_timeout(struct auth_worker_connection *conn)
{
	i_assert(conn->request != NULL);

	auth_worker_destroy(&conn, "Lookup timed out", TRUE);
}

static void auth_worker_request_send(struct auth_worker_connection *conn,
				     struct auth_worker_request *request)
{
	struct const_iovec iov[3];

	if (ioloop_time - request->created > AUTH_WORKER_DELAY_WARN_SECS &&
	    ioloop_time - auth_worker_last_warn >
	    AUTH_WORKER_DELAY_WARN_MIN_INTERVAL_SECS) {
		auth_worker_last_warn = ioloop_time;
		i_warning("auth workers: Auth request was queued for %d "
			  "seconds, %d left in queue",
			  (int)(ioloop_time - request->created),
			  aqueue_count(worker_request_queue));
	}

	request->id = ++conn->id_counter;

	iov[0].iov_base = t_strdup_printf("%d\t", request->id);
	iov[0].iov_len = strlen(iov[0].iov_base);
	iov[1].iov_base = request->data_str;
	iov[1].iov_len = strlen(request->data_str);
	iov[2].iov_base = "\n";
	iov[2].iov_len = 1;

	o_stream_sendv(conn->output, iov, 3);

	conn->request = request;

	timeout_remove(&conn->to);
	conn->to = timeout_add(AUTH_WORKER_LOOKUP_TIMEOUT_SECS * 1000,
			       auth_worker_call_timeout, conn);
	idle_count--;
}

static void auth_worker_request_send_next(struct auth_worker_connection *conn)
{
	struct auth_worker_request *request, *const *requestp;

	if (aqueue_count(worker_request_queue) == 0)
		return;

	requestp = array_idx(&worker_request_array,
			     aqueue_idx(worker_request_queue, 0));
	request = *requestp;
	aqueue_delete_tail(worker_request_queue);
	auth_worker_request_send(conn, request);
}

static struct auth_worker_connection *auth_worker_create(void)
{
	struct auth_worker_connection *conn;
	int fd, try;

	if (array_count(&connections) >= auth_workers_max)
		return NULL;

	for (try = 0;; try++) {
		fd = net_connect_unix(worker_socket_path);
		if (fd >= 0)
			break;

		if (errno == EAGAIN || errno == ECONNREFUSED) {
			/* we're busy. */
		} else if (errno == ENOENT) {
			/* master didn't yet create it? */
		} else {
			i_fatal("net_connect_unix(%s) failed: %m",
				worker_socket_path);
		}

		if (try == 50) {
			i_error("net_connect_unix(%s) "
				"failed after %d secs: %m",
				worker_socket_path, try/10);
			return NULL;
		}

		/* wait and try again */
		usleep(100000);
	}

	conn = i_new(struct auth_worker_connection, 1);
	conn->fd = fd;
	conn->input = i_stream_create_fd(fd, AUTH_WORKER_MAX_LINE_LENGTH,
					 FALSE);
	conn->output = o_stream_create_fd(fd, (size_t)-1, FALSE);
	conn->io = io_add(fd, IO_READ, worker_input, conn);
	conn->to = timeout_add(AUTH_WORKER_MAX_IDLE_SECS * 1000,
			       auth_worker_idle_timeout, conn);

	idle_count++;

	array_append(&connections, &conn, 1);
	return conn;
}

static void auth_worker_destroy(struct auth_worker_connection **_conn,
				const char *reason, bool restart)
{
	struct auth_worker_connection *conn = *_conn;
	struct auth_worker_connection **connp;
	unsigned int i, count;

	*_conn = NULL;

	connp = array_get_modifiable(&connections, &count);
	for (i = 0; i < count; i++) {
		if (connp[i] == conn) {
			array_delete(&connections, i, 1);
			break;
		}
	}

	if (conn->request == NULL)
		idle_count--;

	if (conn->request != NULL) T_BEGIN {
		struct auth_request *auth_request = conn->request->auth_request;

		auth_request_log_error(auth_request, "worker-server",
				       "Aborted: %s", reason);
		conn->request->callback(auth_request, t_strdup_printf(
				"FAIL\t%d", PASSDB_RESULT_INTERNAL_FAILURE));
		auth_request_unref(&conn->request->auth_request);
	} T_END;

	io_remove(&conn->io);
	i_stream_destroy(&conn->input);
	o_stream_destroy(&conn->output);
	timeout_remove(&conn->to);

	if (close(conn->fd) < 0)
		i_error("close(auth worker) failed: %m");
	i_free(conn);

	if (idle_count == 0 && restart) {
		conn = auth_worker_create();
		if (conn != NULL)
			auth_worker_request_send_next(conn);
	}
}

static struct auth_worker_connection *auth_worker_find_free(void)
{
	struct auth_worker_connection **conns;
	unsigned int i, count;

	if (idle_count == 0)
		return NULL;

	conns = array_get_modifiable(&connections, &count);
	for (i = 0; i < count; i++) {
		if (conns[i]->request == NULL)
			return conns[i];
	}
	i_unreached();
	return NULL;
}

static void auth_worker_request_handle(struct auth_worker_connection *conn,
				       struct auth_worker_request *request,
				       const char *line)
{
	conn->request = NULL;
	timeout_remove(&conn->to);
	conn->to = timeout_add(AUTH_WORKER_MAX_IDLE_SECS * 1000,
			       auth_worker_idle_timeout, conn);
	idle_count++;

	request->callback(request->auth_request, line);
	auth_request_unref(&request->auth_request);
}

static void worker_input(struct auth_worker_connection *conn)
{
	const char *line, *id_str;
	unsigned int id;

	switch (i_stream_read(conn->input)) {
	case 0:
		return;
	case -1:
		/* disconnected */
		auth_worker_destroy(&conn, "Worker process died unexpectedly",
				    TRUE);
		return;
	case -2:
		/* buffer full */
		i_error("BUG: Auth worker sent us more than %d bytes",
			(int)AUTH_WORKER_MAX_LINE_LENGTH);
		auth_worker_destroy(&conn, "Worker is buggy", TRUE);
		return;
	}

	while ((line = i_stream_next_line(conn->input)) != NULL) {
		if (strcmp(line, "SHUTDOWN") == 0) {
			conn->shutdown = TRUE;
			continue;
		}
		id_str = line;
		line = strchr(line, '\t');
		if (line == NULL)
			continue;

		id = (unsigned int)strtoul(t_strcut(id_str, '\t'),
					   NULL, 10);
		if (conn->request != NULL && id == conn->request->id) {
			auth_worker_request_handle(conn, conn->request,
						   line + 1);
		} else {
			if (conn->request != NULL) {
				i_error("BUG: Worker sent reply with id %u, "
					"expected %u", id, conn->request->id);
			} else {
				i_error("BUG: Worker sent reply with id %u, "
					"none was expected", id);
			}
			auth_worker_destroy(&conn, "Worker is buggy", TRUE);
			return;
		}
	}

	if (conn->shutdown && conn->request == NULL)
		auth_worker_destroy(&conn, "Max requests limit", TRUE);
	else
		auth_worker_request_send_next(conn);
}

void auth_worker_call(struct auth_request *auth_request,
		      struct auth_stream_reply *data,
		      auth_worker_callback_t *callback)
{
	struct auth_worker_connection *conn;
	struct auth_worker_request *request;

	request = p_new(auth_request->pool, struct auth_worker_request, 1);
	request->created = ioloop_time;
	request->data_str = p_strdup(auth_request->pool,
				     auth_stream_reply_export(data));
	request->auth_request = auth_request;
	request->callback = callback;
	auth_request_ref(auth_request);

	if (aqueue_count(worker_request_queue) > 0) {
		/* requests are already being queued, no chance of
		   finding/creating a worker */
		conn = NULL;
	} else {
		conn = auth_worker_find_free();
		if (conn == NULL) {
			/* no free connections, create a new one */
			conn = auth_worker_create();
		}
	}
	if (conn != NULL)
		auth_worker_request_send(conn, request);
	else {
		/* reached the limit, queue the request */
		aqueue_append(worker_request_queue, &request);
	}
}

void auth_worker_server_init(void)
{
	const char *env;

	if (array_is_created(&connections)) {
		/* already initialized */
		return;
	}

	env = getenv("AUTH_WORKER_PATH");
	if (env == NULL)
		i_fatal("AUTH_WORKER_PATH environment not set");
	worker_socket_path = i_strdup(env);

	env = getenv("AUTH_WORKER_MAX_COUNT");
	if (env == NULL)
		i_fatal("AUTH_WORKER_MAX_COUNT environment not set");
	auth_workers_max = atoi(env);

	i_array_init(&worker_request_array, 128);
	worker_request_queue = aqueue_init(&worker_request_array.arr);

	i_array_init(&connections, 16);
	(void)auth_worker_create();
}

void auth_worker_server_deinit(void)
{
	struct auth_worker_connection **connp, *conn;

	if (!array_is_created(&connections))
		return;

	while (array_count(&connections) > 0) {
		connp = array_idx_modifiable(&connections, 0);
		conn = *connp;
		auth_worker_destroy(&conn, "Shutting down", FALSE);
	}
	array_free(&connections);

	aqueue_deinit(&worker_request_queue);
	array_free(&worker_request_array);
	i_free(worker_socket_path);
}
