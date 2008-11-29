/* Copyright (c) 2006-2008 Dovecot authors, see the included COPYING file */

#include "lib.h"
#include "array.h"
#include "str.h"
#include "strescape.h"
#include "mail-storage-private.h"
#include "mail-namespace.h"
#include "solr-connection.h"
#include "fts-solr-plugin.h"

#include <ctype.h>

#define SOLR_CMDBUF_SIZE (1024*64)
#define SOLR_MAX_ROWS 100000
#define FTS_SOLR_MAX_BOX_INC_PATTERNS 5
#define FTS_SOLR_MAX_BOX_EXC_PATTERNS 5

struct solr_fts_backend {
	struct fts_backend backend;
	char *id_username, *id_namespace;
	struct mail_namespace *default_ns;
};

struct solr_fts_backend_build_context {
	struct fts_backend_build_context ctx;

	struct solr_connection_post *post;
	uint32_t prev_uid, uid_validity;
	string_t *cmd;
	bool headers;
};

struct solr_virtual_uid_map_context {
	struct fts_backend *backend;
	struct mailbox *box;
	string_t *vname;
};

struct fts_backend_solr_get_last_uids_context {
	struct fts_backend *backend;
	pool_t pool;
	ARRAY_TYPE(fts_backend_uid_map) *last_uids;

	struct mailbox *box;
	string_t *vname;
};

static struct solr_connection *solr_conn = NULL;

static void
xml_encode_data(string_t *dest, const unsigned char *data, unsigned int len)
{
	unsigned int i;

	for (i = 0; i < len; i++) {
		switch (data[i]) {
		case '&':
			str_append(dest, "&amp;");
			break;
		case '<':
			str_append(dest, "&lt;");
			break;
		case '>':
			str_append(dest, "&gt;");
			break;
		default:
			str_append_c(dest, data[i]);
			break;
		}
	}
}

static void xml_encode(string_t *dest, const char *str)
{
	xml_encode_data(dest, (const unsigned char *)str, strlen(str));
}

static const char *solr_escape_id_str(const char *str)
{
	string_t *tmp;
	const char *p;

	for (p = str; *p != '\0'; p++) {
		if (*p == '/' || *p == '!')
			break;
	}
	if (*p == '\0')
		return str;

	tmp = t_str_new(64);
	for (p = str; *p != '\0'; p++) {
		switch (*p) {
		case '/':
			str_append(tmp, "!\\");
			break;
		case '!':
			str_append(tmp, "!!");
			break;
		default:
			str_append_c(tmp, *p);
			break;
		}
	}
	return str_c(tmp);
}

static void solr_quote(string_t *dest, const char *str)
{
	str_append_c(dest, '"');
	str_append(dest, str_escape(str));
	str_append_c(dest, '"');
}

static void solr_quote_http(string_t *dest, const char *str)
{
	str_append(dest, "%22");
	solr_connection_http_escape(solr_conn, dest, str);
	str_append(dest, "%22");
}

static struct fts_backend *
fts_backend_solr_init(struct mailbox *box)
{
	const struct fts_solr_settings *set = &fts_solr_settings;
	struct solr_fts_backend *backend;
	struct mail_namespace *ns = box->storage->ns;
	const char *str;

	if (solr_conn == NULL)
		solr_conn = solr_connection_init(set->url, set->debug);

	backend = i_new(struct solr_fts_backend, 1);
	str = fts_solr_settings.default_ns_prefix;
	if (str != NULL) {
		backend->default_ns =
			mail_namespace_find_prefix(ns->user->namespaces, str);
		if (backend->default_ns == NULL) {
			i_fatal("fts_solr: default_ns setting points to "
				"nonexisting namespace");
		}
	} else {
		backend->default_ns =
			mail_namespace_find_inbox(ns->user->namespaces);
	}
	str = solr_escape_id_str(ns->user->username);
	backend->id_username = i_strdup(str);
	if (box->storage->ns != backend->default_ns) {
		str = solr_escape_id_str(ns->prefix);
		backend->id_namespace = i_strdup(str);
	}
	backend->backend = fts_backend_solr;

	if (set->substring_search)
		backend->backend.flags |= FTS_BACKEND_FLAG_SUBSTRING_LOOKUPS;
	return &backend->backend;
}

