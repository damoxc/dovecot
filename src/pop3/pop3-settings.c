/* Copyright (c) 2005-2009 Dovecot authors, see the included COPYING file */

#include "lib.h"
#include "settings-parser.h"
#include "mail-storage-settings.h"
#include "pop3-settings.h"

#include <stddef.h>
#include <stdlib.h>
#include <unistd.h>

#undef DEF
#undef DEFLIST
#define DEF(type, name) \
	{ type, #name, offsetof(struct pop3_settings, name), NULL }
#define DEFLIST(field, name, defines) \
	{ SET_DEFLIST, name, offsetof(struct pop3_settings, field), defines }

static struct setting_define pop3_setting_defines[] = {
	DEF(SET_BOOL, mail_debug),
	DEF(SET_BOOL, shutdown_clients),
	DEF(SET_BOOL, verbose_proctitle),

	DEF(SET_BOOL, pop3_no_flag_updates),
	DEF(SET_BOOL, pop3_enable_last),
	DEF(SET_BOOL, pop3_reuse_xuidl),
	DEF(SET_BOOL, pop3_lock_session),
	DEF(SET_STR, pop3_client_workarounds),
	DEF(SET_STR, pop3_logout_format),

	SETTING_DEFINE_LIST_END
};

static struct pop3_settings pop3_default_settings = {
	MEMBER(mail_debug) FALSE,
	MEMBER(shutdown_clients) FALSE,
	MEMBER(verbose_proctitle) FALSE,

	MEMBER(pop3_no_flag_updates) FALSE,
	MEMBER(pop3_enable_last) FALSE,
	MEMBER(pop3_reuse_xuidl) FALSE,
	MEMBER(pop3_lock_session) FALSE,
	MEMBER(pop3_client_workarounds) "",
	MEMBER(pop3_logout_format) "top=%t/%p, retr=%r/%b, del=%d/%m, size=%s"
};

struct setting_parser_info pop3_setting_parser_info = {
	MEMBER(defines) pop3_setting_defines,
	MEMBER(defaults) &pop3_default_settings,

	MEMBER(parent) NULL,
	MEMBER(dynamic_parsers) NULL,

	MEMBER(parent_offset) (size_t)-1,
	MEMBER(type_offset) (size_t)-1,
	MEMBER(struct_size) sizeof(struct pop3_settings),
	MEMBER(check_func) NULL
};
