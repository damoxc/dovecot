/* Copyright (c) 2002-2009 Dovecot authors, see the included COPYING file */

#include "lib.h"
#include "ioloop.h"
#include "array.h"
#include "llist.h"
#include "mkdir-parents.h"
#include "var-expand.h"
#include "mail-index-private.h"
#include "mailbox-list-private.h"
#include "mail-storage-private.h"
#include "mail-storage-settings.h"
#include "mail-namespace.h"
#include "mail-search.h"
#include "mailbox-search-result-private.h"

#include <stdlib.h>
#include <ctype.h>

struct mail_storage_module_register mail_storage_module_register = { 0 };
struct mail_module_register mail_module_register = { 0 };

struct mail_storage_mail_index_module mail_storage_mail_index_module =
	MODULE_CONTEXT_INIT(&mail_index_module_register);

void (*hook_mail_storage_created)(struct mail_storage *storage);
void (*hook_mailbox_allocated)(struct mailbox *box) = NULL;
void (*hook_mailbox_index_opened)(struct mailbox *box) = NULL;

ARRAY_TYPE(mail_storage) mail_storage_classes;

void mail_storage_init(void)
{
	mailbox_lists_init();
	i_array_init(&mail_storage_classes, 8);
}

void mail_storage_deinit(void)
{
	if (array_is_created(&mail_storage_classes))
		array_free(&mail_storage_classes);
	mailbox_lists_deinit();
}

void mail_storage_class_register(struct mail_storage *storage_class)
{
	i_assert(mail_storage_find_class(storage_class->name) == NULL);

	if (storage_class->v.class_init != NULL)
		storage_class->v.class_init();

	/* append it after the list, so the autodetection order is correct */
	array_append(&mail_storage_classes, &storage_class, 1);
}

void mail_storage_class_unregister(struct mail_storage *storage_class)
{
	struct mail_storage *const *classes;
	unsigned int i, count;

	classes = array_get(&mail_storage_classes, &count);
	for (i = 0; i < count; i++) {
		if (classes[i] == storage_class) {
			array_delete(&mail_storage_classes, i, 1);
			break;
		}
	}

	if (storage_class->v.class_deinit != NULL)
		storage_class->v.class_deinit();
}

struct mail_storage *mail_storage_find_class(const char *name)
{
	struct mail_storage *const *classes;
	unsigned int i, count;

	i_assert(name != NULL);

	classes = array_get(&mail_storage_classes, &count);
	for (i = 0; i < count; i++) {
		if (strcasecmp(classes[i]->name, name) == 0)
			return classes[i];
	}
	return NULL;
}

static struct mail_storage *
mail_storage_autodetect(const struct mail_namespace *ns,
			struct mailbox_list_settings *set)
{
	struct mail_storage *const *classes;
	unsigned int i, count;

	classes = array_get(&mail_storage_classes, &count);
	for (i = 0; i < count; i++) {
		if (classes[i]->v.autodetect != NULL) {
			if (classes[i]->v.autodetect(ns, set))
				return classes[i];
		}
	}
	return NULL;
}

static void
mail_storage_set_autodetection(const char **data, const char **driver)
{
	const char *p;

	/* check if data is in driver:data format (eg. mbox:~/mail) */
	p = *data;
	while (i_isalnum(*p)) p++;

	if (*p == ':' && p != *data) {
		/* no autodetection if the storage driver is given. */
		*driver = t_strdup_until(*data, p);
		*data = p + 1;
	}
}

