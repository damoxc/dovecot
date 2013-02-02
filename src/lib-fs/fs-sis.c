* Copyright (c) 2010-2013 Dovecot authors, see the included COPYING file */

#include "lib.h"
#include "str.h"
#include "istream.h"
#include "ostream.h"
#include "ostream-cmp.h"
#include "fs-sis-common.h"

#define FS_SIS_REQUIRED_PROPS \
	(FS_PROPERTY_FASTCOPY | FS_PROPERTY_STAT)

struct sis_fs {
	struct fs fs;
	struct fs *super;
};

struct sis_fs_file {
	struct fs_file file;
	struct sis_fs *fs;
	struct fs_file *super;
	enum fs_open_mode open_mode;

	struct fs_file *hash_file;
	struct istream *hash_input;
	struct ostream *fs_output;

	char *hash, *hash_path;
	bool opened;
};

static void fs_sis_copy_error(struct sis_fs *fs)
{
	fs_set_error(&fs->fs, "%s", fs_last_error(fs->super));
}

static void fs_sis_file_copy_error(struct sis_fs_file *file)
{
	struct sis_fs *fs = (struct sis_fs *)file->file.fs;

	fs_sis_copy_error(fs);
}

static struct fs *fs_sis_alloc(void)
{
	struct sis_fs *fs;

	fs = i_new(struct sis_fs, 1);
	fs->fs = fs_class_sis;
	return &fs->fs;
}

static int
fs_sis_init(struct fs *_fs, const char *args, const struct fs_settings *set)
{
	struct sis_fs *fs = (struct sis_fs *)_fs;
	enum fs_properties props;
	const char *parent_name, *parent_args, *error;

	if (*args == '\0') {
		fs_set_error(_fs, "Parent filesystem not given as parameter");
		return -1;
	}

	parent_args = strchr(args, ':');
	if (parent_args == NULL) {
		parent_name = args;
		parent_args = "";
	} else {
		parent_name = t_strdup_until(args, parent_args);
		parent_args++;
	}
	if (fs_init(parent_name, parent_args, set, &fs->super, &error) < 0) {
		fs_set_error(_fs, "%s: %s", parent_name, error);
		return -1;
	}
	props = fs_get_properties(fs->super);
	if ((props & FS_SIS_REQUIRED_PROPS) != FS_SIS_REQUIRED_PROPS) {
		fs_set_error(_fs, "%s backend can't be used with SIS",
			     parent_name);
		return -1;
	}
	return 0;
}

static void fs_sis_deinit(struct fs *_fs)
{
	struct sis_fs *fs = (struct sis_fs *)_fs;

	if (fs->super != NULL)
		fs_deinit(&fs->super);
	i_free(fs);
}

static enum fs_properties fs_sis_get_properties(struct fs *_fs)
{
	struct sis_fs *fs = (struct sis_fs *)_fs;

	return fs_get_properties(fs->super);
}

static struct fs_file *
fs_sis_file_init(struct fs *_fs, const char *path,
		 enum fs_open_mode mode, enum fs_open_flags flags)
{
	struct sis_fs *fs = (struct sis_fs *)_fs;
	struct sis_fs_file *file;
	const char *dir, *hash;

	file = i_new(struct sis_fs_file, 1);
	file->file.fs = _fs;
	file->file.path = i_strdup(path);
	file->fs = fs;
	file->open_mode = mode;
	if (mode == FS_OPEN_MODE_APPEND) {
		fs_set_error(_fs, "APPEND mode not supported");
		return &file->file;
	}

	if (fs_sis_path_parse(_fs, path, &dir, &hash) < 0) {
		fs_set_error(_fs, "Invalid path");
		return &file->file;
	}

	/* if hashes/<hash> already exists, open it */
	file->hash_path = i_strdup_printf("%s/"HASH_DIR_NAME"/%s", dir, hash);
	file->hash_file = fs_file_init(fs->super, file->hash_path,
				       FS_OPEN_MODE_READONLY);

