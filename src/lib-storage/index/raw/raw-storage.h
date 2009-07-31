#ifndef RAW_STORAGE_H
#define RAW_STORAGE_H

#include "index-storage.h"
#include "mailbox-list-private.h"

#define RAW_STORAGE_NAME "raw"
#define RAW_SUBSCRIPTION_FILE_NAME "subscriptions"

struct raw_storage {
	struct mail_storage storage;
	union mailbox_list_module_context list_module_ctx;
};

struct raw_mailbox {
	struct index_mailbox ibox;
	struct raw_storage *storage;

	time_t mtime, ctime;
	uoff_t size;
	const char *envelope_sender;

	unsigned int synced:1;
	unsigned int have_filename:1;
};

extern struct mail_vfuncs raw_mail_vfuncs;

#endif
