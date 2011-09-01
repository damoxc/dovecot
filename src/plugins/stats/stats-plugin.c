/* Copyright (c) 2011 Dovecot authors, see the included COPYING file */

#include "lib.h"
#include "ioloop.h"
#include "array.h"
#include "hostpid.h"
#include "llist.h"
#include "str.h"
#include "time-util.h"
#include "stats-connection.h"
#include "stats-plugin.h"

#include <sys/time.h>
#include <sys/resource.h>

#define STATS_CONTEXT(obj) \
	MODULE_CONTEXT(obj, stats_storage_module)

/* Refresh session every 10 seconds, if anything has changed */
#define SESSION_STATS_REFRESH_SECS 10
/* If session isn't refreshed every 15 minutes, it's dropped.
   Must be smaller than MAIL_SESSION_IDLE_TIMEOUT_MSECS in stats server */
#define SESSION_STATS_FORCE_REFRESH_SECS (5*60)
#define MAIL_STATS_SOCKET_NAME "stats-mail"

#define USECS_PER_SEC 1000000

struct stats_transaction_context {
	union mailbox_transaction_module_context module_ctx;

	struct stats_transaction_context *prev, *next;
	struct mailbox_transaction_context *trans;

	struct mailbox_transaction_stats prev_stats;
};

struct stats_mailbox {
	union mailbox_module_context module_ctx;
};

const char *stats_plugin_version = DOVECOT_VERSION;

struct stats_user_module stats_user_module =
	MODULE_CONTEXT_INIT(&mail_user_module_register);
static MODULE_CONTEXT_DEFINE_INIT(stats_storage_module,
				  &mail_storage_module_register);

static struct stats_connection *global_stats_conn = NULL;
static struct mail_user *stats_global_user = NULL;
static unsigned int stats_user_count = 0;

static int
process_io_buffer_parse(const char *buf, uint64_t *read_bytes_r,
			uint64_t *write_bytes_r)
{
	const char *const *tmp;
	uint64_t cbytes;

	tmp = t_strsplit(buf, "\n");
	for (; *tmp != NULL; tmp++) {
		if (strncmp(*tmp, "read_bytes: ", 12) == 0) {
			if (str_to_uint64(*tmp + 12, read_bytes_r) < 0)
				return -1;
		} else if (strncmp(*tmp, "write_bytes: ", 13) == 0) {
			if (str_to_uint64(*tmp + 13, write_bytes_r) < 0)
				return -1;
		} else if (strncmp(*tmp, "cancelled_write_bytes: ", 23) == 0) {
			if (str_to_uint64(*tmp + 23, &cbytes) < 0)
				return -1;
			/* it's not 100% correct to simply subtract the
			   cancelled bytes from write bytes, but it's close
			   enough. */
			if (*write_bytes_r < cbytes)
				*write_bytes_r = 0;
			else
				*write_bytes_r -= cbytes;
		}
	}
	return 0;
}

static void
process_read_io_stats(uint64_t *read_bytes_r, uint64_t *write_bytes_r)
{
	static bool io_disabled = FALSE;
	char path[100], buf[1024];
	int fd, ret;

	*read_bytes_r = *write_bytes_r = 0;

	if (io_disabled)
		return;

	i_snprintf(path, sizeof(path), "/proc/%s/io", my_pid);
	fd = open(path, O_RDONLY);
	if (fd == -1) {
		if (errno != ENOENT)
			i_error("open(%s) failed: %m", path);
		io_disabled = TRUE;
		return;
	}
	ret = read(fd, buf, sizeof(buf));
	if (ret <= 0) {
		if (ret == -1)
			i_error("read(%s) failed: %m", path);
		else
			i_error("read(%s) returned EOF", path);
	} else if (ret == sizeof(buf)) {
		/* just shouldn't happen.. */
		i_error("%s is larger than expected", path);
		io_disabled = TRUE;
	} else {
		buf[ret] = '\0';
		T_BEGIN {
			if (process_io_buffer_parse(buf, read_bytes_r,
						    write_bytes_r) < 0) {
				i_error("Invalid input in file %s", path);
				io_disabled = TRUE;
			}
		} T_END;
	}
	if (close(fd) < 0)
		i_error("close(%s) failed: %m", path);
}

static void trans_stats_dec(struct mailbox_transaction_stats *dest,
			    const struct mailbox_transaction_stats *src)
{
	dest->open_lookup_count -= src->open_lookup_count;
	dest->stat_lookup_count -= src->stat_lookup_count;
	dest->fstat_lookup_count -= src->fstat_lookup_count;
	dest->files_read_count -= src->files_read_count;
	dest->files_read_bytes -= src->files_read_bytes;
	dest->cache_hit_count -= src->cache_hit_count;
}