static struct mail_storage *
mail_storage_get_class(struct mail_namespace *ns, const char *driver,
		       struct mailbox_list_settings *list_set,
		       const char **error_r)
{
	struct mail_storage *storage_class;
	const char *home;

	if (driver != NULL) {
		storage_class = mail_storage_find_class(driver);
		if (storage_class == NULL) {
			*error_r = t_strdup_printf(
				"Unknown mail storage driver %s", driver);
		}
		i_assert(list_set->root_dir != NULL);
		storage_class->v.get_list_settings(ns, list_set);
		return storage_class;
	}

	storage_class = mail_storage_autodetect(ns, list_set);
	if (storage_class != NULL)
		return storage_class;

	if (ns->set->location == NULL || *ns->set->location == '\0') {
		(void)mail_user_get_home(ns->user, &home);
		if (home == NULL || *home == '\0') home = "(not set)";

		*error_r = t_strdup_printf(
			"Mail storage autodetection failed with home=%s", home);
	} else {
		*error_r = t_strdup_printf(
			"Ambiguous mail location setting, "
			"don't know what to do with it: %s "
			"(try prefixing it with mbox: or maildir:)",
			ns->set->location);
	}
	return NULL;
}

static int
mail_storage_create_root(struct mailbox_list *list,
			 enum mail_storage_flags flags, const char **error_r)
{
	const char *root_dir, *origin;
	struct stat st;
	mode_t mode;
	gid_t gid;

	root_dir = mailbox_list_get_path(list, NULL,
					 MAILBOX_LIST_PATH_TYPE_MAILBOX);
	if (stat(root_dir, &st) == 0) {
		/* ok */
		return 0;
	} else if (errno == EACCES) {
		*error_r = mail_error_eacces_msg("stat", root_dir);
		return -1;
	} else if (errno != ENOENT && errno != ENOTDIR) {
		*error_r = t_strdup_printf("stat(%s) failed: %m", root_dir);
		return -1;
	} else if (list->ns->type == NAMESPACE_SHARED) {
		/* can't create a new user, but we don't want to fail
		   the storage creation. */
	} else if ((flags & MAIL_STORAGE_FLAG_NO_AUTOCREATE) != 0) {
		*error_r = t_strdup_printf(
			"Root mail directory doesn't exist: %s", root_dir);
		return -1;
	}

	/* we need to create the root directory. */
	mailbox_list_get_dir_permissions(list, NULL, &mode, &gid, &origin);
	if (mkdir_parents_chgrp(root_dir, mode, gid, origin) < 0 &&
	    errno != EEXIST) {
		*error_r = mail_error_create_eacces_msg("mkdir", root_dir);
		return -1;
	} else {
		/* created */
		return 0;
	}
}

static struct mail_storage *
mail_storage_find(struct mail_user *user,
		  const struct mail_storage *storage_class,
		  const struct mailbox_list_settings *set)
{
	struct mail_storage *storage = user->storages;

	for (; storage != NULL; storage = storage->next) {
		if (strcmp(storage->name, storage_class->name) == 0 &&
		    ((storage->class_flags &
		      MAIL_STORAGE_CLASS_FLAG_UNIQUE_ROOT) == 0 ||
		     strcmp(storage->unique_root_dir, set->root_dir) == 0))
			return storage;
	}
	return NULL;
}

int mail_storage_create(struct mail_namespace *ns, const char *driver,
			enum mail_storage_flags flags, const char **error_r)
{
	struct mail_storage *storage_class, *storage = NULL;
	struct mailbox_list_settings list_set;
	enum mailbox_list_flags list_flags = 0;
	const char *data = ns->set->location;
	const char *p;

	if ((flags & MAIL_STORAGE_FLAG_KEEP_HEADER_MD5) == 0 &&
	    ns->mail_set->pop3_uidl_format != NULL) {
		/* if pop3_uidl_format contains %m, we want to keep the
		   header MD5 sums stored even if we're not running POP3
		   right now. */
		p = ns->mail_set->pop3_uidl_format;
		while ((p = strchr(p, '%')) != NULL) {
			if (p[1] == '%')
				p += 2;
			else if (var_get_key(++p) == 'm') {
				flags |= MAIL_STORAGE_FLAG_KEEP_HEADER_MD5;
				break;
			}
		}
	}

	memset(&list_set, 0, sizeof(list_set));
	if (data == NULL)
		data = "";
	else {
		if (driver == NULL)
			mail_storage_set_autodetection(&data, &driver);
		if (mailbox_list_settings_parse(data, &list_set, ns,
						error_r) < 0)
			return -1;
	}

