#ifndef MASTER_SERVICE_PRIVATE_H
#define MASTER_SERVICE_PRIVATE_H

#include "master-interface.h"
#include "master-service.h"

struct master_service_listener {
	struct master_service *service;
	int fd;
	bool ssl;
	struct io *io;
};

struct master_service {
	struct ioloop *ioloop;

	char *name;
        enum master_service_flags flags;

	int argc;
	char **argv;

	const char *version_string;
	const char *config_path;
	ARRAY_TYPE(const_string) config_overrides;
	int config_fd;
	int syslog_facility;

	unsigned int socket_count, ssl_socket_count;
        struct master_service_listener *listeners;

	struct io *io_status_write, *io_status_error;
	unsigned int service_count_left;
	unsigned int total_available_count;
	struct master_status master_status;

	void (*avail_overflow_callback)(void);
	struct timeout *to_overflow_state;

	master_service_connection_callback_t *callback;

	pool_t set_pool;
	const struct master_service_settings *set;
	struct setting_parser_context *set_parser;

	unsigned int keep_environment:1;
	unsigned int log_directly:1;
	unsigned int initial_status_sent:1;
	unsigned int die_with_master:1;
	unsigned int call_avail_overflow:1;
	unsigned int delay_status_updates:1;
};

void master_service_io_listeners_add(struct master_service *service);

#endif
