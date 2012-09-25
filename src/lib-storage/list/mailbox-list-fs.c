/* Copyright (c) 2006-2012 Dovecot authors, see the included COPYING file */

#include "lib.h"
#include "hostpid.h"
#include "mkdir-parents.h"
#include "mailbox-log.h"
#include "subscription-file.h"
#include "mail-storage.h"
#include "mailbox-list-subscriptions.h"
#include "mailbox-list-delete.h"
#include "mailbox-list-fs.h"

#include <stdio.h>
#include <unistd.h>
#include <sys/stat.h>

#define GLOBAL_TEMP_PREFIX ".temp."

extern struct mailbox_list fs_mailbox_list;

static struct mailbox_list *fs_list_alloc(void)
{
	struct fs_mailbox_list *list;
	pool_t pool;

	pool = pool_alloconly_create("fs list", 2048);

	list = p_new(pool, struct fs_mailbox_list, 1);
	list->list = fs_mailbox_list;
	list->list.pool = pool;

	list->temp_prefix = p_strconcat(pool, GLOBAL_TEMP_PREFIX,
					my_hostname, ".", my_pid, ".", NULL);
	return &list->list;
}

static void fs_list_deinit(struct mailbox_list *_list)
{
	struct fs_mailbox_list *list = (struct fs_mailbox_list *)_list;

	pool_unref(&list->list.pool);
}

static char fs_list_get_hierarchy_sep(struct mailbox_list *list ATTR_UNUSED)
{
	return '/';
}

static const char *
fs_list_get_path(struct mailbox_list *_list, const char *name,
		 enum mailbox_list_path_type type)
{
	const struct mailbox_list_settings *set = &_list->set;
	const char *root_dir, *error;

	if (name == NULL) {
		/* return root directories */
		return mailbox_list_set_get_root_path(set, type);
	}

	i_assert(mailbox_list_is_valid_name(_list, name, &error));

	if (mailbox_list_try_get_absolute_path(_list, &name))
		return name;

	root_dir = set->root_dir;
	switch (type) {
	case MAILBOX_LIST_PATH_TYPE_DIR:
		if (*set->maildir_name != '\0')
			return t_strdup_printf("%s/%s%s", set->root_dir,
					       set->mailbox_dir_name, name);
		break;
	case MAILBOX_LIST_PATH_TYPE_ALT_DIR:
		if (set->alt_dir == NULL)
			return NULL;
		if (*set->maildir_name != '\0')
			return t_strdup_printf("%s/%s%s", set->alt_dir,
					       set->mailbox_dir_name, name);
		root_dir = set->alt_dir;
		break;
	case MAILBOX_LIST_PATH_TYPE_MAILBOX:
		break;
	case MAILBOX_LIST_PATH_TYPE_ALT_MAILBOX:
		if (set->alt_dir == NULL)
			return NULL;
		root_dir = set->alt_dir;
		break;
	case MAILBOX_LIST_PATH_TYPE_CONTROL:
		if (set->control_dir != NULL)
			return t_strdup_printf("%s/%s%s", set->control_dir,
					       set->mailbox_dir_name, name);
		break;
	case MAILBOX_LIST_PATH_TYPE_INDEX:
		if (set->index_dir != NULL) {
			if (*set->index_dir == '\0')
				return "";
			return t_strdup_printf("%s/%s%s", set->index_dir,
					       set->mailbox_dir_name, name);
		}
		break;
	case MAILBOX_LIST_PATH_TYPE_INDEX_PRIVATE:
		if (set->index_pvt_dir == NULL)
			return NULL;
		return t_strdup_printf("%s/%s%s", set->index_pvt_dir,
				       set->mailbox_dir_name, name);
	}

	if (type == MAILBOX_LIST_PATH_TYPE_ALT_DIR ||
	    type == MAILBOX_LIST_PATH_TYPE_ALT_MAILBOX) {
		/* don't use inbox_path */
	} else if (strcmp(name, "INBOX") == 0 && set->inbox_path != NULL) {
		/* If INBOX is a file, index and control directories are
		   located in root directory. */
		if ((_list->flags & MAILBOX_LIST_FLAG_MAILBOX_FILES) == 0 ||
		    type == MAILBOX_LIST_PATH_TYPE_MAILBOX ||
		    type == MAILBOX_LIST_PATH_TYPE_DIR)
			return set->inbox_path;
	}