	if ((flags & MAIL_STORAGE_FLAG_NO_AUTODETECTION) != 0 &&
	    list_set.root_dir == NULL) {
		*error_r = "Root mail directory not given";
		return -1;
	}

	storage_class = mail_storage_get_class(ns, driver, &list_set, error_r);
	if (storage_class == NULL)
		return -1;
	i_assert(list_set.layout != NULL);

	if (ns->list == NULL) {
		/* first storage for namespace */
		if (mail_storage_is_mailbox_file(storage_class))
			list_flags |= MAILBOX_LIST_FLAG_MAILBOX_FILES;
		if (mailbox_list_create(list_set.layout, ns, &list_set,
					list_flags, error_r) < 0)
			return -1;
		if (mail_storage_create_root(ns->list, flags, error_r) < 0)
			return -1;
	}

	storage = mail_storage_find(ns->user, storage_class, &list_set);
	if (storage != NULL) {
		/* using an existing storage */
		storage->refcount++;
		mail_namespace_add_storage(ns, storage);
		return 0;
	}

	storage = storage_class->v.alloc();
	storage->refcount = 1;
	storage->storage_class = storage_class;
	storage->user = ns->user;
	storage->set = ns->mail_set;
	storage->flags = flags;
	p_array_init(&storage->module_contexts, storage->pool, 5);

	if (storage->v.create != NULL &&
	    storage->v.create(storage, ns, error_r) < 0) {
		*error_r = t_strdup_printf("%s: %s", storage->name, *error_r);
		pool_unref(&storage->pool);
		return -1;
	}

	if (hook_mail_storage_created != NULL) T_BEGIN {
		hook_mail_storage_created(storage);
	} T_END;

	DLLIST_PREPEND(&ns->user->storages, storage);
        mail_namespace_add_storage(ns, storage);
	return 0;
}

void mail_storage_ref(struct mail_storage *storage)
{
	storage->refcount++;
}

void mail_storage_unref(struct mail_storage **_storage)
{
	struct mail_storage *storage = *_storage;

	i_assert(storage->refcount > 0);

	*_storage = NULL;

	if (--storage->refcount > 0)
		return;

	DLLIST_REMOVE(&storage->user->storages, storage);

	if (storage->v.destroy != NULL)
		storage->v.destroy(storage);
	i_free(storage->error_string);
	pool_unref(&storage->pool);
}

void mail_storage_clear_error(struct mail_storage *storage)
{
	i_free_and_null(storage->error_string);

	storage->error = MAIL_ERROR_NONE;
}

void mail_storage_set_error(struct mail_storage *storage,
			    enum mail_error error, const char *string)
{
	i_free(storage->error_string);
	storage->error_string = i_strdup(string);
	storage->error = error;
}

void mail_storage_set_internal_error(struct mail_storage *storage)
{
	struct tm *tm;
	char str[256];

	tm = localtime(&ioloop_time);

	i_free(storage->error_string);
	storage->error_string =
		strftime(str, sizeof(str),
			 MAIL_ERRSTR_CRITICAL_MSG_STAMP, tm) > 0 ?
		i_strdup(str) : i_strdup(MAIL_ERRSTR_CRITICAL_MSG);
	storage->error = MAIL_ERROR_TEMP;
}

void mail_storage_set_critical(struct mail_storage *storage,
			       const char *fmt, ...)
{
	va_list va;

	mail_storage_clear_error(storage);
	if (fmt != NULL) {
		va_start(va, fmt);
		i_error("%s", t_strdup_vprintf(fmt, va));
		va_end(va);

		/* critical errors may contain sensitive data, so let user
		   see only "Internal error" with a timestamp to make it
		   easier to look from log files the actual error message. */
		mail_storage_set_internal_error(storage);
	}
}

const struct mail_storage_settings *
mail_storage_get_settings(struct mail_storage *storage)
{
	return storage->set;
}

