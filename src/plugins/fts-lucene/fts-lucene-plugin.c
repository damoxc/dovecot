/* Copyright (c) 2006-2007 Dovecot authors, see the included COPYING file */

#include "lib.h"
#include "mail-storage-private.h"
#include "fts-lucene-plugin.h"

const char *fts_lucene_plugin_version = PACKAGE_VERSION;

unsigned int fts_lucene_storage_module_id;

void fts_lucene_plugin_init(void)
{
	fts_backend_register(&fts_backend_lucene);
}

void fts_lucene_plugin_deinit(void)
{
	fts_backend_unregister(fts_backend_lucene.name);
}
