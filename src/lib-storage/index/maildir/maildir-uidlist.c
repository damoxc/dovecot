/* Copyright (C) 2003 Timo Sirainen */

#include "lib.h"
#include "ioloop.h"
#include "buffer.h"
#include "hash.h"
#include "istream.h"
#include "str.h"
#include "file-dotlock.h"
#include "write-full.h"
#include "maildir-storage.h"
#include "maildir-uidlist.h"

#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <utime.h>

/* how many seconds to wait before overriding uidlist.lock */
#define UIDLIST_LOCK_STALE_TIMEOUT (60*5)

#define UIDLIST_IS_LOCKED(uidlist) \
	((uidlist)->lock_fd != -1)

struct maildir_uidlist_rec {
	uint32_t uid;
	uint32_t flags;
	char *filename;
};

struct maildir_uidlist {
	struct index_mailbox *ibox;
	char *fname;
	int lock_fd;

	time_t last_mtime;

	pool_t filename_pool;
	buffer_t *record_buf;
	struct hash_table *files;

	unsigned int version;
	unsigned int uid_validity, next_uid, last_read_uid;
	uint32_t first_recent_uid;
};

struct maildir_uidlist_sync_ctx {
	struct maildir_uidlist *uidlist;

	pool_t filename_pool;
	struct hash_table *files;
	buffer_t *new_record_buf;

	unsigned int partial_new_pos;

	unsigned int partial:1;
	unsigned int new_files:1;
	unsigned int synced:1;
	unsigned int failed:1;
};

struct maildir_uidlist_iter_ctx {
	const struct maildir_uidlist_rec *next, *end;
};

int maildir_uidlist_try_lock(struct maildir_uidlist *uidlist)
{
	const char *path;
	mode_t old_mask;
	int fd;

	if (UIDLIST_IS_LOCKED(uidlist))
		return 1;

	path = t_strconcat(uidlist->ibox->control_dir,
			   "/" MAILDIR_UIDLIST_NAME, NULL);
        old_mask = umask(0777 & ~uidlist->ibox->mail_create_mode);
	fd = file_dotlock_open(path, NULL, 0, 0, UIDLIST_LOCK_STALE_TIMEOUT,
			       NULL, NULL);
	umask(old_mask);
	if (fd == -1) {
		if (errno == EAGAIN)
			return 0;
		mail_storage_set_critical(uidlist->ibox->box.storage,
			"file_dotlock_open(%s) failed: %m", path);
		return -1;
	}

	uidlist->lock_fd = fd;
	return 1;
}

void maildir_uidlist_unlock(struct maildir_uidlist *uidlist)
{
	const char *path;

	if (!UIDLIST_IS_LOCKED(uidlist))
		return;

	path = t_strconcat(uidlist->ibox->control_dir,
			   "/" MAILDIR_UIDLIST_NAME, NULL);
	(void)file_dotlock_delete(path, uidlist->lock_fd);
	uidlist->lock_fd = -1;
}

struct maildir_uidlist *maildir_uidlist_init(struct index_mailbox *ibox)
{
	struct maildir_uidlist *uidlist;

	uidlist = i_new(struct maildir_uidlist, 1);
	uidlist->ibox = ibox;
	uidlist->fname =
		i_strconcat(ibox->control_dir, "/" MAILDIR_UIDLIST_NAME, NULL);
	uidlist->lock_fd = -1;
	uidlist->record_buf =
		buffer_create_dynamic(default_pool, 512, (size_t)-1);
	uidlist->files = hash_create(default_pool, default_pool, 4096,
				     maildir_hash, maildir_cmp);

	uidlist->uid_validity = ioloop_time;
	uidlist->next_uid = 1;

	return uidlist;
}

void maildir_uidlist_deinit(struct maildir_uidlist *uidlist)
{
	i_assert(!UIDLIST_IS_LOCKED(uidlist));

	hash_destroy(uidlist->files);
	if (uidlist->filename_pool != NULL)
		pool_unref(uidlist->filename_pool);

	buffer_free(uidlist->record_buf);
	i_free(uidlist->fname);
	i_free(uidlist);
}

static void
maildir_uidlist_mark_recent(struct maildir_uidlist *uidlist, uint32_t uid)
{
	if (uidlist->first_recent_uid == 0)
		uidlist->first_recent_uid = uid;
	i_assert(uid >= uidlist->first_recent_uid);
}

