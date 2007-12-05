/* Copyright (c) 2005-2007 Dovecot authors, see the included COPYING file */

/* Only for reporting filesystem quota */

#include "lib.h"
#include "array.h"
#include "str.h"
#include "mountpoint.h"
#include "quota-private.h"
#include "quota-fs.h"

#ifdef HAVE_FS_QUOTA

#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#ifdef HAVE_LINUX_DQBLK_XFS_H
#  include <linux/dqblk_xfs.h>
#  define HAVE_XFS_QUOTA
#elif defined (HAVE_XFS_XQM_H)
#  include <xfs/xqm.h> /* CentOS 4.x at least uses this */
#  define HAVE_XFS_QUOTA
#endif

#ifdef HAVE_RQUOTA
#  include "rquota.h"
#  define RQUOTA_GETQUOTA_TIMEOUT_SECS 10
#endif

#ifndef DEV_BSIZE
#  define DEV_BSIZE 512
#endif

#ifdef HAVE_STRUCT_DQBLK_CURSPACE
#  define dqb_curblocks dqb_curspace
#endif

/* Older sys/quota.h doesn't define _LINUX_QUOTA_VERSION at all, which means
   it supports only v1 quota */
#ifndef _LINUX_QUOTA_VERSION
#  define _LINUX_QUOTA_VERSION 1
#endif

struct fs_quota_mountpoint {
	int refcount;

	char *mount_path;
	char *device_path;
	char *type;

#ifdef FS_QUOTA_SOLARIS
	int fd;
	char *path;
#endif
};

struct fs_quota_root {
	struct quota_root root;
	char *storage_mount_path;

	uid_t uid;
	gid_t gid;
	struct fs_quota_mountpoint *mount;

	unsigned int inode_per_mail:1;
	unsigned int user_disabled:1;
	unsigned int group_disabled:1;
};

extern struct quota_backend quota_backend_fs;

static struct quota_root *fs_quota_alloc(void)
{
	struct fs_quota_root *root;

	root = i_new(struct fs_quota_root, 1);
	root->uid = geteuid();
	root->gid = getegid();

	return &root->root;
}

static int fs_quota_init(struct quota_root *_root, const char *args)
{
	struct fs_quota_root *root = (struct fs_quota_root *)_root;
	const char *const *tmp;

	if (args == NULL)
		return 0;

	for (tmp = t_strsplit(args, ":"); *tmp != NULL; tmp++) {
		if (strcmp(*tmp, "user") == 0)
			root->group_disabled = TRUE;
		else if (strcmp(*tmp, "group") == 0)
			root->user_disabled = TRUE;
		else if (strcmp(*tmp, "inode_per_mail") == 0)
			root->inode_per_mail = TRUE;
		else if (strcmp(*tmp, "noenforcing") == 0)
			_root->no_enforcing = TRUE;
		else if (strncmp(*tmp, "mount=", 6) == 0) {
			i_free(root->storage_mount_path);
			root->storage_mount_path = i_strdup(*tmp + 6);
		} else {
			i_error("fs quota: Invalid parameter: %s", *tmp);
			return -1;
		}
	}
	return 0;
}

static void fs_quota_mountpoint_free(struct fs_quota_mountpoint *mount)
{
	if (--mount->refcount > 0)
		return;

#ifdef FS_QUOTA_SOLARIS
	if (mount->fd != -1) {
		if (close(mount->fd) < 0)
			i_error("close(%s) failed: %m", mount->path);
	}
	i_free(mount->path);
#endif

	i_free(mount->device_path);
	i_free(mount->mount_path);
	i_free(mount->type);
	i_free(mount);
}

static void fs_quota_deinit(struct quota_root *_root)
{
	struct fs_quota_root *root = (struct fs_quota_root *)_root;

	if (root->mount != NULL)
		fs_quota_mountpoint_free(root->mount);
	i_free(root->storage_mount_path);
	i_free(root);
}

static struct fs_quota_mountpoint *fs_quota_mountpoint_get(const char *dir)
{
	struct fs_quota_mountpoint *mount;
	struct mountpoint point;
	int ret;

	ret = mountpoint_get(dir, default_pool, &point);
	if (ret <= 0)
		return NULL;

	mount = i_new(struct fs_quota_mountpoint, 1);
	mount->refcount = 1;
	mount->device_path = point.device_path;
	mount->mount_path = point.mount_path;
	mount->type = point.type;
	return mount;
}

#define QUOTA_ROOT_MATCH(root, mount) \
	((root)->root.backend.name == quota_backend_fs.name && \
	 ((root)->storage_mount_path == NULL || \
	  strcmp((root)->storage_mount_path, (mount)->mount_path) == 0))

