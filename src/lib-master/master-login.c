/* Copyright (c) 2009-2010 Dovecot authors, see the included COPYING file */

#include "lib.h"
#include "ioloop.h"
#include "ostream.h"
#include "fdpass.h"
#include "fd-close-on-exec.h"
#include "llist.h"
#include "str.h"
#include "strescape.h"
#include "master-service-private.h"
#include "master-login.h"
#include "master-login-auth.h"

#include <sys/stat.h>
#include <unistd.h>

#define MASTER_LOGIN_POSTLOGIN_TIMEOUT_MSECS (60*1000)

struct master_login_connection {
	struct master_login_connection *prev, *next;

	struct master_login *login;
	int fd;
	struct io *io;
	struct ostream *output;
};

struct master_login_postlogin {
	struct master_login_client *client;

	int fd;
	struct io *io;
	struct timeout *to;
	string_t *input;
};

struct master_login {
	struct master_service *service;
	master_login_callback_t *callback;
	struct master_login_connection *conns;
	struct master_login_auth *auth;
	char *postlogin_socket_path;

	unsigned int stopping:1;
};

static void master_login_conn_deinit(struct master_login_connection **_conn);

struct master_login *
master_login_init(struct master_service *service, const char *auth_socket_path,
		  const char *postlogin_socket_path,
		  master_login_callback_t *callback)
{
	struct master_login *login;

	login = i_new(struct master_login, 1);
	login->service = service;
	login->callback = callback;
	login->auth = master_login_auth_init(auth_socket_path);
	login->postlogin_socket_path = i_strdup(postlogin_socket_path);

	i_assert(service->login == NULL);
	service->login = login;
	return login;
}

void master_login_deinit(struct master_login **_login)
{
	struct master_login *login = *_login;

	*_login = NULL;

	i_assert(login->service->login == login);
	login->service->login = NULL;

	master_login_auth_deinit(&login->auth);
	while (login->conns != NULL) {
		struct master_login_connection *conn = login->conns;

		master_login_conn_deinit(&conn);
	}
	i_free(login->postlogin_socket_path);
	i_free(login);
}

static int
master_login_conn_read_request(struct master_login_connection *conn,
			       struct master_auth_request *req_r,
			       unsigned char data[MASTER_AUTH_MAX_DATA_SIZE],
			       int *client_fd_r)
{
	struct stat st;
	ssize_t ret;

	*client_fd_r = -1;

	ret = fd_read(conn->fd, req_r, sizeof(*req_r), client_fd_r);
	if (ret != sizeof(*req_r)) {
		if (ret == 0) {
			/* disconnected */
		} else if (ret > 0) {
			/* request wasn't fully read */
			i_error("fd_read() partial input (%d/%d)",
				(int)ret, (int)sizeof(*req_r));
		} else {
			if (errno == EAGAIN)
				return 0;

			i_error("fd_read() failed: %m");
		}
		return -1;
	}

	if (req_r->data_size != 0) {
		if (req_r->data_size > MASTER_AUTH_MAX_DATA_SIZE) {
			i_error("Too large auth data_size sent");
			return -1;
		}
		/* @UNSAFE */
		ret = read(conn->fd, data, req_r->data_size);
		if (ret != (ssize_t)req_r->data_size) {
			if (ret == 0) {
				/* disconnected */
			} else if (ret > 0) {
				/* request wasn't fully read */
				i_error("Data read partially %d/%u",
					(int)ret, req_r->data_size);
			} else {
				i_error("read(data) failed: %m");
			}
			return -1;
		}
	}

	if (*client_fd_r == -1) {
		i_error("Auth request missing a file descriptor");
		return -1;
	}

	if (fstat(*client_fd_r, &st) < 0) {
		i_error("fstat(fd_read client) failed: %m");
		return -1;
	}
	if (st.st_ino != req_r->ino) {
		i_error("Auth request inode mismatch: %s != %s",
			dec2str(st.st_ino), dec2str(req_r->ino));
		return -1;
	}
	return 1;
}

