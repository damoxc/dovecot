/* Copyright (c) 2011-2013 Dovecot authors, see the included COPYING file */

#include "lib.h"
#include "ioloop.h"
#include "str.h"
#include "mail-copy.h"
#include "mail-user.h"
#include "mailbox-list-private.h"
#include "index-mail.h"
#include "pop3c-client.h"
#include "pop3c-settings.h"
#include "pop3c-sync.h"
#include "pop3c-storage.h"

#define DNS_CLIENT_SOCKET_NAME "dns-client"

extern struct mail_storage pop3c_storage;
extern struct mailbox pop3c_mailbox;

static struct mail_storage *pop3c_storage_alloc(void)
{
	struct pop3c_storage *storage;
	pool_t pool;

	pool = pool_alloconly_create("pop3c storage", 512+256);
	storage = p_new(pool, struct pop3c_storage, 1);
	storage->storage = pop3c_storage;
	storage->storage.pool = pool;
	return &storage->storage;
}

static int
pop3c_storage_create(struct mail_storage *_storage,
		     struct mail_namespace *ns ATTR_UNUSED,
		     const char **error_r)
{
	struct pop3c_storage *storage = (struct pop3c_storage *)_storage;

	storage->set = mail_storage_get_driver_settings(_storage);
	if (storage->set->pop3c_host[0] == '\0') {
		*error_r = "missing pop3c_host";
		return -1;
	}
	if (storage->set->pop3c_password == '\0') {
		*error_r = "missing pop3c_password";
		return -1;
	}

	return 0;
}

static struct pop3c_client *
pop3c_client_create_from_set(struct mail_user *user,
			     const struct pop3c_settings *set)
{
	struct pop3c_client_settings client_set;
	string_t *str;

	memset(&client_set, 0, sizeof(client_set));
	client_set.host = set->pop3c_host;
	client_set.port = set->pop3c_port;
	client_set.username = set->pop3c_user;
	client_set.master_user = set->pop3c_master_user;
	client_set.password = set->pop3c_password;
	client_set.dns_client_socket_path =
		t_strconcat(user->set->base_dir, "/",
			    DNS_CLIENT_SOCKET_NAME, NULL);
	str = t_str_new(128);
	mail_user_set_get_temp_prefix(str, user->set);
	client_set.temp_path_prefix = str_c(str);

	client_set.debug = user->mail_debug;
	client_set.rawlog_dir =
		mail_user_home_expand(user, set->pop3c_rawlog_dir);

	client_set.ssl_ca_dir = set->ssl_client_ca_dir;
	client_set.ssl_verify = set->pop3c_ssl_verify;
	if (strcmp(set->pop3c_ssl, "pop3s") == 0)
		client_set.ssl_mode = POP3C_CLIENT_SSL_MODE_IMMEDIATE;
	else if (strcmp(set->pop3c_ssl, "starttls") == 0)
		client_set.ssl_mode = POP3C_CLIENT_SSL_MODE_STARTTLS;
	else
		client_set.ssl_mode = POP3C_CLIENT_SSL_MODE_NONE;
	client_set.ssl_crypto_device = set->ssl_crypto_device;
	return pop3c_client_init(&client_set);
}

static void
pop3c_storage_get_list_settings(const struct mail_namespace *ns,
				struct mailbox_list_settings *set)
{
	set->layout = MAILBOX_LIST_NAME_FS;
	if (set->root_dir != NULL && *set->root_dir != '\0' &&
	    set->index_dir == NULL) {
		/* we don't really care about root_dir, but we
		   just need to get index_dir autocreated.
		   it happens when index_dir differs from root_dir. */
		set->index_dir = set->root_dir;
		set->root_dir = p_strconcat(ns->user->pool,
					    set->root_dir, "/.", NULL);
	}
}

static struct mailbox *
pop3c_mailbox_alloc(struct mail_storage *storage, struct mailbox_list *list,
		    const char *vname, enum mailbox_flags flags)
{
	struct pop3c_mailbox *mbox;
	pool_t pool;

	pool = pool_alloconly_create("pop3c mailbox", 1024*3);
	mbox = p_new(pool, struct pop3c_mailbox, 1);
	mbox->box = pop3c_mailbox;
	mbox->box.pool = pool;
	mbox->box.storage = storage;
	mbox->box.list = list;
	mbox->box.mail_vfuncs = &pop3c_mail_vfuncs;
	mbox->storage = (struct pop3c_storage *)storage;

	index_storage_mailbox_alloc(&mbox->box, vname, flags, MAIL_INDEX_PREFIX);
	return &mbox->box;
}

static int
pop3c_mailbox_exists(struct mailbox *box, bool auto_boxes ATTR_UNUSED,
		     enum mailbox_existence *existence_r)
{
	if (box->inbox_any)
		*existence_r = MAILBOX_EXISTENCE_SELECT;
	else
		*existence_r = MAILBOX_EXISTENCE_NONE;
	return 0;
}

