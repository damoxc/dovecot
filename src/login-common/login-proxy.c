/* Copyright (c) 2004-2009 Dovecot authors, see the included COPYING file */

#include "common.h"
#include "ioloop.h"
#include "istream.h"
#include "ostream.h"
#include "llist.h"
#include "str-sanitize.h"
#include "time-util.h"
#include "master-service.h"
#include "client-common.h"
#include "ssl-proxy.h"
#include "login-proxy-state.h"
#include "login-proxy.h"

#define MAX_PROXY_INPUT_SIZE 4096
#define OUTBUF_THRESHOLD 1024

struct login_proxy {
	struct login_proxy *prev, *next;

	struct client *prelogin_client;
	int client_fd, server_fd;
	struct io *client_io, *server_io;
	struct istream *server_input;
	struct ostream *client_output, *server_output;
	struct ip_addr ip;
	struct ssl_proxy *ssl_proxy;

	struct timeval created;
	struct timeout *to;
	struct login_proxy_record *state_rec;

	char *host, *user;
	unsigned int port;
	enum login_proxy_ssl_flags ssl_flags;

	proxy_callback_t *callback;
	void *context;

	unsigned int destroying:1;
	unsigned int disconnecting:1;
};

static struct login_proxy_state *proxy_state;
static struct login_proxy *login_proxies = NULL;

static void server_input(struct login_proxy *proxy)
{
	unsigned char buf[OUTBUF_THRESHOLD];
	ssize_t ret;

	if (o_stream_get_buffer_used_size(proxy->client_output) >
	    OUTBUF_THRESHOLD) {
		/* client's output buffer is already quite full.
		   don't send more until we're below threshold. */
		io_remove(&proxy->server_io);
		return;
	}

	ret = net_receive(proxy->server_fd, buf, sizeof(buf));
	if (ret < 0 || o_stream_send(proxy->client_output, buf, ret) != ret)
                login_proxy_free(&proxy);
}

static void proxy_client_input(struct login_proxy *proxy)
{
	unsigned char buf[OUTBUF_THRESHOLD];
	ssize_t ret;

	if (o_stream_get_buffer_used_size(proxy->server_output) >
	    OUTBUF_THRESHOLD) {
		/* proxy's output buffer is already quite full.
		   don't send more until we're below threshold. */
		io_remove(&proxy->client_io);
		return;
	}

	ret = net_receive(proxy->client_fd, buf, sizeof(buf));
	if (ret < 0 || o_stream_send(proxy->server_output, buf, ret) != ret)
                login_proxy_free(&proxy);
}

static int server_output(struct login_proxy *proxy)
{
	if (o_stream_flush(proxy->server_output) < 0) {
                login_proxy_free(&proxy);
		return 1;
	}

	if (proxy->client_io == NULL &&
	    o_stream_get_buffer_used_size(proxy->server_output) <
	    OUTBUF_THRESHOLD) {
		/* there's again space in proxy's output buffer, so we can
		   read more from client. */
		proxy->client_io = io_add(proxy->client_fd, IO_READ,
					  proxy_client_input, proxy);
	}
	return 1;
}

static int proxy_client_output(struct login_proxy *proxy)
{
	if (o_stream_flush(proxy->client_output) < 0) {
                login_proxy_free(&proxy);
		return 1;
	}

	if (proxy->server_io == NULL &&
	    o_stream_get_buffer_used_size(proxy->client_output) <
	    OUTBUF_THRESHOLD) {
		/* there's again space in client's output buffer, so we can
		   read more from proxy. */
		proxy->server_io =
			io_add(proxy->server_fd, IO_READ, server_input, proxy);
	}
	return 1;
}

static void proxy_prelogin_input(struct login_proxy *proxy)
{
	proxy->callback(proxy->context);
}

static void proxy_plain_connected(struct login_proxy *proxy)
{
	proxy->server_input =
		i_stream_create_fd(proxy->server_fd, MAX_PROXY_INPUT_SIZE,
				   FALSE);
	proxy->server_output =
		o_stream_create_fd(proxy->server_fd, (size_t)-1, FALSE);

	proxy->server_io =
		io_add(proxy->server_fd, IO_READ, proxy_prelogin_input, proxy);
}

static void proxy_fail_connect(struct login_proxy *proxy)
{
	if (timeval_cmp(&proxy->created, &proxy->state_rec->last_success) < 0) {
		/* there was a successful connection done since we started
		   connecting. perhaps this is just a temporary one-off
		   failure. */
	} else {
		proxy->state_rec->last_failure = ioloop_timeval;
	}
	proxy->state_rec->num_waiting_connections--;
	proxy->state_rec = NULL;
}