	file->hash_input = fs_read_stream(file->hash_file, IO_BLOCK_SIZE);
	if (i_stream_read(file->hash_input) == -1) {
		/* doesn't exist */
		if (errno != ENOENT) {
			i_error("fs-sis: Couldn't read hash file %s: %m",
				file->hash_path);
		}
		i_stream_destroy(&file->hash_input);
	}

	file->super = fs_file_init(fs->super, path, mode | flags);
	return &file->file;
}

static void fs_sis_file_deinit(struct fs_file *_file)
{
	struct sis_fs_file *file = (struct sis_fs_file *)_file;

	if (file->hash_input != NULL)
		i_stream_unref(&file->hash_input);
	fs_file_deinit(&file->hash_file);
	fs_file_deinit(&file->super);
	i_free(file->hash);
	i_free(file->hash_path);
	i_free(file->file.path);
	i_free(file);
}

static const char *fs_sis_file_get_path(struct fs_file *_file)
{
	struct sis_fs_file *file = (struct sis_fs_file *)_file;

	return fs_file_path(file->super);
}

static void
fs_sis_set_async_callback(struct fs_file *_file,
			  fs_file_async_callback_t *callback, void *context)
{
	struct sis_fs_file *file = (struct sis_fs_file *)_file;

	fs_file_set_async_callback(file->super, callback, context);
}

static int fs_sis_wait_async(struct fs *_fs)
{
	struct sis_fs *fs = (struct sis_fs *)_fs;

	return fs_wait_async(fs->super);
}

static void
fs_sis_set_metadata(struct fs_file *_file, const char *key,
		    const char *value)
{
	struct sis_fs_file *file = (struct sis_fs_file *)_file;

	fs_set_metadata(file->super, key, value);
}

static int
fs_sis_get_metadata(struct fs_file *_file,
		    const ARRAY_TYPE(fs_metadata) **metadata_r)
{
	struct sis_fs_file *file = (struct sis_fs_file *)_file;

	return fs_get_metadata(file->super, metadata_r);
}

static bool fs_sis_prefetch(struct fs_file *_file, uoff_t length)
{
	struct sis_fs_file *file = (struct sis_fs_file *)_file;

	return fs_prefetch(file->super, length);
}

static ssize_t fs_sis_read(struct fs_file *_file, void *buf, size_t size)
{
	struct sis_fs_file *file = (struct sis_fs_file *)_file;
	ssize_t ret;

	if ((ret = fs_read(file->super, buf, size)) < 0)
		fs_sis_file_copy_error(file);
	return ret;
}

static struct istream *
fs_sis_read_stream(struct fs_file *_file, size_t max_buffer_size)
{
	struct sis_fs_file *file = (struct sis_fs_file *)_file;

	return fs_read_stream(file->super, max_buffer_size);
}

static bool fs_sis_try_link(struct sis_fs_file *file)
{
	const struct stat *st;
	struct stat st2;

	if (i_stream_stat(file->hash_input, FALSE, &st) < 0)
		return FALSE;

	/* we can use the existing file */
	if (fs_copy(file->hash_file, file->super) < 0) {
		if (errno != ENOENT && errno != EMLINK)
			i_error("fs-sis: %s", fs_last_error(file->super->fs));
		/* failed to use link(), continue as if it hadn't been equal */
		return FALSE;
	}
	if (fs_stat(file->super, &st2) < 0) {
		i_error("fs-sis: %s", fs_last_error(file->super->fs));
		if (fs_delete(file->super) < 0)
			i_error("fs-sis: %s", fs_last_error(file->super->fs));
		return FALSE;
	}
	if (st->st_ino != st2.st_ino) {
		/* the hashes/ file was already replaced with something else */
		if (fs_delete(file->super) < 0)
			i_error("fs-sis: %s", fs_last_error(file->super->fs));
		return FALSE;
	}
	return TRUE;
}

