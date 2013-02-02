/* Copyright (c) 2013 Dovecot authors, see the included COPYING file */

#include "lib.h"
#include "ioloop.h"
#include "str.h"
#include "mail-index-private.h"
#include "mail-transaction-log-private.h"
#include "mail-storage.h"
#include "mailbox-list-notify.h"
#include "mailbox-list-notify-tree.h"
#include "mailbox-list-index.h"

#include <sys/stat.h>

#define NOTIFY_DELAY_MSECS 500

enum ilist_ext_type {
	ILIST_EXT_NONE,
	ILIST_EXT_BASE,
	ILIST_EXT_MSGS,
	ILIST_EXT_HIGHESTMODSEQ,
	ILIST_EXT_UNKNOWN
};

struct mailbox_list_notify_rename {
	uint32_t old_uid, new_uid;
};

struct mailbox_list_inotify_entry {
	uint32_t uid;
	guid_128_t guid;
	bool expunge;
};

struct mailbox_list_notify_index {
	struct mailbox_list_notify notify;

	struct mailbox_tree_context *subscriptions;
	struct mailbox_list_notify_tree *tree;
	struct mail_index_view *view, *old_view;
	struct mail_index_view_sync_ctx *sync_ctx;
	enum ilist_ext_type cur_ext;
	uint32_t cur_ext_id;

	void (*wait_callback)(void *context);
	void *wait_context;
	struct io *io_wait;
	struct timeout *to_wait, *to_notify;

	ARRAY_TYPE(seq_range) new_uids, expunged_uids, changed_uids;
	ARRAY_TYPE(const_string) new_subscriptions, new_unsubscriptions;
	ARRAY(struct mailbox_list_notify_rename) renames;
	struct seq_range_iter new_uids_iter, expunged_uids_iter;
	struct seq_range_iter changed_uids_iter;
	unsigned int new_uids_n, expunged_uids_n, changed_uids_n;
	unsigned int rename_idx, subscription_idx, unsubscription_idx;

	struct mailbox_list_notify_rec notify_rec;
	string_t *rec_name;

	struct stat last_st;

	unsigned int initialized:1;
	unsigned int read_failed:1;
};

int mailbox_list_index_notify_init(struct mailbox_list *list,
				   enum mailbox_list_notify_event mask,
				   struct mailbox_list_notify **notify_r)
{
	struct mailbox_list_index *ilist = INDEX_LIST_CONTEXT(list);
	struct mailbox_list_notify_index *inotify;

	if (ilist == NULL) {
		/* can't do this without mailbox list indexes */
		return -1;
	}

	(void)mailbox_list_index_refresh(list);

	inotify = i_new(struct mailbox_list_notify_index, 1);
	inotify->notify.list = list;
	inotify->notify.mask = mask;
	inotify->view = mail_index_view_open(ilist->index);
	inotify->old_view = mail_index_view_dup_private(inotify->view);
	inotify->tree = mailbox_list_notify_tree_init(list);
	i_array_init(&inotify->new_uids, 8);
	i_array_init(&inotify->expunged_uids, 8);
	i_array_init(&inotify->changed_uids, 16);
	i_array_init(&inotify->renames, 16);
	i_array_init(&inotify->new_subscriptions, 16);
	i_array_init(&inotify->new_unsubscriptions, 16);
	inotify->rec_name = str_new(default_pool, 64);
	if ((mask & (MAILBOX_LIST_NOTIFY_SUBSCRIBE |
		     MAILBOX_LIST_NOTIFY_UNSUBSCRIBE)) != 0) {
		(void)mailbox_list_iter_subscriptions_refresh(list);
		mailbox_tree_sort(list->subscriptions);
		inotify->subscriptions = mailbox_tree_dup(list->subscriptions);
	}

	*notify_r = &inotify->notify;
	return 1;
}