	if (root_dir == NULL)
		return type == MAILBOX_LIST_PATH_TYPE_INDEX ? "" : NULL;
	if (*set->maildir_name == '\0') {
		return t_strdup_printf("%s/%s%s", root_dir,
				       set->mailbox_dir_name, name);
	} else {
		return t_strdup_printf("%s/%s%s/%s", root_dir,
				       set->mailbox_dir_name, name,
				       set->maildir_name);
	}
}

static const char *
fs_list_get_temp_prefix(struct mailbox_list *_list, bool global)
{
	struct fs_mailbox_list *list = (struct fs_mailbox_list *)_list;

	return global ? GLOBAL_TEMP_PREFIX : list->temp_prefix;
}

static const char *
fs_list_join_refpattern(struct mailbox_list *_list ATTR_UNUSED,
			const char *ref, const char *pattern)
{
	if (*pattern == '/' || *pattern == '~') {
		/* pattern overrides reference */
	} else if (*ref != '\0') {
		/* merge reference and pattern */
		pattern = t_strconcat(ref, pattern, NULL);
	}
	return pattern;
}

static int fs_list_set_subscribed(struct mailbox_list *_list,
				  const char *name, bool set)
{
	struct fs_mailbox_list *list = (struct fs_mailbox_list *)_list;
	const char *path;

	path = t_strconcat(_list->set.control_dir != NULL ?
			   _list->set.control_dir : _list->set.root_dir,
			   "/", _list->set.subscription_fname, NULL);
	return subsfile_set_subscribed(_list, path, list->temp_prefix,
				       name, set);
}


static const char *mailbox_list_fs_get_trash_dir(struct mailbox_list *list)
{
	const char *root_dir;

	root_dir = mailbox_list_get_root_path(list, MAILBOX_LIST_PATH_TYPE_DIR);
	return t_strdup_printf("%s/"MAILBOX_LIST_FS_TRASH_DIR_NAME, root_dir);
}

static int
fs_list_delete_maildir(struct mailbox_list *list, const char *name)
{
	const char *path, *trash_dir;
	bool rmdir_path;
	int ret;

	if (*list->set.maildir_name != '\0' &&
	    *list->set.mailbox_dir_name != '\0') {
		trash_dir = mailbox_list_fs_get_trash_dir(list);
		ret = mailbox_list_delete_maildir_via_trash(list, name,
							    trash_dir);
		if (ret < 0)
			return -1;

		if (ret > 0) {
			/* try to delete the parent directory */
			path = mailbox_list_get_path(list, name,
						     MAILBOX_LIST_PATH_TYPE_DIR);
			if (rmdir(path) < 0 && errno != ENOENT &&
			    errno != ENOTEMPTY && errno != EEXIST) {
				mailbox_list_set_critical(list,
					"rmdir(%s) failed: %m", path);
			}
			return 0;
		}
	}

	rmdir_path = *list->set.maildir_name != '\0';
	path = mailbox_list_get_path(list, name,
				     MAILBOX_LIST_PATH_TYPE_MAILBOX);
	return mailbox_list_delete_mailbox_nonrecursive(list, name, path,
							rmdir_path);
}

static int fs_list_delete_mailbox(struct mailbox_list *list, const char *name)
{
	int ret;

	if ((list->flags & MAILBOX_LIST_FLAG_MAILBOX_FILES) != 0) {
		ret = mailbox_list_delete_mailbox_file(list, name);
	} else {
		ret = fs_list_delete_maildir(list, name);
	}

	if (ret == 0 || (list->props & MAILBOX_LIST_PROP_AUTOCREATE_DIRS) != 0)
		mailbox_list_delete_finish(list, name);
	return ret;
}

static int fs_list_rmdir(struct mailbox_list *list, const char *name,
			 const char *path)
{
	guid_128_t dir_sha128;

	if (rmdir(path) < 0)
		return -1;

	mailbox_name_get_sha128(name, dir_sha128);
	mailbox_list_add_change(list, MAILBOX_LOG_RECORD_DELETE_DIR,
				dir_sha128);
	return 0;
}

