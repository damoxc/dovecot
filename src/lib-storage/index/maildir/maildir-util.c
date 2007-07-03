/* Copyright (C) 2004 Timo Sirainen */

#include "lib.h"
#include "hostpid.h"
#include "ioloop.h"
#include "str.h"
#include "maildir-storage.h"
#include "maildir-uidlist.h"
#include "maildir-keywords.h"
#include "maildir-sync.h"

#include <unistd.h>
#include <dirent.h>
#include <fcntl.h>
#include <utime.h>
#include <sys/stat.h>

static int maildir_file_do_try(struct maildir_mailbox *mbox, uint32_t uid,
			       maildir_file_do_func *callback, void *context)
{
	const char *fname, *path;
        enum maildir_uidlist_rec_flag flags;
	int ret;

	fname = maildir_uidlist_lookup(mbox->uidlist, uid, &flags);
	if (fname == NULL)
		return -2; /* expunged */

	t_push();
	if ((flags & MAILDIR_UIDLIST_REC_FLAG_NEW_DIR) != 0) {
		/* probably in new/ dir */
		path = t_strconcat(mbox->path, "/new/", fname, NULL);
		ret = callback(mbox, path, context);
		if (ret != 0) {
			t_pop();
			return ret;
		}
	}

	path = t_strconcat(mbox->path, "/cur/", fname, NULL);
	ret = callback(mbox, path, context);
	t_pop();
	return ret;
}

static int do_racecheck(struct maildir_mailbox *mbox, const char *path,
			void *context __attr_unused__)
{
	struct stat st;

	if (lstat(path, &st) == 0 && (st.st_mode & S_IFLNK) != 0) {
		/* most likely a symlink pointing to a non-existing file */
		mail_storage_set_critical(&mbox->storage->storage,
			"Maildir: Symlink destination doesn't exist: %s", path);
		return -2;
	} else {
		mail_storage_set_critical(&mbox->storage->storage,
			"maildir_file_do(%s): Filename keeps changing", path);
		return -1;
	}
}

#undef maildir_file_do
int maildir_file_do(struct maildir_mailbox *mbox, uint32_t uid,
		    maildir_file_do_func *callback, void *context)
{
	int i, ret;

	ret = maildir_file_do_try(mbox, uid, callback, context);
	for (i = 0; i < 10 && ret == 0; i++) {
		/* file is either renamed or deleted. sync the maildir and
		   see which one. if file appears to be renamed constantly,
		   don't try to open it more than 10 times. */
		if (maildir_storage_sync_force(mbox) < 0)
			return -1;

		ret = maildir_file_do_try(mbox, uid, callback, context);
	}

	if (i == 10)
		ret = maildir_file_do_try(mbox, uid, do_racecheck, context);

	return ret == -2 ? 0 : ret;
}

const char *maildir_generate_tmp_filename(const struct timeval *tv)
{
	static unsigned int create_count = 0;
	static time_t first_stamp = 0;

	if (first_stamp == 0 || first_stamp == ioloop_time) {
		/* it's possible that within last second another process had
		   the same PID as us. Use usecs to make sure we don't create
		   duplicate base name. */
		first_stamp = ioloop_time;
		return t_strdup_printf("%s.P%sQ%uM%s.%s",
				       dec2str(tv->tv_sec), my_pid,
				       create_count++,
				       dec2str(tv->tv_usec), my_hostname);
	} else {
		/* Don't bother with usecs. Saves a bit space :) */
		return t_strdup_printf("%s.P%sQ%u.%s",
				       dec2str(tv->tv_sec), my_pid,
				       create_count++, my_hostname);
	}
}

int maildir_create_tmp(struct maildir_mailbox *mbox, const char *dir,
		       mode_t mode, const char **fname_r)
{
	struct stat st;
	struct timeval *tv, tv_now;
	unsigned int prefix_len;
	const char *tmp_fname = NULL;
	string_t *path;
	int fd;

	tv = &ioloop_timeval;
	path = t_str_new(256);
	str_append(path, dir);
	str_append_c(path, '/');
	prefix_len = str_len(path);

	for (;;) {
		tmp_fname = maildir_generate_tmp_filename(tv);
		str_truncate(path, prefix_len);
		str_append(path, tmp_fname);

		/* stat() first to see if it exists. pretty much the only
		   possibility of that happening is if time had moved
		   backwards, but even then it's highly unlikely. */
		if (stat(str_c(path), &st) < 0 && errno == ENOENT) {
			/* doesn't exist */
			mode_t old_mask = umask(0);
			fd = open(str_c(path), O_WRONLY | O_CREAT | O_EXCL,
				  mode);
			umask(old_mask);
			if (fd != -1 || errno != EEXIST)
				break;
		}

		sleep(2);
		tv = &tv_now;
		if (gettimeofday(&tv_now, NULL) < 0)
			i_fatal("gettimeofday(): %m");
	}

	*fname_r = tmp_fname;
	if (fd == -1) {
		if (ENOSPACE(errno)) {
			mail_storage_set_error(&mbox->storage->storage,
				MAIL_ERROR_NOSPACE, MAIL_ERRSTR_NO_SPACE);
		} else {
			mail_storage_set_critical(&mbox->storage->storage,
				"open(%s) failed: %m", str_c(path));
		}
	} else if (mbox->mail_create_gid != (gid_t)-1) {
		if (fchown(fd, (uid_t)-1, mbox->mail_create_gid) < 0) {
			mail_storage_set_critical(&mbox->storage->storage,
				"fchown(%s) failed: %m", str_c(path));
		}
	}

	return fd;
}