void mailbox_list_index_notify_deinit(struct mailbox_list_notify *notify)
{
	struct mailbox_list_notify_index *inotify =
		(struct mailbox_list_notify_index *)notify;
	bool b;

	if (inotify->subscriptions != NULL)
		mailbox_tree_deinit(&inotify->subscriptions);
	if (inotify->io_wait != NULL)
		io_remove(&inotify->io_wait);
	if (inotify->to_wait != NULL)
		timeout_remove(&inotify->to_wait);
	if (inotify->to_notify != NULL)
		timeout_remove(&inotify->to_notify);
	if (inotify->sync_ctx != NULL)
		(void)mail_index_view_sync_commit(&inotify->sync_ctx, &b);
	mail_index_view_close(&inotify->view);
	mail_index_view_close(&inotify->old_view);
	mailbox_list_notify_tree_deinit(&inotify->tree);
	array_free(&inotify->new_subscriptions);
	array_free(&inotify->new_unsubscriptions);
	array_free(&inotify->new_uids);
	array_free(&inotify->expunged_uids);
	array_free(&inotify->changed_uids);
	array_free(&inotify->renames);
	str_free(&inotify->rec_name);
	i_free(inotify);
}

static struct mailbox_list_index_node *
notify_lookup_guid(struct mailbox_list_notify_index *inotify,
		   struct mail_index_view *view,
		   uint32_t uid, enum mailbox_status_items items,
		   struct mailbox_status *status_r, guid_128_t guid_r)
{
	struct mailbox_list_index *ilist =
		INDEX_LIST_CONTEXT(inotify->notify.list);
	struct mailbox_list_index_node *index_node;
	uint32_t seq;

	if (!mail_index_lookup_seq(view, uid, &seq))
		return NULL;

	index_node = mailbox_list_index_lookup_uid(ilist, uid);
	if (index_node == NULL) {
		/* re-parse the index list using the given view. we could be
		   jumping here between old and new view. */
		(void)mailbox_list_index_parse(inotify->notify.list,
					       view, FALSE);
		index_node = mailbox_list_index_lookup_uid(ilist, uid);
		if (index_node == NULL)
			return NULL;
	}

	/* get GUID */
	memset(status_r, 0, sizeof(*status_r));
	memset(guid_r, 0, GUID_128_SIZE);
	(void)mailbox_list_index_status(inotify->notify.list, view, seq,
					items, status_r, guid_r);
	return index_node;
}

static void notify_update_stat(struct mailbox_list_notify_index *inotify)
{
	struct mailbox_list_index *ilist =
		INDEX_LIST_CONTEXT(inotify->notify.list);
	const char *path = ilist->index->log->filepath;

	if (stat(path, &inotify->last_st) < 0 && errno != ENOENT) {
		i_error("stat(%s) failed: %m", path);
		mailbox_list_index_notify_wait(&inotify->notify, NULL, NULL);
	}
}

static void
mailbox_list_index_notify_sync_init(struct mailbox_list_notify_index *inotify)
{
	struct mail_index_view_sync_rec sync_rec;

	notify_update_stat(inotify);
	(void)mail_index_refresh(inotify->view->index);

	/* sync the view so that map extensions gets updated */
	inotify->sync_ctx = mail_index_view_sync_begin(inotify->view, 0);
	mail_transaction_log_view_mark(inotify->view->log_view);
	while (mail_index_view_sync_next(inotify->sync_ctx, &sync_rec)) ;
	mail_transaction_log_view_rewind(inotify->view->log_view);

	inotify->cur_ext = ILIST_EXT_NONE;
	inotify->cur_ext_id = (uint32_t)-1;
}

static bool notify_ext_rec(struct mailbox_list_notify_index *inotify,
			   uint32_t uid)
{
	struct mailbox_list_notify *notify = &inotify->notify;

	switch (inotify->cur_ext) {
	case ILIST_EXT_NONE:
		i_unreached();
	case ILIST_EXT_BASE:
		/* UIDVALIDITY changed */
		if ((notify->mask & MAILBOX_LIST_NOTIFY_UIDVALIDITY) == 0)
			return FALSE;
		break;
	case ILIST_EXT_MSGS:
		/* APPEND, EXPUNGE, \Seen or \Recent flag change */
		if ((notify->mask & MAILBOX_LIST_NOTIFY_STATUS) == 0)
			return FALSE;
		break;
	case ILIST_EXT_HIGHESTMODSEQ:
		/* when this doesn't come with EXT_MSGS update,
		   it can only be a flag change or an explicit
		   modseq change */
		if ((notify->mask & MAILBOX_LIST_NOTIFY_MODSEQ_CHANGES) == 0)
			return FALSE;
		break;
	case ILIST_EXT_UNKNOWN:
		return FALSE;
	}
	seq_range_array_add(&inotify->changed_uids, uid);
	return TRUE;
}

