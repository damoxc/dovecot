#ifndef __SASL_SERVER_H
#define __SASL_SERVER_H

enum sasl_server_reply {
	SASL_SERVER_REPLY_SUCCESS,
	SASL_SERVER_REPLY_AUTH_FAILED,
	SASL_SERVER_REPLY_MASTER_FAILED,
	SASL_SERVER_REPLY_CONTINUE
};

typedef void sasl_server_callback_t(struct client *client,
				    enum sasl_server_reply reply,
				    const char *data);

void sasl_server_auth_begin(struct client *client,
			    const char *protocol, const char *mech_name,
			    const unsigned char *initial_resp,
			    size_t initial_resp_size,
			    sasl_server_callback_t *callback);
void sasl_server_auth_cancel(struct client *client, const char *reason);

#endif
