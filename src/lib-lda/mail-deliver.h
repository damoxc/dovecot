#ifndef MAIL_DELIVER_H
#define MAIL_DELIVER_H

enum mail_flags;
struct mail_storage;

struct mail_deliver_context {
	pool_t pool;
	const struct lda_settings *set;

	/* Mail to save */
	struct mail *src_mail;
	/* Envelope sender, if known. */
	const char *src_envelope_sender;

	/* Destination user */
	struct mail_user *dest_user;
	/* Destination email address */
	const char *dest_addr;
	/* Mailbox where mail should be saved, unless e.g. Sieve does
	   something to it. */
	const char *dest_mailbox_name;

	bool tried_default_save;
	bool saved_mail;
};

typedef int deliver_mail_func_t(struct mail_deliver_context *ctx,
				struct mail_storage **storage_r);

extern deliver_mail_func_t *deliver_mail;

void mail_deliver_log(struct mail_deliver_context *ctx, const char *fmt, ...)
	ATTR_FORMAT(2, 3);

const char *mail_deliver_get_address(struct mail_deliver_context *ctx,
				     const char *header);
const char *mail_deliver_get_return_address(struct mail_deliver_context *ctx);
const char *mail_deliver_get_new_message_id(struct mail_deliver_context *ctx);

int mail_deliver_save(struct mail_deliver_context *ctx, const char *mailbox,
		      enum mail_flags flags, const char *const *keywords,
		      struct mail_storage **storage_r);

int mail_deliver(struct mail_deliver_context *ctx,
		 struct mail_storage **storage_r);

#endif
