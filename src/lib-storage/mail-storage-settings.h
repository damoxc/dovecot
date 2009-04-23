#ifndef MAIL_STORAGE_SETTINGS_H
#define MAIL_STORAGE_SETTINGS_H

struct mail_user;
struct mail_storage;

struct mail_storage_settings {
	const char *mail_location;
	const char *mail_cache_fields;
	const char *mail_never_cache_fields;
	unsigned int mail_cache_min_mail_count;
	unsigned int mailbox_idle_check_interval;
	unsigned int mail_max_keyword_length;
	bool mail_save_crlf;
	bool fsync_disable;
	bool mmap_disable;
	bool dotlock_use_excl;
	bool mail_nfs_storage;
	bool mail_nfs_index;
	bool mailbox_list_index_disable;
	bool mail_debug;
	bool mail_full_filesystem_access;
	const char *lock_method;
	const char *pop3_uidl_format;
};

struct mail_namespace_settings {
	const char *type;
	const char *separator;
	const char *prefix;
	const char *location;
	const char *alias_for;

	bool inbox;
	bool hidden;
	const char *list;
	bool subscriptions;

	struct mail_user_settings *user_set;
};

struct mail_user_settings {
	const char *base_dir;
	const char *auth_socket_path;

	const char *mail_uid;
	const char *mail_gid;
	const char *mail_home;
	const char *mail_chroot;
	const char *mail_access_groups;
	const char *mail_privileged_group;
	const char *valid_chroot_dirs;

	unsigned int first_valid_uid, last_valid_uid;
	unsigned int first_valid_gid, last_valid_gid;

	unsigned int mail_log_max_lines_per_sec;

	const char *mail_plugins;
	const char *mail_plugin_dir;

	const char *mail_log_prefix;

	ARRAY_DEFINE(namespaces, struct mail_namespace_settings *);
	ARRAY_DEFINE(plugin_envs, const char *);
};

extern struct setting_parser_info mail_user_setting_parser_info;
extern struct setting_parser_info mail_namespace_setting_parser_info;
extern struct setting_parser_info mail_storage_setting_parser_info;
extern struct mail_namespace_settings mail_namespace_default_settings;

const void *
mail_user_set_get_driver_settings(const struct mail_user_settings *set,
				  const char *driver);
const struct mail_storage_settings *
mail_user_set_get_storage_set(const struct mail_user_settings *set);
const void *mail_storage_get_driver_settings(struct mail_storage *storage);

enum mail_index_open_flags
mail_storage_settings_to_index_flags(const struct mail_storage_settings *set);

const struct dynamic_settings_parser *mail_storage_get_dynamic_parsers(void);

#endif