static void trans_stats_add(struct mailbox_transaction_stats *dest,
			    const struct mailbox_transaction_stats *src)
{
	dest->open_lookup_count += src->open_lookup_count;
	dest->stat_lookup_count += src->stat_lookup_count;
	dest->fstat_lookup_count += src->fstat_lookup_count;
	dest->files_read_count += src->files_read_count;
	dest->files_read_bytes += src->files_read_bytes;
	dest->cache_hit_count += src->cache_hit_count;
}

static void user_trans_stats_get(struct stats_user *suser,
				 struct mailbox_transaction_stats *dest_r)
{
	struct stats_transaction_context *strans;

	memset(dest_r, 0, sizeof(*dest_r));
	strans = suser->transactions;
	for (; strans != NULL; strans = strans->next)
		trans_stats_add(dest_r, &strans->trans->stats);
}

void mail_stats_get(struct stats_user *suser, struct mail_stats *stats_r)
{
	struct rusage usage;

	memset(stats_r, 0, sizeof(*stats_r));
	/* cputime */
	if (getrusage(RUSAGE_SELF, &usage) < 0)
		memset(&usage, 0, sizeof(usage));
	stats_r->user_cpu = usage.ru_utime;
	stats_r->sys_cpu = usage.ru_stime;
	stats_r->min_faults = usage.ru_minflt;
	stats_r->maj_faults = usage.ru_majflt;
	stats_r->vol_cs = usage.ru_nvcsw;
	stats_r->invol_cs = usage.ru_nivcsw;
	/* disk io */
	process_read_io_stats(&stats_r->disk_input, &stats_r->disk_output);
	user_trans_stats_get(suser, &stats_r->trans_stats);
}

static struct mailbox_transaction_context *
stats_transaction_begin(struct mailbox *box,
			enum mailbox_transaction_flags flags)
{
	struct stats_user *suser = STATS_USER_CONTEXT(box->storage->user);
	struct stats_mailbox *sbox = STATS_CONTEXT(box);
	struct mailbox_transaction_context *trans;
	struct stats_transaction_context *strans;

	trans = sbox->module_ctx.super.transaction_begin(box, flags);
	trans->stats_track = TRUE;

	strans = i_new(struct stats_transaction_context, 1);
	strans->trans = trans;
	DLLIST_PREPEND(&suser->transactions, strans);

	MODULE_CONTEXT_SET(trans, stats_storage_module, strans);
	return trans;
}

static void stats_transaction_free(struct stats_user *suser,
				   struct stats_transaction_context *strans)
{
	DLLIST_REMOVE(&suser->transactions, strans);

	trans_stats_add(&suser->session_stats.trans_stats,
			&strans->trans->stats);
}

static int
stats_transaction_commit(struct mailbox_transaction_context *ctx,
			 struct mail_transaction_commit_changes *changes_r)
{
	struct stats_transaction_context *strans = STATS_CONTEXT(ctx);
	struct stats_mailbox *sbox = STATS_CONTEXT(ctx->box);
	struct stats_user *suser = STATS_USER_CONTEXT(ctx->box->storage->user);

	stats_transaction_free(suser, strans);
	return sbox->module_ctx.super.transaction_commit(ctx, changes_r);
}

static void
stats_transaction_rollback(struct mailbox_transaction_context *ctx)
{
	struct stats_transaction_context *strans = STATS_CONTEXT(ctx);
	struct stats_mailbox *sbox = STATS_CONTEXT(ctx->box);
	struct stats_user *suser = STATS_USER_CONTEXT(ctx->box->storage->user);

	stats_transaction_free(suser, strans);
	sbox->module_ctx.super.transaction_rollback(ctx);
}

static void stats_mailbox_allocated(struct mailbox *box)
{
	struct mailbox_vfuncs *v = box->vlast;
	struct stats_mailbox *sbox;
	struct stats_user *suser = STATS_USER_CONTEXT(box->storage->user);

	if (suser == NULL)
		return;

	sbox = p_new(box->pool, struct stats_mailbox, 1);
	sbox->module_ctx.super = *v;
	box->vlast = &sbox->module_ctx.super;

	v->transaction_begin = stats_transaction_begin;
	v->transaction_commit = stats_transaction_commit;
	v->transaction_rollback = stats_transaction_rollback;
	MODULE_CONTEXT_SET(box, stats_storage_module, sbox);
}

