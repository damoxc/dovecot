/* Copyright (c) 2008-2013 Dovecot authors, see the included COPYING file */

#include "lib.h"
#include "array.h"
#include "hash.h"
#include "mkdir-parents.h"
#include "file-lock.h"
#include "file-dotlock.h"
#include "nfs-workarounds.h"
#include "istream.h"
#include "ostream.h"
#include "dict-transaction-memory.h"
#include "dict-private.h"

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>

struct file_dict {
	struct dict dict;
	pool_t hash_pool;
	enum file_lock_method lock_method;

	char *path;
	HASH_TABLE(char *, char *) hash;
	int fd;

	bool refreshed;
};

struct file_dict_iterate_path {
	const char *path;
	unsigned int len;
};

struct file_dict_iterate_context {
	struct dict_iterate_context ctx;
	pool_t pool;

	struct hash_iterate_context *iter;
	struct file_dict_iterate_path *paths;

	enum dict_iterate_flags flags;
	unsigned int failed:1;
};

static struct dotlock_settings file_dict_dotlock_settings = {
	.timeout = 60*2,
	.stale_timeout = 60,
	.use_io_notify = TRUE
};

static int
file_dict_init(struct dict *driver, const char *uri,
	       enum dict_data_type value_type ATTR_UNUSED,
	       const char *username ATTR_UNUSED,
	       const char *base_dir ATTR_UNUSED, struct dict **dict_r,
	       const char **error_r)
{
	struct file_dict *dict;
	const char *p;

	dict = i_new(struct file_dict, 1);
	dict->lock_method = FILE_LOCK_METHOD_DOTLOCK;

	p = strchr(uri, ':');
	if (p == NULL) {
		/* no parameters */
		dict->path = i_strdup(uri);
	} else {
		dict->path = i_strdup_until(uri, p++);
		if (strcmp(p, "lock=fcntl") == 0)
			dict->lock_method = FILE_LOCK_METHOD_FCNTL;
		else if (strcmp(p, "lock=flock") == 0)
			dict->lock_method = FILE_LOCK_METHOD_FLOCK;
		else {
			*error_r = t_strdup_printf("Invalid parameter: %s", p+1);
			i_free(dict->path);
			i_free(dict);
			return -1;
		}
	}
	dict->dict = *driver;
	dict->hash_pool = pool_alloconly_create("file dict", 1024);
	hash_table_create(&dict->hash, dict->hash_pool, 0, str_hash, strcmp);
	dict->fd = -1;
	*dict_r = &dict->dict;
	return 0;
}

static void file_dict_deinit(struct dict *_dict)
{
	struct file_dict *dict = (struct file_dict *)_dict;

	if (dict->fd != -1) {
		if (close(dict->fd) < 0)
			i_error("close(%s) failed: %m", dict->path);
	}
	hash_table_destroy(&dict->hash);
	pool_unref(&dict->hash_pool);
	i_free(dict->path);
	i_free(dict);
}

static bool file_dict_need_refresh(struct file_dict *dict)
{
	struct stat st1, st2;

	if (dict->fd == -1)
		return TRUE;

	nfs_flush_file_handle_cache(dict->path);
	if (nfs_safe_stat(dict->path, &st1) < 0) {
		i_error("stat(%s) failed: %m", dict->path);
		return FALSE;
	}

	if (fstat(dict->fd, &st2) < 0) {
		if (errno != ESTALE)
			i_error("fstat(%s) failed: %m", dict->path);
		return TRUE;
	}
	if (st1.st_ino != st2.st_ino ||
	    !CMP_DEV_T(st1.st_dev, st2.st_dev)) {
		/* file changed */
		return TRUE;
	}
	return FALSE;
}