void mail_storage_set_callbacks(struct mail_storage *storage,
				struct mail_storage_callbacks *callbacks,
				void *context)
{
	storage->callbacks = *callbacks;
	storage->callback_context = context;
}

int mail_storage_purge(struct mail_storage *storage)
{
	mail_storage_clear_error(storage);

	return storage->v.purge == NULL ? 0 :
		storage->v.purge(storage);
}

const char *mail_storage_get_last_error(struct mail_storage *storage,
					enum mail_error *error_r)
{
	/* We get here only in error situations, so we have to return some
	   error. If storage->error is NONE, it means we forgot to set it at
	   some point.. */
	if (storage->error == MAIL_ERROR_NONE) {
		if (error_r != NULL)
			*error_r = MAIL_ERROR_TEMP;
		return storage->error_string != NULL ? storage->error_string :
			"BUG: Unknown internal error";
	}

	if (storage->error_string == NULL) {
		/* This shouldn't happen.. */
		storage->error_string =
			i_strdup_printf("BUG: Unknown 0x%x error",
					storage->error);
	}

	if (error_r != NULL)
		*error_r = storage->error;
	return storage->error_string;
}

bool mail_storage_is_mailbox_file(struct mail_storage *storage)
{
	return (storage->class_flags &
		MAIL_STORAGE_CLASS_FLAG_MAILBOX_IS_FILE) != 0;
}

bool mail_storage_set_error_from_errno(struct mail_storage *storage)
{
	const char *error_string;
	enum mail_error error;

	if (!mail_error_from_errno(&error, &error_string))
		return FALSE;
	if (storage->set->mail_debug && error != MAIL_ERROR_NOTFOUND) {
		/* debugging is enabled - admin may be debugging a
		   (permission) problem, so return FALSE to get the caller to
		   log the full error message. */
		return FALSE;
	}

	mail_storage_set_error(storage, error, error_string);
	return TRUE;
}

struct mailbox *mailbox_alloc(struct mailbox_list *list, const char *name,
			      struct istream *input,
			      enum mailbox_flags flags)
{
	struct mailbox_list *new_list = list;
	struct mail_storage *storage;
	struct mailbox *box;

	if (mailbox_list_get_storage(&new_list, &name, &storage) < 0) {
		/* just use the first storage. FIXME: does this break? */
		storage = list->ns->storage;
	}

	T_BEGIN {
		box = storage->v.mailbox_alloc(storage, new_list,
					       name, input, flags);
		if (hook_mailbox_allocated != NULL)
			hook_mailbox_allocated(box);
	} T_END;
	return box;
}

int mailbox_open(struct mailbox *box)
{
	int ret;

	mail_storage_clear_error(box->storage);

	if (!mailbox_list_is_valid_existing_name(box->list, box->name)) {
		mail_storage_set_error(box->storage, MAIL_ERROR_PARAMS,
				       "Invalid mailbox name");
		return -1;
	}

	T_BEGIN {
		ret = box->v.open(box);
	} T_END;

	if (ret < 0)
		return -1;

	box->list->ns->flags |= NAMESPACE_FLAG_USABLE;
	return 0;
}

int mailbox_enable(struct mailbox *box, enum mailbox_feature features)
{
	return box->v.enable(box, features);
}

enum mailbox_feature mailbox_get_enabled_features(struct mailbox *box)
{
	return box->enabled_features;
}

void mailbox_close(struct mailbox **_box)
{
	struct mailbox *box = *_box;

	if (box->transaction_count != 0) {
		i_panic("Trying to close mailbox %s with open transactions",
			box->name);
	}

	*_box = NULL;
	box->v.close(box);
}

int mailbox_create(struct mailbox *box, const struct mailbox_update *update,
		   bool directory)
{
	mail_storage_clear_error(box->storage);

	if (!mailbox_list_is_valid_create_name(box->list, box->name)) {
		mail_storage_set_error(box->storage, MAIL_ERROR_PARAMS,
				       "Invalid mailbox name");
		return -1;
	}

	return box->v.create(box, update, directory);
}