static int fs_list_delete_dir(struct mailbox_list *list, const char *name)
{
	const char *path, *child_name, *child_path, *p;
	char sep;

	path = mailbox_list_get_path(list, name, MAILBOX_LIST_PATH_TYPE_DIR);
	if (fs_list_rmdir(list, name, path) == 0)
		return 0;

	if (errno == ENOENT || errno == ENOTDIR) {
		mailbox_list_set_error(list, MAIL_ERROR_NOTFOUND,
			T_MAIL_ERR_MAILBOX_NOT_FOUND(name));
	} else if (errno == ENOTEMPTY || errno == EEXIST) {
		/* mbox workaround: if only .imap/ directory is preventing the
		   deletion, remove it */
		sep = mailbox_list_get_hierarchy_sep(list);
		child_name = t_strdup_printf("%s%cchild", name, sep);
		child_path = mailbox_list_get_path(list, child_name,
						   MAILBOX_LIST_PATH_TYPE_INDEX);
		if (strncmp(path, child_path, strlen(path)) == 0) {
			/* drop the "/child" part out. */
			p = strrchr(child_path, '/');
			if (rmdir(t_strdup_until(child_path, p)) == 0) {
				/* try again */
				if (fs_list_rmdir(list, name, path) == 0)
					return 0;
			}
		}

		mailbox_list_set_error(list, MAIL_ERROR_EXISTS,
			"Mailbox has children, delete them first");
	} else {
		mailbox_list_set_critical(list, "rmdir(%s) failed: %m", path);
	}
	return -1;
}

static int rename_dir(struct mailbox_list *oldlist, const char *oldname,
		      struct mailbox_list *newlist, const char *newname,
		      enum mailbox_list_path_type type, bool rmdir_parent)
{
	struct stat st;
	const char *oldpath, *newpath, *p, *oldparent, *newparent;

	oldpath = mailbox_list_get_path(oldlist, oldname, type);
	newpath = mailbox_list_get_path(newlist, newname, type);

	if (strcmp(oldpath, newpath) == 0)
		return 0;

	p = strrchr(oldpath, '/');
	oldparent = p == NULL ? "/" : t_strdup_until(oldpath, p);
	p = strrchr(newpath, '/');
	newparent = p == NULL ? "/" : t_strdup_until(newpath, p);

	if (strcmp(oldparent, newparent) != 0 && stat(oldpath, &st) == 0) {
		/* make sure the newparent exists */
		struct mailbox_permissions perm;

		mailbox_list_get_root_permissions(newlist, &perm);
		if (mkdir_parents_chgrp(newparent, perm.dir_create_mode,
					perm.file_create_gid,
					perm.file_create_gid_origin) < 0 &&
		    errno != EEXIST) {
			if (mailbox_list_set_error_from_errno(oldlist))
				return -1;

			mailbox_list_set_critical(oldlist,
				"mkdir_parents(%s) failed: %m", newparent);
			return -1;
		}
	}

	if (rename(oldpath, newpath) < 0 && errno != ENOENT) {
		mailbox_list_set_critical(oldlist, "rename(%s, %s) failed: %m",
					  oldpath, newpath);
		return -1;
	}
	if (rmdir_parent && (p = strrchr(oldpath, '/')) != NULL) {
		oldpath = t_strdup_until(oldpath, p);
		if (rmdir(oldpath) < 0 && errno != ENOENT &&
		    errno != ENOTEMPTY && errno != EEXIST) {
			mailbox_list_set_critical(oldlist,
				"rmdir(%s) failed: %m", oldpath);
		}
	}

	/* avoid leaving empty directories lying around */
	mailbox_list_delete_until_root(oldlist, oldpath, type);
	return 0;
}

