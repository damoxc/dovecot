/* Copyright (c) 2008 Dovecot authors, see the included COPYING file */

#include "common.h"
#include "str.h"
#include "imap-quote.h"
#include "imap-resp-code.h"
#include "commands.h"
#include "mail-storage.h"
#include "mail-namespace.h"
#include "acl-api.h"
#include "acl-storage.h"
#include "imap-acl-plugin.h"

#include <stdlib.h>

#define ERROR_NOT_ADMIN "["IMAP_RESP_CODE_ACL"] " \
	"You lack administrator privileges on this mailbox."

#define ACL_MAILBOX_OPEN_FLAGS \
	(MAILBOX_OPEN_READONLY | MAILBOX_OPEN_FAST | MAILBOX_OPEN_KEEP_RECENT)

#define IMAP_ACL_ANYONE "anyone"
#define IMAP_ACL_AUTHENTICATED "authenticated"
#define IMAP_ACL_OWNER "owner"
#define IMAP_ACL_GROUP_PREFIX "$"
#define IMAP_ACL_GROUP_OVERRIDE_PREFIX "!$"
#define IMAP_ACL_GLOBAL_PREFIX "#"

struct imap_acl_letter_map {
	char letter;
	const char *name;
};

static const struct imap_acl_letter_map imap_acl_letter_map[] = {
	{ 'l', MAIL_ACL_LOOKUP },
	{ 'r', MAIL_ACL_READ },
	{ 'w', MAIL_ACL_WRITE },
	{ 's', MAIL_ACL_WRITE_SEEN },
	{ 't', MAIL_ACL_WRITE_DELETED },
	{ 'i', MAIL_ACL_INSERT },
	{ 'p', MAIL_ACL_POST },
	{ 'e', MAIL_ACL_EXPUNGE },
	{ 'k', MAIL_ACL_CREATE },
	{ 'x', MAIL_ACL_DELETE },
	{ 'a', MAIL_ACL_ADMIN },
	{ '\0', NULL }
};

static bool acl_anyone_allow = FALSE;

static struct mailbox *
acl_mailbox_open_as_admin(struct client_command_context *cmd, const char *name)
{
	struct mail_storage *storage;
	struct mailbox *box;
	int ret;

	storage = client_find_storage(cmd, &name);
	if (storage == NULL)
		return NULL;

	/* Force opening the mailbox so that we can give a nicer error message
	   if mailbox isn't selectable but is listable. */
	box = mailbox_open(&storage, name, NULL, ACL_MAILBOX_OPEN_FLAGS |
			   MAILBOX_OPEN_IGNORE_ACLS);
	if (box == NULL) {
		client_send_storage_error(cmd, storage);
		return NULL;
	}

	ret = acl_mailbox_right_lookup(box, ACL_STORAGE_RIGHT_ADMIN);
	if (ret > 0)
		return box;

	/* not an administrator. */
	if (acl_mailbox_right_lookup(box, ACL_STORAGE_RIGHT_LOOKUP) <= 0) {
		client_send_tagline(cmd, t_strdup_printf(
			"NO ["IMAP_RESP_CODE_NONEXISTENT"] "
			MAIL_ERRSTR_MAILBOX_NOT_FOUND, name));
	} else {
		client_send_tagline(cmd, "NO "ERROR_NOT_ADMIN);
	}
	mailbox_close(&box);
	return NULL;
}

static const struct imap_acl_letter_map *
imap_acl_letter_map_find(const char *name)
{
	unsigned int i;

	for (i = 0; imap_acl_letter_map[i].name != NULL; i++) {
		if (strcmp(imap_acl_letter_map[i].name, name) == 0)
			return &imap_acl_letter_map[i];
	}
	return NULL;
}

static void
imap_acl_write_rights_list(string_t *dest, const char *const *rights)
{
	const struct imap_acl_letter_map *map;
	unsigned int i;
	bool append_c = FALSE, append_d = FALSE;

	for (i = 0; rights[i] != NULL; i++) {
		/* write only letters */
		map = imap_acl_letter_map_find(rights[i]);
		if (map != NULL) {
			str_append_c(dest, map->letter);
			if (map->letter == 'k' || map->letter == 'x')
				append_c = TRUE;
			if (map->letter == 't' || map->letter == 'e')
				append_d = TRUE;
		}
	}
	if (append_c)
		str_append_c(dest, 'c');
	if (append_d)
		str_append_c(dest, 'd');
}

