/* Copyright (c) 2001-2009 Dovecot authors, see the included COPYING file */

#include "lib.h"
#include "ioloop.h"
#include "fd-close-on-exec.h"
#include "lib-signals.h"

#include <signal.h>
#include <unistd.h>

#define MAX_SIGNAL_VALUE 31

struct signal_handler {
	signal_handler_t *handler;
	void *context;

	bool delayed;
        struct signal_handler *next;
};

/* Remember that these are accessed inside signal handler which may be called
   even while we're initializing/deinitializing. Try hard to keep everything
   in consistent state. */
static struct signal_handler *signal_handlers[MAX_SIGNAL_VALUE+1] = { NULL, };
static int sig_pipe_fd[2] = { -1, -1 };

static bool signals_initialized = FALSE;
static struct io *io_sig = NULL;

static void sig_handler(int signo, siginfo_t *si, void *context ATTR_UNUSED)
{
	struct signal_handler *h;
	bool delayed_sent = FALSE;

	if (signo < 0 || signo > MAX_SIGNAL_VALUE)
		return;

	/* remember that we're inside a signal handler which might have been
	   called at any time. don't do anything that's unsafe. */
	for (h = signal_handlers[signo]; h != NULL; h = h->next) {
		if (!h->delayed)
			h->handler(si, h->context);
		else if (!delayed_sent) {
			int saved_errno = errno;

			if (write(sig_pipe_fd[1], si,
				  sizeof(*si)) != sizeof(*si))
				i_error("write(sigpipe) failed: %m");
			delayed_sent = TRUE;
			errno = saved_errno;
		}
	}
}

static void sig_ignore(int signo ATTR_UNUSED, siginfo_t *si ATTR_UNUSED,
		       void *context ATTR_UNUSED)
{
	/* if we used SIG_IGN instead of this function,
	   the system call might be restarted */
}

static void signal_read(void *context ATTR_UNUSED)
{
	siginfo_t signal_buf[10];
	siginfo_t signals[MAX_SIGNAL_VALUE+1];
	ssize_t i, ret;
	int signo;

	ret = read(sig_pipe_fd[0], signal_buf, sizeof(signal_buf));
	if (ret > 0) {
		if (ret % sizeof(siginfo_t) != 0)
			i_fatal("read(sigpipe) returned partial data");
		ret /= sizeof(siginfo_t);

		/* get rid of duplicate signals */
		memset(signals, 0, sizeof(signals));
		for (i = 0; i < ret; i++) {
			signo = signal_buf[i].si_signo;
			if (signo > MAX_SIGNAL_VALUE) {
				i_panic("sigpipe contains signal %d > %d",
					signo, MAX_SIGNAL_VALUE);
			}
			signals[signo] = signal_buf[i];
		}

		/* call the delayed handlers */
		for (signo = 0; signo < MAX_SIGNAL_VALUE; signo++) {
			if (signals[signo].si_signo > 0) {
				struct signal_handler *h =
					signal_handlers[signo];

				for (; h != NULL; h = h->next) {
					if (h->delayed) {
						h->handler(&signals[signo],
							   h->context);
					}
				}
			}
		}
	} else if (ret < 0) {
		if (errno != EAGAIN)
			i_fatal("read(sigpipe) failed: %m");
	} else {
		i_fatal("read(sigpipe) failed: EOF");
	}
}

static void lib_signals_set(int signo, bool ignore)
{
	struct sigaction act;

	if (sigemptyset(&act.sa_mask) < 0)
		i_fatal("sigemptyset(): %m");
	act.sa_flags = SA_SIGINFO;
	act.sa_sigaction = ignore ? sig_ignore : sig_handler;
	if (sigaction(signo, &act, NULL) < 0)
		i_fatal("sigaction(%d): %m", signo);
}

