/* Copyright (c) 2005-2009 Dovecot authors, see the included COPYING file */

#include "lib.h"
#include "ioloop.h"
#include "str.h"
#include "str-sanitize.h"
#include "var-expand.h"
#include "message-address.h"
#include "lda-settings.h"
#include "mail-storage.h"
#include "mail-namespace.h"
#include "mail-deliver.h"

deliver_mail_func_t *deliver_mail = NULL;

const char *mail_deliver_get_address(struct mail_deliver_context *ctx,
				     const char *header)
{
	struct message_address *addr;
	const char *str;

	if (mail_get_first_header(ctx->src_mail, header, &str) <= 0)
		return NULL;
	addr = message_address_parse(pool_datastack_create(),
				     (const unsigned char *)str,
				     strlen(str), 1, FALSE);
	return addr == NULL || addr->mailbox == NULL || addr->domain == NULL ||
		*addr->mailbox == '\0' || *addr->domain == '\0' ?
		NULL : t_strconcat(addr->mailbox, "@", addr->domain, NULL);
}

static const struct var_expand_table *
get_log_var_expand_table(struct mail_deliver_context *ctx, const char *message)
{
	static struct var_expand_table static_tab[] = {
		{ '$', NULL, NULL },
		{ 'm', NULL, "msgid" },
		{ 's', NULL, "subject" },
		{ 'f', NULL, "from" },
		{ '\0', NULL, NULL }
	};
	struct var_expand_table *tab;
	unsigned int i;

	tab = t_malloc(sizeof(static_tab));
	memcpy(tab, static_tab, sizeof(static_tab));

	tab[0].value = message;
	(void)mail_get_first_header(ctx->src_mail, "Message-ID", &tab[1].value);
	(void)mail_get_first_header_utf8(ctx->src_mail, "Subject", &tab[2].value);
	tab[3].value = mail_deliver_get_address(ctx, "From");
	for (i = 1; tab[i].key != '\0'; i++)
		tab[i].value = str_sanitize(tab[i].value, 80);
	return tab;
}

void mail_deliver_log(struct mail_deliver_context *ctx, const char *fmt, ...)
{
	va_list args;
	string_t *str;
	const char *msg;

	va_start(args, fmt);
	msg = t_strdup_vprintf(fmt, args);

	str = t_str_new(256);
	var_expand(str, ctx->set->deliver_log_format,
		   get_log_var_expand_table(ctx, msg));
	i_info("%s", str_c(str));
	va_end(args);
}

static struct mailbox *
mailbox_open_or_create_synced(struct mail_deliver_context *ctx, 
			      const char *name, struct mail_namespace **ns_r,
			      const char **error_r)
{
	struct mail_namespace *ns;
	struct mail_storage *storage;
	struct mailbox *box;
	enum mail_error error;
	enum mailbox_flags flags =
		MAILBOX_FLAG_KEEP_RECENT | MAILBOX_FLAG_SAVEONLY |
		MAILBOX_FLAG_POST_SESSION;

	*error_r = NULL;

	if (strcasecmp(name, "INBOX") == 0) {
		/* deliveries to INBOX must always succeed,
		   regardless of ACLs */
		flags |= MAILBOX_FLAG_IGNORE_ACLS;
	}

	*ns_r = ns = mail_namespace_find(ctx->dest_user->namespaces, &name);
	if (*ns_r == NULL)
		return NULL;

	if (*name == '\0') {
		/* delivering to a namespace prefix means we actually want to
		   deliver to the INBOX instead */
		return NULL;
	}

	box = mailbox_alloc(ns->list, name, NULL, flags);
	if (mailbox_open(box) == 0)
		return box;

	storage = mailbox_get_storage(box);
	*error_r = mail_storage_get_last_error(storage, &error);
	mailbox_close(&box);
	if (!ctx->set->lda_mailbox_autocreate || error != MAIL_ERROR_NOTFOUND)
		return NULL;

	/* try creating it. */
	if (mail_storage_mailbox_create(storage, ns, name, FALSE) < 0) {
		*error_r = mail_storage_get_last_error(storage, &error);
		return NULL;
	}
	if (ctx->set->lda_mailbox_autosubscribe) {
		/* (try to) subscribe to it */
		(void)mailbox_list_set_subscribed(ns->list, name, TRUE);
	}

	/* and try opening again */
	box = mailbox_alloc(ns->list, name, NULL, flags);
	storage = mailbox_get_storage(box);
	if (mailbox_open(box) < 0 ||
	    mailbox_sync(box, 0, 0, NULL) < 0) {
		*error_r = mail_storage_get_last_error(storage, &error);
		mailbox_close(&box);
		return NULL;
	}
	return box;
}