static void
imap_acl_write_right(string_t *dest, string_t *tmp,
		     const struct acl_rights *right, bool neg)
{
	const char *const *rights = neg ? right->neg_rights : right->rights;

	if (neg) str_append_c(dest,'-');

	str_truncate(tmp, 0);
	if (right->global)
		str_append(tmp, IMAP_ACL_GLOBAL_PREFIX);
	switch (right->id_type) {
	case ACL_ID_ANYONE:
		str_append(tmp, IMAP_ACL_ANYONE);
		break;
	case ACL_ID_AUTHENTICATED:
		str_append(tmp, IMAP_ACL_AUTHENTICATED);
		break;
	case ACL_ID_OWNER:
		str_append(tmp, IMAP_ACL_OWNER);
		break;
	case ACL_ID_USER:
		str_append(tmp, right->identifier);
		break;
	case ACL_ID_GROUP:
		str_append(tmp, IMAP_ACL_GROUP_PREFIX);
		str_append(tmp, right->identifier);
		break;
	case ACL_ID_GROUP_OVERRIDE:
		str_append(tmp, IMAP_ACL_GROUP_OVERRIDE_PREFIX);
		str_append(tmp, right->identifier);
		break;
	case ACL_ID_TYPE_COUNT:
		i_unreached();
	}

	imap_quote_append(dest, str_data(tmp), str_len(tmp), FALSE);
	str_append_c(dest, ' ');
	imap_acl_write_rights_list(dest, rights);
}

static int imap_acl_write_aclobj(string_t *dest, struct acl_object *aclobj)
{
	struct acl_object_list_iter *iter;
	struct acl_rights rights;
	string_t *tmp;
	int ret;

	tmp = t_str_new(128);
	iter = acl_object_list_init(aclobj);
	while ((ret = acl_object_list_next(iter, &rights)) > 0) {
		str_append_c(dest, ' ');
		if (rights.rights != NULL)
			imap_acl_write_right(dest, tmp, &rights, FALSE);
		if (rights.neg_rights != NULL)
			imap_acl_write_right(dest, tmp, &rights, TRUE);
	}
	acl_object_list_deinit(&iter);
	return ret;
}

static bool cmd_getacl(struct client_command_context *cmd)
{
	struct mailbox *box;
	const char *mailbox;
	string_t *str;
	unsigned int len;
	int ret;

	if (!client_read_string_args(cmd, 1, &mailbox)) {
		client_send_command_error(cmd, "Invalid arguments.");
		return TRUE;
	}

	box = acl_mailbox_open_as_admin(cmd, mailbox);
	if (box == NULL)
		return TRUE;

	str = t_str_new(128);
	str_append(str, "* ACL ");
	imap_quote_append_string(str, mailbox, FALSE);
	len = str_len(str);

	ret = imap_acl_write_aclobj(str, acl_mailbox_get_aclobj(box));
	if (ret == 0) {
		client_send_line(cmd->client, str_c(str));
		client_send_tagline(cmd, "OK Getacl completed.");
	} else {
		client_send_tagline(cmd, "NO "MAIL_ERRSTR_CRITICAL_MSG);
	}
	mailbox_close(&box);
	return TRUE;
}

static bool cmd_myrights(struct client_command_context *cmd)
{
	struct mail_storage *storage;
	struct mailbox *box;
	const char *mailbox, *real_mailbox;
	const char *const *rights;
	string_t *str;

	if (!client_read_string_args(cmd, 1, &mailbox)) {
		client_send_command_error(cmd, "Invalid arguments.");
		return TRUE;
	}

	real_mailbox = mailbox;
	storage = client_find_storage(cmd, &real_mailbox);
	if (storage == NULL)
		return TRUE;

	box = mailbox_open(&storage, real_mailbox, NULL,
			   ACL_MAILBOX_OPEN_FLAGS | MAILBOX_OPEN_IGNORE_ACLS);
	if (box == NULL) {
		client_send_storage_error(cmd, storage);
		return TRUE;
	}

	if (acl_object_get_my_rights(acl_mailbox_get_aclobj(box),
				     pool_datastack_create(), &rights) < 0) {
		client_send_tagline(cmd, "NO "MAIL_ERRSTR_CRITICAL_MSG);
		mailbox_close(&box);
		return TRUE;
	}
	/* Post right alone doesn't give permissions to see if the mailbox
	   exists or not. Only mail deliveries care about that. */
	if (*rights == NULL ||
	    (strcmp(*rights, MAIL_ACL_POST) == 0 && rights[1] == NULL)) {
		client_send_tagline(cmd, t_strdup_printf(
			"NO ["IMAP_RESP_CODE_NONEXISTENT"] "
			MAIL_ERRSTR_MAILBOX_NOT_FOUND, real_mailbox));
		mailbox_close(&box);
		return TRUE;
	}

	str = t_str_new(128);
	str_append(str, "* MYRIGHTS ");
	imap_quote_append_string(str, mailbox, FALSE);
	str_append_c(str,' ');
	imap_acl_write_rights_list(str, rights);

	client_send_line(cmd->client, str_c(str));
	client_send_tagline(cmd, "OK Myrights completed.");
	return TRUE;
}