static int file_dict_open_latest(struct file_dict *dict)
{
	int open_type;

	if (!file_dict_need_refresh(dict))
		return 0;

	if (dict->fd != -1) {
		if (close(dict->fd) < 0)
			i_error("close(%s) failed: %m", dict->path);
	}

	open_type = dict->lock_method == FILE_LOCK_METHOD_DOTLOCK ?
		O_RDONLY : O_RDWR;
	dict->fd = open(dict->path, open_type);
	if (dict->fd == -1) {
		if (errno == ENOENT)
			return 0;
		i_error("open(%s) failed: %m", dict->path);
		return -1;
	}
	dict->refreshed = FALSE;
	return 1;
}

static int file_dict_refresh(struct file_dict *dict)
{
	struct istream *input;
	char *key, *value;

	if (file_dict_open_latest(dict) < 0)
		return -1;
	if (dict->refreshed)
		return 0;

	hash_table_clear(dict->hash, TRUE);
	p_clear(dict->hash_pool);

	if (dict->fd != -1) {
		input = i_stream_create_fd(dict->fd, (size_t)-1, FALSE);

		while ((key = i_stream_read_next_line(input)) != NULL) {
			/* strdup() before the second read */
			key = p_strdup(dict->hash_pool, key);

			if ((value = i_stream_read_next_line(input)) == NULL)
				break;

			value = p_strdup(dict->hash_pool, value);
			hash_table_insert(dict->hash, key, value);
		}
		i_stream_destroy(&input);
	}
	dict->refreshed = TRUE;
	return 0;
}

static int file_dict_lookup(struct dict *_dict, pool_t pool,
			    const char *key, const char **value_r)
{
	struct file_dict *dict = (struct file_dict *)_dict;

	if (file_dict_refresh(dict) < 0)
		return -1;

	*value_r = p_strdup(pool, hash_table_lookup(dict->hash, key));
	return *value_r == NULL ? 0 : 1;
}

static struct dict_iterate_context *
file_dict_iterate_init(struct dict *_dict, const char *const *paths,
		       enum dict_iterate_flags flags)
{
        struct file_dict_iterate_context *ctx;
	struct file_dict *dict = (struct file_dict *)_dict;
	unsigned int i, path_count;
	pool_t pool;

	pool = pool_alloconly_create("file dict iterate", 256);
	ctx = p_new(pool, struct file_dict_iterate_context, 1);
	ctx->ctx.dict = _dict;
	ctx->pool = pool;

	for (path_count = 0; paths[path_count] != NULL; path_count++) ;
	ctx->paths = p_new(pool, struct file_dict_iterate_path, path_count + 1);
	for (i = 0; i < path_count; i++) {
		ctx->paths[i].path = p_strdup(pool, paths[i]);
		ctx->paths[i].len = strlen(paths[i]);
	}
	ctx->flags = flags;
	ctx->iter = hash_table_iterate_init(dict->hash);

	if (file_dict_refresh(dict) < 0)
		ctx->failed = TRUE;
	return &ctx->ctx;
}

static const struct file_dict_iterate_path *
file_dict_iterate_find_path(struct file_dict_iterate_context *ctx,
			    const char *key)
{
	unsigned int i;

	for (i = 0; ctx->paths[i].path != NULL; i++) {
		if (strncmp(ctx->paths[i].path, key, ctx->paths[i].len) == 0)
			return &ctx->paths[i];
	}
	return NULL;
}

static bool file_dict_iterate(struct dict_iterate_context *_ctx,
			      const char **key_r, const char **value_r)
{
	struct file_dict_iterate_context *ctx =
		(struct file_dict_iterate_context *)_ctx;
	const struct file_dict_iterate_path *path;
	char *key, *value;

	while (hash_table_iterate(ctx->iter,
				  ((struct file_dict *)_ctx->dict)->hash,
				  &key, &value)) {
		path = file_dict_iterate_find_path(ctx, key);
		if (path == NULL)
			continue;

		if ((ctx->flags & DICT_ITERATE_FLAG_RECURSE) == 0 &&
		    strchr(key + path->len, '/') != NULL)
			continue;

		*key_r = key;
		*value_r = value;
		return TRUE;
	}
	return FALSE;
}