int mailbox_update(struct mailbox *box, const struct mailbox_update *update)
{
	mail_storage_clear_error(box->storage);

	return box->v.update(box, update);
}

struct mail_storage *mailbox_get_storage(const struct mailbox *box)
{
	return box->storage;
}

struct mail_namespace *
mailbox_get_namespace(const struct mailbox *box)
{
	return box->list->ns;
}

const struct mail_storage_settings *mailbox_get_settings(struct mailbox *box)
{
	return box->storage->set;
}

const char *mailbox_get_name(const struct mailbox *box)
{
	return box->name;
}

bool mailbox_is_readonly(struct mailbox *box)
{
	return box->v.is_readonly(box);
}

bool mailbox_allow_new_keywords(struct mailbox *box)
{
	return box->v.allow_new_keywords(box);
}

void mailbox_get_status(struct mailbox *box,
			enum mailbox_status_items items,
			struct mailbox_status *status_r)
{
	box->v.get_status(box, items, status_r);
}

struct mailbox_sync_context *
mailbox_sync_init(struct mailbox *box, enum mailbox_sync_flags flags)
{
	if (box->transaction_count != 0) {
		i_panic("Trying to sync mailbox %s with open transactions",
			box->name);
	}
	return box->v.sync_init(box, flags);
}

bool mailbox_sync_next(struct mailbox_sync_context *ctx,
		       struct mailbox_sync_rec *sync_rec_r)
{
	return ctx->box->v.sync_next(ctx, sync_rec_r);
}

int mailbox_sync_deinit(struct mailbox_sync_context **_ctx,
			enum mailbox_status_items status_items,
			struct mailbox_status *status_r)
{
	struct mailbox_sync_context *ctx = *_ctx;

	*_ctx = NULL;
	return ctx->box->v.sync_deinit(ctx, status_items, status_r);
}

int mailbox_sync(struct mailbox *box, enum mailbox_sync_flags flags,
		 enum mailbox_status_items status_items,
		 struct mailbox_status *status_r)
{
	struct mailbox_sync_context *ctx;

	if (array_count(&box->search_results) == 0) {
		/* we don't care about mailbox's current state, so we might
		   as well fix inconsistency state */
		flags |= MAILBOX_SYNC_FLAG_FIX_INCONSISTENT;
	}

	ctx = mailbox_sync_init(box, flags);
	return mailbox_sync_deinit(&ctx, status_items, status_r);
}

#undef mailbox_notify_changes
void mailbox_notify_changes(struct mailbox *box, unsigned int min_interval,
			    mailbox_notify_callback_t *callback, void *context)
{
	box->notify_min_interval = min_interval;
	box->notify_callback = callback;
	box->notify_context = context;

	box->v.notify_changes(box);
}

void mailbox_notify_changes_stop(struct mailbox *box)
{
	mailbox_notify_changes(box, 0, NULL, NULL);
}

int mailbox_keywords_create(struct mailbox *box, const char *const keywords[],
			    struct mail_keywords **keywords_r)
{
	const char *empty_keyword_list = NULL;

	if (keywords == NULL)
		keywords = &empty_keyword_list;
	return box->v.keywords_create(box, keywords, keywords_r, FALSE);
}

struct mail_keywords *
mailbox_keywords_create_valid(struct mailbox *box,
			      const char *const keywords[])
{
	const char *empty_keyword_list = NULL;
	struct mail_keywords *kw;

	if (keywords == NULL)
		keywords = &empty_keyword_list;
	if (box->v.keywords_create(box, keywords, &kw, TRUE) < 0)
		i_unreached();
	return kw;
}

struct mail_keywords *
mailbox_keywords_create_from_indexes(struct mailbox *box,
				     const ARRAY_TYPE(keyword_indexes) *idx)
{
	return box->v.keywords_create_from_indexes(box, idx);
}

void mailbox_keywords_ref(struct mailbox *box, struct mail_keywords *keywords)
{
	box->v.keywords_ref(keywords);
}