void maildir_tmp_cleanup(struct mail_storage *storage, const char *dir)
{
	DIR *dirp;
	struct dirent *d;
	struct stat st;
	string_t *path;
	unsigned int dir_len;

	dirp = opendir(dir);
	if (dirp == NULL) {
		if (errno != ENOENT) {
			mail_storage_set_critical(storage,
				"opendir(%s) failed: %m", dir);
		}
		return;
	}

	t_push();
	path = t_str_new(256);
	str_printfa(path, "%s/", dir);
	dir_len = str_len(path);

	while ((d = readdir(dirp)) != NULL) {
		if (d->d_name[0] == '.' &&
		    (d->d_name[1] == '\0' ||
		     (d->d_name[1] == '.' && d->d_name[2] == '\0'))) {
			/* skip . and .. */
			continue;
		}

		str_truncate(path, dir_len);
		str_append(path, d->d_name);
		if (stat(str_c(path), &st) < 0) {
			if (errno != ENOENT) {
				mail_storage_set_critical(storage,
					"stat(%s) failed: %m", str_c(path));
			}
		} else if (st.st_ctime <=
			   ioloop_time - MAILDIR_TMP_DELETE_SECS) {
			if (unlink(str_c(path)) < 0 && errno != ENOENT) {
				mail_storage_set_critical(storage,
					"unlink(%s) failed: %m", str_c(path));
			}
		}
	}
	t_pop();

#ifdef HAVE_DIRFD
	if (fstat(dirfd(dirp), &st) < 0) {
		mail_storage_set_critical(storage,
			"fstat(%s) failed: %m", dir);
	}
#else
	if (stat(dir, &st) < 0) {
		mail_storage_set_critical(storage,
			"stat(%s) failed: %m", dir);
	}
#endif
	else if (st.st_atime < ioloop_time) {
		/* mounted with noatime. update it ourself. */
		if (utime(dir, NULL) < 0 && errno != ENOENT) {
			mail_storage_set_critical(storage,
				"utime(%s) failed: %m", dir);
		}
	}

	if (closedir(dirp) < 0) {
		mail_storage_set_critical(storage,
			"closedir(%s) failed: %m", dir);
	}
}

bool maildir_filename_get_size(const char *fname, char type, uoff_t *size_r)
{
	uoff_t size = 0;

	for (; *fname != '\0'; fname++) {
		i_assert(*fname != '/');
		if (*fname == ',' && fname[1] == type && fname[2] == '=') {
			fname += 3;
			break;
		}
	}

	if (*fname == '\0')
		return FALSE;

	while (*fname >= '0' && *fname <= '9') {
		size = size * 10 + (*fname - '0');
		fname++;
	}

	if (*fname != MAILDIR_INFO_SEP &&
	    *fname != MAILDIR_EXTRA_SEP &&
	    *fname != '\0')
		return FALSE;

	*size_r = size;
	return TRUE;
}

/* a char* hash function from ASU -- from glib */
unsigned int maildir_hash(const void *p)
{
        const unsigned char *s = p;
	unsigned int g, h = 0;

	while (*s != MAILDIR_INFO_SEP && *s != '\0') {
		i_assert(*s != '/');
		h = (h << 4) + *s;
		if ((g = h & 0xf0000000UL)) {
			h = h ^ (g >> 24);
			h = h ^ g;
		}

		s++;
	}

	return h;
}

int maildir_cmp(const void *p1, const void *p2)
{
	const char *s1 = p1, *s2 = p2;

	while (*s1 == *s2 && *s1 != MAILDIR_INFO_SEP && *s1 != '\0') {
		i_assert(*s1 != '/');
		s1++; s2++;
	}
	if ((*s1 == '\0' || *s1 == MAILDIR_INFO_SEP) &&
	    (*s2 == '\0' || *s2 == MAILDIR_INFO_SEP))
		return 0;
	return *s1 - *s2;
}
