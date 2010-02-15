#ifndef MAILBOX_LIST_FS_H
#define MAILBOX_LIST_FS_H

#include "mailbox-list-private.h"

/* Don't allow creating too long mailbox names. They could start causing
   problems when they reach the limit. */
#define FS_MAX_CREATE_MAILBOX_NAME_LENGTH (MAILBOX_LIST_NAME_MAX_LENGTH/2)

/* When doing deletion via renaming it first to trash directory, use this as
   the trash directory name */
#define MAILBOX_LIST_FS_TRASH_DIR_NAME "..DOVECOT-TrasH"

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

int fs_list_get_mailbox_flags(struct mailbox_list *list,
			      const char *dir, const char *fname,
			      enum mailbox_list_file_type type,
			      struct stat *st_r,
			      enum mailbox_info_flags *flags);

#endif
