/* Copyright (C) 2003-2004 Timo Sirainen */

/*
   Locking should never fail or timeout. Exclusive locks must be kept as short
   time as possible. Shared locks can be long living, so if we can't get
   exclusive lock directly, we'll recreate the index. That means the shared
   lock holders can keep using the old file.

   lock_id is used to figure out if acquired lock is still valid. When index
   file is reopened, the lock_id can become invalid. It doesn't matter however,
   as no-one's going to modify the old file anymore.

   lock_id also tells us if we're referring to a shared or an exclusive lock.
   This allows us to drop back to shared locking once all exclusive locks
   are dropped. Shared locks have even numbers, exclusive locks have odd numbers.
   The number is increased by two every time the lock is dropped or index file
   is reopened.
*/

#include "lib.h"
#include "mail-index-private.h"

int mail_index_lock_fd(struct mail_index *index, const char *path, int fd,
		       int lock_type, unsigned int timeout_secs,
		       struct file_lock **lock_r)
{
	if (fd == -1) {
		i_assert(MAIL_INDEX_IS_IN_MEMORY(index));
		return 1;
	}

	return file_wait_lock(fd, path, lock_type, index->lock_method,
			      timeout_secs, lock_r);
}

static int mail_index_lock(struct mail_index *index, int lock_type,
			   unsigned int timeout_secs, unsigned int *lock_id_r)
{
	int ret;

	i_assert(lock_type == F_RDLCK || lock_type == F_WRLCK);

	if (lock_type == F_RDLCK && index->lock_type != F_UNLCK) {
		index->shared_lock_count++;
		*lock_id_r = index->lock_id_counter;
		ret = 1;
	} else if (lock_type == F_WRLCK && index->lock_type == F_WRLCK) {
		index->excl_lock_count++;
		*lock_id_r = index->lock_id_counter + 1;
		ret = 1;
	} else {
		ret = 0;
	}

	if (ret > 0) {
		/* file is already locked */
		return 1;
	}

	if (index->lock_method == FILE_LOCK_METHOD_DOTLOCK &&
	    !MAIL_INDEX_IS_IN_MEMORY(index)) {
		/* FIXME: exclusive locking will rewrite the index file every
		   time. shouldn't really be needed.. reading doesn't require
		   locks then, though */
		if (lock_type == F_WRLCK)
			return 0;

		index->shared_lock_count++;
		index->lock_type = F_RDLCK;
		*lock_id_r = index->lock_id_counter;
		return 1;
	}

	if (lock_type == F_RDLCK || !index->log_locked) {
		i_assert(index->file_lock == NULL);
		ret = mail_index_lock_fd(index, index->filepath, index->fd,
					 lock_type, timeout_secs,
					 &index->file_lock);
	} else {
		/* this is kind of kludgy. we wish to avoid deadlocks while
		   trying to lock transaction log, but it can happen if our
		   process is holding transaction log lock and waiting for
		   index write lock, while the other process is holding index
		   read lock and waiting for transaction log lock.

		   we don't have a problem with grabbing read index lock
		   because the only way for it to block is if it's
		   write-locked, which isn't allowed unless transaction log
		   is also locked.

		   so, the workaround for this problem is that we simply try
		   locking once. if it doesn't work, just rewrite the file.
		   hopefully there won't be any other deadlocking issues. :) */
		if (index->file_lock == NULL) {
			ret = mail_index_lock_fd(index, index->filepath,
						 index->fd, lock_type, 0,
						 &index->file_lock);
		} else {
			ret = file_lock_try_update(index->file_lock, lock_type);
		}
	}
	if (ret <= 0)
		return ret;

	if (index->lock_type == F_UNLCK)
		index->lock_id_counter += 2;
	index->lock_type = lock_type;

	if (lock_type == F_RDLCK) {
		index->shared_lock_count++;
		*lock_id_r = index->lock_id_counter;
	} else {
		index->excl_lock_count++;
		*lock_id_r = index->lock_id_counter + 1;
	}

	return 1;
}

int mail_index_lock_shared(struct mail_index *index, unsigned int *lock_id_r)
{
	int ret;

	ret = mail_index_lock(index, F_RDLCK, MAIL_INDEX_LOCK_SECS, lock_id_r);
	if (ret > 0)
		return 0;
	if (ret < 0)
		return -1;

	mail_index_set_error(index,
		"Timeout while waiting for shared lock for index file %s",
		index->filepath);
	index->index_lock_timeout = TRUE;
	return -1;
}

int mail_index_try_lock_exclusive(struct mail_index *index,
				  unsigned int *lock_id_r)
{
	return mail_index_lock(index, F_WRLCK, 0, lock_id_r);
}

void mail_index_unlock(struct mail_index *index, unsigned int *_lock_id)
{
	unsigned int lock_id = *_lock_id;

	*_lock_id = 0;

	if ((lock_id & 1) == 0) {
		/* shared lock */
		if (!mail_index_is_locked(index, lock_id)) {
			/* unlocking some older generation of the index file.
			   we've already closed the file so just ignore this. */
			return;
		}

		i_assert(index->shared_lock_count > 0);
		index->shared_lock_count--;
	} else {
		/* exclusive lock */
		i_assert(lock_id == index->lock_id_counter + 1);
		i_assert(index->excl_lock_count > 0);
		i_assert(index->lock_type == F_WRLCK);
		if (--index->excl_lock_count == 0 &&
		    index->shared_lock_count > 0) {
			/* drop back to a shared lock. */
			index->lock_type = F_RDLCK;
			(void)file_lock_try_update(index->file_lock, F_RDLCK);
		}
	}

	if (index->shared_lock_count == 0 && index->excl_lock_count == 0) {
		index->lock_id_counter += 2;
		index->lock_type = F_UNLCK;
		if (index->lock_method != FILE_LOCK_METHOD_DOTLOCK) {
			if (!MAIL_INDEX_IS_IN_MEMORY(index))
				file_unlock(&index->file_lock);
		}
		i_assert(index->file_lock == NULL);
	}
}

bool mail_index_is_locked(struct mail_index *index, unsigned int lock_id)
{
	if ((index->lock_id_counter ^ lock_id) <= 1 && lock_id != 0) {
		i_assert(index->lock_type != F_UNLCK);
		return TRUE;
	}

	return FALSE;
}