static void fts_backend_solr_deinit(struct fts_backend *_backend)
{
	struct solr_fts_backend *backend = (struct solr_fts_backend *)_backend;

	i_free(backend->id_namespace);
	i_free(backend->id_username);
	i_free(backend);
}

static void
solr_add_ns_query(string_t *str, struct fts_backend *_backend,
		  struct mail_namespace *ns)
{
	struct solr_fts_backend *backend = (struct solr_fts_backend *)_backend;

	if (ns == backend->default_ns || *ns->prefix == '\0')
		str_append(str, " -ns:[* TO *]");
	else {
		str_append(str, " ns:");
		solr_quote(str, ns->prefix);
	}
}

static void
solr_add_ns_query_http(string_t *str, struct fts_backend *backend,
		       struct mail_namespace *ns)
{
	string_t *tmp;

	tmp = t_str_new(64);
	solr_add_ns_query(tmp, backend, ns);
	solr_connection_http_escape(solr_conn, str, str_c(tmp));
}

static int fts_backend_solr_get_last_uid_fallback(struct fts_backend *backend,
						  uint32_t *last_uid_r)
{
	struct mailbox *box = backend->box;
	struct mailbox_status status;
	ARRAY_TYPE(seq_range) uids;
	const struct seq_range *uidvals;
	unsigned int count;
	string_t *str;

	str = t_str_new(256);
	str_append(str, "fl=uid&rows=1&sort=uid+desc&q=");

	mailbox_get_status(box, STATUS_UIDVALIDITY, &status);
	str_printfa(str, "uidv:%u+box:", status.uidvalidity);
	solr_quote_http(str, box->name);
	solr_add_ns_query_http(str, backend, box->storage->ns);
	str_append(str, "+user:");
	solr_quote_http(str, box->storage->ns->user->username);

	t_array_init(&uids, 1);
	if (solr_connection_select(solr_conn, str_c(str),
				   NULL, NULL, &uids, NULL) < 0)
		return -1;

	uidvals = array_get(&uids, &count);
	if (count == 0) {
		/* nothing indexed yet for this mailbox */
		*last_uid_r = 0;
	} else if (count == 1 && uidvals[0].seq1 == uidvals[0].seq2) {
		*last_uid_r = uidvals[0].seq1;
	} else {
		i_error("fts_solr: Last UID lookup returned multiple rows");
		return -1;
	}
	return 0;
}

static int fts_backend_solr_get_last_uid(struct fts_backend *backend,
					 uint32_t *last_uid_r)
{
	struct mailbox *box = backend->box;
	struct mailbox_status status;
	ARRAY_TYPE(seq_range) uids;
	const struct seq_range *uidvals;
	unsigned int count;
	string_t *str;

	str = t_str_new(256);
	str_append(str, "fl=uid&rows=1&q=last_uid:TRUE+");

	mailbox_get_status(box, STATUS_UIDVALIDITY, &status);
	str_printfa(str, "uidv:%u+box:", status.uidvalidity);
	solr_quote_http(str, box->name);
	solr_add_ns_query_http(str, backend, box->storage->ns);
	str_append(str, "+user:");
	solr_quote_http(str, box->storage->ns->user->username);

	t_array_init(&uids, 1);
	if (solr_connection_select(solr_conn, str_c(str),
				   NULL, NULL, &uids, NULL) < 0)
		return -1;

	uidvals = array_get(&uids, &count);
	if (count == 0) {
		/* either nothing is indexed or we're converting from an
		   older database format without the last_uid fields */
		return fts_backend_solr_get_last_uid_fallback(backend,
							      last_uid_r);
	} else if (count == 1 && uidvals[0].seq1 == uidvals[0].seq2) {
		*last_uid_r = uidvals[0].seq1;
	} else {
		i_error("fts_solr: Last UID lookup returned multiple rows");
		return -1;
	}
	return 0;
}

