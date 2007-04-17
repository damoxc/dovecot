/* Copyright (C) 2005 Timo Sirainen */

#include "lib.h"
#include "array.h"
#include "istream.h"
#include "mail-search.h"
#include "mail-storage-private.h"
#include "mailbox-list-private.h"
#include "quota-private.h"
#include "quota-plugin.h"

#include <sys/stat.h>

#define QUOTA_CONTEXT(obj) \
	MODULE_CONTEXT(obj, quota_storage_module)
#define QUOTA_MAIL_CONTEXT(obj) \
	MODULE_CONTEXT(obj, quota_mail_module)
#define QUOTA_LIST_CONTEXT(obj) \
	MODULE_CONTEXT(obj, quota_mailbox_list_module)

struct quota_mailbox_list {
	union mailbox_list_module_context module_ctx;

	struct mail_storage *storage;
};

struct quota_mailbox {
	union mailbox_module_context module_ctx;

	struct mailbox_transaction_context *expunge_trans;
	struct quota_transaction_context *expunge_qt;
	ARRAY_DEFINE(expunge_uids, uint32_t);
	ARRAY_DEFINE(expunge_sizes, uoff_t);

	unsigned int save_hack:1;
	unsigned int recalculate:1;
};

static MODULE_CONTEXT_DEFINE_INIT(quota_storage_module,
				  &mail_storage_module_register);
static MODULE_CONTEXT_DEFINE_INIT(quota_mail_module, &mail_module_register);
static MODULE_CONTEXT_DEFINE_INIT(quota_mailbox_list_module,
				  &mailbox_list_module_register);

static int quota_mail_expunge(struct mail *_mail)
{
	struct mail_private *mail = (struct mail_private *)_mail;
	struct quota_mailbox *qbox = QUOTA_CONTEXT(_mail->box);
	union mail_module_context *qmail = QUOTA_MAIL_CONTEXT(mail);
	uoff_t size;

	if (qmail->super.expunge(_mail) < 0)
		return -1;

	/* We need to handle the situation where multiple transactions expunged
	   the mail at the same time. In here we'll just save the message's
	   physical size and do the quota freeing later when the message was
	   known to be expunged. */
	size = mail_get_physical_size(_mail);
	if (size != (uoff_t)-1) {
		if (!array_is_created(&qbox->expunge_uids)) {
			i_array_init(&qbox->expunge_uids, 64);
			i_array_init(&qbox->expunge_sizes, 64);
		}
		array_append(&qbox->expunge_uids, &_mail->uid, 1);
		array_append(&qbox->expunge_sizes, &size, 1);
	}

	return 0;
}

static struct mailbox_transaction_context *
quota_mailbox_transaction_begin(struct mailbox *box,
				enum mailbox_transaction_flags flags)
{
	struct quota_mailbox *qbox = QUOTA_CONTEXT(box);
	struct mailbox_transaction_context *t;
	struct quota_transaction_context *qt;

	t = qbox->module_ctx.super.transaction_begin(box, flags);
	qt = quota_transaction_begin(quota_set, box);

	MODULE_CONTEXT_SET(t, quota_storage_module, qt);
	return t;
}

static int
quota_mailbox_transaction_commit(struct mailbox_transaction_context *ctx,
				 enum mailbox_sync_flags flags)
{
	struct quota_mailbox *qbox = QUOTA_CONTEXT(ctx->box);
	struct quota_transaction_context *qt = QUOTA_CONTEXT(ctx);

	if (qbox->module_ctx.super.transaction_commit(ctx, flags) < 0) {
		quota_transaction_rollback(&qt);
		return -1;
	} else {
		if (qt->tmp_mail != NULL)
			mail_free(&qt->tmp_mail);
		(void)quota_transaction_commit(&qt);
		return 0;
	}
}

static void
quota_mailbox_transaction_rollback(struct mailbox_transaction_context *ctx)
{
	struct quota_mailbox *qbox = QUOTA_CONTEXT(ctx->box);
	struct quota_transaction_context *qt = QUOTA_CONTEXT(ctx);

	qbox->module_ctx.super.transaction_rollback(ctx);

	if (qt->tmp_mail != NULL)
		mail_free(&qt->tmp_mail);
	quota_transaction_rollback(&qt);
}