int mail_deliver_save(struct mail_deliver_context *ctx, const char *mailbox,
		      enum mail_flags flags, const char *const *keywords,
		      struct mail_storage **storage_r)
{
	struct mail_namespace *ns;
	struct mailbox *box;
	enum mailbox_transaction_flags trans_flags;
	struct mailbox_transaction_context *t;
	struct mail_save_context *save_ctx;
	struct mail_keywords *kw;
	enum mail_error error;
	const char *mailbox_name, *errstr;
	uint32_t uid_validity, uid1 = 0, uid2 = 0;
	bool default_save;
	int ret = 0;

	default_save = strcmp(mailbox, ctx->dest_mailbox_name) == 0;
	if (default_save)
		ctx->tried_default_save = TRUE;

	mailbox_name = str_sanitize(mailbox, 80);
	box = mailbox_open_or_create_synced(ctx, mailbox, &ns, &errstr);
	if (box == NULL) {
		if (ns == NULL) {
			mail_deliver_log(ctx,
					 "save failed to %s: Unknown namespace",
					 mailbox_name);
			return -1;
		}
		if (default_save && strcmp(ns->prefix, mailbox) == 0) {
			/* silently store to the INBOX instead */
			return -1;
		}
		mail_deliver_log(ctx, "save failed to %s: %s",
				 mailbox_name, errstr);
		return -1;
	}
	*storage_r = mailbox_get_storage(box);

	trans_flags = MAILBOX_TRANSACTION_FLAG_EXTERNAL;
	if (ctx->save_dest_mail)
		trans_flags |= MAILBOX_TRANSACTION_FLAG_ASSIGN_UIDS;
	t = mailbox_transaction_begin(box, trans_flags);

	kw = str_array_length(keywords) == 0 ? NULL :
		mailbox_keywords_create_valid(box, keywords);
	save_ctx = mailbox_save_alloc(t);
	mailbox_save_set_flags(save_ctx, flags, kw);
	if (mailbox_copy(&save_ctx, ctx->src_mail) < 0)
		ret = -1;
	mailbox_keywords_free(box, &kw);

	if (ret < 0)
		mailbox_transaction_rollback(&t);
	else {
		ret = mailbox_transaction_commit_get_uids(&t, &uid_validity,
							  &uid1, &uid2);
	}

	if (ret == 0) {
		ctx->saved_mail = TRUE;
		mail_deliver_log(ctx, "saved mail to %s", mailbox_name);

		if (ctx->save_dest_mail && mailbox_sync(box, 0, 0, NULL) == 0) {
			i_assert(uid1 == uid2);

			t = mailbox_transaction_begin(box, 0);
			ctx->dest_mail = mail_alloc(t, MAIL_FETCH_STREAM_BODY,
						    NULL);
			if (mail_set_uid(ctx->dest_mail, uid1) < 0) {
				mail_free(&ctx->dest_mail);
				mailbox_transaction_rollback(&t);
			}
		}
	} else {
		mail_deliver_log(ctx, "save failed to %s: %s", mailbox_name,
			mail_storage_get_last_error(*storage_r, &error));
	}

	if (ctx->dest_mail == NULL)
		mailbox_close(&box);
	return ret;
}

const char *mail_deliver_get_return_address(struct mail_deliver_context *ctx)
{
	if (ctx->src_envelope_sender != NULL)
		return ctx->src_envelope_sender;

	return mail_deliver_get_address(ctx, "Return-Path");
}

const char *mail_deliver_get_new_message_id(struct mail_deliver_context *ctx)
{
	static int count = 0;

	return t_strdup_printf("<dovecot-%s-%s-%d@%s>",
			       dec2str(ioloop_timeval.tv_sec),
			       dec2str(ioloop_timeval.tv_usec),
			       count++, ctx->set->hostname);
}

int mail_deliver(struct mail_deliver_context *ctx,
		 struct mail_storage **storage_r)
{
	int ret;

	*storage_r = NULL;
	if (deliver_mail == NULL)
		ret = -1;
	else {
		if (deliver_mail(ctx, storage_r) <= 0) {
			/* if message was saved, don't bounce it even though
			   the script failed later. */
			ret = ctx->saved_mail ? 0 : -1;
		} else {
			/* success. message may or may not have been saved. */
			ret = 0;
		}
	}

	if (ret < 0 && !ctx->tried_default_save) {
		/* plugins didn't handle this. save into the default mailbox. */
		ret = mail_deliver_save(ctx, ctx->dest_mailbox_name, 0, NULL,
					storage_r);
	}
	if (ret < 0 && strcasecmp(ctx->dest_mailbox_name, "INBOX") != 0) {
		/* still didn't work. try once more to save it
		   to INBOX. */
		ret = mail_deliver_save(ctx, "INBOX", 0, NULL, storage_r);
	}
	return ret;
}
