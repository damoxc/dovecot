/* Copyright (c) 2010 Dovecot authors, see the included COPYING file */

#include "lib.h"
#include "array.h"
#include "str.h"
#include "imap-utf7.h"
#include "mail-namespace.h"
#include "mail-storage.h"
#include "mail-search-build.h"
#include "doveadm-mail-list-iter.h"
#include "doveadm-mail.h"

#include <stdio.h>

struct doveadm_mailbox_cmd_context {
	struct doveadm_mail_cmd_context ctx;
	bool mutf7;
};

struct mailbox_cmd_context {
	struct doveadm_mailbox_cmd_context ctx;
	ARRAY_TYPE(const_string) mailboxes;
};

struct rename_cmd_context {
	struct doveadm_mailbox_cmd_context ctx;
	const char *oldname, *newname;
};

struct list_cmd_context {
	struct doveadm_mailbox_cmd_context ctx;
	struct mail_search_args *search_args;
};

static const char *const *
doveadm_mailbox_args_to_mutf7(const char *const args[])
{
	ARRAY_TYPE(const_string) dest;
	string_t *str;
	const char *mutf7;
	unsigned int i;

	str = t_str_new(128);
	t_array_init(&dest, 8);
	for (i = 0; args[i] != NULL; i++) {
		str_truncate(str, 0);
		if (imap_utf8_to_utf7(args[i], str) < 0)
			i_fatal("Mailbox name not valid UTF-8: %s", args[i]);
		mutf7 = t_strdup(str_c(str));
		array_append(&dest, &mutf7, 1);
	}
	(void)array_append_space(&dest);
	return array_idx(&dest, 0);
}

static void
doveadm_mailbox_args_validate_mutf7(const char *const *args)
{
	string_t *str = t_str_new(128);
	unsigned int i;

	for (i = 0; args[i] != NULL; i++) {
		if (imap_utf7_to_utf8(args[i], str) < 0)
			i_fatal("Mailbox name not valid mUTF-7: %s", args[i]);
		str_truncate(str, 0);
	}
}

static bool cmd_mailbox_parse_arg(struct doveadm_mail_cmd_context *_ctx, int c)
{
	struct doveadm_mailbox_cmd_context *ctx =
		(struct doveadm_mailbox_cmd_context *)_ctx;

	switch (c) {
	case '7':
		ctx->mutf7 = TRUE;
		break;
	case '8':
		ctx->mutf7 = FALSE;
		break;
	default:
		return FALSE;
	}
	return TRUE;
}

#define doveadm_mailbox_cmd_alloc(type) \
	(type *)doveadm_mailbox_cmd_alloc_size(sizeof(type))
static struct doveadm_mail_cmd_context *
doveadm_mailbox_cmd_alloc_size(size_t size)
{
	struct doveadm_mail_cmd_context *ctx;

	ctx = doveadm_mail_cmd_alloc_size(size);
	ctx->getopt_args = "78";
	ctx->parse_arg = cmd_mailbox_parse_arg;
	return ctx;
}

static void
doveadm_mailbox_translate_args(struct doveadm_mailbox_cmd_context *ctx,
			       const char *const *args[])
{
	if (!ctx->mutf7)
		*args = doveadm_mailbox_args_to_mutf7(*args);
	else
		doveadm_mailbox_args_validate_mutf7(*args);
}

static void
cmd_mailbox_list_run(struct doveadm_mail_cmd_context *_ctx,
		     struct mail_user *user)
{
	struct list_cmd_context *ctx = (struct list_cmd_context *)_ctx;
	const enum mailbox_list_iter_flags iter_flags =
		MAILBOX_LIST_ITER_RAW_LIST |
		MAILBOX_LIST_ITER_VIRTUAL_NAMES |
		MAILBOX_LIST_ITER_NO_AUTO_INBOX |
		MAILBOX_LIST_ITER_RETURN_NO_FLAGS;
	struct doveadm_mail_list_iter *iter;
	const struct mailbox_info *info;
	string_t *str = t_str_new(256);

	iter = doveadm_mail_list_iter_init(user, ctx->search_args, iter_flags);
	while ((info = doveadm_mail_list_iter_next(iter)) != NULL) {
		str_truncate(str, 0);
		if (ctx->ctx.mutf7 || imap_utf7_to_utf8(info->name, str) < 0)
			printf("%s\n", info->name);
		else
			printf("%s\n", str_c(str));
	}
	doveadm_mail_list_iter_deinit(&iter);
}

