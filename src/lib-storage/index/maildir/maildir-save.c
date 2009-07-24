/* Copyright (c) 2002-2009 Dovecot authors, see the included COPYING file */

#include "lib.h"
#include "ioloop.h"
#include "array.h"
#include "buffer.h"
#include "istream.h"
#include "istream-crlf.h"
#include "ostream.h"
#include "fdatasync-path.h"
#include "eacces-error.h"
#include "str.h"
#include "index-mail.h"
#include "maildir-storage.h"
#include "maildir-uidlist.h"
#include "maildir-keywords.h"
#include "maildir-filename.h"
#include "maildir-sync.h"

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <utime.h>
#include <sys/stat.h>

#define MAILDIR_FILENAME_FLAG_MOVED 0x10000000

struct maildir_filename {
	struct maildir_filename *next;
	const char *basename;

	uoff_t size, vsize;
	enum mail_flags flags;
	unsigned int keywords_count;
	/* unsigned int keywords[]; */
};

struct maildir_save_context {
	struct mail_save_context ctx;
	pool_t pool;

	struct maildir_mailbox *mbox;
	struct mail_index_transaction *trans;
	struct maildir_uidlist_sync_ctx *uidlist_sync_ctx;
	struct maildir_keywords_sync_ctx *keywords_sync_ctx;
	struct maildir_index_sync_context *sync_ctx;
	struct mail *mail, *cur_dest_mail;

	const char *tmpdir, *newdir, *curdir;
	struct maildir_filename *files, **files_tail, *file_last;
	unsigned int files_count;

	buffer_t keywords_buffer;
	ARRAY_TYPE(keyword_indexes) keywords_array;

	struct istream *input;
	int fd;
	uint32_t first_seq, seq;

	unsigned int have_keywords:1;
	unsigned int locked:1;
	unsigned int failed:1;
	unsigned int last_save_finished:1;
};

static int maildir_file_move(struct maildir_save_context *ctx,
			     struct maildir_filename *mf, const char *destname,
			     bool newdir)
{
	struct mail_storage *storage = &ctx->mbox->storage->storage;
	const char *tmp_path, *new_path;

	/* if we have flags, we'll move it to cur/ directly, because files in
	   new/ directory can't have flags. alternative would be to write it
	   in new/ and set the flags dirty in index file, but in that case
	   external MUAs would see wrong flags. */
	tmp_path = t_strconcat(ctx->tmpdir, "/", mf->basename, NULL);
	new_path = newdir ?
		t_strconcat(ctx->newdir, "/", destname, NULL) :
		t_strconcat(ctx->curdir, "/", destname, NULL);

	/* maildir spec says we should use link() + unlink() here. however
	   since our filename is guaranteed to be unique, rename() works just
	   as well, except faster. even if the filename wasn't unique, the
	   problem could still happen if the file was already moved from
	   new/ to cur/, so link() doesn't really provide any safety anyway.

	   Besides the small temporary performance benefits, this rename() is
	   almost required with OSX's HFS+ filesystem, since it implements
	   hard links in a pretty ugly way, which makes the performance crawl
	   when a lot of hard links are used. */
	if (rename(tmp_path, new_path) == 0) {
		mf->flags |= MAILDIR_FILENAME_FLAG_MOVED;
		return 0;
	} else if (ENOSPACE(errno)) {
		mail_storage_set_error(storage, MAIL_ERROR_NOSPACE,
				       MAIL_ERRSTR_NO_SPACE);
		return -1;
	} else {
		mail_storage_set_critical(storage, "rename(%s, %s) failed: %m",
					  tmp_path, new_path);
		return -1;
	}
}

struct maildir_save_context *
maildir_save_transaction_init(struct maildir_transaction_context *t)
{
        struct maildir_mailbox *mbox = (struct maildir_mailbox *)t->ictx.ibox;
	struct maildir_save_context *ctx;
	pool_t pool;

	pool = pool_alloconly_create("maildir_save_context", 4096);
	ctx = p_new(pool, struct maildir_save_context, 1);
	ctx->ctx.transaction = &t->ictx.mailbox_ctx;
	ctx->pool = pool;
	ctx->mbox = mbox;
	ctx->trans = t->ictx.trans;
	ctx->files_tail = &ctx->files;
	ctx->fd = -1;

	ctx->tmpdir = p_strconcat(pool, mbox->ibox.box.path, "/tmp", NULL);
	ctx->newdir = p_strconcat(pool, mbox->ibox.box.path, "/new", NULL);
	ctx->curdir = p_strconcat(pool, mbox->ibox.box.path, "/cur", NULL);

	buffer_create_const_data(&ctx->keywords_buffer, NULL, 0);
	array_create_from_buffer(&ctx->keywords_array, &ctx->keywords_buffer,
				 sizeof(unsigned int));
	ctx->last_save_finished = TRUE;
	return ctx;
}

