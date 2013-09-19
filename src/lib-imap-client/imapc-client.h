#ifndef IMAPC_CLIENT_H
#define IMAPC_CLIENT_H

/* IMAP RFC defines this to be at least 30 minutes. */
#define IMAPC_DEFAULT_MAX_IDLE_TIME (60*29)

enum imapc_command_state {
	IMAPC_COMMAND_STATE_OK,
	IMAPC_COMMAND_STATE_NO,
	IMAPC_COMMAND_STATE_BAD,
	IMAPC_COMMAND_STATE_DISCONNECTED
};

enum imapc_capability {
	IMAPC_CAPABILITY_SASL_IR	= 0x01,
	IMAPC_CAPABILITY_LITERALPLUS	= 0x02,
	IMAPC_CAPABILITY_QRESYNC	= 0x04,
	IMAPC_CAPABILITY_IDLE		= 0x08,
	IMAPC_CAPABILITY_UIDPLUS	= 0x10,
	IMAPC_CAPABILITY_AUTH_PLAIN	= 0x20,
	IMAPC_CAPABILITY_STARTTLS	= 0x40,
	IMAPC_CAPABILITY_X_GM_EXT_1	= 0x80,
	IMAPC_CAPABILITY_CONDSTORE	= 0x100,
	IMAPC_CAPABILITY_NAMESPACE	= 0x200,
	IMAPC_CAPABILITY_UNSELECT	= 0x400,

	IMAPC_CAPABILITY_IMAP4REV1	= 0x40000000
};
struct imapc_capability_name {
	const char *name;
	enum imapc_capability capability;
};
extern const struct imapc_capability_name imapc_capability_names[];

enum imapc_command_flags {
	/* The command changes the selected mailbox (SELECT, EXAMINE) */
	IMAPC_COMMAND_FLAG_SELECT	= 0x01,
	/* The command is sent to server before login (or is the login
	   command itself). Non-prelogin commands will be queued until login
	   is successful. */
	IMAPC_COMMAND_FLAG_PRELOGIN	= 0x02,
	/* Allow command to be automatically retried if disconnected before it
	   finishes. */
	IMAPC_COMMAND_FLAG_RETRIABLE	= 0x04
};

enum imapc_client_ssl_mode {
	IMAPC_CLIENT_SSL_MODE_NONE,
	IMAPC_CLIENT_SSL_MODE_IMMEDIATE,
	IMAPC_CLIENT_SSL_MODE_STARTTLS
};

#define IMAPC_DEFAULT_CONNECT_TIMEOUT_MSECS (1000*30)
#define IMAPC_DEFAULT_COMMAND_TIMEOUT_MSECS (1000*60*5)

struct imapc_client_settings {
	const char *host;
	unsigned int port;

	const char *master_user;
	const char *username;
	const char *password;
	unsigned int max_idle_time;

	const char *dns_client_socket_path;
	const char *temp_path_prefix;

	enum imapc_client_ssl_mode ssl_mode;
	const char *ssl_ca_dir, *ssl_ca_file;
	bool ssl_verify;

	const char *rawlog_dir;
	const char *ssl_crypto_device;
	bool debug;

	/* Timeout for logging in. 0 = default. */
	unsigned int connect_timeout_msecs;
	/* Timeout for IMAP commands. Reset every time more data is being
	   sent or received. 0 = default. */
	unsigned int cmd_timeout_msecs;
};

struct imapc_command_reply {
	enum imapc_command_state state;
	/* "[RESP TEXT]" produces key=RESP, value=TEXT.
	   "[RESP]" produces key=RESP, value=NULL
	   otherwise both are NULL */
	const char *resp_text_key, *resp_text_value;
	/* The full tagged reply, including [RESP TEXT]. */
	const char *text_full;
	/* Tagged reply text without [RESP TEXT] */
	const char *text_without_resp;
};

struct imapc_arg_file {
	/* file descriptor containing the value */
	int fd;

	/* parent_arg.list[list_idx] points to the IMAP_ARG_LITERAL_SIZE
	   argument */
	const struct imap_arg *parent_arg;
	unsigned int list_idx;
};

struct imapc_untagged_reply {
	/* name of the untagged reply, e.g. EXISTS */
	const char *name;
	/* number at the beginning of the reply, or 0 if there wasn't any.
	   Set for EXISTS, EXPUNGE, etc. */
	uint32_t num;
	/* the rest of the reply can be read from these args. */
	const struct imap_arg *args;
	/* arguments whose contents are stored into files. only
	   "FETCH (BODY[" arguments can be here. */
	const struct imapc_arg_file *file_args;
	unsigned int file_args_count;

	/* "* OK [RESP TEXT]" produces key=RESP, value=TEXT.
	   "* OK [RESP]" produces key=RESP, value=NULL
	   otherwise both are NULL */
	const char *resp_text_key, *resp_text_value;

	/* If this reply occurred while a mailbox was selected, this contains
	   the mailbox's untagged_context. */
	void *untagged_box_context;
};

/* Called when tagged reply is received for command. */
typedef void imapc_command_callback_t(const struct imapc_command_reply *reply,
				      void *context);
/* Called each time untagged input is received. */
typedef void imapc_untagged_callback_t(const struct imapc_untagged_reply *reply,
				       void *context);

struct imapc_client *
imapc_client_init(const struct imapc_client_settings *set);
void imapc_client_disconnect(struct imapc_client *client);
void imapc_client_deinit(struct imapc_client **client);

/* Explicitly login to server (also done automatically). */
void imapc_client_login(struct imapc_client *client,
			imapc_command_callback_t *callback, void *context);

struct imapc_command *
imapc_client_cmd(struct imapc_client *client,
		 imapc_command_callback_t *callback, void *context);
void imapc_command_set_flags(struct imapc_command *cmd,
			     enum imapc_command_flags flags);
void imapc_command_send(struct imapc_command *cmd, const char *cmd_str);
void imapc_command_sendf(struct imapc_command *cmd, const char *cmd_fmt, ...)
	ATTR_FORMAT(2, 3);
void imapc_command_sendvf(struct imapc_command *cmd,
			  const char *cmd_fmt, va_list args) ATTR_FORMAT(2, 0);

void imapc_client_register_untagged(struct imapc_client *client,
				    imapc_untagged_callback_t *callback,
				    void *context);

void imapc_client_run(struct imapc_client *client);
void imapc_client_stop(struct imapc_client *client);
bool imapc_client_is_running(struct imapc_client *client);

struct imapc_client_mailbox *
imapc_client_mailbox_open(struct imapc_client *client,
			  void *untagged_box_context);
void imapc_client_mailbox_set_reopen_cb(struct imapc_client_mailbox *box,
					void (*callback)(void *context),
					void *context);
void imapc_client_mailbox_close(struct imapc_client_mailbox **box);
void imapc_client_mailbox_reconnect(struct imapc_client_mailbox *box);
struct imapc_command *
imapc_client_mailbox_cmd(struct imapc_client_mailbox *box,
			 imapc_command_callback_t *callback, void *context);
struct imapc_msgmap *
imapc_client_mailbox_get_msgmap(struct imapc_client_mailbox *box);

void imapc_client_mailbox_idle(struct imapc_client_mailbox *box);
bool imapc_client_mailbox_is_opened(struct imapc_client_mailbox *box);

enum imapc_capability
imapc_client_get_capabilities(struct imapc_client *client);

int imapc_client_create_temp_fd(struct imapc_client *client,
				const char **path_r);

#endif
