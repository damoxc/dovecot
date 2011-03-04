/* Copyright (c) 2002-2011 Dovecot authors, see the included COPYING file */

#include "auth-common.h"

#if defined (USERDB_PASSWD_FILE) || defined(PASSDB_PASSWD_FILE)

#include "userdb.h"
#include "db-passwd-file.h"

#include "buffer.h"
#include "istream.h"
#include "hash.h"
#include "str.h"
#include "eacces-error.h"
#include "var-expand.h"

#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>

static struct db_passwd_file *passwd_files;

static void passwd_file_add(struct passwd_file *pw, const char *username,
			    const char *pass, const char *const *args)
{
	/* args = uid, gid, user info, home dir, shell, extra_fields */
	struct passwd_user *pu;
	const char *extra_fields = NULL;
	char *user;
	size_t len;

	if (hash_table_lookup(pw->users, username) != NULL) {
		i_error("passwd-file %s: User %s exists more than once",
			pw->path, username);
		return;
	}

	pu = p_new(pw->pool, struct passwd_user, 1);
	user = p_strdup(pw->pool, username);

	len = pass == NULL ? 0 : strlen(pass);
	if (len > 4 && pass[0] != '{' && pass[0] != '$' &&
	    pass[len-1] == ']' && pass[len-4] == '[') {
		/* password[type] - we're being libpam-pwdfile compatible
		   here. it uses 13 = DES and 34 = MD5. For backwards
		   comaptibility with ourself, we have also 56 = Digest-MD5. */
		int num = (pass[len-3] - '0') * 10 + (pass[len-2] - '0');

		pass = t_strndup(pass, len-4);
		if (num == 34) {
			pu->password = p_strconcat(pw->pool, "{PLAIN-MD5}",
						   pass, NULL);
		} else if (num == 56) {
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
	} else {
		pu->password = p_strdup(pw->pool, pass);
	}

	pu->uid = (uid_t)-1;
	pu->gid = (gid_t)-1;

	if (*args == NULL)
		;
	else if (!pw->db->userdb || **args == '\0') {
		args++;
	} else {
		pu->uid = userdb_parse_uid(NULL, *args);
		if (pu->uid == 0 || pu->uid == (uid_t)-1) {
			i_error("passwd-file %s: User %s has invalid UID '%s'",
				pw->path, username, *args);
			return;
		}
		args++;
	}

	if (*args == NULL) {
		if (pw->db->userdb) {
			i_error("passwd-file %s: User %s is missing "
				"userdb info", pw->path, username);
		}
	} else if (!pw->db->userdb || **args == '\0')
		args++;
	else {
		pu->gid = userdb_parse_gid(NULL, *args);
		if (pu->gid == 0 || pu->gid == (gid_t)-1) {
			i_error("passwd-file %s: User %s has invalid GID '%s'",
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
		if (pw->db->userdb)
			pu->home = p_strdup_empty(pw->pool, *args);
		args++;
	}

	/* shell */
	if (*args != NULL)
		args++;

	if (*args != NULL && **args == '\0') {
		/* old format, this field is empty and next field may
		   contain MAIL */
		args++;
		if (*args != NULL && **args != '\0' && pw->db->userdb) {
			extra_fields =
                                t_strconcat("userdb_mail=",
                                            t_strarray_join(args, ":"), NULL);
		}
	} else if (*args != NULL) {
		/* new format, contains a space separated list of
		   extra fields */
                extra_fields = t_strarray_join(args, ":");
        }

        if (extra_fields != NULL) {
                pu->extra_fields =
                        p_strsplit_spaces(pw->pool, extra_fields, " ");
        }

	hash_table_insert(pw->users, user, pu);
}

static struct passwd_file *
passwd_file_new(struct db_passwd_file *db, const char *expanded_path)
{
	struct passwd_file *pw;

	pw = i_new(struct passwd_file, 1);
	pw->db = db;
	pw->path = i_strdup(expanded_path);
	pw->fd = -1;

	if (db->files != NULL)
		hash_table_insert(db->files, pw->path, pw);
	return pw;
}

static bool passwd_file_open(struct passwd_file *pw)
{
	const char *no_args = NULL;
	struct istream *input;
	const char *line;
	struct stat st;
	int fd;

	fd = open(pw->path, O_RDONLY);
	if (fd == -1) {
		if (errno == EACCES) {
			i_error("passwd-file %s: %s", pw->path,
				eacces_error_get("open", pw->path));
		} else {
			i_error("passwd-file %s: Can't open file: %m",
				pw->path);
		}
		return FALSE;
	}

	if (fstat(fd, &st) != 0) {
		i_error("passwd-file %s: fstat() failed: %m", pw->path);
		(void)close(fd);
		return FALSE;
	}

	pw->fd = fd;
	pw->stamp = st.st_mtime;
	pw->size = st.st_size;

	pw->pool = pool_alloconly_create(MEMPOOL_GROWING"passwd_file", 10240);
	pw->users = hash_table_create(default_pool, pw->pool, 100,
				      str_hash, (hash_cmp_callback_t *)strcmp);

	input = i_stream_create_fd(pw->fd, 4096, FALSE);
	i_stream_set_return_partial_line(input, TRUE);
	while ((line = i_stream_read_next_line(input)) != NULL) {
		if (*line == '\0' || *line == ':' || *line == '#')
			continue; /* no username or comment */

		T_BEGIN {
			const char *const *args = t_strsplit(line, ":");
			if (args[1] != NULL) {
				/* at least username+password */
				passwd_file_add(pw, args[0], args[1], args+2);
			} else {
				/* only username */
				passwd_file_add(pw, args[0], NULL, &no_args);
			}
		} T_END;
	}
	i_stream_destroy(&input);

	if (pw->db->debug) {
		i_debug("passwd-file %s: Read %u users",
			pw->path, hash_table_count(pw->users));
	}
	return TRUE;
}

static void passwd_file_close(struct passwd_file *pw)
{
	if (pw->fd != -1) {
		if (close(pw->fd) < 0)
			i_error("passwd-file %s: close() failed: %m", pw->path);
		pw->fd = -1;
	}

	if (pw->users != NULL)
		hash_table_destroy(&pw->users);
	if (pw->pool != NULL)
		pool_unref(&pw->pool);
}

static void passwd_file_free(struct passwd_file *pw)
{
	if (pw->db->files != NULL)
		hash_table_remove(pw->db->files, pw->path);

	passwd_file_close(pw);
	i_free(pw->path);
	i_free(pw);
}

static bool passwd_file_sync(struct passwd_file *pw)
{
	struct stat st;

	if (stat(pw->path, &st) < 0) {
		/* with variables don't give hard errors, or errors about
		   nonexistent files */
		if (errno == EACCES) {
			i_error("passwd-file %s: %s", pw->path,
				eacces_error_get("stat", pw->path));
		} else if (errno != ENOENT) {
			i_error("passwd-file %s: stat() failed: %m", pw->path);
		}

		if (pw->db->default_file != pw)
			passwd_file_free(pw);
		return FALSE;
	}

	if (st.st_mtime != pw->stamp || st.st_size != pw->size) {
		passwd_file_close(pw);
		return passwd_file_open(pw);
	}
	return TRUE;
}

static struct db_passwd_file *db_passwd_file_find(const char *path)
{
	struct db_passwd_file *f;

	for (f = passwd_files; f != NULL; f = f->next) {
		if (strcmp(f->path, path) == 0)
			return f;
	}

	return NULL;
}

struct db_passwd_file *
db_passwd_file_init(const char *path, bool userdb, bool debug)
{
	struct db_passwd_file *db;
	const char *p;
	bool percents = FALSE;

	db = db_passwd_file_find(path);
	if (db != NULL) {
		db->refcount++;
		db->userdb = TRUE;
		return db;
	}

	db = i_new(struct db_passwd_file, 1);
	db->refcount = 1;
	db->userdb = userdb;
	db->debug = debug;

	for (p = path; *p != '\0'; p++) {
		if (*p == '%' && p[1] != '\0') {
			if (var_get_key(++p) == '%')
				percents = TRUE;
			else
				db->vars = TRUE;
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
	if (db->vars) {
		db->files = hash_table_create(default_pool, default_pool, 100,
					      str_hash,
					      (hash_cmp_callback_t *)strcmp);
	} else {
		db->default_file = passwd_file_new(db, path);
	}

	db->next = passwd_files;
	passwd_files = db;
	return db;
}

void db_passwd_file_parse(struct db_passwd_file *db)
{
	if (db->default_file != NULL && db->default_file->stamp == 0) {
		/* no variables, open the file immediately */
		if (!passwd_file_open(db->default_file))
			exit(FATAL_DEFAULT);
	}
}

void db_passwd_file_unref(struct db_passwd_file **_db)
{
        struct db_passwd_file *db = *_db;
        struct db_passwd_file **p;
	struct hash_iterate_context *iter;
	void *key, *value;

	*_db = NULL;
	i_assert(db->refcount >= 0);
	if (--db->refcount > 0)
		return;

	for (p = &passwd_files; *p != NULL; p = &(*p)->next) {
		if (*p == db) {
			*p = db->next;
			break;
		}
	}

	if (db->default_file != NULL)
		passwd_file_free(db->default_file);
	else {
		iter = hash_table_iterate_init(db->files);
		while (hash_table_iterate(iter, &key, &value)) {
			struct passwd_file *file = value;

			passwd_file_free(file);
		}
		hash_table_iterate_deinit(&iter);
		hash_table_destroy(&db->files);
	}
	i_free(db->path);
	i_free(db);
}

static const char *
path_fix(const char *path,
	 const struct auth_request *auth_request ATTR_UNUSED)
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
db_passwd_file_lookup(struct db_passwd_file *db, struct auth_request *request,
		      const char *username_format)
{
	struct passwd_file *pw;
	struct passwd_user *pu;
	const struct var_expand_table *table;
	string_t *username, *dest;
	const char *path;

	if (!db->vars)
		pw = db->default_file;
	else {
		table = auth_request_get_var_expand_table(request, path_fix);
		dest = t_str_new(256);
		var_expand(dest, db->path, table);

		pw = hash_table_lookup(db->files, str_c(dest));
		if (pw == NULL) {
			/* doesn't exist yet. create lookup for it. */
			pw = passwd_file_new(db, str_c(dest));
		}
	}

	path = t_strdup(pw->path);
	if (!passwd_file_sync(pw)) {
		/* pw may be freed now */
		auth_request_log_info(request, "passwd-file",
				      "no passwd file: %s", path);
		return NULL;
	}

	username = t_str_new(256);
	table = auth_request_get_var_expand_table(request,
						  auth_request_str_escape);
	var_expand(username, username_format, table);

	auth_request_log_debug(request, "passwd-file",
			       "lookup: user=%s file=%s",
			       str_c(username), pw->path);

	pu = hash_table_lookup(pw->users, str_c(username));
	if (pu == NULL)
                auth_request_log_info(request, "passwd-file", "unknown user");
	return pu;
}

#endif