uint32_t maildir_save_add(struct mail_save_context *_ctx,
			  const char *base_fname)
{
	struct maildir_save_context *ctx = (struct maildir_save_context *)_ctx;
	struct maildir_filename *mf;
	struct istream *input;
	unsigned int keyword_count;

	/* don't allow caller to specify recent flag */
	_ctx->flags &= ~MAIL_RECENT;
	if (ctx->mbox->ibox.keep_recent)
		_ctx->flags |= MAIL_RECENT;

	/* now, we want to be able to rollback the whole append session,
	   so we'll just store the name of this temp file and move it later
	   into new/ or cur/. */
	/* @UNSAFE */
	keyword_count = _ctx->keywords == NULL ? 0 : _ctx->keywords->count;
	mf = p_malloc(ctx->pool, sizeof(*mf) +
		      sizeof(unsigned int) * keyword_count);
	mf->basename = p_strdup(ctx->pool, base_fname);
	mf->flags = _ctx->flags;
	mf->size = (uoff_t)-1;
	mf->vsize = (uoff_t)-1;

	ctx->file_last = mf;
	i_assert(*ctx->files_tail == NULL);
	*ctx->files_tail = mf;
	ctx->files_tail = &mf->next;
	ctx->files_count++;

	if (_ctx->keywords != NULL) {
		/* @UNSAFE */
		mf->keywords_count = keyword_count;
		memcpy(mf + 1, _ctx->keywords->idx,
		       sizeof(unsigned int) * keyword_count);
		ctx->have_keywords = TRUE;
	}

	/* insert into index */
	mail_index_append(ctx->trans, 0, &ctx->seq);
	mail_index_update_flags(ctx->trans, ctx->seq,
				MODIFY_REPLACE, _ctx->flags);
	if (_ctx->keywords != NULL) {
		mail_index_update_keywords(ctx->trans, ctx->seq,
					   MODIFY_REPLACE, _ctx->keywords);
	}

	if (ctx->first_seq == 0) {
		ctx->first_seq = ctx->seq;
		i_assert(ctx->files->next == NULL);
	}

	if (_ctx->dest_mail == NULL) {
		if (ctx->mail == NULL)
			ctx->mail = mail_alloc(_ctx->transaction, 0, NULL);
		_ctx->dest_mail = ctx->mail;
	}
	mail_set_seq(_ctx->dest_mail, ctx->seq);

	if (ctx->input == NULL) {
		/* FIXME: copying with hardlinking. we could copy the
		   cached data directly */
		ctx->cur_dest_mail = NULL;
	} else {
		input = index_mail_cache_parse_init(_ctx->dest_mail,
						    ctx->input);
		i_stream_unref(&ctx->input);
		ctx->input = input;
		ctx->cur_dest_mail = _ctx->dest_mail;
	}
	return ctx->seq;
}

static bool
maildir_get_updated_filename(struct maildir_save_context *ctx,
			     struct maildir_filename *mf,
			     const char **fname_r)
{
	const char *basename = mf->basename;

	if (ctx->mbox->storage->save_size_in_filename &&
	    mf->size != (uoff_t)-1) {
		basename = t_strdup_printf("%s,%c=%"PRIuUOFF_T, basename,
					   MAILDIR_EXTRA_FILE_SIZE, mf->size);
	}

	if (mf->vsize != (uoff_t)-1) {
		basename = t_strdup_printf("%s,%c=%"PRIuUOFF_T, basename,
					   MAILDIR_EXTRA_VIRTUAL_SIZE,
					   mf->vsize);
	}

	if (mf->keywords_count == 0) {
		if ((mf->flags & MAIL_FLAGS_MASK) == MAIL_RECENT) {
			*fname_r = basename;
			return TRUE;
		}

		*fname_r = maildir_filename_set_flags(NULL, basename,
					mf->flags & MAIL_FLAGS_MASK, NULL);
		return FALSE;
	}

	i_assert(ctx->keywords_sync_ctx != NULL || mf->keywords_count == 0);
	buffer_create_const_data(&ctx->keywords_buffer, mf + 1,
				 mf->keywords_count * sizeof(unsigned int));
	*fname_r = maildir_filename_set_flags(ctx->keywords_sync_ctx, basename,
					      mf->flags & MAIL_FLAGS_MASK,
					      &ctx->keywords_array);
	return FALSE;
}

