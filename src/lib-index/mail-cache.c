/* Copyright (c) 2003-2011 Dovecot authors, see the included COPYING file */

#include "lib.h"
#include "array.h"
#include "buffer.h"
#include "hash.h"
#include "nfs-workarounds.h"
#include "file-cache.h"
#include "mmap-util.h"
#include "write-full.h"
#include "mail-cache-private.h"

#include <unistd.h>

void mail_cache_set_syscall_error(struct mail_cache *cache,
				  const char *function)
{
	mail_index_file_set_syscall_error(cache->index, cache->filepath,
					  function);
}

static void mail_cache_unlink(struct mail_cache *cache)
{
	if (!cache->index->readonly)
		(void)unlink(cache->filepath);
}

void mail_cache_reset(struct mail_cache *cache)
{
	mail_cache_unlink(cache);
	/* mark the cache as unusable */
	cache->hdr = NULL;
}

void mail_cache_set_corrupted(struct mail_cache *cache, const char *fmt, ...)
{
	va_list va;

	mail_cache_reset(cache);

	va_start(va, fmt);
	T_BEGIN {
		mail_index_set_error(cache->index,
				     "Corrupted index cache file %s: %s",
				     cache->filepath,
				     t_strdup_vprintf(fmt, va));
	} T_END;
	va_end(va);
}

void mail_cache_file_close(struct mail_cache *cache)
{
	if (cache->mmap_base != NULL) {
		if (munmap(cache->mmap_base, cache->mmap_length) < 0)
			mail_cache_set_syscall_error(cache, "munmap()");
	}

	if (cache->file_cache != NULL)
		file_cache_set_fd(cache->file_cache, -1);

	cache->mmap_base = NULL;
	cache->data = NULL;
	cache->hdr = NULL;
	cache->mmap_length = 0;
	cache->last_field_header_offset = 0;

	if (cache->file_lock != NULL)
		file_lock_free(&cache->file_lock);
	cache->locked = FALSE;

	if (cache->fd != -1) {
		if (close(cache->fd) < 0)
			mail_cache_set_syscall_error(cache, "close()");
		cache->fd = -1;
	}
}

static void mail_cache_init_file_cache(struct mail_cache *cache)
{
	struct stat st;

	if (cache->file_cache == NULL)
		return;

	file_cache_set_fd(cache->file_cache, cache->fd);

	if (fstat(cache->fd, &st) == 0)
		file_cache_set_size(cache->file_cache, st.st_size);
	else if (!ESTALE_FSTAT(errno))
		mail_cache_set_syscall_error(cache, "fstat()");

	cache->st_ino = st.st_ino;
	cache->st_dev = st.st_dev;
}

static bool mail_cache_need_reopen(struct mail_cache *cache)
{
	struct stat st;

	if (MAIL_CACHE_IS_UNUSABLE(cache)) {
		if (cache->need_compress_file_seq != 0) {
			/* we're waiting for compression */
			return FALSE;
		}
		if (MAIL_INDEX_IS_IN_MEMORY(cache->index)) {
			/* disabled */
			return FALSE;
		}
	}

	if (cache->fd == -1)
		return TRUE;

	/* see if the file has changed */
	if ((cache->index->flags & MAIL_INDEX_OPEN_FLAG_NFS_FLUSH) != 0) {
		i_assert(!cache->locked);
		nfs_flush_file_handle_cache(cache->filepath);
	}
	if (nfs_safe_stat(cache->filepath, &st) < 0) {
		mail_cache_set_syscall_error(cache, "stat()");
		return TRUE;
	}

	if (st.st_ino != cache->st_ino ||
	    !CMP_DEV_T(st.st_dev, cache->st_dev)) {
		/* file changed */
		return TRUE;
	}

	if ((cache->index->flags & MAIL_INDEX_OPEN_FLAG_NFS_FLUSH) != 0) {
		/* if the old file has been deleted, the new file may have
		   the same inode as the old one. we'll catch this here by
		   checking if fstat() fails with ESTALE */
		if (fstat(cache->fd, &st) < 0) {
			if (ESTALE_FSTAT(errno))
				return TRUE;
			mail_cache_set_syscall_error(cache, "fstat()");
			return FALSE;
		}
	}
	return FALSE;
}

