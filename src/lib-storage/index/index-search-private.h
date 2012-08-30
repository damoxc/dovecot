#ifndef INDEX_SEARCH_PRIVATE_H
#define INDEX_SEARCH_PRIVATE_H

#include "mail-storage-private.h"

#include <sys/time.h>

struct index_search_context {
        struct mail_search_context mail_ctx;
	struct mail_index_view *view;
	struct mailbox *box;

	uint32_t pvt_uid, pvt_seq;

	enum mail_fetch_field extra_wanted_fields;
	struct mailbox_header_lookup_ctx *extra_wanted_headers;

	uint32_t seq1, seq2;
	struct mail *cur_mail;
	struct index_mail *cur_imail;
	struct mail_thread_context *thread_ctx;

	ARRAY(struct mail *) mails;
	unsigned int unused_mail_idx;
	unsigned int max_mails;

	struct timeval search_start_time, last_notify;
	struct timeval last_nonblock_timeval;
	unsigned long long cost, next_time_check_cost;

	unsigned int failed:1;
	unsigned int sorted:1;
	unsigned int have_seqsets:1;
	unsigned int have_index_args:1;
	unsigned int have_mailbox_args:1;
};

struct mail *index_search_get_mail(struct index_search_context *ctx);

#endif
