/* Copyright (C) 2002 Timo Sirainen */

#include "common.h"
#include "hash.h"
#include "ioloop.h"
#include "network.h"
#include "istream.h"
#include "ostream.h"
#include "auth-connection.h"

#include <unistd.h>
#include <dirent.h>
#include <sys/stat.h>

/* Maximum size for an auth reply. 50kB should be more than enough. */
#define MAX_INBUF_SIZE (1024*50)

#define MAX_OUTBUF_SIZE \
	(sizeof(struct auth_login_request_continue) + \
	 AUTH_LOGIN_MAX_REQUEST_DATA_SIZE)

enum auth_mech available_auth_mechs;

static int auth_reconnect;
static unsigned int request_id_counter;
static struct auth_connection *auth_connections;
static struct timeout *to;

static void auth_connection_destroy(struct auth_connection *conn);
static void auth_input(void *context, int fd, struct io *io);
static void auth_connect_missing(void);

static struct auth_connection *auth_connection_find(const char *path)
{
	struct auth_connection *conn;

	for (conn = auth_connections; conn != NULL; conn = conn->next) {
		if (strcmp(conn->path, path) == 0)
			return conn;
	}

	return NULL;
}

static struct auth_connection *auth_connection_new(const char *path)
{
	struct auth_connection *conn;
        struct auth_login_handshake_input handshake;
	int fd;

	fd = net_connect_unix(path);
	if (fd == -1) {
		i_error("Can't connect to imap-auth at %s: %m", path);
                auth_reconnect = TRUE;
		return NULL;
	}

	/* we depend on auth process - if it's slow, just wait */
        net_set_nonblock(fd, FALSE);

	conn = i_new(struct auth_connection, 1);
	conn->path = i_strdup(path);
	conn->fd = fd;
	conn->io = io_add(fd, IO_READ, auth_input, conn);
	conn->input = i_stream_create_file(fd, default_pool, MAX_INBUF_SIZE,
					   FALSE);
	conn->output = o_stream_create_file(fd, default_pool, MAX_OUTBUF_SIZE,
					    IO_PRIORITY_DEFAULT, FALSE);
	conn->requests = hash_create(default_pool, default_pool, 100,
				     NULL, NULL);

	conn->next = auth_connections;
	auth_connections = conn;

	/* send our handshake */
	memset(&handshake, 0, sizeof(handshake));
	handshake.pid = login_process_uid;
	if (o_stream_send(conn->output, &handshake, sizeof(handshake)) < 0) {
                auth_connection_destroy(conn);
		return NULL;
	}
	return conn;
}

static void request_destroy(struct auth_request *request)
{
	hash_remove(request->conn->requests, POINTER_CAST(request->id));
	i_free(request);
}

static void request_hash_destroy(void *key __attr_unused__, void *value,
				 void *context __attr_unused__)
{
	struct auth_request *request = value;

	request->callback(request, NULL, NULL, request->context);
	request_destroy(request);
}

static void auth_connection_destroy(struct auth_connection *conn)
{
	struct auth_connection **pos;

	for (pos = &auth_connections; *pos != NULL; pos = &(*pos)->next) {
		if (*pos == conn) {
			*pos = conn->next;
			break;
		}
	}

	hash_foreach(conn->requests, request_hash_destroy, NULL);
	hash_destroy(conn->requests);

	if (close(conn->fd) < 0)
		i_error("close(imap-auth) failed: %m");
	io_remove(conn->io);
	i_stream_unref(conn->input);
	o_stream_unref(conn->output);
	i_free(conn->path);
	i_free(conn);
}

static struct auth_connection *
auth_connection_get(enum auth_mech mech, size_t size, const char **error)
{
	struct auth_connection *conn;
	int found;

	found = FALSE;
	for (conn = auth_connections; conn != NULL; conn = conn->next) {
		if ((conn->available_auth_mechs & mech)) {
			if (o_stream_have_space(conn->output, size) > 0)
				return conn;

			found = TRUE;
		}
	}

	if (!found) {
		if ((available_auth_mechs & mech) == 0)
			*error = "Unsupported authentication mechanism";
		else {
			*error = "Authentication server isn't connected, "
				"try again later..";
			auth_reconnect = TRUE;
		}
	} else {
		*error = "Authentication servers are busy, wait..";
		i_warning("Authentication servers are busy");
	}

	return NULL;
}

static void update_available_auth_mechs(void)
{
	struct auth_connection *conn;

        available_auth_mechs = 0;
	for (conn = auth_connections; conn != NULL; conn = conn->next)
                available_auth_mechs |= conn->available_auth_mechs;
}

static void auth_handle_handshake(struct auth_connection *conn,
				  struct auth_login_handshake_output *handshake)
{
	conn->pid = handshake->pid;
	conn->available_auth_mechs = handshake->auth_mechanisms;
	conn->handshake_received = TRUE;

	update_available_auth_mechs();
}

static void auth_handle_reply(struct auth_connection *conn,
			      struct auth_login_reply *reply,
			      const unsigned char *data)
{
	struct auth_request *request;

	request = hash_lookup(conn->requests, POINTER_CAST(reply->id));
	if (request == NULL) {
		i_error("BUG: imap-auth sent us reply with unknown ID %u",
			reply->id);
		return;
	}

	request->callback(request, reply, data, request->context);

	if (reply->result != AUTH_LOGIN_RESULT_CONTINUE)
		request_destroy(request);
}

