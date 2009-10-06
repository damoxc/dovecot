#ifndef DBOX_SAVE_H
#define DBOX_SAVE_H

struct dbox_save_context {
	struct mail_save_context ctx;
	struct mail_index_transaction *trans;

	/* updated for each appended mail: */
	uint32_t seq;
	struct istream *input;
	struct mail *mail;

	struct dbox_file *cur_file;
	struct ostream *cur_output;

	unsigned int failed:1;
	unsigned int finished:1;
};

void dbox_save_begin(struct dbox_save_context *ctx, struct istream *input);
int dbox_save_continue(struct mail_save_context *_ctx);

void dbox_save_write_metadata(struct mail_save_context *ctx,
			      struct ostream *output,
			      const char *orig_mailbox_name,
			      uint8_t guid_128_r[MAIL_GUID_128_SIZE]);

void dbox_save_add_to_index(struct dbox_save_context *ctx);

#endif
