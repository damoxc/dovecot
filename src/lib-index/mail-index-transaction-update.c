/* Copyright (c) 2003-2009 Dovecot authors, see the included COPYING file */

/* Inside transaction we keep messages stored in sequences in uid fields.
   Before they're written to transaction log the sequences are changed to
   UIDs. */

#include "lib.h"
#include "ioloop.h"
#include "array.h"
#include "mail-index-private.h"
#include "mail-index-transaction-private.h"

static bool
mail_index_transaction_has_ext_changes(struct mail_index_transaction *t);

struct mail_index_record *
mail_index_transaction_lookup(struct mail_index_transaction *t, uint32_t seq)
{
	i_assert(seq >= t->first_new_seq && seq <= t->last_new_seq);

	return array_idx_modifiable(&t->appends, seq - t->first_new_seq);
}

void mail_index_transaction_reset_v(struct mail_index_transaction *t)
{
	ARRAY_TYPE(seq_array) *recs;
	struct mail_index_transaction_ext_hdr_update *ext_hdrs;
	unsigned i, count;

	if (array_is_created(&t->ext_rec_updates)) {
		recs = array_get_modifiable(&t->ext_rec_updates, &count);
		for (i = 0; i < count; i++) {
			if (array_is_created(&recs[i]))
				array_free(&recs[i]);
		}
		array_free(&t->ext_rec_updates);
	}
	if (array_is_created(&t->ext_rec_atomics)) {
		recs = array_get_modifiable(&t->ext_rec_atomics, &count);
		for (i = 0; i < count; i++) {
			if (array_is_created(&recs[i]))
				array_free(&recs[i]);
		}
		array_free(&t->ext_rec_atomics);
	}
	if (array_is_created(&t->ext_hdr_updates)) {
		ext_hdrs = array_get_modifiable(&t->ext_hdr_updates, &count);
		for (i = 0; i < count; i++) {
			i_free(ext_hdrs[i].data);
			i_free(ext_hdrs[i].mask);
		}
		array_free(&t->ext_hdr_updates);
	}

	if (array_is_created(&t->keyword_updates)) {
		struct mail_index_transaction_keyword_update *u;

		u = array_get_modifiable(&t->keyword_updates, &count);
		for (i = 0; i < count; i++) {
			if (array_is_created(&u[i].add_seq))
				array_free(&u[i].add_seq);
			if (array_is_created(&u[i].remove_seq))
				array_free(&u[i].remove_seq);
		}
		array_free(&t->keyword_updates);
	}
	if (array_is_created(&t->keyword_resets))
		array_free(&t->keyword_resets);

	if (array_is_created(&t->appends))
		array_free(&t->appends);
	if (array_is_created(&t->expunges))
		array_free(&t->expunges);
	if (array_is_created(&t->updates))
		array_free(&t->updates);
	if (array_is_created(&t->ext_resizes))
		array_free(&t->ext_resizes);
	if (array_is_created(&t->ext_resets))
		array_free(&t->ext_resets);
	if (array_is_created(&t->ext_reset_ids))
		array_free(&t->ext_reset_ids);
	if (array_is_created(&t->ext_reset_atomic))
		array_free(&t->ext_reset_atomic);

	t->first_new_seq = mail_index_view_get_messages_count(t->view)+1;
	t->last_new_seq = 0;
	t->last_update_idx = 0;
	t->min_flagupdate_seq = 0;
	t->max_flagupdate_seq = 0;

	memset(t->pre_hdr_mask, 0, sizeof(t->pre_hdr_mask));
	memset(t->post_hdr_mask, 0, sizeof(t->post_hdr_mask));

	t->appends_nonsorted = FALSE;
	t->expunges_nonsorted = FALSE;
	t->drop_unnecessary_flag_updates = FALSE;
	t->pre_hdr_changed = FALSE;
	t->post_hdr_changed = FALSE;
	t->reset = FALSE;
	t->log_updates = FALSE;
	t->log_ext_updates = FALSE;
}

void mail_index_transaction_set_log_updates(struct mail_index_transaction *t)
{
	/* flag updates aren't included in log_updates */
	t->log_updates = array_is_created(&t->appends) ||
		array_is_created(&t->expunges) ||
		array_is_created(&t->keyword_resets) ||
		array_is_created(&t->keyword_updates) ||
		t->pre_hdr_changed || t->post_hdr_changed;
}