static const char *
solr_get_vmailbox(struct fts_backend *_backend,
		  struct mailbox *box, const char *ns_prefix,
		  const char *mailbox, string_t *dest)
{
	struct solr_fts_backend *backend = (struct solr_fts_backend *)_backend;
	struct mail_namespace *namespaces = box->storage->ns->user->namespaces;
	struct mail_namespace *ns;

	if (ns_prefix == NULL)
		ns = backend->default_ns;
	else {
		ns = mail_namespace_find_prefix(namespaces, ns_prefix);
		if (ns == NULL)
			return FALSE;
	}
	return mail_namespace_get_vname(ns, dest, mailbox);
}

static bool
solr_virtual_get_last_uids(const char *ns_prefix, const char *mailbox,
			   uint32_t uidvalidity, uint32_t *uid, void *context)
{
	struct fts_backend_solr_get_last_uids_context *ctx = context;
	struct fts_backend_uid_map *map;
	const char *vname;

	vname = solr_get_vmailbox(ctx->backend, ctx->box, ns_prefix,
				  mailbox, ctx->vname);

	map = array_append_space(ctx->last_uids);
	map->mailbox = p_strdup(ctx->pool, vname);
	map->uidvalidity = uidvalidity;
	map->uid = *uid;
	return FALSE;
}

static void
solr_add_pattern(string_t *str, const struct mailbox_virtual_pattern *pattern)
{
	const char *name, *p;

	name = pattern->pattern;
	if (!mail_namespace_update_name(pattern->ns, &name))
		name = mail_namespace_fix_sep(pattern->ns, name);

	/* first check if there are any wildcards in the pattern */
	for (p = name; *p != '\0'; p++) {
		if (*p == '%' || *p == '*')
			break;
	}
	if (*p == '\0') {
		/* full mailbox name */
		solr_quote(str, name);
		return;
	}

	/* there are at least some wildcards. */
	for (p = name; *p != '\0'; p++) {
		if (*p == '%' || *p == '*') {
			if (p == name || (p[-1] != '%' && p[-1] != '*'))
				str_append_c(str, '*');
		} else {
			if (!i_isalnum(*p))
				str_append_c(str, '\\');
			str_append_c(str, *p);
		}
	}
}

static void
fts_backend_solr_filter_mailboxes(struct fts_backend *_backend,
				  string_t *str, struct mailbox *box)
{
	struct solr_fts_backend *backend = (struct solr_fts_backend *)_backend;
	ARRAY_TYPE(mailbox_virtual_patterns) includes_arr, excludes_arr;
	const struct mailbox_virtual_pattern *includes, *excludes;
	unsigned int i, inc_count, exc_count, len;
	string_t *fq;

	t_array_init(&includes_arr, 16);
	t_array_init(&excludes_arr, 16);
	mailbox_get_virtual_box_patterns(box, &includes_arr, &excludes_arr);
	includes = array_get(&includes_arr, &inc_count);
	excludes = array_get(&excludes_arr, &exc_count);
	i_assert(inc_count > 0);

	/* First see if there are any patterns that begin with a wildcard.
	   Solr doesn't allow them, so in that case we'll need to return
	   all mailboxes. */
	for (i = 0; i < inc_count; i++) {
		if (*includes[i].pattern == '*' ||
		    *includes[i].pattern == '%')
			break;
	}

	fq = t_str_new(128);
	if (i == inc_count && inc_count <= FTS_SOLR_MAX_BOX_INC_PATTERNS) {
		/* we can filter what mailboxes we want returned */
		str_append_c(fq, '(');
		for (i = 0; i < inc_count; i++) {
			if (i != 0)
				str_append(fq, " OR ");
			str_append_c(fq, '(');
			str_append(fq, "box:");
			solr_add_pattern(fq, &includes[i]);
			solr_add_ns_query(fq, _backend, includes[i].ns);
			str_append_c(fq, ')');
		}
		str_append_c(fq, ')');
	}
	exc_count = I_MIN(FTS_SOLR_MAX_BOX_EXC_PATTERNS, exc_count);
	for (i = 0; i < exc_count; i++) {
		if (str_len(fq) > len)
			str_append_c(fq, ' ');
		str_append_c(fq, '(');
		str_append(fq, "-box:");
		solr_add_pattern(fq, &excludes[i]);
		if (excludes[i].ns == backend->default_ns) {
			str_append(fq, " OR NOT");
			solr_add_ns_query(fq, _backend, excludes[i].ns);
		} else if (*excludes[i].ns->prefix != '\0') {
			str_append(fq, " OR -ns:");
			solr_quote(fq, excludes[i].ns->prefix);
		}
		str_append_c(fq, ')');
	}
	if (str_len(fq) > 0) {
		str_append(str, "&fq=");
		solr_connection_http_escape(solr_conn, str, str_c(fq));
	}
}

