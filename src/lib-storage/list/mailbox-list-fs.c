/* Copyright (c) 2006-2009 Dovecot authors, see the included COPYING file */

#include "lib.h"
#include "hostpid.h"
#include "mkdir-parents.h"
#include "subscription-file.h"
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

	pool = pool_alloconly_create("fs list", 1024);

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

static bool fs_list_is_valid_common(const char *name, size_t *len_r)
{
	*len_r = strlen(name);

	if (name[0] == '\0' || name[*len_r-1] == '/')
		return FALSE;
	return TRUE;
}

static bool
fs_list_is_valid_common_nonfs(struct mailbox_list *list, const char *name)
{
	const char *p;
	bool newdir;
	size_t maildir_len;

	/* make sure it's not absolute path */
	if (*name == '/' || *name == '~')
		return FALSE;

	/* make sure the mailbox name doesn't contain any foolishness:
	   "../" could give access outside the mailbox directory.
	   "./" and "//" could fool ACL checks. */
	newdir = TRUE;
	maildir_len = strlen(list->set.maildir_name);
	for (p = name; *p != '\0'; p++) {
		if (newdir) {
			if (p[0] == '/')
				return FALSE; /* // */
			if (p[0] == '.') {
				if (p[1] == '/' || p[1] == '\0')
					return FALSE; /* ./ */
				if (p[1] == '.' &&
				    (p[2] == '/' || p[2] == '\0'))
					return FALSE; /* ../ */
			}
			if (maildir_len > 0 &&
			    strncmp(p, list->set.maildir_name,
				    maildir_len) == 0 &&
			    (p[maildir_len-1] == '\0' ||
			     p[maildir_len-1] == '/')) {
				/* don't allow maildir_name to be used as part
				   of the mailbox name */
				return FALSE;
			}
		} 
		newdir = p[0] == '/';
	}
	if (name[0] == '.' && (name[1] == '\0' ||
			       (name[1] == '.' && name[2] == '\0'))) {
		/* "." and ".." aren't allowed. */
		return FALSE;
	}

	return TRUE;
}

static bool
fs_is_valid_pattern(struct mailbox_list *list, const char *pattern)
{
	if ((list->flags & MAILBOX_LIST_FLAG_FULL_FS_ACCESS) != 0)
		return TRUE;

	return fs_list_is_valid_common_nonfs(list, pattern);
}

static bool
fs_is_valid_existing_name(struct mailbox_list *list, const char *name)
{
	size_t len;

	if (!fs_list_is_valid_common(name, &len))
		return FALSE;

	if ((list->flags & MAILBOX_LIST_FLAG_FULL_FS_ACCESS) != 0)
		return TRUE;

	return fs_list_is_valid_common_nonfs(list, name);
}

static bool
fs_is_valid_create_name(struct mailbox_list *list, const char *name)
{
	size_t len;

	if (!fs_list_is_valid_common(name, &len))
		return FALSE;
	if (len > FS_MAX_CREATE_MAILBOX_NAME_LENGTH)
		return FALSE;

	if ((list->flags & MAILBOX_LIST_FLAG_FULL_FS_ACCESS) != 0)
		return TRUE;

	if (mailbox_list_name_is_too_large(name, '/'))
		return FALSE;
	return fs_list_is_valid_common_nonfs(list, name);
}

static const char *
fs_list_get_path(struct mailbox_list *_list, const char *name,
		 enum mailbox_list_path_type type)
{
	const struct mailbox_list_settings *set = &_list->set;

	if (name == NULL) {
		/* return root directories */
		switch (type) {
		case MAILBOX_LIST_PATH_TYPE_DIR:
			return set->root_dir;
		case MAILBOX_LIST_PATH_TYPE_MAILBOX:
			return t_strconcat(set->root_dir, "/",
					   set->mailbox_dir_name, NULL);
		case MAILBOX_LIST_PATH_TYPE_CONTROL:
			return set->control_dir != NULL ?
				set->control_dir : set->root_dir;
		case MAILBOX_LIST_PATH_TYPE_INDEX:
			return set->index_dir != NULL ?
				set->index_dir : set->root_dir;
		}
		i_unreached();
	}

	i_assert(mailbox_list_is_valid_pattern(_list, name));

	if (mailbox_list_try_get_absolute_path(_list, &name))
		return name;

	switch (type) {
	case MAILBOX_LIST_PATH_TYPE_DIR:
		if (*set->maildir_name != '\0')
			return t_strdup_printf("%s/%s%s", set->root_dir,
					       set->mailbox_dir_name, name);
		break;
	case MAILBOX_LIST_PATH_TYPE_MAILBOX:
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
	}

	/* If INBOX is a file, index and control directories are located
	   in root directory. */
	if (strcmp(name, "INBOX") == 0 && set->inbox_path != NULL &&
	    ((_list->flags & MAILBOX_LIST_FLAG_MAILBOX_FILES) == 0 ||
	     type == MAILBOX_LIST_PATH_TYPE_MAILBOX ||
	     type == MAILBOX_LIST_PATH_TYPE_DIR))
		return set->inbox_path;

	if (*set->maildir_name == '\0') {
		return t_strdup_printf("%s/%s%s", set->root_dir,
				       set->mailbox_dir_name, name);
	} else {
		return t_strdup_printf("%s/%s%s/%s", set->root_dir,
				       set->mailbox_dir_name, name,
				       set->maildir_name);
	}
}