void mail_index_update_day_headers(struct mail_index_transaction *t)
{
	struct mail_index_header hdr;
	const struct mail_index_record *rec;
	const int max_days = N_ELEMENTS(hdr.day_first_uid);
	struct tm tm;
	time_t stamp;
	int i, days;

	hdr = *mail_index_get_header(t->view);
	rec = array_idx(&t->appends, 0);

	/* get beginning of today */
	tm = *localtime(&ioloop_time);
	tm.tm_hour = 0;
	tm.tm_min = 0;
	tm.tm_sec = 0;
	stamp = mktime(&tm);
	i_assert(stamp != (time_t)-1);

	if ((time_t)hdr.day_stamp >= stamp)
		return;

	/* get number of days since last message */
	days = (stamp - hdr.day_stamp) / (3600*24);
	if (days > max_days)
		days = max_days;

	/* @UNSAFE: move days forward and fill the missing days with old
	   day_first_uid[0]. */
	memmove(hdr.day_first_uid + days, hdr.day_first_uid, max_days - days);
	for (i = 1; i < days; i++)
		hdr.day_first_uid[i] = hdr.day_first_uid[0];

	hdr.day_stamp = stamp;
	hdr.day_first_uid[0] = rec->uid;

	mail_index_update_header(t,
		offsetof(struct mail_index_header, day_stamp),
		&hdr.day_stamp, sizeof(hdr.day_stamp), FALSE);
	mail_index_update_header(t,
		offsetof(struct mail_index_header, day_first_uid),
		hdr.day_first_uid, sizeof(hdr.day_first_uid), FALSE);
}

void mail_index_append(struct mail_index_transaction *t, uint32_t uid,
		       uint32_t *seq_r)
{
        struct mail_index_record *rec;

	i_assert(!t->no_appends);

	t->log_updates = TRUE;

	if (!array_is_created(&t->appends))
		i_array_init(&t->appends, 32);

	/* sequence number is visible only inside given view,
	   so let it generate it */
	if (t->last_new_seq != 0)
		*seq_r = ++t->last_new_seq;
	else
		*seq_r = t->last_new_seq = t->first_new_seq;

	rec = array_append_space(&t->appends);
	if (uid != 0) {
		rec->uid = uid;
		if (!t->appends_nonsorted &&
		    t->last_new_seq != t->first_new_seq) {
			/* if previous record's UID is larger than this one,
			   we'll have to sort the appends later */
			rec = mail_index_transaction_lookup(t, *seq_r - 1);
			if (rec->uid > uid)
				t->appends_nonsorted = TRUE;
			else if (rec->uid == uid)
				i_panic("Duplicate UIDs added in transaction");
		}
		if (t->highest_append_uid < uid)
			t->highest_append_uid = uid;
	}
}

void mail_index_append_assign_uids(struct mail_index_transaction *t,
				   uint32_t first_uid, uint32_t *next_uid_r)
{
	struct mail_index_record *recs;
	unsigned int i, count;

	if (!array_is_created(&t->appends))
		return;

	i_assert(first_uid > t->highest_append_uid);

	recs = array_get_modifiable(&t->appends, &count);
	for (i = 0; i < count; i++) {
		if (recs[i].uid == 0)
			recs[i].uid = first_uid++;
	}

	*next_uid_r = first_uid;
}

static void
mail_index_expunge_last_append_ext(ARRAY_TYPE(seq_array_array) *ext_updates,
				   uint32_t seq)
{
	ARRAY_TYPE(seq_array) *seqs;
	unsigned int i, count, idx;

	if (!array_is_created(ext_updates))
		return;

	seqs = array_get_modifiable(ext_updates, &count);
	for (i = 0; i < count; i++) {
		if (array_is_created(&seqs[i]) &&
		    mail_index_seq_array_lookup(&seqs[i], seq, &idx))
			array_delete(&seqs[i], idx, 1);
	}
}

static void
mail_index_expunge_last_append(struct mail_index_transaction *t, uint32_t seq)
{
	struct mail_index_transaction_keyword_update *kw_updates;
	unsigned int i, count;

	i_assert(seq == t->last_new_seq);

	/* remove extension updates */
	mail_index_expunge_last_append_ext(&t->ext_rec_updates, seq);
	mail_index_expunge_last_append_ext(&t->ext_rec_atomics, seq);
	t->log_ext_updates = mail_index_transaction_has_ext_changes(t);

	/* remove keywords */
	if (array_is_created(&t->keyword_resets))
		seq_range_array_remove(&t->keyword_resets, seq);
	if (array_is_created(&t->keyword_updates)) {
		kw_updates = array_get_modifiable(&t->keyword_updates, &count);
		for (i = 0; i < count; i++) {
			if (array_is_created(&kw_updates[i].add_seq)) {
				seq_range_array_remove(&kw_updates[i].add_seq,
						       seq);
			}
			if (array_is_created(&kw_updates[i].remove_seq)) {
				seq_range_array_remove(
					&kw_updates[i].remove_seq, seq);
			}
		}
	}

	/* and finally remove the append itself */
	array_delete(&t->appends, seq - t->first_new_seq, 1);
	t->last_new_seq--;
	if (t->first_new_seq > t->last_new_seq) {
		t->last_new_seq = 0;
		t->appends_nonsorted = FALSE;
		array_free(&t->appends);
	}
	mail_index_transaction_set_log_updates(t);
}

