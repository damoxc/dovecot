/* Copyright (c) 2005-2009 Dovecot authors, see the included COPYING file */

#include "lib.h"
#include "array.h"
#include "istream.h"
#include "write-full.h"
#include "str.h"
#include "settings-parser.h"
#include "master-service-private.h"
#include "master-service-settings.h"

#include <stddef.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/stat.h>

#define DOVECOT_CONFIG_BIN_PATH BINDIR"/doveconf"

#define CONFIG_HANDSHAKE "VERSION\t1\t0\n"

#undef DEF
#define DEF(type, name) \
	{ type, #name, offsetof(struct master_service_settings, name), NULL }

static struct setting_define master_service_setting_defines[] = {
	DEF(SET_STR, log_path),
	DEF(SET_STR, info_log_path),
	DEF(SET_STR, log_timestamp),
	DEF(SET_STR, syslog_facility),
	DEF(SET_BOOL, version_ignore),

	SETTING_DEFINE_LIST_END
};

static struct master_service_settings master_service_default_settings = {
	MEMBER(log_path) "",
	MEMBER(info_log_path) "",
	MEMBER(log_timestamp) DEFAULT_FAILURE_STAMP_FORMAT,
	MEMBER(syslog_facility) "mail",
	MEMBER(version_ignore) FALSE
};

struct setting_parser_info master_service_setting_parser_info = {
	MEMBER(defines) master_service_setting_defines,
	MEMBER(defaults) &master_service_default_settings,

	MEMBER(parent) NULL,
	MEMBER(dynamic_parsers) NULL,

	MEMBER(parent_offset) (size_t)-1,
	MEMBER(type_offset) (size_t)-1,
	MEMBER(struct_size) sizeof(struct master_service_settings)
};

static void ATTR_NORETURN
master_service_exec_config(struct master_service *service, bool preserve_home)
{
	const char **conf_argv, *path, *const *paths, *binary_path;
	char full_path[PATH_MAX];

	binary_path = service->argv[0];
	if (*service->argv[0] == '/') {
		/* already have the path */
	} else if (strchr(service->argv[0], '/') != NULL) {
		/* relative to current directory */
		if (realpath(service->argv[0], full_path) == NULL)
			i_fatal("realpath(%s) failed: %m", service->argv[0]);
		binary_path = full_path;
	} else if ((path = getenv("PATH")) != NULL) {
		/* we have to find our executable from path */
		paths = t_strsplit(path, ":");
		for (; *paths != NULL; paths++) {
			path = t_strconcat(*paths, "/", binary_path, NULL);
			if (access(path, X_OK) == 0) {
				binary_path = path;
				break;
			}
		}
	}

	if (!service->keep_environment)
		master_service_env_clean(preserve_home);

	/* @UNSAFE */
	conf_argv = t_new(const char *, 6 + (service->argc + 1) + 1);
	conf_argv[0] = DOVECOT_CONFIG_BIN_PATH;
	conf_argv[1] = "-p";
	conf_argv[2] = service->name;
	conf_argv[3] = "-c";
	conf_argv[4] = service->config_path;
	conf_argv[5] = "-e";
	conf_argv[6] = binary_path;
	memcpy(conf_argv+7, service->argv + 1,
	       (service->argc) * sizeof(conf_argv[0]));
	execv(conf_argv[0], (char **)conf_argv);
	i_fatal("execv(%s) failed: %m", conf_argv[0]);
}

static int
master_service_read_config(struct master_service *service,
			   const struct master_service_settings_input *input,
			   const char **error_r)
{
	const char *path;
	struct stat st;
	int fd, ret;

	path = master_service_get_config_path(service);
	fd = net_connect_unix(path);
	if (fd < 0) {
		*error_r = t_strdup_printf("net_connect_unix(%s) failed: %m",
					   path);

		if (stat(path, &st) == 0 && !S_ISFIFO(st.st_mode)) {
			/* it's a file, not a socket */
			master_service_exec_config(service,
						   input->preserve_home);
		}
		return -1;
	}
	net_set_nonblock(fd, FALSE);

	T_BEGIN {
		string_t *str;

		str = t_str_new(128);
		str_append(str, CONFIG_HANDSHAKE);
		str_printfa(str, "REQ\tservice=%s", service->name);
		if (input->username != NULL)
			str_printfa(str, "\tuser=%s", input->username);
		if (input->local_ip.family != 0) {
			str_printfa(str, "\tlip=%s",
				    net_ip2addr(&input->local_ip));
		}
		if (input->remote_ip.family != 0) {
			str_printfa(str, "\trip=%s",
				    net_ip2addr(&input->remote_ip));
		}
		str_append_c(str, '\n');
		ret = write_full(fd, str_data(str), str_len(str));
	} T_END;
	if (ret < 0) {
		*error_r = t_strdup_printf("write_full(%s) failed: %m", path);
		return -1;
	}
	return fd;
}

static int
master_service_apply_config_overrides(struct master_service *service,
				      struct setting_parser_context *parser,
				      const char **error_r)
{
	const char *const *overrides;
	unsigned int i, count;

	overrides = array_get(&service->config_overrides, &count);
	for (i = 0; i < count; i++) {
		if (settings_parse_line(parser, overrides[i]) < 0) {
			*error_r = t_strdup_printf(
				"Invalid -o parameter %s: %s", overrides[i],
				settings_parser_get_error(parser));
			return -1;
		}
		settings_parse_set_key_expandeded(parser, service->set_pool,
						  t_strcut(overrides[i], '='));
	}
	return 0;
}

int master_service_settings_read(struct master_service *service,
				 const struct master_service_settings_input *input,
				 const char **error_r)
{
	ARRAY_DEFINE(all_roots, const struct setting_parser_info *);
	const struct setting_parser_info *tmp_root;
	struct setting_parser_context *parser;
	struct istream *istream;
	const char *error, *env, *const *keys;
	void **sets;
	unsigned int i;
	int ret, fd = -1;

	if (getenv("DOVECONF_ENV") == NULL) {
		fd = master_service_read_config(service, input, error_r);
		if (fd == -1)
			return -1;
	}

	if (service->set_pool != NULL)
		p_clear(service->set_pool);
	else {
		service->set_pool =
			pool_alloconly_create("master service settings", 4096);
	}

	if (input->dyn_parsers != NULL) {
		settings_parser_info_update(service->set_pool,
					    input->dyn_parsers);
	}

	p_array_init(&all_roots, service->set_pool, 8);
	tmp_root = &master_service_setting_parser_info;
	array_append(&all_roots, &tmp_root, 1);
	if (input->roots != NULL) {
		for (i = 0; input->roots[i] != NULL; i++)
			array_append(&all_roots, &input->roots[i], 1);
	}

	parser = settings_parser_init_list(service->set_pool,
			array_idx(&all_roots, 0), array_count(&all_roots),
			SETTINGS_PARSER_FLAG_IGNORE_UNKNOWN_KEYS);

	if (fd != -1) {
		istream = i_stream_create_fd(fd, (size_t)-1, FALSE);
		ret = settings_parse_stream_read(parser, istream);
		i_stream_unref(&istream);
		i_assert(ret <= 0);
		if (ret < 0) {
			*error_r = settings_parser_get_error(parser);
			return -1;
		}
	}
	/* let environment override settings. especially useful for the
	   settings from userdb. */
	if (settings_parse_environ(parser) < 0) {
		*error_r = settings_parser_get_error(parser);
		return -1;
	}
	env = getenv("VARS_EXPANDED");
	if (env != NULL) T_BEGIN {
		keys = t_strsplit(env, " ");
		settings_parse_set_keys_expandeded(parser, service->set_pool,
						   keys);
	} T_END;

	if (array_is_created(&service->config_overrides)) {
		if (master_service_apply_config_overrides(service, parser,
							  error_r) < 0)
			return -1;
	}

	if (!settings_parser_check(parser, service->set_pool, &error)) {
		*error_r = t_strdup_printf("Invalid settings: %s", error);
		return -1;
	}

	sets = settings_parser_get_list(parser);
	service->set = sets[0];
	service->set_parser = parser;

	if (service->set->version_ignore &&
	    (service->flags & MASTER_SERVICE_FLAG_STANDALONE) != 0) {
		/* running standalone. we want to ignore plugin versions. */
		service->version_string = NULL;
	}

	/* if we change any settings afterwards, they're in expanded form.
	   especially all settings from userdb are already expanded. */
	settings_parse_set_expanded(service->set_parser, TRUE);
	return 0;
}

int master_service_settings_read_simple(struct master_service *service,
					const struct setting_parser_info **roots,
					const char **error_r)
{
	struct master_service_settings_input input;

	memset(&input, 0, sizeof(input));
	input.roots = roots;
	return master_service_settings_read(service, &input, error_r);
}

const struct master_service_settings *
master_service_settings_get(struct master_service *service)
{
	void **sets;

	sets = settings_parser_get_list(service->set_parser);
	return sets[0];
}

void **master_service_settings_get_others(struct master_service *service)
{
	return settings_parser_get_list(service->set_parser) + 1;
}

int master_service_set(struct master_service *service, const char *line)
{
	return settings_parse_line(service->set_parser, line);
}
