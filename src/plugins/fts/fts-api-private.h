#ifndef FTS_API_PRIVATE_H
#define FTS_API_PRIVATE_H

#include "fts-api.h"

struct fts_backend_vfuncs {
	struct fts_backend *(*init)(struct mailbox *box);
	void (*deinit)(struct fts_backend *backend);

	int (*get_last_uid)(struct fts_backend *backend, uint32_t *last_uid_r);
	int (*get_all_last_uids)(struct fts_backend *backend, pool_t pool,
				 ARRAY_TYPE(fts_backend_uid_map) *last_uids);

	int (*build_init)(struct fts_backend *backend, uint32_t *last_uid_r,
			  struct fts_backend_build_context **ctx_r);
	int (*build_more)(struct fts_backend_build_context *ctx, uint32_t uid,
			  const unsigned char *data, size_t size, bool headers);
	int (*build_deinit)(struct fts_backend_build_context *ctx);

	void (*expunge)(struct fts_backend *backend, struct mail *mail);
	void (*expunge_finish)(struct fts_backend *backend,
			       struct mailbox *box, bool committed);

	int (*lock)(struct fts_backend *backend);
	void (*unlock)(struct fts_backend *backend);

	int (*lookup)(struct fts_backend *backend, const char *key, 
		      enum fts_lookup_flags flags,
		      ARRAY_TYPE(seq_range) *definite_uids,
		      ARRAY_TYPE(seq_range) *maybe_uids);
	int (*filter)(struct fts_backend *backend, const char *key,
		      enum fts_lookup_flags flags,
		      ARRAY_TYPE(seq_range) *definite_uids,
		      ARRAY_TYPE(seq_range) *maybe_uids);

	int (*lookup2)(struct fts_backend_lookup_context *ctx,
		       ARRAY_TYPE(seq_range) *definite_uids,
		       ARRAY_TYPE(seq_range) *maybe_uids,
		       ARRAY_TYPE(fts_score_map) *scores);
};

enum fts_backend_flags {
	/* If set, the backend is used for TEXT and BODY search
	   optimizations. Otherwise only TEXT_FAST and BODY_FAST are
	   optimized. */
	FTS_BACKEND_FLAG_SUBSTRING_LOOKUPS	= 0x01,
	/* Backend supports virtual mailbox lookups. */
	FTS_BACKEND_FLAG_VIRTUAL_LOOKUPS	= 0x02
};

struct fts_backend {
	const char *name;
	enum fts_backend_flags flags;

	struct fts_backend_vfuncs v;
	struct mailbox *box;

	unsigned int locked:1;
	unsigned int building:1;
};

struct fts_backend_build_context {
	struct fts_backend *backend;

	unsigned int failed:1;
};

struct fts_backend_lookup_field {
	const char *key;
	enum fts_lookup_flags flags;
};

struct fts_backend_lookup_context {
	struct fts_backend *backend;
	pool_t pool;

	ARRAY_DEFINE(fields, struct fts_backend_lookup_field);
};

void fts_backend_register(const struct fts_backend *backend);
void fts_backend_unregister(const char *name);

void fts_filter_uids(ARRAY_TYPE(seq_range) *definite_dest,
		     const ARRAY_TYPE(seq_range) *definite_filter,
		     ARRAY_TYPE(seq_range) *maybe_dest,
		     const ARRAY_TYPE(seq_range) *maybe_filter);

#endif
