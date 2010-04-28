/* Copyright (c) 2010 Dovecot authors, see the included COPYING file */

#include "lib.h"
#include "array.h"
#include "istream.h"
#include "ostream.h"
#include "base64.h"
#include "randgen.h"
#include "str.h"
#include "message-size.h"
#include "imap-util.h"
#include "mail-namespace.h"
#include "mail-storage.h"
#include "mail-search.h"
#include "doveadm-mail.h"
#include "doveadm-mail-list-iter.h"

#include <stdio.h>

struct fetch_context {
	struct mail_search_args *search_args;
	struct ostream *output;
	struct mail *mail;

	ARRAY_DEFINE(fields, const struct fetch_field);
	enum mail_fetch_field wanted_fields;

	string_t *hdr;
	const char *prefix;
};

static int fetch_mailbox(struct fetch_context *ctx)
{
	const char *value;

	if (mail_get_special(ctx->mail, MAIL_FETCH_MAILBOX_NAME, &value) < 0)
		return -1;
	str_append(ctx->hdr, value);
	return 0;
}

static int fetch_seq(struct fetch_context *ctx)
{
	str_printfa(ctx->hdr, "%u", ctx->mail->seq);
	return 0;
}

static int fetch_uid(struct fetch_context *ctx)
{
	str_printfa(ctx->hdr, "%u", ctx->mail->seq);
	return 0;
}

static int fetch_guid(struct fetch_context *ctx)
{
	const char *value;

	if (mail_get_special(ctx->mail, MAIL_FETCH_GUID, &value) < 0)
		return -1;
	str_append(ctx->hdr, value);
	return 0;
}

static int fetch_flags(struct fetch_context *ctx)
{
	imap_write_flags(ctx->hdr, mail_get_flags(ctx->mail),
			 mail_get_keywords(ctx->mail));
	return 0;
}

static void flush_hdr(struct fetch_context *ctx)
{
	o_stream_send(ctx->output, str_data(ctx->hdr), str_len(ctx->hdr));
	str_truncate(ctx->hdr, 0);
}

static int fetch_hdr(struct fetch_context *ctx)
{
	struct istream *input;
	struct message_size hdr_size;
	int ret = 0;

	if (mail_get_stream(ctx->mail, &hdr_size, NULL, &input) < 0)
		return -1;

	str_append_c(ctx->hdr, '\n');
	flush_hdr(ctx);
	input = i_stream_create_limit(input, hdr_size.physical_size);
	while (!i_stream_is_eof(input)) {
		if (o_stream_send_istream(ctx->output, input) <= 0)
			i_fatal("write(stdout) failed: %m");
	}
	if (input->stream_errno != 0) {
		i_error("read() failed: %m");
		ret = -1;
	}
	i_stream_unref(&input);
	o_stream_flush(ctx->output);
	return ret;
}

static int fetch_body(struct fetch_context *ctx)
{
	struct istream *input;
	struct message_size hdr_size;
	int ret = 0;

	if (mail_get_stream(ctx->mail, &hdr_size, NULL, &input) < 0)
		return -1;

	str_append_c(ctx->hdr, '\n');
	flush_hdr(ctx);
	i_stream_skip(input, hdr_size.physical_size);
	while (!i_stream_is_eof(input)) {
		if (o_stream_send_istream(ctx->output, input) <= 0)
			i_fatal("write(stdout) failed: %m");
	}
	if (input->stream_errno != 0) {
		i_error("read() failed: %m");
		ret = -1;
	}
	o_stream_flush(ctx->output);
	return ret;
}

static int fetch_text(struct fetch_context *ctx)
{
	struct istream *input;
	int ret = 0;

	if (mail_get_stream(ctx->mail, NULL, NULL, &input) < 0)
		return -1;

	str_append_c(ctx->hdr, '\n');
	flush_hdr(ctx);
	while (!i_stream_is_eof(input)) {
		if (o_stream_send_istream(ctx->output, input) <= 0)
			i_fatal("write(stdout) failed: %m");
	}
	if (input->stream_errno != 0) {
		i_error("read() failed: %m");
		ret = -1;
	}
	o_stream_flush(ctx->output);
	return ret;
}