static int file_dict_iterate_deinit(struct dict_iterate_context *_ctx)
{
	struct file_dict_iterate_context *ctx =
		(struct file_dict_iterate_context *)_ctx;
	int ret = ctx->failed ? -1 : 0;

	hash_table_iterate_deinit(&ctx->iter);
	pool_unref(&ctx->pool);
	return ret;
}

static struct dict_transaction_context *
file_dict_transaction_init(struct dict *_dict)
{
	struct dict_transaction_memory_context *ctx;
	pool_t pool;

	pool = pool_alloconly_create("file dict transaction", 2048);
	ctx = p_new(pool, struct dict_transaction_memory_context, 1);
	dict_transaction_memory_init(ctx, _dict, pool);
	return &ctx->ctx;
}

static void file_dict_apply_changes(struct dict_transaction_memory_context *ctx,
				    bool *atomic_inc_not_found_r)
{
	struct file_dict *dict = (struct file_dict *)ctx->ctx.dict;
	const char *tmp;
	char *key, *value, *old_value;
	char *orig_key, *orig_value;
	const struct dict_transaction_memory_change *change;
	unsigned int new_len;
	long long diff;

	array_foreach(&ctx->changes, change) {
		if (hash_table_lookup_full(dict->hash, change->key,
					   &orig_key, &orig_value)) {
			key = orig_key;
			old_value = orig_value;
		} else {
			key = NULL;
			old_value = NULL;
		}
		value = NULL;

		switch (change->type) {
		case DICT_CHANGE_TYPE_INC:
			if (old_value == NULL) {
				*atomic_inc_not_found_r = TRUE;
				break;
			}
			diff = strtoll(old_value, NULL, 10) +
				change->value.diff;
			tmp = t_strdup_printf("%lld", diff);
			new_len = strlen(tmp);
			if (old_value == NULL || new_len > strlen(old_value))
				value = p_strdup(dict->hash_pool, tmp);
			else {
				memcpy(old_value, tmp, new_len + 1);
				value = old_value;
			}
			/* fall through */
		case DICT_CHANGE_TYPE_SET:
			if (key == NULL)
				key = p_strdup(dict->hash_pool, change->key);
			if (value == NULL) {
				value = p_strdup(dict->hash_pool,
						 change->value.str);
			}
			hash_table_update(dict->hash, key, value);
			break;
		case DICT_CHANGE_TYPE_APPEND:
			if (key == NULL)
				key = p_strdup(dict->hash_pool, change->key);
			if (old_value == NULL) {
				value = p_strdup(dict->hash_pool,
						 change->value.str);
			} else {
				value = p_strconcat(dict->hash_pool, old_value,
						    change->value.str, NULL);
			}
			hash_table_update(dict->hash, key, value);
			break;
		case DICT_CHANGE_TYPE_UNSET:
			if (old_value != NULL)
				hash_table_remove(dict->hash, key);
			break;
		}
	}
}

static int
fd_copy_stat_permissions(const struct stat *src_st,
			 int dest_fd, const char *dest_path)
{
	struct stat dest_st;

	if (fstat(dest_fd, &dest_st) < 0) {
		i_error("fstat(%s) failed: %m", dest_path);
		return -1;
	}

	if (src_st->st_gid != dest_st.st_gid &&
	    ((src_st->st_mode & 0070) >> 3 != (src_st->st_mode & 0007))) {
		/* group has different permissions from world.
		   preserve the group. */
		if (fchown(dest_fd, (uid_t)-1, src_st->st_gid) < 0) {
			i_error("fchown(%s, -1, %s) failed: %m",
				dest_path, dec2str(src_st->st_gid));
			return -1;
		}
	}

	if ((src_st->st_mode & 07777) != (dest_st.st_mode & 07777)) {
		if (fchmod(dest_fd, src_st->st_mode & 07777) < 0) {
			i_error("fchmod(%s, %o) failed: %m",
				dest_path, (int)(src_st->st_mode & 0777));
			return -1;
		}
	}
	return 0;
}

