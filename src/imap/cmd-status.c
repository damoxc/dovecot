/* Copyright (c) 2002-2011 Dovecot authors, see the included COPYING file */

#include "imap-common.h"
#include "imap-resp-code.h"
#include "imap-commands.h"
#include "imap-sync.h"
#include "imap-status.h"

bool cmd_status(struct client_command_context *cmd)
{
	struct client *client = cmd->client;
	enum mailbox_name_status status;
	const struct imap_arg *args, *list_args;
	struct imap_status_items items;
	struct imap_status_result result;
	struct mail_namespace *ns;
	const char *mailbox, *storage_name, *error;
	bool selected_mailbox;

	/* <mailbox> <status items> */
	if (!client_read_args(cmd, 2, 0, &args))
		return FALSE;

	if (!imap_arg_get_astring(&args[0], &mailbox) ||
	    !imap_arg_get_list(&args[1], &list_args)) {
		client_send_command_error(cmd, "Invalid arguments.");
		return TRUE;
	}

	/* get the items client wants */
	if (imap_status_parse_items(cmd, list_args, &items) < 0)
		return TRUE;

	ns = client_find_namespace(cmd, mailbox, &storage_name, &status);
	if (ns == NULL)
		return TRUE;
	switch (status) {
	case MAILBOX_NAME_EXISTS_MAILBOX:
		break;
	case MAILBOX_NAME_EXISTS_DIR:
		status = MAILBOX_NAME_VALID;
		/* fall through */
	case MAILBOX_NAME_VALID:
	case MAILBOX_NAME_INVALID:
	case MAILBOX_NAME_NOINFERIORS:
		client_fail_mailbox_name_status(cmd, mailbox, NULL, status);
		return TRUE;
	}

	selected_mailbox = client->mailbox != NULL &&
		mailbox_equals(client->mailbox, ns, storage_name);
	if (imap_status_get(cmd, ns, storage_name, &items,
			    &result, &error) < 0) {
		client_send_tagline(cmd, error);
		return TRUE;
	}

	imap_status_send(client, mailbox, &items, &result);
	if (!selected_mailbox)
		client_send_tagline(cmd, "OK Status completed.");
	else {
		client_send_tagline(cmd, "OK ["IMAP_RESP_CODE_CLIENTBUG"] "
				    "Status on selected mailbox completed.");
	}
	return TRUE;
}