static int
mailbox_list_index_notify_read_next(struct mailbox_list_notify_index *inotify)
{
	struct mailbox_list_notify *notify = &inotify->notify;
	struct mailbox_list_index *ilist = INDEX_LIST_CONTEXT(notify->list);
	const struct mail_transaction_header *hdr;
	const void *data;
	int ret;

	ret = mail_transaction_log_view_next(inotify->view->log_view,
					     &hdr, &data);
	if (ret <= 0)
		return ret;

	if ((hdr->type & MAIL_TRANSACTION_EXTERNAL) == 0) {
		/* all mailbox index updates are external */
		return 1;
	}
	switch (hdr->type & MAIL_TRANSACTION_TYPE_MASK) {
	case MAIL_TRANSACTION_APPEND: {
		/* mailbox added or renamed */
		const struct mail_index_record *rec, *end;

		if ((notify->mask & (MAILBOX_LIST_NOTIFY_CREATE |
				     MAILBOX_LIST_NOTIFY_RENAME)) == 0)
			break;

		end = CONST_PTR_OFFSET(data, hdr->size);
		for (rec = data; rec != end; rec++)
			seq_range_array_add(&inotify->new_uids, rec->uid);
		break;
	}
	case MAIL_TRANSACTION_EXPUNGE_GUID: {
		/* mailbox deleted or renamed */
		const struct mail_transaction_expunge_guid *rec, *end;

		if ((notify->mask & (MAILBOX_LIST_NOTIFY_DELETE |
				     MAILBOX_LIST_NOTIFY_RENAME)) == 0)
			break;

		end = CONST_PTR_OFFSET(data, hdr->size);
		for (rec = data; rec != end; rec++)
			seq_range_array_add(&inotify->expunged_uids, rec->uid);
		break;
	}
	case MAIL_TRANSACTION_EXT_INTRO: {
		struct mail_index_map *map = inotify->view->map;
		const struct mail_transaction_ext_intro *rec = data;
		const struct mail_index_ext *ext = NULL;
		const char *name;
		uint32_t ext_map_idx;

		if (!array_is_created(&map->extensions))
			break;
		/* we want to know what extension the future
		   ext-rec-updates are changing. we're assuming here that
		   there is only one ext-intro record before those,
		   which is true at least for now. */
		if (rec->ext_id != (uint32_t)-1 &&
		    rec->ext_id < array_count(&map->extensions)) {
			/* get extension by id */
			ext = array_idx(&map->extensions, rec->ext_id);
		} else if (rec->name_size > 0) {
			/* by name */
			name = t_strndup(rec+1, rec->name_size);
			if (mail_index_map_lookup_ext(map, name, &ext_map_idx))
				ext = array_idx(&map->extensions, ext_map_idx);
		}
		if (ext != NULL) {
			if (ext->index_idx == ilist->ext_id)
				inotify->cur_ext = ILIST_EXT_BASE;
			else if (ext->index_idx == ilist->msgs_ext_id)
				inotify->cur_ext = ILIST_EXT_MSGS;
			else if (ext->index_idx == ilist->hmodseq_ext_id)
				inotify->cur_ext = ILIST_EXT_HIGHESTMODSEQ;
			else
				inotify->cur_ext = ILIST_EXT_UNKNOWN;
			inotify->cur_ext_id = ext->index_idx;
		}
		break;
	}
	case MAIL_TRANSACTION_EXT_REC_UPDATE: {
		const struct mail_index_registered_ext *ext;
		const struct mail_transaction_ext_rec_update *rec;
		unsigned int i, record_size;

		if (inotify->cur_ext == ILIST_EXT_NONE) {
			i_error("%s: Missing ext-intro for ext-rec-update",
				ilist->index->filepath);
			break;
		}

		/* the record is padded to 32bits in the transaction log */
		ext = array_idx(&inotify->view->index->extensions,
				inotify->cur_ext_id);
		record_size = (sizeof(*rec) + ext->record_size + 3) & ~3;
		for (i = 0; i < hdr->size; i += record_size) {
			rec = CONST_PTR_OFFSET(data, i);

			if (i + record_size > hdr->size)
				break;
			if (!notify_ext_rec(inotify, rec->uid))
				break;
		}
		break;
	}
	}
	return 1;
}