static void stats_io_activate(void *context)
{
	struct mail_user *user = context;
	struct stats_user *suser = STATS_USER_CONTEXT(user);

	if (stats_user_count == 1) {
		/* the first user sets the global user. the second user sets
		   it to NULL. when we get back to one user we'll need to set
		   the global user again somewhere. do it here. */
		stats_global_user = user;
	} else {
		i_assert(stats_global_user == NULL);

		mail_stats_get(suser, &suser->pre_io_stats);
	}
}

static void timeval_add_diff(struct timeval *dest,
			     const struct timeval *newsrc,
			     const struct timeval *oldsrc)
{
	long long usecs;

	usecs = timeval_diff_usecs(newsrc, oldsrc);
	dest->tv_sec += usecs / USECS_PER_SEC;
	dest->tv_usec += usecs % USECS_PER_SEC;
	if (dest->tv_usec > USECS_PER_SEC) {
		dest->tv_usec -= USECS_PER_SEC;
		dest->tv_sec++;
	}
}

void mail_stats_add_diff(struct mail_stats *dest,
			 const struct mail_stats *old_stats,
			 const struct mail_stats *new_stats)
{
	dest->disk_input += new_stats->disk_input - old_stats->disk_input;
	dest->disk_output += new_stats->disk_output - old_stats->disk_output;
	dest->min_faults += new_stats->min_faults - old_stats->min_faults;
	dest->maj_faults += new_stats->maj_faults - old_stats->maj_faults;
	dest->vol_cs += new_stats->vol_cs - old_stats->vol_cs;
	dest->invol_cs += new_stats->invol_cs - old_stats->invol_cs;

	timeval_add_diff(&dest->user_cpu, &new_stats->user_cpu,
			 &old_stats->user_cpu);
	timeval_add_diff(&dest->sys_cpu, &new_stats->sys_cpu,
			 &old_stats->sys_cpu);
	trans_stats_dec(&dest->trans_stats, &old_stats->trans_stats);
	trans_stats_add(&dest->trans_stats, &new_stats->trans_stats);
}

void mail_stats_export(string_t *str, const struct mail_stats *stats)
{
	const struct mailbox_transaction_stats *tstats = &stats->trans_stats;

	str_printfa(str, "\tucpu=%ld.%ld", (long)stats->user_cpu.tv_sec,
		    (long)stats->user_cpu.tv_usec);
	str_printfa(str, "\tscpu=%ld.%ld", (long)stats->sys_cpu.tv_sec,
		    (long)stats->sys_cpu.tv_usec);
	str_printfa(str, "\tminflt=%u", stats->min_faults);
	str_printfa(str, "\tmajflt=%u", stats->maj_faults);
	str_printfa(str, "\tvolcs=%u", stats->vol_cs);
	str_printfa(str, "\tinvolcs=%u", stats->invol_cs);
	str_printfa(str, "\tdiskin=%llu",
		    (unsigned long long)stats->disk_input);
	str_printfa(str, "\tdiskout=%llu",
		    (unsigned long long)stats->disk_output);
	str_printfa(str, "\tlpath=%lu",
		    tstats->open_lookup_count + tstats->stat_lookup_count);
	str_printfa(str, "\tlattr=%lu",
		    tstats->fstat_lookup_count + tstats->stat_lookup_count);
	str_printfa(str, "\trcount=%lu", tstats->files_read_count);
	str_printfa(str, "\trbytes=%llu", tstats->files_read_bytes);
	str_printfa(str, "\tcache=%lu", tstats->cache_hit_count);
}

static void stats_add_session(struct mail_user *user)
{
	struct stats_user *suser = STATS_USER_CONTEXT(user);
	struct mail_stats new_stats;

	mail_stats_get(suser, &new_stats);
	mail_stats_add_diff(&suser->session_stats, &suser->pre_io_stats,
			    &new_stats);
	suser->pre_io_stats = new_stats;
}

static bool session_stats_need_send(struct stats_user *suser)
{
	if (memcmp(&suser->last_sent_session_stats, &suser->session_stats,
		   sizeof(suser->session_stats)) != 0)
		return TRUE;

	return ioloop_time - suser->last_session_update >=
		SESSION_STATS_FORCE_REFRESH_SECS;
}

