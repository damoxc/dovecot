/* Copyright (c) 2002-2008 Dovecot authors, see the included COPYING file */

#include "common.h"
#include "str.h"
#include "imap-quote.h"
#include "imap-status.h"

int imap_status_parse_items(struct client_command_context *cmd,
			    const struct imap_arg *args,
			    enum mailbox_status_items *items_r)
{
	const char *item;
	enum mailbox_status_items items;

	items = 0;
	for (; args->type != IMAP_ARG_EOL; args++) {
		if (args->type != IMAP_ARG_ATOM) {
			/* list may contain only atoms */
			client_send_command_error(cmd,
				"Status list contains non-atoms.");
			return -1;
		}

		item = t_str_ucase(IMAP_ARG_STR(args));

		if (strcmp(item, "MESSAGES") == 0)
			items |= STATUS_MESSAGES;
		else if (strcmp(item, "RECENT") == 0)
			items |= STATUS_RECENT;
		else if (strcmp(item, "UIDNEXT") == 0)
			items |= STATUS_UIDNEXT;
		else if (strcmp(item, "UIDVALIDITY") == 0)
			items |= STATUS_UIDVALIDITY;
		else if (strcmp(item, "UNSEEN") == 0)
			items |= STATUS_UNSEEN;
		else if (strcmp(item, "HIGHESTMODSEQ") == 0)
			items |= STATUS_HIGHESTMODSEQ;
		else {
			client_send_tagline(cmd, t_strconcat(
				"BAD Invalid status item ", item, NULL));
			return -1;
		}
	}

	*items_r = items;
	return 0;
}

bool imap_status_get(struct client *client, struct mail_storage *storage,
		     const char *mailbox, enum mailbox_status_items items,
		     struct mailbox_status *status_r)
{
	struct mailbox *box;
	int ret;

	if (client->mailbox != NULL &&
	    mailbox_equals(client->mailbox, storage, mailbox)) {
		/* this mailbox is selected */
		mailbox_get_status(client->mailbox, items, status_r);
		return TRUE;
	}

	/* open the mailbox */
	box = mailbox_open(&storage, mailbox, NULL, MAILBOX_OPEN_FAST |
			   MAILBOX_OPEN_READONLY | MAILBOX_OPEN_KEEP_RECENT);
	if (box == NULL)
		return FALSE;

	if ((items & STATUS_HIGHESTMODSEQ) != 0)
		client_enable(client, MAILBOX_FEATURE_CONDSTORE);
	if (client->enabled_features != 0)
		mailbox_enable(box, client->enabled_features);

	ret = mailbox_sync(box, 0, items, status_r);
	mailbox_close(&box);
	return ret == 0;
}

void imap_status_send(struct client *client, const char *mailbox,
		      enum mailbox_status_items items,
		      const struct mailbox_status *status)
{
	string_t *str;

	str = t_str_new(128);
	str_append(str, "* STATUS ");
        imap_quote_append_string(str, mailbox, FALSE);
	str_append(str, " (");

	if (items & STATUS_MESSAGES)
		str_printfa(str, "MESSAGES %u ", status->messages);
	if (items & STATUS_RECENT)
		str_printfa(str, "RECENT %u ", status->recent);
	if (items & STATUS_UIDNEXT)
		str_printfa(str, "UIDNEXT %u ", status->uidnext);
	if (items & STATUS_UIDVALIDITY)
		str_printfa(str, "UIDVALIDITY %u ", status->uidvalidity);
	if (items & STATUS_UNSEEN)
		str_printfa(str, "UNSEEN %u ", status->unseen);
	if (items & STATUS_HIGHESTMODSEQ) {
		str_printfa(str, "HIGHESTMODSEQ %llu ",
			    (unsigned long long)status->highest_modseq);
	}

	if (items != 0)
		str_truncate(str, str_len(str)-1);
	str_append_c(str, ')');

	client_send_line(client, str_c(str));
}
