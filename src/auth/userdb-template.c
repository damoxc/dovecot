/* Copyright (c) 2003-2013 Dovecot authors, see the included COPYING file */

#include "auth-common.h"
#include "array.h"
#include "str.h"
#include "var-expand.h"
#include "userdb.h"
#include "userdb-template.h"

struct userdb_template {
	ARRAY(const char *) args;
};

struct userdb_template *
userdb_template_build(pool_t pool, const char *userdb_name, const char *args)
{
	struct userdb_template *tmpl;
	const char *const *tmp, *key, *value, *nonull_value;
	uid_t uid;
	gid_t gid;

	tmpl = p_new(pool, struct userdb_template, 1);

	tmp = t_strsplit_spaces(args, " ");
	p_array_init(&tmpl->args, pool, str_array_length(tmp));

	for (; *tmp != NULL; tmp++) {
		value = strchr(*tmp, '=');
		if (value == NULL)
			key = *tmp;
		else
			key = t_strdup_until(*tmp, value++);

		nonull_value = value == NULL ? "" : value;
		if (strcasecmp(key, "uid") == 0) {
			uid = userdb_parse_uid(NULL, nonull_value);
			if (uid == (uid_t)-1) {
				i_fatal("%s userdb: Invalid uid: %s",
					userdb_name, nonull_value);
			}
			value = dec2str(uid);
		} else if (strcasecmp(key, "gid") == 0) {
			gid = userdb_parse_gid(NULL, nonull_value);
			if (gid == (gid_t)-1) {
				i_fatal("%s userdb: Invalid gid: %s",
					userdb_name, nonull_value);
			}
			value = dec2str(gid);
		} else if (*key == '\0') {
			i_fatal("%s userdb: Empty key (=%s)",
				userdb_name, nonull_value);
		}
		key = p_strdup(pool, key);
		value = p_strdup(pool, value);

		array_append(&tmpl->args, &key, 1);
		array_append(&tmpl->args, &value, 1);
	}
	return tmpl;
}

void userdb_template_export(struct userdb_template *tmpl,
			    struct auth_request *auth_request)
{
        const struct var_expand_table *table;
	string_t *str;
	const char *const *args, *value;
	unsigned int i, count;

	str = t_str_new(256);
	table = auth_request_get_var_expand_table(auth_request, NULL);

	args = array_get(&tmpl->args, &count);
	i_assert((count % 2) == 0);
	for (i = 0; i < count; i += 2) {
		if (args[i+1] == NULL)
			value = NULL;
		else {
			str_truncate(str, 0);
			var_expand(str, args[i+1], table);
			value = str_c(str);
		}
		auth_request_set_userdb_field(auth_request, args[i], value);
	}
}

bool userdb_template_remove(struct userdb_template *tmpl,
			    const char *key, const char **value_r)
{
	const char *const *args;
	unsigned int i, count;

	args = array_get(&tmpl->args, &count);
	i_assert((count % 2) == 0);
	for (i = 0; i < count; i += 2) {
		if (strcmp(args[i], key) == 0) {
			*value_r = args[i+1];
			array_delete(&tmpl->args, i, 2);
			return TRUE;
		}
	}
	return FALSE;
}

bool userdb_template_is_empty(struct userdb_template *tmpl)
{
	return array_count(&tmpl->args) == 0;
}
