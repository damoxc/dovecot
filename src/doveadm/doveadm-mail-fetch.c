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
#include "mail-storage.h"
#include "mail-search.h"
#include "doveadm-mail.h"
#include "doveadm-mail-list-iter.h"
#include "doveadm-mail-iter.h"

#include <stdio.h>

struct fetch_cmd_context {
	struct doveadm_mail_cmd_context ctx;

	struct mail_search_args *search_args;
	struct ostream *output;
	struct mail *mail;

	ARRAY_DEFINE(fields, const struct fetch_field);
	enum mail_fetch_field wanted_fields;

	string_t *hdr;
	const char *prefix;

	bool print_field_prefix;
};

static int fetch_mailbox(struct fetch_cmd_context *ctx)
{
	const char *value;

	if (mail_get_special(ctx->mail, MAIL_FETCH_MAILBOX_NAME, &value) < 0)
		return -1;
	str_append(ctx->hdr, value);
	return 0;
}

static int fetch_mailbox_guid(struct fetch_cmd_context *ctx)
{
	uint8_t guid[MAIL_GUID_128_SIZE];

	if (mailbox_get_guid(ctx->mail->box, guid) < 0)
		return -1;
	str_append(ctx->hdr, mail_guid_128_to_string(guid));
	return 0;
}

static int fetch_seq(struct fetch_cmd_context *ctx)
{
	str_printfa(ctx->hdr, "%u", ctx->mail->seq);
	return 0;
}

static int fetch_uid(struct fetch_cmd_context *ctx)
{
	str_printfa(ctx->hdr, "%u", ctx->mail->seq);
	return 0;
}

static int fetch_guid(struct fetch_cmd_context *ctx)
{
	const char *value;

	if (mail_get_special(ctx->mail, MAIL_FETCH_GUID, &value) < 0)
		return -1;
	str_append(ctx->hdr, value);
	return 0;
}

static int fetch_flags(struct fetch_cmd_context *ctx)
{
	imap_write_flags(ctx->hdr, mail_get_flags(ctx->mail),
			 mail_get_keywords(ctx->mail));
	return 0;
}

static void flush_hdr(struct fetch_cmd_context *ctx)
{
	o_stream_send(ctx->output, str_data(ctx->hdr), str_len(ctx->hdr));
	str_truncate(ctx->hdr, 0);
}

