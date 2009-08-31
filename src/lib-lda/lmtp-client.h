#ifndef LMTP_CLIENT_H
#define LMTP_CLIENT_H

/* LMTP/SMTP client code. */

enum lmtp_client_protocol {
	LMTP_CLIENT_PROTOCOL_LMTP,
	LMTP_CLIENT_PROTOCOL_SMTP
};

/* reply contains the reply coming from remote server, or NULL
   if it's a connection error. */
typedef void lmtp_callback_t(bool success, const char *reply, void *context);

struct lmtp_client *
lmtp_client_init(const char *mail_from, const char *my_hostname);
void lmtp_client_deinit(struct lmtp_client **client);

int lmtp_client_connect_tcp(struct lmtp_client *client,
			    enum lmtp_client_protocol protocol,
			    const char *host, unsigned int port);

/* Add recipient to the session. rcpt_to_callback is called once LMTP server
   replies with RCPT TO. If RCPT TO was a succees, data_callback is called
   when DATA replies. */
void lmtp_client_add_rcpt(struct lmtp_client *client, const char *address,
			  lmtp_callback_t *rcpt_to_callback,
			  lmtp_callback_t *data_callback, void *context);
/* Start sending input stream as DATA. */
void lmtp_client_send(struct lmtp_client *client, struct istream *data_input);
/* Call this function whenever input stream can potentially be read forward.
   This is useful with non-blocking istreams and tee-istreams. */
void lmtp_client_send_more(struct lmtp_client *client);

#endif