static struct mail *
quota_mail_alloc(struct mailbox_transaction_context *t,
		 enum mail_fetch_field wanted_fields,
		 struct mailbox_header_lookup_ctx *wanted_headers)
{
	struct quota_mailbox *qbox = QUOTA_CONTEXT(t->box);
	union mail_module_context *qmail;
	struct mail *_mail;
	struct mail_private *mail;

	_mail = qbox->module_ctx.super.
		mail_alloc(t, wanted_fields, wanted_headers);
	mail = (struct mail_private *)_mail;

	qmail = p_new(mail->pool, union mail_module_context, 1);
	qmail->super = mail->v;

	mail->v.expunge = quota_mail_expunge;
	MODULE_CONTEXT_SET_SELF(mail, quota_mail_module, qmail);
	return _mail;
}

static int quota_check(struct mailbox_transaction_context *t, struct mail *mail)
{
	struct quota_transaction_context *qt = QUOTA_CONTEXT(t);
	int ret;
	bool too_large;

	ret = quota_try_alloc(qt, mail, &too_large);
	if (ret > 0)
		return 0;
	else if (ret == 0) {
		mail_storage_set_error(t->box->storage, "Quota exceeded");
		return -1;
	} else {
		mail_storage_set_error(t->box->storage,
				       "Internal quota calculation error");
		return -1;
	}
}

static int
quota_copy(struct mailbox_transaction_context *t, struct mail *mail,
	   enum mail_flags flags, struct mail_keywords *keywords,
	   struct mail *dest_mail)
{
	struct quota_transaction_context *qt = QUOTA_CONTEXT(t);
	struct quota_mailbox *qbox = QUOTA_CONTEXT(t->box);

	if (dest_mail == NULL) {
		/* we always want to know the mail size */
		if (qt->tmp_mail == NULL) {
			qt->tmp_mail = mail_alloc(t, MAIL_FETCH_PHYSICAL_SIZE,
						  NULL);
		}
		dest_mail = qt->tmp_mail;
	}

	qbox->save_hack = FALSE;
	if (qbox->module_ctx.super.copy(t, mail, flags, keywords,
					dest_mail) < 0)
		return -1;

	/* if copying used saving internally, we already checked the quota
	   and set qbox->save_hack = TRUE. */
	return qbox->save_hack ? 0 : quota_check(t, dest_mail);
}

static int
quota_save_init(struct mailbox_transaction_context *t,
		enum mail_flags flags, struct mail_keywords *keywords,
		time_t received_date, int timezone_offset,
		const char *from_envelope, struct istream *input,
		struct mail *dest_mail, struct mail_save_context **ctx_r)
{
	struct quota_transaction_context *qt = QUOTA_CONTEXT(t);
	struct quota_mailbox *qbox = QUOTA_CONTEXT(t->box);
	const struct stat *st;
	int ret;

	st = i_stream_stat(input, TRUE);
	if (st != NULL && st->st_size != -1) {
		/* Input size is known, check for quota immediately. This
		   check isn't perfect, especially because input stream's
		   linefeeds may contain CR+LFs while physical message would
		   only contain LFs. With mbox some headers might be skipped
		   entirely.

		   I think these don't really matter though compared to the
		   benefit of giving "out of quota" error before sending the
		   full mail. */
		bool too_large;

		ret = quota_test_alloc(qt, st->st_size, &too_large);
		if (ret == 0) {
			mail_storage_set_error(t->box->storage,
					       "Quota exceeded");
			return -1;
		} else if (ret < 0) {
			mail_storage_set_error(t->box->storage,
				"Internal quota calculation error");
			return -1;
		}
	}

	if (dest_mail == NULL) {
		/* we always want to know the mail size */
		if (qt->tmp_mail == NULL) {
			qt->tmp_mail = mail_alloc(t, MAIL_FETCH_PHYSICAL_SIZE,
						  NULL);
		}
		dest_mail = qt->tmp_mail;
	}

	return qbox->module_ctx.super.
		save_init(t, flags, keywords, received_date,
			  timezone_offset, from_envelope,
			  input, dest_mail, ctx_r);
}

static int quota_save_finish(struct mail_save_context *ctx)
{
	struct quota_transaction_context *qt = QUOTA_CONTEXT(ctx->transaction);
	struct quota_mailbox *qbox = QUOTA_CONTEXT(ctx->transaction->box);

	if (qbox->module_ctx.super.save_finish(ctx) < 0)
		return -1;

	qbox->save_hack = TRUE;
	return quota_check(ctx->transaction, ctx->dest_mail != NULL ?
			   ctx->dest_mail : qt->tmp_mail);
}

