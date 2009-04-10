/* Copyright (c) 2002-2009 Dovecot authors, see the included COPYING file */

#define _GNU_SOURCE /* setresgid() */
#include <stdio.h> /* for AIX */
#include <sys/types.h>
#include <unistd.h>

#include "lib.h"
#include "restrict-access.h"
#include "env-util.h"

#include <stdlib.h>
#include <time.h>
#include <pwd.h>
#include <grp.h>
#ifdef HAVE_PR_SET_DUMPABLE
#  include <sys/prctl.h>
#endif

static gid_t process_primary_gid = (gid_t)-1;
static gid_t process_privileged_gid = (gid_t)-1;
static bool process_using_priv_gid = FALSE;

void restrict_access_init(struct restrict_access_settings *set)
{
	memset(set, 0, sizeof(*set));

	set->uid = (uid_t)-1;
	set->gid = (gid_t)-1;
	set->privileged_gid = (gid_t)-1;
}

static const char *get_uid_str(uid_t uid)
{
	const struct passwd *pw;
	const char *ret;
	int old_errno = errno;

	pw = getpwuid(uid);
	if (pw == NULL)
		ret = dec2str(uid);
	else
		ret = t_strdup_printf("%s(%s)", dec2str(uid), pw->pw_name);
	errno = old_errno;
	return ret;
}

static const char *get_gid_str(gid_t gid)
{
	const struct group *group;
	const char *ret;
	int old_errno = errno;

	group = getgrgid(gid);
	if (group == NULL)
		ret = dec2str(gid);
	else
		ret = t_strdup_printf("%s(%s)", dec2str(gid), group->gr_name);
	errno = old_errno;
	return ret;
}

static void restrict_init_groups(gid_t primary_gid, gid_t privileged_gid)
{
	if (privileged_gid == (gid_t)-1) {
		if (primary_gid == getgid() && primary_gid == getegid()) {
			/* everything is already set */
			return;
		}

		if (setgid(primary_gid) != 0) {
			i_fatal("setgid(%s) failed with euid=%s, "
				"gid=%s, egid=%s: %m",
				get_gid_str(primary_gid), get_uid_str(geteuid()),
				get_gid_str(getgid()), get_gid_str(getegid()));
		}
		return;
	}

	if (getegid() != 0 && primary_gid == getgid() &&
	    primary_gid == getegid()) {
		/* privileged_gid is hopefully in saved ID. if not,
		   there's nothing we can do about it. */
		return;
	}

#ifdef HAVE_SETRESGID
	if (setresgid(primary_gid, primary_gid, privileged_gid) != 0) {
		i_fatal("setresgid(%s,%s,%s) failed with euid=%s: %m",
			get_gid_str(primary_gid), get_gid_str(primary_gid),
			get_gid_str(privileged_gid), get_uid_str(geteuid()));
	}
#else
	if (geteuid() == 0) {
		/* real, effective, saved -> privileged_gid */
		if (setgid(privileged_gid) < 0) {
			i_fatal("setgid(%s) failed: %m",
				get_gid_str(privileged_gid));
		}
	}
	/* real, effective -> primary_gid
	   saved -> keep */
	if (setregid(primary_gid, primary_gid) != 0) {
		i_fatal("setregid(%s,%s) failed with euid=%s: %m",
			get_gid_str(primary_gid), get_gid_str(privileged_gid),
			get_uid_str(geteuid()));
	}
#endif
}

static gid_t *get_groups_list(unsigned int *gid_count_r)
{
	gid_t *gid_list;
	int ret, gid_count;

	if ((gid_count = getgroups(0, NULL)) < 0)
		i_fatal("getgroups() failed: %m");

	/* @UNSAFE */
	gid_list = t_new(gid_t, gid_count);
	if ((ret = getgroups(gid_count, gid_list)) < 0)
		i_fatal("getgroups() failed: %m");

	*gid_count_r = ret;
	return gid_list;
}

static void drop_restricted_groups(const struct restrict_access_settings *set,
				   gid_t *gid_list, unsigned int *gid_count,
				   bool *have_root_group)
{
	/* @UNSAFE */
	unsigned int i, used;

	for (i = 0, used = 0; i < *gid_count; i++) {
		if (gid_list[i] >= set->first_valid_gid &&
		    (set->last_valid_gid == 0 ||
		     gid_list[i] <= set->last_valid_gid)) {
			if (gid_list[i] == 0)
				*have_root_group = TRUE;
			gid_list[used++] = gid_list[i];
		}
	}
	*gid_count = used;
}

static gid_t get_group_id(const char *name)
{
	struct group *group;

	if (is_numeric(name, '\0'))
		return (gid_t)strtoul(name, NULL, 10);

	group = getgrnam(name);
	if (group == NULL)
		i_fatal("unknown group name in extra_groups: %s", name);
	return group->gr_gid;
}