static int
fts_backend_solr_get_all_last_uids(struct fts_backend *backend, pool_t pool,
				   ARRAY_TYPE(fts_backend_uid_map) *last_uids)
{
	struct fts_backend_solr_get_last_uids_context ctx;
	string_t *str;

	memset(&ctx, 0, sizeof(ctx));
	ctx.backend = backend;
	ctx.pool = pool;
	ctx.last_uids = last_uids;
	ctx.box = backend->box;
	ctx.vname = t_str_new(256);

	str = t_str_new(256);
	str_printfa(str, "fl=uid,box,uidv,ns&rows=%u&q=last_uid:TRUE+user:",
		    SOLR_MAX_ROWS);
	solr_quote_http(str, backend->box->storage->ns->user->username);
	fts_backend_solr_filter_mailboxes(backend, str, backend->box);

	return solr_connection_select(solr_conn, str_c(str),
				      solr_virtual_get_last_uids, &ctx,
				      NULL, NULL);
}

static int
fts_backend_solr_build_init(struct fts_backend *backend, uint32_t *last_uid_r,
			    struct fts_backend_build_context **ctx_r)
{
	struct solr_fts_backend_build_context *ctx;
	struct mailbox_status status;

	*last_uid_r = (uint32_t)-1;

	ctx = i_new(struct solr_fts_backend_build_context, 1);
	ctx->ctx.backend = backend;
	ctx->cmd = str_new(default_pool, SOLR_CMDBUF_SIZE);

	mailbox_get_status(backend->box, STATUS_UIDVALIDITY, &status);
	ctx->uid_validity = status.uidvalidity;

	*ctx_r = &ctx->ctx;
	return 0;
}

static void
fts_backend_solr_add_doc_prefix(struct solr_fts_backend_build_context *ctx,
				uint32_t uid)
{
	struct solr_fts_backend *backend =
		(struct solr_fts_backend *)ctx->ctx.backend;
	struct mailbox *box = ctx->ctx.backend->box;

	str_printfa(ctx->cmd, "<doc>"
		    "<field name=\"uid\">%u</field>"
		    "<field name=\"uidv\">%u</field>",
		    uid, ctx->uid_validity);

	if (box->storage->ns != backend->default_ns) {
		str_append(ctx->cmd, "<field name=\"ns\">");
		xml_encode(ctx->cmd, box->storage->ns->prefix);
		str_append(ctx->cmd, "</field>");
	}
	str_append(ctx->cmd, "<field name=\"box\">");
	xml_encode(ctx->cmd, box->name);
	str_append(ctx->cmd, "</field><field name=\"user\">");
	xml_encode(ctx->cmd, box->storage->ns->user->username);
	str_append(ctx->cmd, "</field>");
}

static void xml_encode_id(string_t *str, struct fts_backend *_backend,
			  uint32_t uid, uint32_t uid_validity,
			  const char *mailbox)
{
	struct solr_fts_backend *backend = (struct solr_fts_backend *)_backend;

	if (uid != 0)
		str_printfa(str, "%u/", uid);
	else
		str_append(str, "L/");
	if (backend->id_namespace != NULL) {
		xml_encode(str, backend->id_namespace);
		str_append_c(str, '/');
	}
	str_printfa(str, "%u/", uid_validity);
	xml_encode(str, backend->id_username);
	str_append_c(str, '/');
	xml_encode(str, mailbox);
}