static int maildir_uidlist_next(struct maildir_uidlist *uidlist,
				const char *line, uint32_t last_uid)
{
        struct maildir_uidlist_rec *rec;
	uint32_t uid, flags;

	uid = flags = 0;
	while (*line >= '0' && *line <= '9') {
		uid = uid*10 + (*line - '0');
		line++;
	}

	if (uid <= last_uid) {
		/* we already have this */
		return 1;
	}

	if (uid == 0 || *line != ' ') {
		/* invalid file */
                mail_storage_set_critical(uidlist->ibox->box.storage,
			"Invalid data in file %s", uidlist->fname);
		return 0;
	}
	if (uid <= uidlist->last_read_uid) {
                mail_storage_set_critical(uidlist->ibox->box.storage,
			"UIDs not ordered in file %s (%u > %u)",
			uidlist->fname, uid, uidlist->last_read_uid);
		return 0;
	}
	if (uid >= uidlist->next_uid) {
                mail_storage_set_critical(uidlist->ibox->box.storage,
			"UID larger than next_uid in file %s (%u >= %u)",
			uidlist->fname, uid, uidlist->next_uid);
		return 0;
	}

	while (*line == ' ') line++;

	flags = 0;
	if (uidlist->version > 1) {
		while (*line != ' ') {
			switch (*line) {
			case 'N':
				flags |= MAILDIR_UIDLIST_REC_FLAG_NEW_DIR;
				break;
			}
			line++;
		}
		while (*line == ' ') line++;
	} else {
		/* old version, have to assume it's in new dir since we
		   don't know */
		flags |= MAILDIR_UIDLIST_REC_FLAG_NEW_DIR;
	}

	if (hash_lookup(uidlist->files, line) != NULL) {
                mail_storage_set_critical(uidlist->ibox->box.storage,
			"Duplicate file in uidlist file %s: %s",
			uidlist->fname, line);
		return 0;
	}

	rec = buffer_append_space_unsafe(uidlist->record_buf, sizeof(*rec));
	memset(rec, 0, sizeof(*rec));
	rec->uid = uid;
	rec->flags = flags;
	rec->filename = p_strdup(uidlist->filename_pool, line);
	hash_insert(uidlist->files, rec->filename, rec);
	return 1;
}

int maildir_uidlist_update(struct maildir_uidlist *uidlist)
{
	struct mail_storage *storage = uidlist->ibox->box.storage;
	const struct maildir_uidlist_rec *rec;
	const char *line;
	struct istream *input;
	struct stat st;
	uint32_t last_uid;
	size_t size;
	int fd, ret;

	if (uidlist->last_mtime != 0) {
		if (stat(uidlist->fname, &st) < 0) {
			if (errno != ENOENT) {
				mail_storage_set_critical(storage,
					"stat(%s) failed: %m", uidlist->fname);
				return -1;
			}
			return 0;
		}

		if (st.st_mtime == uidlist->last_mtime) {
			/* unchanged */
			return 1;
		}
	}

	fd = open(uidlist->fname, O_RDONLY);
	if (fd == -1) {
		if (errno != ENOENT) {
			mail_storage_set_critical(storage,
				"open(%s) failed: %m", uidlist->fname);
			return -1;
		}
		return 0;
	}

	if (fstat(fd, &st) < 0) {
		mail_storage_set_critical(storage,
			"fstat(%s) failed: %m", uidlist->fname);
		return -1;
	}

	if (uidlist->filename_pool == NULL) {
		uidlist->filename_pool =
			pool_alloconly_create("uidlist filename_pool",
					      nearest_power(st.st_size -
							    st.st_size/8));
	}

	rec = buffer_get_data(uidlist->record_buf, &size);
	last_uid = size == 0 ? 0 : rec[(size / sizeof(*rec))-1].uid;

	uidlist->version = 0;

	input = i_stream_create_file(fd, default_pool, 4096, TRUE);

	/* get header */
	line = i_stream_read_next_line(input);
	if (line == NULL || sscanf(line, "%u %u %u", &uidlist->version,
				   &uidlist->uid_validity,
				   &uidlist->next_uid) != 3 ||
	    uidlist->version < 1 || uidlist->version > 2) {
		/* broken file */
                mail_storage_set_critical(storage,
			"Corrupted header in file %s (version = %u)",
			uidlist->fname, uidlist->version);
		ret = 0;
	} else {
		ret = 1;
		while ((line = i_stream_read_next_line(input)) != NULL) {
			if (!maildir_uidlist_next(uidlist, line, last_uid)) {
				ret = 0;
				break;
			}
		}
	}

	if (ret != 0)
		uidlist->last_mtime = st.st_mtime;
	else {
		(void)unlink(uidlist->fname);
                uidlist->last_mtime = 0;
	}

	i_stream_unref(input);
	return ret;
}

