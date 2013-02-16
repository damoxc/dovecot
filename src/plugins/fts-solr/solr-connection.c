/* Copyright (c) 2006-2013 Dovecot authors, see the included COPYING file */

#include "lib.h"
#include "array.h"
#include "hash.h"
#include "str.h"
#include "strescape.h"
#include "ioloop.h"
#include "istream.h"
#include "http-url.h"
#include "http-client.h"
#include "solr-connection.h"

#include <expat.h>

enum solr_xml_response_state {
	SOLR_XML_RESPONSE_STATE_ROOT,
	SOLR_XML_RESPONSE_STATE_RESPONSE,
	SOLR_XML_RESPONSE_STATE_RESULT,
	SOLR_XML_RESPONSE_STATE_DOC,
	SOLR_XML_RESPONSE_STATE_CONTENT
};

enum solr_xml_content_state {
	SOLR_XML_CONTENT_STATE_NONE = 0,
	SOLR_XML_CONTENT_STATE_UID,
	SOLR_XML_CONTENT_STATE_SCORE,
	SOLR_XML_CONTENT_STATE_MAILBOX,
	SOLR_XML_CONTENT_STATE_NAMESPACE,
	SOLR_XML_CONTENT_STATE_UIDVALIDITY
};

struct solr_lookup_xml_context {
	enum solr_xml_response_state state;
	enum solr_xml_content_state content_state;
	int depth;

	uint32_t uid, uidvalidity;
	float score;
	char *mailbox, *ns;

	pool_t result_pool;
	/* box_id -> solr_result */
	HASH_TABLE(char *, struct solr_result *) mailboxes;
	ARRAY(struct solr_result *) results;
};

struct solr_connection_post {
	struct solr_connection *conn;

	struct http_client_request *http_req;

	unsigned int failed:1;
};

struct solr_connection {
	struct http_client *http_client;

	XML_Parser xml_parser;

	char *http_host;
	in_port_t http_port;
	char *http_base_url;
	char *http_failure;

	int request_status;

	struct istream *payload;
	struct io *io;

	unsigned int debug:1;
	unsigned int posting:1;
	unsigned int xml_failed:1;
	unsigned int http_ssl:1;
};

static int solr_xml_parse(struct solr_connection *conn,
			  const void *data, size_t size, bool done)
{
	enum XML_Error err;
	int line, col;

	if (conn->xml_failed)
		return -1;

	if (XML_Parse(conn->xml_parser, data, size, done))
		return 0;

	err = XML_GetErrorCode(conn->xml_parser);
	if (err != XML_ERROR_FINISHED) {
		line = XML_GetCurrentLineNumber(conn->xml_parser);
		col = XML_GetCurrentColumnNumber(conn->xml_parser);
		i_error("fts_solr: Invalid XML input at %d:%d: %s "
			"(near: %.*s)", line, col, XML_ErrorString(err),
			(int)I_MIN(size, 128), (const char *)data);
		conn->xml_failed = TRUE;
		return -1;
	}
	return 0;
}

int solr_connection_init(const char *url, bool debug,
			 struct solr_connection **conn_r, const char **error_r)
{
	struct http_client_settings http_set;
	struct solr_connection *conn;
	struct http_url *http_url;
	const char *error;

	if (http_url_parse(url, NULL, 0, pool_datastack_create(),
			   &http_url, &error) < 0) {
		*error_r = t_strdup_printf(
			"fts_solr: Failed to parse HTTP url: %s", error);
		return -1;
	}

	conn = i_new(struct solr_connection, 1);
	conn->http_host = i_strdup(http_url->host_name);
	conn->http_port = http_url->port;
	conn->http_base_url = i_strconcat(http_url->path, http_url->enc_query, NULL);
	conn->http_ssl = http_url->have_ssl;
	conn->debug = debug;

	memset(&http_set, 0, sizeof(http_set));
	http_set.debug = TRUE;
	http_set.max_idle_time_msecs = 5*1000;
	http_set.max_parallel_connections = 1;
	http_set.max_pipelined_requests = 1;
	http_set.max_redirects = 1;
	http_set.max_attempts = 3;
	http_set.debug = conn->debug;

	conn->http_client = http_client_init(&http_set);

	conn->xml_parser = XML_ParserCreate("UTF-8");
	if (conn->xml_parser == NULL) {
		i_fatal_status(FATAL_OUTOFMEM,
			       "fts_solr: Failed to allocate XML parser");
	}
	*conn_r = conn;
	return 0;
}

