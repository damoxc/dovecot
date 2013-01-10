/* Copyright (c) 2012 Dovecot authors, see the included COPYING file */

#include "lib.h"
#include "array.h"
#include "hash.h"
#include "mail-index-modseq.h"
#include "mail-storage-private.h"
#include "mail-search-build.h"
#include "dsync-transaction-log-scan.h"
#include "dsync-mail.h"
#include "dsync-mailbox-export.h"

struct dsync_mail_guid_instances {
	ARRAY_TYPE(seq_range) seqs;
	bool requested;
	bool searched;
};

struct dsync_mailbox_exporter {
	pool_t pool;
	struct mailbox *box;
	struct dsync_transaction_log_scan *log_scan;
	uint32_t last_common_uid;

	struct mailbox_transaction_context *trans;
	struct mail_search_context *search_ctx;

	/* GUID => instances */
	HASH_TABLE(char *, struct dsync_mail_guid_instances *) export_guids;
	ARRAY_TYPE(seq_range) requested_uids;
	unsigned int requested_uid_search_idx;

	ARRAY_TYPE(seq_range) expunged_seqs;
	ARRAY_TYPE(const_string) expunged_guids;
	unsigned int expunged_guid_idx;

	/* uint32_t UID => struct dsync_mail_change */
	HASH_TABLE(void *, struct dsync_mail_change *) changes;
	/* changes sorted by UID */
	ARRAY(struct dsync_mail_change *) sorted_changes;
	unsigned int change_idx;
	uint32_t highest_changed_uid;

	struct dsync_mail_change change;
	struct dsync_mail dsync_mail;

	const char *error;
	unsigned int body_search_initialized:1;
	unsigned int auto_export_mails:1;
	unsigned int mails_have_guids:1;
	unsigned int return_all_mails:1;
};

static int dsync_mail_error(struct dsync_mailbox_exporter *exporter,
			    struct mail *mail, const char *field)
{
	const char *errstr;
	enum mail_error error;

	errstr = mailbox_get_last_error(exporter->box, &error);
	if (error == MAIL_ERROR_EXPUNGED)
		return 0;

	exporter->error = p_strdup_printf(exporter->pool,
		"Can't lookup %s for UID=%u: %s",
		field, mail->uid, errstr);
	return -1;
}

static bool
final_keyword_check(struct dsync_mail_change *change, const char *name)
{
	const char *const *changes;
	unsigned int i, count;

	changes = array_get(&change->keyword_changes, &count);
	for (i = 0; i < count; i++) {
		if (strcmp(changes[i]+1, name) == 0) {
			if (changes[i][0] == KEYWORD_CHANGE_REMOVE) {
				/* a final keyword is marked as removed.
				   this shouldn't normally happen. */
				array_delete(&change->keyword_changes, i, 1);
				break;
			}
			return TRUE;
		}
	}
	return FALSE;
}

static void
search_update_flag_changes(struct dsync_mailbox_exporter *exporter,
			   struct mail *mail, struct dsync_mail_change *change)
{
	const char *const *keywords;
	unsigned int i;

	i_assert((change->add_flags & change->remove_flags) == 0);

	change->modseq = mail_get_modseq(mail);
	change->pvt_modseq = mail_get_pvt_modseq(mail);
	change->final_flags = mail_get_flags(mail);

	keywords = mail_get_keywords(mail);
	if (!array_is_created(&change->keyword_changes) &&
	    keywords[0] != NULL) {
		p_array_init(&change->keyword_changes, exporter->pool,
			     str_array_length(keywords));
	}
	for (i = 0; keywords[i] != NULL; i++) {
		/* add the final keyword if it's not already there
		   as +keyword */
		if (!final_keyword_check(change, keywords[i])) {
			const char *keyword_change =
				p_strdup_printf(exporter->pool, "%c%s",
						KEYWORD_CHANGE_ADD,
						keywords[i]);
			array_append(&change->keyword_changes,
				     &keyword_change, 1);
		}
	}
}

