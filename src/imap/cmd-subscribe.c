/* Copyright (c) 2002-2008 Dovecot authors, see the included COPYING file */

#include "common.h"
#include "commands.h"
#include "mail-namespace.h"

static bool have_listable_namespace_prefix(struct mail_namespace *ns,
					   const char *name)
{
	unsigned int name_len = strlen(name);

	for (; ns != NULL; ns = ns->next) {
		if ((ns->flags & NAMESPACE_FLAG_LIST) == 0)
			continue;

		if (ns->prefix_len <= name_len)
			continue;

		/* if prefix has multiple hierarchies, allow subscribing to
		   any of the hierarchies */
		if (strncmp(ns->prefix, name, name_len) == 0 &&
		    ns->prefix[name_len] == ns->sep)
			return TRUE;
	}
	return FALSE;
}

bool cmd_subscribe_full(struct client_command_context *cmd, bool subscribe)
{
	struct mail_namespace *ns, *real_ns;
	struct mail_storage *storage;
	const char *mailbox, *verify_name, *real_name;

	/* <mailbox> */
	if (!client_read_string_args(cmd, 1, &mailbox))
		return FALSE;
	verify_name = mailbox;

	real_name = mailbox;
	storage = client_find_storage(cmd, &real_name);
	if (storage == NULL)
		return TRUE;

	/* now find a namespace where the subscription can be added to */
	ns = mail_namespace_find_subscribable(cmd->client->user->namespaces,
					      &mailbox);
	if (ns == NULL) {
		client_send_tagline(cmd, "NO Unknown subscription namespace.");
		return TRUE;
	}

	real_ns = mail_storage_get_namespace(storage);
	if (ns != real_ns) {
		/* subscription is being written to a different namespace
		   than where the mailbox exists. */
		mailbox = t_strconcat(real_ns->prefix, real_name, NULL);
		/* drop the common prefix */
		i_assert(strncmp(ns->prefix, mailbox, strlen(ns->prefix)) == 0);
		mailbox += strlen(ns->prefix);
	}

	if ((client_workarounds & WORKAROUND_TB_EXTRA_MAILBOX_SEP) != 0 &&
	    *mailbox != '\0' && mailbox[strlen(mailbox)-1] ==
	    mail_storage_get_hierarchy_sep(ns->storage)) {
		/* verify the validity without the trailing '/' */
		verify_name = t_strndup(verify_name, strlen(verify_name)-1);
	}

	if (have_listable_namespace_prefix(cmd->client->user->namespaces,
					   verify_name)) {
		/* subscribing to a listable namespace prefix, allow it. */
	} else {
		if (!client_verify_mailbox_name(cmd, verify_name,
						subscribe, FALSE))
			return TRUE;
	}

	if (mailbox_list_set_subscribed(ns->list, mailbox, subscribe) < 0)
		client_send_list_error(cmd, ns->list);
	else {
		client_send_tagline(cmd, subscribe ?
				    "OK Subscribe completed." :
				    "OK Unsubscribe completed.");
	}
	return TRUE;
}

bool cmd_subscribe(struct client_command_context *cmd)
{
	return cmd_subscribe_full(cmd, TRUE);
}