static int fetch_size_physical(struct fetch_context *ctx)
{
	uoff_t size;

	if (mail_get_physical_size(ctx->mail, &size) < 0)
		return -1;
	str_printfa(ctx->hdr, "%"PRIuUOFF_T, size);
	return 0;
}

static int fetch_size_virtual(struct fetch_context *ctx)
{
	uoff_t size;

	if (mail_get_virtual_size(ctx->mail, &size) < 0)
		return -1;
	str_printfa(ctx->hdr, "%"PRIuUOFF_T, size);
	return 0;
}

static int fetch_date_received(struct fetch_context *ctx)
{
	time_t t;

	if (mail_get_received_date(ctx->mail, &t) < 0)
		return -1;
	str_printfa(ctx->hdr, "%s", unixdate2str(t));
	return 0;
}

static int fetch_date_sent(struct fetch_context *ctx)
{
	time_t t;
	int tz;
	char chr;

	if (mail_get_date(ctx->mail, &t, &tz) < 0)
		return -1;

	chr = tz < 0 ? '-' : '+';
	if (tz < 0) tz = -tz;
	str_printfa(ctx->hdr, "%s (%c%02u%02u)", unixdate2str(t),
		    chr, tz/60, tz%60);
	return 0;
}

static int fetch_date_saved(struct fetch_context *ctx)
{
	time_t t;

	if (mail_get_save_date(ctx->mail, &t) < 0)
		return -1;
	str_printfa(ctx->hdr, "%s", unixdate2str(t));
	return 0;
}

struct fetch_field {
	const char *name;
	enum mail_fetch_field wanted_fields;
	int (*print)(struct fetch_context *ctx);
};

static const struct fetch_field fetch_fields[] = {
	{ "mailbox",       0,                        fetch_mailbox },
	{ "seq",           0,                        fetch_seq },
	{ "uid",           0,                        fetch_uid },
	{ "guid",          0,                        fetch_guid },
	{ "flags",         MAIL_FETCH_FLAGS,         fetch_flags },
	{ "hdr",           MAIL_FETCH_STREAM_HEADER, fetch_hdr },
	{ "body",          MAIL_FETCH_STREAM_BODY,   fetch_body },
	{ "text",          MAIL_FETCH_STREAM_HEADER |
	                   MAIL_FETCH_STREAM_BODY,   fetch_text },
	{ "size.physical", MAIL_FETCH_PHYSICAL_SIZE, fetch_size_physical },
	{ "size.virtual",  MAIL_FETCH_VIRTUAL_SIZE,  fetch_size_virtual },
	{ "date.received", MAIL_FETCH_RECEIVED_DATE, fetch_date_received },
	{ "date.sent",     MAIL_FETCH_DATE,          fetch_date_sent },
	{ "date.saved",    MAIL_FETCH_SAVE_DATE,     fetch_date_saved }
};

static const struct fetch_field *fetch_field_find(const char *name)
{
	unsigned int i;

	for (i = 0; i < N_ELEMENTS(fetch_fields); i++) {
		if (strcmp(fetch_fields[i].name, name) == 0)
			return &fetch_fields[i];
	}
	return NULL;
}

static void print_fetch_fields(void)
{
	unsigned int i;

	fprintf(stderr, "Available fetch fields: %s", fetch_fields[0].name);
	for (i = 1; i < N_ELEMENTS(fetch_fields); i++)
		fprintf(stderr, " %s", fetch_fields[i].name);
	fprintf(stderr, "\n");
}

static void parse_fetch_fields(struct fetch_context *ctx, const char *str)
{
	const char *const *fields, *name;
	const struct fetch_field *field;

	t_array_init(&ctx->fields, 32);
	fields = t_strsplit_spaces(str, " ");
	for (; *fields != NULL; fields++) {
		name = t_str_lcase(*fields);

		field = fetch_field_find(name);
		if (field == NULL) {
			print_fetch_fields();
			i_fatal("Unknown fetch field: %s", name);
		}
		ctx->wanted_fields |= field->wanted_fields;

		array_append(&ctx->fields, field, 1);
	}
}