static void quota_mailbox_sync_finish(struct quota_mailbox *qbox)
{
	if (array_is_created(&qbox->expunge_uids)) {
		array_clear(&qbox->expunge_uids);
		array_clear(&qbox->expunge_sizes);
	}

	if (qbox->expunge_qt != NULL) {
		if (qbox->expunge_qt->tmp_mail != NULL) {
			mail_free(&qbox->expunge_qt->tmp_mail);
			mailbox_transaction_rollback(&qbox->expunge_trans);
		}
		(void)quota_transaction_commit(&qbox->expunge_qt);
	}
	qbox->recalculate = FALSE;
}

static void quota_mailbox_sync_notify(struct mailbox *box, uint32_t uid,
				      enum mailbox_sync_type sync_type)
{
	struct quota_mailbox *qbox = QUOTA_CONTEXT(box);
	const uint32_t *uids;
	const uoff_t *sizep;
	unsigned int i, count;
	uoff_t size;

	if (qbox->module_ctx.super.sync_notify != NULL)
		qbox->module_ctx.super.sync_notify(box, uid, sync_type);

	if (sync_type != MAILBOX_SYNC_TYPE_EXPUNGE || qbox->recalculate) {
		if (uid == 0)
			quota_mailbox_sync_finish(qbox);
		return;
	}

	/* we're in the middle of syncing the mailbox, so it's a bad idea to
	   try and get the message sizes at this point. Rely on sizes that
	   we saved earlier, or recalculate the whole quota if we don't know
	   the size. */
	if (!array_is_created(&qbox->expunge_uids)) {
		i = count = 0;
	} else {
		uids = array_get(&qbox->expunge_uids, &count);
		for (i = 0; i < count; i++) {
			if (uids[i] == uid)
				break;
		}
	}

	if (qbox->expunge_qt == NULL)
		qbox->expunge_qt = quota_transaction_begin(quota_set, box);

	if (i != count) {
		/* we already know the size */
		sizep = array_idx(&qbox->expunge_sizes, i);
		quota_free_bytes(qbox->expunge_qt, *sizep);
		return;
	}

	/* try to look up the size. this works only if it's cached. */
	if (qbox->expunge_qt->tmp_mail == NULL) {
		qbox->expunge_trans = mailbox_transaction_begin(box, 0);
		qbox->expunge_qt->tmp_mail =
			mail_alloc(qbox->expunge_trans,
				   MAIL_FETCH_PHYSICAL_SIZE, NULL);
	}
	mail_set_uid(qbox->expunge_qt->tmp_mail, uid);

	size = mail_get_physical_size(qbox->expunge_qt->tmp_mail);
	if (size != (uoff_t)-1)
		quota_free_bytes(qbox->expunge_qt, size);
	else {
		/* there's no way to get the size. recalculate the quota. */
		quota_recalculate(qbox->expunge_qt);
		qbox->recalculate = TRUE;
	}
}

static int quota_mailbox_sync_deinit(struct mailbox_sync_context *ctx,
				     enum mailbox_status_items status_items,
				     struct mailbox_status *status_r)
{
	struct quota_mailbox *qbox = QUOTA_CONTEXT(ctx->box);

	/* just in case sync_notify() wasn't called with uid=0 */
	quota_mailbox_sync_finish(qbox);

	return qbox->module_ctx.super.sync_deinit(ctx, status_items, status_r);
}

static int quota_mailbox_close(struct mailbox *box)
{
	struct quota_mailbox *qbox = QUOTA_CONTEXT(box);

	if (array_is_created(&qbox->expunge_uids)) {
		array_free(&qbox->expunge_uids);
		array_free(&qbox->expunge_sizes);
	}
	i_assert(qbox->expunge_qt == NULL ||
		 qbox->expunge_qt->tmp_mail == NULL);

	return qbox->module_ctx.super.close(box);
}

static struct mailbox *
quota_mailbox_open(struct mail_storage *storage, const char *name,
		   struct istream *input, enum mailbox_open_flags flags)
{
	union mail_storage_module_context *qstorage = QUOTA_CONTEXT(storage);
	struct mailbox *box;
	struct quota_mailbox *qbox;