void mailbox_keywords_unref(struct mailbox *box,
			    struct mail_keywords **_keywords)
{
	struct mail_keywords *keywords = *_keywords;

	*_keywords = NULL;
	box->v.keywords_unref(keywords);
}

bool mailbox_keyword_is_valid(struct mailbox *box, const char *keyword,
			      const char **error_r)
{
	return box->v.keyword_is_valid(box, keyword, error_r);
}

void mailbox_get_seq_range(struct mailbox *box, uint32_t uid1, uint32_t uid2,
			   uint32_t *seq1_r, uint32_t *seq2_r)
{
	box->v.get_seq_range(box, uid1, uid2, seq1_r, seq2_r);
}

void mailbox_get_uid_range(struct mailbox *box,
			   const ARRAY_TYPE(seq_range) *seqs,
			   ARRAY_TYPE(seq_range) *uids)
{
	box->v.get_uid_range(box, seqs, uids);
}

bool mailbox_get_expunged_uids(struct mailbox *box, uint64_t modseq,
			       const ARRAY_TYPE(seq_range) *uids,
			       ARRAY_TYPE(seq_range) *expunged_uids)
{
	return box->v.get_expunged_uids(box, modseq, uids, expunged_uids);
}

bool mailbox_get_virtual_uid(struct mailbox *box, const char *backend_mailbox,
			     uint32_t backend_uidvalidity,
			     uint32_t backend_uid, uint32_t *uid_r)
{
	if (box->v.get_virtual_uid == NULL)
		return FALSE;
	return box->v.get_virtual_uid(box, backend_mailbox, backend_uidvalidity,
				      backend_uid, uid_r);
}

void mailbox_get_virtual_backend_boxes(struct mailbox *box,
				       ARRAY_TYPE(mailboxes) *mailboxes,
				       bool only_with_msgs)
{
	if (box->v.get_virtual_backend_boxes == NULL)
		array_append(mailboxes, &box, 1);
	else
		box->v.get_virtual_backend_boxes(box, mailboxes, only_with_msgs);
}

void mailbox_get_virtual_box_patterns(struct mailbox *box,
				ARRAY_TYPE(mailbox_virtual_patterns) *includes,
				ARRAY_TYPE(mailbox_virtual_patterns) *excludes)
{
	if (box->v.get_virtual_box_patterns == NULL) {
		struct mailbox_virtual_pattern pat;

		memset(&pat, 0, sizeof(pat));
		pat.ns = box->list->ns;
		pat.pattern = box->name;
		array_append(includes, &pat, 1);
	} else {
		box->v.get_virtual_box_patterns(box, includes, excludes);
	}
}

struct mailbox_header_lookup_ctx *
mailbox_header_lookup_init(struct mailbox *box, const char *const headers[])
{
	return box->v.header_lookup_init(box, headers);
}

void mailbox_header_lookup_ref(struct mailbox_header_lookup_ctx *ctx)
{
	i_assert(ctx->refcount > 0);
	ctx->refcount++;
}

void mailbox_header_lookup_unref(struct mailbox_header_lookup_ctx **_ctx)
{
	struct mailbox_header_lookup_ctx *ctx = *_ctx;

	*_ctx = NULL;

	i_assert(ctx->refcount > 0);
	if (--ctx->refcount > 0)
		return;

	ctx->box->v.header_lookup_deinit(ctx);
}

struct mail_search_context *
mailbox_search_init(struct mailbox_transaction_context *t,
		    struct mail_search_args *args,
		    const enum mail_sort_type *sort_program)
{
	mail_search_args_ref(args);
	if (!args->simplified)
		mail_search_args_simplify(args);
	return t->box->v.search_init(t, args, sort_program);
}

int mailbox_search_deinit(struct mail_search_context **_ctx)
{
	struct mail_search_context *ctx = *_ctx;
	struct mail_search_args *args = ctx->args;
	int ret;

	*_ctx = NULL;
	mailbox_search_results_initial_done(ctx);
	ret = ctx->transaction->box->v.search_deinit(ctx);
	mail_search_args_unref(&args);
	return ret;
}

