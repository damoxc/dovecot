/* Copyright (c) 2003-2008 Dovecot authors, see the included COPYING file */

#include "lib.h"
#include "ioloop.h"
#include "istream.h"
#include "index-mail.h"
#include "mbox-storage.h"
#include "mbox-file.h"
#include "mbox-lock.h"
#include "mbox-sync-private.h"
#include "istream-raw-mbox.h"
#include "istream-header-filter.h"

#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>

static void mbox_prepare_resync(struct index_mail *mail)
{
	struct mbox_transaction_context *t =
		(struct mbox_transaction_context *)mail->trans;
	struct mbox_mailbox *mbox = (struct mbox_mailbox *)mail->ibox;

	if (mbox->mbox_lock_type == F_RDLCK) {
		if (mbox->mbox_lock_id == t->mbox_lock_id)
			t->mbox_lock_id = 0;
		(void)mbox_unlock(mbox, mbox->mbox_lock_id);
		mbox->mbox_lock_id = 0;
		i_assert(mbox->mbox_lock_type == F_UNLCK);
	}
}

static int mbox_mail_seek(struct index_mail *mail)
{
	struct mbox_transaction_context *t =
		(struct mbox_transaction_context *)mail->trans;
	struct mbox_mailbox *mbox = (struct mbox_mailbox *)mail->ibox;
	enum mbox_sync_flags sync_flags = 0;
	int ret, try;
	bool deleted;

	if (mail->mail.mail.expunged || mbox->syncing)
		return -1;

	for (try = 0; try < 2; try++) {
		if (mbox->mbox_lock_type == F_UNLCK) {
			sync_flags |= MBOX_SYNC_LOCK_READING;
			if (mbox_sync(mbox, sync_flags) < 0)
				return -1;

			/* refresh index file after mbox has been locked to
			   make sure we get only up-to-date mbox offsets. */
			if (mail_index_refresh(mbox->ibox.index) < 0) {
				mail_storage_set_index_error(&mbox->ibox);
				return -1;
			}

			i_assert(mbox->mbox_lock_type != F_UNLCK);
			t->mbox_lock_id = mbox->mbox_lock_id;
		} else if ((sync_flags & MBOX_SYNC_FORCE_SYNC) != 0) {
			/* dirty offsets are broken and mbox is write-locked.
			   sync it to update offsets. */
			if (mbox_sync(mbox, sync_flags) < 0)
				return -1;
		}

		if (mbox_file_open_stream(mbox) < 0)
			return -1;

		ret = mbox_file_seek(mbox, mail->trans->trans_view,
				     mail->mail.mail.seq, &deleted);
		if (ret > 0) {
			/* success */
			break;
		}
		if (ret < 0) {
			if (deleted)
				mail_set_expunged(&mail->mail.mail);
			return -1;
		}

		/* we'll need to re-sync it completely */
		mbox_prepare_resync(mail);
		sync_flags |= MBOX_SYNC_UNDIRTY | MBOX_SYNC_FORCE_SYNC;
	}
	if (ret == 0) {
		mail_storage_set_critical(&mbox->storage->storage,
			"Losing sync for mail uid=%u in mbox file %s",
			mail->mail.mail.uid, mbox->path);
	}
	return 0;
}

static int mbox_mail_get_received_date(struct mail *_mail, time_t *date_r)
{
	struct index_mail *mail = (struct index_mail *)_mail;
	struct index_mail_data *data = &mail->data;
	struct mbox_mailbox *mbox = (struct mbox_mailbox *)mail->ibox;

	if (index_mail_get_received_date(_mail, date_r) == 0)
		return 0;

	if (mbox_mail_seek(mail) < 0)
		return -1;
	data->received_date =
		istream_raw_mbox_get_received_time(mbox->mbox_stream);
	if (data->received_date == (time_t)-1) {
		/* it's broken and conflicts with our "not found"
		   return value. change it. */
		data->received_date = 0;
	}

	*date_r = data->received_date;
	return 0;
}

static int mbox_mail_get_save_date(struct mail *_mail, time_t *date_r)
{
	struct index_mail *mail = (struct index_mail *)_mail;
	struct index_mail_data *data = &mail->data;

	if (index_mail_get_save_date(_mail, date_r) == 0)
		return 0;

	/* no way to know this. save the current time into cache and use
	   that from now on. this works only as long as the index files
	   are permanent */
	data->save_date = ioloop_time;
	*date_r = data->save_date;
	return 0;
}

static int
mbox_mail_get_special(struct mail *_mail, enum mail_fetch_field field,
		      const char **value_r)
{
#define EMPTY_MD5_SUM "00000000000000000000000000000000"
	struct index_mail *mail = (struct index_mail *)_mail;
	struct mbox_mailbox *mbox = (struct mbox_mailbox *)mail->ibox;

	switch (field) {
	case MAIL_FETCH_FROM_ENVELOPE:
		if (mbox_mail_seek(mail) < 0)
			return -1;

		*value_r = istream_raw_mbox_get_sender(mbox->mbox_stream);
		return 0;
	case MAIL_FETCH_HEADER_MD5:
		if (index_mail_get_special(_mail, field, value_r) < 0)
			return -1;
		if (**value_r != '\0' && strcmp(*value_r, EMPTY_MD5_SUM) != 0)
			return 0;

		/* i guess in theory the EMPTY_MD5_SUM is valid and can happen,
		   but it's almost guaranteed that it means the MD5 sum is
		   missing. recalculate it. */
		mbox->mbox_save_md5 = TRUE;
                mbox_prepare_resync(mail);
		if (mbox_sync(mbox, MBOX_SYNC_FORCE_SYNC) < 0)
			return -1;
		break;
	default:
		break;
	}

	return index_mail_get_special(_mail, field, value_r);
}

