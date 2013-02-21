/* Copyright (c) 2013 Dovecot authors, see the included COPYING file */

#include "lib.h"
#include "ioloop.h"
#include "istream.h"
#include "write-full.h"
#include "http-url.h"
#include "http-client.h"

struct http_test_request {
	struct io *io;
	struct istream *payload;
	bool write_output;
};

static void payload_input(struct http_test_request *req)
{
	const unsigned char *data;
	size_t size;
	int ret;

	/* read payload */
	while ((ret=i_stream_read_data(req->payload, &data, &size, 0)) > 0) {
		i_info("DEBUG: got data (size=%d)", (int)size); 
		if (req->write_output)
			write_full(1, data, size);
		i_stream_skip(req->payload, size);
	}

	if (ret == 0) {
		i_info("DEBUG: REQUEST: NEED MORE DATA");
		/* we will be called again for this request */
	} else {
		if (req->payload->stream_errno != 0)
			i_error("REQUEST PAYLOAD READ ERROR: %m");
		else
			i_info("DEBUG: REQUEST: Finished");
		io_remove(&req->io);
		i_stream_unref(&req->payload);
		i_free(req);
	}
}

static void
got_request_response(const struct http_response *response,
		     struct http_test_request *req)
{
	if (response == NULL) {
		/* request failed */
		i_free(req);
		i_error("HTTP REQUEST FAILED");
		return;
	}

	if (response->status / 100 != 2) {
		i_error("HTTP Request failed: %s", response->reason);
		i_free(req);
		/* payload (if any) is skipped implicitly */
		return;
	}

	i_info("DEBUG: REQUEST SUCCEEDED: %s", response->reason);

	if (response->payload == NULL) {
		i_free(req);
		return;
	}

	i_info("DEBUG: REQUEST: Got payload");
	i_stream_ref(response->payload);
	req->payload = response->payload;
	req->io = io_add(i_stream_get_fd(response->payload), IO_READ,
			 payload_input, req);
	payload_input(req);
}

static const char *test_query1 = "data=Frop&submit=Submit";
static const char *test_query2 = "data=This%20is%20a%20test&submit=Submit";
static const char *test_query3 = "foo=bar";

