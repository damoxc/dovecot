/* Copyright (C) 2005-2009 Dovecot authors, see the included COPYING file */

#include "lib.h"
#include "array.h"
#include "str.h"
#include "hash.h"
#include "ostream.h"
#include "settings-parser.h"
#include "master-service-settings.h"
#include "all-settings.h"
#include "config-parser.h"
#include "config-request.h"

struct settings_export_context {
	pool_t pool;
	string_t *value;
	string_t *prefix;
	struct hash_table *keys;
	enum config_dump_scope scope;

	config_request_callback_t *callback;
	void *context;
};

static bool parsers_are_connected(struct setting_parser_info *root,
				  struct setting_parser_info *info)
{
	struct setting_parser_info *const *dep, *p;

	/* we're trying to find info or its parents from root's dependencies. */

	for (p = info; p != NULL; p = p->parent) {
		if (p == root)
			return TRUE;
	}

	if (root->dependencies != NULL) {
		for (dep = root->dependencies; *dep != NULL; dep++) {
			for (p = info; p != NULL; p = p->parent) {
				if (p == *dep)
					return TRUE;
			}
		}
	}
	return FALSE;
}

static bool
config_module_parser_is_in_service(const struct config_module_parser *list,
				   const char *module)
{
	struct config_module_parser *l;

	if (strcmp(list->module_name, module) == 0)
		return TRUE;
	if (list->root == &master_service_setting_parser_info) {
		/* everyone wants master service settings */
		return TRUE;
	}

	for (l = config_module_parsers; l->module_name != NULL; l++) {
		if (strcmp(l->module_name, module) != 0)
			continue;

		/* see if we can find a way to get from the original parser
		   to this parser */
		if (parsers_are_connected(l->root, list->root))
			return TRUE;
	}
	return FALSE;
}

