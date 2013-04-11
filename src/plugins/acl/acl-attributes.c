/* Copyright (c) 2013 Dovecot authors, see the included COPYING file */

#include "lib.h"
#include "str.h"
#include "mail-storage-private.h"
#include "acl-api-private.h"
#include "acl-plugin.h"
#include "acl-storage.h"

struct acl_mailbox_attribute_iter {
	struct mailbox_attribute_iter iter;
	struct mailbox_attribute_iter *super;

	struct acl_object_list_iter *acl_iter;
	string_t *acl_name;

	bool failed;
};

static int
acl_attribute_update_acl(struct mailbox_transaction_context *t, const char *key,
			 const struct mail_attribute_value *value)
{
	const char *value_str, *id, *const *rights, *error;
	struct acl_rights_update update;

	/* for now allow only dsync to update ACLs this way.
	   if this check is removed, it should be replaced by a setting, since
	   some admins may still have configured Dovecot using dovecot-acl
	   files directly that they don't want users to update. and in any case
	   ACL_STORAGE_RIGHT_ADMIN must be checked then. */
	if (!t->box->storage->user->dsyncing) {
		mail_storage_set_error(t->box->storage, MAIL_ERROR_PERM,
				       MAIL_ERRSTR_NO_PERMISSION);
		return -1;
	}

	if (mailbox_attribute_value_to_string(t->box->storage, value,
					      &value_str) < 0)
		return -1;

	memset(&update, 0, sizeof(update));
	update.modify_mode = ACL_MODIFY_MODE_REPLACE;
	update.neg_modify_mode = ACL_MODIFY_MODE_REPLACE;
	update.last_change = value->last_change;
	id = key + strlen(MAILBOX_ATTRIBUTE_PREFIX_ACL);
	rights = value_str == NULL ? NULL : t_strsplit(value_str, " ");
	if (acl_rights_update_import(&update, id, rights, &error) < 0) {
		mail_storage_set_error(t->box->storage, MAIL_ERROR_PARAMS, error);
		return -1;
	}
	/* FIXME: this should actually be done only at commit().. */
	return acl_mailbox_update_acl(t, &update);
}

static int acl_attribute_get_acl(struct mailbox *box, const char *key,
				 struct mail_attribute_value *value_r)
{
	struct acl_object *aclobj = acl_mailbox_get_aclobj(box);
	struct acl_object_list_iter *iter;
	struct acl_rights rights, wanted_rights;
	const char *id;
	int ret;

	memset(value_r, 0, sizeof(*value_r));

	if (!box->storage->user->dsyncing) {
		mail_storage_set_error(box->storage, MAIL_ERROR_PERM,
				       MAIL_ERRSTR_NO_PERMISSION);
		return -1;
	}
	/* set last_change for all ACL objects, even if they don't exist
	   (because they could have been removed by the last change, and dsync
	   can use this information) */
	(void)acl_object_last_changed(aclobj, &value_r->last_change);

	memset(&wanted_rights, 0, sizeof(wanted_rights));
	id = key + strlen(MAILBOX_ATTRIBUTE_PREFIX_ACL);
	if (acl_identifier_parse(id, &wanted_rights) < 0) {
		mail_storage_set_error(box->storage, MAIL_ERROR_PARAMS,
				       t_strdup_printf("Invalid ID: %s", id));
		return -1;
	}

	iter = acl_object_list_init(aclobj);
	while ((ret = acl_object_list_next(iter, &rights)) > 0) {
		if (!rights.global &&
		    rights.id_type == wanted_rights.id_type &&
		    null_strcmp(rights.identifier, wanted_rights.identifier) == 0) {
			value_r->value = acl_rights_export(&rights);
			break;
		}
	}
	if (ret < 0)
		mail_storage_set_internal_error(box->storage);
	acl_object_list_deinit(&iter);
	return ret;
}

static int acl_have_attribute_rights(struct mailbox *box)
{
	int ret;

	/* RFC 5464:

	   When the ACL extension [RFC4314] is present, users can only set and
	   retrieve private or shared mailbox annotations on a mailbox on which
	   they have the "l" right and any one of the "r", "s", "w", "i", or "p"
	   rights.
	*/
	ret = acl_mailbox_right_lookup(box, ACL_STORAGE_RIGHT_LOOKUP);
	if (ret <= 0) {
		if (ret < 0)
			return -1;
		mail_storage_set_error(box->storage, MAIL_ERROR_NOTFOUND,
				T_MAIL_ERR_MAILBOX_NOT_FOUND(box->vname));
		return -1;
	}

	if (acl_mailbox_right_lookup(box, ACL_STORAGE_RIGHT_READ) > 0)
		return 0;
	if (acl_mailbox_right_lookup(box, ACL_STORAGE_RIGHT_WRITE_SEEN) > 0)
		return 0;
	if (acl_mailbox_right_lookup(box, ACL_STORAGE_RIGHT_WRITE) > 0)
		return 0;
	if (acl_mailbox_right_lookup(box, ACL_STORAGE_RIGHT_INSERT) > 0)
		return 0;
	if (acl_mailbox_right_lookup(box, ACL_STORAGE_RIGHT_POST) > 0)
		return 0;
	return -1;
}