static int
fs_list_get_mailbox_name_status(struct mailbox_list *_list, const char *name,
				enum mailbox_name_status *status)
{
	struct stat st;
	const char *path;

	path = mailbox_list_get_path(_list, name,
				     MAILBOX_LIST_PATH_TYPE_MAILBOX);

	if (strcmp(name, "INBOX") == 0 || stat(path, &st) == 0) {
		*status = MAILBOX_NAME_EXISTS;
		return 0;
	}

	if (!mailbox_list_is_valid_create_name(_list, name)) {
		*status = MAILBOX_NAME_INVALID;
		return 0;
	}

	if (ENOTFOUND(errno) || errno == EACCES) {
		*status = MAILBOX_NAME_VALID;
		return 0;
	} else if (errno == ENOTDIR) {
		*status = MAILBOX_NAME_NOINFERIORS;
		return 0;
	} else {
		mailbox_list_set_critical(_list, "stat(%s) failed: %m", path);
		return -1;
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

static int fs_list_delete_mailbox(struct mailbox_list *list, const char *name)
{
	/* let the backend handle the rest */
	return mailbox_list_delete_index_control(list, name);
}

static int fs_list_rename_mailbox(struct mailbox_list *list,
				  const char *oldname, const char *newname)
{
	const char *oldpath, *newpath, *old_indexdir, *new_indexdir, *p;
	struct stat st;
	mode_t mode;
	gid_t gid;

	oldpath = mailbox_list_get_path(list, oldname,
					MAILBOX_LIST_PATH_TYPE_DIR);
	newpath = mailbox_list_get_path(list, newname,
					MAILBOX_LIST_PATH_TYPE_DIR);

	/* create the hierarchy */
	p = strrchr(newpath, '/');
	if (p != NULL) {
		mailbox_list_get_dir_permissions(list, NULL, &mode, &gid);
		p = t_strdup_until(newpath, p);
		if (mkdir_parents_chown(p, mode, (uid_t)-1, gid) < 0 &&
		    errno != EEXIST) {
			if (mailbox_list_set_error_from_errno(list))
				return -1;

			mailbox_list_set_critical(list,
				"mkdir_parents(%s) failed: %m", p);
			return -1;
		}
	}

	/* first check that the destination mailbox doesn't exist.
	   this is racy, but we need to be atomic and there's hardly any
	   possibility that someone actually tries to rename two mailboxes
	   to same new one */
	if (lstat(newpath, &st) == 0) {
		mailbox_list_set_error(list, MAIL_ERROR_EXISTS,
				       "Target mailbox already exists");
		return -1;
	} else if (errno == ENOTDIR) {
		mailbox_list_set_error(list, MAIL_ERROR_NOTPOSSIBLE,
			"Target mailbox doesn't allow inferior mailboxes");
		return -1;
	} else if (errno != ENOENT && errno != EACCES) {
		mailbox_list_set_critical(list, "lstat(%s) failed: %m",
					  newpath);
		return -1;
	}

	if (list->v.rename_mailbox_pre != NULL) {
		if (list->v.rename_mailbox_pre(list, oldname, newname) < 0)
			return -1;
	}

	/* NOTE: renaming INBOX works just fine with us, it's simply recreated
	   the next time it's needed. */
	if (rename(oldpath, newpath) < 0) {
		if (ENOTFOUND(errno)) {
			mailbox_list_set_error(list, MAIL_ERROR_NOTFOUND,
				T_MAIL_ERR_MAILBOX_NOT_FOUND(oldname));
		} else if (!mailbox_list_set_error_from_errno(list)) {
			mailbox_list_set_critical(list,
				"rename(%s, %s) failed: %m", oldpath, newpath);
		}
		return -1;
	}

	/* we need to rename the index directory as well */
	old_indexdir = mailbox_list_get_path(list, oldname,
					     MAILBOX_LIST_PATH_TYPE_INDEX);
	new_indexdir = mailbox_list_get_path(list, newname,
					     MAILBOX_LIST_PATH_TYPE_INDEX);
	if (*old_indexdir != '\0') {
		if (rename(old_indexdir, new_indexdir) < 0 &&
		    errno != ENOENT) {
			mailbox_list_set_critical(list,
						  "rename(%s, %s) failed: %m",
						  old_indexdir, new_indexdir);
		}
	}

	return 0;
}

struct mailbox_list fs_mailbox_list = {
	MEMBER(name) "fs",
	MEMBER(hierarchy_sep) '/',
	MEMBER(props) 0,
	MEMBER(mailbox_name_max_length) PATH_MAX,

	{
		fs_list_alloc,
		fs_list_deinit,
		fs_is_valid_pattern,
		fs_is_valid_existing_name,
		fs_is_valid_create_name,
		fs_list_get_path,
		fs_list_get_mailbox_name_status,
		fs_list_get_temp_prefix,
		fs_list_join_refpattern,
		fs_list_iter_init,
		fs_list_iter_next,
		fs_list_iter_deinit,
		NULL,
		fs_list_set_subscribed,
		fs_list_delete_mailbox,
		fs_list_rename_mailbox,
		NULL
	}
};
