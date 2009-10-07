/* Copyright (C) 2009 Dovecot authors, see the included COPYING file */

#include "lib.h"
#include "lib-signals.h"
#include "array.h"
#include "ostream.h"
#include "master-service.h"
#include "ssl-params-settings.h"
#include "ssl-params.h"

#include <stdlib.h>
#include <unistd.h>
#include <sys/wait.h>

#define SSL_BUILD_PARAM_FNAME "ssl-parameters.dat"

struct client {
	int fd;
	struct ostream *output;
};

static ARRAY_DEFINE(delayed_fds, int);
struct ssl_params *param;
static buffer_t *ssl_params;

static int client_output_flush(struct ostream *output)
{
	if (o_stream_flush(output) == 0) {
		/* more to come */
		return 0;
	}
	/* finished / disconnected */
	o_stream_destroy(&output);
	return -1;
}

static void client_handle(int fd)
{
	struct ostream *output;

	output = o_stream_create_fd(fd, (size_t)-1, TRUE);
	o_stream_send(output, ssl_params->data, ssl_params->used);

	if (o_stream_get_buffer_used_size(output) == 0)
		o_stream_destroy(&output);
	else {
		o_stream_set_flush_callback(output, client_output_flush,
					    output);
	}
}

static void client_connected(const struct master_service_connection *conn)
{
	if (ssl_params->used == 0) {
		/* waiting for parameter building to finish */
		if (!array_is_created(&delayed_fds))
			i_array_init(&delayed_fds, 32);
		array_append(&delayed_fds, &conn->fd, 1);
	} else {
		client_handle(conn->fd);
	}
}

static void ssl_params_callback(const unsigned char *data, size_t size)
{
	const int *fds;
	unsigned int i, count;

	buffer_set_used_size(ssl_params, 0);
	buffer_append(ssl_params, data, size);

	if (!array_is_created(&delayed_fds))
		return;

	fds = array_get(&delayed_fds, &count);
	for (i = 0; i < count; i++)
		client_handle(fds[i]);
	array_free(&delayed_fds);
}

static void sig_chld(const siginfo_t *si ATTR_UNUSED, void *context ATTR_UNUSED)
{
	int status;

	if (waitpid(-1, &status, WNOHANG) < 0)
		i_error("waitpid() failed: %m");
	else if (status != 0)
		i_error("child process failed with status %d", status);
	else {
		/* params should have been created now. try refreshing. */
		ssl_params_refresh(param);
	}
}

static void main_init(const struct ssl_params_settings *set)
{
	lib_signals_set_handler(SIGCHLD, TRUE, sig_chld, NULL);

	ssl_params = buffer_create_dynamic(default_pool, 1024);
	param = ssl_params_init(PKG_STATEDIR"/"SSL_BUILD_PARAM_FNAME,
				ssl_params_callback, set);
}

static void main_deinit(void)
{
	ssl_params_deinit(&param);
	if (array_is_created(&delayed_fds))
		array_free(&delayed_fds);
}

int main(int argc, char *argv[])
{
	const struct ssl_params_settings *set;
	int c;

	master_service = master_service_init("ssl-params", 0, argc, argv);
	master_service_init_log(master_service, "ssl-params: ");

	while ((c = getopt(argc, argv, master_service_getopt_string())) > 0) {
		if (!master_service_parse_option(master_service, c, optarg))
			exit(FATAL_DEFAULT);
	}

	set = ssl_params_settings_read(master_service);
	master_service_init_finish(master_service);

#ifndef HAVE_SSL
	i_fatal("Dovecot built without SSL support");
#endif

	main_init(set);
	master_service_run(master_service, client_connected);
	main_deinit();

	master_service_deinit(&master_service);
        return 0;
}