void solr_connection_deinit(struct solr_connection *conn)
{
	http_client_deinit(&conn->http_client);
	XML_ParserFree(conn->xml_parser);
	i_free(conn->http_host);
	i_free(conn->http_base_url);
	i_free(conn);
}

static const char *attrs_get_name(const char **attrs)
{
	for (; *attrs != NULL; attrs += 2) {
		if (strcmp(attrs[0], "name") == 0)
			return attrs[1];
	}
	return "";
}

static void
solr_lookup_xml_start(void *context, const char *name, const char **attrs)
{
	struct solr_lookup_xml_context *ctx = context;
	const char *name_attr;

	i_assert(ctx->depth >= (int)ctx->state);

	ctx->depth++;
	if (ctx->depth - 1 > (int)ctx->state) {
		/* skipping over unwanted elements */
		return;
	}

	/* response -> result -> doc */
	switch (ctx->state) {
	case SOLR_XML_RESPONSE_STATE_ROOT:
		if (strcmp(name, "response") == 0)
			ctx->state++;
		break;
	case SOLR_XML_RESPONSE_STATE_RESPONSE:
		if (strcmp(name, "result") == 0)
			ctx->state++;
		break;
	case SOLR_XML_RESPONSE_STATE_RESULT:
		if (strcmp(name, "doc") == 0) {
			ctx->state++;
			ctx->uid = 0;
			ctx->score = 0;
			i_free_and_null(ctx->mailbox);
			i_free_and_null(ctx->ns);
			ctx->uidvalidity = 0;
		}
		break;
	case SOLR_XML_RESPONSE_STATE_DOC:
		name_attr = attrs_get_name(attrs);
		if (strcmp(name_attr, "uid") == 0)
			ctx->content_state = SOLR_XML_CONTENT_STATE_UID;
		else if (strcmp(name_attr, "score") == 0)
			ctx->content_state = SOLR_XML_CONTENT_STATE_SCORE;
		else if (strcmp(name_attr, "box") == 0)
			ctx->content_state = SOLR_XML_CONTENT_STATE_MAILBOX;
		else if (strcmp(name_attr, "ns") == 0)
			ctx->content_state = SOLR_XML_CONTENT_STATE_NAMESPACE;
		else if (strcmp(name_attr, "uidv") == 0)
			ctx->content_state = SOLR_XML_CONTENT_STATE_UIDVALIDITY;
		else 
			break;
		ctx->state++;
		break;
	case SOLR_XML_RESPONSE_STATE_CONTENT:
		break;
	}
}

static struct solr_result *
solr_result_get(struct solr_lookup_xml_context *ctx, const char *box_id)
{
	struct solr_result *result;
	char *box_id_dup;

	result = hash_table_lookup(ctx->mailboxes, box_id);
	if (result != NULL)
		return result;

	box_id_dup = p_strdup(ctx->result_pool, box_id);
	result = p_new(ctx->result_pool, struct solr_result, 1);
	result->box_id = box_id_dup;
	p_array_init(&result->uids, ctx->result_pool, 32);
	p_array_init(&result->scores, ctx->result_pool, 32);
	hash_table_insert(ctx->mailboxes, box_id_dup, result);
	array_append(&ctx->results, &result, 1);
	return result;
}

static void solr_lookup_add_doc(struct solr_lookup_xml_context *ctx)
{
	struct fts_score_map *score;
	struct solr_result *result;
	const char *box_id;

	if (ctx->uid == 0) {
		i_error("fts_solr: Query didn't return uid");
		return;
	}

	if (ctx->mailbox == NULL) {
		/* looking up from a single mailbox only */
		box_id = "";
	} else if (ctx->uidvalidity != 0) {
		/* old style lookup */
		string_t *str = t_str_new(64);
		str_printfa(str, "%u\001", ctx->uidvalidity);
		str_append(str, ctx->mailbox);
		if (ctx->ns != NULL)
			str_printfa(str, "\001%s", ctx->ns);
		box_id = str_c(str);
	} else {
		/* new style lookup */
		box_id = ctx->mailbox;
	}
	result = solr_result_get(ctx, box_id);

	seq_range_array_add(&result->uids, ctx->uid);
	if (ctx->score != 0) {
		score = array_append_space(&result->scores);
		score->uid = ctx->uid;
		score->score = ctx->score;
	}
}