int mail_cache_reopen(struct mail_cache *cache)
{
	struct mail_index_view *view;
	const struct mail_index_ext *ext;

	i_assert(!cache->locked);

	if (!mail_cache_need_reopen(cache)) {
		/* reopening does no good */
		return 0;
	}

	mail_cache_file_close(cache);

	cache->fd = nfs_safe_open(cache->filepath,
				  cache->index->readonly ? O_RDONLY : O_RDWR);
	if (cache->fd == -1) {
		if (errno == ENOENT)
			cache->need_compress_file_seq = 0;
		else
			mail_cache_set_syscall_error(cache, "open()");
		return -1;
	}

	mail_cache_init_file_cache(cache);

	if (mail_cache_map(cache, 0, 0) < 0)
		return -1;

	if (mail_cache_header_fields_read(cache) < 0)
		return -1;

	view = mail_index_view_open(cache->index);
	ext = mail_index_view_get_ext(view, cache->ext_id);
	if (ext == NULL || cache->hdr->file_seq != ext->reset_id) {
		/* still different - maybe a race condition or maybe the
		   file_seq really is corrupted. either way, this shouldn't
		   happen often so we'll just mark cache to be compressed
		   later which fixes this. */
		cache->need_compress_file_seq = cache->hdr->file_seq;
		mail_index_view_close(&view);
		return 0;
	}

	mail_index_view_close(&view);
	i_assert(!MAIL_CACHE_IS_UNUSABLE(cache));
	return 1;
}

static void mail_cache_update_need_compress(struct mail_cache *cache)
{
	const struct mail_cache_header *hdr = cache->hdr;
	unsigned int cont_percentage;
	uoff_t max_del_space;

        cont_percentage = hdr->continued_record_count * 100 /
		(cache->index->map->rec_map->records_count == 0 ? 1 :
		 cache->index->map->rec_map->records_count);
	if (cont_percentage >= MAIL_CACHE_COMPRESS_CONTINUED_PERCENTAGE &&
	    hdr->used_file_size >= MAIL_CACHE_COMPRESS_MIN_SIZE) {
		/* too many continued rows, compress */
		cache->need_compress_file_seq = hdr->file_seq;
	}

	/* see if we've reached the max. deleted space in file */
	max_del_space = hdr->used_file_size / 100 *
		MAIL_CACHE_COMPRESS_PERCENTAGE;
	if (hdr->deleted_space >= max_del_space &&
	    hdr->used_file_size >= MAIL_CACHE_COMPRESS_MIN_SIZE)
		cache->need_compress_file_seq = hdr->file_seq;
}

static bool mail_cache_verify_header(struct mail_cache *cache)
{
	const struct mail_cache_header *hdr = cache->data;

	/* check that the header is still ok */
	if (cache->mmap_length < sizeof(struct mail_cache_header)) {
		mail_cache_set_corrupted(cache, "File too small");
		return FALSE;
	}

	if (hdr->version != MAIL_CACHE_VERSION) {
		/* version changed - upgrade silently */
		mail_cache_unlink(cache);
		return FALSE;
	}
	if (hdr->compat_sizeof_uoff_t != sizeof(uoff_t)) {
		/* architecture change - handle silently(?) */
		mail_cache_unlink(cache);
		return FALSE;
	}

	if (hdr->indexid != cache->index->indexid) {
		/* index id changed - handle silently */
		mail_cache_unlink(cache);
		return FALSE;
	}
	if (hdr->file_seq == 0) {
		mail_cache_set_corrupted(cache, "file_seq is 0");
		return FALSE;
	}

	/* only check the header if we're locked */
	if (!cache->locked)
		return TRUE;

	if (hdr->used_file_size < sizeof(struct mail_cache_header)) {
		mail_cache_set_corrupted(cache, "used_file_size too small");
		return FALSE;
	}
	if ((hdr->used_file_size % sizeof(uint32_t)) != 0) {
		mail_cache_set_corrupted(cache, "used_file_size not aligned");
		return FALSE;
	}

	if (cache->mmap_base != NULL &&
	    hdr->used_file_size > cache->mmap_length) {
		mail_cache_set_corrupted(cache, "used_file_size too large");
		return FALSE;
	}
	return TRUE;
}

