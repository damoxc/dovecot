/* Copyright (c) 2002-2009 Dovecot authors, see the included COPYING file */

#include "lib.h"
#include "array.h"
#include "mail-index-modseq.h"
#include "index-storage.h"
#include "index-mail.h"

void index_storage_get_seq_range(struct mailbox *box,
				 uint32_t uid1, uint32_t uid2,
				 uint32_t *seq1_r, uint32_t *seq2_r)
{
	struct index_mailbox *ibox = (struct index_mailbox *)box;

	mail_index_lookup_seq_range(ibox->view, uid1, uid2, seq1_r, seq2_r);
}

void index_storage_get_uid_range(struct mailbox *box,
				 const ARRAY_TYPE(seq_range) *seqs,
				 ARRAY_TYPE(seq_range) *uids)
{
	struct index_mailbox *ibox = (struct index_mailbox *)box;
	const struct seq_range *range;
	unsigned int i, count;
	uint32_t seq, uid;

	range = array_get(seqs, &count);
	for (i = 0; i < count; i++) {
		if (range[i].seq2 == (uint32_t)-1) {
			i_assert(count == i-1);
			mail_index_lookup_uid(ibox->view, range[i].seq1, &uid);
			seq_range_array_add_range(uids, uid, (uint32_t)-1);
			break;
		}
		for (seq = range[i].seq1; seq <= range[i].seq2; seq++) {
			mail_index_lookup_uid(ibox->view, seq, &uid);
			seq_range_array_add(uids, 0, uid);
		}
	}
}

static void
add_expunges(ARRAY_TYPE(mailbox_expunge_rec) *expunges,
	     const struct mail_transaction_expunge *src, size_t src_size,
	     const ARRAY_TYPE(seq_range) *uids_filter)
{
	const struct mail_transaction_expunge *end;
	struct mailbox_expunge_rec *expunge;
	uint32_t uid;

	end = src + src_size / sizeof(*src);
	for (; src != end; src++) {
		for (uid = src->uid1; uid < src->uid2; uid++) {
			if (seq_range_exists(uids_filter, uid)) {
				expunge = array_append_space(expunges);
				expunge->uid = uid;
			}
		}
	}
}

static void
add_guid_expunges(ARRAY_TYPE(mailbox_expunge_rec) *expunges,
		  const struct mail_transaction_expunge_guid *src,
		  size_t src_size, const ARRAY_TYPE(seq_range) *uids_filter)
{
	const struct mail_transaction_expunge_guid *end;
	struct mailbox_expunge_rec *expunge;

	end = src + src_size / sizeof(*src);
	for (; src != end; src++) {
		if (seq_range_exists(uids_filter, src->uid)) {
			expunge = array_append_space(expunges);
			expunge->uid = src->uid;
			memcpy(expunge->guid_128, src->guid_128,
			       sizeof(expunge->guid_128));
		}
	}
}

bool index_storage_get_expunges(struct mailbox *box, uint64_t prev_modseq,
				const ARRAY_TYPE(seq_range) *uids_filter,
				ARRAY_TYPE(mailbox_expunge_rec) *expunges)
{
#define EXPUNGE_MASK (MAIL_TRANSACTION_EXPUNGE | MAIL_TRANSACTION_EXTERNAL)
	struct index_mailbox *ibox = (struct index_mailbox *)box;
	struct mail_transaction_log_view *log_view;
	const struct mail_transaction_header *thdr;
	const void *tdata;
	uint32_t log_seq;
	uoff_t log_offset;
	bool reset;

	if (!mail_index_modseq_get_next_log_offset(ibox->view, prev_modseq,
						   &log_seq, &log_offset))
		return FALSE;
	if (log_seq > ibox->view->log_file_head_seq ||
	    (log_seq == ibox->view->log_file_head_seq &&
	     log_offset >= ibox->view->log_file_head_offset)) {
		/* we haven't seen this high expunges at all */
		return TRUE;
	}

	log_view = mail_transaction_log_view_open(ibox->index->log);
	if (mail_transaction_log_view_set(log_view, log_seq, log_offset,
					  ibox->view->log_file_head_seq,
					  ibox->view->log_file_head_offset, 
					  &reset) <= 0) {
		mail_transaction_log_view_close(&log_view);
		return FALSE;
	}

	while (mail_transaction_log_view_next(log_view, &thdr, &tdata) > 0) {
		if ((thdr->type & MAIL_TRANSACTION_EXTERNAL) == 0) {
			/* skip expunge requests */
			continue;
		}
		switch (thdr->type) {
		case MAIL_TRANSACTION_EXPUNGE:
			add_expunges(expunges, tdata, thdr->size, uids_filter);
			break;
		case MAIL_TRANSACTION_EXPUNGE_GUID:
			add_guid_expunges(expunges, tdata, thdr->size,
					  uids_filter);
			break;
		}
	}

	mail_transaction_log_view_close(&log_view);
	return TRUE;
}