static void fs_sis_replace_hash_file(struct sis_fs_file *file)
{
	struct fs *super_fs = file->super->fs;
	struct fs_file *temp_file;
	const char *hash_fname;
	string_t *temp_path;
	int ret;

	if (file->hash_input == NULL) {
		/* hash file didn't exist previously. we should be able to
		   create it with link() */
		if (fs_copy(file->super, file->hash_file) < 0) {
			if (errno == EEXIST) {
				/* the file was just created. it's probably
				   a duplicate, but it's too much trouble
				   trying to deduplicate it anymore */
			} else {
				i_error("fs-sis: %s", fs_last_error(super_fs));
			}
		}
		return;
	}

	temp_path = t_str_new(256);
	hash_fname = strrchr(file->hash_path, '/');
	if (hash_fname == NULL)
		hash_fname = file->hash_path;
	else {
		str_append_n(temp_path, file->hash_path,
			     (hash_fname-file->hash_path) + 1);
		hash_fname++;
	}
	str_printfa(temp_path, "%s%s.tmp",
		    super_fs->set.temp_file_prefix, hash_fname);

	/* replace existing hash file atomically */
	temp_file = fs_file_init(super_fs, str_c(temp_path),
				 FS_OPEN_MODE_READONLY);
	ret = fs_copy(file->super, temp_file);
	if (ret < 0 && errno == EEXIST) {
		/* either someone's racing us or it's a stale file.
		   try to continue. */
		if (fs_delete(temp_file) < 0 &&
		    errno != ENOENT)
			i_error("fs-sis: %s", fs_last_error(super_fs));
		ret = fs_copy(file->super, temp_file);
	}
	if (ret < 0) {
		i_error("fs-sis: %s", fs_last_error(super_fs));
		fs_file_deinit(&temp_file);
		return;
	}

	if (fs_rename(temp_file, file->hash_file) < 0) {
		if (errno == ENOENT) {
			/* apparently someone else just renamed it. ignore. */
		} else {
			i_error("fs-sis: %s", fs_last_error(super_fs));
		}
		(void)fs_delete(temp_file);
	}
	fs_file_deinit(&temp_file);
}

static int fs_sis_write(struct fs_file *_file, const void *data, size_t size)
{
	struct sis_fs_file *file = (struct sis_fs_file *)_file;

	if (file->super == NULL)
		return -1;

	if (file->hash_input != NULL &&
	    stream_cmp_block(file->hash_input, data, size) &&
	    i_stream_is_eof(file->hash_input)) {
		/* try to use existing file */
		if (fs_sis_try_link(file))
			return 0;
	}

	if (fs_write(file->super, data, size) < 0) {
		fs_sis_file_copy_error(file);
		return -1;
	}
	T_BEGIN {
		fs_sis_replace_hash_file(file);
	} T_END;
	return 0;
}

static void fs_sis_write_stream(struct fs_file *_file)
{
	struct sis_fs_file *file = (struct sis_fs_file *)_file;

	i_assert(_file->output == NULL);

	if (file->super == NULL)
		_file->output = o_stream_create_error(EINVAL);
	else {
		file->fs_output = fs_write_stream(file->super);
		if (file->hash_input == NULL)
			_file->output = file->fs_output;
		else {
			/* compare if files are equal */
			_file->output = o_stream_create_cmp(file->fs_output,
							    file->hash_input);
		}
	}
	o_stream_set_name(_file->output, _file->path);
}

