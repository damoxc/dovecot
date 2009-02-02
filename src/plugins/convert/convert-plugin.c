/* Copyright (c) 2006-2009 Dovecot authors, see the included COPYING file */

#include "lib.h"
#include "mail-namespace.h"
#include "convert-storage.h"
#include "convert-plugin.h"

#include <stdlib.h>

const char *convert_plugin_version = PACKAGE_VERSION;

static void (*convert_next_hook_mail_namespaces_created)
	(struct mail_namespace *namespaces);

static void convert_mail_storage(struct mail_namespace *namespaces,
				 const char *convert_mail)
{
	const char *str;
	struct convert_plugin_settings set;

	memset(&set, 0, sizeof(set));
	if (mail_user_get_home(namespaces->user, &str) <= 0)
		i_fatal("convert plugin: HOME unset");

	set.skip_broken_mailboxes =
		mail_user_plugin_getenv(namespaces->user,
					"convert_skip_broken_mailboxes") != NULL;
	set.skip_dotdirs =
		mail_user_plugin_getenv(namespaces->user,
					"convert_skip_dotdirs") != NULL;

	str = mail_user_plugin_getenv(namespaces->user,
				      "convert_alt_hierarchy_char");
	set.alt_hierarchy_char = str != NULL && *str != '\0' ? *str : '_';

	if (convert_storage(convert_mail, namespaces, &set) < 0)
		i_fatal("Mailbox conversion failed, exiting");
}

static void
convert_hook_mail_namespaces_created(struct mail_namespace *namespaces)
{
	const char *convert_mail;

	convert_mail = mail_user_plugin_getenv(namespaces->user,
					       "convert_mail");
	if (convert_mail != NULL)
		convert_mail_storage(namespaces, convert_mail);
	else if (namespaces->user->mail_debug)
		i_info("convert: No convert_mail setting - plugin disabled");

	if (convert_next_hook_mail_namespaces_created != NULL)
		convert_next_hook_mail_namespaces_created(namespaces);
}

void convert_plugin_init(void)
{
	convert_next_hook_mail_namespaces_created =
		hook_mail_namespaces_created;
	hook_mail_namespaces_created = convert_hook_mail_namespaces_created;
}

void convert_plugin_deinit(void)
{
	hook_mail_namespaces_created =
		convert_next_hook_mail_namespaces_created;
}