static const struct maildir_uidlist_rec *
maildir_uidlist_lookup_rec(struct maildir_uidlist *uidlist, uint32_t uid,
			   unsigned int *idx_r)
{
	const struct maildir_uidlist_rec *rec;
	unsigned int idx, left_idx, right_idx;
	size_t size;

	if (uidlist->last_mtime == 0) {
		/* first time we need to read uidlist */
		if (maildir_uidlist_update(uidlist) < 0)
			return NULL;
	}

	rec = buffer_get_data(uidlist->record_buf, &size);
	size /= sizeof(*rec);

	idx = 0;
	left_idx = 0;
	right_idx = size;

	while (left_idx < right_idx) {
		idx = (left_idx + right_idx) / 2;

		if (rec[idx].uid < uid)
			left_idx = idx+1;
		else if (rec[idx].uid > uid)
			right_idx = idx;
		else {
			*idx_r = idx;
			return &rec[idx];
		}
	}

	if (idx > 0) idx--;
	*idx_r = idx;
	return NULL;
}

const char *
maildir_uidlist_lookup(struct maildir_uidlist *uidlist, uint32_t uid,
		       enum maildir_uidlist_rec_flag *flags_r)
{
	const struct maildir_uidlist_rec *rec;
	unsigned int idx;

	rec = maildir_uidlist_lookup_rec(uidlist, uid, &idx);
	if (rec == NULL)
		return NULL;

	*flags_r = rec->flags;
	return rec->filename;
}

int maildir_uidlist_is_recent(struct maildir_uidlist *uidlist, uint32_t uid)
{
	enum maildir_uidlist_rec_flag flags;

	if (uidlist->first_recent_uid == 0 || uid < uidlist->first_recent_uid)
		return FALSE;

	if (maildir_uidlist_lookup(uidlist, uid, &flags) == NULL)
		return FALSE;

	i_assert(uidlist->first_recent_uid != uid ||
		 (flags & MAILDIR_UIDLIST_REC_FLAG_RECENT) != 0);
	return (flags & MAILDIR_UIDLIST_REC_FLAG_RECENT) != 0;
}

uint32_t maildir_uidlist_get_recent_count(struct maildir_uidlist *uidlist)
{
	const struct maildir_uidlist_rec *rec;
	unsigned int idx;
	size_t size;
	uint32_t count;

	if (uidlist->first_recent_uid == 0)
		return 0;

	rec = buffer_get_data(uidlist->record_buf, &size);
	size /= sizeof(*rec);

	maildir_uidlist_lookup_rec(uidlist, uidlist->first_recent_uid, &idx);
	for (count = 0; idx < size; idx++) {
		if ((rec[idx].flags & MAILDIR_UIDLIST_REC_FLAG_RECENT) != 0)
			count++;
	}
	return count;
}

static int maildir_uidlist_rewrite_fd(struct maildir_uidlist *uidlist,
				      const char *temp_path)
{
	struct mail_storage *storage = uidlist->ibox->box.storage;
	struct maildir_uidlist_iter_ctx *iter;
	struct utimbuf ut;
	string_t *str;
	uint32_t uid;
        enum maildir_uidlist_rec_flag flags;
	const char *filename, *flags_str;
	int ret = 0;

        uidlist->version = 2;

	str = t_str_new(4096);
	str_printfa(str, "%u %u %u\n", uidlist->version,
		    uidlist->uid_validity, uidlist->next_uid);

	iter = maildir_uidlist_iter_init(uidlist->ibox->uidlist);
	while (maildir_uidlist_iter_next(iter, &uid, &flags, &filename)) {
		if (str_len(str) + MAX_INT_STRLEN +
		    strlen(filename) + 2 >= 4096) {
			/* flush buffer */
			if (write_full(uidlist->lock_fd,
				       str_data(str), str_len(str)) < 0) {
				mail_storage_set_critical(storage,
					"write_full(%s) failed: %m", temp_path);
				ret = -1;
				break;
			}
			str_truncate(str, 0);
		}

		flags_str = (flags & MAILDIR_UIDLIST_REC_FLAG_NEW_DIR) != 0 ?
			"N" : "-";
		str_printfa(str, "%u %s %s\n", uid, flags_str, filename);
	}
	maildir_uidlist_iter_deinit(iter);

	if (ret < 0)
		return -1;

	if (write_full(uidlist->lock_fd, str_data(str), str_len(str)) < 0) {
		mail_storage_set_critical(storage,
			"write_full(%s) failed: %m", temp_path);
		return -1;
	}

	/* uidlist's mtime must grow every time */
	uidlist->last_mtime = ioloop_time <= uidlist->last_mtime ?
		uidlist->last_mtime + 1 : ioloop_time;
	ut.actime = ioloop_time;
	ut.modtime = uidlist->last_mtime;
	if (utime(temp_path, &ut) < 0) {
		mail_storage_set_critical(storage,
			"utime(%s) failed: %m", temp_path);
		return -1;
	}

	if (fsync(uidlist->lock_fd) < 0) {
		mail_storage_set_critical(storage,
			"fsync(%s) failed: %m", temp_path);
		return -1;
	}

	return 0;
}

