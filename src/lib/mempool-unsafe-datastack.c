/* Copyright (c) 2002-2011 Dovecot authors, see the included COPYING file */

#include "lib.h"
#include "mempool.h"

#include <stdlib.h>

static const char *pool_unsafe_data_stack_get_name(pool_t pool);
static void pool_unsafe_data_stack_ref(pool_t pool);
static void pool_unsafe_data_stack_unref(pool_t *pool);
static void *pool_unsafe_data_stack_malloc(pool_t pool, size_t size);
static void pool_unsafe_data_stack_free(pool_t pool, void *mem);
static void *pool_unsafe_data_stack_realloc(pool_t pool, void *mem,
					    size_t old_size, size_t new_size);
static void pool_unsafe_data_stack_clear(pool_t pool);
static size_t pool_unsafe_data_stack_get_max_easy_alloc_size(pool_t pool);

static struct pool_vfuncs static_unsafe_data_stack_pool_vfuncs = {
	pool_unsafe_data_stack_get_name,

	pool_unsafe_data_stack_ref,
	pool_unsafe_data_stack_unref,

	pool_unsafe_data_stack_malloc,
	pool_unsafe_data_stack_free,

	pool_unsafe_data_stack_realloc,

	pool_unsafe_data_stack_clear,
	pool_unsafe_data_stack_get_max_easy_alloc_size
};

static struct pool static_unsafe_data_stack_pool = {
	.v = &static_unsafe_data_stack_pool_vfuncs,

	.alloconly_pool = TRUE,
	.datastack_pool = TRUE
};

pool_t unsafe_data_stack_pool = &static_unsafe_data_stack_pool;

static const char *pool_unsafe_data_stack_get_name(pool_t pool ATTR_UNUSED)
{
	return "unsafe data stack";
}

static void pool_unsafe_data_stack_ref(pool_t pool ATTR_UNUSED)
{
}

static void pool_unsafe_data_stack_unref(pool_t *pool ATTR_UNUSED)
{
}

static void *pool_unsafe_data_stack_malloc(pool_t pool ATTR_UNUSED,
					   size_t size)
{
	if (unlikely(size == 0 || size > SSIZE_T_MAX))
		i_panic("Trying to allocate %"PRIuSIZE_T" bytes", size);

	return t_malloc0(size);
}

static void pool_unsafe_data_stack_free(pool_t pool ATTR_UNUSED,
					void *mem ATTR_UNUSED)
{
}

static void *pool_unsafe_data_stack_realloc(pool_t pool ATTR_UNUSED,
					    void *mem,
					    size_t old_size, size_t new_size)
{
	void *new_mem;

	/* @UNSAFE */
	if (new_size == 0 || new_size > SSIZE_T_MAX)
		i_panic("Trying to allocate %"PRIuSIZE_T" bytes", new_size);

	if (mem == NULL)
		return pool_unsafe_data_stack_malloc(pool, new_size);

	if (old_size >= new_size)
		return mem;

	if (!t_try_realloc(mem, new_size)) {
		new_mem = t_malloc(new_size);
		memcpy(new_mem, mem, old_size);
		mem = new_mem;
	}

	memset((char *) mem + old_size, 0, new_size - old_size);
	return mem;
}

static void pool_unsafe_data_stack_clear(pool_t pool ATTR_UNUSED)
{
}

static size_t
pool_unsafe_data_stack_get_max_easy_alloc_size(pool_t pool ATTR_UNUSED)
{
	return t_get_bytes_available();
}