static int
exporter_get_guids(struct dsync_mailbox_exporter *exporter,
		   struct mail *mail, const char **guid_r,
		   const char **hdr_hash_r)
{
	*guid_r = "";
	*hdr_hash_r = NULL;

	/* always try to get GUID, even if we're also getting header hash */
	if (mail_get_special(mail, MAIL_FETCH_GUID, guid_r) < 0)
		return dsync_mail_error(exporter, mail, "GUID");

	if (!exporter->mails_have_guids) {
		/* get header hash also */
		if (dsync_mail_get_hdr_hash(mail, hdr_hash_r) < 0)
			return dsync_mail_error(exporter, mail, "hdr-stream");
		return 1;
	} else if (**guid_r == '\0') {
		exporter->error = "Backend doesn't support GUIDs, "
			"sync with header hashes instead";
		return -1;
	} else {
		/* GUIDs are required, we don't need header hash */
		return 1;
	} 
}

static int
search_update_flag_change_guid(struct dsync_mailbox_exporter *exporter,
			       struct mail *mail)
{
	struct dsync_mail_change *change, *log_change;
	const char *guid, *hdr_hash;
	int ret;

	change = hash_table_lookup(exporter->changes, POINTER_CAST(mail->uid));
	if (change != NULL) {
		i_assert(change->type == DSYNC_MAIL_CHANGE_TYPE_FLAG_CHANGE);
	} else {
		i_assert(exporter->return_all_mails);

		change = p_new(exporter->pool, struct dsync_mail_change, 1);
		change->uid = mail->uid;
		change->type = DSYNC_MAIL_CHANGE_TYPE_FLAG_CHANGE;
		hash_table_insert(exporter->changes,
				  POINTER_CAST(mail->uid), change);
	}

	if ((ret = exporter_get_guids(exporter, mail, &guid, &hdr_hash)) < 0)
		return -1;
	if (ret == 0) {
		/* the message was expunged during export */
		memset(change, 0, sizeof(*change));
		change->type = DSYNC_MAIL_CHANGE_TYPE_EXPUNGE;
		change->uid = mail->uid;

		/* find its GUID from log if possible */
		log_change = dsync_transaction_log_scan_find_new_expunge(
			exporter->log_scan, mail->uid);
		if (log_change != NULL)
			change->guid = log_change->guid;
	} else {
		change->guid = *guid == '\0' ? "" :
			p_strdup(exporter->pool, guid);
		change->hdr_hash = p_strdup(exporter->pool, hdr_hash);
		search_update_flag_changes(exporter, mail, change);
	}
	return 0;
}

static struct dsync_mail_change *
export_save_change_get(struct dsync_mailbox_exporter *exporter, uint32_t uid)
{
	struct dsync_mail_change *change;

	change = hash_table_lookup(exporter->changes, POINTER_CAST(uid));
	if (change == NULL) {
		change = p_new(exporter->pool, struct dsync_mail_change, 1);
		change->uid = uid;
		hash_table_insert(exporter->changes, POINTER_CAST(uid), change);
	} else {
		/* move flag changes into a save. this happens only when
		   last_common_uid isn't known */
		i_assert(change->type == DSYNC_MAIL_CHANGE_TYPE_FLAG_CHANGE);
		i_assert(exporter->last_common_uid == 0);
	}

	change->type = DSYNC_MAIL_CHANGE_TYPE_SAVE;
	return change;
}

static void
export_add_mail_instance(struct dsync_mailbox_exporter *exporter,
			 struct dsync_mail_change *change, uint32_t seq)
{
	struct dsync_mail_guid_instances *instances;

	if (exporter->auto_export_mails && !exporter->mails_have_guids) {
		/* GUIDs not supported, mail is requested by UIDs */
		seq_range_array_add(&exporter->requested_uids, change->uid);
		return;
	}
	if (*change->guid == '\0') {
		/* mail UIDs are manually requested */
		i_assert(!exporter->mails_have_guids);
		return;
	}

	instances = hash_table_lookup(exporter->export_guids, change->guid);
	if (instances == NULL) {
		instances = p_new(exporter->pool,
				  struct dsync_mail_guid_instances, 1);
		p_array_init(&instances->seqs, exporter->pool, 2);
		hash_table_insert(exporter->export_guids,
				  p_strdup(exporter->pool, change->guid),
				  instances);
		if (exporter->auto_export_mails)
			instances->requested = TRUE;
	}
	seq_range_array_add(&instances->seqs, seq);
}