int mailbox_search_next(struct mail_search_context *ctx, struct mail *mail)
{
	bool tryagain;
	int ret;

	while ((ret = mailbox_search_next_nonblock(ctx, mail,
						   &tryagain)) == 0) {
		if (!tryagain)
			break;
	}

	return ret;
}

int mailbox_search_next_nonblock(struct mail_search_context *ctx,
				 struct mail *mail, bool *tryagain_r)
{
	int ret;

	ret = ctx->transaction->box->v.
		search_next_nonblock(ctx, mail, tryagain_r);
	if (ret > 0)
		mailbox_search_results_add(ctx, mail->uid);
	return ret;
}

bool mailbox_search_seen_lost_data(struct mail_search_context *ctx)
{
	return ctx->seen_lost_data;
}

int mailbox_search_result_build(struct mailbox_transaction_context *t,
				struct mail_search_args *args,
				enum mailbox_search_result_flags flags,
				struct mail_search_result **result_r)
{
	struct mail_search_context *ctx;
	struct mail *mail;
	int ret;

	ctx = mailbox_search_init(t, args, NULL);
	*result_r = mailbox_search_result_save(ctx, flags);
	mail = mail_alloc(t, 0, NULL);
	while (mailbox_search_next(ctx, mail) > 0) ;
	mail_free(&mail);

	ret = mailbox_search_deinit(&ctx);
	if (ret < 0)
		mailbox_search_result_free(result_r);
	return ret;
}

struct mailbox_transaction_context *
mailbox_transaction_begin(struct mailbox *box,
			  enum mailbox_transaction_flags flags)
{
	struct mailbox_transaction_context *trans;

	box->transaction_count++;
	trans = box->v.transaction_begin(box, flags);
	trans->flags = flags;
	return trans;
}

int mailbox_transaction_commit(struct mailbox_transaction_context **t)
{
	uint32_t uidvalidity, uid1, uid2;

	/* Store the return values to separate temporary variables so that
	   plugins overriding transaction_commit() can look at them. */
	return mailbox_transaction_commit_get_uids(t, &uidvalidity,
						   &uid1, &uid2);
}

int mailbox_transaction_commit_get_uids(struct mailbox_transaction_context **_t,
					uint32_t *uid_validity_r,
					uint32_t *first_saved_uid_r,
					uint32_t *last_saved_uid_r)
{
	struct mailbox_transaction_context *t = *_t;

	t->box->transaction_count--;

	*_t = NULL;
	return t->box->v.transaction_commit(t, uid_validity_r,
					    first_saved_uid_r,
					    last_saved_uid_r);
}

void mailbox_transaction_rollback(struct mailbox_transaction_context **_t)
{
	struct mailbox_transaction_context *t = *_t;

	t->box->transaction_count--;

	*_t = NULL;
	t->box->v.transaction_rollback(t);
}

unsigned int mailbox_transaction_get_count(const struct mailbox *box)
{
	return box->transaction_count;
}

void mailbox_transaction_set_max_modseq(struct mailbox_transaction_context *t,
					uint64_t max_modseq,
					ARRAY_TYPE(seq_range) *seqs)
{
	t->box->v.transaction_set_max_modseq(t, max_modseq, seqs);
}

struct mailbox *
mailbox_transaction_get_mailbox(const struct mailbox_transaction_context *t)
{
	return t->box;
}

struct mail_save_context *
mailbox_save_alloc(struct mailbox_transaction_context *t)
{
	struct mail_save_context *ctx;

	ctx = t->box->v.save_alloc(t);
	ctx->received_date = (time_t)-1;
	return ctx;
}

void mailbox_save_set_flags(struct mail_save_context *ctx,
			    enum mail_flags flags,
			    struct mail_keywords *keywords)
{
	ctx->flags = flags;
	ctx->keywords = keywords;
	if (keywords != NULL)
		mailbox_keywords_ref(ctx->transaction->box, keywords);
}

