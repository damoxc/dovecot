#ifndef MAILBOX_LIST_PRIVATE_H
#define MAILBOX_LIST_PRIVATE_H

#include "mail-namespace.h"
#include "mailbox-list.h"
#include "mail-storage-settings.h"

#define MAILBOX_LIST_NAME_MAILDIRPLUSPLUS "maildir++"
#define MAILBOX_LIST_NAME_IMAPDIR "imapdir"
#define MAILBOX_LIST_NAME_FS "fs"

#define MAILBOX_LOG_FILE_NAME "dovecot.mailbox.log"

enum mailbox_log_record_type;
enum mailbox_list_notify_event;
struct stat;
struct dirent;
struct imap_match_glob;
struct mailbox_tree_context;
struct mailbox_list_notify;
struct mailbox_list_notify_rec;

#define MAILBOX_INFO_FLAGS_FINISHED(flags) \
	(((flags) & (MAILBOX_SELECT | MAILBOX_NOSELECT | \
		     MAILBOX_NONEXISTENT)) != 0)

enum mailbox_dir_create_type {
	/* Creating a mailbox */
	MAILBOX_DIR_CREATE_TYPE_MAILBOX,
	/* Create a \Noselect or a mailbox */
	MAILBOX_DIR_CREATE_TYPE_TRY_NOSELECT,
	/* Create a \Noselect or fail */
	MAILBOX_DIR_CREATE_TYPE_ONLY_NOSELECT
};

struct mailbox_list_vfuncs {
	struct mailbox_list *(*alloc)(void);
	void (*deinit)(struct mailbox_list *list);

	int (*get_storage)(struct mailbox_list **list, const char *vname,
			   struct mail_storage **storage_r);
	bool (*is_valid_pattern)(struct mailbox_list *list,
				 const char *pattern);
	bool (*is_valid_existing_name)(struct mailbox_list *list,
				       const char *name);
	bool (*is_valid_create_name)(struct mailbox_list *list,
				     const char *name);

	char (*get_hierarchy_sep)(struct mailbox_list *list);
	const char *(*get_vname)(struct mailbox_list *list,
				 const char *storage_name);
	const char *(*get_storage_name)(struct mailbox_list *list,
					const char *vname);
	const char *(*get_path)(struct mailbox_list *list, const char *name,
				enum mailbox_list_path_type type);

	const char *(*get_temp_prefix)(struct mailbox_list *list, bool global);
	const char *(*join_refpattern)(struct mailbox_list *list,
				       const char *ref, const char *pattern);

	struct mailbox_list_iterate_context *
		(*iter_init)(struct mailbox_list *list,
			     const char *const *patterns,
			     enum mailbox_list_iter_flags flags);
	const struct mailbox_info *
		(*iter_next)(struct mailbox_list_iterate_context *ctx);
	int (*iter_deinit)(struct mailbox_list_iterate_context *ctx);

	int (*get_mailbox_flags)(struct mailbox_list *list,
				 const char *dir, const char *fname,
				 enum mailbox_list_file_type type,
				 enum mailbox_info_flags *flags_r);
	/* Returns TRUE if name is mailbox's internal file/directory.
	   If it does, mailbox deletion assumes it can safely delete it. */
	bool (*is_internal_name)(struct mailbox_list *list, const char *name);

	/* Read subscriptions from src_list, but place them into
	   dest_list->subscriptions. Set errors to dest_list. */
	int (*subscriptions_refresh)(struct mailbox_list *src_list,
				     struct mailbox_list *dest_list);
	int (*set_subscribed)(struct mailbox_list *list,
			      const char *name, bool set);
	int (*create_mailbox_dir)(struct mailbox_list *list, const char *name,
				  enum mailbox_dir_create_type type);
	int (*delete_mailbox)(struct mailbox_list *list, const char *name);
	int (*delete_dir)(struct mailbox_list *list, const char *name);
	int (*delete_symlink)(struct mailbox_list *list, const char *name);
	int (*rename_mailbox)(struct mailbox_list *oldlist, const char *oldname,
			      struct mailbox_list *newlist, const char *newname,
			      bool rename_children);

	int (*notify_init)(struct mailbox_list *list,
			   enum mailbox_list_notify_event mask,
			   struct mailbox_list_notify **notify_r);
	int (*notify_next)(struct mailbox_list_notify *notify,
			   const struct mailbox_list_notify_rec **rec_r);
	void (*notify_deinit)(struct mailbox_list_notify *notify);
	void (*notify_wait)(struct mailbox_list_notify *notify,
			    void (*callback)(void *context), void *context);
};