int mail_cache_map(struct mail_cache *cache, size_t offset, size_t size)
{
	ssize_t ret;

	cache->remap_counter++;

	if (size == 0)
		size = sizeof(struct mail_cache_header);

	if (cache->file_cache != NULL) {
		cache->data = NULL;
		cache->hdr = NULL;

		ret = file_cache_read(cache->file_cache, offset, size);
		if (ret < 0) {
                        /* In case of ESTALE we'll simply fail without error
                           messages. The caller will then just have to
                           fallback to generating the value itself.

                           We can't simply reopen the cache flie, because
                           using it requires also having updated file
                           offsets. */
                        if (errno != ESTALE)
                                mail_cache_set_syscall_error(cache, "read()");
			return -1;
		}

		cache->data = file_cache_get_map(cache->file_cache,
						 &cache->mmap_length);

		if (offset == 0) {
			if (!mail_cache_verify_header(cache)) {
				cache->need_compress_file_seq =
					!MAIL_CACHE_IS_UNUSABLE(cache) &&
					cache->hdr->file_seq != 0 ?
					cache->hdr->file_seq : 0;
				return -1;
			}
			memcpy(&cache->hdr_ro_copy, cache->data,
			       sizeof(cache->hdr_ro_copy));
		}
		cache->hdr = &cache->hdr_ro_copy;
		if (offset == 0)
			mail_cache_update_need_compress(cache);
		return 0;
	}

	if (offset < cache->mmap_length &&
	    size <= cache->mmap_length - offset) {
		/* already mapped */
		return 0;
	}

	if (cache->mmap_base != NULL) {
		if (munmap(cache->mmap_base, cache->mmap_length) < 0)
			mail_cache_set_syscall_error(cache, "munmap()");
	} else {
		if (cache->fd == -1) {
			/* unusable, waiting for compression or
			   index is in memory */
			i_assert(cache->need_compress_file_seq != 0 ||
				 MAIL_INDEX_IS_IN_MEMORY(cache->index));
			return -1;
		}
	}

	/* map the whole file */
	cache->hdr = NULL;
	cache->mmap_length = 0;

	cache->mmap_base = mmap_ro_file(cache->fd, &cache->mmap_length);
	if (cache->mmap_base == MAP_FAILED) {
		cache->mmap_base = NULL;
		cache->data = NULL;
		mail_cache_set_syscall_error(cache, "mmap()");
		return -1;
	}
	cache->data = cache->mmap_base;

	if (!mail_cache_verify_header(cache)) {
		cache->need_compress_file_seq =
			!MAIL_CACHE_IS_UNUSABLE(cache) &&
			cache->hdr->file_seq != 0 ?
			cache->hdr->file_seq : 0;
		return -1;
	}

	cache->hdr = cache->data;
	if (offset == 0)
		mail_cache_update_need_compress(cache);
	return 0;
}

static int mail_cache_try_open(struct mail_cache *cache)
{
	cache->opened = TRUE;

	if (MAIL_INDEX_IS_IN_MEMORY(cache->index))
		return 0;

	cache->fd = nfs_safe_open(cache->filepath,
				  cache->index->readonly ? O_RDONLY : O_RDWR);
	if (cache->fd == -1) {
		if (errno == ENOENT) {
			cache->need_compress_file_seq = 0;
			return 0;
		}

		mail_cache_set_syscall_error(cache, "open()");
		return -1;
	}

	mail_cache_init_file_cache(cache);

	if (mail_cache_map(cache, 0, sizeof(struct mail_cache_header)) < 0)
		return -1;

	return 1;
}

