#ifndef DICT_MONGODB_SETTINGS_H
#define DICT_MONGODB_SETTINGS_H

struct dict_mongodb_map {
	/* pattern is in simplified form: all variables are stored as simple
	   '$' character. fields array is sorted by the variable index. */
	const char *pattern;
	const char *collection;
	const char *username_field;
	const char *value_field;
};

struct dict_mongodb_settings {
	const char *uri;

	unsigned int max_field_count;
	ARRAY(struct dict_mongodb_map) maps;
};

struct dict_mongodb_settings *
dict_mongodb_settings_read(pool_t pool, const char *path, const char **error_r);

#endif