#ifndef SETTINGS_PARSER_H
#define SETTINGS_PARSER_H

struct var_expand_table;

#define SETTINGS_SEPARATOR '/'
#define SETTINGS_SEPARATOR_S "/"

/* STR_VARS pointer begins with either of these initially. Before actually
   using the variables all variables in all unexpanded strings need to be
   expanded. Afterwards the string pointers should be increased to skip
   the initial '1' so it'll be easy to use them. */
#define SETTING_STRVAR_UNEXPANDED "0"
#define SETTING_STRVAR_EXPANDED "1"

/* When parsing streams, this character is translated to LF. */
#define SETTING_STREAM_LF_CHAR "\003"

enum setting_type {
	SET_INTERNAL, /* don't set this variable */
	SET_BOOL,
	SET_UINT,
	SET_STR,
	SET_STR_VARS, /* string with %variables */
	SET_ENUM,
	SET_DEFLIST, /* of type array_t */
	SET_STRLIST /* of type ARRAY_TYPE(const_string) */
};

#define SETTING_DEFINE_LIST_END { 0, NULL, 0, NULL }

struct setting_define {
	enum setting_type type;
	const char *key;

	size_t offset;
	const struct setting_parser_info *list_info;
};

#define SETTING_DEFINE_STRUCT_BOOL(name, struct_name) \
	{ SET_BOOL + COMPILE_ERROR_IF_TYPES_NOT_COMPATIBLE( \
		((struct struct_name *)0)->name, bool), \
	  #name, offsetof(struct struct_name, name), NULL }
#define SETTING_DEFINE_STRUCT_UINT(name, struct_name) \
	{ SET_UINT + COMPILE_ERROR_IF_TYPES_NOT_COMPATIBLE( \
		((struct struct_name *)0)->name, unsigned int), \
	  #name, offsetof(struct struct_name, name), NULL }
#define SETTING_DEFINE_STRUCT_STR(name, struct_name) \
	{ SET_STR + COMPILE_ERROR_IF_TYPES_NOT_COMPATIBLE( \
		((struct struct_name *)0)->name, const char *), \
	  #name, offsetof(struct struct_name, name), NULL }

struct setting_parser_info {
	const struct setting_define *defines;
	const void *defaults;

	struct setting_parser_info *parent;
	struct dynamic_settings_parser *dynamic_parsers;

	size_t parent_offset;
	size_t type_offset;
	size_t struct_size;
	bool (*check_func)(void *set, pool_t pool, const char **error_r);
	struct setting_parser_info *const *dependencies;

};
ARRAY_DEFINE_TYPE(setting_parser_info, struct setting_parser_info);

/* name=NULL-terminated list of parsers. These follow the static settings.
   After this list follows the actual settings. */
struct dynamic_settings_parser {
	const char *name;
	const struct setting_parser_info *info;
	size_t struct_offset;
};
ARRAY_DEFINE_TYPE(dynamic_settings_parser, struct dynamic_settings_parser);

enum settings_parser_flags {
	SETTINGS_PARSER_FLAG_IGNORE_UNKNOWN_KEYS	= 0x01
};

struct setting_parser_context;

struct setting_parser_context *
settings_parser_init(pool_t set_pool, const struct setting_parser_info *root,
		     enum settings_parser_flags flags);
struct setting_parser_context *
settings_parser_init_list(pool_t set_pool,
			  const struct setting_parser_info *const *roots,
			  unsigned int count, enum settings_parser_flags flags);
void settings_parser_deinit(struct setting_parser_context **ctx);

/* Return pointer to root setting structure. */
void *settings_parser_get(struct setting_parser_context *ctx);
/* If there are multiple roots, return list to all of their settings. */
void **settings_parser_get_list(struct setting_parser_context *ctx);

/* Return the last error. */
const char *settings_parser_get_error(struct setting_parser_context *ctx);
/* Return the parser info used for the previously parsed line. */
const struct setting_parser_info *
settings_parse_get_prev_info(struct setting_parser_context *ctx);
/* Save all parsed input to given string. */
void settings_parse_save_input(struct setting_parser_context *ctx,
			       string_t *dest);

/* Returns TRUE if the given key is a valid setting. */
bool settings_parse_is_valid_key(struct setting_parser_context *ctx,
				 const char *key);
/* Parse a single line. Returns 1 if OK, 0 if key is unknown, -1 if error. */
int settings_parse_line(struct setting_parser_context *ctx, const char *line);
/* Parse data already read in input stream. */
int settings_parse_stream(struct setting_parser_context *ctx,
			  struct istream *input);
/* Read data from input stream and parser it. returns -1 = error,
   0 = done, 1 = not finished yet (stream is non-blocking) */
int settings_parse_stream_read(struct setting_parser_context *ctx,
         		       struct istream *input);
/* Open file and parse it. */
int settings_parse_file(struct setting_parser_context *ctx,
			const char *path, size_t max_line_length);
int settings_parse_environ(struct setting_parser_context *ctx);
/* Execute the given binary and wait for it to return the configuration. */
int settings_parse_exec(struct setting_parser_context *ctx,
			const char *bin_path, const char *config_path,
			const char *service);
/* Call all check_func()s to see if currently parsed settings are valid. */
bool settings_parser_check(struct setting_parser_context *ctx, pool_t pool,
			   const char **error_r);

/* While parsing values, specifies if STR_VARS strings are already expanded. */
void settings_parse_set_expanded(struct setting_parser_context *ctx,
				 bool is_expanded);
/* Mark all the parsed settings with given keys as being already expanded. */
void settings_parse_set_key_expandeded(struct setting_parser_context *ctx,
				       pool_t pool, const char *key);
void settings_parse_set_keys_expandeded(struct setting_parser_context *ctx,
					pool_t pool, const char *const *keys);
/* Expand all unexpanded variables using the given table. Update the string
   pointers so that they can be used without skipping over the '1'. */
void settings_var_expand(const struct setting_parser_info *info,
			 void *set, pool_t pool,
			 const struct var_expand_table *table);
/* Go through all the settings and return the first one that has an unexpanded
   setting containing the given %key. */
bool settings_vars_have_key(const struct setting_parser_info *info, void *set,
			    char var_key, const char *long_var_key,
			    const char **key_r, const char **value_r);
/* Duplicate the entire settings structure. */
void *settings_dup(const struct setting_parser_info *info,
		   const void *set, pool_t pool);
/* Duplicate the entire setting parser. */
struct setting_parser_context *
settings_parser_dup(struct setting_parser_context *old_ctx, pool_t new_pool);

/* parsers is a name=NULL -terminated list. The parsers are appended as
   dynamic_settings_list structures to their parent. All must have the same
   parent. The new structures are allocated from the given pool. */
void settings_parser_info_update(pool_t pool,
				 const struct dynamic_settings_parser *parsers);

/* Return pointer to beginning of settings for given name, or NULL if there is
   no such registered name. */
const void *settings_find_dynamic(struct setting_parser_info *info,
				  const void *base_set, const char *name);

#endif
