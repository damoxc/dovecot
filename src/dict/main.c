/* Copyright (c) 2005-2009 Dovecot authors, see the included COPYING file */

#include "lib.h"
#include "restrict-access.h"
#include "randgen.h"
#include "env-util.h"
#include "module-dir.h"
#include "master-service.h"
#include "master-service-settings.h"
#include "sql-api.h"
#include "dict.h"
#include "dict-client.h"
#include "dict-connection.h"
#include "dict-settings.h"

#include <stdlib.h>
#include <unistd.h>

static struct module *modules;

static void client_connected(const struct master_service_connection *conn)
{
	dict_connection_create(conn->fd);
}

static void main_preinit(void)
{
	/* Maybe needed. Have to open /dev/urandom before possible
	   chrooting. */
	random_init();

	/* Load built-in SQL drivers (if any) */
	sql_drivers_init();
	sql_drivers_register_all();

	restrict_access_by_env(NULL, FALSE);
	restrict_access_allow_coredumps(TRUE);
}

static void main_init(void)
{
	void **sets;

	sets = master_service_settings_get_others(master_service);
	dict_settings = sets[0];

	if (*dict_settings->dict_db_config != '\0') {
		/* for berkeley db library */
		env_put(t_strconcat("DB_CONFIG=", dict_settings->dict_db_config,
				    NULL));
	}

	modules = module_dir_load(DICT_MODULE_DIR, NULL, TRUE,
			master_service_get_version_string(master_service));
	module_dir_init(modules);

	/* Register only after loading modules. They may contain SQL drivers,
	   which we'll need to register. */
	dict_drivers_register_all();
}

static void main_deinit(void)
{
	dict_connections_destroy_all();
	module_dir_unload(&modules);

	dict_drivers_unregister_all();

	sql_drivers_deinit();
	random_deinit();
}

int main(int argc, char *argv[])
{
	const struct setting_parser_info *set_roots[] = {
		&dict_setting_parser_info,
		NULL
	};
	const char *error;
	int c;

	master_service = master_service_init("dict", 0, argc, argv);
	while ((c = getopt(argc, argv, master_service_getopt_string())) > 0) {
		if (!master_service_parse_option(master_service, c, optarg))
			exit(FATAL_DEFAULT);
	}

	if (master_service_settings_read_simple(master_service, set_roots,
						&error) < 0)
		i_fatal("Error reading configuration: %s", error);

	master_service_init_log(master_service, "dict: ");
	main_preinit();
	master_service_init_finish(master_service);

	main_init();
	master_service_run(master_service, client_connected);

	main_deinit();
	master_service_deinit(&master_service);
        return 0;
}