static void auth_input(void *context, int fd __attr_unused__,
		       struct io *io __attr_unused__)
{
	struct auth_connection *conn = context;
        struct auth_login_handshake_output handshake;
	const unsigned char *data;
	size_t size;

	switch (i_stream_read(conn->input)) {
	case 0:
		return;
	case -1:
		/* disconnected */
                auth_reconnect = TRUE;
		auth_connection_destroy(conn);
		return;
	case -2:
		/* buffer full - can't happen unless imap-auth is buggy */
		i_error("BUG: imap-auth sent us more than %d bytes of data",
			MAX_INBUF_SIZE);
		auth_connection_destroy(conn);
		return;
	}

	if (!conn->handshake_received) {
		data = i_stream_get_data(conn->input, &size);
		if (size == sizeof(handshake)) {
			memcpy(&handshake, data, sizeof(handshake));
			i_stream_skip(conn->input, sizeof(handshake));

			auth_handle_handshake(conn, &handshake);
		} else if (size > sizeof(handshake)) {
			i_error("BUG: imap-auth sent us too large handshake "
				"(%"PRIuSIZE_T " vs %"PRIuSIZE_T")", size,
				sizeof(handshake));
			auth_connection_destroy(conn);
		}
		return;
	}

	if (!conn->reply_received) {
		data = i_stream_get_data(conn->input, &size);
		if (size < sizeof(conn->reply))
			return;

		memcpy(&conn->reply, data, sizeof(conn->reply));
		i_stream_skip(conn->input, sizeof(conn->reply));
		conn->reply_received = TRUE;
	}

	data = i_stream_get_data(conn->input, &size);
	if (size < conn->reply.data_size)
		return;

	/* we've got a full reply */
	conn->reply_received = FALSE;
	auth_handle_reply(conn, &conn->reply, data);
	i_stream_skip(conn->input, conn->reply.data_size);
}

int auth_init_request(enum auth_mech mech, auth_callback_t callback,
		      void *context, const char **error)
{
	struct auth_connection *conn;
	struct auth_request *request;
	struct auth_login_request_new auth_request;

	if (auth_reconnect)
		auth_connect_missing();

	conn = auth_connection_get(mech, sizeof(auth_request), error);
	if (conn == NULL)
		return FALSE;

	/* create internal request structure */
	request = i_new(struct auth_request, 1);
	request->mech = mech;
	request->conn = conn;
	request->id = ++request_id_counter;
	if (request->id == 0) {
		/* wrapped - ID 0 not allowed */
		request->id = ++request_id_counter;
	}
	request->callback = callback;
	request->context = context;

	hash_insert(conn->requests, POINTER_CAST(request->id), request);

	/* send request to auth */
	auth_request.type = AUTH_LOGIN_REQUEST_NEW;
	auth_request.mech = request->mech;
	auth_request.id = request->id;
	if (o_stream_send(request->conn->output, &auth_request,
			  sizeof(auth_request)) < 0)
		auth_connection_destroy(request->conn);
	return TRUE;
}

void auth_continue_request(struct auth_request *request,
			   const unsigned char *data, size_t data_size)
{
	struct auth_login_request_continue auth_request;

	/* send continued request to auth */
	auth_request.type = AUTH_LOGIN_REQUEST_CONTINUE;
	auth_request.id = request->id;
	auth_request.data_size = data_size;

	if (o_stream_send(request->conn->output, &auth_request,
			  sizeof(auth_request)) < 0)
		auth_connection_destroy(request->conn);
	else if (o_stream_send(request->conn->output, data, data_size) < 0)
		auth_connection_destroy(request->conn);
}

void auth_abort_request(struct auth_request *request)
{
        request_destroy(request);
}

static void auth_connect_missing(void)
{
	DIR *dirp;
	struct dirent *dp;
	struct stat st;

	auth_reconnect = TRUE;

	/* we're chrooted into */
	dirp = opendir(".");
	if (dirp == NULL) {
		i_error("opendir(\".\") failed when trying to get list of "
			"authentication servers: %m");
		return;
	}

	while ((dp = readdir(dirp)) != NULL) {
		if (dp->d_name[0] == '.')
			continue;

		if (auth_connection_find(dp->d_name) != NULL) {
			/* already connected */
			continue;
		}

		if (stat(dp->d_name, &st) == 0 && S_ISSOCK(st.st_mode)) {
			if (auth_connection_new(dp->d_name) != NULL)
				auth_reconnect = FALSE;
		}
	}

	(void)closedir(dirp);
}

static void
auth_connect_missing_timeout(void *context __attr_unused__,
			     struct timeout *timeout __attr_unused__)
{
	if (auth_reconnect)
                auth_connect_missing();
}

void auth_connection_init(void)
{
	auth_connections = NULL;
	request_id_counter = 0;
        auth_reconnect = FALSE;

	auth_connect_missing();
	to = timeout_add(1000, auth_connect_missing_timeout, NULL);
}

void auth_connection_deinit(void)
{
	struct auth_connection *next;

	while (auth_connections != NULL) {
		next = auth_connections->next;
		auth_connection_destroy(auth_connections);
		auth_connections = next;
	}

	timeout_remove(to);
}