static void fix_groups_list(const struct restrict_access_settings *set,
			    bool preserve_existing, bool *have_root_group)
{
	gid_t gid, *gid_list, *gid_list2;
	const char *const *tmp, *empty = NULL;
	unsigned int i, gid_count;
	bool add_primary_gid;

	/* if we're using a privileged GID, we can temporarily drop our
	   effective GID. we still want to be able to use its privileges,
	   so add it to supplementary groups. */
	add_primary_gid = process_privileged_gid != (gid_t)-1;

	tmp = set->extra_groups == NULL ? &empty :
		t_strsplit_spaces(set->extra_groups, ", ");

	if (preserve_existing) {
		gid_list = get_groups_list(&gid_count);
		drop_restricted_groups(set, gid_list, &gid_count,
				       have_root_group);
		/* see if the list already contains the primary GID */
		for (i = 0; i < gid_count; i++) {
			if (gid_list[i] == process_primary_gid) {
				add_primary_gid = FALSE;
				break;
			}
		}
	} else {
		gid_list = NULL;
		gid_count = 0;
	}
	if (gid_count == 0) {
		/* Some OSes don't like an empty groups list,
		   so use the primary GID as the only one. */
		gid_list = t_new(gid_t, 2);
		gid_list[0] = process_primary_gid;
		gid_count = 1;
		add_primary_gid = FALSE;
	}

	if (*tmp != NULL || add_primary_gid) {
		/* @UNSAFE: add extra groups and/or primary GID to gids list */
		gid_list2 = t_new(gid_t, gid_count + str_array_length(tmp) + 1);
		memcpy(gid_list2, gid_list, gid_count * sizeof(gid_t));
		for (; *tmp != NULL; tmp++) {
			gid = get_group_id(*tmp);
			if (gid != process_primary_gid)
				gid_list2[gid_count++] = gid;
		}
		if (add_primary_gid)
			gid_list2[gid_count++] = process_primary_gid;
		gid_list = gid_list2;
	}

	if (setgroups(gid_count, gid_list) < 0) {
		if (errno == EINVAL) {
			i_fatal("setgroups(%s) failed: Too many extra groups",
				set->extra_groups == NULL ? "" :
				set->extra_groups);
		} else {
			i_fatal("setgroups() failed: %m");
		}
	}
}

void restrict_access(const struct restrict_access_settings *set,
		     const char *home, bool disallow_root)
{
	bool is_root, have_root_group, preserve_groups = FALSE;
	bool allow_root_gid;

	is_root = geteuid() == 0;

	/* set the primary/privileged group */
	process_primary_gid = set->gid;
	process_privileged_gid = set->privileged_gid;

	have_root_group = process_primary_gid == 0;
	if (process_primary_gid != (gid_t)-1 ||
	    process_privileged_gid != (gid_t)-1) {
		if (process_primary_gid == (gid_t)-1)
			process_primary_gid = getegid();
		restrict_init_groups(process_primary_gid,
				     process_privileged_gid);
	} else {
		if (process_primary_gid == (gid_t)-1)
			process_primary_gid = getegid();
	}

	/* set system user's groups */
	if (set->system_groups_user != NULL && is_root) {
		if (initgroups(set->system_groups_user,
			       process_primary_gid) < 0) {
			i_fatal("initgroups(%s, %s) failed: %m",
				set->system_groups_user,
				get_gid_str(process_primary_gid));
		}
		preserve_groups = TRUE;
	}

	/* add extra groups. if we set system user's groups, drop the
	   restricted groups at the same time. */
	if (is_root) T_BEGIN {
		fix_groups_list(set, preserve_groups,
				&have_root_group);
	} T_END;

	/* chrooting */
	if (set->chroot_dir != NULL) {
		/* kludge: localtime() must be called before chroot(),
		   or the timezone isn't known */
		time_t t = 0;
		(void)localtime(&t);

		if (chroot(set->chroot_dir) != 0)
			i_fatal("chroot(%s) failed: %m", set->chroot_dir);

		if (home != NULL) {
			if (chdir(home) < 0) {
				i_error("chdir(%s) failed: %m", home);
				home = NULL;
			}
		}
		if (home == NULL) {
			if (chdir("/") != 0)
				i_fatal("chdir(/) failed: %m");
		}
	}

	/* uid last */
	if (set->uid != (uid_t)-1) {
		if (setuid(set->uid) != 0) {
			i_fatal("setuid(%s) failed with euid=%s: %m",
				get_uid_str(set->uid), get_uid_str(geteuid()));
		}
	}

	/* verify that we actually dropped the privileges */
	if ((set->uid != (uid_t)-1 && set->uid != 0) || disallow_root) {
		if (setuid(0) == 0) {
			if (disallow_root &&
			    (set->uid == 0 || set->uid == (uid_t)-1))
				i_fatal("This process must not be run as root");

			i_fatal("We couldn't drop root privileges");
		}
	}

	if (set->first_valid_gid != 0)
		allow_root_gid = FALSE;
	else if (process_primary_gid == 0 || process_privileged_gid == 0)
		allow_root_gid = TRUE;
	else
		allow_root_gid = FALSE;

	if (!allow_root_gid && set->uid != 0 &&
	    (set->uid != (uid_t)-1 || !is_root)) {
		if (getgid() == 0 || getegid() == 0 || setgid(0) == 0) {
			if (process_primary_gid == 0)
				i_fatal("GID 0 isn't permitted");
			i_fatal("We couldn't drop root group privileges "
				"(wanted=%s, gid=%s, egid=%s)",
				get_gid_str(process_primary_gid),
				get_gid_str(getgid()), get_gid_str(getegid()));
		}
	}
}

