/* Copyright (c) 2008-2009 Dovecot authors, see the included COPYING file */

#include "lib.h"
#include "array.h"
#include "hash.h"
#include "file-dotlock.h"
#include "nfs-workarounds.h"
#include "istream.h"
#include "ostream.h"
#include "dict-private.h"

#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>

struct file_dict {
	struct dict dict;
	pool_t hash_pool;

	char *path;
	struct hash_table *hash;
	int fd;
};

struct file_dict_iterate_context {
	struct dict_iterate_context ctx;

	struct hash_iterate_context *iter;
	char *path;
	unsigned int path_len;

	enum dict_iterate_flags flags;
	unsigned int failed:1;
};

enum file_dict_change_type {
	FILE_DICT_CHANGE_TYPE_SET,
	FILE_DICT_CHANGE_TYPE_UNSET,
	FILE_DICT_CHANGE_TYPE_INC
};

struct file_dict_change {
	enum file_dict_change_type type;
	const char *key;
	union {
		const char *str;
		long long diff;
	} value;
};

struct file_dict_transaction_context {
	struct dict_transaction_context ctx;

	pool_t pool;
	ARRAY_DEFINE(changes, struct file_dict_change);
};

static struct dotlock_settings file_dict_dotlock_settings = {
	MEMBER(temp_prefix) NULL,
	MEMBER(lock_suffix) NULL,

	MEMBER(timeout) 30,
	MEMBER(stale_timeout) 5
};

static struct dict *file_dict_init(struct dict *driver, const char *uri,
				   enum dict_data_type value_type ATTR_UNUSED,
				   const char *username ATTR_UNUSED)
{
	struct file_dict *dict;
	
	dict = i_new(struct file_dict, 1);
	dict->dict = *driver;
	dict->path = i_strdup(uri);
	dict->hash_pool = pool_alloconly_create("file dict", 1024);
	dict->hash = hash_table_create(default_pool, dict->hash_pool, 0,
				       str_hash, (hash_cmp_callback_t *)strcmp);
	dict->fd = -1;
	return &dict->dict;
}

