/* Copyright (c) 2007-2011 Dovecot authors, see the included COPYING file */

#include "lib.h"
#include "istream.h"
#include "index-mail.h"
#include "cydir-storage.h"

#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>

static const char *cydir_mail_get_path(struct mail *mail)
{
	const char *dir;

	dir = mailbox_list_get_path(mail->box->list, mail->box->name,
				    MAILBOX_LIST_PATH_TYPE_MAILBOX);
	return t_strdup_printf("%s/%u.", dir, mail->uid);
}

static int cydir_mail_stat(struct mail *mail, struct stat *st_r)
{
	struct mail_private *p = (struct mail_private *)mail;
	const char *path;

	if (mail->lookup_abort == MAIL_LOOKUP_ABORT_NOT_IN_CACHE)
		return mail_set_aborted(mail);

	p->stats_stat_lookup_count++;
	path = cydir_mail_get_path(mail);
	if (stat(path, st_r) < 0) {
		if (errno == ENOENT)
			mail_set_expunged(mail);
		else {
			mail_storage_set_critical(mail->box->storage,
						  "stat(%s) failed: %m", path);
		}
		return -1;
	}
	return 0;
}

static int cydir_mail_get_received_date(struct mail *_mail, time_t *date_r)
{
	struct index_mail *mail = (struct index_mail *)_mail;
	struct index_mail_data *data = &mail->data;
	struct stat st;

	if (index_mail_get_received_date(_mail, date_r) == 0)
		return 0;

	if (cydir_mail_stat(_mail, &st) < 0)
		return -1;

	data->received_date = st.st_mtime;
	*date_r = data->received_date;
	return 0;
}

static int cydir_mail_get_save_date(struct mail *_mail, time_t *date_r)
{
	struct index_mail *mail = (struct index_mail *)_mail;
	struct index_mail_data *data = &mail->data;
	struct stat st;

	if (index_mail_get_save_date(_mail, date_r) == 0)
		return 0;

	if (cydir_mail_stat(_mail, &st) < 0)
		return -1;

	data->save_date = st.st_ctime;
	*date_r = data->save_date;
	return 0;
}

static int cydir_mail_get_physical_size(struct mail *_mail, uoff_t *size_r)
{
	struct index_mail *mail = (struct index_mail *)_mail;
	struct index_mail_data *data = &mail->data;
	struct stat st;

	if (index_mail_get_physical_size(_mail, size_r) == 0)
		return 0;

	if (cydir_mail_stat(_mail, &st) < 0)
		return -1;

	data->physical_size = data->virtual_size = st.st_size;
	*size_r = data->physical_size;
	return 0;
}

static int
cydir_mail_get_stream(struct mail *_mail, struct message_size *hdr_size,
		      struct message_size *body_size, struct istream **stream_r)
{
	struct index_mail *mail = (struct index_mail *)_mail;
	const char *path;
	int fd;

	if (mail->data.stream == NULL) {
		mail->mail.stats_open_lookup_count++;
		path = cydir_mail_get_path(_mail);
		fd = open(path, O_RDONLY);
		if (fd == -1) {
			if (errno == ENOENT)
				mail_set_expunged(_mail);
			else {
				mail_storage_set_critical(_mail->box->storage,
					"open(%s) failed: %m", path);
			}
			return -1;
		}
		mail->data.stream = i_stream_create_fd(fd, 0, TRUE);
		i_stream_set_name(mail->data.stream, path);
		index_mail_set_read_buffer_size(_mail, mail->data.stream);
		if (mail->mail.v.istream_opened != NULL) {
			if (mail->mail.v.istream_opened(_mail, stream_r) < 0)
				return -1;
		}
	}

	return index_mail_init_stream(mail, hdr_size, body_size, stream_r);
}

struct mail_vfuncs cydir_mail_vfuncs = {
	index_mail_close,
	index_mail_free,
	index_mail_set_seq,
	index_mail_set_uid,
	index_mail_set_uid_cache_updates,

	index_mail_get_flags,
	index_mail_get_keywords,
	index_mail_get_keyword_indexes,
	index_mail_get_modseq,
	index_mail_get_parts,
	index_mail_get_date,
	cydir_mail_get_received_date,
	cydir_mail_get_save_date,
	cydir_mail_get_physical_size, /* physical = virtual in our case */
	cydir_mail_get_physical_size,
	index_mail_get_first_header,
	index_mail_get_headers,
	index_mail_get_header_stream,
	cydir_mail_get_stream,
	index_mail_get_special,
	index_mail_get_real_mail,
	index_mail_update_flags,
	index_mail_update_keywords,
	index_mail_update_modseq,
	NULL,
	index_mail_expunge,
	index_mail_parse,
	index_mail_set_cache_corrupted,
	index_mail_opened
};