void mail_index_expunge(struct mail_index_transaction *t, uint32_t seq)
{
	static uint8_t null_guid[MAIL_GUID_128_SIZE] =
		{ 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 };
	mail_index_expunge_guid(t, seq, null_guid);
}

void mail_index_expunge_guid(struct mail_index_transaction *t, uint32_t seq,
			     const uint8_t guid_128[MAIL_GUID_128_SIZE])
{
	const struct mail_transaction_expunge_guid *expunges;
	struct mail_transaction_expunge_guid *expunge;
	unsigned int count;

	i_assert(seq > 0);
	if (seq >= t->first_new_seq) {
		/* we can handle only the last append. otherwise we'd have to
		   renumber sequences and that gets tricky. for now this is
		   enough, since we typically want to expunge all the
		   appends. */
		mail_index_expunge_last_append(t, seq);
	} else {
		t->log_updates = TRUE;

		/* ignore duplicates here. drop them when commiting. */
		if (!array_is_created(&t->expunges))
			i_array_init(&t->expunges, 64);
		else if (!t->expunges_nonsorted) {
			/* usually expunges are added in increasing order. */
			expunges = array_get(&t->expunges, &count);
			if (count > 0 && seq < expunges[count-1].uid)
				t->expunges_nonsorted = TRUE;
		}
		expunge = array_append_space(&t->expunges);
		expunge->uid = seq;
		memcpy(expunge->guid_128, guid_128, sizeof(expunge->guid_128));
	}
}

static void update_minmax_flagupdate_seq(struct mail_index_transaction *t,
					 uint32_t seq1, uint32_t seq2)
{
	if (t->min_flagupdate_seq == 0) {
		t->min_flagupdate_seq = seq1;
		t->max_flagupdate_seq = seq2;
	} else {
		if (t->min_flagupdate_seq > seq1)
			t->min_flagupdate_seq = seq1;
		if (t->max_flagupdate_seq < seq2)
			t->max_flagupdate_seq = seq2;
	}
}

unsigned int
mail_index_transaction_get_flag_update_pos(struct mail_index_transaction *t,
					   unsigned int left_idx,
					   unsigned int right_idx,
					   uint32_t seq)
{
	const struct mail_transaction_flag_update *updates;
	unsigned int idx, count;

	updates = array_get(&t->updates, &count);
	i_assert(left_idx <= right_idx && right_idx <= count);

	/* find the first update with either overlapping range,
	   or the update which will come after our insert */
	idx = left_idx;
	while (left_idx < right_idx) {
		idx = (left_idx + right_idx) / 2;

		if (updates[idx].uid2 < seq)
			left_idx = idx+1;
		else if (updates[idx].uid1 > seq)
			right_idx = idx;
		else
			break;
	}
	if (left_idx > idx)
		idx++;
	return idx;
}