static bool cmd_listrights(struct client_command_context *cmd)
{
	struct mailbox *box;
	const char *mailbox, *identifier;
	string_t *str;

	if (!client_read_string_args(cmd, 2, &mailbox, &identifier)) {
		client_send_command_error(cmd, "Invalid arguments.");
		return TRUE;
	}

	box = acl_mailbox_open_as_admin(cmd, mailbox);
	if (box == NULL)
		return TRUE;

	str = t_str_new(128);
	str_append(str, "* LISTRIGHTS ");
	imap_quote_append_string(str, mailbox, FALSE);
	str_append_c(str, ' ');
	imap_quote_append_string(str, identifier, FALSE);
	str_append_c(str, ' ');
	str_append(str, "\"\" l r w s t p i e k x a c d");

	client_send_line(cmd->client, str_c(str));
	client_send_tagline(cmd, "OK Listrights completed.");
	return TRUE;
}

static int
imap_acl_letters_parse(const char *letters, const char *const **rights_r,
		       const char **error_r)
{
	ARRAY_TYPE(const_string) rights;
	unsigned int i;

	t_array_init(&rights, 64);
	for (; *letters != '\0'; letters++) {
		for (i = 0; imap_acl_letter_map[i].name != NULL; i++) {
			if (imap_acl_letter_map[i].letter == *letters) {
				array_append(&rights,
					     &imap_acl_letter_map[i].name, 1);
				break;
			}
		}
		if (imap_acl_letter_map[i].name == NULL) {
			*error_r = t_strdup_printf("Invalid ACL right: %c",
						   *letters);
			return -1;
		}
	}
	(void)array_append_space(&rights);
	*rights_r = array_idx(&rights, 0);
	return 0;
}

static int
imap_acl_identifier_parse(const char *id, struct acl_rights *rights,
			  bool check_anyone, const char **error_r)
{
	if (strncmp(id, IMAP_ACL_GLOBAL_PREFIX,
		    strlen(IMAP_ACL_GLOBAL_PREFIX)) == 0) {
		*error_r = t_strdup_printf("Global ACLs can't be modified: %s",
					   id);
		return -1;
	}

	if (strcmp(id, IMAP_ACL_ANYONE) == 0) {
		if (!acl_anyone_allow && check_anyone) {
			*error_r = "'anyone' identifier is disallowed";
			return -1;
		}
		rights->id_type = ACL_ID_ANYONE;
	} else if (strcmp(id, IMAP_ACL_AUTHENTICATED) == 0) {
		if (!acl_anyone_allow && check_anyone) {
			*error_r = "'authenticated' identifier is disallowed";
			return -1;
		}
		rights->id_type = ACL_ID_AUTHENTICATED;
	} else if (strcmp(id, IMAP_ACL_OWNER) == 0)
		rights->id_type = ACL_ID_OWNER;
	else if (strncmp(id, IMAP_ACL_GROUP_PREFIX,
			 strlen(IMAP_ACL_GROUP_PREFIX)) == 0) {
		rights->id_type = ACL_ID_GROUP;
		rights->identifier = id + strlen(IMAP_ACL_GROUP_PREFIX);
	} else if (strncmp(id, IMAP_ACL_GROUP_OVERRIDE_PREFIX,
			   strlen(IMAP_ACL_GROUP_OVERRIDE_PREFIX)) == 0) {
		rights->id_type = ACL_ID_GROUP_OVERRIDE;
		rights->identifier = id +
			strlen(IMAP_ACL_GROUP_OVERRIDE_PREFIX);
	} else {
		rights->id_type = ACL_ID_USER;
		rights->identifier = id;
	}
	return 0;
}