static int
mailbox_list_inotify_entry_guid_cmp(const struct mailbox_list_inotify_entry *r1,
				    const struct mailbox_list_inotify_entry *r2)
{
	int ret;

	ret = memcmp(r1->guid, r2->guid, sizeof(r1->guid));
	if (ret != 0)
		return ret;

	if (r1->expunge == r2->expunge) {
		/* this really shouldn't happen */
		return 0;
	}
	return r1->expunge ? -1 : 1;
}

static void
mailbox_list_index_notify_find_renames(struct mailbox_list_notify_index *inotify)
{
	ARRAY(struct mailbox_list_inotify_entry) entries;
	struct mailbox_status status;
	struct mailbox_list_notify_rename *rename;
	struct mailbox_list_inotify_entry *entry;
	const struct mailbox_list_inotify_entry *e;
	unsigned int i, count;
	guid_128_t guid;
	uint32_t uid;

	/* first get all of the added and expunged GUIDs */
	t_array_init(&entries, array_count(&inotify->new_uids) +
		     array_count(&inotify->expunged_uids));
	while (seq_range_array_iter_nth(&inotify->expunged_uids_iter,
					inotify->expunged_uids_n++, &uid)) {
		if (notify_lookup_guid(inotify, inotify->old_view, uid,
				       0, &status, guid) != NULL &&
		    !guid_128_is_empty(guid)) {
			entry = array_append_space(&entries);
			entry->uid = uid;
			entry->expunge = TRUE;
			memcpy(entry->guid, guid, sizeof(entry->guid));
		}
	}

	(void)mailbox_list_index_parse(inotify->notify.list,
				       inotify->view, TRUE);
	while (seq_range_array_iter_nth(&inotify->new_uids_iter,
					inotify->new_uids_n++, &uid)) {
		if (notify_lookup_guid(inotify, inotify->view, uid,
				       0, &status, guid) != NULL &&
		    !guid_128_is_empty(guid)) {
			entry = array_append_space(&entries);
			entry->uid = uid;
			memcpy(entry->guid, guid, sizeof(entry->guid));
		}
	}

	/* now sort the entries by GUID and find those that have been both
	   added and expunged */
	array_sort(&entries, mailbox_list_inotify_entry_guid_cmp);

	e = array_get(&entries, &count);
	for (i = 1; i < count; i++) {
		if (e[i-1].expunge && !e[i].expunge &&
		    memcmp(e[i-1].guid, e[i].guid, sizeof(e[i].guid)) == 0) {
			rename = array_append_space(&inotify->renames);
			rename->old_uid = e[i-1].uid;
			rename->new_uid = e[i].uid;

			seq_range_array_remove(&inotify->expunged_uids,
					       rename->old_uid);
			seq_range_array_remove(&inotify->new_uids,
					       rename->new_uid);
		}
	}
}

static void
mailbox_list_index_notify_find_subscribes(struct mailbox_list_notify_index *inotify)
{
	struct mailbox_tree_iterate_context *old_iter, *new_iter;
	struct mailbox_tree_context *old_tree, *new_tree;
	const char *old_path = NULL, *new_path = NULL;
	pool_t pool;
	int ret;

	if (mailbox_list_iter_subscriptions_refresh(inotify->notify.list) < 0)
		return;
	mailbox_tree_sort(inotify->notify.list->subscriptions);

	old_tree = inotify->subscriptions;
	new_tree = mailbox_tree_dup(inotify->notify.list->subscriptions);

	old_iter = mailbox_tree_iterate_init(old_tree, NULL, MAILBOX_SUBSCRIBED);
	new_iter = mailbox_tree_iterate_init(new_tree, NULL, MAILBOX_SUBSCRIBED);

	pool = mailbox_tree_get_pool(new_tree);
	for (;;) {
		if (old_path == NULL) {
			if (mailbox_tree_iterate_next(old_iter, &old_path) == NULL)
				old_path = NULL;
		}
		if (new_path == NULL) {
			if (mailbox_tree_iterate_next(new_iter, &new_path) == NULL)
				new_path = NULL;
		}

		if (old_path == NULL) {
			if (new_path == NULL)
				break;
			ret = 1;
		} else if (new_path == NULL)
			ret = -1;
		else {
			ret = strcmp(old_path, new_path);
		}

		if (ret == 0) {
			old_path = NULL;
			new_path = NULL;
		} else if (ret > 0) {
			new_path = p_strdup(pool, new_path);
			array_append(&inotify->new_subscriptions, &new_path, 1);
			new_path = NULL;
		} else {
			old_path = p_strdup(pool, old_path);
			array_append(&inotify->new_unsubscriptions, &old_path, 1);
			old_path = NULL;
		}
	}
	mailbox_tree_iterate_deinit(&old_iter);
	mailbox_tree_iterate_deinit(&new_iter);

	mailbox_tree_deinit(&inotify->subscriptions);
	inotify->subscriptions = new_tree;
}

