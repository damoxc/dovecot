/* Copyright (c) 2013 Dovecot authors, see the included COPYING file */

#include "imap-urlauth-common.h"
#include "array.h"
#include "ioloop.h"
#include "net.h"
#include "fdpass.h"
#include "istream.h"
#include "ostream.h"
#include "str.h"
#include "strescape.h"
#include "eacces-error.h"
#include "llist.h"
#include "hostpid.h"
#include "execv-const.h"
#include "env-util.h"
#include "var-expand.h"
#include "restrict-access.h"
#include "master-service.h"
#include "master-interface.h"

#include <stdlib.h>
#include <unistd.h>
#include <sys/wait.h>

#define IMAP_URLAUTH_PROTOCOL_MAJOR_VERSION 1
#define IMAP_URLAUTH_PROTOCOL_MINOR_VERSION 0

#define IMAP_URLAUTH_WORKER_SOCKET "imap-urlauth-worker"

/* max. length of input lines (URLs) */
#define MAX_INBUF_SIZE 2048

/* Disconnect client after idling this many milliseconds */
#define CLIENT_IDLE_TIMEOUT_MSECS (10*60*1000)

#define USER_EXECUTABLE "imap-urlauth-worker"

#define IS_STANDALONE() \
        (getenv(MASTER_IS_PARENT_ENV) == NULL)

struct client *imap_urlauth_clients;
unsigned int imap_urlauth_client_count;

static int client_worker_connect(struct client *client);
static void client_worker_disconnect(struct client *client);
static void client_worker_input(struct client *client);

int client_create(const char *username, int fd_in, int fd_out,
		  const struct imap_urlauth_settings *set,
		  struct client **client_r)
{
	struct client *client;
	const char *app;

	/* always use nonblocking I/O */
	net_set_nonblock(fd_in, TRUE);
	net_set_nonblock(fd_out, TRUE);

	client = i_new(struct client, 1);
	client->fd_in = fd_in;
	client->fd_out = fd_out;
	client->fd_ctrl = -1;
	client->set = set;

	if (client_worker_connect(client) < 0) {
		i_free(client);
		return -1;
	}

	/* determine user's special privileges */
	i_array_init(&client->access_apps, 4);
	if (username != NULL) {
		if (set->imap_urlauth_submit_user != NULL &&
		    strcmp(set->imap_urlauth_submit_user, username) == 0) {
			if (set->mail_debug)
				i_debug("User %s has URLAUTH submit access", username);
			app = "submit+";
			array_append(&client->access_apps, &app, 1);
		}
		if (set->imap_urlauth_stream_user != NULL &&
		    strcmp(set->imap_urlauth_stream_user, username) == 0) {
			if (set->mail_debug)
				i_debug("User %s has URLAUTH stream access", username);
			app = "stream";
			array_append(&client->access_apps, &app, 1);
		}
	}

	if (username != NULL)
		client->username = i_strdup(username);

	client->output = o_stream_create_fd(fd_out, (size_t)-1, FALSE);

	imap_urlauth_client_count++;
	DLLIST_PREPEND(&imap_urlauth_clients, client);

	imap_urlauth_refresh_proctitle();
	*client_r = client;
	return 0;
}

void client_send_line(struct client *client, const char *fmt, ...)
{
	va_list va;
	ssize_t ret;

	if (client->output->closed)
		return;

	va_start(va, fmt);

	T_BEGIN {
		string_t *str;

		str = t_str_new(256);
		str_vprintfa(str, fmt, va);
		str_append(str, "\n");

		ret = o_stream_send(client->output,
				    str_data(str), str_len(str));
		i_assert(ret < 0 || (size_t)ret == str_len(str));
	} T_END;

	va_end(va);
}

static int client_worker_connect(struct client *client)
{
	static const char handshake[] = "VERSION\timap-urlauth-worker\t1\t0\n";
	const char *socket_path;
	ssize_t ret;
	unsigned char data;

	socket_path = t_strconcat(client->set->base_dir,
				  "/"IMAP_URLAUTH_WORKER_SOCKET, NULL);

	if (client->set->mail_debug)
		i_debug("Connecting to worker socket %s", socket_path);

	client->fd_ctrl = net_connect_unix_with_retries(socket_path, 1000);
	if (client->fd_ctrl < 0) {
		if (errno == EACCES) {
			i_error("imap-urlauth-client: %s",
				eacces_error_get("net_connect_unix",
						 socket_path));
		} else {
			i_error("imap-urlauth-client: net_connect_unix(%s) failed: %m",
				socket_path);
		}
		return -1;
	}

	/* transfer one or two fds */
	data = (client->fd_in == client->fd_out ? '0' : '1');
	ret = fd_send(client->fd_ctrl, client->fd_in, &data, sizeof(data));
	if (ret > 0 && client->fd_in != client->fd_out) {
		data = '0';
		ret = fd_send(client->fd_ctrl, client->fd_out,
			      &data, sizeof(data));
	}

	if (ret <= 0) {
		if (ret < 0) {
			i_error("fd_send(%s, %d) failed: %m",
				socket_path, client->fd_ctrl);
		} else {
			i_error("fd_send(%s, %d) failed to send byte",
				socket_path, client->fd_ctrl);
		}
		client_worker_disconnect(client);
		return -1;
	}

	client->ctrl_output =
		o_stream_create_fd(client->fd_ctrl, (size_t)-1, FALSE);

	/* send protocol version handshake */
	if (o_stream_send_str(client->ctrl_output, handshake) < 0) {
		i_error("Error sending handshake to imap-urlauth worker: %m");
		client_worker_disconnect(client);
		return -1;
	}

	client->ctrl_input =
		i_stream_create_fd(client->fd_ctrl, MAX_INBUF_SIZE, FALSE);
	client->ctrl_io =
		io_add(client->fd_ctrl, IO_READ, client_worker_input, client);  
	return 0;
}