static int
search_add_save(struct dsync_mailbox_exporter *exporter, struct mail *mail)
{
	struct dsync_mail_change *change;
	const char *guid, *hdr_hash;
	time_t save_timestamp;
	int ret;

	/* If message is already expunged here, just skip it */
	if ((ret = exporter_get_guids(exporter, mail, &guid, &hdr_hash)) <= 0)
		return ret;
	if (mail_get_save_date(mail, &save_timestamp) < 0)
		return dsync_mail_error(exporter, mail, "save-date");

	change = export_save_change_get(exporter, mail->uid);
	change->save_timestamp = save_timestamp;

	change->guid = *guid == '\0' ? "" :
		p_strdup(exporter->pool, guid);
	change->hdr_hash = p_strdup(exporter->pool, hdr_hash);
	search_update_flag_changes(exporter, mail, change);

	export_add_mail_instance(exporter, change, mail->seq);
	return 1;
}

static void
dsync_mailbox_export_add_flagchange_uids(struct dsync_mailbox_exporter *exporter,
					 ARRAY_TYPE(seq_range) *uids)
{
	struct hash_iterate_context *iter;
	void *key;
	struct dsync_mail_change *change;

	iter = hash_table_iterate_init(exporter->changes);
	while (hash_table_iterate(iter, exporter->changes, &key, &change)) {
		if (change->type == DSYNC_MAIL_CHANGE_TYPE_FLAG_CHANGE)
			seq_range_array_add(uids, change->uid);
	}
	hash_table_iterate_deinit(&iter);
}

static void
dsync_mailbox_export_drop_expunged_flag_changes(struct dsync_mailbox_exporter *exporter)
{
	struct hash_iterate_context *iter;
	void *key;
	struct dsync_mail_change *change;

	/* any flag changes for UIDs above last_common_uid weren't found by
	   mailbox search, which means they were already expunged. for some
	   reason the log scanner found flag changes for the message, but not
	   the expunge. just remove these. */
	iter = hash_table_iterate_init(exporter->changes);
	while (hash_table_iterate(iter, exporter->changes, &key, &change)) {
		if (change->type == DSYNC_MAIL_CHANGE_TYPE_FLAG_CHANGE &&
		    change->uid > exporter->last_common_uid)
			hash_table_remove(exporter->changes, key);
	}
	hash_table_iterate_deinit(&iter);
}

static void
dsync_mailbox_export_search(struct dsync_mailbox_exporter *exporter)
{
	struct mail_search_context *search_ctx;
	struct mail_search_args *search_args;
	struct mail_search_arg *sarg;
	struct mail *mail;
	int ret;

	search_args = mail_search_build_init();
	sarg = mail_search_build_add(search_args, SEARCH_UIDSET);
	p_array_init(&sarg->value.seqset, search_args->pool, 1);

	if (exporter->return_all_mails || exporter->last_common_uid == 0) {
		/* we want to know about all mails */
		seq_range_array_add_range(&sarg->value.seqset, 1, (uint32_t)-1);
	} else {
		/* lookup GUIDs for messages with flag changes */
		dsync_mailbox_export_add_flagchange_uids(exporter,
							 &sarg->value.seqset);
		/* lookup new messages */
		seq_range_array_add_range(&sarg->value.seqset,
					  exporter->last_common_uid + 1,
					  (uint32_t)-1);
	}

	exporter->trans = mailbox_transaction_begin(exporter->box, 0);
	search_ctx = mailbox_search_init(exporter->trans, search_args, NULL,
					 0, NULL);
	mail_search_args_unref(&search_args);

	while (mailbox_search_next(search_ctx, &mail)) {
		if (mail->uid <= exporter->last_common_uid)
			ret = search_update_flag_change_guid(exporter, mail);
		else
			ret = search_add_save(exporter, mail);
		if (ret < 0)
			break;
	}

	dsync_mailbox_export_drop_expunged_flag_changes(exporter);

	if (mailbox_search_deinit(&search_ctx) < 0 &&
	    exporter->error == NULL) {
		exporter->error = p_strdup_printf(exporter->pool,
			"Mail search failed: %s",
			mailbox_get_last_error(exporter->box, NULL));
	}
}