static void solr_lookup_xml_end(void *context, const char *name ATTR_UNUSED)
{
	struct solr_lookup_xml_context *ctx = context;

	i_assert(ctx->depth >= (int)ctx->state);

	if (ctx->state == SOLR_XML_RESPONSE_STATE_CONTENT &&
	    ctx->content_state == SOLR_XML_CONTENT_STATE_MAILBOX &&
	    ctx->mailbox == NULL) {
		/* mailbox is namespace prefix */
		ctx->mailbox = i_strdup("");
	}

	if (ctx->depth == (int)ctx->state) {
		if (ctx->state == SOLR_XML_RESPONSE_STATE_DOC) {
			T_BEGIN {
				solr_lookup_add_doc(ctx);
			} T_END;
		}
		ctx->state--;
		ctx->content_state = SOLR_XML_CONTENT_STATE_NONE;
	}
	ctx->depth--;
}

static int uint32_parse(const char *str, int len, uint32_t *value_r)
{
	uint32_t value = 0;
	int i;

	for (i = 0; i < len; i++) {
		if (str[i] < '0' || str[i] > '9')
			break;
		value = value*10 + str[i]-'0';
	}
	if (i != len)
		return -1;

	*value_r = value;
	return 0;
}

static void solr_lookup_xml_data(void *context, const char *str, int len)
{
	struct solr_lookup_xml_context *ctx = context;
	char *new_name;

	switch (ctx->content_state) {
	case SOLR_XML_CONTENT_STATE_NONE:
		break;
	case SOLR_XML_CONTENT_STATE_UID:
		if (uint32_parse(str, len, &ctx->uid) < 0)
			i_error("fts_solr: received invalid uid");
		break;
	case SOLR_XML_CONTENT_STATE_SCORE:
		T_BEGIN {
			ctx->score = strtod(t_strndup(str, len), NULL);
		} T_END;
		break;
	case SOLR_XML_CONTENT_STATE_MAILBOX:
		/* this may be called multiple times, for example if input
		   contains '&' characters */
		new_name = ctx->mailbox == NULL ? i_strndup(str, len) :
			i_strconcat(ctx->mailbox, t_strndup(str, len), NULL);
		i_free(ctx->mailbox);
		ctx->mailbox = new_name;
		break;
	case SOLR_XML_CONTENT_STATE_NAMESPACE:
		new_name = ctx->ns == NULL ? i_strndup(str, len) :
			i_strconcat(ctx->ns, t_strndup(str, len), NULL);
		i_free(ctx->ns);
		ctx->ns = new_name;
		break;
	case SOLR_XML_CONTENT_STATE_UIDVALIDITY:
		if (uint32_parse(str, len, &ctx->uidvalidity) < 0)
			i_error("fts_solr: received invalid uidvalidity");
		break;
	}
}

static void solr_connection_payload_input(struct solr_connection *conn)
{
	const unsigned char *data;
	size_t size;
	int ret;

	/* read payload */
	while ((ret = i_stream_read_data(conn->payload, &data, &size, 0)) > 0) {
		(void)solr_xml_parse(conn, data, size, FALSE);
		i_stream_skip(conn->payload, size);
	}

	if (ret == 0) {
		/* we will be called again for more data */
	} else {
		if (conn->payload->stream_errno != 0) {
			i_error("fts_solr: failed to read payload from HTTP server: %m");
			conn->request_status = -1;
		}
		io_remove(&conn->io);
		i_stream_unref(&conn->payload);
	}
}

static void
solr_connection_select_response(const struct http_response *response,
				struct solr_connection *conn)
{
	if (response == NULL) {
		/* request failed */
		i_error("fts_solr: HTTP GET request failed");
		conn->request_status = -1;
		return;
	}

	if (response->status / 100 != 2) {
		i_error("fts_solr: Lookup failed: %s", response->reason);
		conn->request_status = -1;
		return;
	}

	if (response->payload == NULL) {
		i_error("fts_solr: Lookup failed: Empty response payload");
		conn->request_status = -1;
		return;
	}

	i_stream_ref(response->payload);
	conn->payload = response->payload;
	conn->io = io_add(i_stream_get_fd(response->payload), IO_READ,
			  solr_connection_payload_input, conn);
	solr_connection_payload_input(conn);
}

