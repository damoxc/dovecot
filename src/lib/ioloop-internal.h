#ifndef IOLOOP_INTERNAL_H
#define IOLOOP_INTERNAL_H

#include "priorityq.h"
#include "ioloop.h"

#ifndef IOLOOP_INITIAL_FD_COUNT
#  define IOLOOP_INITIAL_FD_COUNT 128
#endif

struct ioloop {
        struct ioloop *prev;

	struct io_file *io_files;
	struct io_file *next_io_file;
	struct priorityq *timeouts;

        struct ioloop_handler_context *handler_context;
        struct ioloop_notify_handler_context *notify_handler_context;

	unsigned int running:1;
};

struct io {
	enum io_condition condition;

	io_callback_t *callback;
        void *context;

	struct ioloop *ioloop;
};

struct io_file {
	struct io io;

	/* use a doubly linked list so that io_remove() is quick */
	struct io_file *prev, *next;

	int refcount;
	int fd;
};

struct timeout {
	struct priorityq_item item;

        unsigned int msecs;
	struct timeval next_run;

	timeout_callback_t *callback;
        void *context;

	struct ioloop *ioloop;
};

int io_loop_get_wait_time(struct ioloop *ioloop, struct timeval *tv_r,
			  struct timeval *tv_now);
void io_loop_handle_timeouts(struct ioloop *ioloop);

/* I/O handler calls */
void io_loop_handle_add(struct io_file *io);
void io_loop_handle_remove(struct io_file *io, bool closed);

void io_loop_handler_init(struct ioloop *ioloop);
void io_loop_handler_deinit(struct ioloop *ioloop);

void io_loop_notify_remove(struct io *io);
void io_loop_notify_handler_deinit(struct ioloop *ioloop);

#endif
