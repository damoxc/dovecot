#ifndef __QUOTA_H
#define __QUOTA_H

struct mail;
struct mailbox;

/* Message storage size kilobytes. */
#define QUOTA_NAME_STORAGE "STORAGE"
/* Number of messages. */
#define QUOTA_NAME_MESSAGES "MESSAGES"

struct quota;
struct quota_root;
struct quota_root_iter;
struct quota_transaction_context;

struct quota *quota_init(void);
void quota_deinit(struct quota *quota);

/* Create a new quota root. */
struct quota_root *quota_root_init(struct quota *quota, const char *root_def);
void quota_root_deinit(struct quota_root *root);

/* Add a new rule too the quota root. Returns 0 if ok, -1 if rule is invalid. */
int quota_root_add_rule(struct quota_root *root, const char *rule_def,
			const char **error_r);

/* List all quota roots. Returned quota roots are freed by quota_deinit(). */
struct quota_root_iter *
quota_root_iter_init(struct quota *quota, struct mailbox *box);
struct quota_root *quota_root_iter_next(struct quota_root_iter *iter);
void quota_root_iter_deinit(struct quota_root_iter *iter);

/* Return quota root or NULL. */
struct quota_root *quota_root_lookup(struct quota *quota, const char *name);

/* Returns name of the quota root. */
const char *quota_root_get_name(struct quota_root *root);
/* Return a list of all resources set for the quota root. */
const char *const *quota_root_get_resources(struct quota_root *root);

/* Returns 1 if quota value was found, 0 if not, -1 if error. */
int quota_get_resource(struct quota_root *root, const char *mailbox_name,
		       const char *name, uint64_t *value_r, uint64_t *limit_r);
/* Returns 0 if OK, -1 if error (eg. permission denied, invalid name). */
int quota_set_resource(struct quota_root *root, const char *name,
		       uint64_t value, const char **error_r);

/* Start a new quota transaction. */
struct quota_transaction_context *quota_transaction_begin(struct quota *quota, 
							  struct mailbox *box);
/* Commit quota transaction. Returns 0 if ok, -1 if failed. */
int quota_transaction_commit(struct quota_transaction_context *ctx);
/* Rollback quota transaction changes. */
void quota_transaction_rollback(struct quota_transaction_context *ctx);

/* Allocate from quota if there's space. Returns 1 if updated, 0 if not,
   -1 if error. If mail size is larger than even maximum allowed quota,
   too_large_r is set to TRUE. */
int quota_try_alloc(struct quota_transaction_context *ctx,
		    struct mail *mail, bool *too_large_r);
/* Like quota_try_alloc(), but don't actually allocate anything. */
int quota_test_alloc(struct quota_transaction_context *ctx,
		     uoff_t size, bool *too_large_r);
/* Update quota by allocating/freeing space used by mail. */
void quota_alloc(struct quota_transaction_context *ctx, struct mail *mail);
void quota_free(struct quota_transaction_context *ctx, struct mail *mail);

#endif