static int maildir_uidlist_rewrite(struct maildir_uidlist *uidlist)
{
	struct index_mailbox *ibox = uidlist->ibox;
	const char *temp_path, *db_path;
	int ret;

	i_assert(UIDLIST_IS_LOCKED(uidlist));

	temp_path = t_strconcat(ibox->control_dir,
				"/" MAILDIR_UIDLIST_NAME ".lock", NULL);
	ret = maildir_uidlist_rewrite_fd(uidlist, temp_path);

	if (ret == 0) {
		db_path = t_strconcat(ibox->control_dir,
				      "/" MAILDIR_UIDLIST_NAME, NULL);

		if (file_dotlock_replace(db_path, uidlist->lock_fd,
					 FALSE) <= 0) {
			mail_storage_set_critical(ibox->box.storage,
				"file_dotlock_replace(%s) failed: %m", db_path);
			ret = -1;
		}
	} else {
		(void)close(uidlist->lock_fd);
	}
        uidlist->lock_fd = -1;

	if (ret < 0)
		(void)unlink(temp_path);
	return ret;
}

static void maildir_uidlist_mark_all(struct maildir_uidlist *uidlist,
				     int nonsynced)
{
	struct maildir_uidlist_rec *rec;
	size_t i, size;

	rec = buffer_get_modifyable_data(uidlist->record_buf, &size);
	size /= sizeof(*rec);

	if (nonsynced) {
		for (i = 0; i < size; i++)
			rec[i].flags |= MAILDIR_UIDLIST_REC_FLAG_NONSYNCED;
	} else {
		for (i = 0; i < size; i++)
			rec[i].flags &= ~MAILDIR_UIDLIST_REC_FLAG_NONSYNCED;
	}
}

struct maildir_uidlist_sync_ctx *
maildir_uidlist_sync_init(struct maildir_uidlist *uidlist, int partial)
{
	struct maildir_uidlist_sync_ctx *ctx;

	ctx = i_new(struct maildir_uidlist_sync_ctx, 1);
	ctx->uidlist = uidlist;
	ctx->partial = partial;

	if (partial) {
		/* initially mark all nonsynced */
                maildir_uidlist_mark_all(uidlist, TRUE);
		return ctx;
	}

	ctx->filename_pool =
		pool_alloconly_create("maildir_uidlist_sync", 16384);
	ctx->new_record_buf =
		buffer_create_dynamic(default_pool, 512, (size_t)-1);
	ctx->files = hash_create(default_pool, ctx->filename_pool, 4096,
				 maildir_hash, maildir_cmp);
	return ctx;
}

static int maildir_uidlist_sync_uidlist(struct maildir_uidlist_sync_ctx *ctx)
{
	int ret;

	if (ctx->uidlist->last_mtime == 0) {
		/* first time reading the uidlist,
		   no locking yet */
		if (maildir_uidlist_update(ctx->uidlist) < 0) {
			ctx->failed = TRUE;
			return -1;
		}
		return 0;
	}

	/* lock and update uidlist to see if it's just been added */
	ret = maildir_uidlist_try_lock(ctx->uidlist);
	if (ret <= 0) {
		if (ret == 0)
			return 1; // FIXME: does it work right?
		ctx->failed = TRUE;
		return -1;
	}
	if (maildir_uidlist_update(ctx->uidlist) < 0) {
		ctx->failed = TRUE;
		return -1;
	}

	ctx->synced = TRUE;
	return 1;
}