static int fs_sis_write_stream_finish(struct fs_file *_file, bool success)
{
	struct sis_fs_file *file = (struct sis_fs_file *)_file;

	if (!success) {
		if (file->super != NULL) {
			fs_write_stream_abort(file->super, &file->fs_output);
			fs_sis_file_copy_error(file);
		}
		return -1;
	}

	if (file->hash_input != NULL &&
	    o_stream_cmp_equals(_file->output) &&
	    i_stream_is_eof(file->hash_input)) {
		if (fs_sis_try_link(file)) {
			fs_write_stream_abort(file->super, &file->fs_output);
			return 1;
		}
	}

	if (fs_write_stream_finish(file->super, &file->fs_output) < 0) {
		fs_sis_file_copy_error(file);
		return -1;
	}
	T_BEGIN {
		fs_sis_replace_hash_file(file);
	} T_END;
	return 1;
}

static int
fs_sis_lock(struct fs_file *_file, unsigned int secs, struct fs_lock **lock_r)
{
	struct sis_fs_file *file = (struct sis_fs_file *)_file;

	if (fs_lock(file->super, secs, lock_r) < 0) {
		fs_sis_file_copy_error(file);
		return -1;
	}
	return 0;
}

static void fs_sis_unlock(struct fs_lock *_lock ATTR_UNUSED)
{
	i_unreached();
}

static int fs_sis_exists(struct fs_file *_file)
{
	struct sis_fs_file *file = (struct sis_fs_file *)_file;

	if (fs_exists(file->super) < 0) {
		fs_sis_copy_error(file->fs);
		return -1;
	}
	return 0;
}

static int fs_sis_stat(struct fs_file *_file, struct stat *st_r)
{
	struct sis_fs_file *file = (struct sis_fs_file *)_file;

	if (fs_stat(file->super, st_r) < 0) {
		fs_sis_copy_error(file->fs);
		return -1;
	}
	return 0;
}

static int fs_sis_copy(struct fs_file *_src, struct fs_file *_dest)
{
	struct sis_fs_file *src = (struct sis_fs_file *)_src;
	struct sis_fs_file *dest = (struct sis_fs_file *)_dest;

	if (fs_copy(src->super, dest->super) < 0) {
		fs_sis_copy_error(src->fs);
		return -1;
	}
	return 0;
}

static int fs_sis_rename(struct fs_file *_src, struct fs_file *_dest)
{
	struct sis_fs_file *src = (struct sis_fs_file *)_src;
	struct sis_fs_file *dest = (struct sis_fs_file *)_dest;

	if (fs_rename(src->super, dest->super) < 0) {
		fs_sis_copy_error(src->fs);
		return -1;
	}
	return 0;
}

static int fs_sis_delete(struct fs_file *_file)
{
	struct sis_fs_file *file = (struct sis_fs_file *)_file;

	T_BEGIN {
		fs_sis_try_unlink_hash_file(_file->fs, file->super);
	} T_END;
	if (fs_delete(file->super) < 0) {
		fs_sis_copy_error(file->fs);
		return -1;
	}
	return 0;
}

static struct fs_iter *
fs_sis_iter_init(struct fs *_fs, const char *path, enum fs_iter_flags flags)
{
	struct sis_fs *fs = (struct sis_fs *)_fs;

	return fs_iter_init(fs->super, path, flags);
}

const struct fs fs_class_sis = {
	.name = "sis",
	.v = {
		fs_sis_alloc,
		fs_sis_init,
		fs_sis_deinit,
		fs_sis_get_properties,
		fs_sis_file_init,
		fs_sis_file_deinit,
		fs_sis_file_get_path,
		fs_sis_set_async_callback,
		fs_sis_wait_async,
		fs_sis_set_metadata,
		fs_sis_get_metadata,
		fs_sis_prefetch,
		fs_sis_read,
		fs_sis_read_stream,
		fs_sis_write,
		fs_sis_write_stream,
		fs_sis_write_stream_finish,
		fs_sis_lock,
		fs_sis_unlock,
		fs_sis_exists,
		fs_sis_stat,
		fs_sis_copy,
		fs_sis_rename,
		fs_sis_delete,
		fs_sis_iter_init,
		NULL,
		NULL
	}
};
