/* Copyright (C) 2002 Timo Sirainen */

#include "lib.h"
#include "ibuffer.h"
#include "obuffer.h"
#include "mbox-index.h"
#include "mbox-storage.h"
#include "mbox-lock.h"

#include <fcntl.h>
#include <unistd.h>

static int expunge_real(IndexMailbox *ibox, MailIndexRecord *rec,
			unsigned int seq, IBuffer *inbuf, OBuffer *outbuf,
			int notify)
{
	uoff_t offset, hdr_size, body_size;
	uoff_t end_offset, from_offset, copy_size, old_limit;
	const unsigned char *data;
	size_t size;
	int expunges, failed;

	if (seq == 1)
		end_offset = 0;
	else {
		/* we need to find offset to beginning of From-line.
		   not the fastest way maybe, but easiest.. */
		rec = ibox->index->lookup(ibox->index, seq-1);
		
		if (!mbox_mail_get_location(ibox->index, rec, &offset,
					    &hdr_size, &body_size))
			return FALSE;
		end_offset = offset + hdr_size + body_size;

		/* get back to the deleted record */
		rec = ibox->index->next(ibox->index, rec);
	}

	old_limit = inbuf->v_limit;

	expunges = FALSE;
	while (rec != NULL) {
		if (!mbox_mail_get_location(ibox->index, rec, &offset,
					    &hdr_size, &body_size))
			return FALSE;

		from_offset = end_offset;
		end_offset = offset + hdr_size + body_size;

		if (rec->msg_flags & MAIL_DELETED) {
			if (!index_expunge_mail(ibox, rec, seq, notify))
				return FALSE;
			seq--;

			if (!expunges) {
				/* first expunged record, seek to position
				   where we want to begin writing */
				if (!o_buffer_seek(outbuf, from_offset))
					return FALSE;
				expunges = TRUE;
			}
		} else if (expunges) {
			/* seek to wanted input position, and copy
			   this messages */
			i_assert(inbuf->v_offset <= from_offset);
			i_buffer_skip(inbuf, from_offset - inbuf->v_offset);

			if (outbuf->offset == 0) {
				/* we're writing to beginning of mbox, so we
				   don't want the [\r]\n there */
				(void)i_buffer_read_data(inbuf, &data,
							 &size, 1);
				if (size > 0 && data[0] == '\n')
					i_buffer_skip(inbuf, 1);
				else if (size > 1 && data[0] == '\r' &&
					 data[1] == '\n')
					i_buffer_skip(inbuf, 2);
			}

			i_buffer_set_read_limit(inbuf, end_offset);
			failed = o_buffer_send_ibuffer(outbuf, inbuf) < 0;
			i_buffer_set_read_limit(inbuf, old_limit);

			if (failed || inbuf->v_offset != end_offset)
				return FALSE;
		}

		rec = ibox->index->next(ibox->index, rec);
		seq++;
	}

	i_buffer_skip(inbuf, end_offset - inbuf->v_offset);

	/* copy the rest as well, should be only \n but someone might
	   as well just appended more data.. but if we've deleted all mail,
	   don't write the only \n there. */
	copy_size = inbuf->v_size - inbuf->v_offset;
	if (outbuf->offset == 0 && copy_size == 1)
		return TRUE;

	return o_buffer_send_ibuffer(outbuf, inbuf) >= 0;
}

int mbox_expunge_locked(IndexMailbox *ibox, int notify)
{
	MailIndexRecord *rec;
	IBuffer *inbuf;
	OBuffer *outbuf;
	unsigned int seq;
	int failed;

	if (!index_expunge_seek_first(ibox, &seq, &rec))
		return FALSE;

	if (rec == NULL) {
		/* no deleted messages */
		return TRUE;
	}

	/* mbox must be already opened, synced and locked at this point.
	   we just want the IBuffer. */
	inbuf = mbox_get_inbuf(ibox->index, 0, MAIL_LOCK_EXCLUSIVE);
	if (inbuf == NULL)
		return FALSE;

	i_assert(ibox->index->mbox_sync_counter ==
		 ibox->index->mbox_lock_counter);

	t_push();
	outbuf = o_buffer_create_file(ibox->index->mbox_fd, data_stack_pool,
				      4096, IO_PRIORITY_DEFAULT, FALSE);

	failed = !expunge_real(ibox, rec, seq, inbuf, outbuf, notify);

	if (failed && outbuf->offset > 0) {
		/* we moved some of the data. move the rest as well so there
		   won't be invalid holes in mbox file */
		(void)o_buffer_send_ibuffer(outbuf, inbuf);
	}

	if (ftruncate(ibox->index->mbox_fd, outbuf->offset) < 0) {
		mail_storage_set_error(ibox->box.storage, "ftruncate() failed "
				       "for mbox file %s: %m",
				       ibox->index->mbox_path);
		failed = TRUE;
	}

	o_buffer_unref(outbuf);
	t_pop();

	return !failed;
}

int mbox_storage_expunge(Mailbox *box, int notify)
{
	IndexMailbox *ibox = (IndexMailbox *) box;
	int ret;

	ibox->index->mbox_lock_next_sync = MAIL_LOCK_EXCLUSIVE;
	ret = index_storage_expunge(box, notify);
	(void)mbox_unlock(ibox->index);

	return ret;
}
