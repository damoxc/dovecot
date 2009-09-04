#ifndef COMMON_H
#define COMMON_H

#include "lib.h"
#include "master-interface.h"
#include "master-settings.h"

#define AUTH_SUCCESS_PATH PKG_STATEDIR"/auth-success"

extern uid_t master_uid;
extern gid_t master_gid;
extern bool auth_success_written;
extern bool core_dumps_disabled;
extern int null_fd;
extern struct service_list *services;

void process_exec(const char *cmd, const char *extra_args[]) ATTR_NORETURN;

int get_uidgid(const char *user, uid_t *uid_r, gid_t *gid_r,
	       const char **error_r);
int get_gid(const char *group, gid_t *gid_r, const char **error_r);

#endif
