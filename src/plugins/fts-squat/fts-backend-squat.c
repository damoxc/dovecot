/* Copyright (c) 2006-2007 Dovecot authors, see the included COPYING file */

#include "lib.h"
#include "array.h"
#include "mail-storage-private.h"
#include "mail-search.h"
#include "squat-trie.h"
#include "fts-squat-plugin.h"

#define SQUAT_FILE_PREFIX "dovecot.index.search"

struct squat_fts_backend {
	struct fts_backend backend;
	struct squat_trie *trie;
};

struct squat_fts_backend_build_context {
	struct fts_backend_build_context ctx;
	struct squat_trie_build_context *build_ctx;
};

static struct fts_backend *fts_backend_squat_init(struct mailbox *box)
{
	struct squat_fts_backend *backend;
	struct mail_storage *storage;
	struct mailbox_status status;
	const char *path;
	bool mmap_disable;

	storage = mailbox_get_storage(box);
	path = mail_storage_get_mailbox_index_dir(storage,
						  mailbox_get_name(box));
	if (*path == '\0') {
		/* in-memory indexes */
		return NULL;
	}

	mailbox_get_status(box, STATUS_UIDVALIDITY, &status);
	mmap_disable = (storage->flags &
			(MAIL_STORAGE_FLAG_MMAP_DISABLE |
			 MAIL_STORAGE_FLAG_MMAP_NO_WRITE)) != 0;

	backend = i_new(struct squat_fts_backend, 1);
	backend->backend = fts_backend_squat;
	backend->trie =
		squat_trie_init(t_strconcat(path, "/"SQUAT_FILE_PREFIX, NULL),
				status.uidvalidity, storage->lock_method,
				mmap_disable);
	return &backend->backend;
}

static void fts_backend_squat_deinit(struct fts_backend *_backend)
{
	struct squat_fts_backend *backend =
		(struct squat_fts_backend *)_backend;

	squat_trie_deinit(&backend->trie);
	i_free(backend);
}

static int fts_backend_squat_get_last_uid(struct fts_backend *_backend,
					  uint32_t *last_uid_r)
{
	struct squat_fts_backend *backend =
		(struct squat_fts_backend *)_backend;

	return squat_trie_get_last_uid(backend->trie, last_uid_r);
}

static int
fts_backend_squat_build_init(struct fts_backend *_backend, uint32_t *last_uid_r,
			     struct fts_backend_build_context **ctx_r)
{
	struct squat_fts_backend *backend =
		(struct squat_fts_backend *)_backend;
	struct squat_fts_backend_build_context *ctx;
	struct squat_trie_build_context *build_ctx;

	if (squat_trie_build_init(backend->trie, last_uid_r, &build_ctx) < 0)
		return -1;

	ctx = i_new(struct squat_fts_backend_build_context, 1);
	ctx->ctx.backend = _backend;
	ctx->build_ctx = build_ctx;

	*ctx_r = &ctx->ctx;
	return 0;
}

static int
fts_backend_squat_build_more(struct fts_backend_build_context *_ctx,
			     uint32_t uid, const unsigned char *data,
			     size_t size, bool headers)
{
	struct squat_fts_backend_build_context *ctx =
		(struct squat_fts_backend_build_context *)_ctx;
	enum squat_index_type squat_type;

	squat_type = headers ? SQUAT_INDEX_TYPE_HEADER :
		SQUAT_INDEX_TYPE_BODY;
	return squat_trie_build_more(ctx->build_ctx, uid, squat_type,
				     data, size);
}

static int
fts_backend_squat_build_deinit(struct fts_backend_build_context *_ctx)
{
	struct squat_fts_backend_build_context *ctx =
		(struct squat_fts_backend_build_context *)_ctx;
	int ret;

	ret = squat_trie_build_deinit(&ctx->build_ctx);
	i_free(ctx);
	return ret;
}

static void
fts_backend_squat_expunge(struct fts_backend *_backend ATTR_UNUSED,
			  struct mail *mail ATTR_UNUSED)
{
}

static void
fts_backend_squat_expunge_finish(struct fts_backend *_backend,
				 struct mailbox *box, bool committed)
{
	/* FIXME */
}

static int fts_backend_squat_lock(struct fts_backend *_backend)
{
	struct squat_fts_backend *backend =
		(struct squat_fts_backend *)_backend;

	squat_trie_refresh(backend->trie);
	return 1;
}

static void fts_backend_squat_unlock(struct fts_backend *_backend ATTR_UNUSED)
{
}

static int
fts_backend_squat_lookup(struct fts_backend *_backend, const char *key,
			 enum fts_lookup_flags flags,
			 ARRAY_TYPE(seq_range) *definite_uids,
			 ARRAY_TYPE(seq_range) *maybe_uids)
{
	struct squat_fts_backend *backend =
		(struct squat_fts_backend *)_backend;
	enum squat_index_type squat_type = 0;

	i_assert((flags & FTS_LOOKUP_FLAG_INVERT) == 0);

	if ((flags & FTS_LOOKUP_FLAG_HEADER) != 0)
		squat_type |= SQUAT_INDEX_TYPE_HEADER;
	if ((flags & FTS_LOOKUP_FLAG_BODY) != 0)
		squat_type |= SQUAT_INDEX_TYPE_BODY;
	i_assert(squat_type != 0);

	return squat_trie_lookup(backend->trie, key, flags,
				 definite_uids, maybe_uids);
}

struct fts_backend fts_backend_squat = {
	MEMBER(name) "squat",
	MEMBER(flags) FTS_BACKEND_FLAG_SUBSTRING_LOOKUPS,

	{
		fts_backend_squat_init,
		fts_backend_squat_deinit,
		fts_backend_squat_get_last_uid,
		fts_backend_squat_build_init,
		fts_backend_squat_build_more,
		fts_backend_squat_build_deinit,
		fts_backend_squat_expunge,
		fts_backend_squat_expunge_finish,
		fts_backend_squat_lock,
		fts_backend_squat_unlock,
		fts_backend_squat_lookup,
		NULL
	}
};