static int fs_list_rename_mailbox(struct mailbox_list *oldlist,
				  const char *oldname,
				  struct mailbox_list *newlist,
				  const char *newname)
{
	struct mail_storage *oldstorage;
	const char *oldvname, *oldpath, *newpath, *alt_newpath, *root_path, *p;
	struct stat st;
	struct mailbox_permissions old_perm, new_perm;
	bool rmdir_parent = FALSE;

	oldvname = mailbox_list_get_vname(oldlist, oldname);
	if (mailbox_list_get_storage(&oldlist, oldvname, &oldstorage) < 0)
		return -1;

	oldpath = mailbox_list_get_path(oldlist, oldname,
					MAILBOX_LIST_PATH_TYPE_DIR);
	newpath = mailbox_list_get_path(newlist, newname,
					MAILBOX_LIST_PATH_TYPE_DIR);
	alt_newpath = mailbox_list_get_path(newlist, newname,
					    MAILBOX_LIST_PATH_TYPE_ALT_DIR);

	root_path = mailbox_list_get_root_path(oldlist, MAILBOX_LIST_PATH_TYPE_MAILBOX);
	if (strcmp(oldpath, root_path) == 0) {
		/* most likely INBOX */
		mailbox_list_set_error(oldlist, MAIL_ERROR_NOTPOSSIBLE,
			t_strdup_printf("Renaming %s isn't supported.",
					oldname));
		return -1;
	}

	mailbox_list_get_permissions(oldlist, oldname, &old_perm);
	mailbox_list_get_permissions(newlist, newname, &new_perm);

	/* if we're renaming under another mailbox, require its permissions
	   to be same as ours. */
	if (strchr(newname, mailbox_list_get_hierarchy_sep(newlist)) != NULL &&
	    (new_perm.file_create_mode != old_perm.file_create_mode ||
	     new_perm.dir_create_mode != old_perm.dir_create_mode ||
	     new_perm.file_create_gid != old_perm.file_create_gid)) {
		mailbox_list_set_error(oldlist, MAIL_ERROR_NOTPOSSIBLE,
			"Renaming not supported across conflicting "
			"directory permissions");
		return -1;
	}

	/* create the hierarchy */
	p = strrchr(newpath, '/');
	if (p != NULL) {
		p = t_strdup_until(newpath, p);
		if (mkdir_parents_chgrp(p, new_perm.dir_create_mode,
					new_perm.file_create_gid,
					new_perm.file_create_gid_origin) < 0 &&
		    errno != EEXIST) {
			if (mailbox_list_set_error_from_errno(oldlist))
				return -1;

			mailbox_list_set_critical(oldlist,
				"mkdir_parents(%s) failed: %m", p);
			return -1;
		}
	}

	/* first check that the destination mailbox doesn't exist.
	   this is racy, but we need to be atomic and there's hardly any
	   possibility that someone actually tries to rename two mailboxes
	   to same new one */
	if (lstat(newpath, &st) == 0) {
		mailbox_list_set_error(oldlist, MAIL_ERROR_EXISTS,
				       "Target mailbox already exists");
		return -1;
	} else if (errno == ENOTDIR) {
		mailbox_list_set_error(oldlist, MAIL_ERROR_NOTPOSSIBLE,
			"Target mailbox doesn't allow inferior mailboxes");
		return -1;
	} else if (errno != ENOENT && errno != EACCES) {
		mailbox_list_set_critical(oldlist, "lstat(%s) failed: %m",
					  newpath);
		return -1;
	}

	if (alt_newpath != NULL) {
		if (stat(alt_newpath, &st) == 0) {
			/* race condition or a directory left there lying around?
			   safest to just report error. */
			mailbox_list_set_error(oldlist, MAIL_ERROR_EXISTS,
					       "Target mailbox already exists");
			return -1;
		} else if (errno != ENOENT) {
			mailbox_list_set_critical(oldlist, "stat(%s) failed: %m",
						  alt_newpath);
			return -1;
		}
	}

	if (rename(oldpath, newpath) < 0) {
		if (ENOTFOUND(errno)) {
			mailbox_list_set_error(oldlist, MAIL_ERROR_NOTFOUND,
				T_MAIL_ERR_MAILBOX_NOT_FOUND(oldname));
		} else if (!mailbox_list_set_error_from_errno(oldlist)) {
			mailbox_list_set_critical(oldlist,
				"rename(%s, %s) failed: %m", oldpath, newpath);
		}
		return -1;
	}

	if (alt_newpath != NULL) {
		(void)rename_dir(oldlist, oldname, newlist, newname,
				 MAILBOX_LIST_PATH_TYPE_ALT_DIR, rmdir_parent);
	}
	(void)rename_dir(oldlist, oldname, newlist, newname,
			 MAILBOX_LIST_PATH_TYPE_CONTROL, rmdir_parent);
	(void)rename_dir(oldlist, oldname, newlist, newname,
			 MAILBOX_LIST_PATH_TYPE_INDEX, rmdir_parent);
	return 0;
}

struct mailbox_list fs_mailbox_list = {
	.name = MAILBOX_LIST_NAME_FS,
	.props = 0,
	.mailbox_name_max_length = MAILBOX_LIST_NAME_MAX_LENGTH,

	{
		fs_list_alloc,
		fs_list_deinit,
		NULL,
		fs_list_get_hierarchy_sep,
		mailbox_list_default_get_vname,
		mailbox_list_default_get_storage_name,
		fs_list_get_path,
		fs_list_get_temp_prefix,
		fs_list_join_refpattern,
		fs_list_iter_init,
		fs_list_iter_next,
		fs_list_iter_deinit,
		fs_list_get_mailbox_flags,
		NULL,
		mailbox_list_subscriptions_refresh,
		fs_list_set_subscribed,
		fs_list_delete_mailbox,
		fs_list_delete_dir,
		mailbox_list_delete_symlink_default,
		fs_list_rename_mailbox,
		NULL, NULL, NULL, NULL
	}
};
