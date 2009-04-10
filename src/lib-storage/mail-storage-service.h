#ifndef MAIL_STORAGE_SERVICE_H
#define MAIL_STORAGE_SERVICE_H

enum mail_storage_service_flags {
	MAIL_STORAGE_SERVICE_FLAG_DISALLOW_ROOT		= 0x01,
	MAIL_STORAGE_SERVICE_FLAG_USERDB_LOOKUP		= 0x02
};

struct setting_parser_info;

struct mail_user *
mail_storage_service_init_user(struct master_service *service, const char *user,
			       const struct setting_parser_info *set_root,
			       enum mail_storage_service_flags flags);
void mail_storage_service_deinit_user(void);

struct mail_storage_service_multi_ctx *
mail_storage_service_multi_init(struct master_service *service,
				const struct setting_parser_info *set_root,
				enum mail_storage_service_flags flags);
/* Returns 1 if ok, 0 if user wasn't found, -1 if error. */
int mail_storage_service_multi_next(struct mail_storage_service_multi_ctx *ctx,
				    const char *user,
				    struct mail_user **mail_user_r,
				    const char **error_r);
void mail_storage_service_multi_deinit(struct mail_storage_service_multi_ctx **ctx);

/* Return the settings pointed to by set_root parameter in _init() */
void *mail_storage_service_get_settings(struct master_service *service);

#endif