static void
mailbox_list_index_notify_reset_iters(struct mailbox_list_notify_index *inotify)
{
	seq_range_array_iter_init(&inotify->new_uids_iter,
				  &inotify->new_uids);
	seq_range_array_iter_init(&inotify->expunged_uids_iter,
				  &inotify->expunged_uids);
	seq_range_array_iter_init(&inotify->changed_uids_iter,
				  &inotify->changed_uids);
	inotify->changed_uids_n = 0;
	inotify->new_uids_n = 0;
	inotify->expunged_uids_n = 0;
	inotify->rename_idx = 0;
	inotify->subscription_idx = 0;
	inotify->unsubscription_idx = 0;
}

static void
mailbox_list_index_notify_read_init(struct mailbox_list_notify_index *inotify)
{
	bool b;
	int ret;

	mailbox_list_index_notify_sync_init(inotify);

	/* read all changes from .log file */
	while ((ret = mailbox_list_index_notify_read_next(inotify)) > 0) ;
	inotify->read_failed = ret < 0;

	(void)mail_index_view_sync_commit(&inotify->sync_ctx, &b);

	/* remove changes for already deleted mailboxes */
	seq_range_array_remove_seq_range(&inotify->new_uids,
					 &inotify->expunged_uids);
	seq_range_array_remove_seq_range(&inotify->changed_uids,
					 &inotify->expunged_uids);
	mailbox_list_index_notify_reset_iters(inotify);
	if (array_count(&inotify->new_uids) > 0 &&
	    array_count(&inotify->expunged_uids) > 0) {
		mailbox_list_index_notify_find_renames(inotify);
		mailbox_list_index_notify_reset_iters(inotify);
	}
	if (inotify->subscriptions != NULL)
		mailbox_list_index_notify_find_subscribes(inotify);

	inotify->initialized = TRUE;
}

static void
mailbox_list_index_notify_read_deinit(struct mailbox_list_notify_index *inotify)
{
	/* save the old view so we can look up expunged records */
	mail_index_view_close(&inotify->old_view);
	inotify->old_view = mail_index_view_dup_private(inotify->view);

	array_clear(&inotify->new_subscriptions);
	array_clear(&inotify->new_unsubscriptions);
	array_clear(&inotify->new_uids);
	array_clear(&inotify->expunged_uids);
	array_clear(&inotify->changed_uids);
	array_clear(&inotify->renames);

	inotify->initialized = FALSE;
}

static bool
mailbox_list_index_notify_lookup(struct mailbox_list_notify_index *inotify,
				 struct mail_index_view *view,
				 uint32_t uid, enum mailbox_status_items items,
				 struct mailbox_status *status_r,
				 struct mailbox_list_notify_rec **rec_r)
{
	struct mailbox_list_notify_rec *rec = &inotify->notify_rec;
	struct mailbox_list_index_node *index_node;
	const char *storage_name;
	char ns_sep = mailbox_list_get_hierarchy_sep(inotify->notify.list);

	memset(rec, 0, sizeof(*rec));
	index_node = notify_lookup_guid(inotify, view, uid,
					items, status_r, rec->guid);
	if (index_node == NULL)
		return FALSE;

	/* get storage_name */
	str_truncate(inotify->rec_name, 0);
	mailbox_list_index_node_get_path(index_node, ns_sep, inotify->rec_name);
	storage_name = str_c(inotify->rec_name);

	rec->storage_name = storage_name;
	rec->vname = mailbox_list_get_vname(inotify->notify.list,
					    rec->storage_name);
	*rec_r = rec;
	return TRUE;
}

static bool
mailbox_list_index_notify_rename(struct mailbox_list_notify_index *inotify,
				 unsigned int idx)
{
	const struct mailbox_list_notify_rename *rename;
	struct mailbox_list_notify_rec *rec;
	struct mailbox_status status;
	const char *old_vname;

	rename = array_idx(&inotify->renames, idx);

	/* lookup the old name */
	if (!mailbox_list_index_notify_lookup(inotify, inotify->old_view,
					      rename->old_uid, 0, &status, &rec))
		return FALSE;
	old_vname = t_strdup(rec->vname);

	/* return using the new name */
	if (!mailbox_list_index_notify_lookup(inotify, inotify->view,
					      rename->new_uid, 0, &status, &rec))
		return FALSE;

	rec->old_vname = old_vname;
	rec->event = MAILBOX_LIST_NOTIFY_RENAME;
	return TRUE;
}

