/* Copyright (c) 2002-2003 Timo Sirainen */

/* @UNSAFE: whole file */

#include "lib.h"
#include "ioloop-internal.h"

#ifdef IOLOOP_POLL

#include <sys/poll.h>

#ifndef INITIAL_POLL_FDS
#  define INITIAL_POLL_FDS 128
#endif

struct ioloop_handler_context {
	unsigned int fds_size, fds_pos;
	struct pollfd *fds;

	unsigned int idx_size;
	int *fd_index;
};

void io_loop_handler_init(struct ioloop *ioloop)
{
	struct ioloop_handler_context *ctx;

	ioloop->handler_context = ctx =
		p_new(ioloop->pool, struct ioloop_handler_context, 1);
	ctx->fds_size = INITIAL_POLL_FDS;
	ctx->fds = p_new(ioloop->pool, struct pollfd, ctx->fds_size);

	ctx->idx_size = INITIAL_POLL_FDS;
	ctx->fd_index = p_new(ioloop->pool, int, ctx->idx_size);
        memset(ctx->fd_index, 0xff, sizeof(int) * ctx->idx_size);
}

void io_loop_handler_deinit(struct ioloop *ioloop)
{
        p_free(ioloop->pool, ioloop->handler_context->fds);
        p_free(ioloop->pool, ioloop->handler_context->fd_index);
        p_free(ioloop->pool, ioloop->handler_context);
}

#define IO_POLL_INPUT (POLLIN|POLLPRI|POLLERR|POLLHUP|POLLNVAL)
#define IO_POLL_OUTPUT (POLLOUT|POLLERR|POLLHUP|POLLNVAL)

void io_loop_handle_add(struct ioloop *ioloop, struct io *io)
{
	struct ioloop_handler_context *ctx = ioloop->handler_context;
	enum io_condition condition = io->condition;
	unsigned int old_size;
	int index, fd = io->fd;

	if ((unsigned int)fd >= ctx->idx_size) {
                /* grow the fd -> index array */
		old_size = ctx->idx_size;

		ctx->idx_size = nearest_power((unsigned int) fd+1);
		i_assert(ctx->idx_size < (size_t)-1 / sizeof(int));

		ctx->fd_index = p_realloc(ioloop->pool, ctx->fd_index,
					  sizeof(int) * old_size,
					  sizeof(int) * ctx->idx_size);
		memset(ctx->fd_index + old_size, 0xff,
		       sizeof(int) * (ctx->idx_size-old_size));
	}

	if (ctx->fds_pos >= ctx->fds_size) {
		/* grow the fd array */
		old_size = ctx->fds_size;

		ctx->fds_size = nearest_power(ctx->fds_size+1);
		i_assert(ctx->fds_size < (size_t)-1 / sizeof(struct pollfd));

		ctx->fds = p_realloc(ioloop->pool, ctx->fds,
				     sizeof(struct pollfd) * old_size,
				     sizeof(struct pollfd) * ctx->fds_size);
	}

	if (ctx->fd_index[fd] != -1) {
		/* update existing pollfd */
                index = ctx->fd_index[fd];
	} else {
                /* add new pollfd */
                index = ctx->fds_pos++;

		ctx->fd_index[fd] = index;
		ctx->fds[index].fd = fd;
		ctx->fds[index].events = 0;
		ctx->fds[index].revents = 0;
	}

        if (condition & IO_READ)
		ctx->fds[index].events |= IO_POLL_INPUT;
        if (condition & IO_WRITE)
		ctx->fds[index].events |= IO_POLL_OUTPUT;
}

void io_loop_handle_remove(struct ioloop *ioloop,  struct io *io)
{
	struct ioloop_handler_context *ctx = ioloop->handler_context;
	enum io_condition condition = io->condition;
	int index, fd = io->fd;

	index = ctx->fd_index[fd];
	i_assert(index >= 0 && (unsigned int) index < ctx->fds_size);

	if (condition & IO_READ) {
		ctx->fds[index].events &= ~(POLLIN|POLLPRI);
		ctx->fds[index].revents &= ~(POLLIN|POLLPRI);
	}
	if (condition & IO_WRITE) {
		ctx->fds[index].events &= ~POLLOUT;
		ctx->fds[index].revents &= ~POLLOUT;
	}

	if ((ctx->fds[index].events & (POLLIN|POLLOUT)) == 0) {
		/* remove the whole pollfd */
		ctx->fd_index[ctx->fds[index].fd] = -1;
		if (--ctx->fds_pos == (unsigned int) index)
                        return; /* removing last one */

                /* move the last pollfd over the removed one */
		ctx->fds[index] = ctx->fds[ctx->fds_pos];
		ctx->fd_index[ctx->fds[index].fd] = index;
	}
}

void io_loop_handler_run(struct ioloop *ioloop)
{
	struct ioloop_handler_context *ctx = ioloop->handler_context;
        struct pollfd *pollfd;
        struct timeval tv;
	struct io *io;
	unsigned int t_id;
	int msecs, ret, call;

        /* get the time left for next timeout task */
	msecs = io_loop_get_wait_time(ioloop->timeouts, &tv, NULL);

	ret = poll(ctx->fds, ctx->fds_pos, msecs);
	if (ret < 0 && errno != EINTR)
		i_fatal("poll(): %m");

	/* execute timeout handlers */
        io_loop_handle_timeouts(ioloop);

	if (ret <= 0 || !ioloop->running) {
                /* no I/O events */
		return;
	}

	for (io = ioloop->ios; io != NULL && ret > 0; io = ioloop->next_io) {
		ioloop->next_io = io->next;

		pollfd = &ctx->fds[ctx->fd_index[io->fd]];
		if (pollfd->revents != 0) {
			if (pollfd->revents & POLLNVAL) {
				i_error("invalid I/O fd %d, callback %p",
					io->fd, (void *) io->callback);
				pollfd->events &= ~POLLNVAL;
				pollfd->revents = 0;
				call = TRUE;
			} else if ((io->condition &
				    (IO_READ|IO_WRITE)) == (IO_READ|IO_WRITE)) {
				call = TRUE;
				pollfd->revents = 0;
			} else if (io->condition & IO_READ) {
				call = (pollfd->revents & IO_POLL_INPUT) != 0;
				pollfd->revents &= ~IO_POLL_INPUT;
			} else if (io->condition & IO_WRITE) {
				call = (pollfd->revents & IO_POLL_OUTPUT) != 0;
				pollfd->revents &= ~IO_POLL_OUTPUT;
			} else {
				call = FALSE;
			}

			if (pollfd->revents == 0)
				ret--;

			if (call) {
				t_id = t_push();
				io->callback(io->context);
				if (t_pop() != t_id) {
					i_panic("Leaked a t_pop() call in "
						"I/O handler %p",
						(void *)io->callback);
				}
			}
		}
	}
}

#endif