static int
fts_backend_solr_build_more(struct fts_backend_build_context *_ctx,
			    uint32_t uid, const unsigned char *data,
			    size_t size, bool headers)
{
	struct solr_fts_backend_build_context *ctx =
		(struct solr_fts_backend_build_context *)_ctx;
	struct mailbox *box = _ctx->backend->box;
	string_t *cmd = ctx->cmd;

	/* body comes first, then headers */
	if (ctx->prev_uid != uid) {
		/* uid changed */
		if (ctx->post == NULL) {
			ctx->post = solr_connection_post_begin(solr_conn);
			str_append(cmd, "<add>");
		} else {
			str_append(cmd, "</field></doc>");
		}
		ctx->prev_uid = uid;

		fts_backend_solr_add_doc_prefix(ctx, uid);
		str_printfa(cmd, "<field name=\"id\">");
		xml_encode_id(cmd, _ctx->backend, uid, ctx->uid_validity,
			      box->name);
		str_append(cmd, "</field>");

		ctx->headers = headers;
		if (headers) {
			str_append(cmd, "<field name=\"hdr\">");
		} else {
			str_append(cmd, "<field name=\"body\">");
		}
	} else if (headers && !ctx->headers) {
		str_append(cmd, "</field><field name=\"hdr\">");
	} else {
		i_assert(!(!headers && ctx->headers));
	}

	xml_encode_data(cmd, data, size);
	if (str_len(cmd) > SOLR_CMDBUF_SIZE-128) {
		solr_connection_post_more(ctx->post, str_data(cmd),
					  str_len(cmd));
		str_truncate(cmd, 0);
	}
	return 0;
}

static int
fts_backed_solr_build_commit(struct solr_fts_backend_build_context *ctx)
{
	struct mailbox *box = ctx->ctx.backend->box;
	int ret;

	if (ctx->post == NULL)
		return 0;

	str_append(ctx->cmd, "</field></doc>");

	/* Update the mailbox's last_uid field, replacing the existing
	   document. Note that since there is no locking, it's possible that
	   if another session is indexing at the same time, the last_uid value
	   may shrink. This doesn't really matter, we'll simply do more work
	   in future by reindexing some messages. */
	fts_backend_solr_add_doc_prefix(ctx, ctx->prev_uid);
	str_printfa(ctx->cmd, "<field name=\"last_uid\">TRUE</field>"
		    "<field name=\"id\">");
	xml_encode_id(ctx->cmd, ctx->ctx.backend, 0, ctx->uid_validity,
		      box->name);
	str_append(ctx->cmd, "</field></doc></add>");

	solr_connection_post_more(ctx->post, str_data(ctx->cmd),
				  str_len(ctx->cmd));
	ret = solr_connection_post_end(ctx->post);
	/* commit and wait until the documents we just indexed are
	   visible to the following search */
	if (solr_connection_post(solr_conn, "<commit waitFlush=\"false\" "
				 "waitSearcher=\"true\"/>") < 0)
		ret = -1;
	return ret;
}

static int
fts_backend_solr_build_deinit(struct fts_backend_build_context *_ctx)
{
	struct solr_fts_backend_build_context *ctx =
		(struct solr_fts_backend_build_context *)_ctx;
	int ret;

	ret = fts_backed_solr_build_commit(ctx);
	str_free(&ctx->cmd);
	i_free(ctx);
	return ret;
}

static void
fts_backend_solr_expunge(struct fts_backend *backend, struct mail *mail)
{
	struct mailbox_status status;

	mailbox_get_status(mail->box, STATUS_UIDVALIDITY, &status);

	T_BEGIN {
		string_t *cmd;

		cmd = t_str_new(256);
		str_append(cmd, "<delete><id>");
		xml_encode_id(cmd, backend, mail->uid, status.uidvalidity,
			      mail->box->name);
		str_append(cmd, "</id></delete>");

		(void)solr_connection_post(solr_conn, str_c(cmd));
	} T_END;
}

static void
fts_backend_solr_expunge_finish(struct fts_backend *backend ATTR_UNUSED,
				struct mailbox *box ATTR_UNUSED,
				bool committed ATTR_UNUSED)
{
	solr_connection_post(solr_conn,
		"<commit waitFlush=\"false\" waitSearcher=\"false\"/>");
}

static int fts_backend_solr_lock(struct fts_backend *backend ATTR_UNUSED)
{
	return 1;
}

static void fts_backend_solr_unlock(struct fts_backend *backend ATTR_UNUSED)
{
}