struct mailbox_list_module_register {
	unsigned int id;
};

union mailbox_list_module_context {
	struct mailbox_list_vfuncs super;
	struct mailbox_list_module_register *reg;
};

struct mailbox_list {
	const char *name;
	enum mailbox_list_properties props;
	size_t mailbox_name_max_length;

	struct mailbox_list_vfuncs v, *vlast;

/* private: */
	pool_t pool;
	struct mail_namespace *ns;
	struct mailbox_list_settings set;
	const struct mail_storage_settings *mail_set;
	enum mailbox_list_flags flags;

	/* -1 if not set yet. use mailbox_list_get_permissions() to set them */
	mode_t file_create_mode, dir_create_mode;
	gid_t file_create_gid;
	/* origin (e.g. path) where the file_create_gid was got from */
	const char *file_create_gid_origin;

	struct mailbox_tree_context *subscriptions;
	time_t subscriptions_mtime, subscriptions_read_time;

	struct mailbox_log *changelog;
	time_t changelog_timestamp;

	pool_t guid_cache_pool;
	HASH_TABLE(uint8_t *, struct mailbox_guid_cache_rec *) guid_cache;
	bool guid_cache_errors;

	char *error_string;
	enum mail_error error;
	bool temporary_error;

	ARRAY_DEFINE(module_contexts, union mailbox_list_module_context *);

	unsigned int index_root_dir_created:1;
};

union mailbox_list_iterate_module_context {
	struct mailbox_list_module_register *reg;
};

struct mailbox_list_iterate_context {
	struct mailbox_list *list;
	pool_t pool;
	enum mailbox_list_iter_flags flags;
	bool failed;

	struct imap_match_glob *glob;
	struct mailbox_list_autocreate_iterate_context *autocreate_ctx;
	struct mailbox_info specialuse_info;

	ARRAY_DEFINE(module_contexts,
		     union mailbox_list_iterate_module_context *);
};

struct mailbox_list_iter_update_context {
	struct mailbox_list_iterate_context *iter_ctx;
	struct mailbox_tree_context *tree_ctx;
			      
	struct imap_match_glob *glob;
	enum mailbox_info_flags leaf_flags, parent_flags;

	unsigned int update_only:1;
	unsigned int match_parents:1;
};

/* Modules should use do "my_id = mailbox_list_module_id++" and
   use objects' module_contexts[id] for their own purposes. */
extern struct mailbox_list_module_register mailbox_list_module_register;

void mailbox_lists_init(void);
void mailbox_lists_deinit(void);

int mailbox_list_settings_parse(struct mail_user *user, const char *data,
				struct mailbox_list_settings *set_r,
				const char **error_r);
const char *mailbox_list_default_get_storage_name(struct mailbox_list *list,
						  const char *vname);
const char *mailbox_list_default_get_vname(struct mailbox_list *list,
					   const char *storage_name);
const char *mailbox_list_get_unexpanded_path(struct mailbox_list *list,
					     enum mailbox_list_path_type type);
const char *
mailbox_list_set_get_root_path(const struct mailbox_list_settings *set,
			       enum mailbox_list_path_type type);

int mailbox_list_delete_index_control(struct mailbox_list *list,
				      const char *name);

void mailbox_list_iter_update(struct mailbox_list_iter_update_context *ctx,
			      const char *name);

bool mailbox_list_name_is_too_large(const char *name, char sep);
enum mailbox_list_file_type mailbox_list_get_file_type(const struct dirent *d);
bool mailbox_list_try_get_absolute_path(struct mailbox_list *list,
					const char **name);
int mailbox_list_create_missing_index_dir(struct mailbox_list *list,
					  const char *name) ATTR_NULL(2);
int mailbox_list_create_missing_index_pvt_dir(struct mailbox_list *list,
					      const char *name);

void mailbox_list_add_change(struct mailbox_list *list,
			     enum mailbox_log_record_type type,
			     const guid_128_t guid_128);
int mailbox_list_get_guid_path(struct mailbox_list *list, const char *path,
			       guid_128_t guid_128_r);
void mailbox_name_get_sha128(const char *name, guid_128_t guid_128_r);

void mailbox_list_clear_error(struct mailbox_list *list);
void mailbox_list_set_error(struct mailbox_list *list,
			    enum mail_error error, const char *string);
void mailbox_list_set_critical(struct mailbox_list *list, const char *fmt, ...)
	ATTR_FORMAT(2, 3);
void mailbox_list_set_internal_error(struct mailbox_list *list);
bool mailbox_list_set_error_from_errno(struct mailbox_list *list);

#endif