static int mbox_mail_get_physical_size(struct mail *_mail, uoff_t *size_r)
{
	struct index_mail *mail = (struct index_mail *)_mail;
	struct index_mail_data *data = &mail->data;
	struct mbox_mailbox *mbox = (struct mbox_mailbox *)mail->ibox;
	const struct mail_index_header *hdr;
	struct istream *input;
	struct message_size hdr_size;
	uoff_t old_offset, body_offset, body_size, next_offset;

	if (index_mail_get_physical_size(_mail, size_r) == 0)
		return 0;

	/* we want to return the header size as seen by mail_get_stream(). */
	old_offset = data->stream == NULL ? 0 : data->stream->v_offset;
	if (mail_get_stream(_mail, &hdr_size, NULL, &input) < 0)
		return -1;

	/* our header size varies, so don't do any caching */
	body_offset = istream_raw_mbox_get_body_offset(mbox->mbox_stream);
	if (body_offset == (uoff_t)-1) {
		mail_storage_set_critical(_mail->box->storage,
					  "Couldn't get mbox size");
		return -1;
	}

	/* use the next message's offset to avoid reading through the entire
	   message body to find out its size */
	hdr = mail_index_get_header(mail->trans->trans_view);
	if (_mail->seq >= hdr->messages_count) {
		if (_mail->seq == hdr->messages_count) {
			/* last message, use the synced mbox size */
			int trailer_size;

			trailer_size = (mbox->storage->storage.flags &
					MAIL_STORAGE_FLAG_SAVE_CRLF) != 0 ?
				2 : 1;
			body_size = hdr->sync_size - body_offset - trailer_size;
		} else {
			/* we're appending a new message */
			body_size = (uoff_t)-1;
		}
	} else if (mbox_file_lookup_offset(mbox, mail->trans->trans_view,
					   _mail->seq + 1, &next_offset) > 0) {
		body_size = next_offset - body_offset;
	} else {
		body_size = (uoff_t)-1;
	}

	/* verify that the calculated body size is correct */
	body_size = istream_raw_mbox_get_body_size(mbox->mbox_stream,
						   body_size);

	data->physical_size = hdr_size.physical_size + body_size;
	*size_r = data->physical_size;

	i_stream_seek(input, old_offset);
	return 0;
}

static int mbox_mail_get_stream(struct mail *_mail,
				struct message_size *hdr_size,
				struct message_size *body_size,
				struct istream **stream_r)
{
	struct index_mail *mail = (struct index_mail *)_mail;
	struct index_mail_data *data = &mail->data;
	struct mbox_mailbox *mbox = (struct mbox_mailbox *)mail->ibox;
	struct istream *raw_stream;
	uoff_t offset;

	if (data->stream == NULL) {
		if (mbox_mail_seek(mail) < 0)
			return -1;

		raw_stream = mbox->mbox_stream;
		offset = istream_raw_mbox_get_header_offset(raw_stream);
		i_stream_seek(raw_stream, offset);
		raw_stream = i_stream_create_limit(raw_stream, (uoff_t)-1);
		data->stream =
			i_stream_create_header_filter(raw_stream,
				HEADER_FILTER_EXCLUDE | HEADER_FILTER_NO_CR,
				mbox_hide_headers, mbox_hide_headers_count,
				null_header_filter_callback, NULL);
		i_stream_unref(&raw_stream);
	}

	return index_mail_init_stream(mail, hdr_size, body_size, stream_r);
}

static void mbox_mail_set_seq(struct mail *_mail, uint32_t seq)
{
	struct index_mail *mail = (struct index_mail *)_mail;

	index_mail_set_seq(_mail, seq);
	mail->data.dont_cache_fetch_fields |= MAIL_FETCH_PHYSICAL_SIZE;
}

static bool mbox_mail_set_uid(struct mail *_mail, uint32_t uid)
{
	struct index_mail *mail = (struct index_mail *)_mail;
	bool ret;

	ret = index_mail_set_uid(_mail, uid);
	mail->data.dont_cache_fetch_fields |= MAIL_FETCH_PHYSICAL_SIZE;
	return ret;
}

struct mail_vfuncs mbox_mail_vfuncs = {
	index_mail_close,
	index_mail_free,
	mbox_mail_set_seq,
	mbox_mail_set_uid,

	index_mail_get_flags,
	index_mail_get_keywords,
	index_mail_get_keyword_indexes,
	index_mail_get_parts,
	index_mail_get_date,
	mbox_mail_get_received_date,
	mbox_mail_get_save_date,
	index_mail_get_virtual_size,
	mbox_mail_get_physical_size,
	index_mail_get_first_header,
	index_mail_get_headers,
	index_mail_get_header_stream,
	mbox_mail_get_stream,
	mbox_mail_get_special,
	index_mail_update_flags,
	index_mail_update_keywords,
	index_mail_expunge,
	index_mail_set_cache_corrupted
};