int mail_cache_open_and_verify(struct mail_cache *cache)
{
	int ret;

	ret = mail_cache_try_open(cache);
	if (ret > 0)
		ret = mail_cache_header_fields_read(cache);
	if (ret < 0) {
		/* failed for some reason - doesn't really matter,
		   it's disabled for now. */
		mail_cache_file_close(cache);
	}
	return ret;
}

static struct mail_cache *mail_cache_alloc(struct mail_index *index)
{
	struct mail_cache *cache;

	cache = i_new(struct mail_cache, 1);
	cache->index = index;
	cache->fd = -1;
	cache->filepath =
		i_strconcat(index->filepath, MAIL_CACHE_FILE_SUFFIX, NULL);
	cache->field_pool = pool_alloconly_create("Cache fields", 1024);
	cache->field_name_hash =
		hash_table_create(default_pool, cache->field_pool, 0,
				  strcase_hash, (hash_cmp_callback_t *)strcasecmp);

	cache->dotlock_settings.use_excl_lock =
		(index->flags & MAIL_INDEX_OPEN_FLAG_DOTLOCK_USE_EXCL) != 0;
	cache->dotlock_settings.nfs_flush =
		(index->flags & MAIL_INDEX_OPEN_FLAG_NFS_FLUSH) != 0;
	cache->dotlock_settings.timeout =
		I_MIN(MAIL_CACHE_LOCK_TIMEOUT, index->max_lock_timeout_secs);
	cache->dotlock_settings.stale_timeout = MAIL_CACHE_LOCK_CHANGE_TIMEOUT;

	if (!MAIL_INDEX_IS_IN_MEMORY(index) &&
	    (index->flags & MAIL_INDEX_OPEN_FLAG_MMAP_DISABLE) != 0)
		cache->file_cache = file_cache_new(-1);

	cache->ext_id =
		mail_index_ext_register(index, "cache", 0,
					sizeof(uint32_t), sizeof(uint32_t));
	mail_index_register_expunge_handler(index, cache->ext_id, FALSE,
					    mail_cache_expunge_handler, cache);
	mail_index_register_sync_handler(index, cache->ext_id,
					 mail_cache_sync_handler,
                                         MAIL_INDEX_SYNC_HANDLER_FILE |
                                         MAIL_INDEX_SYNC_HANDLER_HEAD |
					 (cache->file_cache == NULL ? 0 :
					  MAIL_INDEX_SYNC_HANDLER_VIEW));

	if (cache->file_cache != NULL) {
		mail_index_register_sync_lost_handler(index,
			mail_cache_sync_lost_handler);
	}
	return cache;
}

struct mail_cache *mail_cache_open_or_create(struct mail_index *index)
{
	struct mail_cache *cache;

	cache = mail_cache_alloc(index);
	return cache;
}

struct mail_cache *mail_cache_create(struct mail_index *index)
{
	struct mail_cache *cache;

	cache = mail_cache_alloc(index);
	if (!MAIL_INDEX_IS_IN_MEMORY(index)) {
		if (unlink(cache->filepath) < 0 && errno != ENOENT)
			mail_cache_set_syscall_error(cache, "unlink()");
	}
	return cache;
}

void mail_cache_free(struct mail_cache **_cache)
{
	struct mail_cache *cache = *_cache;

	*_cache = NULL;
	if (cache->file_cache != NULL) {
		mail_index_unregister_sync_lost_handler(cache->index,
			mail_cache_sync_lost_handler);

		file_cache_free(&cache->file_cache);
	}

	mail_index_unregister_expunge_handler(cache->index, cache->ext_id);
	mail_index_unregister_sync_handler(cache->index, cache->ext_id);

	mail_cache_file_close(cache);

	hash_table_destroy(&cache->field_name_hash);
	pool_unref(&cache->field_pool);
	i_free(cache->field_file_map);
	i_free(cache->file_field_map);
	i_free(cache->fields);
	i_free(cache->filepath);
	i_free(cache);
}