static void cmd_mailbox_list_init(struct doveadm_mail_cmd_context *_ctx,
				  const char *const args[])
{
	struct list_cmd_context *ctx = (struct list_cmd_context *)_ctx;
	struct mail_search_arg *arg;
	unsigned int i;

	doveadm_mailbox_translate_args(&ctx->ctx, &args);
	ctx->search_args = mail_search_build_init();
	for (i = 0; args[i] != NULL; i++) {
		arg = mail_search_build_add(ctx->search_args,
					    SEARCH_MAILBOX_GLOB);
		arg->value.str = p_strdup(ctx->search_args->pool, args[i]);
	}
	if (i > 1) {
		struct mail_search_arg *subargs = ctx->search_args->args;

		ctx->search_args->args = NULL;
		arg = mail_search_build_add(ctx->search_args, SEARCH_OR);
		arg->value.subargs = subargs;
	}
}

static struct doveadm_mail_cmd_context *cmd_mailbox_list_alloc(void)
{
	struct list_cmd_context *ctx;

	ctx = doveadm_mailbox_cmd_alloc(struct list_cmd_context);
	ctx->ctx.ctx.init = cmd_mailbox_list_init;
	ctx->ctx.ctx.run = cmd_mailbox_list_run;
	return &ctx->ctx.ctx;
}

static void
cmd_mailbox_create_run(struct doveadm_mail_cmd_context *_ctx,
		       struct mail_user *user)
{
	struct mailbox_cmd_context *ctx = (struct mailbox_cmd_context *)_ctx;
	struct mail_namespace *ns;
	struct mailbox *box;
	const char *const *namep;

	array_foreach(&ctx->mailboxes, namep) {
		const char *storage_name = *namep;
		unsigned int len;
		bool directory = FALSE;

		ns = mail_namespace_find(user->namespaces, &storage_name);
		if (ns == NULL)
			i_fatal("Can't find namespace for: %s", *namep);

		len = strlen(storage_name);
		if (len > 0 && storage_name[len-1] == ns->real_sep) {
			storage_name = t_strndup(storage_name, len-1);
			directory = TRUE;
		}

		box = mailbox_alloc(ns->list, storage_name, 0);
		if (mailbox_create(box, NULL, directory) < 0) {
			struct mail_storage *storage = mailbox_get_storage(box);

			i_error("Can't create mailbox %s: %s", *namep,
				mail_storage_get_last_error(storage, NULL));
		}
		mailbox_free(&box);
	}
}

static void cmd_mailbox_create_init(struct doveadm_mail_cmd_context *_ctx,
				    const char *const args[])
{
	struct mailbox_cmd_context *ctx = (struct mailbox_cmd_context *)_ctx;
	const char *name;
	unsigned int i;

	if (args[0] == NULL)
		doveadm_mail_help_name("mailbox create");
	doveadm_mailbox_translate_args(&ctx->ctx, &args);

	for (i = 0; args[i] != NULL; i++) {
		name = p_strdup(ctx->ctx.ctx.pool, args[i]);
		array_append(&ctx->mailboxes, &name, 1);
	}
}

static struct doveadm_mail_cmd_context *cmd_mailbox_create_alloc(void)
{
	struct mailbox_cmd_context *ctx;

	ctx = doveadm_mailbox_cmd_alloc(struct mailbox_cmd_context);
	ctx->ctx.ctx.init = cmd_mailbox_create_init;
	ctx->ctx.ctx.run = cmd_mailbox_create_run;
	p_array_init(&ctx->mailboxes, ctx->ctx.ctx.pool, 16);
	return &ctx->ctx.ctx;
}

static void
cmd_mailbox_delete_run(struct doveadm_mail_cmd_context *_ctx,
		       struct mail_user *user)
{
	struct mailbox_cmd_context *ctx = (struct mailbox_cmd_context *)_ctx;
	struct mail_namespace *ns;
	struct mailbox *box;
	const char *const *namep;

	array_foreach(&ctx->mailboxes, namep) {
		const char *storage_name = *namep;

		ns = mail_namespace_find(user->namespaces, &storage_name);
		if (ns == NULL)
			i_fatal("Can't find namespace for: %s", *namep);

		box = mailbox_alloc(ns->list, storage_name, 0);
		if (mailbox_delete(box) < 0) {
			struct mail_storage *storage = mailbox_get_storage(box);

			i_error("Can't delete mailbox %s: %s", *namep,
				mail_storage_get_last_error(storage, NULL));
		}
		mailbox_free(&box);
	}
}