int solr_connection_select(struct solr_connection *conn, const char *query,
			   pool_t pool, struct solr_result ***box_results_r)
{
	struct solr_lookup_xml_context solr_lookup_context;
	struct http_client_request *http_req;
	const char *url;
	int parse_ret;

	i_assert(!conn->posting);

	memset(&solr_lookup_context, 0, sizeof(solr_lookup_context));
	solr_lookup_context.result_pool = pool;
	hash_table_create(&solr_lookup_context.mailboxes, default_pool, 0,
			  str_hash, strcmp);
	p_array_init(&solr_lookup_context.results, pool, 32);

	i_free_and_null(conn->http_failure);
	conn->xml_failed = FALSE;
	XML_ParserReset(conn->xml_parser, "UTF-8");
	XML_SetElementHandler(conn->xml_parser,
			      solr_lookup_xml_start, solr_lookup_xml_end);
	XML_SetCharacterDataHandler(conn->xml_parser, solr_lookup_xml_data);
	XML_SetUserData(conn->xml_parser, &solr_lookup_context);

	url = t_strconcat(conn->http_base_url, "select?", query, NULL);

	http_req = http_client_request(conn->http_client, "GET",
				       conn->http_host, url,
				       solr_connection_select_response, conn);
	http_client_request_set_port(http_req, conn->http_port);
	http_client_request_set_ssl(http_req, conn->http_ssl);
	http_client_request_add_header(http_req, "Content-Type", "text/xml");
	http_client_request_submit(http_req);

	conn->request_status = 0;
	http_client_wait(conn->http_client);

	if (conn->request_status < 0)
		return -1;

	parse_ret = solr_xml_parse(conn, "", 0, TRUE);
	hash_table_destroy(&solr_lookup_context.mailboxes);

	array_append_zero(&solr_lookup_context.results);
	*box_results_r = array_idx_modifiable(&solr_lookup_context.results, 0);
	return parse_ret;
}

static void
solr_connection_update_response(const struct http_response *response,
				struct solr_connection *conn)
{
	if (response == NULL) {
		/* request failed */
		i_error("fts_solr: HTTP POST request failed");
		conn->request_status = -1;
		return;
	}

	if (response->status / 100 != 2) {
		i_error("fts_solr: Indexing failed: %s", response->reason);
		conn->request_status = -1;
		return;
	}
}

static struct http_client_request *
solr_connection_post_request(struct solr_connection *conn)
{
	struct http_client_request *http_req;
	const char *url;

	url = t_strconcat(conn->http_base_url, "update", NULL);

	http_req = http_client_request(conn->http_client, "POST",
				       conn->http_host, url,
				       solr_connection_update_response, conn);
	http_client_request_set_port(http_req, conn->http_port);
	http_client_request_set_ssl(http_req, conn->http_ssl);
	http_client_request_add_header(http_req, "Content-Type", "text/xml");
	return http_req;
}

struct solr_connection_post *
solr_connection_post_begin(struct solr_connection *conn)
{
	struct solr_connection_post *post;

	i_assert(!conn->posting);
	conn->posting = TRUE;

	post = i_new(struct solr_connection_post, 1);
	post->conn = conn;
	post->http_req = solr_connection_post_request(conn);
	XML_ParserReset(conn->xml_parser, "UTF-8");
	return post;
}

void solr_connection_post_more(struct solr_connection_post *post,
			       const unsigned char *data, size_t size)
{
	struct solr_connection *conn = post->conn;
	i_assert(post->conn->posting);

	if (post->failed)
		return;

	if (http_client_request_send_payload(&post->http_req, data, size) != 0 &&
	    conn->request_status < 0)
		post->failed = TRUE;
}

int solr_connection_post_end(struct solr_connection_post *post)
{
	struct solr_connection *conn = post->conn;
	int ret = post->failed ? -1 : 0;

	i_assert(conn->posting);

	if (!post->failed) {
		if (http_client_request_send_payload(&post->http_req, NULL, 0) <= 0 ||
			conn->request_status < 0) {
			ret = -1;
		}
	} else {
		if (post->http_req != NULL)
			http_client_request_abort(&post->http_req);
	}
	i_free(post);

	conn->posting = FALSE;
	return ret;
}

int solr_connection_post(struct solr_connection *conn, const char *cmd)
{
	struct http_client_request *http_req;
	struct istream *post_payload;

	i_assert(!conn->posting);

	http_req = solr_connection_post_request(conn);
	post_payload = i_stream_create_from_data(cmd, strlen(cmd));
	http_client_request_set_payload(http_req, post_payload, TRUE);
	i_stream_unref(&post_payload);
	http_client_request_submit(http_req);

	XML_ParserReset(conn->xml_parser, "UTF-8");

	conn->request_status = 0;
	http_client_wait(conn->http_client);

	return conn->request_status;
}
