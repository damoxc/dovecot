#ifndef __AUTH_REQUEST_HANDLER_H
#define __AUTH_REQUEST_HANDLER_H

typedef void auth_request_callback_t(const char *reply, void *context);

struct auth_request_handler *
auth_request_handler_create(struct auth *auth,
			    unsigned int connect_uid, unsigned int client_pid,
			    auth_request_callback_t *callback, void *context,
			    auth_request_callback_t *master_callback,
			    void *master_context);
void auth_request_handler_unref(struct auth_request_handler *handler);

void auth_request_handler_check_timeouts(struct auth_request_handler *handler);

int auth_request_handler_auth_begin(struct auth_request_handler *handler,
				    const char *args);
int auth_request_handler_auth_continue(struct auth_request_handler *handler,
				       const char *args);
void auth_request_handler_master_request(struct auth_request_handler *handler,
					 unsigned int id,
					 unsigned int client_id);

void auth_request_handlers_flush_failures(void);

void auth_request_handlers_init(void);
void auth_request_handlers_deinit(void);

#endif
