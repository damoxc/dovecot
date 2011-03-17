/* Copyright (c) 2005-2011 Dovecot authors, see the included COPYING file */

#define _GNU_SOURCE
#include "lib.h"

#ifdef IOLOOP_NOTIFY_INOTIFY

#include "fd-close-on-exec.h"
#include "fd-set-nonblock.h"
#include "ioloop-internal.h"
#include "ioloop-notify-fd.h"
#include "buffer.h"
#include "network.h"
#include "ipwd.h"

#include <stdio.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/inotify.h>

#define INOTIFY_BUFLEN (32*1024)

struct ioloop_notify_handler_context {
	struct ioloop_notify_fd_context fd_ctx;

	int inotify_fd;
	struct io *event_io;

	bool disabled;
};

static struct ioloop_notify_handler_context *io_loop_notify_handler_init(void);

static bool inotify_input_more(struct ioloop *ioloop)
{
	struct ioloop_notify_handler_context *ctx =
		ioloop->notify_handler_context;
        const struct inotify_event *event;
	unsigned char event_buf[INOTIFY_BUFLEN];
	struct io_notify *io;
	ssize_t ret, pos;

	/* read as many events as there is available and fit into our buffer.
	   only full events are returned by the kernel. */
	ret = read(ctx->inotify_fd, event_buf, sizeof(event_buf));
	if (ret <= 0) {
		if (ret == 0) {
			/* nothing more to read */
			return FALSE;
		}
		i_fatal("read(inotify) failed: %m");
	}

	if (gettimeofday(&ioloop_timeval, NULL) < 0)
		i_fatal("gettimeofday(): %m");
	ioloop_time = ioloop_timeval.tv_sec;

	for (pos = 0; pos < ret; ) {
		if ((size_t)(ret - pos) < sizeof(*event))
			break;

		event = (struct inotify_event *)(event_buf + pos);
		pos += sizeof(*event) + event->len;

		io = io_notify_fd_find(&ctx->fd_ctx, event->wd);
		if (io != NULL) {
			if ((event->mask & IN_IGNORED) != 0) {
				/* calling inotify_rm_watch() would now give
				   EINVAL */
				io->fd = -1;
			}
			io->io.callback(io->io.context);
		}
	}
	if (pos != ret)
		i_error("read(inotify) returned partial event");
	return (size_t)ret >= sizeof(event_buf)-512;
}

static void inotify_input(struct ioloop *ioloop)
{
	while (inotify_input_more(ioloop)) ;
}

#undef io_add_notify
enum io_notify_result io_add_notify(const char *path, io_callback_t *callback,
				    void *context, struct io **io_r)
{
	struct ioloop_notify_handler_context *ctx =
		current_ioloop->notify_handler_context;
	int wd;

	*io_r = NULL;

	if (ctx == NULL)
		ctx = io_loop_notify_handler_init();
	if (ctx->disabled)
		return IO_NOTIFY_NOSUPPORT;

	wd = inotify_add_watch(ctx->inotify_fd, path,
			       IN_CREATE | IN_DELETE | IN_DELETE_SELF |
			       IN_MOVE | IN_CLOSE | IN_MODIFY);
	if (wd < 0) {
		/* ESTALE could happen with NFS. Don't bother giving an error
		   message then. */
		if (errno == ENOENT || errno == ESTALE)
			return IO_NOTIFY_NOTFOUND;

		if (errno != ENOSPC)
			i_error("inotify_add_watch(%s) failed: %m", path);
		else {
			i_warning("Inotify watch limit for user exceeded, "
				  "disabling. Increase "
				  "/proc/sys/fs/inotify/max_user_watches");
		}
		ctx->disabled = TRUE;
		return IO_NOTIFY_NOSUPPORT;
	}

	if (ctx->event_io == NULL) {
		ctx->event_io = io_add(ctx->inotify_fd, IO_READ,
				       inotify_input, current_ioloop);
	}

	*io_r = io_notify_fd_add(&ctx->fd_ctx, wd, callback, context);
	return IO_NOTIFY_ADDED;
}

void io_loop_notify_remove(struct io *_io)
{
	struct ioloop_notify_handler_context *ctx =
		_io->ioloop->notify_handler_context;
	struct io_notify *io = (struct io_notify *)_io;

	if (io->fd != -1) {
		/* ernro=EINVAL happens if the file itself is deleted and
		   kernel has sent IN_IGNORED event which we haven't read. */
		if (inotify_rm_watch(ctx->inotify_fd, io->fd) < 0 &&
		    errno != EINVAL)
			i_error("inotify_rm_watch() failed: %m");
	}

	io_notify_fd_free(&ctx->fd_ctx, io);

	if (ctx->fd_ctx.notifies == NULL)
		io_remove(&ctx->event_io);
}

static void ioloop_inotify_user_limit_exceeded(void)
{
	struct passwd pw;
	const char *name;
	uid_t uid = geteuid();

	if (i_getpwuid(uid, &pw) <= 0)
		name = t_strdup_printf("UID %s", dec2str(uid));
	else {
		name = t_strdup_printf("%s (UID %s)",
				       dec2str(uid), pw.pw_name);
	}
	i_warning("Inotify instance limit for user %s exceeded, disabling. "
		  "Increase /proc/sys/fs/inotify/max_user_instances", name);
}

static struct ioloop_notify_handler_context *io_loop_notify_handler_init(void)
{
	struct ioloop *ioloop = current_ioloop;
	struct ioloop_notify_handler_context *ctx;

	ctx = ioloop->notify_handler_context =
		i_new(struct ioloop_notify_handler_context, 1);

	ctx->inotify_fd = inotify_init();
	if (ctx->inotify_fd == -1) {
		if (errno != EMFILE)
			i_error("inotify_init() failed: %m");
		else
			ioloop_inotify_user_limit_exceeded();
		ctx->disabled = TRUE;
	} else {
		fd_close_on_exec(ctx->inotify_fd, TRUE);
		fd_set_nonblock(ctx->inotify_fd, TRUE);
	}
	return ctx;
}

void io_loop_notify_handler_deinit(struct ioloop *ioloop)
{
	struct ioloop_notify_handler_context *ctx =
		ioloop->notify_handler_context;

	if (ctx->inotify_fd != -1) {
		if (close(ctx->inotify_fd) < 0)
			i_error("close(inotify) failed: %m");
		ctx->inotify_fd = -1;
	}
	i_free(ctx);
}

#endif