static void
mail_index_insert_flag_update(struct mail_index_transaction *t,
			      struct mail_transaction_flag_update u,
			      unsigned int idx)
{
	struct mail_transaction_flag_update *updates, tmp_update;
	unsigned int count, first_idx, max;

	updates = array_get_modifiable(&t->updates, &count);

	/* overlapping ranges, split/merge them */
	i_assert(idx == 0 || updates[idx-1].uid2 < u.uid1);
	i_assert(idx == count || updates[idx].uid2 >= u.uid1);

	/* first we'll just add the changes without trying to merge anything */
	first_idx = idx;
	for (; idx < count && u.uid2 >= updates[idx].uid1; idx++) {
		i_assert(u.uid1 <= updates[idx].uid2);
		if (u.uid1 != updates[idx].uid1 &&
		    (updates[idx].add_flags != u.add_flags ||
		     updates[idx].remove_flags != u.remove_flags)) {
			if (u.uid1 < updates[idx].uid1) {
				/* insert new update */
				tmp_update = u;
				tmp_update.uid2 = updates[idx].uid1 - 1;
			} else {
				/* split existing update from beginning */
				tmp_update = updates[idx];
				tmp_update.uid2 = u.uid1 - 1;
				updates[idx].uid1 = u.uid1;
			}

			i_assert(tmp_update.uid1 <= tmp_update.uid2);
			i_assert(updates[idx].uid1 <= updates[idx].uid2);

			array_insert(&t->updates, idx, &tmp_update, 1);
			updates = array_get_modifiable(&t->updates, &count);
			idx++;
		} else if (u.uid1 < updates[idx].uid1) {
			updates[idx].uid1 = u.uid1;
		}

		if (u.uid2 < updates[idx].uid2 &&
		    (updates[idx].add_flags != u.add_flags ||
		     updates[idx].remove_flags != u.remove_flags)) {
			/* split existing update from end */
			tmp_update = updates[idx];
			tmp_update.uid2 = u.uid2;
			updates[idx].uid1 = u.uid2 + 1;

			i_assert(tmp_update.uid1 <= tmp_update.uid2);
			i_assert(updates[idx].uid1 <= updates[idx].uid2);

			array_insert(&t->updates, idx, &tmp_update, 1);
			updates = array_get_modifiable(&t->updates, &count);
		}

		updates[idx].add_flags =
			(updates[idx].add_flags | u.add_flags) &
			~u.remove_flags;
		updates[idx].remove_flags =
			(updates[idx].remove_flags | u.remove_flags) &
			~u.add_flags;
		u.uid1 = updates[idx].uid2 + 1;

		if (updates[idx].add_flags == 0 &&
		    updates[idx].remove_flags == 0) {
			/* we can remove this update completely */
			array_delete(&t->updates, idx, 1);
			updates = array_get_modifiable(&t->updates, &count);
		}

		if (u.uid1 > u.uid2) {
			/* break here before idx++ so last_update_idx is set
			   correctly */
			break;
		}
	}
	i_assert(idx <= count);

	if (u.uid1 <= u.uid2) {
		i_assert(idx == 0 || updates[idx-1].uid2 < u.uid1);
		i_assert(idx == count || updates[idx].uid1 > u.uid2);
		array_insert(&t->updates, idx, &u, 1);
	}
	updates = array_get_modifiable(&t->updates, &count);
	t->last_update_idx = idx == count ? count-1 : idx;

	/* merge everything */
	idx = first_idx == 0 ? 0 : first_idx - 1;
	max = I_MIN(t->last_update_idx + 1, count);
	for (; idx < max; ) {
		if (updates[idx].uid2 + 1 == updates[idx+1].uid1 &&
		    updates[idx].add_flags == updates[idx+1].add_flags &&
		    updates[idx].remove_flags == updates[idx+1].remove_flags) {
			/* merge */
			updates[idx].uid2 = updates[idx+1].uid2;
			array_delete(&t->updates, idx + 1, 1);
			max--;
			if (t->last_update_idx > idx)
				t->last_update_idx--;
			updates = array_get_modifiable(&t->updates, &count);
		} else {
			idx++;
		}
	}
}

static void mail_index_record_modify_flags(struct mail_index_record *rec,
					   enum modify_type modify_type,
					   enum mail_flags flags)
{
	switch (modify_type) {
	case MODIFY_REPLACE:
		rec->flags = flags;
		break;
	case MODIFY_ADD:
		rec->flags |= flags;
		break;
	case MODIFY_REMOVE:
		rec->flags &= ~flags;
		break;
	}
}

void mail_index_update_flags_range(struct mail_index_transaction *t,
				   uint32_t seq1, uint32_t seq2,
				   enum modify_type modify_type,
				   enum mail_flags flags)
{
	struct mail_index_record *rec;
	struct mail_transaction_flag_update u, *last_update;
	unsigned int idx, first_idx, count;

	update_minmax_flagupdate_seq(t, seq1, seq2);
	if (seq2 >= t->first_new_seq) {
		/* updates for appended messages, modify them directly */
		uint32_t seq;

		for (seq = I_MAX(t->first_new_seq, seq1); seq <= seq2; seq++) {
			rec = mail_index_transaction_lookup(t, seq);
			mail_index_record_modify_flags(rec, modify_type, flags);
		}
		if (seq1 >= t->first_new_seq)
			return;

		/* range contains also existing messages. update them next. */
		seq2 = t->first_new_seq - 1;
	}

	i_assert(seq1 <= seq2 && seq1 > 0);
	i_assert(seq2 <= mail_index_view_get_messages_count(t->view));

