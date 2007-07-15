#ifndef __MAILBOX_LIST_FS_H
#define __MAILBOX_LIST_FS_H

#include "mailbox-list-private.h"

/* Don't allow creating too long mailbox names. They could start causing
   problems when they reach the limit. */
#define FS_MAX_CREATE_MAILBOX_NAME_LENGTH (PATH_MAX/2)

struct fs_mailbox_list {
	struct mailbox_list list;

	const char *temp_prefix;
};

struct mailbox_list_iterate_context *
fs_list_iter_init(struct mailbox_list *_list, const char *const *patterns,
		  enum mailbox_list_iter_flags flags);
int fs_list_iter_deinit(struct mailbox_list_iterate_context *ctx);
const struct mailbox_info *
fs_list_iter_next(struct mailbox_list_iterate_context *ctx);

#endif
