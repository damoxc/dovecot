/* Copyright (c) 2009 Dovecot authors, see the included COPYING file */

#include "lib.h"
#include "strescape.h"
#include "fd-set-nonblock.h"
#include "istream.h"
#include "ostream.h"
#include "master-service.h"
#include "dsync-worker.h"
#include "dsync-proxy.h"
#include "dsync-proxy-server.h"

#include <stdlib.h>

static int
proxy_server_read_line(struct dsync_proxy_server *server,
		       const char **line_r)
{
	*line_r = i_stream_read_next_line(server->input);
	if (*line_r == NULL) {
		if (server->input->stream_errno != 0) {
			errno = server->input->stream_errno;
			i_error("read() from proxy client failed: %m");
			master_service_stop(master_service);
			return -1;
		}
		if (server->input->eof) {
			master_service_stop(master_service);
			return -1;
		}
	}
	return *line_r != NULL ? 1 : 0;
}

static int proxy_server_run_cmd(struct dsync_proxy_server *server)
{
	uint32_t tag;
	int ret, result;

	if ((ret = server->cur_cmd->func(server, server->cur_args)) == 0)
		return 0;
	if (ret < 0)
		i_error("command %s failed", server->cur_cmd->name);
	dsync_worker_verify_result_is_clear(server->worker);

	while (dsync_worker_get_next_result(server->worker, &tag, &result)) {
		o_stream_send_str(server->output,
				  t_strdup_printf("%u\t%d", tag, result));
	}

	server->cur_cmd = NULL;
	server->cur_args = NULL;
	return 1;
}

static int
proxy_server_input_line(struct dsync_proxy_server *server, const char *line)
{
	const char *const *args;
	const char **cmd_args;
	unsigned int i, count;
	uint32_t tag;

	i_assert(server->cur_cmd == NULL);

	args = t_strsplit(line, "\t");
	if (args[0] == NULL || args[1] == NULL) {
		i_error("proxy client sent invalid input: %s", line);
		return -1;
	}

	tag = strtoul(args[0], NULL, 10);
	server->cur_cmd = dsync_proxy_server_command_find(args[1]);
	if (server->cur_cmd == NULL) {
		i_error("proxy client sent unknown command: %s", args[1]);
		return -1;
	} else {
		args += 2;
		count = str_array_length(args);

		p_clear(server->cmd_pool);
		cmd_args = p_new(server->cmd_pool, const char *, count + 1);
		for (i = 0; i < count; i++) {
			cmd_args[i] = str_tabunescape(p_strdup(server->cmd_pool,
							       args[i]));
		}

		server->cur_args = cmd_args;
		if (tag != 0)
			dsync_worker_set_next_result_tag(server->worker, tag);
		return proxy_server_run_cmd(server);
	}
}

static void proxy_server_input(struct dsync_proxy_server *server)
{
	const char *line;
	int ret = 0;

	if (server->cur_cmd != NULL) {
		/* wait until command handling is finished */
		io_remove(&server->io);
		return;
	}

	o_stream_cork(server->output);
	while (proxy_server_read_line(server, &line) > 0) {
		T_BEGIN {
			ret = proxy_server_input_line(server, line);
		} T_END;
		if (ret <= 0)
			break;
	}
	o_stream_uncork(server->output);

	if (ret < 0)
		master_service_stop(master_service);
}

static int proxy_server_output(struct dsync_proxy_server *server)
{
	struct ostream *output = server->output;
	int ret;

	if ((ret = o_stream_flush(output)) < 0)
		return 1;

	if (server->cur_cmd != NULL) {
		o_stream_cork(output);
		(void)proxy_server_run_cmd(server);
		o_stream_uncork(output);

		if (server->cur_cmd == NULL && server->io == NULL) {
			server->io = io_add(server->fd_in, IO_READ,
					    proxy_server_input, server);
		}
	}
	return ret;
}

struct dsync_proxy_server *
dsync_proxy_server_init(int fd_in, int fd_out, struct dsync_worker *worker)
{
	struct dsync_proxy_server *server;

	server = i_new(struct dsync_proxy_server, 1);
	server->worker = worker;
	server->fd_in = fd_in;
	server->fd_out = fd_out;

	server->cmd_pool = pool_alloconly_create("worker server cmd", 1024);
	server->io = io_add(fd_in, IO_READ, proxy_server_input, server);
	server->input = i_stream_create_fd(fd_in, (size_t)-1, FALSE);
	server->output = o_stream_create_fd(fd_out, (size_t)-1, FALSE);
	o_stream_set_flush_callback(server->output, proxy_server_output,
				    server);
	fd_set_nonblock(fd_in, TRUE);
	fd_set_nonblock(fd_out, TRUE);
	return server;
}

void dsync_proxy_server_deinit(struct dsync_proxy_server **_server)
{
	struct dsync_proxy_server *server = *_server;

	*_server = NULL;

	pool_unref(&server->cmd_pool);
	io_remove(&server->io);
	i_stream_destroy(&server->input);
	o_stream_destroy(&server->output);
	if (close(server->fd_in) < 0)
		i_error("close(proxy input) failed: %m");
	if (server->fd_in != server->fd_out) {
		if (close(server->fd_out) < 0)
			i_error("close(proxy output) failed: %m");
	}
	i_free(server);
}