static void master_login_auth_finish(struct master_login_client *client,
				     const char *const *auth_args)
{
	struct master_login_connection *conn = client->conn;
	struct master_login *login = conn->login;
	struct master_service *service = login->service;
	bool close_sockets;

	close_sockets = service->master_status.available_count == 0 &&
		service->service_count_left == 1;

	login->callback(client, auth_args[0], auth_args+1);
	i_free(client);

	if (close_sockets) {
		/* we're dying as soon as this connection closes. */
		i_assert(master_login_auth_request_count(login->auth) == 0);
		master_login_auth_disconnect(login->auth);

		master_service_close_config_fd(service);
		master_login_conn_deinit(&conn);
	} else if (login->stopping) {
		/* try stopping again */
		master_login_stop(login);
	}
}

static void master_login_client_free(struct master_login_client *client)
{
	if (close(client->fd) < 0)
		i_error("close(fd_read client) failed: %m");
	i_free(client);
}

static void master_login_postlogin_free(struct master_login_postlogin *pl)
{
	timeout_remove(&pl->to);
	io_remove(&pl->io);
	if (close(pl->fd) < 0)
		i_error("close(postlogin) failed: %m");
	str_free(&pl->input);
	i_free(pl);
}

static void master_login_postlogin_input(struct master_login_postlogin *pl)
{
	struct master_login *login = pl->client->conn->login;
	char buf[1024];
	const char **auth_args, **p;
	unsigned int len;
	ssize_t ret;
	int fd = -1;

	while ((ret = fd_read(pl->fd, buf, sizeof(buf), &fd)) > 0) {
		if (fd != -1) {
			/* post-login script replaced fd */
			if (close(pl->client->fd) < 0)
				i_error("close(client) failed: %m");
			pl->client->fd = fd;
		}
		str_append_n(pl->input, buf, ret);
	}

	len = str_len(pl->input);
	if (len > 0 && str_c(pl->input)[len-1] == '\n') {
		/* finished reading the input */
		str_truncate(pl->input, len-1);
	} else {
		if (ret < 0) {
			if (errno == EAGAIN)
				return;

			i_error("fd_read(%s) failed: %m",
				login->postlogin_socket_path);
		} else {
			i_error("fd_read(%s) failed: disconnected",
				login->postlogin_socket_path);
		}
		master_login_client_free(pl->client);
		master_login_postlogin_free(pl);
		master_service_client_connection_destroyed(login->service);
		return;
	}

	auth_args = t_strsplit(str_c(pl->input), "\t");
	for (p = auth_args; *p != NULL; p++)
		*p = str_tabunescape(t_strdup_noconst(*p));

	master_login_auth_finish(pl->client, auth_args);
	master_login_postlogin_free(pl);
}

static void master_login_postlogin_timeout(struct master_login_postlogin *pl)
{
	struct master_login *login = pl->client->conn->login;

	i_error("%s: Timeout waiting for post-login script to finish, aborting",
		login->postlogin_socket_path);

	master_login_client_free(pl->client);
	master_login_postlogin_free(pl);
	master_service_client_connection_destroyed(login->service);
}

static int master_login_postlogin(struct master_login_client *client,
				  const char *const *auth_args)
{
	struct master_login *login = client->conn->login;
	struct master_login_postlogin *pl;
	string_t *str;
	unsigned int i;
	int fd;
	ssize_t ret;

	fd = net_connect_unix_with_retries(login->postlogin_socket_path, 1000);
	if (fd == -1) {
		i_error("net_connect_unix(%s) failed: %m",
			login->postlogin_socket_path);
		return -1;
	}

	str = t_str_new(256);
	str_printfa(str, "%s\t%s", net_ip2addr(&client->auth_req.local_ip),
		    net_ip2addr(&client->auth_req.remote_ip));
	for (i = 0; auth_args[i] != NULL; i++) {
		str_append_c(str, '\t');
		str_tabescape_write(str, auth_args[i]);
	}
	str_append_c(str, '\n');
	ret = fd_send(fd, client->fd, str_data(str), str_len(str));
	if (ret != (ssize_t)str_len(str)) {
		if (ret < 0) {
			i_error("write(%s) failed: %m",
				login->postlogin_socket_path);
		} else {
			i_error("write(%s) failed: partial write",
				login->postlogin_socket_path);
		}
		(void)close(fd);
		return -1;
	}
	net_set_nonblock(fd, TRUE);

	pl = i_new(struct master_login_postlogin, 1);
	pl->client = client;
	pl->fd = fd;
	pl->io = io_add(fd, IO_READ, master_login_postlogin_input, pl);
	pl->to = timeout_add(MASTER_LOGIN_POSTLOGIN_TIMEOUT_MSECS,
			     master_login_postlogin_timeout, pl);
	pl->input = str_new(default_pool, 512);
	return 0;
}