static int mail_cache_lock_file(struct mail_cache *cache, bool nonblock)
{
	unsigned int timeout_secs;
	int ret;

	if (cache->last_lock_failed) {
		/* previous locking failed. don't waste time waiting on it
		   again, just try once to see if it's available now. */
		nonblock = TRUE;
	}

	if (cache->index->lock_method != FILE_LOCK_METHOD_DOTLOCK) {
		i_assert(cache->file_lock == NULL);
		timeout_secs = I_MIN(MAIL_CACHE_LOCK_TIMEOUT,
				     cache->index->max_lock_timeout_secs);

		ret = mail_index_lock_fd(cache->index, cache->filepath,
					 cache->fd, F_WRLCK,
					 nonblock ? 0 : timeout_secs,
					 &cache->file_lock);
	} else {
		enum dotlock_create_flags flags =
			nonblock ? DOTLOCK_CREATE_FLAG_NONBLOCK : 0;

		i_assert(cache->dotlock == NULL);
		ret = file_dotlock_create(&cache->dotlock_settings,
					  cache->filepath, flags,
					  &cache->dotlock);
		if (ret < 0) {
			mail_cache_set_syscall_error(cache,
						     "file_dotlock_create()");
		}
	}
	cache->last_lock_failed = ret <= 0;

	/* don't bother warning if locking failed due to a timeout. since cache
	   updating isn't all that important we're using a very short timeout
	   so it can be triggered sometimes on heavy load */
	if (ret <= 0)
		return ret;

	mail_index_flush_read_cache(cache->index, cache->filepath, cache->fd,
				    TRUE);
	return 1;
}

static void mail_cache_unlock_file(struct mail_cache *cache)
{
	if (cache->index->lock_method != FILE_LOCK_METHOD_DOTLOCK)
		file_unlock(&cache->file_lock);
	else
		(void)file_dotlock_delete(&cache->dotlock);
}

static int
mail_cache_lock_full(struct mail_cache *cache, bool require_same_reset_id,
		     bool nonblock)
{
	const struct mail_index_ext *ext;
	struct mail_index_view *iview;
	uint32_t reset_id;
	int i, ret;

	i_assert(!cache->locked);

	if (!cache->opened)
		(void)mail_cache_open_and_verify(cache);

	if (MAIL_CACHE_IS_UNUSABLE(cache) ||
	    MAIL_INDEX_IS_IN_MEMORY(cache->index) ||
	    cache->index->readonly)
		return 0;

	iview = mail_index_view_open(cache->index);
	ext = mail_index_view_get_ext(iview, cache->ext_id);
	reset_id = ext == NULL ? 0 : ext->reset_id;
	mail_index_view_close(&iview);

	if (ext == NULL && require_same_reset_id) {
		/* cache not used */
		return 0;
	}

	for (i = 0; i < 3; i++) {
		if (cache->hdr->file_seq != reset_id &&
		    (require_same_reset_id || i == 0)) {
			/* we want the latest cache file */
			if (reset_id < cache->hdr->file_seq) {
				/* either we're still waiting for index to
				   catch up with a cache compression, or
				   that catching up is never going to happen */
				ret = 0;
				break;
			}
			ret = mail_cache_reopen(cache);
			if (ret < 0 || (ret == 0 && require_same_reset_id))
				break;
		}

		if ((ret = mail_cache_lock_file(cache, nonblock)) <= 0) {
			ret = -1;
			break;
		}
		cache->locked = TRUE;

		if (cache->hdr->file_seq == reset_id ||
		    !require_same_reset_id) {
			/* got it */
			break;
		}

		/* okay, so it was just compressed. try again. */
		(void)mail_cache_unlock(cache);
		ret = 0;
	}

	if (ret > 0) {
		/* make sure our header is up to date */
		if (cache->file_cache != NULL) {
			file_cache_invalidate(cache->file_cache, 0,
					      sizeof(struct mail_cache_header));
		}
		if (mail_cache_map(cache, 0, 0) == 0)
			cache->hdr_copy = *cache->hdr;
		else {
			(void)mail_cache_unlock(cache);
			ret = -1;
		}
	}

	i_assert((ret <= 0 && !cache->locked) || (ret > 0 && cache->locked));
	return ret;
}