static void run_tests(struct http_client *http_client)
{
	struct http_client_request *http_req;
	struct http_test_request *test_req;
	struct istream *post_payload;

	// JigSAW is useful for testing: http://jigsaw.w3.org/HTTP/

	test_req = i_new(struct http_test_request, 1);
	http_req = http_client_request(http_client,
		"GET", "pigeonhole.dovecot.org", "/",
		got_request_response, test_req);
	http_client_request_submit(http_req);

	test_req = i_new(struct http_test_request, 1);
	http_req = http_client_request(http_client,
		"GET", "pigeonhole.dovecot.org", "/download.html",
		got_request_response, test_req);
	http_client_request_submit(http_req);

	test_req = i_new(struct http_test_request, 1);
	http_req = http_client_request(http_client,
		"GET", "jigsaw.w3.org", "/HTTP/300/301.html",
		got_request_response, test_req);
	http_client_request_submit(http_req);

	test_req = i_new(struct http_test_request, 1);
	http_req = http_client_request(http_client,
		"GET", "pigeonhole.dovecot.org", "/frop.html",
		got_request_response, test_req);
	http_client_request_submit(http_req);

	test_req = i_new(struct http_test_request, 1);
	http_req = http_client_request(http_client,
		"GET", "jigsaw.w3.org", "/HTTP/300/307.html",
		got_request_response, test_req);
	http_client_request_submit(http_req);

	test_req = i_new(struct http_test_request, 1);
	http_req = http_client_request(http_client,
		"GET", "pigeonhole.dovecot.org", "/documentation.html",
		got_request_response, test_req);
	http_client_request_set_urgent(http_req);
	http_client_request_submit(http_req);

	test_req = i_new(struct http_test_request, 1);
	http_req = http_client_request(http_client,
		"GET", "jigsaw.w3.org", "/HTTP/300/302.html",
		got_request_response, test_req);
	http_client_request_submit(http_req);

	test_req = i_new(struct http_test_request, 1);
	http_req = http_client_request(http_client,
		"POST", "test.dovecot.org", "/http/post/index.php",
		got_request_response, test_req);
	post_payload = i_stream_create_from_data
		((unsigned char *)test_query1, strlen(test_query1));
	http_client_request_set_payload(http_req, post_payload, FALSE);
	i_stream_unref(&post_payload);
	http_client_request_add_header(http_req,
		"Content-Type", "application/x-www-form-urlencoded");
	http_client_request_submit(http_req);

	test_req = i_new(struct http_test_request, 1);
	http_req = http_client_request(http_client,
		"POST", "test.dovecot.org", "/http/post/index.php",
		got_request_response, test_req);
	post_payload = i_stream_create_from_data
		((unsigned char *)test_query2, strlen(test_query2));
	http_client_request_set_payload(http_req, post_payload, TRUE);
	i_stream_unref(&post_payload);
	http_client_request_add_header(http_req,
		"Content-Type", "application/x-www-form-urlencoded");
	http_client_request_submit(http_req);

	test_req = i_new(struct http_test_request, 1);
	http_req = http_client_request(http_client,
		"GET", "pigeonhole.dovecot.org", "/",
		got_request_response, test_req);
	http_client_request_set_port(http_req, 81);
	http_client_request_submit(http_req);

	test_req = i_new(struct http_test_request, 1);
	http_req = http_client_request(http_client,
		"HEAD", "pigeonhole.dovecot.org", "/download.html",
		got_request_response, test_req);
	http_client_request_submit(http_req);

	test_req = i_new(struct http_test_request, 1);
	http_req = http_client_request(http_client,
		"GET", "pigeonhole.dovecot.org", "/",
		got_request_response, test_req);
	http_client_request_set_ssl(http_req, TRUE);
	http_client_request_submit(http_req);

	test_req = i_new(struct http_test_request, 1);
	http_req = http_client_request(http_client,
		"GET", "pigeonhole.dovecot.org", "/download.html",
		got_request_response, test_req);
	http_client_request_set_ssl(http_req, TRUE);
	http_client_request_submit(http_req);

	test_req = i_new(struct http_test_request, 1);
	http_req = http_client_request(http_client,
		"GET", "pigeonhole.dovecot.org", "/documentation.html",
		got_request_response, test_req);
	http_client_request_set_ssl(http_req, TRUE);
	http_client_request_submit(http_req);
	http_client_request_abort(&http_req);
	i_free(test_req);

	test_req = i_new(struct http_test_request, 1);
	http_req = http_client_request(http_client,
		"POST", "posttestserver.com", "/post.php",
		got_request_response, test_req);
	post_payload = i_stream_create_from_data
		((unsigned char *)test_query1, strlen(test_query1));
	http_client_request_set_payload(http_req, post_payload, TRUE);
	i_stream_unref(&post_payload);
	http_client_request_set_ssl(http_req, TRUE);
	http_client_request_submit(http_req);

	test_req = i_new(struct http_test_request, 1);
	http_req = http_client_request(http_client,
		"POST", "posttestserver.com", "/post.php",
		got_request_response, test_req);
	post_payload = i_stream_create_from_data
		((unsigned char *)test_query1, strlen(test_query1));
	http_client_request_set_payload(http_req, post_payload, TRUE);
	i_stream_unref(&post_payload);
	http_client_request_set_ssl(http_req, TRUE);
	http_client_request_submit(http_req);

	test_req = i_new(struct http_test_request, 1);
	http_req = http_client_request(http_client,
		"POST", "posttestserver.com", "/post.php",
		got_request_response, test_req);
	post_payload = i_stream_create_from_data
		((unsigned char *)test_query1, strlen(test_query1));
	http_client_request_set_payload(http_req, post_payload, TRUE);
	i_stream_unref(&post_payload);
	http_client_request_set_ssl(http_req, TRUE);
	http_client_request_submit(http_req);

	test_req = i_new(struct http_test_request, 1);
	http_req = http_client_request(http_client,
		"GET", "wiki2.dovecot.org", "/Pigeonhole",
		got_request_response, test_req);
	http_client_request_submit(http_req);

	test_req = i_new(struct http_test_request, 1);
	http_req = http_client_request(http_client,
		"GET", "jigsaw.w3.org", "/HTTP/ChunkedScript",
		got_request_response, test_req);
	http_client_request_submit(http_req);

	test_req = i_new(struct http_test_request, 1);
	http_req = http_client_request(http_client,
		"POST", "jigsaw.w3.org", "/HTTP/300/Go_307",
		got_request_response, test_req);
	post_payload = i_stream_create_from_data
		((unsigned char *)test_query3, strlen(test_query3));
	http_client_request_set_payload(http_req, post_payload, FALSE);
	i_stream_unref(&post_payload);
	http_client_request_submit(http_req);

	test_req = i_new(struct http_test_request, 1);
	http_req = http_client_request(http_client,
		"POST", "jigsaw.w3.org", "/HTTP/300/Go_307",
		got_request_response, test_req);
	post_payload = i_stream_create_from_data
		((unsigned char *)test_query3, strlen(test_query3));
	http_client_request_set_payload(http_req, post_payload, FALSE);
	i_stream_unref(&post_payload);
	http_client_request_submit(http_req);

	test_req = i_new(struct http_test_request, 1);
	http_req = http_client_request(http_client,
		"POST", "jigsaw.w3.org", "/HTTP/300/Go_307",
		got_request_response, test_req);
	post_payload = i_stream_create_from_data
		((unsigned char *)test_query3, strlen(test_query3));
	http_client_request_set_payload(http_req, post_payload, FALSE);
	i_stream_unref(&post_payload);
	http_client_request_submit(http_req);

	test_req = i_new(struct http_test_request, 1);
	http_req = http_client_request(http_client,
		"PUT", "test.dovecot.org", "/http/put/put.php",
		got_request_response, test_req);
	post_payload = i_stream_create_file("Makefile.am", 10);
	http_client_request_set_payload(http_req, post_payload, TRUE);
	i_stream_unref(&post_payload);
	http_client_request_submit(http_req);
}