static void proxy_wait_connect(struct login_proxy *proxy)
{
	int err;

	err = net_geterror(proxy->server_fd);
	if (err != 0) {
		i_error("proxy: connect(%s, %u) failed: %s",
			proxy->host, proxy->port, strerror(err));
		proxy_fail_connect(proxy);
                login_proxy_free(&proxy);
		return;
	}
	proxy->state_rec->last_success = ioloop_timeval;
	proxy->state_rec->num_waiting_connections--;
	proxy->state_rec = NULL;

	if (proxy->to != NULL)
		timeout_remove(&proxy->to);

	if ((proxy->ssl_flags & PROXY_SSL_FLAG_YES) != 0 &&
	    (proxy->ssl_flags & PROXY_SSL_FLAG_STARTTLS) == 0) {
		if (login_proxy_starttls(proxy) < 0) {
			login_proxy_free(&proxy);
			return;
		}
	} else {
		io_remove(&proxy->server_io);
		proxy_plain_connected(proxy);
	}
}

static void proxy_connect_timeout(struct login_proxy *proxy)
{
	i_error("proxy: connect(%s, %u) timed out", proxy->host, proxy->port);
	proxy_fail_connect(proxy);
	login_proxy_free(&proxy);
}

#undef login_proxy_new
struct login_proxy *
login_proxy_new(struct client *client, const struct login_proxy_settings *set,
		proxy_callback_t *callback, void *context)
{
	struct login_proxy *proxy;
	struct login_proxy_record *rec;
	struct ip_addr ip;
	int fd;

	if (set->host == NULL) {
		i_error("proxy(%s): host not given", client->virtual_user);
		return NULL;
	}

	if (net_addr2ip(set->host, &ip) < 0) {
		i_error("proxy(%s): %s is not a valid IP",
			client->virtual_user, set->host);
		return NULL;
	}

	rec = login_proxy_state_get(proxy_state, &ip);
	if (timeval_cmp(&rec->last_failure, &rec->last_success) > 0 &&
	    rec->num_waiting_connections != 0) {
		/* the server is down. fail immediately */
	       return NULL;
	}

	fd = net_connect_ip(&ip, set->port, NULL);
	if (fd < 0) {
		i_error("proxy(%s): connect(%s, %u) failed: %m",
			client->virtual_user, set->host, set->port);
		return NULL;
	}

	proxy = i_new(struct login_proxy, 1);
	proxy->created = ioloop_timeval;
	proxy->host = i_strdup(set->host);
	proxy->user = i_strdup(client->virtual_user);
	proxy->port = set->port;
	proxy->ssl_flags = set->ssl_flags;
	proxy->prelogin_client = client;

	proxy->server_fd = fd;
	proxy->server_io = io_add(fd, IO_WRITE, proxy_wait_connect, proxy);
	if (set->connect_timeout_msecs != 0) {
		proxy->to = timeout_add(set->connect_timeout_msecs,
					proxy_connect_timeout, proxy);
	}

	proxy->callback = callback;
	proxy->context = context;

	proxy->ip = client->ip;
	proxy->client_fd = -1;

	proxy->state_rec = rec;
	rec->num_waiting_connections++;
	return proxy;
}

void login_proxy_free(struct login_proxy **_proxy)
{
	struct login_proxy *proxy = *_proxy;
	const char *ipstr;

	*_proxy = NULL;

	if (proxy->destroying)
		return;
	proxy->destroying = TRUE;

	if (proxy->to != NULL)
		timeout_remove(&proxy->to);

	if (proxy->state_rec != NULL)
		proxy->state_rec->num_waiting_connections--;
	if (proxy->to != NULL)
		timeout_remove(&proxy->to);

	if (proxy->server_io != NULL)
		io_remove(&proxy->server_io);
	if (proxy->server_input != NULL)
		i_stream_destroy(&proxy->server_input);
	if (proxy->server_output != NULL)
		o_stream_destroy(&proxy->server_output);

	if (proxy->client_fd != -1) {
		/* detached proxy */
		DLLIST_REMOVE(&login_proxies, proxy);

		ipstr = net_ip2addr(&proxy->ip);
		i_info("proxy(%s): disconnecting %s",
		       str_sanitize(proxy->user, 80),
		       ipstr != NULL ? ipstr : "");

		if (proxy->client_io != NULL)
			io_remove(&proxy->client_io);
		if (proxy->client_output != NULL)
			o_stream_destroy(&proxy->client_output);
		net_disconnect(proxy->client_fd);
	} else {
		i_assert(proxy->client_io == NULL);
		i_assert(proxy->client_output == NULL);

		proxy->callback(proxy->context);
	}

	if (proxy->ssl_proxy != NULL)
		ssl_proxy_free(&proxy->ssl_proxy);
	net_disconnect(proxy->server_fd);

	i_free(proxy->host);
	i_free(proxy->user);
	i_free(proxy);

	master_service_client_connection_destroyed(master_service);
}