	if ((t->flags & MAIL_INDEX_TRANSACTION_FLAG_AVOID_FLAG_UPDATES) != 0)
		t->drop_unnecessary_flag_updates = TRUE;

	memset(&u, 0, sizeof(u));
	u.uid1 = seq1;
	u.uid2 = seq2;

	switch (modify_type) {
	case MODIFY_REPLACE:
		u.add_flags = flags;
		u.remove_flags = ~flags & MAIL_INDEX_FLAGS_MASK;
		break;
	case MODIFY_ADD:
		if (flags == 0)
			return;
		u.add_flags = flags;
		break;
	case MODIFY_REMOVE:
		if (flags == 0)
			return;
		u.remove_flags = flags;
		break;
	}

	if (!array_is_created(&t->updates)) {
		i_array_init(&t->updates, 256);
		array_append(&t->updates, &u, 1);
		return;
	}

	last_update = array_get_modifiable(&t->updates, &count);
	if (t->last_update_idx < count) {
		/* fast path - hopefully we're updating the next message,
		   or a message that is to be appended as last update */
		last_update += t->last_update_idx;
		if (seq1 - 1 == last_update->uid2) {
			if (u.add_flags == last_update->add_flags &&
			    u.remove_flags == last_update->remove_flags &&
			    (t->last_update_idx + 1 == count ||
			     last_update[1].uid1 > seq2)) {
				/* we can just update the UID range */
				last_update->uid2 = seq2;
				return;
			}
		} else if (seq1 > last_update->uid2) {
			/* hopefully we can just append it */
			t->last_update_idx++;
			last_update++;
		}
	}

	if (t->last_update_idx == count)
		array_append(&t->updates, &u, 1);
	else {
		i_assert(t->last_update_idx < count);

		/* slow path */
		if (seq1 > last_update->uid2) {
			/* added after this */
			first_idx = t->last_update_idx + 1;
		} else {
			/* added before this or on top of this */
			first_idx = 0;
			count = t->last_update_idx + 1;
		}
		idx = mail_index_transaction_get_flag_update_pos(t, first_idx,
								 count, u.uid1);
		mail_index_insert_flag_update(t, u, idx);
	}
}

void mail_index_update_flags(struct mail_index_transaction *t, uint32_t seq,
			     enum modify_type modify_type,
			     enum mail_flags flags)
{
	mail_index_update_flags_range(t, seq, seq, modify_type, flags);
}

void mail_index_update_header(struct mail_index_transaction *t,
			      size_t offset, const void *data, size_t size,
			      bool prepend)
{
	i_assert(offset < sizeof(t->pre_hdr_change));
	i_assert(size <= sizeof(t->pre_hdr_change) - offset);

	t->log_updates = TRUE;

	if (prepend) {
		t->pre_hdr_changed = TRUE;
		memcpy(t->pre_hdr_change + offset, data, size);
		for (; size > 0; size--)
			t->pre_hdr_mask[offset++] = 1;
	} else {
		t->post_hdr_changed = TRUE;
		memcpy(t->post_hdr_change + offset, data, size);
		for (; size > 0; size--)
			t->post_hdr_mask[offset++] = 1;
	}
}

void mail_index_ext_resize(struct mail_index_transaction *t, uint32_t ext_id,
			   uint32_t hdr_size, uint16_t record_size,
			   uint16_t record_align)
{
	struct mail_transaction_ext_intro intro;
	uint32_t old_record_size, old_record_align;

	memset(&intro, 0, sizeof(intro));

	/* get ext_id from transaction's map if it's there */
	if (!mail_index_map_get_ext_idx(t->view->map, ext_id, &intro.ext_id)) {
		/* have to create it */
		const struct mail_index_registered_ext *rext;

		intro.ext_id = (uint32_t)-1;
		rext = array_idx(&t->view->index->extensions, ext_id);
		old_record_size = rext->record_size;
		old_record_align = rext->record_align;
	} else {
		const struct mail_index_ext *ext;

		ext = array_idx(&t->view->map->extensions, intro.ext_id);
		old_record_size = ext->record_size;
		old_record_align = ext->record_align;
	}

	/* allow only header size changes if extension records have already
	   been changed in transaction */
	i_assert(!array_is_created(&t->ext_rec_updates) ||
		 (old_record_size == record_size &&
		  old_record_align == record_align));

	t->log_ext_updates = TRUE;

	if (!array_is_created(&t->ext_resizes))
		i_array_init(&t->ext_resizes, ext_id + 2);

	intro.hdr_size = hdr_size;
	intro.record_size = record_size;
	intro.record_align = record_align;
	intro.name_size = 1;
	array_idx_set(&t->ext_resizes, ext_id, &intro);
}