static bool
mailbox_list_index_notify_subscribe(struct mailbox_list_notify_index *inotify,
				    unsigned int idx)
{
	struct mailbox_list_notify_rec *rec = &inotify->notify_rec;
	const char *const *vnamep;

	memset(rec, 0, sizeof(*rec));
	vnamep = array_idx(&inotify->new_subscriptions, idx);
	rec->vname = *vnamep;
	rec->storage_name = mailbox_list_get_storage_name(inotify->notify.list,
							  rec->vname);
	rec->event = MAILBOX_LIST_NOTIFY_SUBSCRIBE;
	return TRUE;
}

static bool
mailbox_list_index_notify_unsubscribe(struct mailbox_list_notify_index *inotify,
				      unsigned int idx)
{
	struct mailbox_list_notify_rec *rec = &inotify->notify_rec;
	const char *const *vnamep;

	memset(rec, 0, sizeof(*rec));
	vnamep = array_idx(&inotify->new_unsubscriptions, idx);
	rec->vname = *vnamep;
	rec->storage_name = mailbox_list_get_storage_name(inotify->notify.list,
							  rec->vname);
	rec->event = MAILBOX_LIST_NOTIFY_UNSUBSCRIBE;
	return TRUE;
}

static bool
mailbox_list_index_notify_expunge(struct mailbox_list_notify_index *inotify,
				  uint32_t uid)
{
	struct mailbox_list_notify_rec *rec;
	struct mailbox_status status;

	if (!mailbox_list_index_notify_lookup(inotify, inotify->old_view,
					      uid, 0, &status, &rec))
		return FALSE;
	rec->event = MAILBOX_LIST_NOTIFY_DELETE;
	return TRUE;
}

static bool
mailbox_list_index_notify_new(struct mailbox_list_notify_index *inotify,
			      uint32_t uid)
{
	struct mailbox_list_notify_rec *rec;
	struct mailbox_status status;

	if (!mailbox_list_index_notify_lookup(inotify, inotify->view,
					      uid, 0, &status, &rec))
		i_unreached();
	rec->event = MAILBOX_LIST_NOTIFY_CREATE;
	return TRUE;
}

static bool
mailbox_list_index_notify_change(struct mailbox_list_notify_index *inotify,
				 uint32_t uid)
{
	const enum mailbox_status_items status_items =
		STATUS_UIDVALIDITY | STATUS_UIDNEXT | STATUS_MESSAGES |
		STATUS_UNSEEN | STATUS_HIGHESTMODSEQ;
	struct mailbox_list_notify_rec *rec;
	struct mailbox_notify_node *nnode;
	struct mailbox_status status;

	if (!mailbox_list_index_notify_lookup(inotify, inotify->view,
					      uid, status_items, &status, &rec))
		i_unreached();

	/* get the old status */
	nnode = mailbox_list_notify_tree_lookup(inotify->tree,
						rec->storage_name);
	if (nnode == NULL || nnode->uidvalidity != status.uidvalidity)
		rec->event = MAILBOX_LIST_NOTIFY_UIDVALIDITY;
	else if (nnode->uidnext != status.uidnext)
		rec->event = MAILBOX_LIST_NOTIFY_APPENDS;
	else if (nnode->messages > status.messages)
		rec->event = MAILBOX_LIST_NOTIFY_EXPUNGES;
	else if (nnode->unseen != status.unseen)
		rec->event = MAILBOX_LIST_NOTIFY_SEEN_CHANGES;
	else if (nnode->highest_modseq < status.highest_modseq)
		rec->event = MAILBOX_LIST_NOTIFY_MODSEQ_CHANGES;
	else {
		/* nothing changed */
		return FALSE;
	}

	/* update internal state */
	if (nnode != NULL) {
		nnode->uidvalidity = status.uidvalidity;
		nnode->uidnext = status.uidnext;
		nnode->messages = status.messages;
		nnode->unseen = status.unseen;
		nnode->highest_modseq = status.highest_modseq;
	}
	return TRUE;
}