static int
maildir_uidlist_sync_next_partial(struct maildir_uidlist_sync_ctx *ctx,
				  const char *filename,
				  enum maildir_uidlist_rec_flag flags)
{
	struct maildir_uidlist *uidlist = ctx->uidlist;
	struct maildir_uidlist_rec *rec;
	int ret;

	/* we'll update uidlist directly */
	rec = hash_lookup(uidlist->files, filename);
	if (rec == NULL && !ctx->synced) {
		ret = maildir_uidlist_sync_uidlist(ctx);
		if (ret < 0)
			return -1;
		if (ret == 0) {
			return maildir_uidlist_sync_next_partial(ctx, filename,
								 flags);
		}
		rec = hash_lookup(uidlist->files, filename);
	}

	if (rec == NULL) {
		ctx->new_files = TRUE;
		ctx->partial_new_pos =
			buffer_get_used_size(uidlist->record_buf) /
			sizeof(*rec);
		rec = buffer_append_space_unsafe(uidlist->record_buf,
						 sizeof(*rec));
		memset(rec, 0, sizeof(*rec));
	}

	rec->flags = (rec->flags | flags) & ~MAILDIR_UIDLIST_REC_FLAG_NONSYNCED;
	rec->filename = p_strdup(uidlist->filename_pool, filename);
	hash_insert(uidlist->files, rec->filename, rec);
	return 1;
}

int maildir_uidlist_sync_next(struct maildir_uidlist_sync_ctx *ctx,
			      const char *filename,
			      enum maildir_uidlist_rec_flag flags)
{
	struct maildir_uidlist_rec *rec;
	int ret;

	if (ctx->failed)
		return -1;

	if (ctx->partial)
		return maildir_uidlist_sync_next_partial(ctx, filename, flags);

	rec = hash_lookup(ctx->files, filename);
	if (rec != NULL) {
		if ((rec->flags & (MAILDIR_UIDLIST_REC_FLAG_NEW_DIR |
				   MAILDIR_UIDLIST_REC_FLAG_MOVED)) == 0) {
			/* possibly duplicate */
			return 0;
		}

		rec->flags &= ~(MAILDIR_UIDLIST_REC_FLAG_NEW_DIR |
				MAILDIR_UIDLIST_REC_FLAG_MOVED);
	} else {
		rec = hash_lookup(ctx->uidlist->files, filename);
		if (rec == NULL && !ctx->synced) {
			ret = maildir_uidlist_sync_uidlist(ctx);
			if (ret < 0)
				return -1;
			if (ret == 0) {
				return maildir_uidlist_sync_next(ctx, filename,
								 flags);
			}
			rec = hash_lookup(ctx->uidlist->files, filename);
		}

		if (rec == NULL) {
			ctx->new_files = TRUE;
			rec = buffer_append_space_unsafe(ctx->new_record_buf,
							 sizeof(*rec));
			memset(rec, 0, sizeof(*rec));
		}
	}

	rec->flags |= flags;
	rec->filename = p_strdup(ctx->filename_pool, filename);
	hash_insert(ctx->files, rec->filename, rec);
	return 1;
}

static int maildir_time_cmp(const void *p1, const void *p2)
{
	const struct maildir_uidlist_rec *rec1 = p1, *rec2 = p2;
	const char *s1 = rec1->filename, *s2 = rec2->filename;
	time_t t1 = 0, t2 = 0;

	/* we have to do numeric comparision, strcmp() will break when
	   there's different amount of digits (mostly the 999999999 ->
	   1000000000 change in Sep 9 2001) */
	while (*s1 >= '0' && *s1 <= '9') {
		t1 = t1*10 + (*s1 - '0');
		s1++;
	}
	while (*s2 >= '0' && *s2 <= '9') {
		t2 = t2*10 + (*s2 - '0');
		s2++;
	}

	return t1 < t2 ? -1 : t1 > t2 ? 1 : 0;
}