int acl_attribute_set(struct mailbox_transaction_context *t,
		      enum mail_attribute_type type, const char *key,
		      const struct mail_attribute_value *value)
{
	struct acl_mailbox *abox = ACL_CONTEXT(t->box);

	if (acl_have_attribute_rights(t->box) < 0)
		return -1;
	if (strncmp(key, MAILBOX_ATTRIBUTE_PREFIX_ACL,
		    strlen(MAILBOX_ATTRIBUTE_PREFIX_ACL)) == 0)
		return acl_attribute_update_acl(t, key, value);
	return abox->module_ctx.super.attribute_set(t, type, key, value);
}

int acl_attribute_get(struct mailbox_transaction_context *t,
		      enum mail_attribute_type type, const char *key,
		      struct mail_attribute_value *value_r)
{
	struct acl_mailbox *abox = ACL_CONTEXT(t->box);

	if (acl_have_attribute_rights(t->box) < 0)
		return -1;
	if (strncmp(key, MAILBOX_ATTRIBUTE_PREFIX_ACL,
		    strlen(MAILBOX_ATTRIBUTE_PREFIX_ACL)) == 0)
		return acl_attribute_get_acl(t->box, key, value_r);
	return abox->module_ctx.super.attribute_get(t, type, key, value_r);
}

struct mailbox_attribute_iter *
acl_attribute_iter_init(struct mailbox *box, enum mail_attribute_type type,
			const char *prefix)
{
	struct acl_mailbox *abox = ACL_CONTEXT(box);
	struct acl_mailbox_attribute_iter *aiter;

	aiter = i_new(struct acl_mailbox_attribute_iter, 1);
	aiter->iter.box = box;
	if (acl_have_attribute_rights(box) < 0)
		aiter->failed = TRUE;
	else {
		aiter->super = abox->module_ctx.super.
			attribute_iter_init(box, type, prefix);
		if (box->storage->user->dsyncing &&
		    type == MAIL_ATTRIBUTE_TYPE_SHARED &&
		    strncmp(prefix, MAILBOX_ATTRIBUTE_PREFIX_ACL,
			    strlen(prefix)) == 0) {
			aiter->acl_iter = acl_object_list_init(abox->aclobj);
			aiter->acl_name = str_new(default_pool, 128);
			str_append(aiter->acl_name, MAILBOX_ATTRIBUTE_PREFIX_ACL);
		}
	}
	return &aiter->iter;
}

static const char *
acl_attribute_iter_next_acl(struct acl_mailbox_attribute_iter *aiter)
{
	struct acl_rights rights;
	int ret;

	while ((ret = acl_object_list_next(aiter->acl_iter, &rights)) > 0) {
		if (rights.global)
			continue;
		str_truncate(aiter->acl_name, strlen(MAILBOX_ATTRIBUTE_PREFIX_ACL));
		acl_rights_write_id(aiter->acl_name, &rights);
		return str_c(aiter->acl_name);
	}
	if (ret < 0) {
		mail_storage_set_internal_error(aiter->iter.box->storage);
		aiter->failed = TRUE;
		return NULL;
	}
	acl_object_list_deinit(&aiter->acl_iter);
	return NULL;
}

const char *acl_attribute_iter_next(struct mailbox_attribute_iter *iter)
{
	struct acl_mailbox_attribute_iter *aiter =
		(struct acl_mailbox_attribute_iter *)iter;
	struct acl_mailbox *abox = ACL_CONTEXT(iter->box);
	const char *key;

	if (aiter->super == NULL)
		return NULL;
	if (aiter->acl_iter != NULL) {
		if ((key = acl_attribute_iter_next_acl(aiter)) != NULL)
			return key;
	}
	return abox->module_ctx.super.attribute_iter_next(aiter->super);
}

int acl_attribute_iter_deinit(struct mailbox_attribute_iter *iter)
{
	struct acl_mailbox_attribute_iter *aiter =
		(struct acl_mailbox_attribute_iter *)iter;
	struct acl_mailbox *abox = ACL_CONTEXT(iter->box);
	int ret = aiter->failed ? -1 : 0;

	if (aiter->super != NULL) {
		if (abox->module_ctx.super.attribute_iter_deinit(aiter->super) < 0)
			ret = -1;
	}
	if (aiter->acl_iter != NULL)
		acl_object_list_deinit(&aiter->acl_iter);
	if (aiter->acl_name != NULL)
		str_free(&aiter->acl_name);
	i_free(aiter);
	return ret;
}
