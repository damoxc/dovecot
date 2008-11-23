/* Copyright (c) 2005-2008 Dovecot authors, see the included COPYING file */

#include "lib.h"
#include "array.h"
#include "istream.h"
#include "mail-namespace.h"
#include "mail-search-build.h"
#include "quota-private.h"
#include "quota-plugin.h"
#include "trash-plugin.h"

#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>

#define INIT_TRASH_MAILBOX_COUNT 4
#define MAX_RETRY_COUNT 3

#define TRASH_USER_CONTEXT(obj) \
	MODULE_CONTEXT(obj, trash_user_module)

struct trash_mailbox {
	const char *name;
	int priority; /* lower number = higher priority */

	struct mail_storage *storage;

	/* temporarily set while cleaning: */
	struct mailbox *box;
	struct mailbox_transaction_context *trans;
	struct mail_search_context *search_ctx;
	struct mail *mail;

	unsigned int mail_set:1;
};

struct trash_user {
	union mail_user_module_context module_ctx;

	/* ordered by priority, highest first */
	ARRAY_DEFINE(trash_boxes, struct trash_mailbox);
};

const char *trash_plugin_version = PACKAGE_VERSION;

static MODULE_CONTEXT_DEFINE_INIT(trash_user_module,
				  &mail_user_module_register);
static void (*trash_next_hook_mail_namespaces_created)
	(struct mail_namespace *namespaces);
static int (*trash_next_quota_test_alloc)(struct quota_transaction_context *,
					  uoff_t, bool *);

static int trash_clean_mailbox_open(struct trash_mailbox *trash)
{
	struct mail_storage *storage = trash->storage;
	struct mail_search_args *search_args;

	trash->box = mailbox_open(&storage, trash->name, NULL,
				  MAILBOX_OPEN_KEEP_RECENT);
	if (trash->box == NULL)
		return 0;

	if (mailbox_sync(trash->box, MAILBOX_SYNC_FLAG_FULL_READ, 0, NULL) < 0)
		return -1;

	trash->trans = mailbox_transaction_begin(trash->box, 0);
	trash->mail = mail_alloc(trash->trans, MAIL_FETCH_PHYSICAL_SIZE |
				 MAIL_FETCH_RECEIVED_DATE, NULL);

	search_args = mail_search_build_init();
	mail_search_build_add_all(search_args);
	trash->search_ctx = mailbox_search_init(trash->trans,
						search_args, NULL);
	mail_search_args_unref(&search_args);

	return mailbox_search_next(trash->search_ctx, trash->mail);
}

static int trash_clean_mailbox_get_next(struct trash_mailbox *trash,
					time_t *received_time_r)
{
	int ret;

	if (!trash->mail_set) {
		if (trash->box == NULL)
			ret = trash_clean_mailbox_open(trash);
		else
			ret = mailbox_search_next(trash->search_ctx,
						  trash->mail);
		if (ret <= 0) {
			*received_time_r = 0;
			return ret;
		}
		trash->mail_set = TRUE;
	}

	if (mail_get_received_date(trash->mail, received_time_r) < 0)
		return -1;
	return 1;
}

static int trash_try_clean_mails(struct quota_transaction_context *ctx,
				 uint64_t size_needed)
{
	struct trash_user *tuser = TRASH_USER_CONTEXT(ctx->quota->user);
	struct trash_mailbox *trashes;
	unsigned int i, j, count, oldest_idx;
	time_t oldest, received = 0;
	uint64_t size, size_expunged = 0, expunged_count = 0;
	int ret = 0;

	trashes = array_get_modifiable(&tuser->trash_boxes, &count);
	for (i = 0; i < count; ) {
		/* expunge oldest mails first in all trash boxes with
		   same priority */
		oldest_idx = count;
		oldest = (time_t)-1;
		for (j = i; j < count; j++) {
			if (trashes[j].priority != trashes[i].priority)
				break;

			ret = trash_clean_mailbox_get_next(&trashes[j],
							   &received);
			if (ret < 0)
				goto err;
			if (ret > 0) {
				if (oldest == (time_t)-1 || received < oldest) {
					oldest = received;
					oldest_idx = j;
				}
			}
		}

		if (oldest_idx < count) {
			if (mail_get_physical_size(trashes[oldest_idx].mail,
						   &size) < 0) {
				/* maybe expunged already? */
				trashes[oldest_idx].mail_set = FALSE;
				continue;
			}

			mail_expunge(trashes[oldest_idx].mail);
			expunged_count++;
			size_expunged += size;
			if (size_expunged >= size_needed)
				break;
			trashes[oldest_idx].mail_set = FALSE;
		} else {
			/* find more mails from next priority's mailbox */
			i = j;
		}
	}

err:
	for (i = 0; i < count; i++) {
		struct trash_mailbox *trash = &trashes[i];

		if (trash->box == NULL)
			continue;

		trash->mail_set = FALSE;
		mail_free(&trash->mail);
		(void)mailbox_search_deinit(&trash->search_ctx);

		if (size_expunged >= size_needed)
			(void)mailbox_transaction_commit(&trash->trans);
		else {
			/* couldn't get enough space, don't expunge anything */
                        mailbox_transaction_rollback(&trash->trans);
		}

		mailbox_close(&trash->box);
	}

	if (size_expunged < size_needed) {
		if (getenv("DEBUG") != NULL) {
			i_info("trash plugin: Failed to remove enough messages "
			       "(needed %llu bytes, expunged only %llu bytes)",
			       (unsigned long long)size_needed,
			       (unsigned long long)size_expunged);
		}
		return FALSE;
	}

	ctx->bytes_used = ctx->bytes_used > (int64_t)size_expunged ?
		ctx->bytes_used - size_expunged : 0;
	ctx->count_used = ctx->count_used > (int64_t)expunged_count ?
		ctx->count_used - expunged_count : 0;
	return TRUE;
}