	box = qstorage->super.mailbox_open(storage, name, input, flags);
	if (box == NULL)
		return NULL;

	qbox = p_new(box->pool, struct quota_mailbox, 1);
	qbox->module_ctx.super = box->v;

	box->v.transaction_begin = quota_mailbox_transaction_begin;
	box->v.transaction_commit = quota_mailbox_transaction_commit;
	box->v.transaction_rollback = quota_mailbox_transaction_rollback;
	box->v.mail_alloc = quota_mail_alloc;
	box->v.save_init = quota_save_init;
	box->v.save_finish = quota_save_finish;
	box->v.copy = quota_copy;
	box->v.sync_notify = quota_mailbox_sync_notify;
	box->v.sync_deinit = quota_mailbox_sync_deinit;
	box->v.close = quota_mailbox_close;
	MODULE_CONTEXT_SET(box, quota_storage_module, qbox);
	return box;
}

static int
quota_mailbox_list_delete(struct mailbox_list *list, const char *name)
{
	struct quota_mailbox_list *qlist = QUOTA_LIST_CONTEXT(list);
	struct mailbox *box;
	struct mail_search_context *ctx;
        struct mailbox_transaction_context *t;
	struct quota_transaction_context *qt;
	struct mail *mail;
	struct mail_search_arg search_arg;
	int ret;

	/* This is a bit annoying to handle. We'll have to open the mailbox
	   and free the quota for all the messages existing in it. Open the
	   mailbox locked so that other processes can't mess up the quota
	   calculations by adding/removing mails while we're doing this. */
	box = mailbox_open(qlist->storage, name, NULL, MAILBOX_OPEN_FAST |
			   MAILBOX_OPEN_KEEP_RECENT | MAILBOX_OPEN_KEEP_LOCKED);
	if (box == NULL)
		return -1;

	memset(&search_arg, 0, sizeof(search_arg));
	search_arg.type = SEARCH_ALL;

	t = mailbox_transaction_begin(box, 0);
	qt = QUOTA_CONTEXT(t);
	ctx = mailbox_search_init(t, NULL, &search_arg, NULL);

	mail = mail_alloc(t, 0, NULL);
	while (mailbox_search_next(ctx, mail) > 0)
		quota_free(qt, mail);
	mail_free(&mail);

	ret = mailbox_search_deinit(&ctx);
	if (ret < 0)
		mailbox_transaction_rollback(&t);
	else
		ret = mailbox_transaction_commit(&t, 0);
	mailbox_close(&box);
	/* FIXME: here's an unfortunate race condition */
	return ret < 0 ? -1 :
		qlist->module_ctx.super.delete_mailbox(list, name);
}

static void quota_storage_destroy(struct mail_storage *storage)
{
	union mail_storage_module_context *qstorage = QUOTA_CONTEXT(storage);

	quota_remove_user_storage(quota_set, storage);

	if (qstorage->super.destroy != NULL)
		qstorage->super.destroy(storage);
}

void quota_mail_storage_created(struct mail_storage *storage)
{
	struct quota_mailbox_list *qlist = QUOTA_LIST_CONTEXT(storage->list);
	union mail_storage_module_context *qstorage;

	if (quota_next_hook_mail_storage_created != NULL)
		quota_next_hook_mail_storage_created(storage);

	qlist->storage = storage;

	qstorage = p_new(storage->pool, union mail_storage_module_context, 1);
	qstorage->super = storage->v;
	storage->v.destroy = quota_storage_destroy;
	storage->v.mailbox_open = quota_mailbox_open;

	MODULE_CONTEXT_SET_SELF(storage, quota_storage_module, qstorage);

	if (storage->ns->type == NAMESPACE_PRIVATE) {
		/* register to user's quota roots */
		quota_add_user_storage(quota_set, storage);
	}
}

void quota_mailbox_list_created(struct mailbox_list *list)
{
	struct quota_mailbox_list *qlist;

	if (quota_next_hook_mailbox_list_created != NULL)
		quota_next_hook_mailbox_list_created(list);

	qlist = p_new(list->pool, struct quota_mailbox_list, 1);
	qlist->module_ctx.super = list->v;
	list->v.delete_mailbox = quota_mailbox_list_delete;

	MODULE_CONTEXT_SET(list, quota_mailbox_list_module, qlist);
}