void lib_signals_set_handler(int signo, bool delayed,
			     signal_handler_t *handler, void *context)
{
	struct signal_handler *h;

	i_assert(handler != NULL);

	if (signo < 0 || signo > MAX_SIGNAL_VALUE) {
		i_panic("Trying to set signal %d handler, but max is %d",
			signo, MAX_SIGNAL_VALUE);
	}

	if (signal_handlers[signo] == NULL && signals_initialized)
		lib_signals_set(signo, FALSE);

	if (delayed && sig_pipe_fd[0] == -1) {
		/* first delayed handler */
		if (pipe(sig_pipe_fd) < 0)
			i_fatal("pipe() failed: %m");
		fd_close_on_exec(sig_pipe_fd[0], TRUE);
		fd_close_on_exec(sig_pipe_fd[1], TRUE);
		if (signals_initialized) {
			io_sig = io_add(sig_pipe_fd[0], IO_READ,
					signal_read, NULL);
		}
	}

	h = i_new(struct signal_handler, 1);
	h->handler = handler;
	h->context = context;
	h->delayed = delayed;

	/* atomically set to signal_handlers[] list */
	h->next = signal_handlers[signo];
	signal_handlers[signo] = h;
}

void lib_signals_ignore(int signo, bool restart_syscalls)
{
	struct sigaction act;

	if (signo < 0 || signo > MAX_SIGNAL_VALUE) {
		i_panic("Trying to ignore signal %d, but max is %d",
			signo, MAX_SIGNAL_VALUE);
	}

	i_assert(signal_handlers[signo] == NULL);

	if (sigemptyset(&act.sa_mask) < 0)
		i_fatal("sigemptyset(): %m");
	if (restart_syscalls) {
		act.sa_flags = SA_RESTART;
		act.sa_handler = SIG_IGN;
	} else {
		act.sa_flags = SA_SIGINFO;
		act.sa_sigaction = sig_ignore;
	}

	if (sigaction(signo, &act, NULL) < 0)
		i_fatal("sigaction(%d): %m", signo);
}

void lib_signals_unset_handler(int signo, signal_handler_t *handler,
			       void *context)
{
	struct signal_handler *h, **p;

	for (p = &signal_handlers[signo]; *p != NULL; p = &(*p)->next) {
		if ((*p)->handler == handler && (*p)->context == context) {
			h = *p;
			*p = h->next;
			i_free(h);
			return;
		}
	}

	i_panic("lib_signals_unset_handler(%d, %p, %p): handler not found",
		signo, (void *)handler, context);
}

void lib_signals_reset_ioloop(void)
{
	if (io_sig != NULL) {
		io_remove(&io_sig);
		io_sig = io_add(sig_pipe_fd[0], IO_READ, signal_read, NULL);
	}
}

void lib_signals_init(void)
{
	int i;

	signals_initialized = TRUE;

	/* add signals that were already registered */
	for (i = 0; i < MAX_SIGNAL_VALUE; i++) {
		if (signal_handlers[i] != NULL)
			lib_signals_set(i, FALSE);
	}

	if (sig_pipe_fd[0] != -1)
		io_sig = io_add(sig_pipe_fd[0], IO_READ, signal_read, NULL);
}

void lib_signals_deinit(void)
{
	struct signal_handler *handlers, *h;
	int i;

	for (i = 0; i < MAX_SIGNAL_VALUE; i++) {
		if (signal_handlers[i] != NULL) {
			/* atomically remove from signal_handlers[] list */
			handlers = signal_handlers[i];
			signal_handlers[i] = NULL;

			while (handlers != NULL) {
				h = handlers;
				handlers = h->next;
				i_free(h);
			}
		}
	}

	if (io_sig != NULL)
		io_remove(&io_sig);
	if (sig_pipe_fd[0] != -1) {
		if (close(sig_pipe_fd[0]) < 0)
			i_error("close(sigpipe) failed: %m");
		if (close(sig_pipe_fd[1]) < 0)
			i_error("close(sigpipe) failed: %m");
	}
}