static const char *maildir_mf_get_path(struct maildir_save_context *ctx,
				       struct maildir_filename *mf)
{
	const char *fname;

	if ((mf->flags & MAILDIR_FILENAME_FLAG_MOVED) == 0) {
		/* file is still in tmp/ */
		return t_strdup_printf("%s/%s", ctx->tmpdir, mf->basename);
	}

	/* already moved to new/ or cur/ */
	if (maildir_get_updated_filename(ctx, mf, &fname))
		return t_strdup_printf("%s/%s", ctx->newdir, mf->basename);
	else
		return t_strdup_printf("%s/%s", ctx->curdir, fname);
}

const char *maildir_save_file_get_path(struct mailbox_transaction_context *_t,
				       uint32_t seq)
{
	struct maildir_transaction_context *t =
		(struct maildir_transaction_context *)_t;
	struct maildir_save_context *ctx = t->save_ctx;
	struct maildir_filename *mf;

	i_assert(seq >= ctx->first_seq);

	seq -= ctx->first_seq;
	mf = ctx->files;
	while (seq > 0) {
		mf = mf->next;
		i_assert(mf != NULL);
		seq--;
	}

	return maildir_mf_get_path(ctx, mf);
}

static int maildir_create_tmp(struct maildir_mailbox *mbox, const char *dir,
			      const char **fname)
{
	struct mailbox *box = &mbox->ibox.box;
	struct stat st;
	unsigned int prefix_len;
	const char *tmp_fname = *fname;
	string_t *path;
	int fd;

	path = t_str_new(256);
	str_append(path, dir);
	str_append_c(path, '/');
	prefix_len = str_len(path);

	for (;;) {
		if (tmp_fname == NULL)
			tmp_fname = maildir_filename_generate();
		str_truncate(path, prefix_len);
		str_append(path, tmp_fname);

		/* stat() first to see if it exists. pretty much the only
		   possibility of that happening is if time had moved
		   backwards, but even then it's highly unlikely. */
		if (stat(str_c(path), &st) == 0) {
			/* try another file name */
		} else if (errno != ENOENT) {
			mail_storage_set_critical(box->storage,
				"stat(%s) failed: %m", str_c(path));
			return -1;
		} else {
			/* doesn't exist */
			mode_t old_mask = umask(0777 & ~box->file_create_mode);
			fd = open(str_c(path),
				  O_WRONLY | O_CREAT | O_TRUNC | O_EXCL, 0777);
			umask(old_mask);

			if (fd != -1 || errno != EEXIST)
				break;
			/* race condition between stat() and open().
			   highly unlikely. */
		}
		tmp_fname = NULL;
	}

	*fname = tmp_fname;
	if (fd == -1) {
		if (ENOSPACE(errno)) {
			mail_storage_set_error(box->storage,
				MAIL_ERROR_NOSPACE, MAIL_ERRSTR_NO_SPACE);
		} else {
			mail_storage_set_critical(box->storage,
				"open(%s) failed: %m", str_c(path));
		}
	} else if (box->file_create_gid != (gid_t)-1) {
		if (fchown(fd, (uid_t)-1, box->file_create_gid) < 0) {
			if (errno == EPERM) {
				mail_storage_set_critical(box->storage, "%s",
					eperm_error_get_chgrp("fchown",
						str_c(path),
						box->file_create_gid,
						box->file_create_gid_origin));
			} else {
				mail_storage_set_critical(box->storage,
					"fchown(%s) failed: %m", str_c(path));
			}
		}
	}

	return fd;
}

struct mail_save_context *
maildir_save_alloc(struct mailbox_transaction_context *_t)
{
	struct maildir_transaction_context *t =
		(struct maildir_transaction_context *)_t;

	i_assert((t->ictx.flags & MAILBOX_TRANSACTION_FLAG_EXTERNAL) != 0);

	if (t->save_ctx == NULL)
		t->save_ctx = maildir_save_transaction_init(t);
	return &t->save_ctx->ctx;
}

