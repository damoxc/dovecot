#ifndef __DBOX_STORAGE_H
#define __DBOX_STORAGE_H

#include "index-storage.h"
#include "dbox-format.h"

#define STORAGE(mbox_storage) \
	(&(mbox_storage)->storage.storage)
#define INDEX_STORAGE(mbox_storage) \
	(&(mbox_storage)->storage)

struct dbox_uidlist;

struct dbox_storage {
	struct index_storage storage;
};

struct keyword_map {
	unsigned int index_idx;
	unsigned int file_idx;
};

struct dbox_file {
	uint32_t file_seq;
	char *path;

	int fd;
	struct istream *input;
	struct ostream *output; /* while appending mails */

	uint16_t base_header_size;
	uint32_t header_size;
	time_t create_time;
	uint64_t append_offset;
	uint16_t mail_header_size;
	uint16_t mail_header_align;
	uint16_t keyword_count;
	uint64_t keyword_list_offset;
	uint32_t keyword_list_size_alloc;
	uint32_t keyword_list_size_used;
	struct dbox_file_header hdr;

	uoff_t seeked_offset;
	uoff_t seeked_mail_size;
	uint32_t seeked_uid;
	struct dbox_mail_header seeked_mail_header;
	unsigned char *seeked_keywords;

	/* Keywords list, sorted by index_idx. */
	array_t ARRAY_DEFINE(idx_file_keywords, struct keyword_map);
	/* idx -> index_idx array */
	array_t ARRAY_DEFINE(file_idx_keywords, unsigned int);
};

struct dbox_mailbox {
	struct index_mailbox ibox;
	struct dbox_storage *storage;
	struct dbox_uidlist *uidlist;

	const char *path;

        struct dbox_file *file;
	uint32_t dbox_file_ext_idx;
	uint32_t dbox_offset_ext_idx;

	uoff_t rotate_size, rotate_min_size;
	unsigned int rotate_days;
};

struct dbox_transaction_context {
	struct index_transaction_context ictx;

	uint32_t first_saved_mail_seq;
	struct dbox_save_context *save_ctx;
};

extern struct mail_vfuncs dbox_mail_vfuncs;

struct mailbox_list_context *
dbox_mailbox_list_init(struct mail_storage *storage,
		       const char *ref, const char *mask,
		       enum mailbox_list_flags flags);
int dbox_mailbox_list_deinit(struct mailbox_list_context *ctx);
struct mailbox_list *dbox_mailbox_list_next(struct mailbox_list_context *ctx);

struct mailbox_transaction_context *
dbox_transaction_begin(struct mailbox *box,
		       enum mailbox_transaction_flags flags);
int dbox_transaction_commit(struct mailbox_transaction_context *t,
			    enum mailbox_sync_flags flags);
void dbox_transaction_rollback(struct mailbox_transaction_context *t);

int dbox_save_init(struct mailbox_transaction_context *_t,
		   enum mail_flags flags, struct mail_keywords *keywords,
		   time_t received_date, int timezone_offset,
		   const char *from_envelope, struct istream *input,
		   struct mail *dest_mail, struct mail_save_context **ctx_r);
int dbox_save_continue(struct mail_save_context *ctx);
int dbox_save_finish(struct mail_save_context *ctx);
void dbox_save_cancel(struct mail_save_context *ctx);

int dbox_transaction_save_commit_pre(struct dbox_save_context *ctx);
void dbox_transaction_save_commit_post(struct dbox_save_context *ctx);
void dbox_transaction_save_rollback(struct dbox_save_context *ctx);

bool dbox_is_valid_mask(struct mail_storage *storage, const char *mask);

int dbox_mail_lookup_offset(struct index_transaction_context *trans,
			    uint32_t seq, uint32_t *file_seq_r,
			    uoff_t *offset_r);

#endif
