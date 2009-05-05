/* Copyright (c) 2002-2009 Dovecot authors, see the included COPYING file */

#include "imap-common.h"
#include "imap-commands.h"
#include "imap-expunge.h"

bool cmd_close(struct client_command_context *cmd)
{
	struct client *client = cmd->client;
	struct mailbox *mailbox = client->mailbox;
	struct mail_storage *storage;
	int ret;

	if (!client_verify_open_mailbox(cmd))
		return TRUE;

	i_assert(client->mailbox_change_lock == NULL);
	client->mailbox = NULL;

	storage = mailbox_get_storage(mailbox);
	if ((ret = imap_expunge(mailbox, NULL)) < 0)
		client_send_untagged_storage_error(client, storage);
	if (mailbox_sync(mailbox, 0, 0, NULL) < 0)
		client_send_untagged_storage_error(client, storage);

	if (mailbox_close(&mailbox) < 0)
		client_send_untagged_storage_error(client, storage);
	client_update_mailbox_flags(client, NULL);

	client_send_tagline(cmd, "OK Close completed.");
	return TRUE;
}