void mail_index_ext_reset(struct mail_index_transaction *t, uint32_t ext_id,
			  uint32_t reset_id, bool clear_data)
{
	struct mail_transaction_ext_reset reset;

	i_assert(reset_id != 0);

	memset(&reset, 0, sizeof(reset));
	reset.new_reset_id = reset_id;
	reset.preserve_data = !clear_data;

	mail_index_ext_set_reset_id(t, ext_id, reset_id);

	if (!array_is_created(&t->ext_resets))
		i_array_init(&t->ext_resets, ext_id + 2);
	array_idx_set(&t->ext_resets, ext_id, &reset);
	t->log_ext_updates = TRUE;
}

void mail_index_ext_reset_inc(struct mail_index_transaction *t, uint32_t ext_id,
			      uint32_t prev_reset_id, bool clear_data)
{
	uint32_t expected_reset_id = prev_reset_id + 1;

	mail_index_ext_reset(t, ext_id, (uint32_t)-1, clear_data);

	if (!array_is_created(&t->ext_reset_atomic))
		i_array_init(&t->ext_reset_atomic, ext_id + 2);
	array_idx_set(&t->ext_reset_atomic, ext_id, &expected_reset_id);
}

static bool
mail_index_transaction_has_ext_updates(const ARRAY_TYPE(seq_array_array) *arr)
{
	const ARRAY_TYPE(seq_array) *array;
	unsigned int i, count;

	if (array_is_created(arr)) {
		array = array_get(arr, &count);
		for (i = 0; i < count; i++) {
			if (array_is_created(&array[i]))
				return TRUE;
		}
	}
	return FALSE;
}

static bool
mail_index_transaction_has_ext_changes(struct mail_index_transaction *t)
{
	unsigned int i, count;

	if (mail_index_transaction_has_ext_updates(&t->ext_rec_updates))
		return TRUE;
	if (mail_index_transaction_has_ext_updates(&t->ext_rec_atomics))
		return TRUE;

	if (array_is_created(&t->ext_hdr_updates)) {
		const struct mail_index_transaction_ext_hdr_update *hdr;

		hdr = array_get(&t->ext_hdr_updates, &count);
		for (i = 0; i < count; i++) {
			if (hdr[i].alloc_size > 0)
				return TRUE;
		}
	}
	if (array_is_created(&t->ext_resets)) {
		const struct mail_transaction_ext_reset *resets;

		resets = array_get(&t->ext_resets, &count);
		for (i = 0; i < count; i++) {
			if (resets[i].new_reset_id != 0)
				return TRUE;
		}
	}
	if (array_is_created(&t->ext_resizes)) {
		const struct mail_transaction_ext_intro *resizes;

		resizes = array_get(&t->ext_resizes, &count);
		for (i = 0; i < count; i++) {
			if (resizes[i].name_size > 0)
				return TRUE;
		}
	}
	return FALSE;
}

static void
mail_index_ext_update_reset(ARRAY_TYPE(seq_array_array) *arr, uint32_t ext_id)
{
	if (array_is_created(arr) && ext_id < array_count(arr)) {
		/* if extension records have been updated, clear them */
		ARRAY_TYPE(seq_array) *array;

		array = array_idx_modifiable(arr, ext_id);
		if (array_is_created(array))
			array_clear(array);
	}
}

void mail_index_ext_set_reset_id(struct mail_index_transaction *t,
				 uint32_t ext_id, uint32_t reset_id)
{
	mail_index_ext_update_reset(&t->ext_rec_updates, ext_id);
	mail_index_ext_update_reset(&t->ext_rec_atomics, ext_id);
	if (array_is_created(&t->ext_hdr_updates) &&
	    ext_id < array_count(&t->ext_hdr_updates)) {
		/* if extension headers have been updated, clear them */
		struct mail_index_transaction_ext_hdr_update *hdr;

		hdr = array_idx_modifiable(&t->ext_hdr_updates, ext_id);
		if (hdr->alloc_size > 0) {
			i_free_and_null(hdr->mask);
			i_free_and_null(hdr->data);
		}
		hdr->alloc_size = 0;
	}
	if (array_is_created(&t->ext_resets) &&
	    ext_id < array_count(&t->ext_resets)) {
		/* clear resets */
		array_idx_clear(&t->ext_resets, ext_id);
	}
	if (array_is_created(&t->ext_resizes) &&
	    ext_id < array_count(&t->ext_resizes)) {
		/* clear resizes */
		array_idx_clear(&t->ext_resizes, ext_id);
	}

	if (!array_is_created(&t->ext_reset_ids))
		i_array_init(&t->ext_reset_ids, ext_id + 2);
	array_idx_set(&t->ext_reset_ids, ext_id, &reset_id);

	t->log_ext_updates = mail_index_transaction_has_ext_changes(t);
}