static int dsync_mail_change_p_uid_cmp(struct dsync_mail_change *const *c1,
				       struct dsync_mail_change *const *c2)
{
	if ((*c1)->uid < (*c2)->uid)
		return -1;
	if ((*c1)->uid > (*c2)->uid)
		return 1;
	return 0;
}

static void
dsync_mailbox_export_sort_changes(struct dsync_mailbox_exporter *exporter)
{
	struct hash_iterate_context *iter;
	void *key;
	struct dsync_mail_change *change;

	p_array_init(&exporter->sorted_changes, exporter->pool,
		     hash_table_count(exporter->changes));

	iter = hash_table_iterate_init(exporter->changes);
	while (hash_table_iterate(iter, exporter->changes, &key, &change))
		array_append(&exporter->sorted_changes, &change, 1);
	hash_table_iterate_deinit(&iter);
	array_sort(&exporter->sorted_changes, dsync_mail_change_p_uid_cmp);
}

static void
dsync_mailbox_export_log_scan(struct dsync_mailbox_exporter *exporter,
			      struct dsync_transaction_log_scan *log_scan)
{
	HASH_TABLE_TYPE(dsync_uid_mail_change) log_changes;
	struct hash_iterate_context *iter;
	void *key;
	struct dsync_mail_change *change, *dup_change;

	log_changes = dsync_transaction_log_scan_get_hash(log_scan);
	if (dsync_transaction_log_scan_has_all_changes(log_scan)) {
		/* we tried to access too old/invalid modseqs. to make sure
		   no changes get lost, we need to send all of the messages */
		exporter->return_all_mails = TRUE;
	}

	/* clone the hash table, since we're changing it. */
	hash_table_create_direct(&exporter->changes, exporter->pool,
				 hash_table_count(log_changes));
	iter = hash_table_iterate_init(log_changes);
	while (hash_table_iterate(iter, log_changes, &key, &change)) {
		dup_change = p_new(exporter->pool, struct dsync_mail_change, 1);
		*dup_change = *change;
		hash_table_insert(exporter->changes, key, dup_change);
		if (exporter->highest_changed_uid < change->uid)
			exporter->highest_changed_uid = change->uid;
	}
	hash_table_iterate_deinit(&iter);
}

struct dsync_mailbox_exporter *
dsync_mailbox_export_init(struct mailbox *box,
			  struct dsync_transaction_log_scan *log_scan,
			  uint32_t last_common_uid,
			  enum dsync_mailbox_exporter_flags flags)
{
	struct dsync_mailbox_exporter *exporter;
	pool_t pool;

	pool = pool_alloconly_create(MEMPOOL_GROWING"dsync mailbox export",
				     4096);
	exporter = p_new(pool, struct dsync_mailbox_exporter, 1);
	exporter->pool = pool;
	exporter->box = box;
	exporter->log_scan = log_scan;
	exporter->last_common_uid = last_common_uid;
	exporter->auto_export_mails =
		(flags & DSYNC_MAILBOX_EXPORTER_FLAG_AUTO_EXPORT_MAILS) != 0;
	exporter->mails_have_guids =
		(flags & DSYNC_MAILBOX_EXPORTER_FLAG_MAILS_HAVE_GUIDS) != 0;
	p_array_init(&exporter->requested_uids, pool, 16);
	hash_table_create(&exporter->export_guids, pool, 0, str_hash, strcmp);
	p_array_init(&exporter->expunged_seqs, pool, 16);
	p_array_init(&exporter->expunged_guids, pool, 16);

	/* first scan transaction log and save any expunges and flag changes */
	dsync_mailbox_export_log_scan(exporter, log_scan);
	/* get saves and also find GUIDs for flag changes */
	dsync_mailbox_export_search(exporter);
	/* get the changes sorted by UID */
	dsync_mailbox_export_sort_changes(exporter);
	return exporter;
}

const struct dsync_mail_change *
dsync_mailbox_export_next(struct dsync_mailbox_exporter *exporter)
{
	struct dsync_mail_change *const *changes;
	unsigned int count;

	if (exporter->error != NULL)
		return NULL;

	changes = array_get(&exporter->sorted_changes, &count);
	if (exporter->change_idx == count)
		return NULL;

	return changes[exporter->change_idx++];
}

