/* Copyright (c) 2013 Dovecot authors, see the included COPYING file */

#include "lib.h"
#include "array.h"
#include "str.h"
#include "settings.h"
#include "dict-mongodb-settings.h"

#include <ctype.h>

enum section_type {
	SECTION_ROOT = 0,
	SECTION_MAP
};

struct dict_mongodb_map_field {
	const char *field, *variable;
};

struct setting_parser_ctx {
	pool_t pool;
	struct dict_mongodb_settings *set;
	enum section_type type;

	struct dict_mongodb_map cur_map;
	ARRAY(struct dict_mongodb_map_field) cur_fields;
	ARRAY(struct dict_mongodb_map_field) cur_values;
};

#undef DEF_STR
#define DEF_STR(name) DEF_STRUCT_STR(name, dict_mongodb_map)

static const struct setting_def dict_mongodb_map_setting_defs[] = {
	DEF_STR(pattern),
	DEF_STR(collection),
	DEF_STR(username_field),
	DEF_STR(value_field),

	{ 0, NULL, 0 }
};
#undef DEF_STR

static const char *pattern_read_name(const char **pattern)
{
	const char *p = *pattern, *name;

	if (*p == '{') {
		/* ${name} */
		name = ++p;
		p = strchr(p, '}');
		if (p == NULL) {
			/* error, but allow anyway */
			*pattern += strlen(*pattern);
			return "";
		}
		*pattern = p + 1;
	} else {
		/* $name - ends at the first non-alnum_ character */
		name = p;
		for (; *p != '\0'; p++) {
			if (!i_isalnum(*p) && *p != '_')
				break;
		}
		*pattern = p;
	}
	name = t_strdup_until(name, p);
	return name;
}

static const char *dict_mongodb_map_finish(struct setting_parser_ctx *ctx)
{
	if (ctx->cur_map.pattern == NULL)
		return "Missing setting: pattern";
	if (ctx->cur_map.collection == NULL)
		return "Missing setting: collection";

	if (ctx->cur_map.value_field == NULL)
		return "Missing setting: value_field";

	if (ctx->cur_map.username_field == NULL) {
		/* not all queries require this */
		ctx->cur_map.username_field = "'username_field not set'";
	}

	array_append(&ctx->set->maps, &ctx->cur_map, 1);
	memset(&ctx->cur_map, 0, sizeof(ctx->cur_map));
	return NULL;
}

static const char *
parse_setting(const char *key, const char *value,
	      struct setting_parser_ctx *ctx)
{
	switch (ctx->type) {
		case SECTION_ROOT:
			if (strcmp(key, "uri") == 0) {
				ctx->set->uri = p_strdup(ctx->pool, value);
				return NULL;
			}
			break;
		case SECTION_MAP:
			return parse_setting_from_defs(ctx->pool,
						       dict_mongodb_map_setting_defs,
						       &ctx->cur_map, key, value);
	}

	return t_strconcat("Unknown setting: ", key, NULL);
}

static bool
parse_section(const char *type, const char *name ATTR_UNUSED,
	      struct setting_parser_ctx *ctx, const char **error_r)
{
	switch (ctx->type) {
	case SECTION_ROOT:
		if (type == NULL)
			return FALSE;
		if (strcmp(type, "map") == 0) {
			ctx->type = SECTION_MAP;
			return TRUE;
		}
		break;
	case SECTION_MAP:
		if (type == NULL) {
			ctx->type = SECTION_ROOT;
			*error_r = dict_mongodb_map_finish(ctx);
			return FALSE;
		}
		break;
	}
	*error_r = t_strconcat("Unknown section: ", type, NULL);
	return FALSE;
}

struct dict_mongodb_settings *
dict_mongodb_settings_read(pool_t pool, const char *path, const char **error_r)
{
	struct setting_parser_ctx ctx;

	memset(&ctx, 0, sizeof(ctx));
	ctx.pool = pool;
	ctx.set = p_new(pool, struct dict_mongodb_settings, 1);
	p_array_init(&ctx.set->maps, pool, 8);

	if (!settings_read(path, NULL, parse_setting, parse_section,
			   &ctx, error_r))
		return NULL;

	if (ctx.set->uri == NULL) {
		*error_r = t_strdup_printf("Error in configuration file %s: "
					   "Missing connect setting", path);
		return NULL;
	}

	return ctx.set;
}

// vim: noexpandtab shiftwidth=8 tabstop=8