static void file_dict_deinit(struct dict *_dict)
{
	struct file_dict *dict = (struct file_dict *)_dict;

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

static int file_dict_refresh(struct file_dict *dict)
{
	struct istream *input;
	char *key, *value;

	if (!file_dict_need_refresh(dict))
		return 0;

	if (dict->fd != -1) {
		if (close(dict->fd) < 0)
			i_error("close(%s) failed: %m", dict->path);
	}
	dict->fd = open(dict->path, O_RDONLY);
	if (dict->fd == -1) {
		if (errno == ENOENT)
			return 0;
		i_error("open(%s) failed: %m", dict->path);
		return -1;
	}

	hash_table_clear(dict->hash, TRUE);
	p_clear(dict->hash_pool);

	input = i_stream_create_fd(dict->fd, (size_t)-1, FALSE);
	while ((key = i_stream_read_next_line(input)) != NULL &&
	       (value = i_stream_read_next_line(input)) != NULL) {
		key = p_strdup(dict->hash_pool, key);
		value = p_strdup(dict->hash_pool, value);
		hash_table_insert(dict->hash, key, value);
	}
	i_stream_destroy(&input);
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
file_dict_iterate_init(struct dict *_dict, const char *path,
		       enum dict_iterate_flags flags)
{
        struct file_dict_iterate_context *ctx;
	struct file_dict *dict = (struct file_dict *)_dict;

	ctx = i_new(struct file_dict_iterate_context, 1);
	ctx->ctx.dict = _dict;
	ctx->path = i_strdup(path);
	ctx->path_len = strlen(path);
	ctx->flags = flags;
	ctx->iter = hash_table_iterate_init(dict->hash);

	if (file_dict_refresh(dict) < 0)
		ctx->failed = TRUE;
	return &ctx->ctx;
}

static int file_dict_iterate(struct dict_iterate_context *_ctx,
			     const char **key_r, const char **value_r)
{
	struct file_dict_iterate_context *ctx =
		(struct file_dict_iterate_context *)_ctx;
	void *key, *value;

	while (hash_table_iterate(ctx->iter, &key, &value)) {
		if (strncmp(ctx->path, key, ctx->path_len) != 0)
			continue;

		if ((ctx->flags & DICT_ITERATE_FLAG_RECURSE) == 0 &&
		    strchr((char *)key + ctx->path_len, '/') != NULL)
			continue;

		*key_r = key;
		*value_r = value;
		return 1;
	}
	return ctx->failed ? -1 : 0;
}

static void file_dict_iterate_deinit(struct dict_iterate_context *_ctx)
{
	struct file_dict_iterate_context *ctx =
		(struct file_dict_iterate_context *)_ctx;

	hash_table_iterate_deinit(&ctx->iter);
	i_free(ctx->path);
	i_free(ctx);
}

static struct dict_transaction_context *
file_dict_transaction_init(struct dict *_dict)
{
	struct file_dict_transaction_context *ctx;
	pool_t pool;

	pool = pool_alloconly_create("file dict transaction", 1024);
	ctx = p_new(pool, struct file_dict_transaction_context, 1);
	ctx->ctx.dict = _dict;
	ctx->pool = pool;
	p_array_init(&ctx->changes, pool, 32);
	return &ctx->ctx;
}

static void file_dict_apply_changes(struct file_dict_transaction_context *ctx)
{
	struct file_dict *dict = (struct file_dict *)ctx->ctx.dict;
	const char *tmp;
	char *key, *value, *old_value;
	void *orig_key, *orig_value;
	const struct file_dict_change *changes;
	unsigned int i, count, new_len;
	long long diff;

	changes = array_get(&ctx->changes, &count);
	for (i = 0; i < count; i++) {
		if (hash_table_lookup_full(dict->hash, changes[i].key,
					   &orig_key, &orig_value)) {
			key = orig_key;
			old_value = orig_value;
		} else {
			key = NULL;
			old_value = NULL;
		}
		value = NULL;

		switch (changes[i].type) {
		case FILE_DICT_CHANGE_TYPE_INC:
			diff = old_value == NULL ? 0 :
				strtoll(old_value, NULL, 10);
			diff += changes[i].value.diff;
			tmp = t_strdup_printf("%lld", diff);
			new_len = strlen(tmp);
			if (old_value == NULL || new_len > strlen(old_value))
				value = p_strdup(dict->hash_pool, tmp);
			else {
				memcpy(old_value, tmp, new_len + 1);
				value = old_value;
			}
			/* fall through */
		case FILE_DICT_CHANGE_TYPE_SET:
			if (key == NULL)
				key = p_strdup(dict->hash_pool, changes[i].key);
			if (value == NULL) {
				value = p_strdup(dict->hash_pool,
						 changes[i].value.str);
			}
			hash_table_update(dict->hash, key, value);
			break;
		case FILE_DICT_CHANGE_TYPE_UNSET:
			if (old_value != NULL)
				hash_table_remove(dict->hash, key);
			break;
		}
	}
}

static int fd_copy_permissions(int src_fd, const char *src_path,
			       int dest_fd, const char *dest_path)
{
	struct stat src_st, dest_st;

	if (fstat(src_fd, &src_st) < 0) {
		i_error("fstat(%s) failed: %m", src_path);
		return -1;
	}
	if (fstat(dest_fd, &dest_st) < 0) {
		i_error("fstat(%s) failed: %m", dest_path);
		return -1;
	}

	if (src_st.st_gid != dest_st.st_gid) {
		if (fchown(dest_fd, (uid_t)-1, src_st.st_gid) < 0) {
			i_error("fchown(%s, -1, %s) failed: %m",
				dest_path, dec2str(src_st.st_gid));
			return -1;
		}
	}

	if ((src_st.st_mode & 07777) != (dest_st.st_mode & 07777)) {
		if (fchmod(dest_fd, src_st.st_mode & 07777) < 0) {
			i_error("fchmod(%s, %o) failed: %m",
				dest_path, (int)(src_st.st_mode & 0777));
			return -1;
		}
	}
	return 0;
}

static int file_dict_write_changes(struct file_dict_transaction_context *ctx)
{
	struct file_dict *dict = (struct file_dict *)ctx->ctx.dict;
	struct dotlock *dotlock;
	struct hash_iterate_context *iter;
	struct ostream *output;
	void *key, *value;
	int fd;

	fd = file_dotlock_open(&file_dict_dotlock_settings, dict->path, 0,
			       &dotlock);
	if (fd == -1) {
		i_error("file dict commit: file_dotlock_open(%s) failed: %m",
			dict->path);
		return -1;
	}
	/* refresh once more now that we're locked */
	if (file_dict_refresh(dict) < 0) {
		file_dotlock_delete(&dotlock);
		return -1;
	}
	if (dict->fd != -1) {
		/* preserve the permissions */
		(void)fd_copy_permissions(dict->fd, dict->path, fd,
					  file_dotlock_get_lock_path(dotlock));
	}
	file_dict_apply_changes(ctx);

	output = o_stream_create_fd(fd, 0, FALSE);
	o_stream_cork(output);
	iter = hash_table_iterate_init(dict->hash);
	while (hash_table_iterate(iter, &key, &value)) {
		o_stream_send_str(output, key);
		o_stream_send(output, "\n", 1);
		o_stream_send_str(output, value);
		o_stream_send(output, "\n", 1);
	}
	hash_table_iterate_deinit(&iter);
	o_stream_destroy(&output);

	if (file_dotlock_replace(&dotlock,
				 DOTLOCK_REPLACE_FLAG_DONT_CLOSE_FD) < 0) {
		(void)close(fd);
		return -1;
	}

	if (dict->fd != -1)
		(void)close(dict->fd);
	dict->fd = fd;
	return 0;
}

static int file_dict_transaction_commit(struct dict_transaction_context *_ctx,
					bool async ATTR_UNUSED)
{
	struct file_dict_transaction_context *ctx =
		(struct file_dict_transaction_context *)_ctx;
	int ret;

	ret = file_dict_write_changes(ctx);
	pool_unref(&ctx->pool);
	return ret;
}

static void file_dict_transaction_rollback(struct dict_transaction_context *_ctx)
{
	struct file_dict_transaction_context *ctx =
		(struct file_dict_transaction_context *)_ctx;

	pool_unref(&ctx->pool);
}

static void file_dict_set(struct dict_transaction_context *_ctx,
			  const char *key, const char *value)
{
	struct file_dict_transaction_context *ctx =
		(struct file_dict_transaction_context *)_ctx;
	struct file_dict_change *change;

	change = array_append_space(&ctx->changes);
	change->type = FILE_DICT_CHANGE_TYPE_SET;
	change->key = p_strdup(ctx->pool, key);
	change->value.str = p_strdup(ctx->pool, value);
}

static void file_dict_unset(struct dict_transaction_context *_ctx,
			    const char *key)
{
	struct file_dict_transaction_context *ctx =
		(struct file_dict_transaction_context *)_ctx;
	struct file_dict_change *change;

	change = array_append_space(&ctx->changes);
	change->type = FILE_DICT_CHANGE_TYPE_UNSET;
	change->key = p_strdup(ctx->pool, key);
}

static void
file_dict_atomic_inc(struct dict_transaction_context *_ctx,
		     const char *key, long long diff)
{
	struct file_dict_transaction_context *ctx =
		(struct file_dict_transaction_context *)_ctx;
	struct file_dict_change *change;

	change = array_append_space(&ctx->changes);
	change->type = FILE_DICT_CHANGE_TYPE_INC;
	change->key = p_strdup(ctx->pool, key);
	change->value.diff = diff;
}

struct dict dict_driver_file = {
	MEMBER(name) "file",
	{
		file_dict_init,
		file_dict_deinit,
		file_dict_lookup,
		file_dict_iterate_init,
		file_dict_iterate,
		file_dict_iterate_deinit,
		file_dict_transaction_init,
		file_dict_transaction_commit,
		file_dict_transaction_rollback,
		file_dict_set,
		file_dict_unset,
		file_dict_atomic_inc
	}
};