static int
dsync_mailbox_export_body_search_init(struct dsync_mailbox_exporter *exporter)
{
	struct mail_search_args *search_args;
	struct mail_search_arg *sarg;
	struct hash_iterate_context *iter;
	const struct seq_range *uids;
	char *guid;
	const char *const_guid;
	struct dsync_mail_guid_instances *instances;
	const struct seq_range *range;
	unsigned int i, count;
	uint32_t seq, seq1, seq2;

	i_assert(exporter->search_ctx == NULL);

	search_args = mail_search_build_init();
	sarg = mail_search_build_add(search_args, SEARCH_SEQSET);
	p_array_init(&sarg->value.seqset, search_args->pool, 128);

	/* get a list of messages we want to fetch. if there are more than one
	   instance for a GUID, use the first one. */
	iter = hash_table_iterate_init(exporter->export_guids);
	while (hash_table_iterate(iter, exporter->export_guids,
				  &guid, &instances)) {
		if (!instances->requested ||
		    array_count(&instances->seqs) == 0)
			continue;

		uids = array_idx(&instances->seqs, 0);
		seq = uids[0].seq1;
		if (!instances->searched) {
			instances->searched = TRUE;
			seq_range_array_add(&sarg->value.seqset, seq);
		} else if (seq_range_exists(&exporter->expunged_seqs, seq)) {
			/* we're on a second round, refetching expunged
			   messages */
			seq_range_array_remove(&instances->seqs, seq);
			seq_range_array_remove(&exporter->expunged_seqs, seq);
			if (array_count(&instances->seqs) == 0) {
				/* no instances left */
				const_guid = guid;
				array_append(&exporter->expunged_guids,
					     &const_guid, 1);
				continue;
			}
			uids = array_idx(&instances->seqs, 0);
			seq = uids[0].seq1;
			seq_range_array_add(&sarg->value.seqset, seq);
		}
	}
	hash_table_iterate_deinit(&iter);

	/* add requested UIDs */
	range = array_get(&exporter->requested_uids, &count);
	for (i = exporter->requested_uid_search_idx; i < count; i++) {
		mailbox_get_seq_range(exporter->box,
				      range->seq1, range->seq2,
				      &seq1, &seq2);
		seq_range_array_add_range(&sarg->value.seqset,
					  seq1, seq2);
	}
	exporter->requested_uid_search_idx = count;

	exporter->search_ctx =
		mailbox_search_init(exporter->trans, search_args, NULL,
				    MAIL_FETCH_GUID |
				    MAIL_FETCH_UIDL_BACKEND |
				    MAIL_FETCH_POP3_ORDER |
				    MAIL_FETCH_RECEIVED_DATE, NULL);
	mail_search_args_unref(&search_args);
	return array_count(&sarg->value.seqset) > 0 ? 1 : 0;
}

static void
dsync_mailbox_export_body_search_deinit(struct dsync_mailbox_exporter *exporter)
{
	if (exporter->search_ctx == NULL)
		return;

	if (mailbox_search_deinit(&exporter->search_ctx) < 0 &&
	    exporter->error == NULL) {
		exporter->error = p_strdup_printf(exporter->pool,
			"Mail search failed: %s",
			mailbox_get_last_error(exporter->box, NULL));
	}
}

static int dsync_mailbox_export_mail(struct dsync_mailbox_exporter *exporter,
				     struct mail *mail)
{
	struct dsync_mail *dmail = &exporter->dsync_mail;
	struct dsync_mail_guid_instances *instances;
	const char *guid, *str;

	if (mail_get_special(mail, MAIL_FETCH_GUID, &guid) < 0)
		return dsync_mail_error(exporter, mail, "GUID");

	memset(dmail, 0, sizeof(*dmail));
	if (!seq_range_exists(&exporter->requested_uids, mail->uid))
		dmail->guid = guid;
	else {
		dmail->uid = mail->uid;
		dmail->guid = "";
	}

