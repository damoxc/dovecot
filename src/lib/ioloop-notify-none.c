/* Copyright (C) 2003 Timo Sirainen */

#include "lib.h"
#include "ioloop-internal.h"

#ifdef IOLOOP_NOTIFY_NONE

#undef io_add_notify
enum io_notify_result
io_add_notify(const char *path __attr_unused__,
	      io_callback_t *callback __attr_unused__,
	      void *context __attr_unused__, struct io **io_r)
{
	*io_r = NULL;
	return IO_NOTIFY_DISABLED;
}

void io_loop_notify_remove(struct ioloop *ioloop __attr_unused__,
			   struct io *io __attr_unused__)
{
}

void io_loop_notify_handler_deinit(struct ioloop *ioloop __attr_unused__)
{
}

#endif