int maildir_save_begin(struct mail_save_context *_ctx, struct istream *input)
{
	struct maildir_save_context *ctx = (struct maildir_save_context *)_ctx;

	T_BEGIN {
		/* create a new file in tmp/ directory */
		const char *fname = _ctx->guid;

		ctx->fd = maildir_create_tmp(ctx->mbox, ctx->tmpdir, &fname);
		if (ctx->fd == -1)
			ctx->failed = TRUE;
		else {
			if (ctx->mbox->storage->storage.set->mail_save_crlf)
				ctx->input = i_stream_create_crlf(input);
			else
				ctx->input = i_stream_create_lf(input);
			maildir_save_add(_ctx, fname);
		}
	} T_END;

	if (!ctx->failed) {
		_ctx->output = o_stream_create_fd_file(ctx->fd, 0, FALSE);
		o_stream_cork(_ctx->output);
		ctx->last_save_finished = FALSE;
	}
	return ctx->failed ? -1 : 0;
}

int maildir_save_continue(struct mail_save_context *_ctx)
{
	struct maildir_save_context *ctx = (struct maildir_save_context *)_ctx;
	struct mail_storage *storage = &ctx->mbox->storage->storage;

	if (ctx->failed)
		return -1;

	do {
		if (o_stream_send_istream(_ctx->output, ctx->input) < 0) {
			if (!mail_storage_set_error_from_errno(storage)) {
				mail_storage_set_critical(storage,
					"o_stream_send_istream(%s/%s) "
					"failed: %m",
					ctx->tmpdir, ctx->file_last->basename);
			}
			ctx->failed = TRUE;
			return -1;
		}
		if (ctx->cur_dest_mail != NULL)
			index_mail_cache_parse_continue(ctx->cur_dest_mail);

		/* both tee input readers may consume data from our primary
		   input stream. we'll have to make sure we don't return with
		   one of the streams still having data in them. */
	} while (i_stream_read(ctx->input) > 0);
	return 0;
}

static int maildir_save_finish_received_date(struct maildir_save_context *ctx,
					     const char *path)
{
	struct mail_storage *storage = &ctx->mbox->storage->storage;
	struct utimbuf buf;
	struct stat st;

	if (ctx->ctx.received_date != (time_t)-1) {
		/* set the received_date by modifying mtime */
		buf.actime = ioloop_time;
		buf.modtime = ctx->ctx.received_date;

		if (utime(path, &buf) < 0) {
			mail_storage_set_critical(storage,
						  "utime(%s) failed: %m", path);
			return -1;
		}
	} else if (ctx->fd != -1) {
		if (fstat(ctx->fd, &st) == 0)
			ctx->ctx.received_date = st.st_mtime;
		else {
			mail_storage_set_critical(storage,
						  "fstat(%s) failed: %m", path);
			return -1;
		}
	} else {
		/* hardlinked */
		if (stat(path, &st) == 0)
			ctx->ctx.received_date = st.st_mtime;
		else {
			mail_storage_set_critical(storage,
						  "stat(%s) failed: %m", path);
			return -1;
		}
	}
	return 0;
}

static void maildir_save_remove_last_filename(struct maildir_save_context *ctx)
{
	struct maildir_filename **fm;

	for (fm = &ctx->files; (*fm)->next != NULL; fm = &(*fm)->next) ;
	i_assert(*fm == ctx->file_last);
	*fm = NULL;

	ctx->files_tail = fm;
	ctx->file_last = NULL;
	ctx->files_count--;
}

