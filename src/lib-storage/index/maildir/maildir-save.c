/* Copyright (C) 2002 Timo Sirainen */

#include "lib.h"
#include "ioloop.h"
#include "hostpid.h"
#include "ostream.h"
#include "maildir-index.h"
#include "maildir-storage.h"

#include <stdio.h>
#include <unistd.h>
#include <fcntl.h>
#include <utime.h>

const char *maildir_generate_tmp_filename(void)
{
	static unsigned int create_count = 0;

	hostpid_init();

	return t_strdup_printf("%lu.%s_%u.%s", (unsigned long) ioloop_time,
			       my_pid, create_count++, my_hostname);
}

static int maildir_create_tmp(MailStorage *storage, const char *dir,
			      const char **fname)
{
	const char *path;
	int fd;

	*fname = maildir_generate_tmp_filename();

	path = t_strconcat(dir, "/", *fname, NULL);
	fd = open(path, O_WRONLY | O_CREAT | O_EXCL, 0660);
	if (fd == -1) {
		if (errno == ENOSPC) {
			mail_storage_set_error(storage,
					       "Not enough disk space");
		} else {
			/* don't bother checking if it was because file
			   existed - if that happens it's itself an error. */
			mail_storage_set_critical(storage, "Can't create file "
						  "%s: %m", path);
		}
	}

	return fd;
}

static const char *maildir_read_into_tmp(MailStorage *storage, const char *dir,
					 IStream *input, uoff_t data_size)
{
	const char *fname, *path;
	OStream *output;
	int fd;

	fd = maildir_create_tmp(storage, dir, &fname);
	if (fd == -1)
		return NULL;

	t_push();
	output = o_stream_create_file(fd, data_stack_pool, 4096,
				      IO_PRIORITY_DEFAULT, FALSE);
	o_stream_set_blocking(output, 60000, NULL, NULL);

	path = t_strconcat(dir, "/", fname, NULL);
	if (!index_storage_save(storage, path, input, output, data_size))
		fname = NULL;

	o_stream_unref(output);
	if (close(fd) < 0)
		fname = NULL;

	if (fname == NULL)
		(void)unlink(path);
	t_pop();
	return fname;
}

int maildir_storage_save(Mailbox *box, MailFlags flags,
			 const char *custom_flags[], time_t internal_date,
			 int timezone_offset __attr_unused__,
			 IStream *data, uoff_t data_size)
{
        IndexMailbox *ibox = (IndexMailbox *) box;
        struct utimbuf buf;
	const char *tmpdir, *fname, *tmp_path, *new_path;
	int failed;

	if (box->readonly) {
		mail_storage_set_error(box->storage, "Mailbox is read-only");
		return FALSE;
	}

	if (!index_mailbox_fix_custom_flags(ibox, &flags, custom_flags))
		return FALSE;

	t_push();

	/* create the file into tmp/ directory */
	tmpdir = t_strconcat(ibox->index->dir, "/tmp", NULL);
	fname = maildir_read_into_tmp(box->storage, tmpdir, data, data_size);
	if (fname == NULL) {
		t_pop();
		return FALSE;
	}
	tmp_path = t_strconcat(tmpdir, "/", fname, NULL);

	fname = maildir_filename_set_flags(fname, flags);
	new_path = t_strconcat(ibox->index->dir, "/new/", fname, NULL);

	/* set the internal_date by modifying mtime */
	buf.actime = ioloop_time;
	buf.modtime = internal_date;
	if (utime(tmp_path, &buf) < 0) {
		/* just warn, don't bother actually failing */
		mail_storage_set_critical(box->storage, "utime() failed for "
					  "%s: %m", tmp_path);
	}

	/* move the file into new/ directory - syncing will pick it
	   up from there */
	if (rename(tmp_path, new_path) == 0)
		failed = FALSE;
	else {
		if (errno == ENOSPC) {
			mail_storage_set_error(box->storage,
					       "Not enough disk space");
		} else {
			mail_storage_set_critical(box->storage,
						  "rename(%s, %s) failed: %m",
						  tmp_path, new_path);
		}

		(void)unlink(tmp_path);
		failed = TRUE;
	}

	t_pop();
	return !failed;
}
