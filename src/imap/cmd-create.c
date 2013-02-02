* Copyright (c) 2002-2013 Dovecot authors, see the included COPYING file */

#include "imap-common.h"
#include "imap-resp-code.h"
#include "mail-namespace.h"
#include "imap-commands.h"

bool cmd_create(struct client_command_context *cmd)
{
	struct mail_namespace *ns;
	const char *mailbox, *orig_mailbox;
	struct mailbox *box;
	bool directory;
	size_t len;

	/* <mailbox> */
	if (!client_read_string_args(cmd, 1, &mailbox))
		return FALSE;

	orig_mailbox = mailbox;
	ns = client_find_namespace(cmd, &mailbox);
	if (ns == NULL)
		return TRUE;

	len = strlen(orig_mailbox);
	if (len == 0 || orig_mailbox[len-1] != mail_namespace_get_sep(ns))
		directory = FALSE;
	else {
		/* name ends with hierarchy separator - client is just
		   informing us that it wants to create children under this
		   mailbox. */
                directory = TRUE;

		/* drop separator from mailbox. it's already dropped when
		   WORKAROUND_TB_EXTRA_MAILBOX_SEP is enabled */
		if (len == strlen(mailbox))
			mailbox = t_strndup(mailbox, len-1);
	}

	box = mailbox_alloc(ns->list, mailbox, 0);
	if (mailbox_create(box, NULL, directory) < 0)
		client_send_storage_error(cmd, mailbox_get_storage(box));
	else
		client_send_tagline(cmd, "OK Create completed.");
	mailbox_free(&box);
	return TRUE;
}
