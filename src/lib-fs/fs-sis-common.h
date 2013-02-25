#ifndef FS_SIS_COMMON_H
#define FS_SIS_COMMON_H

#include "fs-api-private.h"

#define HASH_DIR_NAME "hashes"

int fs_sis_path_parse(struct fs *fs, const char *path,
		      const char **dir_r, const char **hash_r);
void fs_sis_try_unlink_hash_file(struct fs *sis_fs, struct fs_file *super_file);

#endif