static struct fs_quota_root *
fs_quota_root_find_mountpoint(struct quota *quota,
			      const struct fs_quota_mountpoint *mount)
{
	struct quota_root *const *roots;
	struct fs_quota_root *empty = NULL;
	unsigned int i, count;

	roots = array_get(&quota->roots, &count);
	for (i = 0; i < count; i++) {
		struct fs_quota_root *root = (struct fs_quota_root *)roots[i];
		if (QUOTA_ROOT_MATCH(root, mount)) {
			if (root->mount == NULL)
				empty = root;
			else if (strcmp(root->mount->mount_path,
					mount->mount_path) == 0)
				return root;
		}
	}
	return empty;
}

static void fs_quota_mount_init(struct fs_quota_root *root,
				struct fs_quota_mountpoint *mount)
{
	struct quota_root *const *roots;
	unsigned int i, count;

#ifdef FS_QUOTA_SOLARIS
	if (mount->path == NULL) {
		mount->path = i_strconcat(mount->mount_path, "/quotas", NULL);
		mount->fd = open(mount->path, O_RDONLY);
		if (mount->fd == -1 && errno != ENOENT)
			i_error("open(%s) failed: %m", mount->path);
	}
#endif
	root->mount = mount;

	/* if there are more unused quota roots, copy this mount to them */
	roots = array_get(&root->root.quota->roots, &count);
	for (i = 0; i < count; i++) {
		struct fs_quota_root *root = (struct fs_quota_root *)roots[i];
		if (QUOTA_ROOT_MATCH(root, mount) && root->mount == NULL) {
			mount->refcount++;
			root->mount = mount;
		}
	}
}

static void fs_quota_add_missing_mounts(struct quota *quota)
{
	struct fs_quota_mountpoint *mount;
	struct quota_root *const *roots;
	unsigned int i, count;

	roots = array_get(&quota->roots, &count);
	for (i = 0; i < count; i++) {
		struct fs_quota_root *root = (struct fs_quota_root *)roots[i];

		if (root->root.backend.name != quota_backend_fs.name ||
		    root->storage_mount_path == NULL || root->mount != NULL)
			continue;

		mount = fs_quota_mountpoint_get(root->storage_mount_path);
		fs_quota_mount_init(root, mount);
	}
}

static void fs_quota_storage_added(struct quota *quota,
				   struct mail_storage *storage)
{
	struct fs_quota_mountpoint *mount;
	struct fs_quota_root *root;
	const char *dir;
	bool is_file;

	dir = mail_storage_get_mailbox_path(storage, "", &is_file);
	mount = fs_quota_mountpoint_get(dir);
	if (getenv("DEBUG") != NULL) {
		i_info("fs quota add storage dir = %s", dir);
		i_info("fs quota block device = %s", mount->device_path);
		i_info("fs quota mount point = %s", mount->mount_path);
	}

	root = fs_quota_root_find_mountpoint(quota, mount);
	if (root != NULL && root->mount == NULL)
		fs_quota_mount_init(root, mount);
	else
		fs_quota_mountpoint_free(mount);

	/* we would actually want to do this only once after all quota roots
	   are created, but there's no way to do this right now */
	fs_quota_add_missing_mounts(quota);
}

static const char *const *
fs_quota_root_get_resources(struct quota_root *_root)
{
	struct fs_quota_root *root = (struct fs_quota_root *)_root;
	static const char *resources_kb[] = {
		QUOTA_NAME_STORAGE_KILOBYTES,
		NULL
	};
	static const char *resources_kb_messages[] = {
		QUOTA_NAME_STORAGE_KILOBYTES,
		QUOTA_NAME_MESSAGES,
		NULL
	};

	return root->inode_per_mail ? resources_kb_messages : resources_kb;
}