static int fd_copy_permissions(int src_fd, const char *src_path,
			       int dest_fd, const char *dest_path)
{
	struct stat src_st;

	if (fstat(src_fd, &src_st) < 0) {
		i_error("fstat(%s) failed: %m", src_path);
		return -1;
	}
	return fd_copy_stat_permissions(&src_st, dest_fd, dest_path);
}

static int
fd_copy_parent_dir_permissions(const char *src_path, int dest_fd,
			       const char *dest_path)
{
	struct stat src_st;
	const char *src_dir, *p;

	p = strrchr(src_path, '/');
	if (p == NULL)
		src_dir = ".";
	else
		src_dir = t_strdup_until(src_path, p);
	if (stat(src_dir, &src_st) < 0) {
		i_error("stat(%s) failed: %m", src_dir);
		return -1;
	}
	src_st.st_mode &= 0666;
	return fd_copy_stat_permissions(&src_st, dest_fd, dest_path);
}

static int file_dict_mkdir(struct file_dict *dict)
{
	const char *path, *p, *root;
	struct stat st;
	mode_t mode = 0700;

	p = strrchr(dict->path, '/');
	if (p == NULL)
		return 0;
	path = t_strdup_until(dict->path, p);

	if (stat_first_parent(path, &root, &st) < 0) {
		i_error("stat(%s) failed: %m", root);
		return -1;
	}
	if ((st.st_mode & S_ISGID) != 0) {
		/* preserve parent's permissions when it has setgid bit */
		mode = st.st_mode;
	}

	if (mkdir_parents(path, mode) < 0) {
		i_error("mkdir_parents(%s) failed: %m", path);
		return -1;
	}
	return 0;
}

static int
file_dict_lock(struct file_dict *dict, struct file_lock **lock_r)
{
	int ret;

	if (file_dict_open_latest(dict) < 0)
		return -1;

	if (dict->fd == -1) {
		/* quota file doesn't exist yet, we need to create it */
		dict->fd = open(dict->path, O_CREAT | O_RDWR, 0600);
		if (dict->fd == -1 && errno == ENOENT) {
			if (file_dict_mkdir(dict) < 0)
				return -1;
			dict->fd = open(dict->path, O_CREAT | O_RDWR, 0600);
		}
		if (dict->fd == -1) {
			i_error("creat(%s) failed: %m", dict->path);
			return -1;
		}
		(void)fd_copy_parent_dir_permissions(dict->path, dict->fd,
						     dict->path);
	}

	do {
		if (file_wait_lock(dict->fd, dict->path, F_WRLCK,
				   dict->lock_method,
				   file_dict_dotlock_settings.timeout,
				   lock_r) <= 0) {
			i_error("file_wait_lock(%s) failed: %m", dict->path);
			return -1;
		}
		/* check again if we need to reopen the file because it was
		   just replaced */
	} while ((ret = file_dict_open_latest(dict)) > 0);

	return ret < 0 ? -1 : 0;
}

