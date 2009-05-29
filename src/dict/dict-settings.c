/* Copyright (c) 2009 Dovecot authors, see the included COPYING file */

#include "lib.h"
#include "settings-parser.h"
#include "dict-settings.h"

#undef DEF
#define DEF(type, name) \
	{ type, #name, offsetof(struct dict_settings, name), NULL }

static struct setting_define dict_setting_defines[] = {
	DEF(SET_STR, dict_db_config),
	{ SET_STRLIST, "dict", offsetof(struct dict_settings, dicts), NULL },

	SETTING_DEFINE_LIST_END
};

struct dict_settings dict_default_settings = {
	MEMBER(dict_db_config) "",
	MEMBER(dicts) ARRAY_INIT
};

struct setting_parser_info dict_setting_parser_info = {
	MEMBER(defines) dict_setting_defines,
	MEMBER(defaults) &dict_default_settings,

	MEMBER(parent) NULL,
	MEMBER(dynamic_parsers) NULL,

	MEMBER(parent_offset) (size_t)-1,
	MEMBER(type_offset) (size_t)-1,
	MEMBER(struct_size) sizeof(struct dict_settings)
};

struct dict_settings *dict_settings;