static bool cmd_setacl(struct client_command_context *cmd)
{
	struct mailbox *box;
	struct acl_rights_update update;
	const char *mailbox, *identifier, *rights, *error;
	bool negative = FALSE;

	if (!client_read_string_args(cmd, 3, &mailbox, &identifier, &rights) ||
	    *identifier == '\0' || *rights == '\0') {
		client_send_command_error(cmd, "Invalid arguments.");
		return TRUE;
	}

	memset(&update, 0, sizeof(update));
	if (*identifier == '-') {
		negative = TRUE;
		identifier++;
	}

	switch (*rights) {
	case '-':
		update.modify_mode = ACL_MODIFY_MODE_REMOVE;
		rights++;
		break;
	case '+':
		update.modify_mode = ACL_MODIFY_MODE_ADD;
		rights++;
		break;
	default:
		update.modify_mode = ACL_MODIFY_MODE_REPLACE;
		break;
	}

	if (imap_acl_identifier_parse(identifier, &update.rights,
				      TRUE, &error) < 0) {
		client_send_command_error(cmd, error);
		return TRUE;
	}
	if (imap_acl_letters_parse(rights, &update.rights.rights, &error) < 0) {
		client_send_command_error(cmd, error);
		return TRUE;
	}

	box = acl_mailbox_open_as_admin(cmd, mailbox);
	if (box == NULL)
		return TRUE;

	if (negative) {
		update.neg_modify_mode = update.modify_mode;
		update.modify_mode = ACL_MODIFY_MODE_REMOVE;
		update.rights.neg_rights = update.rights.rights;
		update.rights.rights = NULL;
	}

	if (acl_object_update(acl_mailbox_get_aclobj(box), &update) < 0)
		client_send_tagline(cmd, "NO "MAIL_ERRSTR_CRITICAL_MSG);
	else
		client_send_tagline(cmd, "OK Setacl complete.");
	mailbox_close(&box);
	return TRUE;
}

static bool cmd_deleteacl(struct client_command_context *cmd)
{
	struct mailbox *box;
	struct acl_rights_update update;
	const char *mailbox, *identifier, *error;

	if (!client_read_string_args(cmd, 2, &mailbox, &identifier) ||
	    *identifier == '\0') {
		client_send_command_error(cmd, "Invalid arguments.");
		return TRUE;
	}

	memset(&update, 0, sizeof(update));
	if (*identifier != '-')
		update.modify_mode = ACL_MODIFY_MODE_CLEAR;
	else {
		update.neg_modify_mode = ACL_MODIFY_MODE_CLEAR;
		identifier++;
	}

	if (imap_acl_identifier_parse(identifier, &update.rights,
				      FALSE, &error) < 0) {
		client_send_command_error(cmd, error);
		return TRUE;
	}

	box = acl_mailbox_open_as_admin(cmd, mailbox);
	if (box == NULL)
		return TRUE;

	if (acl_object_update(acl_mailbox_get_aclobj(box), &update) < 0)
		client_send_tagline(cmd, "NO "MAIL_ERRSTR_CRITICAL_MSG);
	else
		client_send_tagline(cmd, "OK Deleteacl complete.");
	mailbox_close(&box);
	return TRUE;
}

void imap_acl_plugin_init(void)
{
	const char *env;

	if (getenv("ACL") == NULL)
		return;

	env = getenv("ACL_ANYONE");
	if (env != NULL)
		acl_anyone_allow = strcmp(env, "allow") == 0;

	str_append(capability_string, " ACL RIGHTS=texk");

	command_register("LISTRIGHTS", cmd_listrights, 0);
	command_register("GETACL", cmd_getacl, 0);
	command_register("MYRIGHTS", cmd_myrights, 0);
	command_register("SETACL", cmd_setacl, 0);
	command_register("DELETEACL", cmd_deleteacl, 0);
}

void imap_acl_plugin_deinit(void)
{
	if (getenv("ACL") == NULL)
		return;

	command_unregister("GETACL");
	command_unregister("MYRIGHTS");
	command_unregister("SETACL");
	command_unregister("DELETEACL");
	command_unregister("LISTRIGHTS");
}
