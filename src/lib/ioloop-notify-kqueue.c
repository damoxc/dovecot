/*
 * BSD kqueue() based ioloop notify handler.
 *
 * Copyright (c) 2005 Vaclav Haisman <v.haisman@sh.cvut.cz>
 */

#define _GNU_SOURCE
#include "lib.h"

#ifdef IOLOOP_NOTIFY_KQUEUE

#include "ioloop-internal.h"
#include "fd-close-on-exec.h"
#include <unistd.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/event.h>
#include <sys/time.h>
#include <sys/stat.h>

/* kevent.udata's type just has to be different in NetBSD than in
   FreeBSD and OpenBSD.. */
#ifdef __NetBSD__
#  define MY_EV_SET(a, b, c, d, e, f, g) \
	EV_SET(a, b, c, d, e, f, (intptr_t)g)
#else
#  define MY_EV_SET(a, b, c, d, e, f, g) \
	EV_SET(a, b, c, d, e, f, g)
#endif

struct io_notify {
	struct io io;
	int refcount;
	int fd;
};

struct ioloop_notify_handler_context {
	int kq;
	struct io *event_io;
};

static void event_callback(struct ioloop_notify_handler_context *ctx)
{
	struct io_notify *io;
	struct kevent events[64];
	struct timespec ts;
	int i, ret;

	ts.tv_sec = 0;
	ts.tv_nsec = 0;

	ret = kevent(ctx->kq, NULL, 0, events, N_ELEMENTS(events), &ts);
	if (ret <= 0) {
		if (ret == 0 || errno == EINTR)
			return;

		i_fatal("kevent(notify) failed: %m");
	}

	if (gettimeofday(&ioloop_timeval, NULL) < 0)
		i_fatal("gettimeofday() failed: %m");
	ioloop_time = ioloop_timeval.tv_sec;

	for (i = 0; i < ret; i++) {
		io = (void *)events[i].udata;
		i_assert(io->refcount >= 1);
		io->refcount++;
	}
	for (i = 0; i < ret; i++) {
		io = (void *)events[i].udata;
		/* there can be multiple events for a single io.
		   call the callback only once if that happens. */
		if (io->refcount == 2 && io->io.callback != NULL)
			io->io.callback(io->io.context);

		if (--io->refcount == 0)
			i_free(io);
	}
}

static struct ioloop_notify_handler_context *io_loop_notify_handler_init(void)
{
	struct ioloop_notify_handler_context *ctx;

	ctx = current_ioloop->notify_handler_context =
		i_new(struct ioloop_notify_handler_context, 1);
	ctx->kq = kqueue();
	if (ctx->kq < 0)
		i_fatal("kqueue(notify) failed: %m");
	fd_close_on_exec(ctx->kq, TRUE);
	return ctx;
}

void io_loop_notify_handler_deinit(struct ioloop *ioloop)
{
	struct ioloop_notify_handler_context *ctx =
		ioloop->notify_handler_context;

	if (ctx->event_io)
		io_remove(&ctx->event_io);
	if (close(ctx->kq) < 0)
		i_error("close(kqueue notify) failed: %m");
	i_free(ctx);
}

#undef io_add_notify
enum io_notify_result io_add_notify(const char *path, io_callback_t *callback,
				    void *context, struct io **io_r)
{
	struct ioloop_notify_handler_context *ctx =
		current_ioloop->notify_handler_context;
	struct kevent ev;
	struct io_notify *io;
	int fd;

	if (ctx == NULL)
		ctx = io_loop_notify_handler_init();

	fd = open(path, O_RDONLY);
	if (fd == -1) {
		/* ESTALE could happen with NFS. Don't bother giving an error
		   message then. */
		if (errno != ENOENT && errno != ESTALE)
			i_error("open(%s) for kq notify failed: %m", path);
		return IO_NOTIFY_NOTFOUND;
	}
	fd_close_on_exec(fd, TRUE);

	io = i_new(struct io_notify, 1);
	io->io.condition = IO_NOTIFY;
	io->io.callback = callback;
	io->io.context = context;
	io->io.ioloop = current_ioloop;
	io->refcount = 1;
	io->fd = fd;

	/* EV_CLEAR flag is needed because the EVFILT_VNODE filter reports
	   event state transitions and not the current state.  With this flag,
	   the same event is only returned once. */
	MY_EV_SET(&ev, fd, EVFILT_VNODE, EV_ADD | EV_CLEAR,
		  NOTE_DELETE | NOTE_WRITE | NOTE_EXTEND | NOTE_REVOKE, 0, io);
	if (kevent(ctx->kq, &ev, 1, NULL, 0, NULL) < 0) {
		i_error("kevent(%d, %s) for notify failed: %m", fd, path);
		(void)close(fd);
		i_free(io);
		return IO_NOTIFY_NOSUPPORT;
	}

	if (ctx->event_io == NULL) {
		ctx->event_io = io_add(ctx->kq, IO_READ, event_callback,
				       io->io.ioloop->notify_handler_context);
	}
	*io_r = &io->io;
	return IO_NOTIFY_ADDED;
}

void io_loop_notify_remove(struct io *_io)
{
	struct ioloop_notify_handler_context *ctx =
		_io->ioloop->notify_handler_context;
	struct io_notify *io = (struct io_notify *)_io;
	struct kevent ev;

	MY_EV_SET(&ev, io->fd, EVFILT_VNODE, EV_DELETE, 0, 0, NULL);
	if (kevent(ctx->kq, &ev, 1, NULL, 0, 0) < 0)
		i_error("kevent(%d) for notify remove failed: %m", io->fd);
	if (close(io->fd) < 0)
		i_error("close(%d) for notify remove failed: %m", io->fd);
	io->fd = -1;

	if (--io->refcount == 0)
		i_free(io);
}

#endif