void mailbox_save_copy_flags(struct mail_save_context *ctx, struct mail *mail)
{
	const char *const *keywords_list;

	keywords_list = mail_get_keywords(mail);
	ctx->keywords = str_array_length(keywords_list) == 0 ? NULL :
		mailbox_keywords_create_valid(ctx->transaction->box,
					      keywords_list);
	ctx->flags = mail_get_flags(mail);
}

void mailbox_save_set_received_date(struct mail_save_context *ctx,
				    time_t received_date, int timezone_offset)
{
	ctx->received_date = received_date;
	ctx->received_tz_offset = timezone_offset;
}

void mailbox_save_set_from_envelope(struct mail_save_context *ctx,
				    const char *envelope)
{
	i_free(ctx->from_envelope);
	ctx->from_envelope = i_strdup(envelope);
}

void mailbox_save_set_guid(struct mail_save_context *ctx, const char *guid)
{
	i_assert(guid == NULL || *guid != '\0');

	i_free(ctx->guid);
	ctx->guid = i_strdup(guid);
}

void mailbox_save_set_dest_mail(struct mail_save_context *ctx,
				struct mail *mail)
{
	ctx->dest_mail = mail;
}

int mailbox_save_begin(struct mail_save_context **ctx, struct istream *input)
{
	struct mailbox *box = (*ctx)->transaction->box;
	int ret;

	if (box->v.save_begin == NULL) {
		mail_storage_set_error(box->storage, MAIL_ERROR_NOTPOSSIBLE,
				       "Saving messages not supported");
		ret = -1;
	} else {
		ret = box->v.save_begin(*ctx, input);
	}

	if (ret < 0) {
		mailbox_save_cancel(ctx);
		return -1;
	}
	return 0;
}

int mailbox_save_continue(struct mail_save_context *ctx)
{
	return ctx->transaction->box->v.save_continue(ctx);
}

int mailbox_save_finish(struct mail_save_context **_ctx)
{
	struct mail_save_context *ctx = *_ctx;
	struct mailbox *box = ctx->transaction->box;
	struct mail_keywords *keywords = ctx->keywords;
	int ret;

	*_ctx = NULL;
	ret = box->v.save_finish(ctx);
	if (keywords != NULL)
		mailbox_keywords_unref(box, &keywords);
	return ret;
}

void mailbox_save_cancel(struct mail_save_context **_ctx)
{
	struct mail_save_context *ctx = *_ctx;
	struct mailbox *box = ctx->transaction->box;
	struct mail_keywords *keywords = ctx->keywords;

	*_ctx = NULL;
	ctx->transaction->box->v.save_cancel(ctx);
	if (keywords != NULL)
		mailbox_keywords_unref(box, &keywords);
}

int mailbox_copy(struct mail_save_context **_ctx, struct mail *mail)
{
	struct mail_save_context *ctx = *_ctx;
	struct mailbox *box = ctx->transaction->box;
	struct mail_keywords *keywords = ctx->keywords;
	int ret;

	*_ctx = NULL;
	ret = ctx->transaction->box->v.copy(ctx, mail);
	if (keywords != NULL)
		mailbox_keywords_unref(box, &keywords);
	return ret;
}

bool mailbox_is_inconsistent(struct mailbox *box)
{
	return box->mailbox_deleted || box->v.is_inconsistent(box);
}

void mailbox_set_deleted(struct mailbox *box)
{
	mail_storage_set_error(box->storage, MAIL_ERROR_NOTFOUND,
			       "Mailbox was deleted under us");
	box->mailbox_deleted = TRUE;
}

bool mailbox_guid_is_empty(const uint8_t guid[MAILBOX_GUID_SIZE])
{
	unsigned int i;

	for (i = 0; i < MAILBOX_GUID_SIZE; i++) {
		if (guid[i] != 0)
			return FALSE;
	}
	return TRUE;
}