static void
master_login_auth_callback(const char *const *auth_args, void *context)
{
	struct master_login_client *client = context;
	struct master_auth_reply reply;
	struct master_service *service = client->conn->login->service;

	memset(&reply, 0, sizeof(reply));
	reply.tag = client->auth_req.tag;
	reply.status = auth_args != NULL ? MASTER_AUTH_STATUS_OK :
		MASTER_AUTH_STATUS_INTERNAL_ERROR;
	reply.mail_pid = getpid();
	o_stream_send(client->conn->output, &reply, sizeof(reply));

	if (auth_args == NULL || auth_args[0] == NULL) {
		if (auth_args != NULL)
			i_error("login client: Username missing from auth reply");
		master_login_client_free(client);
		return;
	}

	i_assert(service->master_status.available_count > 0);
	service->master_status.available_count--;
	master_status_update(service);

	if (client->conn->login->postlogin_socket_path == NULL)
		master_login_auth_finish(client, auth_args);
	else {
		/* execute post-login scripts before finishing auth */
		if (master_login_postlogin(client, auth_args) < 0) {
			master_login_client_free(client);
			master_service_client_connection_destroyed(service);
		}
	}
}

static void master_login_conn_input(struct master_login_connection *conn)
{
	struct master_auth_request req;
	struct master_login_client *client;
	struct master_login *login = conn->login;
	unsigned char data[MASTER_AUTH_MAX_DATA_SIZE];
	int ret, client_fd;

	ret = master_login_conn_read_request(conn, &req, data, &client_fd);
	if (ret <= 0) {
		if (ret < 0)
			master_login_conn_deinit(&conn);
		if (client_fd != -1) {
			if (close(client_fd) < 0)
				i_error("close(fd_read client) failed: %m");
		}
		return;
	}
	fd_close_on_exec(client_fd, TRUE);

	/* @UNSAFE: we have a request. do userdb lookup for it. */
	client = i_malloc(sizeof(struct master_login_client) + req.data_size);
	client->conn = conn;
	client->fd = client_fd;
	client->auth_req = req;
	memcpy(client->data, data, req.data_size);

	master_login_auth_request(login->auth, &req,
				  master_login_auth_callback, client);
}

void master_login_add(struct master_login *login, int fd)
{
	struct master_login_connection *conn;

	conn = i_new(struct master_login_connection, 1);
	conn->login = login;
	conn->fd = fd;
	conn->io = io_add(conn->fd, IO_READ, master_login_conn_input, conn);
	conn->output = o_stream_create_fd(fd, (size_t)-1, FALSE);

	DLLIST_PREPEND(&login->conns, conn);

	/* FIXME: currently there's a separate connection for each request.
	   and currently we don't try to accept more connections until this
	   request's authentication is finished, because updating
	   available_count gets tricky. */
	master_service_io_listeners_remove(login->service);
}

static void master_login_conn_deinit(struct master_login_connection **_conn)
{
	struct master_login_connection *conn = *_conn;

	*_conn = NULL;

	DLLIST_REMOVE(&conn->login->conns, conn);

	if (conn->io != NULL)
		io_remove(&conn->io);
	o_stream_unref(&conn->output);
	if (close(conn->fd) < 0)
		i_error("close(master login) failed: %m");
	master_service_io_listeners_add(conn->login->service);
	i_free(conn);
}

void master_login_stop(struct master_login *login)
{
	login->stopping = TRUE;
	if (master_login_auth_request_count(login->auth) == 0) {
		master_login_auth_disconnect(login->auth);
		master_service_close_config_fd(login->service);
	}
}