bool login_proxy_is_ourself(const struct client *client, const char *host,
			    unsigned int port, const char *destuser)
{
	struct ip_addr ip;

	if (port != client->local_port)
		return FALSE;

	if (net_addr2ip(host, &ip) < 0)
		return FALSE;
	if (!net_ip_compare(&ip, &client->local_ip))
		return FALSE;

	return strcmp(client->virtual_user, destuser) == 0;
}

struct istream *login_proxy_get_istream(struct login_proxy *proxy)
{
	return proxy->disconnecting ? NULL : proxy->server_input;
}

struct ostream *login_proxy_get_ostream(struct login_proxy *proxy)
{
	return proxy->server_output;
}

const char *login_proxy_get_host(const struct login_proxy *proxy)
{
	return proxy->host;
}

unsigned int login_proxy_get_port(const struct login_proxy *proxy)
{
	return proxy->port;
}

enum login_proxy_ssl_flags
login_proxy_get_ssl_flags(const struct login_proxy *proxy)
{
	return proxy->ssl_flags;
}

void login_proxy_detach(struct login_proxy *proxy, struct istream *client_input,
			struct ostream *client_output)
{
	const unsigned char *data;
	size_t size;

	i_assert(proxy->client_fd == -1);
	i_assert(proxy->server_output != NULL);

	proxy->prelogin_client = NULL;
	proxy->client_fd = i_stream_get_fd(client_input);
	proxy->client_output = client_output;

	o_stream_set_max_buffer_size(client_output, (size_t)-1);
	o_stream_set_flush_callback(client_output, proxy_client_output, proxy);

	/* send all pending client input to proxy and get rid of the stream */
	data = i_stream_get_data(client_input, &size);
	if (size != 0)
		(void)o_stream_send(proxy->server_output, data, size);
	i_stream_unref(&client_input);

	/* from now on, just do dummy proxying */
	io_remove(&proxy->server_io);
	proxy->server_io =
		io_add(proxy->server_fd, IO_READ, server_input, proxy);
	proxy->client_io =
		io_add(proxy->client_fd, IO_READ, proxy_client_input, proxy);
	o_stream_set_flush_callback(proxy->server_output, server_output, proxy);
	i_stream_destroy(&proxy->server_input);

	proxy->callback = NULL;
	proxy->context = NULL;

	DLLIST_PREPEND(&login_proxies, proxy);
}

static int login_proxy_ssl_handshaked(void *context)
{
	struct login_proxy *proxy = context;

	if ((proxy->ssl_flags & PROXY_SSL_FLAG_ANY_CERT) != 0 ||
	    ssl_proxy_has_valid_client_cert(proxy->ssl_proxy))
		return 0;

	if (!ssl_proxy_has_broken_client_cert(proxy->ssl_proxy)) {
		client_log_err(proxy->prelogin_client, t_strdup_printf(
			"proxy: SSL certificate not received from %s:%u",
			proxy->host, proxy->port));
	} else {
		client_log_err(proxy->prelogin_client, t_strdup_printf(
			"proxy: Received invalid SSL certificate from %s:%u",
			proxy->host, proxy->port));
	}
	proxy->disconnecting = TRUE;
	return -1;
}

int login_proxy_starttls(struct login_proxy *proxy)
{
	int fd;

	if (proxy->server_input != NULL)
		i_stream_destroy(&proxy->server_input);
	if (proxy->server_output != NULL)
		o_stream_destroy(&proxy->server_output);
	io_remove(&proxy->server_io);

	fd = ssl_proxy_client_new(proxy->server_fd, &proxy->ip,
				  proxy->prelogin_client->set,
				  login_proxy_ssl_handshaked, proxy,
				  &proxy->ssl_proxy);
	if (fd < 0) {
		client_log_err(proxy->prelogin_client, t_strdup_printf(
			"proxy: SSL handshake failed to %s:%u",
			proxy->host, proxy->port));
		return -1;
	}

	proxy->server_fd = fd;
	proxy_plain_connected(proxy);
	return 0;
}

void login_proxy_init(void)
{
	proxy_state = login_proxy_state_init();
}

void login_proxy_deinit(void)
{
	struct login_proxy *proxy;

	while (login_proxies != NULL) {
		proxy = login_proxies;
		login_proxy_free(&proxy);
	}
	login_proxy_state_deinit(&proxy_state);
}
