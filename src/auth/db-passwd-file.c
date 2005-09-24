/* Copyright (C) 2002-2003 Timo Sirainen */

#include "common.h"

#if defined (USERDB_PASSWD_FILE) || defined(PASSDB_PASSWD_FILE)

#include "userdb.h"
#include "db-passwd-file.h"

#include "buffer.h"
#include "istream.h"
#include "hash.h"
#include "str.h"
#include "var-expand.h"

#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>

static void passwd_file_add(struct passwd_file *pw, const char *username,
			    const char *pass, const char *const *args)
{
	/* args = uid, gid, user info, home dir, shell, flags, mail */
	struct passwd_user *pu;
	const char *p;

	if (hash_lookup(pw->users, username) != NULL) {
		i_error("passwd-file %s: User %s exists more than once",
			pw->path, username);
		return;
	}

	pu = p_new(pw->pool, struct passwd_user, 1);
	pu->user_realm = p_strdup(pw->pool, username);

	pu->realm = strchr(pu->user_realm, '@');
	if (pu->realm != NULL)
		pu->realm++;

	p = pass == NULL ? NULL : strchr(pass, '[');
	if (p == NULL) {
		pu->password = p_strdup(pw->pool, pass);
	} else {
		/* password[type] - we're being libpam-pwdfile compatible
		   here. it uses 13 = DES and 34 = MD5. For backwards
		   comaptibility with ourself, we have also 56 = Digest-MD5. */
		pass = t_strdup_until(pass, p);
		if (p[1] == '3' && p[2] == '4') {
			pu->password = p_strconcat(pw->pool, "{PLAIN-MD5}",
						   pass, NULL);
		} else if (p[1] == '5' && p[2] == '6') {
			pu->password = p_strconcat(pw->pool, "{DIGEST-MD5}",
						   pass, NULL);
			if (strlen(pu->password) != 32 + 12) {
				i_error("passwd-file %s: User %s "
					"has invalid password",
					pw->path, username);
				return;
			}
		} else {
			pu->password = p_strconcat(pw->pool, "{CRYPT}",
						   pass, NULL);
		}
	}

	if (*args != NULL) {
		pu->uid = userdb_parse_uid(NULL, *args);
		if (pu->uid == 0 || pu->uid == (uid_t)-1) {
			i_error("passwd-file %s: User %s has invalid UID %s",
				pw->path, username, *args);
			return;
		}
		args++;
	}

	if (*args != NULL) {
		pu->gid = userdb_parse_gid(NULL, *args);
		if (pu->gid == 0 || pu->gid == (gid_t)-1) {
			i_error("passwd-file %s: User %s has invalid GID %s",
				pw->path, username, *args);
			return;
		}
		args++;
	}

	/* user info */
	if (*args != NULL)
		args++;

	/* home */
	if (*args != NULL) {
		pu->home = p_strdup_empty(pw->pool, *args);
		args++;
	}

	/* shell */
	if (*args != NULL)
		args++;

	/* flags */
	if (*args != NULL) {
		/* no flags currently */
		args++;
	}

	/* rest is MAIL environment */
	if (*args != NULL) {
		string_t *str = t_str_new(100);
		str_append(str, *args);
		args++;

		while (*args != NULL) {
			str_append_c(str, ':');
			str_append(str, *args);
			args++;
		}
		pu->mail = p_strdup_empty(pw->pool, str_c(str));
	}

	hash_insert(pw->users, pu->user_realm, pu);
}

static struct passwd_file *
passwd_file_new(struct db_passwd_file *db, const char *expanded_path)
{
	struct passwd_file *pw;

	pw = i_new(struct passwd_file, 1);
	pw->db = db;
	pw->path = i_strdup(expanded_path);
	pw->fd = -1;

	hash_insert(db->files, pw->path, pw);
	return pw;
}

static int passwd_file_open(struct passwd_file *pw)
{
	const char *no_args = NULL;
	struct istream *input;
	const char *const *args;
	const char *line;
	struct stat st;
	int fd;

	fd = open(pw->path, O_RDONLY);
	if (fd == -1) {
		i_error("passwd-file %s: Can't open file: %m", pw->path);
		return FALSE;
	}

	if (fstat(fd, &st) != 0) {
		i_error("passwd-file %s: fstat() failed: %m", pw->path);
		(void)close(fd);
		return FALSE;
	}

	pw->fd = fd;
	pw->stamp = st.st_mtime;

	pw->pool = pool_alloconly_create("passwd_file", 10240);;
	pw->users = hash_create(default_pool, pw->pool, 100,
				str_hash, (hash_cmp_callback_t *)strcmp);

	input = i_stream_create_file(pw->fd, default_pool, 4096, FALSE);
	while ((line = i_stream_read_next_line(input)) != NULL) {
		if (*line == '\0' || *line == ':' || *line == '#')
			continue; /* no username or comment */

		t_push();
		args = t_strsplit(line, ":");
		if (args[1] != NULL) {
			/* at least username+password */
			passwd_file_add(pw, args[0], args[1],
					pw->db->userdb ? args+2 : &no_args);
		} else {
			/* only username */
			passwd_file_add(pw, args[0], NULL, &no_args);
		}
		t_pop();
	}
	i_stream_unref(input);
	return TRUE;
}