static int fetch_hdr(struct fetch_cmd_context *ctx)
{
	struct istream *input;
	struct message_size hdr_size;
	int ret = 0;

	if (mail_get_stream(ctx->mail, &hdr_size, NULL, &input) < 0)
		return -1;

	if (ctx->print_field_prefix)
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

static int fetch_body(struct fetch_cmd_context *ctx)
{
	struct istream *input;
	struct message_size hdr_size;
	int ret = 0;

	if (mail_get_stream(ctx->mail, &hdr_size, NULL, &input) < 0)
		return -1;

	if (ctx->print_field_prefix)
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

static int fetch_text(struct fetch_cmd_context *ctx)
{
	struct istream *input;
	int ret = 0;

	if (mail_get_stream(ctx->mail, NULL, NULL, &input) < 0)
		return -1;

	if (ctx->print_field_prefix)
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

static int fetch_size_physical(struct fetch_cmd_context *ctx)
{
	uoff_t size;

	if (mail_get_physical_size(ctx->mail, &size) < 0)
		return -1;
	str_printfa(ctx->hdr, "%"PRIuUOFF_T, size);
	return 0;
}

static int fetch_size_virtual(struct fetch_cmd_context *ctx)
{
	uoff_t size;

	if (mail_get_virtual_size(ctx->mail, &size) < 0)
		return -1;
	str_printfa(ctx->hdr, "%"PRIuUOFF_T, size);
	return 0;
}

static int fetch_date_received(struct fetch_cmd_context *ctx)
{
	time_t t;

	if (mail_get_received_date(ctx->mail, &t) < 0)
		return -1;
	str_printfa(ctx->hdr, "%s", unixdate2str(t));
	return 0;
}

static int fetch_date_sent(struct fetch_cmd_context *ctx)
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

static int fetch_date_saved(struct fetch_cmd_context *ctx)
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
	int (*print)(struct fetch_cmd_context *ctx);
};

static const struct fetch_field fetch_fields[] = {
	{ "mailbox",       0,                        fetch_mailbox },
	{ "mailbox-guid",  0,                        fetch_mailbox_guid },
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

static void parse_fetch_fields(struct fetch_cmd_context *ctx, const char *str)
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
	ctx->print_field_prefix = array_count(&ctx->fields) > 1;
}

static void cmd_fetch_mail(struct fetch_cmd_context *ctx)
{
	const struct fetch_field *field;
	struct mail *mail = ctx->mail;

	array_foreach(&ctx->fields, field) {
		if (ctx->print_field_prefix)
			str_printfa(ctx->hdr, "%s: ", field->name);
		if (field->print(ctx) < 0) {
			struct mail_storage *storage =
				mailbox_get_storage(mail->box);

			i_error("fetch(%s) failed for box=%s uid=%u: %s",
				field->name, mailbox_get_vname(mail->box),
				mail->uid, mail_storage_get_last_error(storage, NULL));
		}
		str_append_c(ctx->hdr, '\n');
	}
	flush_hdr(ctx);
}

static int
cmd_fetch_box(struct fetch_cmd_context *ctx, const struct mailbox_info *info)
{
	struct doveadm_mail_iter *iter;
	struct mailbox_transaction_context *trans;
	struct mail *mail;

	if (doveadm_mail_iter_init(info, ctx->search_args, &trans, &iter) < 0)
		return -1;

	mail = mail_alloc(trans, ctx->wanted_fields, NULL);
	while (doveadm_mail_iter_next(iter, mail)) {
		str_truncate(ctx->hdr, 0);
		str_append(ctx->hdr, ctx->prefix);

		ctx->mail = mail;
		cmd_fetch_mail(ctx);
		ctx->mail = NULL;
	}
	mail_free(&mail);
	return doveadm_mail_iter_deinit(&iter);
}

static bool search_args_have_unique_fetch(struct mail_search_args *args)
{
	struct mail_search_arg *arg;
	const struct seq_range *seqset;
	unsigned int count;
	bool have_mailbox = FALSE, have_msg = FALSE;

	for (arg = args->args; arg != NULL; arg = arg->next) {
		switch (arg->type) {
		case SEARCH_MAILBOX:
		case SEARCH_MAILBOX_GUID:
			if (!arg->not)
				have_mailbox = TRUE;
			break;
		case SEARCH_SEQSET:
		case SEARCH_UIDSET:
			seqset = array_get(&arg->value.seqset, &count);
			if (count == 1 && seqset->seq1 == seqset->seq2 &&
			    !arg->not)
				have_msg = TRUE;
			break;
		default:
			break;
		}
	}
	return have_mailbox && have_msg;
}

static void
cmd_fetch_run(struct doveadm_mail_cmd_context *_ctx, struct mail_user *user)
{
	struct fetch_cmd_context *ctx = (struct fetch_cmd_context *)_ctx;
	const enum mailbox_list_iter_flags iter_flags =
		MAILBOX_LIST_ITER_VIRTUAL_NAMES |
		MAILBOX_LIST_ITER_NO_AUTO_INBOX |
		MAILBOX_LIST_ITER_RETURN_NO_FLAGS;
	struct doveadm_mail_list_iter *iter;
	const struct mailbox_info *info;

	iter = doveadm_mail_list_iter_init(user, ctx->search_args, iter_flags);
	while ((info = doveadm_mail_list_iter_next(iter)) != NULL) T_BEGIN {
		(void)cmd_fetch_box(ctx, info);
	} T_END;
	doveadm_mail_list_iter_deinit(&iter);
}

static void cmd_fetch_deinit(struct doveadm_mail_cmd_context *_ctx)
{
	struct fetch_cmd_context *ctx = (struct fetch_cmd_context *)_ctx;

	o_stream_unref(&ctx->output);
	str_free(&ctx->hdr);
}

struct doveadm_mail_cmd_context *cmd_fetch(const char *const args[])
{
	const char *fetch_fields = args[0];
	struct fetch_cmd_context *ctx;
	unsigned char prefix_buf[9];

	if (fetch_fields == NULL || args[1] == NULL)
		doveadm_mail_help_name("fetch");

	ctx = doveadm_mail_cmd_init(struct fetch_cmd_context);
	ctx->ctx.run = cmd_fetch_run;
	ctx->ctx.deinit = cmd_fetch_deinit;

	parse_fetch_fields(ctx, fetch_fields);
	ctx->search_args = doveadm_mail_build_search_args(args + 1);

	ctx->output = o_stream_create_fd(STDOUT_FILENO, 0, FALSE);
	ctx->hdr = str_new(default_pool, 512);
	if (search_args_have_unique_fetch(ctx->search_args))
		ctx->prefix = "";
	else {
		random_fill_weak(prefix_buf, sizeof(prefix_buf));
		str_append(ctx->hdr, "===");
		base64_encode(prefix_buf, sizeof(prefix_buf), ctx->hdr);
		str_append_c(ctx->hdr, '\n');
		ctx->prefix = t_strdup(str_c(ctx->hdr));
		str_truncate(ctx->hdr, 0);
	}
	return &ctx->ctx;
}