static int maildir_save_finish_real(struct mail_save_context *_ctx)
{
	struct maildir_save_context *ctx = (struct maildir_save_context *)_ctx;
	struct mail_storage *storage = &ctx->mbox->storage->storage;
	const char *path;
	int output_errno;

	ctx->last_save_finished = TRUE;
	if (ctx->failed && ctx->fd == -1) {
		/* tmp file creation failed */
		return -1;
	}

	path = t_strconcat(ctx->tmpdir, "/", ctx->file_last->basename, NULL);
	if (o_stream_flush(_ctx->output) < 0) {
		if (!mail_storage_set_error_from_errno(storage)) {
			mail_storage_set_critical(storage,
				"o_stream_flush(%s) failed: %m", path);
		}
		ctx->failed = TRUE;
	}

	if (_ctx->save_date != (time_t)-1) {
		/* we can't change ctime, but we can add the date to cache */
		struct index_mail *mail = (struct index_mail *)_ctx->dest_mail;
		uint32_t t = _ctx->save_date;

		index_mail_cache_add(mail, MAIL_CACHE_SAVE_DATE, &t, sizeof(t));
	}

 	if (maildir_save_finish_received_date(ctx, path) < 0)
		ctx->failed = TRUE;

	if (ctx->cur_dest_mail != NULL) {
		index_mail_cache_parse_deinit(ctx->cur_dest_mail,
					      ctx->ctx.received_date,
					      !ctx->failed);
	}
	i_stream_unref(&ctx->input);

	/* remember the size in case we want to add it to filename */
	ctx->file_last->size = _ctx->output->offset;
	if (ctx->cur_dest_mail == NULL ||
	    mail_get_virtual_size(ctx->cur_dest_mail,
				  &ctx->file_last->vsize) < 0)
		ctx->file_last->vsize = (uoff_t)-1;

	output_errno = _ctx->output->stream_errno;
	o_stream_destroy(&_ctx->output);

	if (!storage->set->fsync_disable && !ctx->failed) {
		if (fsync(ctx->fd) < 0) {
			if (!mail_storage_set_error_from_errno(storage)) {
				mail_storage_set_critical(storage,
						  "fsync(%s) failed: %m", path);
			}
			ctx->failed = TRUE;
		}
	}
	if (close(ctx->fd) < 0) {
		if (!mail_storage_set_error_from_errno(storage)) {
			mail_storage_set_critical(storage,
						  "close(%s) failed: %m", path);
		}
		ctx->failed = TRUE;
	}
	ctx->fd = -1;

	if (ctx->failed) {
		/* delete the tmp file */
		if (unlink(path) < 0 && errno != ENOENT) {
			mail_storage_set_critical(storage,
				"unlink(%s) failed: %m", path);
		}

		errno = output_errno;
		if (ENOSPACE(errno)) {
			mail_storage_set_error(storage,
				MAIL_ERROR_NOSPACE, MAIL_ERRSTR_NO_SPACE);
		} else if (errno != 0) {
			mail_storage_set_critical(storage,
				"write(%s) failed: %m", path);
		}

		maildir_save_remove_last_filename(ctx);
		return -1;
	}

	ctx->file_last = NULL;
	return 0;
}

int maildir_save_finish(struct mail_save_context *ctx)
{
	int ret;

	T_BEGIN {
		ret = maildir_save_finish_real(ctx);
	} T_END;
	index_save_context_free(ctx);
	return ret;
}

void maildir_save_cancel(struct mail_save_context *_ctx)
{
	struct maildir_save_context *ctx = (struct maildir_save_context *)_ctx;

	ctx->failed = TRUE;
	(void)maildir_save_finish(_ctx);
}

static void
maildir_save_unlink_files(struct maildir_save_context *ctx)
{
	struct maildir_filename *mf;

	for (mf = ctx->files; mf != NULL; mf = mf->next) T_BEGIN {
		(void)unlink(maildir_mf_get_path(ctx, mf));
	} T_END;
	ctx->files = NULL;
}

static int maildir_transaction_fsync_dirs(struct maildir_save_context *ctx,
					  bool new_changed, bool cur_changed)
{
	struct mail_storage *storage = &ctx->mbox->storage->storage;

	if (storage->set->fsync_disable)
		return 0;

	if (new_changed) {
		if (fdatasync_path(ctx->newdir) < 0) {
			mail_storage_set_critical(storage,
				"fdatasync_path(%s) failed: %m", ctx->newdir);
			return -1;
		}
	}
	if (cur_changed) {
		if (fdatasync_path(ctx->curdir) < 0) {
			mail_storage_set_critical(storage,
				"fdatasync_path(%s) failed: %m", ctx->curdir);
			return -1;
		}
	}
	return 0;
}