static void passwd_file_close(struct passwd_file *pw)
{
	if (pw->fd != -1) {
		if (close(pw->fd) < 0)
			i_error("passwd-file %s: close() failed: %m", pw->path);
		pw->fd = -1;
	}

	if (pw->users != NULL) {
		hash_destroy(pw->users);
		pw->users = NULL;
	}
	if (pw->pool != NULL) {
		pool_unref(pw->pool);
		pw->pool = NULL;
	}
}

static void passwd_file_free(struct passwd_file *pw)
{
	hash_remove(pw->db->files, pw->path);

	passwd_file_close(pw);
	i_free(pw->path);
	i_free(pw);
}

static int passwd_file_sync(struct passwd_file *pw)
{
	struct stat st;

	if (stat(pw->path, &st) < 0) {
		/* with variables don't give hard errors, or errors about
		   nonexisting files */
		if (errno != ENOENT)
			i_error("passwd-file %s: stat() failed: %m", pw->path);

		passwd_file_free(pw);
		return FALSE;
	}

	if (st.st_mtime != pw->stamp) {
		passwd_file_close(pw);
		return passwd_file_open(pw);
	}
	return TRUE;
}

struct db_passwd_file *db_passwd_file_parse(const char *path, int userdb)
{
	struct db_passwd_file *db;
	const char *p;
	int percents = FALSE;

	db = i_new(struct db_passwd_file, 1);
	db->refcount = 1;
	db->userdb = userdb;
	db->files = hash_create(default_pool, default_pool, 100,
				str_hash, (hash_cmp_callback_t *)strcmp);

	for (p = path; *p != '\0'; p++) {
		if (*p == '%' && p[1] != '\0') {
			p++;
			switch (var_get_key(p)) {
			case 'd':
				db->domain_var = TRUE;
				db->vars = TRUE;
				break;
			case '%':
				percents = TRUE;
				break;
			default:
				db->vars = TRUE;
				break;
			}
		}
	}

	if (percents && !db->vars) {
		/* just extra escaped % chars. remove them. */
		struct var_expand_table empty_table[1];
		string_t *dest;

		empty_table[0].key = '\0';
		dest = t_str_new(256);
		var_expand(dest, path, empty_table);
		path = str_c(dest);
	}

	db->path = i_strdup(path);

	if (!db->vars) {
		/* no variables, open the file immediately */
		db->default_file = passwd_file_new(db, path);
		if (!passwd_file_open(db->default_file))
			exit(FATAL_DEFAULT);
	}
	return db;
}

void db_passwd_file_unref(struct db_passwd_file *db)
{
	struct hash_iterate_context *iter;
	void *key, *value;

	if (--db->refcount == 0) {
		iter = hash_iterate_init(db->files);
		while (hash_iterate(iter, &key, &value))
			passwd_file_free(value);
		hash_iterate_deinit(iter);

		hash_destroy(db->files);
		i_free(db->path);
		i_free(db);
	}
}

static const char *path_fix(const char *path)
{
	const char *p;

	p = strchr(path, '/');
	if (p == NULL)
		return path;

	/* most likely this is an invalid request. just cut off the '/' and
	   everything after it. */
	return t_strdup_until(path, p);
}

struct passwd_user *
db_passwd_file_lookup(struct db_passwd_file *db, struct auth_request *request)
{
	struct passwd_file *pw;
	struct passwd_user *pu;

	if (!db->vars)
		pw = db->default_file;
	else {
		const struct var_expand_table *table;
		string_t *dest;

		t_push();

		table = auth_request_get_var_expand_table(request, path_fix);
		dest = t_str_new(256);
		var_expand(dest, db->path, table);

		pw = hash_lookup(db->files, str_c(dest));
		if (pw == NULL) {
			/* doesn't exist yet. create lookup for it. */
			pw = passwd_file_new(db, str_c(dest));
		}

		t_pop();
	}

	if (!passwd_file_sync(pw)) {
		auth_request_log_info(request, "passwd-file",
				      "no passwd file");
		return NULL;
	}

	t_push();
	pu = hash_lookup(pw->users, !db->domain_var ? request->user :
			 t_strcut(request->user, '@'));
	if (pu == NULL)
                auth_request_log_info(request, "passwd-file", "unknown user");
	t_pop();

	return pu;
}

#endif