	instances = *guid == '\0' ? NULL :
		hash_table_lookup(exporter->export_guids, guid);
	if (instances != NULL) {
		/* GUID found */
	} else if (dmail->uid != 0) {
		/* mail requested by UID */
	} else {
		exporter->error = p_strdup_printf(exporter->pool,
			"GUID unexpectedly changed for UID=%u GUID=%s",
			mail->uid, guid);
		return -1;
	}

	if (mail_get_stream(mail, NULL, NULL, &dmail->input) < 0)
		return dsync_mail_error(exporter, mail, "body");

	if (mail_get_special(mail, MAIL_FETCH_UIDL_BACKEND, &dmail->pop3_uidl) < 0)
		return dsync_mail_error(exporter, mail, "pop3-uidl");
	if (mail_get_special(mail, MAIL_FETCH_POP3_ORDER, &str) < 0)
		return dsync_mail_error(exporter, mail, "pop3-order");
	if (*str != '\0') {
		if (str_to_uint(str, &dmail->pop3_order) < 0)
			i_unreached();
	}
	if (mail_get_received_date(mail, &dmail->received_date) < 0)
		return dsync_mail_error(exporter, mail, "received-date");

	/* this message was successfully returned, don't try retrying it */
	if (instances != NULL)
		array_clear(&instances->seqs);
	return 1;
}

void dsync_mailbox_export_want_mail(struct dsync_mailbox_exporter *exporter,
				    const struct dsync_mail_request *request)
{
	struct dsync_mail_guid_instances *instances;

	i_assert(!exporter->auto_export_mails);

	if (*request->guid == '\0') {
		i_assert(request->uid > 0);
		seq_range_array_add(&exporter->requested_uids, request->uid);
		return;
	}

	instances = hash_table_lookup(exporter->export_guids, request->guid);
	if (instances == NULL) {
		exporter->error = p_strdup_printf(exporter->pool,
			"Remote requested unexpected GUID %s", request->guid);
		return;
	}
	instances->requested = TRUE;
}

const struct dsync_mail *
dsync_mailbox_export_next_mail(struct dsync_mailbox_exporter *exporter)
{
	struct mail *mail;
	const char *const *guids;
	unsigned int count;
	int ret;

	if (exporter->error != NULL)
		return NULL;
	if (!exporter->body_search_initialized) {
		exporter->body_search_initialized = TRUE;
		if (dsync_mailbox_export_body_search_init(exporter) < 0) {
			i_assert(exporter->error != NULL);
			return NULL;
		}
	}

	while (mailbox_search_next(exporter->search_ctx, &mail)) {
		if ((ret = dsync_mailbox_export_mail(exporter, mail)) > 0)
			return &exporter->dsync_mail;
		if (ret < 0) {
			i_assert(exporter->error != NULL);
			return NULL;
		}
		/* the message was expunged. if the GUID has another instance,
		   try sending it later. */
		seq_range_array_add(&exporter->expunged_seqs, mail->seq);
	}
	/* if some instances of messages were expunged, retry fetching them
	   with other instances */
	dsync_mailbox_export_body_search_deinit(exporter);
	if ((ret = dsync_mailbox_export_body_search_init(exporter)) < 0) {
		i_assert(exporter->error != NULL);
		return NULL;
	}
	if (ret > 0) {
		/* not finished yet */
		return dsync_mailbox_export_next_mail(exporter);
	}

	/* finished with messages. if there are any expunged messages,
	   return them */
	guids = array_get(&exporter->expunged_guids, &count);
	if (exporter->expunged_guid_idx < count) {
		memset(&exporter->dsync_mail, 0, sizeof(exporter->dsync_mail));
		exporter->dsync_mail.guid =
			guids[exporter->expunged_guid_idx++];
		return &exporter->dsync_mail;
	}
	return NULL;
}

int dsync_mailbox_export_deinit(struct dsync_mailbox_exporter **_exporter,
				const char **error_r)
{
	struct dsync_mailbox_exporter *exporter = *_exporter;

	*_exporter = NULL;

	dsync_mailbox_export_body_search_deinit(exporter);
	(void)mailbox_transaction_commit(&exporter->trans);

	hash_table_destroy(&exporter->export_guids);

	*error_r = t_strdup(exporter->error);
	pool_unref(&exporter->pool);
	return *error_r != NULL ? -1 : 0;
}
