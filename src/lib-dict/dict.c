/* Copyright (c) 2005-2008 Dovecot authors, see the included COPYING file */

#include "lib.h"
#include "array.h"
#include "dict-sql.h"
#include "dict-private.h"

static ARRAY_DEFINE(dict_drivers, struct dict *);

static struct dict *dict_driver_lookup(const char *name)
{
	struct dict *const *dicts;
	unsigned int i, count;

	dicts = array_get(&dict_drivers, &count);
	for (i = 0; i < count; i++) {
		if (strcmp(dicts[i]->name, name) == 0)
			return dicts[i];
	}
	return NULL;
}

void dict_driver_register(struct dict *driver)
{
	if (!array_is_created(&dict_drivers))
		i_array_init(&dict_drivers, 8);

	if (dict_driver_lookup(driver->name) != NULL) {
		i_fatal("dict_driver_register(%s): Already registered",
			driver->name);
	}
	array_append(&dict_drivers, &driver, 1);
}

void dict_driver_unregister(struct dict *driver)
{
	struct dict *const *dicts;
	unsigned int i, count;

	dicts = array_get(&dict_drivers, &count);
	for (i = 0; i < count; i++) {
		if (dicts[i] == driver) {
			array_delete(&dict_drivers, i, 1);
			break;
		}
	}

	i_assert(i < count);

	if (array_count(&dict_drivers) == 0)
		array_free(&dict_drivers);
}

struct dict *dict_init(const char *uri, enum dict_data_type value_type,
		       const char *username)
{
	struct dict *dict;
	const char *p, *name;

	i_assert(username != NULL);

	p = strchr(uri, ':');
	if (p == NULL) {
		i_error("Dictionary URI is missing ':': %s", uri);
		return NULL;
	}

	T_BEGIN {
		name = t_strdup_until(uri, p);
		dict = dict_driver_lookup(name);
		if (dict == NULL)
			i_error("Unknown dict module: %s", name);
	} T_END;

	return dict == NULL ? NULL :
		dict->v.init(dict, p+1, value_type, username);
}

void dict_deinit(struct dict **_dict)
{
	struct dict *dict = *_dict;

	*_dict = NULL;
	dict->v.deinit(dict);
}

int dict_lookup(struct dict *dict, pool_t pool, const char *key,
		const char **value_r)
{
	return dict->v.lookup(dict, pool, key, value_r);
}

struct dict_iterate_context *
dict_iterate_init(struct dict *dict, const char *path, 
		  enum dict_iterate_flags flags)
{
	return dict->v.iterate_init(dict, path, flags);
}

int dict_iterate(struct dict_iterate_context *ctx,
		 const char **key_r, const char **value_r)
{
	return ctx->dict->v.iterate(ctx, key_r, value_r);
}

void dict_iterate_deinit(struct dict_iterate_context **_ctx)
{
	struct dict_iterate_context *ctx = *_ctx;

	*_ctx = NULL;
	ctx->dict->v.iterate_deinit(ctx);
}

struct dict_transaction_context *dict_transaction_begin(struct dict *dict)
{
	return dict->v.transaction_init(dict);
}

int dict_transaction_commit(struct dict_transaction_context **_ctx)
{
	struct dict_transaction_context *ctx = *_ctx;

	*_ctx = NULL;
	return ctx->dict->v.transaction_commit(ctx);
}

void dict_transaction_rollback(struct dict_transaction_context **_ctx)
{
	struct dict_transaction_context *ctx = *_ctx;

	*_ctx = NULL;
	ctx->dict->v.transaction_rollback(ctx);
}

void dict_set(struct dict_transaction_context *ctx,
	      const char *key, const char *value)
{
	ctx->dict->v.set(ctx, key, value);
	ctx->changed = TRUE;
}

void dict_unset(struct dict_transaction_context *ctx,
		const char *key)
{
	ctx->dict->v.unset(ctx, key);
	ctx->changed = TRUE;
}

void dict_atomic_inc(struct dict_transaction_context *ctx,
		     const char *key, long long diff)
{
	if (diff != 0) {
		ctx->dict->v.atomic_inc(ctx, key, diff);
		ctx->changed = TRUE;
	}
}
