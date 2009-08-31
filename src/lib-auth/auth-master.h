#ifndef AUTH_MASTER_H
#define AUTH_MASTER_H

#include "network.h"

struct auth_user_info {
	const char *service;
	struct ip_addr local_ip, remote_ip;
	unsigned int local_port, remote_port;
};

struct auth_user_reply {
	uid_t uid;
	gid_t gid;
	const char *user, *home, *chroot;
	ARRAY_TYPE(const_string) extra_fields;
};

struct auth_master_connection *
auth_master_init(const char *auth_socket_path, bool debug);
void auth_master_deinit(struct auth_master_connection **conn);

/* Do a USER lookup. Returns -1 = error, 0 = user not found, 1 = ok */
int auth_master_user_lookup(struct auth_master_connection *conn,
			    const char *user, const struct auth_user_info *info,
			    pool_t pool, struct auth_user_reply *reply_r);
/* Do a PASS lookup (the actual password isn't returned). */
int auth_master_pass_lookup(struct auth_master_connection *conn,
			    const char *user, const struct auth_user_info *info,
			    pool_t pool, const char *const **fields_r);

/* Iterate through all users. */
struct auth_master_user_list_ctx *
auth_master_user_list_init(struct auth_master_connection *conn);
const char *auth_master_user_list_next(struct auth_master_user_list_ctx *ctx);
unsigned int auth_master_user_list_count(struct auth_master_user_list_ctx *ctx);
/* Returns -1 if anything failed, 0 if ok */
int auth_master_user_list_deinit(struct auth_master_user_list_ctx **ctx);

#endif