static void cmd_mailbox_delete_init(struct doveadm_mail_cmd_context *_ctx,
				    const char *const args[])
{
	struct mailbox_cmd_context *ctx = (struct mailbox_cmd_context *)_ctx;
	const char *name;
	unsigned int i;

	if (args[0] == NULL)
		doveadm_mail_help_name("mailbox delete");
	doveadm_mailbox_translate_args(&ctx->ctx, &args);

	for (i = 0; args[i] != NULL; i++) {
		name = p_strdup(ctx->ctx.ctx.pool, args[i]);
		array_append(&ctx->mailboxes, &name, 1);
	}
}

static struct doveadm_mail_cmd_context *cmd_mailbox_delete_alloc(void)
{
	struct mailbox_cmd_context *ctx;

	ctx = doveadm_mailbox_cmd_alloc(struct mailbox_cmd_context);
	ctx->ctx.ctx.init = cmd_mailbox_delete_init;
	ctx->ctx.ctx.run = cmd_mailbox_delete_run;
	p_array_init(&ctx->mailboxes, ctx->ctx.ctx.pool, 16);
	return &ctx->ctx.ctx;
}

static void
cmd_mailbox_rename_run(struct doveadm_mail_cmd_context *_ctx,
		       struct mail_user *user)
{
	struct rename_cmd_context *ctx = (struct rename_cmd_context *)_ctx;
	struct mail_namespace *oldns, *newns;
	struct mailbox *oldbox, *newbox;
	const char *oldname = ctx->oldname;
	const char *newname = ctx->newname;

	oldns = mail_namespace_find(user->namespaces, &oldname);
	if (oldns == NULL)
		i_fatal("Can't find namespace for: %s", oldname);
	newns = mail_namespace_find(user->namespaces, &newname);
	if (newns == NULL)
		i_fatal("Can't find namespace for: %s", newname);

	oldbox = mailbox_alloc(oldns->list, oldname, 0);
	newbox = mailbox_alloc(newns->list, newname, 0);
	if (mailbox_rename(oldbox, newbox, TRUE) < 0) {
		struct mail_storage *storage = mailbox_get_storage(oldbox);

		i_error("Can't rename mailbox %s to %s: %s", oldname, newname,
			mail_storage_get_last_error(storage, NULL));
	}
	mailbox_free(&oldbox);
	mailbox_free(&newbox);
}

static void cmd_mailbox_rename_init(struct doveadm_mail_cmd_context *_ctx,
				    const char *const args[])
{
	struct rename_cmd_context *ctx = (struct rename_cmd_context *)_ctx;

	if (str_array_length(args) != 2)
		doveadm_mail_help_name("mailbox rename");
	doveadm_mailbox_translate_args(&ctx->ctx, &args);

	ctx->oldname = p_strdup(ctx->ctx.ctx.pool, args[0]);
	ctx->newname = p_strdup(ctx->ctx.ctx.pool, args[1]);
}

static struct doveadm_mail_cmd_context *cmd_mailbox_rename_alloc(void)
{
	struct rename_cmd_context *ctx;

	ctx = doveadm_mailbox_cmd_alloc(struct rename_cmd_context);
	ctx->ctx.ctx.init = cmd_mailbox_rename_init;
	ctx->ctx.ctx.run = cmd_mailbox_rename_run;
	return &ctx->ctx.ctx;
}

struct doveadm_mail_cmd cmd_mailbox_list = {
	cmd_mailbox_list_alloc, "mailbox list",
	"[-7|-8] [<mailbox> [...]]"
};
struct doveadm_mail_cmd cmd_mailbox_create = {
	cmd_mailbox_create_alloc, "mailbox create",
	"[-7|-8] <mailbox> [...]"
};
struct doveadm_mail_cmd cmd_mailbox_delete = {
	cmd_mailbox_delete_alloc, "mailbox delete",
	"[-7|-8] <mailbox> [...]"
};
struct doveadm_mail_cmd cmd_mailbox_rename = {
	cmd_mailbox_rename_alloc, "mailbox rename",
	"[-7|-8] <old name> <new name>"
};