static void pop3c_login_callback(enum pop3c_command_state state,
				 const char *reply, void *context)
{
	struct pop3c_mailbox *mbox = context;

	switch (state) {
	case POP3C_COMMAND_STATE_OK:
		mbox->logged_in = TRUE;
		break;
	case POP3C_COMMAND_STATE_ERR:
		if (strncmp(reply, "[IN-USE] ", 9) == 0) {
			mail_storage_set_error(mbox->box.storage,
					       MAIL_ERROR_INUSE, reply + 9);
		} else {
			/* authentication failure probably */
			mail_storage_set_error(mbox->box.storage,
					       MAIL_ERROR_PARAMS, reply);
		}
		break;
	case POP3C_COMMAND_STATE_DISCONNECTED:
		mail_storage_set_critical(mbox->box.storage,
			"pop3c: Disconnected from remote server");
		break;
	}
}

static int pop3c_mailbox_open(struct mailbox *box)
{
	struct pop3c_mailbox *mbox = (struct pop3c_mailbox *)box;

	if (strcmp(box->name, "INBOX") != 0) {
		mail_storage_set_error(box->storage, MAIL_ERROR_NOTFOUND,
				       T_MAIL_ERR_MAILBOX_NOT_FOUND(box->name));
		return -1;
	}

	if (index_storage_mailbox_open(box, FALSE) < 0)
		return -1;

	mbox->client = pop3c_client_create_from_set(box->storage->user,
						    mbox->storage->set);
	pop3c_client_login(mbox->client, pop3c_login_callback, mbox);
	pop3c_client_run(mbox->client);
	return mbox->logged_in ? 0 : -1;
}

static void pop3c_mailbox_close(struct mailbox *box)
{
	struct pop3c_mailbox *mbox = (struct pop3c_mailbox *)box;

	if (mbox->uidl_pool != NULL)
		pool_unref(&mbox->uidl_pool);
	i_free(mbox->msg_sizes);
	pop3c_client_deinit(&mbox->client);
	index_storage_mailbox_close(box);
}

static int
pop3c_mailbox_create(struct mailbox *box,
		     const struct mailbox_update *update ATTR_UNUSED,
		     bool directory ATTR_UNUSED)
{
	mail_storage_set_error(box->storage, MAIL_ERROR_NOTPOSSIBLE,
			       "POP3 mailbox creation isn't supported");
	return -1;
}

static int
pop3c_mailbox_update(struct mailbox *box,
		     const struct mailbox_update *update ATTR_UNUSED)
{
	mail_storage_set_error(box->storage, MAIL_ERROR_NOTPOSSIBLE,
			       "POP3 mailbox update isn't supported");
	return -1;
}

static void pop3c_notify_changes(struct mailbox *box ATTR_UNUSED)
{
}

static bool pop3c_storage_is_inconsistent(struct mailbox *box)
{
	struct pop3c_mailbox *mbox = (struct pop3c_mailbox *)box;

	return index_storage_is_inconsistent(box) ||
		!pop3c_client_is_connected(mbox->client);
}

struct mail_storage pop3c_storage = {
	.name = POP3C_STORAGE_NAME,
	.class_flags = MAIL_STORAGE_CLASS_FLAG_NO_ROOT,

	.v = {
		pop3c_get_setting_parser_info,
		pop3c_storage_alloc,
		pop3c_storage_create,
		index_storage_destroy,
		NULL,
		pop3c_storage_get_list_settings,
		NULL,
		pop3c_mailbox_alloc,
		NULL
	}
};

struct mailbox pop3c_mailbox = {
	.v = {
		index_storage_is_readonly,
		index_storage_mailbox_enable,
		pop3c_mailbox_exists,
		pop3c_mailbox_open,
		pop3c_mailbox_close,
		index_storage_mailbox_free,
		pop3c_mailbox_create,
		pop3c_mailbox_update,
		index_storage_mailbox_delete,
		index_storage_mailbox_rename,
		index_storage_get_status,
		index_mailbox_get_metadata,
		index_storage_set_subscribed,
		index_storage_attribute_set,
		index_storage_attribute_get,
		index_storage_attribute_iter_init,
		index_storage_attribute_iter_next,
		index_storage_attribute_iter_deinit,
		index_storage_list_index_has_changed,
		index_storage_list_index_update_sync,
		pop3c_storage_sync_init,
		index_mailbox_sync_next,
		index_mailbox_sync_deinit,
		NULL,
		pop3c_notify_changes,
		index_transaction_begin,
		index_transaction_commit,
		index_transaction_rollback,
		NULL,
		index_mail_alloc,
		index_storage_search_init,
		index_storage_search_deinit,
		index_storage_search_next_nonblock,
		index_storage_search_next_update_seq,
		NULL,
		NULL,
		NULL,
		NULL,
		NULL,
		mail_storage_copy,
		NULL,
		NULL,
		NULL,
		pop3c_storage_is_inconsistent
	}
};
