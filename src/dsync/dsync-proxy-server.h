#ifndef DSYNC_PROXY_SERVER_H
#define DSYNC_PROXY_SERVER_H

struct dsync_proxy_server;

struct dsync_proxy_server_command {
	const char *name;
	int (*func)(struct dsync_proxy_server *server,
		    const char *const *args);
};

struct dsync_proxy_server {
	int fd_in, fd_out;
	struct io *io;
	struct istream *input;
	struct ostream *output;

	struct dsync_worker *worker;

	pool_t cmd_pool;
	struct dsync_proxy_server_command *cur_cmd;
	const char *const *cur_args;

	struct dsync_worker_mailbox_iter *mailbox_iter;
	struct dsync_worker_msg_iter *msg_iter;
};

struct dsync_proxy_server *
dsync_proxy_server_init(int fd_in, int fd_out, struct dsync_worker *worker);
void dsync_proxy_server_deinit(struct dsync_proxy_server **server);

struct dsync_proxy_server_command *
dsync_proxy_server_command_find(const char *name);

#endif