static void settings_export(struct settings_export_context *ctx,
			    const struct setting_parser_info *info,
			    const void *set, const void *change_set)
{
	const struct setting_define *def;
	const void *value, *default_value, *change_value;
	void *const *children = NULL, *const *change_children = NULL;
	unsigned int i, count, count2, prefix_len;
	const char *str;
	char *key;
	bool dump, dump_default = FALSE;

	for (def = info->defines; def->key != NULL; def++) {
		value = CONST_PTR_OFFSET(set, def->offset);
		default_value = info->defaults == NULL ? NULL :
			CONST_PTR_OFFSET(info->defaults, def->offset);
		change_value = CONST_PTR_OFFSET(change_set, def->offset);
		switch (ctx->scope) {
		case CONFIG_DUMP_SCOPE_ALL:
			dump_default = TRUE;
			break;
		case CONFIG_DUMP_SCOPE_SET:
			dump_default = *((const char *)change_value) != 0;
			break;
		case CONFIG_DUMP_SCOPE_CHANGED:
			dump_default = FALSE;
			break;
		}

		dump = FALSE;
		count = 0;
		str_truncate(ctx->value, 0);
		switch (def->type) {
		case SET_BOOL: {
			const bool *val = value, *dval = default_value;
			if (dump_default || dval == NULL || *val != *dval) {
				str_append(ctx->value,
					   *val ? "yes" : "no");
			}
			break;
		}
		case SET_UINT: {
			const unsigned int *val = value, *dval = default_value;
			if (dump_default || dval == NULL || *val != *dval)
				str_printfa(ctx->value, "%u", *val);
			break;
		}
		case SET_STR_VARS: {
			const char *const *val = value, *sval;
			const char *const *_dval = default_value;
			const char *dval = _dval == NULL ? NULL : *_dval;

			i_assert(*val == NULL ||
				 **val == SETTING_STRVAR_UNEXPANDED[0]);

			sval = *val == NULL ? NULL : (*val + 1);
			if ((dump_default || null_strcmp(sval, dval) != 0) &&
			    sval != NULL) {
				str_append(ctx->value, sval);
				dump = TRUE;
			}
			break;
		}
		case SET_STR: {
			const char *const *val = value;
			const char *const *_dval = default_value;
			const char *dval = _dval == NULL ? NULL : *_dval;

			if ((dump_default || null_strcmp(*val, dval) != 0) &&
			    *val != NULL) {
				str_append(ctx->value, *val);
				dump = TRUE;
			}
			break;
		}
		case SET_ENUM: {
			const char *const *val = value;
			const char *const *_dval = default_value;
			const char *dval = _dval == NULL ? NULL : *_dval;
			unsigned int len = strlen(*val);

			if (dump_default || strncmp(*val, dval, len) != 0 ||
			    ((*val)[len] != ':' && (*val)[len] != '\0'))
				str_append(ctx->value, *val);
			break;
		}
		case SET_DEFLIST: {
			const ARRAY_TYPE(void_array) *val = value;
			const ARRAY_TYPE(void_array) *change_val = change_value;

			if (!array_is_created(val))
				break;

			children = array_get(val, &count);
			for (i = 0; i < count; i++) {
				if (i > 0)
					str_append_c(ctx->value, ' ');
				str_printfa(ctx->value, "%u", i);
			}
			change_children = array_get(change_val, &count2);
			i_assert(count == count2);
			break;
		}
		case SET_STRLIST: {
			const ARRAY_TYPE(const_string) *val = value;
			const char *const *strings;

			if (!array_is_created(val))
				break;

			key = p_strconcat(ctx->pool, str_c(ctx->prefix),
					  def->key, NULL);

			if (hash_table_lookup(ctx->keys, key) != NULL) {
				/* already added all of these */
				break;
			}
			hash_table_insert(ctx->keys, key, key);
			ctx->callback(key, "0", TRUE, ctx->context);

			strings = array_get(val, &count);
			i_assert(count % 2 == 0);
			for (i = 0; i < count; i += 2) {
				str = p_strdup_printf(ctx->pool, "%s%s%c0%c%s",
						      str_c(ctx->prefix),
						      def->key,
						      SETTINGS_SEPARATOR,
						      SETTINGS_SEPARATOR,
						      strings[i]);
				ctx->callback(str, strings[i+1], FALSE,
					      ctx->context);
			}
			count = 0;
			break;
		}
		}
		if (str_len(ctx->value) > 0 || dump) {
			key = p_strconcat(ctx->pool, str_c(ctx->prefix),
					  def->key, NULL);
			if (hash_table_lookup(ctx->keys, key) == NULL) {
				ctx->callback(key, str_c(ctx->value),
					      def->type == SET_DEFLIST,
					      ctx->context);
				hash_table_insert(ctx->keys, key, key);
			}
		}

		prefix_len = str_len(ctx->prefix);
		for (i = 0; i < count; i++) {
			str_append(ctx->prefix, def->key);
			str_append_c(ctx->prefix, SETTINGS_SEPARATOR);
			str_printfa(ctx->prefix, "%u", i);
			str_append_c(ctx->prefix, SETTINGS_SEPARATOR);
			settings_export(ctx, def->list_info, children[i],
					change_children[i]);

			str_truncate(ctx->prefix, prefix_len);
		}
	}
}

int config_request_handle(const struct config_filter *filter,
			  const char *module, enum config_dump_scope scope,
			  bool check_settings,
			  config_request_callback_t *callback, void *context)
{
	const struct config_module_parser *l;
	struct settings_export_context ctx;
	const char *error;
	int ret = 0;

	memset(&ctx, 0, sizeof(ctx));
	ctx.pool = pool_alloconly_create("config request", 10240);

	if (config_filter_get_parsers(config_filter, ctx.pool, filter,
				      &l, &error) < 0) {
		i_error("%s", error);
		pool_unref(&ctx.pool);
		return -1;
	}

	ctx.callback = callback;
	ctx.context = context;
	ctx.scope = scope;
	ctx.value = t_str_new(256);
	ctx.prefix = t_str_new(64);
	ctx.keys = hash_table_create(default_pool, ctx.pool, 0,
				     str_hash, (hash_cmp_callback_t *)strcmp);

	for (; l->module_name != NULL; l++) {
		if (*module != '\0' &&
		    !config_module_parser_is_in_service(l, module))
			continue;

		settings_export(&ctx, l->root, settings_parser_get(l->parser),
				settings_parser_get_changes(l->parser));

		if (check_settings) {
			if (!settings_parser_check(l->parser, ctx.pool,
						   &error)) {
				i_error("%s", error);
				ret = -1;
				break;
			}
		}
	}
	hash_table_destroy(&ctx.keys);
	pool_unref(&ctx.pool);
	return ret;
}
