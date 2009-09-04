/* Copyright (C) 2009 Dovecot authors, see the included COPYING file */

#include "common.h"
#include "array.h"
#include "env-util.h"
#include "master-service.h"
#include "master-interface.h"
#include "connect-limit.h"
#include "anvil-connection.h"

#include <stdlib.h>
#include <unistd.h>

struct connect_limit *connect_limit;

static void client_connected(const struct master_service_connection *conn)
{
	bool master = conn->listen_fd == MASTER_LISTEN_FD_FIRST;

	anvil_connection_create(conn->fd, master, conn->fifo);
}

int main(int argc, char *argv[])
{
	int c;

	master_service = master_service_init("anvil", 0, argc, argv);
	while ((c = getopt(argc, argv, master_service_getopt_string())) > 0) {
		if (!master_service_parse_option(master_service, c, optarg))
			exit(FATAL_DEFAULT);
	}

	master_service_init_log(master_service, "anvil: ", 0);
	master_service_init_finish(master_service);
	connect_limit = connect_limit_init();

	master_service_run(master_service, client_connected);

	connect_limit_deinit(&connect_limit);
	anvil_connections_destroy_all();
	master_service_deinit(&master_service);
        return 0;
}