void mail_index_update_header_ext(struct mail_index_transaction *t,
				  uint32_t ext_id, size_t offset,
				  const void *data, size_t size)
{
	struct mail_index_transaction_ext_hdr_update *hdr;
	size_t new_size;

	i_assert(offset <= (uint16_t)-1 && size <= (uint16_t)-1 &&
		 offset + size <= (uint16_t)-1);

	if (!array_is_created(&t->ext_hdr_updates))
		i_array_init(&t->ext_hdr_updates, ext_id + 2);

	hdr = array_idx_modifiable(&t->ext_hdr_updates, ext_id);
	if (hdr->alloc_size < offset || hdr->alloc_size - offset < size) {
		i_assert(size < (size_t)-1 - offset);
		new_size = nearest_power(offset + size);
		hdr->mask = i_realloc(hdr->mask, hdr->alloc_size, new_size);
		hdr->data = i_realloc(hdr->data, hdr->alloc_size, new_size);
		hdr->alloc_size = new_size;
	}
	memset(hdr->mask + offset, 1, size);
	memcpy(hdr->data + offset, data, size);

	t->log_ext_updates = TRUE;
}

void mail_index_update_ext(struct mail_index_transaction *t, uint32_t seq,
			   uint32_t ext_id, const void *data, void *old_data_r)
{
	struct mail_index *index = t->view->index;
        const struct mail_index_registered_ext *rext;
	const struct mail_transaction_ext_intro *intro;
	uint16_t record_size;
	ARRAY_TYPE(seq_array) *array;
	unsigned int count;

	i_assert(seq > 0 &&
		 (seq <= mail_index_view_get_messages_count(t->view) ||
		  seq <= t->last_new_seq));
	i_assert(ext_id < array_count(&index->extensions));

	t->log_ext_updates = TRUE;

	if (!array_is_created(&t->ext_resizes)) {
		intro = NULL;
		count = 0;
	} else {
		intro = array_get(&t->ext_resizes, &count);
	}
	if (ext_id < count && intro[ext_id].name_size != 0) {
		/* resized record */
		record_size = intro[ext_id].record_size;
	} else {
		rext = array_idx(&index->extensions, ext_id);
		record_size = rext->record_size;
	}

	if (!array_is_created(&t->ext_rec_updates))
		i_array_init(&t->ext_rec_updates, ext_id + 2);
	array = array_idx_modifiable(&t->ext_rec_updates, ext_id);

	/* @UNSAFE */
	if (!mail_index_seq_array_add(array, seq, data, record_size,
				      old_data_r)) {
		/* not found, clear old_data if it was given */
		if (old_data_r != NULL)
			memset(old_data_r, 0, record_size);
	}
}

int mail_index_atomic_inc_ext(struct mail_index_transaction *t,
			      uint32_t seq, uint32_t ext_id, int diff)
{
	ARRAY_TYPE(seq_array) *array;
	int32_t old_diff32, diff32 = diff;

	i_assert(seq > 0 &&
		 (seq <= mail_index_view_get_messages_count(t->view) ||
		  seq <= t->last_new_seq));
	i_assert(ext_id < array_count(&t->view->index->extensions));
	/* currently non-external transactions can be applied multiple times,
	   causing multiple increments. */
	//FIXME:i_assert((t->flags & MAIL_INDEX_TRANSACTION_FLAG_EXTERNAL) != 0);

	t->log_ext_updates = TRUE;
	if (!array_is_created(&t->ext_rec_atomics))
		i_array_init(&t->ext_rec_atomics, ext_id + 2);
	array = array_idx_modifiable(&t->ext_rec_atomics, ext_id);
	if (mail_index_seq_array_add(array, seq, &diff32, sizeof(diff32),
				     &old_diff32)) {
		/* already incremented this sequence in this transaction */
		diff32 += old_diff32;
		mail_index_seq_array_add(array, seq, &diff32, sizeof(diff32),
					 NULL);
	}
	return diff32;
}

static bool
keyword_update_has_changes(struct mail_index_transaction *t, uint32_t seq,
			   enum modify_type modify_type,
			   struct mail_keywords *keywords)
{
	struct mail_index_transaction_keyword_update *u;
	ARRAY_TYPE(keyword_indexes) existing;
	const unsigned int *existing_idx;
	unsigned int i, j, existing_count;
	bool found;