static int
maildir_save_sync_index(struct maildir_save_context *ctx)
{
	struct maildir_transaction_context *t =
		(struct maildir_transaction_context *)ctx->ctx.transaction;
	struct maildir_mailbox *mbox = ctx->mbox;
	struct seq_range *range;
	uint32_t uid, first_uid, next_uid;
	int ret;

	/* we'll need to keep the lock past the sync deinit */
	ret = maildir_uidlist_lock(mbox->uidlist);
	i_assert(ret > 0);

	if (maildir_sync_index_begin(mbox, NULL, &ctx->sync_ctx) < 0)
		return -1;
	ctx->keywords_sync_ctx =
		maildir_sync_get_keywords_sync_ctx(ctx->sync_ctx);

	if (maildir_sync_header_refresh(mbox) < 0)
		return -1;
	if (maildir_uidlist_refresh_fast_init(mbox->uidlist) < 0)
		return -1;

	/* now that uidlist is locked, make sure all the existing mails
	   have been added to index. we don't really look into the
	   maildir, just add all the new mails listed in
	   dovecot-uidlist to index. */
	if (maildir_sync_index(ctx->sync_ctx, TRUE) < 0)
		return -1;

	/* if messages were added to index, assign them UIDs */
	first_uid = maildir_uidlist_get_next_uid(mbox->uidlist);
	i_assert(first_uid != 0);
	mail_index_append_assign_uids(ctx->trans, first_uid, &next_uid);
	i_assert(next_uid = first_uid + ctx->files_count);

	/* these mails are all recent in our session */
	for (uid = first_uid; uid < next_uid; uid++)
		index_mailbox_set_recent_uid(&mbox->ibox, uid);

	if (!mbox->ibox.keep_recent) {
		/* maildir_sync_index() dropped recent flags from
		   existing messages. we'll still need to drop recent
		   flags from these newly added messages. */
		mail_index_update_header(ctx->trans,
					 offsetof(struct mail_index_header,
						  first_recent_uid),
					 &next_uid, sizeof(next_uid), FALSE);
	}

	/* this will work even if index isn't updated */
	range = array_append_space(&t->ictx.mailbox_ctx.changes->saved_uids);
	range->seq1 = first_uid;
	range->seq2 = next_uid - 1;
	return 0;
}

static void
maildir_save_rollback_index_changes(struct maildir_save_context *ctx)
{
	struct maildir_transaction_context *t =
		(struct maildir_transaction_context *)ctx->ctx.transaction;
	uint32_t seq;

	for (seq = ctx->seq; seq >= ctx->first_seq; seq--)
		mail_index_expunge(ctx->trans, seq);

	mail_cache_transaction_reset(t->ictx.cache_trans);
}

static int
maildir_save_move_files_to_newcur(struct maildir_save_context *ctx,
				  struct maildir_filename **last_mf_r)
{
	struct maildir_filename *mf;
	bool newdir, new_changed, cur_changed;
	int ret = 0;

	*last_mf_r = NULL;

	ret = 0;
	new_changed = cur_changed = FALSE;
	for (mf = ctx->files; mf != NULL; mf = mf->next) {
		T_BEGIN {
			const char *dest;

			newdir = maildir_get_updated_filename(ctx, mf, &dest);
			if (newdir)
				new_changed = TRUE;
			else
				cur_changed = TRUE;
			ret = maildir_file_move(ctx, mf, dest, newdir);
		} T_END;
		if (ret < 0)
			return -1;
		*last_mf_r = mf;
	}

	return maildir_transaction_fsync_dirs(ctx, new_changed, cur_changed);
}

static void maildir_save_sync_uidlist(struct maildir_save_context *ctx)
{
	struct maildir_filename *mf;
	enum maildir_uidlist_rec_flag flags;
	bool newdir;
	int ret;

	for (mf = ctx->files; mf != NULL; mf = mf->next) T_BEGIN {
		const char *dest;

		newdir = maildir_get_updated_filename(ctx, mf, &dest);
		flags = MAILDIR_UIDLIST_REC_FLAG_RECENT;
		if (newdir)
			flags |= MAILDIR_UIDLIST_REC_FLAG_NEW_DIR;
		ret = maildir_uidlist_sync_next(ctx->uidlist_sync_ctx,
						dest, flags);
		i_assert(ret > 0);
	} T_END;
}

