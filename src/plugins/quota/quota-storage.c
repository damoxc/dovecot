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
	*((void **)array_idx_modifiable(&(obj)->module_contexts, \
					quota_storage_module_id))
#define QUOTA_LIST_CONTEXT(obj) \
	*((void **)array_idx_modifiable(&(obj)->module_contexts, \
					quota_mailbox_list_module_id))

struct quota_mailbox_list {
	struct mailbox_list_vfuncs super;

	struct mail_storage *storage;
};

struct quota_mail_storage {
	struct mail_storage_vfuncs super;
};

struct quota_mailbox {
	struct mailbox_vfuncs super;

	unsigned int save_hack:1;
};

struct quota_mail {
	struct mail_vfuncs super;
};

static unsigned int quota_storage_module_id = 0;
static bool quota_storage_module_id_set = FALSE;

static unsigned int quota_mailbox_list_module_id = 0;
static bool quota_mailbox_list_module_id_set = FALSE;

static int quota_mail_expunge(struct mail *_mail)
{
	struct mail_private *mail = (struct mail_private *)_mail;
	struct quota_mail *qmail = QUOTA_CONTEXT(mail);
	struct quota_transaction_context *qt =
		QUOTA_CONTEXT(_mail->transaction);

	if (qmail->super.expunge(_mail) < 0)
		return -1;

	quota_free(qt, _mail);
	return 0;
}

static struct mailbox_transaction_context *
quota_mailbox_transaction_begin(struct mailbox *box,
				enum mailbox_transaction_flags flags)
{
	struct quota_mailbox *qbox = QUOTA_CONTEXT(box);
	struct mailbox_transaction_context *t;
	struct quota_transaction_context *qt;

	t = qbox->super.transaction_begin(box, flags);
	qt = quota_transaction_begin(quota_set, box);

	array_idx_set(&t->module_contexts, quota_storage_module_id, &qt);
	return t;
}

static int
quota_mailbox_transaction_commit(struct mailbox_transaction_context *ctx,
				 enum mailbox_sync_flags flags)
{
	struct quota_mailbox *qbox = QUOTA_CONTEXT(ctx->box);
	struct quota_transaction_context *qt = QUOTA_CONTEXT(ctx);

	if (qbox->super.transaction_commit(ctx, flags) < 0) {
		quota_transaction_rollback(qt);
		return -1;
	} else {
		(void)quota_transaction_commit(qt);
		if (qt->tmp_mail != NULL)
			mail_free(&qt->tmp_mail);
		return 0;
	}
}

static void
quota_mailbox_transaction_rollback(struct mailbox_transaction_context *ctx)
{
	struct quota_mailbox *qbox = QUOTA_CONTEXT(ctx->box);
	struct quota_transaction_context *qt = QUOTA_CONTEXT(ctx);

	qbox->super.transaction_rollback(ctx);

	if (qt->tmp_mail != NULL)
		mail_free(&qt->tmp_mail);
	quota_transaction_rollback(qt);
}

static struct mail *
quota_mail_alloc(struct mailbox_transaction_context *t,
		 enum mail_fetch_field wanted_fields,
		 struct mailbox_header_lookup_ctx *wanted_headers)
{
	struct quota_mailbox *qbox = QUOTA_CONTEXT(t->box);
	struct quota_mail *qmail;
	struct mail *_mail;
	struct mail_private *mail;

	_mail = qbox->super.mail_alloc(t, wanted_fields, wanted_headers);
	mail = (struct mail_private *)_mail;

	qmail = p_new(mail->pool, struct quota_mail, 1);
	qmail->super = mail->v;

	mail->v.expunge = quota_mail_expunge;
	array_idx_set(&mail->module_contexts, quota_storage_module_id, &qmail);
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
	if (qbox->super.copy(t, mail, flags, keywords, dest_mail) < 0)
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

	return qbox->super.save_init(t, flags, keywords, received_date,
				     timezone_offset, from_envelope,
				     input, dest_mail, ctx_r);
}

static int quota_save_finish(struct mail_save_context *ctx)
{
	struct quota_transaction_context *qt = QUOTA_CONTEXT(ctx->transaction);
	struct quota_mailbox *qbox = QUOTA_CONTEXT(ctx->transaction->box);

	if (qbox->super.save_finish(ctx) < 0)
		return -1;

	qbox->save_hack = TRUE;
	return quota_check(ctx->transaction, ctx->dest_mail != NULL ?
			   ctx->dest_mail : qt->tmp_mail);
}

static struct mailbox *
quota_mailbox_open(struct mail_storage *storage, const char *name,
		   struct istream *input, enum mailbox_open_flags flags)
{
	struct quota_mail_storage *qstorage = QUOTA_CONTEXT(storage);
	struct mailbox *box;
	struct quota_mailbox *qbox;

	box = qstorage->super.mailbox_open(storage, name, input, flags);
	if (box == NULL)
		return NULL;

	qbox = p_new(box->pool, struct quota_mailbox, 1);
	qbox->super = box->v;

	box->v.transaction_begin = quota_mailbox_transaction_begin;
	box->v.transaction_commit = quota_mailbox_transaction_commit;
	box->v.transaction_rollback = quota_mailbox_transaction_rollback;
	box->v.mail_alloc = quota_mail_alloc;
	box->v.save_init = quota_save_init;
	box->v.save_finish = quota_save_finish;
	box->v.copy = quota_copy;
	array_idx_set(&box->module_contexts, quota_storage_module_id, &qbox);
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
		qlist->super.delete_mailbox(list, name);
}

static void quota_storage_destroy(struct mail_storage *storage)
{
	struct quota_mail_storage *qstorage = QUOTA_CONTEXT(storage);

	quota_remove_user_storage(quota_set, storage);

	qstorage->super.destroy(storage);
}

void quota_mail_storage_created(struct mail_storage *storage)
{
	struct quota_mailbox_list *qlist = QUOTA_LIST_CONTEXT(storage->list);
	struct quota_mail_storage *qstorage;

	if (quota_next_hook_mail_storage_created != NULL)
		quota_next_hook_mail_storage_created(storage);

	qlist->storage = storage;

	qstorage = p_new(storage->pool, struct quota_mail_storage, 1);
	qstorage->super = storage->v;
	storage->v.destroy = quota_storage_destroy;
	storage->v.mailbox_open = quota_mailbox_open;

	if (!quota_storage_module_id_set) {
		quota_storage_module_id = mail_storage_module_id++;
		quota_storage_module_id_set = TRUE;
	}

	array_idx_set(&storage->module_contexts,
		      quota_storage_module_id, &qstorage);

	if ((storage->flags & MAIL_STORAGE_FLAG_SHARED_NAMESPACE) == 0) {
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
	qlist->super = list->v;
	list->v.delete_mailbox = quota_mailbox_list_delete;

	if (!quota_mailbox_list_module_id_set) {
		quota_mailbox_list_module_id = mailbox_list_module_id++;
		quota_mailbox_list_module_id_set = TRUE;
	}

	array_idx_set(&list->module_contexts,
		      quota_mailbox_list_module_id, &qlist);
}