static bool solr_virtual_uid_map(const char *ns_prefix, const char *mailbox,
				 uint32_t uidvalidity, uint32_t *uid,
				 void *context)
{
	struct solr_virtual_uid_map_context *ctx = context;
	const char *vname;

	vname = solr_get_vmailbox(ctx->backend, ctx->box, ns_prefix,
				  mailbox, ctx->vname);
	return mailbox_get_virtual_uid(ctx->box, vname, uidvalidity, *uid, uid);
}

static int fts_backend_solr_lookup(struct fts_backend_lookup_context *ctx,
				   ARRAY_TYPE(seq_range) *definite_uids,
				   ARRAY_TYPE(seq_range) *maybe_uids,
				   ARRAY_TYPE(fts_score_map) *scores)
{
	struct mailbox *box = ctx->backend->box;
	struct solr_virtual_uid_map_context uid_map_ctx;
	const struct fts_backend_lookup_field *fields;
	unsigned int i, count;
	struct mailbox_status status;
	string_t *str;
	bool virtual;

	virtual = strcmp(box->storage->name, "virtual") == 0;
	mailbox_get_status(box, STATUS_UIDVALIDITY, &status);

	str = t_str_new(256);
	if (!virtual) {
		str_printfa(str, "fl=uid,score&rows=%u&sort=uid+asc&q=",
			    status.uidnext);
	} else {
		str_printfa(str, "fl=uid,score,box,uidv,ns&rows=%u"
			    "&sort=box+asc,uid+asc&q=",
			    SOLR_MAX_ROWS);
	}

	/* build a lucene search query from the fields */
	fields = array_get(&ctx->fields, &count);
	for (i = 0; i < count; i++) {
		if (i > 0)
			str_append_c(str, '+');

		if ((fields[i].flags & FTS_LOOKUP_FLAG_INVERT) != 0)
			str_append_c(str, '-');

		if ((fields[i].flags & FTS_LOOKUP_FLAG_HEADER) == 0) {
			/* body only */
			i_assert((fields[i].flags & FTS_LOOKUP_FLAG_BODY) != 0);
			str_append(str, "body:");
		} else if ((fields[i].flags & FTS_LOOKUP_FLAG_BODY) == 0) {
			/* header only */
			str_append(str, "hdr:");
		} else {
			/* both */
			str_append(str, "any:");
		}
		solr_quote_http(str, fields[i].key);
	}

	/* use a separate filter query for selecting the mailbox. it shouldn't
	   affect the score and there could be some caching benefits too. */
	str_append(str, "&fq=user:");
	solr_quote_http(str, box->storage->ns->user->username);
	if (virtual)
		fts_backend_solr_filter_mailboxes(ctx->backend, str, box);
	else {
		str_printfa(str, "+uidv:%u+box:", status.uidvalidity);
		solr_quote_http(str, box->name);
		solr_add_ns_query_http(str, ctx->backend, box->storage->ns);
	}

	array_clear(maybe_uids);
	if (!virtual) {
		return solr_connection_select(solr_conn, str_c(str), NULL, NULL,
					      definite_uids, scores);
	} else {
		memset(&uid_map_ctx, 0, sizeof(uid_map_ctx));
		uid_map_ctx.backend = ctx->backend;
		uid_map_ctx.box = box;
		uid_map_ctx.vname = t_str_new(256);
		return solr_connection_select(solr_conn, str_c(str),
					      solr_virtual_uid_map,
					      &uid_map_ctx,
					      definite_uids, scores);
	}
}

struct fts_backend fts_backend_solr = {
	MEMBER(name) "solr",
	MEMBER(flags) 0,

	{
		fts_backend_solr_init,
		fts_backend_solr_deinit,
		fts_backend_solr_get_last_uid,
		fts_backend_solr_get_all_last_uids,
		fts_backend_solr_build_init,
		fts_backend_solr_build_more,
		fts_backend_solr_build_deinit,
		fts_backend_solr_expunge,
		fts_backend_solr_expunge_finish,
		fts_backend_solr_lock,
		fts_backend_solr_unlock,
		NULL,
		NULL,
		fts_backend_solr_lookup
	}
};
