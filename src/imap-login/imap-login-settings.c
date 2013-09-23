/* Copyright (c) 2005-2013 Dovecot authors, see the included COPYING file */

#include "lib.h"
#include "buffer.h"
#include "settings-parser.h"
#include "service-settings.h"
#include "login-settings.h"
#include "imap-login-settings.h"

#include <stddef.h>

/* <settings checks> */
static struct inet_listener_settings imap_login_inet_listeners_array[] = {
	{ .name = "imap", .address = "", .port = 143 },
	{ .name = "imaps", .address = "", .port = 993, .ssl = TRUE }
};
static struct inet_listener_settings *imap_login_inet_listeners[] = {
	&imap_login_inet_listeners_array[0],
	&imap_login_inet_listeners_array[1]
};
static buffer_t imap_login_inet_listeners_buf = {
	imap_login_inet_listeners, sizeof(imap_login_inet_listeners), { 0, }
};
/* </settings checks> */

struct service_settings imap_login_service_settings = {
	.name = "imap-login",
	.protocol = "imap",
	.type = "login",
	.executable = "imap-login",
	.user = "$default_login_user",
	.group = "",
	.privileged_group = "",
	.extra_groups = "",
	.chroot = "login",

	.drop_priv_before_exec = FALSE,

	.process_min_avail = 0,
	.process_limit = 0,
	.client_limit = 0,
	.service_count = 1,
	.idle_kill = 0,
	.vsz_limit = (uoff_t)-1,

	.unix_listeners = ARRAY_INIT,
	.fifo_listeners = ARRAY_INIT,
	.inet_listeners = { { &imap_login_inet_listeners_buf,
			      sizeof(imap_login_inet_listeners[0]) } }
};

#undef DEF
#define DEF(type, name) \
	{ type, #name, offsetof(struct imap_login_settings, name), NULL }

static const struct setting_define imap_login_setting_defines[] = {
	DEF(SET_STR, imap_capability),
	DEF(SET_STR, imap_id_send),
	DEF(SET_STR, imap_id_log),

	SETTING_DEFINE_LIST_END
};

static const struct imap_login_settings imap_login_default_settings = {
	.imap_capability = "",
	.imap_id_send = "name *",
	.imap_id_log = ""
};

static const struct setting_parser_info *imap_login_setting_dependencies[] = {
	&login_setting_parser_info,
	NULL
};

static const struct setting_parser_info imap_login_setting_parser_info = {
	.module_name = "imap-login",
	.defines = imap_login_setting_defines,
	.defaults = &imap_login_default_settings,

	.type_offset = (size_t)-1,
	.struct_size = sizeof(struct imap_login_settings),

	.parent_offset = (size_t)-1,
	.dependencies = imap_login_setting_dependencies
};

const struct setting_parser_info *imap_login_setting_roots[] = {
	&login_setting_parser_info,
	&imap_login_setting_parser_info,
	NULL
};
