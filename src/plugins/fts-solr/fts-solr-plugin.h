#ifndef FTS_SOLR_PLUGIN_H
#define FTS_SOLR_PLUGIN_H

#include "module-context.h"
#include "fts-api-private.h"

#define FTS_SOLR_USER_CONTEXT(obj) \
	MODULE_CONTEXT(obj, fts_solr_user_module)

struct fts_solr_settings {
	const char *url, *default_ns_prefix;
	bool debug;
	bool substring_search;
};

struct fts_solr_user {
	union mail_user_module_context module_ctx;
	struct fts_solr_settings set;
};

extern const char *fts_solr_plugin_dependencies[];
extern struct fts_backend fts_backend_solr;
extern MODULE_CONTEXT_DEFINE(fts_solr_user_module, &mail_user_module_register);

void fts_solr_plugin_init(struct module *module);
void fts_solr_plugin_deinit(void);

#endif