int maildir_transaction_save_commit_pre(struct maildir_save_context *ctx)
{
	struct maildir_transaction_context *t =
		(struct maildir_transaction_context *)ctx->ctx.transaction;
	struct maildir_filename *last_mf;
	enum maildir_uidlist_sync_flags sync_flags;
	int ret;

	i_assert(ctx->ctx.output == NULL);
	i_assert(ctx->last_save_finished);

	sync_flags = MAILDIR_UIDLIST_SYNC_PARTIAL |
		MAILDIR_UIDLIST_SYNC_NOREFRESH;

	if ((t->ictx.flags & MAILBOX_TRANSACTION_FLAG_ASSIGN_UIDS) != 0) {
		/* we want to assign UIDs, we must lock uidlist */
	} else if (ctx->have_keywords) {
		/* keywords file updating relies on uidlist lock. */
	} else {
		/* no requirement to lock uidlist. if we happen to get a lock,
		   assign uids. */
		sync_flags |= MAILDIR_UIDLIST_SYNC_TRYLOCK;
	}
	ret = maildir_uidlist_sync_init(ctx->mbox->uidlist, sync_flags,
					&ctx->uidlist_sync_ctx);
	if (ret > 0) {
		ctx->locked = TRUE;
		if (maildir_save_sync_index(ctx) < 0) {
			maildir_transaction_save_rollback(ctx);
			return -1;
		}
	} else if (ret == 0 &&
		   (sync_flags & MAILDIR_UIDLIST_SYNC_TRYLOCK) != 0) {
		ctx->locked = FALSE;
		i_assert(ctx->uidlist_sync_ctx == NULL);
		/* since we couldn't lock uidlist, we'll have to drop the
		   appends to index. */
		maildir_save_rollback_index_changes(ctx);
	} else {
		maildir_transaction_save_rollback(ctx);
		return -1;
	}

	ret = maildir_save_move_files_to_newcur(ctx, &last_mf);
	if (ctx->locked) {
		if (ret == 0) {
			/* update dovecot-uidlist file. */
			maildir_save_sync_uidlist(ctx);
		}

		if (maildir_uidlist_sync_deinit(&ctx->uidlist_sync_ctx,
						ret == 0) < 0)
			ret = -1;
	}

	t->ictx.mailbox_ctx.changes->uid_validity =
		maildir_uidlist_get_uid_validity(ctx->mbox->uidlist);

	if (ctx->mail != NULL) {
		/* Mail freeing may trigger cache updates and a call to
		   maildir_save_file_get_path(). Do this before finishing index
		   sync so we still have keywords_sync_ctx. */
		mail_free(&ctx->mail);
	}

	if (ctx->locked) {
		/* It doesn't matter if index syncing fails */
		ctx->keywords_sync_ctx = NULL;
		if (ret < 0)
			maildir_sync_index_rollback(&ctx->sync_ctx);
		else
			(void)maildir_sync_index_commit(&ctx->sync_ctx);
	}

	if (ret < 0) {
		ctx->keywords_sync_ctx = !ctx->have_keywords ? NULL :
			maildir_keywords_sync_init(ctx->mbox->keywords,
						   ctx->mbox->ibox.index);

		/* unlink the files we just moved in an attempt to rollback
		   the transaction. uidlist is still locked, so at least other
		   Dovecot instances haven't yet seen the files. we need
		   to have the keywords sync context to be able to generate
		   the destination filenames if keywords were used. */
		maildir_save_unlink_files(ctx);

		if (ctx->keywords_sync_ctx != NULL)
			maildir_keywords_sync_deinit(&ctx->keywords_sync_ctx);
		/* returning failure finishes the save_context */
		maildir_transaction_save_rollback(ctx);
		return -1;
	}
	return 0;
}

void maildir_transaction_save_commit_post(struct maildir_save_context *ctx)
{
	ctx->ctx.transaction = NULL; /* transaction is already freed */

	if (ctx->locked)
		maildir_uidlist_unlock(ctx->mbox->uidlist);
	pool_unref(&ctx->pool);
}

static void
maildir_transaction_save_rollback_real(struct maildir_save_context *ctx)
{
	i_assert(ctx->ctx.output == NULL);

	if (!ctx->last_save_finished)
		maildir_save_cancel(&ctx->ctx);

	/* delete files in tmp/ */
	maildir_save_unlink_files(ctx);

	if (ctx->uidlist_sync_ctx != NULL)
		(void)maildir_uidlist_sync_deinit(&ctx->uidlist_sync_ctx, FALSE);
	if (ctx->sync_ctx != NULL)
		maildir_sync_index_rollback(&ctx->sync_ctx);
	if (ctx->locked)
		maildir_uidlist_unlock(ctx->mbox->uidlist);

	if (ctx->mail != NULL)
		mail_free(&ctx->mail);
	pool_unref(&ctx->pool);
}

void maildir_transaction_save_rollback(struct maildir_save_context *ctx)
{
	T_BEGIN {
		maildir_transaction_save_rollback_real(ctx);
	} T_END;
}