	t_array_init(&existing, 32);
	if (seq < t->first_new_seq)
		mail_index_lookup_keywords(t->view, seq, &existing);
	existing_idx = array_get(&existing, &existing_count);

	if (modify_type == MODIFY_REPLACE && existing_count != keywords->count)
		return TRUE;

	for (i = 0; i < keywords->count; i++) {
		u = array_idx_modifiable(&t->keyword_updates,
					 keywords->idx[i]);
		if (array_is_created(&u->add_seq) ||
		    array_is_created(&u->remove_seq))
			return TRUE;

		found = FALSE;
		for (j = 0; j < existing_count; j++) {
			if (existing_idx[j] == keywords->idx[i]) {
				found = TRUE;
				break;
			}
		}
		switch (modify_type) {
		case MODIFY_ADD:
		case MODIFY_REPLACE:
			if (!found)
				return TRUE;
			break;
		case MODIFY_REMOVE:
			if (found)
				return TRUE;
			break;
		}
	}
	return FALSE;
}

void mail_index_update_keywords(struct mail_index_transaction *t, uint32_t seq,
				enum modify_type modify_type,
				struct mail_keywords *keywords)
{
	struct mail_index_transaction_keyword_update *u;
	unsigned int i, ku_count;
	bool changed;

	i_assert(seq > 0 &&
		 (seq <= mail_index_view_get_messages_count(t->view) ||
		  seq <= t->last_new_seq));
	i_assert(keywords->count > 0 || modify_type == MODIFY_REPLACE);
	i_assert(keywords->index == t->view->index);

	update_minmax_flagupdate_seq(t, seq, seq);

	if (!array_is_created(&t->keyword_updates) && keywords->count > 0) {
		uint32_t max_idx = keywords->idx[keywords->count-1];

		i_array_init(&t->keyword_updates, max_idx + 1);
	}

	if ((t->flags & MAIL_INDEX_TRANSACTION_FLAG_AVOID_FLAG_UPDATES) != 0) {
		T_BEGIN {
			changed = keyword_update_has_changes(t, seq,
							     modify_type,
							     keywords);
		} T_END;
		if (!changed)
			return;
	}

	/* Update add_seq and remove_seq arrays which describe the keyword
	   changes. Don't bother updating remove_seq or keyword resets for
	   newly added messages since they default to not having any
	   keywords anyway. */
	switch (modify_type) {
	case MODIFY_ADD:
		for (i = 0; i < keywords->count; i++) {
			u = array_idx_modifiable(&t->keyword_updates,
						 keywords->idx[i]);
			seq_range_array_add(&u->add_seq, 16, seq);
			seq_range_array_remove(&u->remove_seq, seq);
		}
		break;
	case MODIFY_REMOVE:
		for (i = 0; i < keywords->count; i++) {
			u = array_idx_modifiable(&t->keyword_updates,
						 keywords->idx[i]);
			seq_range_array_remove(&u->add_seq, seq);
			if (seq < t->first_new_seq)
				seq_range_array_add(&u->remove_seq, 16, seq);
		}
		break;
	case MODIFY_REPLACE:
		/* Remove sequence from all add/remove arrays */
		if (array_is_created(&t->keyword_updates)) {
			u = array_get_modifiable(&t->keyword_updates,
						 &ku_count);
			for (i = 0; i < ku_count; i++) {
				seq_range_array_remove(&u[i].add_seq, seq);
				seq_range_array_remove(&u[i].remove_seq, seq);
			}
		}
		/* Add the wanted keyword back */
		for (i = 0; i < keywords->count; i++) {
			u = array_idx_modifiable(&t->keyword_updates,
						 keywords->idx[i]);
			seq_range_array_add(&u->add_seq, 16, seq);
		}

		if (seq < t->first_new_seq)
			seq_range_array_add(&t->keyword_resets, 16, seq);
		break;
	}

	t->log_updates = TRUE;
}

void mail_index_transaction_reset(struct mail_index_transaction *t)
{
	t->v.reset(t);
}

void mail_index_reset(struct mail_index_transaction *t)
{
	mail_index_transaction_reset(t);

	t->reset = TRUE;
}

void mail_index_transaction_set_max_modseq(struct mail_index_transaction *t,
					   uint64_t max_modseq,
					   ARRAY_TYPE(seq_range) *seqs)
{
	i_assert(array_is_created(seqs));

	t->max_modseq = max_modseq;
	t->conflict_seqs = seqs;
}