static void maildir_uidlist_assign_uids(struct maildir_uidlist *uidlist,
					unsigned int first_new_pos)
{
	struct maildir_uidlist_rec *rec;
	unsigned int dest;
	size_t size;

	rec = buffer_get_modifyable_data(uidlist->record_buf, &size);
	size /= sizeof(*rec);

	/* sort new files and assign UIDs for them */
	qsort(rec + first_new_pos, size - first_new_pos,
	      sizeof(*rec), maildir_time_cmp);
	for (dest = first_new_pos; dest < size; dest++) {
		i_assert(rec[dest].uid == 0);
		rec[dest].uid = uidlist->next_uid++;
		rec[dest].flags &= ~MAILDIR_UIDLIST_REC_FLAG_MOVED;

		if ((rec[dest].flags & MAILDIR_UIDLIST_REC_FLAG_RECENT) != 0) {
			maildir_uidlist_mark_recent(uidlist,
						    rec[dest].uid);
		}
	}
}

static void maildir_uidlist_swap(struct maildir_uidlist_sync_ctx *ctx)
{
	struct maildir_uidlist *uidlist = ctx->uidlist;
	struct maildir_uidlist_rec *rec;
	void *key, *value;
	size_t size;
	unsigned int src, dest;

	/* @UNSAFE */

	rec = buffer_get_modifyable_data(uidlist->record_buf, &size);
	size /= sizeof(*rec);

	/* update filename pointers, skip deleted messages */
	for (dest = src = 0; src < size; src++) {
		if (!hash_lookup_full(ctx->files, rec[src].filename,
				      &key, &value))
			continue;

		rec[dest].uid = rec[src].uid;
		rec[dest].flags =
			rec[src].flags & ~MAILDIR_UIDLIST_REC_FLAG_MOVED;
		rec[dest].filename = key;
		if ((rec[dest].flags & MAILDIR_UIDLIST_REC_FLAG_RECENT) != 0) {
			maildir_uidlist_mark_recent(ctx->uidlist,
						    rec[dest].uid);
		}
		dest++;
	}
	buffer_set_used_size(uidlist->record_buf, dest * sizeof(*rec));

	buffer_append_buf(uidlist->record_buf, ctx->new_record_buf,
			  0, (size_t)-1);
        maildir_uidlist_assign_uids(uidlist, dest);

	hash_destroy(uidlist->files);
	uidlist->files = ctx->files;
	ctx->files = NULL;

	if (uidlist->filename_pool != NULL)
		pool_unref(uidlist->filename_pool);
	uidlist->filename_pool = ctx->filename_pool;
	ctx->filename_pool = NULL;
}

int maildir_uidlist_sync_deinit(struct maildir_uidlist_sync_ctx *ctx)
{
	int ret;

	if (ctx->failed)
		ret = -1;
	else {
		if (!ctx->partial)
			maildir_uidlist_swap(ctx);
		else {
			if (ctx->new_files) {
				maildir_uidlist_assign_uids(ctx->uidlist,
					ctx->partial_new_pos);
			}
			maildir_uidlist_mark_all(ctx->uidlist, FALSE);
		}

		if (!ctx->new_files)
			ret = 0;
		else
			ret = maildir_uidlist_rewrite(ctx->uidlist);
	}

	if (UIDLIST_IS_LOCKED(ctx->uidlist))
		maildir_uidlist_unlock(ctx->uidlist);

	if (ctx->files != NULL)
		hash_destroy(ctx->files);
	if (ctx->filename_pool != NULL)
		pool_unref(ctx->filename_pool);
	if (ctx->new_record_buf != NULL)
		buffer_free(ctx->new_record_buf);
	i_free(ctx);
	return ret;
}

struct maildir_uidlist_iter_ctx *
maildir_uidlist_iter_init(struct maildir_uidlist *uidlist)
{
	struct maildir_uidlist_iter_ctx *ctx;
	size_t size;

	ctx = i_new(struct maildir_uidlist_iter_ctx, 1);
	ctx->next = buffer_get_data(uidlist->record_buf, &size);
	size /= sizeof(*ctx->next);
	ctx->end = ctx->next + size;
	return ctx;
}

int maildir_uidlist_iter_next(struct maildir_uidlist_iter_ctx *ctx,
			      uint32_t *uid_r,
			      enum maildir_uidlist_rec_flag *flags_r,
			      const char **filename_r)
{
	if (ctx->next == ctx->end)
		return 0;

	*uid_r = ctx->next->uid;
	*flags_r = ctx->next->flags;
	*filename_r = ctx->next->filename;
	ctx->next++;
	return 1;
}

void maildir_uidlist_iter_deinit(struct maildir_uidlist_iter_ctx *ctx)
{
	i_free(ctx);
}