static bool
mailbox_list_index_notify_try_next(struct mailbox_list_notify_index *inotify)
{
	uint32_t uid;

	/* first show mailbox deletes */
	if (seq_range_array_iter_nth(&inotify->expunged_uids_iter,
				     inotify->expunged_uids_n++, &uid))
		return mailbox_list_index_notify_expunge(inotify, uid);

	/* mailbox renames */
	if (inotify->rename_idx < array_count(&inotify->renames)) {
		return mailbox_list_index_notify_rename(inotify,
							inotify->rename_idx++);
	}

	/* next mailbox creates */
	if (seq_range_array_iter_nth(&inotify->new_uids_iter,
				     inotify->new_uids_n++, &uid))
		return mailbox_list_index_notify_new(inotify, uid);

	/* subscribes */
	if (inotify->subscription_idx < array_count(&inotify->new_subscriptions)) {
		return mailbox_list_index_notify_subscribe(inotify,
					inotify->subscription_idx++);
	}
	if (inotify->unsubscription_idx < array_count(&inotify->new_unsubscriptions)) {
		return mailbox_list_index_notify_unsubscribe(inotify,
					inotify->unsubscription_idx++);
	}

	/* STATUS updates */
	while (seq_range_array_iter_nth(&inotify->changed_uids_iter,
					inotify->changed_uids_n++, &uid)) {
		if (mailbox_list_index_notify_change(inotify, uid))
			return TRUE;
	}
	return FALSE;
}

int mailbox_list_index_notify_next(struct mailbox_list_notify *notify,
				   const struct mailbox_list_notify_rec **rec_r)
{
	struct mailbox_list_notify_index *inotify =
		(struct mailbox_list_notify_index *)notify;

	if (!inotify->initialized)
		mailbox_list_index_notify_read_init(inotify);
	while (mailbox_list_index_notify_try_next(inotify)) {
		if ((inotify->notify_rec.event & inotify->notify.mask) != 0) {
			*rec_r = &inotify->notify_rec;
			return 1;
		} else {
			/* caller doesn't care about this change */
		}
	}

	mailbox_list_index_notify_read_deinit(inotify);
	return inotify->read_failed ? -1 : 0;
}

static void notify_now_callback(struct mailbox_list_notify_index *inotify)
{
	timeout_remove(&inotify->to_notify);
	inotify->wait_callback(inotify->wait_context);
}

static void notify_callback(struct mailbox_list_notify_index *inotify)
{
	struct stat prev_st = inotify->last_st;

	notify_update_stat(inotify);
	if (inotify->last_st.st_mtime != prev_st.st_mtime ||
	    ST_MTIME_NSEC(inotify->last_st) != ST_MTIME_NSEC(prev_st) ||
	    inotify->last_st.st_size != prev_st.st_size ||
	    inotify->last_st.st_ino != prev_st.st_ino) {
		/* log has changed. call the callback with a small delay
		   to allow bundling multiple changes together */
		if (inotify->to_notify != NULL) {
			/* already doing this */
			return;
		}
		inotify->to_notify =
			timeout_add_short(NOTIFY_DELAY_MSECS,
					  notify_now_callback, inotify);
	}
}

void mailbox_list_index_notify_wait(struct mailbox_list_notify *notify,
				    void (*callback)(void *context),
				    void *context)
{
	struct mailbox_list_notify_index *inotify =
		(struct mailbox_list_notify_index *)notify;
	const char *path;
	unsigned int check_interval;

	inotify->wait_callback = callback;
	inotify->wait_context = context;

	if (callback == NULL) {
		if (inotify->io_wait != NULL)
			io_remove(&inotify->io_wait);
		if (inotify->to_wait != NULL)
			timeout_remove(&inotify->to_wait);
		if (inotify->to_notify != NULL)
			timeout_remove(&inotify->to_notify);
	} else if (inotify->to_wait == NULL) {
		path = inotify->view->index->log->filepath;
		(void)io_add_notify(path, notify_callback, inotify,
				    &inotify->io_wait);
		/* check with timeout as well, in case io_add_notify()
		   doesn't work (e.g. NFS) */
		check_interval = notify->list->mail_set->mailbox_idle_check_interval;
		i_assert(check_interval > 0);
		inotify->to_wait = timeout_add(check_interval * 1000,
					       notify_callback, inotify);
		notify_update_stat(inotify);
	}
}