static int
trash_quota_test_alloc(struct quota_transaction_context *ctx,
		       uoff_t size, bool *too_large_r)
{
	int ret, i;

	for (i = 0; ; i++) {
		ret = trash_next_quota_test_alloc(ctx, size, too_large_r);
		if (ret != 0 || *too_large_r) {
			if (getenv("DEBUG") != NULL && *too_large_r) {
				i_info("trash plugin: Mail is larger than "
				       "quota, won't even try to handle");
			}
			return ret;
		}

		if (i == MAX_RETRY_COUNT) {
			/* trash_try_clean_mails() should have returned 0 if
			   it couldn't get enough space, but allow retrying
			   it a couple of times if there was some extra space
			   that was needed.. */
			break;
		}

		/* not enough space. try deleting some from mailbox. */
		ret = trash_try_clean_mails(ctx, size);
		if (ret <= 0)
			return 0;
	}

	return 0;
}

static bool trash_find_storage(struct mail_user *user,
			       struct trash_mailbox *trash)
{
	struct mail_namespace *ns;
	const char *name;

	for (ns = user->namespaces; ns != NULL; ns = ns->next) {
		name = trash->name;
		if (mail_namespace_update_name(ns, &name)) {
			if (name != trash->name)
				trash->name = p_strdup(user->pool, name);
			trash->storage = ns->storage;
			return TRUE;
		}
	}
	return FALSE;
}

static int trash_mailbox_priority_cmp(const void *p1, const void *p2)
{
	const struct trash_mailbox *t1 = p1, *t2 = p2;

	return t1->priority - t2->priority;
}

static int read_configuration(struct mail_user *user, const char *path)
{
	struct trash_user *tuser = TRASH_USER_CONTEXT(user);
	struct istream *input;
	const char *line, *name;
	struct trash_mailbox *trash;
	unsigned int count;
	int fd, ret = 0;

	fd = open(path, O_RDONLY);
	if (fd == -1) {
		i_error("trash plugin: open(%s) failed: %m", path);
		return -1;
	}

	p_array_init(&tuser->trash_boxes, user->pool, INIT_TRASH_MAILBOX_COUNT);

	input = i_stream_create_fd(fd, (size_t)-1, FALSE);
	i_stream_set_return_partial_line(input, TRUE);
	while ((line = i_stream_read_next_line(input)) != NULL) {
		/* <priority> <mailbox name> */
		name = strchr(line, ' ');
		if (name == NULL || name[1] == '\0' || *line == '#')
			continue;

		trash = array_append_space(&tuser->trash_boxes);
		trash->name = p_strdup(user->pool, name+1);
		trash->priority = atoi(t_strdup_until(line, name));

		if (!trash_find_storage(user, trash)) {
			i_error("trash: Namespace not found for mailbox '%s'",
				trash->name);
			ret = -1;
		}

		if (getenv("DEBUG") != NULL) {
			i_info("trash plugin: Added '%s' with priority %d",
			       trash->name, trash->priority);
		}
	}
	i_stream_destroy(&input);
	(void)close(fd);

	trash = array_get_modifiable(&tuser->trash_boxes, &count);
	qsort(trash, count, sizeof(*trash), trash_mailbox_priority_cmp);
	return ret;
}

static void
trash_hook_mail_namespaces_created(struct mail_namespace *namespaces)
{
	struct mail_user *user = namespaces->user;
	struct trash_user *tuser;
	const char *env;

	env = getenv("TRASH");
	if (env == NULL) {
		if (getenv("DEBUG") != NULL)
			i_info("trash: No trash setting - plugin disabled");
	} else if (quota_set == NULL) {
		i_error("trash plugin: quota plugin not initialized");
	} else {
		tuser = p_new(user->pool, struct trash_user, 1);
		tuser->module_ctx.super = user->v;
		MODULE_CONTEXT_SET(user, trash_user_module, tuser);

		if (read_configuration(user, env) == 0) {
			trash_next_quota_test_alloc = quota_set->test_alloc;
			quota_set->test_alloc = trash_quota_test_alloc;
		}
	}

	if (trash_next_hook_mail_namespaces_created != NULL)
		trash_next_hook_mail_namespaces_created(namespaces);
}

void trash_plugin_init(void)
{
	trash_next_hook_mail_namespaces_created = hook_mail_namespaces_created;
	hook_mail_namespaces_created = trash_hook_mail_namespaces_created;
}

void trash_plugin_deinit(void)
{
	hook_mail_namespaces_created = trash_hook_mail_namespaces_created;
	quota_set->test_alloc = trash_next_quota_test_alloc;
}