#ifdef HAVE_RQUOTA
/* retrieve user quota from a remote host */
static int do_rquota(struct fs_quota_root *root, bool bytes,
		     uint64_t *value_r, uint64_t *limit_r)
{
	struct getquota_rslt result;
	struct getquota_args args;
	struct timeval timeout;
	enum clnt_stat call_status;
	CLIENT *cl;
	struct fs_quota_mountpoint *mount = root->mount;
	const char *host;
	char *path;

	path = strchr(mount->device_path, ':');
	if (path == NULL) {
		i_error("quota-fs: %s is not a valid NFS device path",
			mount->device_path);
		return -1;
	}

	host = t_strdup_until(mount->device_path, path);
	path++;

	if (getenv("DEBUG") != NULL) {
		i_info("quota-fs: host=%s, path=%s, uid=%s",
			host, path, dec2str(root->uid));
	}

	/* clnt_create() polls for a while to establish a connection */
	cl = clnt_create(host, RQUOTAPROG, RQUOTAVERS, "udp");
	if (cl == NULL) {
		i_error("quota-fs: could not contact RPC service on %s",
			host);
		return -1;
	}

	/* Establish some RPC credentials */
	auth_destroy(cl->cl_auth);
	cl->cl_auth = authunix_create_default();

	/* make the rquota call on the remote host */
	args.gqa_pathp = path;
	args.gqa_uid = root->uid;

	timeout.tv_sec = RQUOTA_GETQUOTA_TIMEOUT_SECS;
	timeout.tv_usec = 0;
	call_status = clnt_call(cl, RQUOTAPROC_GETQUOTA,
				(xdrproc_t)xdr_getquota_args, (char *)&args,
				(xdrproc_t)xdr_getquota_rslt, (char *)&result,
				timeout);
	
	/* the result has been deserialized, let the client go */
	auth_destroy(cl->cl_auth);
	clnt_destroy(cl);

	if (call_status != RPC_SUCCESS) {
		const char *rpc_error_msg = clnt_sperrno(call_status);

		i_error("quota-fs: remote rquota call failed: %s",
			rpc_error_msg);
		return -1;
	}

	switch (result.status) {
	case Q_OK: {
		/* convert the results from blocks to bytes */
		rquota *rq = &result.getquota_rslt_u.gqr_rquota;

		if (rq->rq_active) {
			if (bytes) {
				*value_r = (uint64_t)rq->rq_curblocks *
					(uint64_t)rq->rq_bsize;
				*limit_r = (uint64_t)rq->rq_bsoftlimit *
					(uint64_t)rq->rq_bsize;
			} else {
				*value_r = rq->rq_curfiles;
				*limit_r = rq->rq_fsoftlimit;
			}
		}
		if (getenv("DEBUG") != NULL) {
			i_info("quota-fs: uid=%s, value=%llu, "
			       "limit=%llu, active=%d", dec2str(root->uid),
			       (unsigned long long)*value_r,
			       (unsigned long long)*limit_r, rq->rq_active);
		}
		return 1;
	}
	case Q_NOQUOTA:
		if (getenv("DEBUG") != NULL) {
			i_info("quota-fs: uid=%s, limit=unlimited",
			       dec2str(root->uid));
		}
		return 1;
	case Q_EPERM:
		i_error("quota-fs: permission denied to rquota service");
		return -1;
	default:
		i_error("quota-fs: unrecognized status code (%d) "
			"from rquota service", result.status);
		return -1;
	}
}
#endif

#if defined(FS_QUOTA_LINUX) || defined(FS_QUOTA_BSDAIX)
static void fs_quota_root_disable(struct fs_quota_root *root, bool group)
{
	if (group)
		root->group_disabled = TRUE;
	else
		root->user_disabled = TRUE;
}
#endif

#ifdef FS_QUOTA_LINUX
static int
fs_quota_get_linux(struct fs_quota_root *root, bool group, bool bytes,
		   uint64_t *value_r, uint64_t *limit_r)
{
	struct dqblk dqblk;
	int type, id;

	type = group ? GRPQUOTA : USRQUOTA;
	id = group ? root->gid : root->uid;

#ifdef HAVE_XFS_QUOTA
	if (strcmp(root->mount->type, "xfs") == 0) {
		struct fs_disk_quota xdqblk;

		if (quotactl(QCMD(Q_XGETQUOTA, type),
			     root->mount->device_path,
			     id, (caddr_t)&xdqblk) < 0) {
			if (errno == ESRCH) {
				fs_quota_root_disable(root, group);
				return 0;
			}
			i_error("%d quotactl(Q_XGETQUOTA, %s) failed: %m",
				errno, root->mount->device_path);
			return -1;
		}

		if (bytes) {
			/* values always returned in 512 byte blocks */
			*value_r = xdqblk.d_bcount * 512;
			*limit_r = xdqblk.d_blk_softlimit * 512;
		} else {
			*value_r = xdqblk.d_icount;
			*limit_r = xdqblk.d_ino_softlimit;
		}
	} else
#endif
	{
		/* ext2, ext3 */
		if (quotactl(QCMD(Q_GETQUOTA, type),
			     root->mount->device_path,
			     id, (caddr_t)&dqblk) < 0) {
			if (errno == ESRCH) {
				fs_quota_root_disable(root, group);
				return 0;
			}
			i_error("quotactl(Q_GETQUOTA, %s) failed: %m",
				root->mount->device_path);
			if (errno == EINVAL) {
				i_error("Dovecot was compiled with Linux quota "
					"v%d support, try changing it "
					"(--with-linux-quota configure option)",
					_LINUX_QUOTA_VERSION);
			}
			return -1;
		}

		if (bytes) {
#if _LINUX_QUOTA_VERSION == 1
			*value_r = dqblk.dqb_curblocks * 1024;
#else
			*value_r = dqblk.dqb_curblocks;
#endif
			*limit_r = dqblk.dqb_bsoftlimit * 1024;
		} else {
			*value_r = dqblk.dqb_curinodes;
			*value_r = dqblk.dqb_isoftlimit;
		}
	}
	return 1;
}
#endif