void client_worker_disconnect(struct client *client)
{
	client->worker_state = IMAP_URLAUTH_WORKER_STATE_INACTIVE;

	if (client->ctrl_io != NULL)
		io_remove(&client->ctrl_io);
	if (client->ctrl_output != NULL)
		o_stream_destroy(&client->ctrl_output);
	if (client->ctrl_input != NULL)
		i_stream_destroy(&client->ctrl_input);
	if (client->fd_ctrl >= 0) {
		net_disconnect(client->fd_ctrl);
		client->fd_ctrl = -1;
	}
}

static int
client_worker_input_line(struct client *client, const char *response)
{
	const char *const *apps;
	unsigned int count, i;
	bool restart;
	string_t *str;
	int ret;

	switch (client->worker_state) {
	case IMAP_URLAUTH_WORKER_STATE_INACTIVE:
		if (strcasecmp(response, "OK") != 0) {
			client_disconnect(client, "Worker handshake failed");
			return -1;
		}
		client->worker_state = IMAP_URLAUTH_WORKER_STATE_CONNECTED;

		str = t_str_new(256);
		str_append(str, "ACCESS\t");
		if (client->username != NULL)
			str_append_tabescaped(str, client->username);
		if (client->set->mail_debug)
			str_append(str, "\tdebug");
		if (array_count(&client->access_apps) > 0) {
			str_append(str, "\tapps=");
			apps = array_get(&client->access_apps, &count);
			str_append(str, apps[0]);
			for (i = 1; i < count; i++) {
				str_append_c(str, ',');
				str_append_tabescaped(str, apps[i]);
			}
		}
		str_append(str, "\n");

		ret = o_stream_send(client->ctrl_output,
				    str_data(str), str_len(str));
		i_assert(ret < 0 || (size_t)ret == str_len(str));
		if (ret < 0) {
			client_disconnect(client,
				"Failed to send ACCESS control command to worker");
			return -1;
		}
		break;

	case IMAP_URLAUTH_WORKER_STATE_CONNECTED:
		if (strcasecmp(response, "OK") != 0) {
			client_disconnect(client,
				"Failed to negotiate access parameters");
			return -1;
		}
		client->worker_state = IMAP_URLAUTH_WORKER_STATE_ACTIVE;
		break;

	case IMAP_URLAUTH_WORKER_STATE_ACTIVE:
		restart = TRUE;
		if (strcasecmp(response, "DISCONNECTED") == 0) {
			/* worker detected client disconnect */
			restart = FALSE;
		} else if (strcasecmp(response, "FINISHED") != 0) {
			/* unknown response */
			client_disconnect(client,
				"Worker finished with unknown response");
			return -1;
		}

		if (client->set->mail_debug)
			i_debug("Worker finished successfully");

		if (restart) {
			/* connect to new worker for accessing different user */
			client_worker_disconnect(client);
			if (client_worker_connect(client) < 0) {
				client_disconnect(client,
					"Failed to connect to new worker");
				return -1;
			}

			/* indicate success of "END" command */
			client_send_line(client, "OK");
		} else {
			client_disconnect(client, "Client disconnected");
		}
		return -1;
 	default:
		i_unreached();
	}
	return 0;
}

void client_worker_input(struct client *client)
{
	struct istream *input = client->ctrl_input;
	const char *line;

	if (input->closed) {
		/* disconnected */
		client_disconnect(client, "Worker disconnected unexpectedly");
		return;
	}

	switch (i_stream_read(input)) {
	case -1:
		/* disconnected */
		client_disconnect(client, "Worker disconnected unexpectedly");
		return;
	case -2:
		/* input buffer full */
		client_disconnect(client, "Worker sent too large input");
		return;
	}

	while ((line = i_stream_next_line(input)) != NULL) {
		if (client_worker_input_line(client, line) < 0)
			return;
	}
}

void client_destroy(struct client *client, const char *reason)
{
	i_set_failure_prefix("%s: ", master_service_get_name(master_service));

	if (!client->disconnected) {
		if (reason == NULL)
			reason = "Connection closed";
		i_info("Disconnected: %s", reason);
	}

	imap_urlauth_client_count--;
	DLLIST_REMOVE(&imap_urlauth_clients, client);

	if (client->to_idle != NULL)
		timeout_remove(&client->to_idle);

	client_worker_disconnect(client);
	
	o_stream_destroy(&client->output);

	net_disconnect(client->fd_in);
	if (client->fd_in != client->fd_out)
		net_disconnect(client->fd_out);

	if (client->username != NULL)
		i_free(client->username);
	array_free(&client->access_apps);
	i_free(client);

	master_service_client_connection_destroyed(master_service);
	imap_urlauth_refresh_proctitle();
}

static void client_destroy_timeout(struct client *client)
{
	client_destroy(client, NULL);
}

void client_disconnect(struct client *client, const char *reason)
{
	if (client->disconnected)
		return;

	client->disconnected = TRUE;
	i_info("Disconnected: %s", reason);

	client->to_idle = timeout_add(0, client_destroy_timeout, client);
}

void clients_destroy_all(void)
{
	while (imap_urlauth_clients != NULL)
		client_destroy(imap_urlauth_clients, "Server shutting down.");
}
