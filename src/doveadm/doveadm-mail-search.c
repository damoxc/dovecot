/* Copyright (c) 2010 Dovecot authors, see the included COPYING file */

#include "lib.h"
#include "mail-storage.h"
#include "doveadm-mail-list-iter.h"
#include "doveadm-mail-iter.h"
#include "doveadm-mail.h"

#include <stdio.h>

struct search_cmd_context {
	struct doveadm_mail_cmd_context ctx;
	struct mail_search_args *search_args;
};

static int
cmd_search_box(const struct mailbox_info *info,
	       struct mail_search_args *search_args)
{
	struct doveadm_mail_iter *iter;
	struct mailbox_transaction_context *trans;
	struct mail *mail;
	uint8_t guid[MAIL_GUID_128_SIZE];
	const char *guid_str;
	int ret = 0;

	if (doveadm_mail_iter_init(info, search_args, &trans, &iter) < 0)
		return -1;

	mail = mail_alloc(trans, 0, NULL);
	if (mailbox_get_guid(mail->box, guid) < 0)
		ret = -1;
	else {
		guid_str = mail_guid_128_to_string(guid);
		while (doveadm_mail_iter_next(iter, mail))
			printf("%s %u\n", guid_str, mail->uid);
	}
	mail_free(&mail);
	if (doveadm_mail_iter_deinit(&iter) < 0)
		ret = -1;
	return ret;
}

static void
cmd_search_run(struct doveadm_mail_cmd_context *_ctx, struct mail_user *user)
{
	struct search_cmd_context *ctx = (struct search_cmd_context *)_ctx;
	const enum mailbox_list_iter_flags iter_flags =
		MAILBOX_LIST_ITER_RAW_LIST |
		MAILBOX_LIST_ITER_VIRTUAL_NAMES |
		MAILBOX_LIST_ITER_NO_AUTO_INBOX |
		MAILBOX_LIST_ITER_RETURN_NO_FLAGS;
	struct doveadm_mail_list_iter *iter;
	const struct mailbox_info *info;

	iter = doveadm_mail_list_iter_init(user, ctx->search_args, iter_flags);
	while ((info = doveadm_mail_list_iter_next(iter)) != NULL) T_BEGIN {
		(void)cmd_search_box(info, ctx->search_args);
	} T_END;
	doveadm_mail_list_iter_deinit(&iter);
}

static void cmd_search_init(struct doveadm_mail_cmd_context *_ctx,
			    const char *const args[])
{
	struct search_cmd_context *ctx = (struct search_cmd_context *)_ctx;

	if (args[0] == NULL)
		doveadm_mail_help_name("search");

	ctx->search_args = doveadm_mail_build_search_args(args);
}

static struct doveadm_mail_cmd_context *cmd_search_alloc(void)
{
	struct search_cmd_context *ctx;

	ctx = doveadm_mail_cmd_alloc(struct search_cmd_context);
	ctx->ctx.init = cmd_search_init;
	ctx->ctx.run = cmd_search_run;
	return &ctx->ctx;
}

struct doveadm_mail_cmd cmd_search = {
	cmd_search_alloc, "search", "<search query>"
};