static void session_stats_refresh(struct mail_user *user)
{
	struct stats_user *suser = STATS_USER_CONTEXT(user);

	if (session_stats_need_send(suser)) {
		suser->last_session_update = ioloop_time;
		suser->last_sent_session_stats = suser->session_stats;
		stats_connection_send_session(suser->stats_conn, user,
					      &suser->session_stats);
	}
	if (suser->to_stats_timeout != NULL)
		timeout_remove(&suser->to_stats_timeout);
}

static void session_stats_refresh_timeout(struct mail_user *user)
{
	if (stats_global_user != NULL)
		stats_add_session(user);
	session_stats_refresh(user);
}

static void stats_io_deactivate(void *context)
{
	struct mail_user *user = context;
	struct stats_user *suser = STATS_USER_CONTEXT(user);
	unsigned int last_update_secs;

	if (stats_global_user == NULL)
		stats_add_session(user);

	last_update_secs = ioloop_time - suser->last_session_update;
	if (last_update_secs >= SESSION_STATS_REFRESH_SECS) {
		if (stats_global_user != NULL)
			stats_add_session(user);
		session_stats_refresh(user);
	} else if (suser->to_stats_timeout == NULL) {
		suser->to_stats_timeout =
			timeout_add(SESSION_STATS_REFRESH_SECS*1000,
				    session_stats_refresh_timeout, user);
	}
}

static void stats_user_deinit(struct mail_user *user)
{
	struct stats_user *suser = STATS_USER_CONTEXT(user);
	struct stats_connection *stats_conn = suser->stats_conn;

	i_assert(stats_user_count > 0);
	if (--stats_user_count == 0) {
		/* we were updating the session lazily. do one final update. */
		i_assert(stats_global_user == user);
		stats_add_session(user);
		stats_global_user = NULL;
	} else {
		i_assert(stats_global_user == NULL);
	}

	io_loop_context_remove_callbacks(suser->ioloop_ctx,
					 stats_io_activate,
					 stats_io_deactivate, user);
	/* send final stats before disconnection */
	session_stats_refresh(user);
	stats_connection_disconnect(stats_conn, user);

	if (suser->to_stats_timeout != NULL)
		timeout_remove(&suser->to_stats_timeout);
	suser->module_ctx.super.deinit(user);

	stats_connection_unref(&stats_conn);
}

static void stats_user_created(struct mail_user *user)
{
	struct ioloop_context *ioloop_ctx =
		io_loop_get_current_context(current_ioloop);
	struct stats_user *suser;
	struct mail_user_vfuncs *v = user->vlast;
	const char *path;

	if (ioloop_ctx == NULL) {
		/* we're probably running some test program, or at least
		   mail-storage-service wasn't used to create this user.
		   disable stats tracking. */
		return;
	}
	if (user->autocreated) {
		/* lda / shared user. we're not tracking this one. */
		return;
	}

	if (global_stats_conn == NULL) {
		path = t_strconcat(user->set->base_dir,
				   "/"MAIL_STATS_SOCKET_NAME, NULL);
		global_stats_conn = stats_connection_create(path);
	}
	stats_connection_ref(global_stats_conn);

	if (stats_user_count == 0) {
		/* first user connection */
		stats_global_user = user;
	} else if (stats_user_count == 1) {
		/* second user connection. we'll need to start doing
		   per-io callback tracking now. */
		stats_add_session(stats_global_user);
		stats_global_user = NULL;
	}
	stats_user_count++;

	suser = p_new(user->pool, struct stats_user, 1);
	suser->module_ctx.super = *v;
	user->vlast = &suser->module_ctx.super;
	user->v.deinit = stats_user_deinit;

	suser->stats_conn = global_stats_conn;
	guid_128_generate(suser->session_guid);
	suser->last_session_update = ioloop_time;

	suser->ioloop_ctx = ioloop_ctx;
	io_loop_context_add_callbacks(ioloop_ctx,
				      stats_io_activate,
				      stats_io_deactivate, user);

	MODULE_CONTEXT_SET(user, stats_user_module, suser);
	stats_connection_connect(suser->stats_conn, user);
}

static struct mail_storage_hooks stats_mail_storage_hooks = {
	.mailbox_allocated = stats_mailbox_allocated,
	.mail_user_created = stats_user_created
};

void stats_plugin_init(struct module *module)
{
	mail_storage_hooks_add(module, &stats_mail_storage_hooks);
}

void stats_plugin_deinit(void)
{
	if (global_stats_conn != NULL)
		stats_connection_unref(&global_stats_conn);
	mail_storage_hooks_remove(&stats_mail_storage_hooks);
}