static int file_dict_write_changes(struct dict_transaction_memory_context *ctx,
				   bool *atomic_inc_not_found_r)
{
	struct file_dict *dict = (struct file_dict *)ctx->ctx.dict;
	struct dotlock *dotlock = NULL;
	struct file_lock *lock = NULL;
	const char *temp_path = NULL;
	struct hash_iterate_context *iter;
	struct ostream *output;
	char *key, *value;
	int fd = -1;

	*atomic_inc_not_found_r = FALSE;

	switch (dict->lock_method) {
	case FILE_LOCK_METHOD_FCNTL:
	case FILE_LOCK_METHOD_FLOCK:
		if (file_dict_lock(dict, &lock) < 0)
			return -1;
		temp_path = t_strdup_printf("%s.tmp", dict->path);
		fd = creat(temp_path, 0600);
		if (fd == -1) {
			i_error("file dict commit: creat(%s) failed: %m",
				temp_path);
			return -1;
		}
		break;
	case FILE_LOCK_METHOD_DOTLOCK:
		fd = file_dotlock_open(&file_dict_dotlock_settings, dict->path, 0,
				       &dotlock);
		if (fd == -1 && errno == ENOENT) {
			if (file_dict_mkdir(dict) < 0)
				return -1;
			fd = file_dotlock_open(&file_dict_dotlock_settings,
					       dict->path, 0, &dotlock);
		}
		if (fd == -1) {
			i_error("file dict commit: file_dotlock_open(%s) failed: %m",
				dict->path);
			return -1;
		}
		temp_path = file_dotlock_get_lock_path(dotlock);
		break;
	}

	/* refresh once more now that we're locked */
	if (file_dict_refresh(dict) < 0) {
		if (dotlock != NULL)
			file_dotlock_delete(&dotlock);
		else {
			i_close_fd(&fd);
			file_unlock(&lock);
		}
		return -1;
	}
	if (dict->fd != -1) {
		/* preserve the permissions */
		(void)fd_copy_permissions(dict->fd, dict->path, fd, temp_path);
	} else {
		/* get initial permissions from parent directory */
		(void)fd_copy_parent_dir_permissions(dict->path, fd, temp_path);
	}
	file_dict_apply_changes(ctx, atomic_inc_not_found_r);

	output = o_stream_create_fd(fd, 0, FALSE);
	o_stream_cork(output);
	iter = hash_table_iterate_init(dict->hash);
	while (hash_table_iterate(iter, dict->hash, &key, &value)) {
		o_stream_nsend_str(output, key);
		o_stream_nsend(output, "\n", 1);
		o_stream_nsend_str(output, value);
		o_stream_nsend(output, "\n", 1);
	}
	hash_table_iterate_deinit(&iter);

	if (o_stream_nfinish(output) < 0) {
		i_error("write(%s) failed: %m", temp_path);
		o_stream_destroy(&output);
		i_close_fd(&fd);
		return -1;
	}
	o_stream_destroy(&output);

	if (dotlock != NULL) {
		if (file_dotlock_replace(&dotlock,
				DOTLOCK_REPLACE_FLAG_DONT_CLOSE_FD) < 0) {
			i_close_fd(&fd);
			return -1;
		}
	} else {
		if (rename(temp_path, dict->path) < 0) {
			i_error("rename(%s, %s) failed: %m",
				temp_path, dict->path);
			file_unlock(&lock);
			i_close_fd(&fd);
			return -1;
		}
		file_lock_free(&lock);
	}

	if (dict->fd != -1)
		i_close_fd(&dict->fd);
	dict->fd = fd;
	return 0;
}

static int
file_dict_transaction_commit(struct dict_transaction_context *_ctx,
			     bool async ATTR_UNUSED,
			     dict_transaction_commit_callback_t *callback,
			     void *context)
{
	struct dict_transaction_memory_context *ctx =
		(struct dict_transaction_memory_context *)_ctx;
	bool atomic_inc_not_found;
	int ret;

	if (file_dict_write_changes(ctx, &atomic_inc_not_found) < 0)
		ret = -1;
	else if (atomic_inc_not_found)
		ret = 0;
	else
		ret = 1;
	pool_unref(&ctx->pool);

	if (callback != NULL)
		callback(ret, context);
	return ret;
}

struct dict dict_driver_file = {
	.name = "file",
	{
		file_dict_init,
		file_dict_deinit,
		NULL,
		file_dict_lookup,
		file_dict_iterate_init,
		file_dict_iterate,
		file_dict_iterate_deinit,
		file_dict_transaction_init,
		file_dict_transaction_commit,
		dict_transaction_memory_rollback,
		dict_transaction_memory_set,
		dict_transaction_memory_unset,
		dict_transaction_memory_append,
		dict_transaction_memory_atomic_inc
	}
};