void restrict_access_set_env(const struct restrict_access_settings *set)
{
	if (set->system_groups_user != NULL &&
	    *set->system_groups_user != '\0') {
		env_put(t_strconcat("RESTRICT_USER=",
				    set->system_groups_user, NULL));
	}
	if (set->chroot_dir != NULL && *set->chroot_dir != '\0')
		env_put(t_strconcat("RESTRICT_CHROOT=", set->chroot_dir, NULL));

	if (set->uid != (uid_t)-1) {
		env_put(t_strdup_printf("RESTRICT_SETUID=%s",
					dec2str(set->uid)));
	}
	if (set->gid != (gid_t)-1) {
		env_put(t_strdup_printf("RESTRICT_SETGID=%s",
					dec2str(set->gid)));
	}
	if (set->privileged_gid != (gid_t)-1) {
		env_put(t_strdup_printf("RESTRICT_SETGID_PRIV=%s",
					dec2str(set->privileged_gid)));
	}
	if (set->extra_groups != NULL && *set->extra_groups != '\0') {
		env_put(t_strconcat("RESTRICT_SETEXTRAGROUPS=",
				    set->extra_groups, NULL));
	}

	if (set->first_valid_gid != 0) {
		env_put(t_strdup_printf("RESTRICT_GID_FIRST=%s",
					dec2str(set->first_valid_gid)));
	}
	if (set->last_valid_gid != 0) {
		env_put(t_strdup_printf("RESTRICT_GID_LAST=%s",
					dec2str(set->last_valid_gid)));
	}
}

static const char *null_if_empty(const char *str)
{
	return str == NULL || *str == '\0' ? NULL : str;
}

void restrict_access_by_env(const char *home, bool disallow_root)
{
	struct restrict_access_settings set;
	const char *value;

	restrict_access_init(&set);

	if ((value = getenv("RESTRICT_SETUID")) != NULL)
		set.uid = (uid_t)strtol(value, NULL, 10);
	if ((value = getenv("RESTRICT_SETGID")) != NULL)
		set.gid = (gid_t)strtol(value, NULL, 10);
	if ((value = getenv("RESTRICT_SETGID_PRIV")) != NULL)
		set.privileged_gid = (gid_t)strtol(value, NULL, 10);
	if ((value = getenv("RESTRICT_GID_FIRST")) != NULL)
		set.first_valid_gid = (gid_t)strtol(value, NULL, 10);
	if ((value = getenv("RESTRICT_GID_LAST")) != NULL)
		set.last_valid_gid = (gid_t)strtol(value, NULL, 10);

	set.extra_groups = null_if_empty(getenv("RESTRICT_SETEXTRAGROUPS"));
	set.system_groups_user = null_if_empty(getenv("RESTRICT_USER"));
	set.chroot_dir = null_if_empty(getenv("RESTRICT_CHROOT"));

	restrict_access(&set, home, disallow_root);

	/* clear the environment, so we don't fail if we get back here */
	env_remove("RESTRICT_SETUID");
	if (process_privileged_gid == (gid_t)-1) {
		/* if we're dropping privileges before executing and
		   a privileged group is set, the groups must be fixed
		   after exec */
		env_remove("RESTRICT_SETGID");
		env_remove("RESTRICT_SETGID_PRIV");
	}
	env_remove("RESTRICT_GID_FIRST");
	env_remove("RESTRICT_GID_LAST");
	env_remove("RESTRICT_SETEXTRAGROUPS");
	env_remove("RESTRICT_USER");
	env_remove("RESTRICT_CHROOT");
}

void restrict_access_allow_coredumps(bool allow ATTR_UNUSED)
{
#ifdef HAVE_PR_SET_DUMPABLE
	(void)prctl(PR_SET_DUMPABLE, allow, 0, 0, 0);
#endif
}

int restrict_access_use_priv_gid(void)
{
	i_assert(!process_using_priv_gid);

	if (process_privileged_gid == (gid_t)-1)
		return 0;
	if (setegid(process_privileged_gid) < 0) {
		i_error("setegid(privileged) failed: %m");
		return -1;
	}
	process_using_priv_gid = TRUE;
	return 0;
}

void restrict_access_drop_priv_gid(void)
{
	if (!process_using_priv_gid)
		return;

	if (setegid(process_primary_gid) < 0)
		i_fatal("setegid(primary) failed: %m");
	process_using_priv_gid = FALSE;
}

bool restrict_access_have_priv_gid(void)
{
	return process_privileged_gid != (gid_t)-1;
}