#ifdef FS_QUOTA_BSDAIX
static int
fs_quota_get_bsdaix(struct fs_quota_root *root, bool group, bool bytes,
		    uint64_t *value_r, uint64_t *limit_r)
{
	struct dqblk dqblk;
	int type, id;

	type = group ? GRPQUOTA : USRQUOTA;
	id = group ? root->gid : root->uid;

	if (quotactl(root->mount->mount_path, QCMD(Q_GETQUOTA, type),
		     id, (void *)&dqblk) < 0) {
		if (errno == ESRCH) {
			fs_quota_root_disable(root, group);
			return 0;
		}
		i_error("quotactl(Q_GETQUOTA, %s) failed: %m",
			root->mount->mount_path);
		return -1;
	}
	if (bytes) {
		*value_r = (uint64_t)dqblk.dqb_curblocks * DEV_BSIZE;
		*limit_r = (uint64_t)dqblk.dqb_bsoftlimit * DEV_BSIZE;
	} else {
		*value_r = dqblk.dqb_curinodes;
		*value_r = dqblk.dqb_isoftlimit;
	}
	return 1;
}
#endif

#ifdef FS_QUOTA_SOLARIS
static int
fs_quota_get_solaris(struct fs_quota_root *root, bool bytes,
		     uint64_t *value_r, uint64_t *limit_r)
{
	struct dqblk dqblk;
	struct quotctl ctl;

	if (root->mount->fd == -1)
		return 0;

	ctl.op = Q_GETQUOTA;
	ctl.uid = root->uid;
	ctl.addr = (caddr_t)&dqblk;
	if (ioctl(root->mount->fd, Q_QUOTACTL, &ctl) < 0) {
		i_error("ioctl(%s, Q_QUOTACTL) failed: %m", root->mount->path);
		return -1;
	}
	if (bytes) {
		*value_r = (uint64_t)dqblk.dqb_curblocks * DEV_BSIZE;
		*limit_r = (uint64_t)dqblk.dqb_bsoftlimit * DEV_BSIZE;
	} else {
		*value_r = dqblk.dqb_curfiles;
		*value_r = dqblk.dqb_fsoftlimit;
	}
	return 1;
}
#endif

static int
fs_quota_get_one_resource(struct fs_quota_root *root, bool group, bool bytes,
			  uint64_t *value_r, uint64_t *limit_r)
{
	if (group) {
		if (root->group_disabled)
			return 0;
	} else {
		if (root->user_disabled)
			return 0;
	}
#ifdef FS_QUOTA_LINUX
	return fs_quota_get_linux(root, group, bytes, value_r, limit_r);
#elif defined (FS_QUOTA_BSDAIX)
	return fs_quota_get_bsdaix(root, group, bytes, value_r, limit_r);
#else
	if (group) {
		/* not supported */
		return 0;
	}
	return fs_quota_get_solaris(root, bytes, value_r, limit_r);
#endif
}

static int
fs_quota_get_resource(struct quota_root *_root, const char *name,
		      uint64_t *value_r, uint64_t *limit_r)
{
	struct fs_quota_root *root = (struct fs_quota_root *)_root;
	bool bytes;
	int ret;

	*value_r = 0;
	*limit_r = 0;

	if (root->mount == NULL ||
	    (strcasecmp(name, QUOTA_NAME_STORAGE_BYTES) != 0 &&
	     strcasecmp(name, QUOTA_NAME_MESSAGES) != 0))
		return 0;
	bytes = strcasecmp(name, QUOTA_NAME_STORAGE_BYTES) == 0;

#ifdef HAVE_RQUOTA
	if (strcmp(root->mount->type, "nfs") == 0) {
		int ret;

		T_FRAME(
			ret = do_rquota(root, bytes, value_r, limit_r);
		);
		return ret;
	}
#endif

	ret = fs_quota_get_one_resource(root, FALSE, bytes,
					value_r, limit_r);
	if (ret != 0)
		return ret;

	/* fallback to group quota */
	return fs_quota_get_one_resource(root, TRUE, bytes, value_r, limit_r);
}

static int 
fs_quota_update(struct quota_root *root ATTR_UNUSED,
		struct quota_transaction_context *ctx ATTR_UNUSED)
{
	return 0;
}

struct quota_backend quota_backend_fs = {
	"fs",

	{
		fs_quota_alloc,
		fs_quota_init,
		fs_quota_deinit,
		NULL,

		fs_quota_storage_added,

		fs_quota_root_get_resources,
		fs_quota_get_resource,
		fs_quota_update
	}
};

#endif