int mail_cache_lock(struct mail_cache *cache, bool require_same_reset_id)
{
	return mail_cache_lock_full(cache, require_same_reset_id, FALSE);
}

int mail_cache_try_lock(struct mail_cache *cache)
{
	return mail_cache_lock_full(cache, FALSE, TRUE);
}

int mail_cache_unlock(struct mail_cache *cache)
{
	int ret = 0;

	i_assert(cache->locked);

	if (cache->field_header_write_pending)
                ret = mail_cache_header_fields_update(cache);

	cache->locked = FALSE;

	if (MAIL_CACHE_IS_UNUSABLE(cache)) {
		/* we found it to be broken during the lock. just clean up. */
		cache->hdr_modified = FALSE;
		return -1;
	}

	if (cache->hdr_modified) {
		cache->hdr_modified = FALSE;
		if (mail_cache_write(cache, &cache->hdr_copy,
				     sizeof(cache->hdr_copy), 0) < 0)
			ret = -1;
		cache->hdr_ro_copy = cache->hdr_copy;
		mail_cache_update_need_compress(cache);
	}

	if (cache->index->fsync_mode == FSYNC_MODE_ALWAYS) {
		if (fdatasync(cache->fd) < 0)
			mail_cache_set_syscall_error(cache, "fdatasync()");
	}

	mail_cache_unlock_file(cache);
	return ret;
}

int mail_cache_write(struct mail_cache *cache, const void *data, size_t size,
		     uoff_t offset)
{
	if (pwrite_full(cache->fd, data, size, offset) < 0) {
		mail_cache_set_syscall_error(cache, "pwrite_full()");
		return -1;
	}

	if (cache->file_cache != NULL) {
		file_cache_write(cache->file_cache, data, size, offset);

		/* data pointer may change if file cache was grown */
		cache->data = file_cache_get_map(cache->file_cache,
						 &cache->mmap_length);
	}
	return 0;
}

struct mail_cache_view *
mail_cache_view_open(struct mail_cache *cache, struct mail_index_view *iview)
{
	struct mail_cache_view *view;

	view = i_new(struct mail_cache_view, 1);
	view->cache = cache;
	view->view = iview;
	view->cached_exists_buf =
		buffer_create_dynamic(default_pool,
				      cache->file_fields_count + 10);
	return view;
}

void mail_cache_view_close(struct mail_cache_view *view)
{
	i_assert(view->trans_view == NULL);

	if (view->cache->field_header_write_pending &&
	    !view->cache->compressing)
                (void)mail_cache_header_fields_update(view->cache);

	buffer_free(&view->cached_exists_buf);
	i_free(view);
}

uint32_t mail_cache_get_first_new_seq(struct mail_index_view *view)
{
	const struct mail_index_header *idx_hdr;
	uint32_t first_new_seq, message_count;

	idx_hdr = mail_index_get_header(view);
	if (idx_hdr->day_first_uid[7] == 0)
		return 1;

	if (!mail_index_lookup_seq_range(view, idx_hdr->day_first_uid[7],
					 (uint32_t)-1, &first_new_seq,
					 &message_count)) {
		/* all messages are too old */
		return message_count+1;
	}
	return first_new_seq;
}