static void
cmd_fetch_box(struct fetch_context *ctx, struct mailbox *box)
{
	struct mail_storage *storage = mailbox_get_storage(box);
	struct mailbox_transaction_context *t;
	struct mail_search_context *search_ctx;
	struct mail *mail;
	const struct fetch_field *field;

	if (mailbox_sync(box, MAILBOX_SYNC_FLAG_FULL_READ) < 0) {
		i_error("Syncing mailbox %s failed: %s", mailbox_get_vname(box),
			mail_storage_get_last_error(storage, NULL));
		return;
	}

	mail_search_args_init(ctx->search_args, box, FALSE, NULL);
	t = mailbox_transaction_begin(box, 0);
	search_ctx = mailbox_search_init(t, ctx->search_args, NULL);
	mail = mail_alloc(t, ctx->wanted_fields, NULL);
	while (mailbox_search_next(search_ctx, mail)) {
		str_truncate(ctx->hdr, 0);
		str_append(ctx->hdr, ctx->prefix);

		ctx->mail = mail;
		array_foreach(&ctx->fields, field) {
			str_printfa(ctx->hdr, "%s: ", field->name);
			if (field->print(ctx) < 0) {
				i_error("fetch(%s) failed for box=%s uid=%u: %s",
					field->name, mailbox_get_vname(box),
					mail->uid, mail_storage_get_last_error(storage, NULL));
			}
			str_append_c(ctx->hdr, '\n');
		}
		flush_hdr(ctx);

		ctx->mail = NULL;
	}
	mail_free(&mail);
	if (mailbox_search_deinit(&search_ctx) < 0) {
		i_error("Search failed: %s",
			mail_storage_get_last_error(storage, NULL));
	}
	mail_search_args_deinit(ctx->search_args);
	(void)mailbox_transaction_commit(&t);
}

void cmd_fetch(struct mail_user *user, const char *const args[])
{
	const enum mailbox_list_iter_flags iter_flags =
		MAILBOX_LIST_ITER_VIRTUAL_NAMES |
		MAILBOX_LIST_ITER_NO_AUTO_INBOX |
		MAILBOX_LIST_ITER_RETURN_NO_FLAGS;
	const char *fetch_fields = args[0];
	struct fetch_context ctx;
	struct doveadm_mail_list_iter *iter;
	const struct mailbox_info *info;
	struct mailbox *box;
	const char *storage_name;
	unsigned char prefix_buf[9];

	memset(&ctx, 0, sizeof(ctx));
	if (fetch_fields == NULL || args[1] == NULL)
		doveadm_mail_help_name("fetch");
	parse_fetch_fields(&ctx, fetch_fields);
	ctx.search_args = doveadm_mail_build_search_args(args + 1);

	ctx.output = o_stream_create_fd(STDOUT_FILENO, 0, FALSE);

	random_fill_weak(prefix_buf, sizeof(prefix_buf));
	ctx.hdr = str_new(default_pool, 512);
	str_append(ctx.hdr, "===");
	base64_encode(prefix_buf, sizeof(prefix_buf), ctx.hdr);
	str_append_c(ctx.hdr, '\n');
	ctx.prefix = t_strdup(str_c(ctx.hdr));
	str_truncate(ctx.hdr, 0);

	iter = doveadm_mail_list_iter_init(user, ctx.search_args, iter_flags);
	while ((info = doveadm_mail_list_iter_next(iter)) != NULL) T_BEGIN {
		storage_name = mail_namespace_get_storage_name(info->ns,
							       info->name);
		box = mailbox_alloc(info->ns->list, storage_name,
				    MAILBOX_FLAG_KEEP_RECENT |
				    MAILBOX_FLAG_IGNORE_ACLS);
		(void)cmd_fetch_box(&ctx, box);
		mailbox_free(&box);
	} T_END;
	doveadm_mail_list_iter_deinit(&iter);
	o_stream_unref(&ctx.output);
	str_free(&ctx.hdr);
}