static void
test_http_request_init(struct http_client *http_client,
		       const char *method, const char *url_str,
		       struct http_client_request **http_req_r,
		       struct http_test_request **test_req_r)
{
	struct http_client_request *http_req;
	struct http_test_request *test_req;
	struct http_url *url;
	const char *error;

	if (http_url_parse(url_str, NULL, 0, pool_datastack_create(),
			   &url, &error) < 0)
		i_fatal("Invalid URL %s: %s", url_str, error);

	test_req = i_new(struct http_test_request, 1);
	test_req->write_output = TRUE;
	http_req = http_client_request(http_client,
		method, url->host_name,
		t_strconcat("/", url->path, url->enc_query, NULL),
		got_request_response, test_req);
	if (url->have_port)
		http_client_request_set_port(http_req, url->port);
	if (url->have_ssl)
		http_client_request_set_ssl(http_req, TRUE);

	*http_req_r = http_req;
	*test_req_r = test_req;
}

static void run_http_get(struct http_client *http_client, const char *url_str)
{
	struct http_client_request *http_req;
	struct http_test_request *test_req;

	test_http_request_init(http_client, "GET", url_str, &http_req, &test_req);
	http_client_request_submit(http_req);
}

static void run_http_post(struct http_client *http_client, const char *url_str,
			  const char *path)
{
	struct http_client_request *http_req;
	struct http_test_request *test_req;
	struct istream *input;

	test_http_request_init(http_client, "POST", url_str, &http_req, &test_req);
	input = i_stream_create_file(path, IO_BLOCK_SIZE);
	http_client_request_set_payload(http_req, input, FALSE);
	i_stream_unref(&input);
	http_client_request_submit(http_req);
}

int main(int argc, char *argv[])
{
	struct http_client_settings http_set;
	struct http_client *http_client;
	struct ioloop *ioloop;

	memset(&http_set, 0, sizeof(http_set));
	http_set.dns_client_socket_path = "/var/run/dovecot/dns-client";
	http_set.debug = TRUE;
	http_set.ssl_ca_dir = "/etc/ssl/certs";
	http_set.max_idle_time_msecs = 5*1000;
	http_set.max_parallel_connections = 4;
	http_set.max_pipelined_requests = 4;
	http_set.max_redirects = 2;
	http_set.max_attempts = 1;
	http_set.debug = TRUE;
	http_set.rawlog_dir = "/tmp/http-test";

	lib_init();

	ioloop = io_loop_create();
	io_loop_set_running(ioloop);

	http_client = http_client_init(&http_set);

	switch (argc) {
	case 1:
		run_tests(http_client);
		break;
	case 2:
		run_http_get(http_client, argv[1]);
		break;
	case 3:
		run_http_post(http_client, argv[1], argv[2]);
		break;
	default:
		i_fatal("Too many parameters");
	}

	http_client_wait(http_client);
	http_client_deinit(&http_client);

	io_loop_destroy(&ioloop);
	lib_deinit();
}
