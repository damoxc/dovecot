#ifndef SOLR_CONNECTION_H
#define SOLR_CONNECTION_H

#include "seq-range-array.h"
#include "fts-api.h"

struct solr_connection;

struct solr_result {
	const char *box_id;

	ARRAY_TYPE(seq_range) uids;
	ARRAY_TYPE(fts_score_map) scores;
};

int solr_connection_init(const char *url, bool debug,
			 struct solr_connection **conn_r, const char **error_r);
void solr_connection_deinit(struct solr_connection *conn);

int solr_connection_select(struct solr_connection *conn, const char *query,
			   pool_t pool, struct solr_result ***box_results_r);
int solr_connection_post(struct solr_connection *conn, const char *cmd);

struct solr_connection_post *
solr_connection_post_begin(struct solr_connection *conn);
void solr_connection_post_more(struct solr_connection_post *post,
			       const unsigned char *data, size_t size);
int solr_connection_post_end(struct solr_connection_post *post);

#endif
