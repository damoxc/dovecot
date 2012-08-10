/* Copyright (c) 2012 Dovecot authors, see the included COPYING memcached */

#include "lib.h"
#include "array.h"
#include "str.h"
#include "istream.h"
#include "ostream.h"
#include "connection.h"
#include "dict-private.h"

#define MEMCACHED_DEFAULT_PORT 11211
#define MEMCACHED_DEFAULT_LOOKUP_TIMEOUT_MSECS (1000*30)

/* we need only very limited memcached functionality, so just define the binary
   protocol ourself instead requiring protocol_binary.h */
#define MEMCACHED_REQUEST_HDR_MAGIC 0x80
#define MEMCACHED_REPLY_HDR_MAGIC 0x81

#define MEMCACHED_REQUEST_HDR_LENGTH 24
#define MEMCACHED_REPLY_HDR_LENGTH 24

#define MEMCACHED_CMD_GET 0x00

#define MEMCACHED_DATA_TYPE_RAW 0x00

enum memcached_response {
	MEMCACHED_RESPONSE_OK		= 0x0000,
	MEMCACHED_RESPONSE_NOTFOUND	= 0x0001,
	MEMCACHED_RESPONSE_INTERNALERROR= 0x0084,
	MEMCACHED_RESPONSE_BUSY		= 0x0085,
	MEMCACHED_RESPONSE_TEMPFAILURE	= 0x0086
};

struct memcached_connection {
	struct connection conn;
	struct memcached_dict *dict;

	buffer_t *cmd;
	struct {
		const unsigned char *value;
		unsigned int value_len;
		enum memcached_response status;
		bool reply_received;
	} reply;
};

struct memcached_dict {
	struct dict dict;
	struct ip_addr ip;
	char *key_prefix;
	unsigned int port;
	unsigned int timeout_msecs;

	struct ioloop *ioloop;
	struct memcached_connection conn;

	bool connected;
};

static struct connection_list *memcached_connections;

static void memcached_conn_destroy(struct connection *_conn)
{
	struct memcached_connection *conn = (struct memcached_connection *)_conn;

	conn->dict->connected = FALSE;
	connection_disconnect(_conn);

	if (conn->dict->ioloop != NULL)
		io_loop_stop(conn->dict->ioloop);
}

static int memcached_input_get(struct memcached_connection *conn)
{
	const unsigned char *data;
	size_t size;
	uint32_t body_len, value_pos;
	uint16_t key_len, key_pos, status;
	uint8_t extras_len, data_type;

	data = i_stream_get_data(conn->conn.input, &size);
	if (size < MEMCACHED_REPLY_HDR_LENGTH)
		return 0;

	if (data[0] != MEMCACHED_REPLY_HDR_MAGIC) {
		i_error("memcached: Invalid reply magic: %u != %u",
			data[0], MEMCACHED_REPLY_HDR_MAGIC);
		return -1;
	}
	memcpy(&body_len, data+8, 4); body_len = ntohl(body_len);
	body_len += MEMCACHED_REPLY_HDR_LENGTH;
	if (size < body_len) {
		/* we haven't read the whole response yet */
		return 0;
	}

	memcpy(&key_len, data+2, 2); key_len = ntohs(key_len);
	extras_len = data[4];
	data_type = data[5];
	memcpy(&status, data+6, 2); status = ntohs(status);
	if (data_type != MEMCACHED_DATA_TYPE_RAW) {
		i_error("memcached: Unsupported data type: %u != %u",
			data[0], MEMCACHED_DATA_TYPE_RAW);
		return -1;
	}

	key_pos = MEMCACHED_REPLY_HDR_LENGTH + extras_len;
	value_pos = key_pos + key_len;
	if (value_pos > body_len) {
		i_error("memcached: Invalid key/extras lengths");
		return -1;
	}
	conn->reply.value = data + value_pos;
	conn->reply.value_len = body_len - value_pos;
	conn->reply.status = status;

	i_stream_skip(conn->conn.input, body_len);
	conn->reply.reply_received = TRUE;

	if (conn->dict->ioloop != NULL)
		io_loop_stop(conn->dict->ioloop);
	return 1;
}

static void memcached_conn_input(struct connection *_conn)
{
	struct memcached_connection *conn = (struct memcached_connection *)_conn;

	switch (i_stream_read(_conn->input)) {
	case 0:
		return;
	case -1:
		memcached_conn_destroy(_conn);
		return;
	default:
		break;
	}

	if (memcached_input_get(conn) < 0)
		memcached_conn_destroy(_conn);
}

static void memcached_conn_connected(struct connection *_conn)
{
	struct memcached_connection *conn = (struct memcached_connection *)_conn;

	if ((errno = net_geterror(_conn->fd_in)) != 0) {
		i_error("memcached: connect(%s, %u) failed: %m",
			net_ip2addr(&conn->dict->ip), conn->dict->port);
	} else {
		conn->dict->connected = TRUE;
	}
	if (conn->dict->ioloop != NULL)
		io_loop_stop(conn->dict->ioloop);
}

static const struct connection_settings memcached_conn_set = {
	.input_max_size = (size_t)-1,
	.output_max_size = (size_t)-1,
	.client = TRUE
};

static const struct connection_vfuncs memcached_conn_vfuncs = {
	.destroy = memcached_conn_destroy,
	.input = memcached_conn_input,
	.connected = memcached_conn_connected
};

static struct dict *
memcached_dict_init(struct dict *driver, const char *uri,
		    enum dict_data_type value_type ATTR_UNUSED,
		    const char *username ATTR_UNUSED,
		    const char *base_dir ATTR_UNUSED)
{
	struct memcached_dict *dict;
	const char *const *args;

	if (memcached_connections == NULL) {
		memcached_connections =
			connection_list_init(&memcached_conn_set,
					     &memcached_conn_vfuncs);
	}

	dict = i_new(struct memcached_dict, 1);
	if (net_addr2ip("127.0.0.1", &dict->ip) < 0)
		i_unreached();
	dict->port = MEMCACHED_DEFAULT_PORT;
	dict->timeout_msecs = MEMCACHED_DEFAULT_LOOKUP_TIMEOUT_MSECS;
	dict->key_prefix = i_strdup("");

	args = t_strsplit(uri, ":");
	for (; *args != NULL; args++) {
		if (strncmp(*args, "host=", 5) == 0) {
			if (net_addr2ip(*args+5, &dict->ip) < 0)
				i_error("Invalid IP: %s", *args+5);
		} else if (strncmp(*args, "port=", 5) == 0) {
			if (str_to_uint(*args+5, &dict->port) < 0)
				i_error("Invalid port: %s", *args+5);
		} else if (strncmp(*args, "prefix=", 7) == 0) {
			i_free(dict->key_prefix);
			dict->key_prefix = i_strdup(*args + 7);
		} else if (strncmp(*args, "timeout_msecs=", 14) == 0) {
			if (str_to_uint(*args+14, &dict->timeout_msecs) < 0)
				i_error("Invalid timeout_msecs: %s", *args+14);
		} else {
			i_error("Unknown parameter: %s", *args);
		}
	}
	connection_init_client_ip(memcached_connections, &dict->conn.conn,
				  &dict->ip, dict->port);

	dict->dict = *driver;
	dict->conn.cmd = buffer_create_dynamic(default_pool, 256);
	dict->conn.dict = dict;
	return &dict->dict;
}

static void memcached_dict_deinit(struct dict *_dict)
{
	struct memcached_dict *dict = (struct memcached_dict *)_dict;

	connection_deinit(&dict->conn.conn);
	buffer_free(&dict->conn.cmd);
	i_free(dict->key_prefix);
	i_free(dict);

	if (memcached_connections->connections == NULL)
		connection_list_deinit(&memcached_connections);
}

static void memcached_dict_lookup_timeout(struct memcached_dict *dict)
{
	i_error("memcached: Lookup timed out in %u.%03u secs",
		dict->timeout_msecs/1000, dict->timeout_msecs%1000);
	io_loop_stop(dict->ioloop);
}

static void memcached_add_header(buffer_t *buf, unsigned int key_len)
{
	uint32_t body_len = htonl(key_len);

	i_assert(key_len <= 0xffff);

	buffer_append_c(buf, MEMCACHED_REQUEST_HDR_MAGIC);
	buffer_append_c(buf, MEMCACHED_CMD_GET);
	buffer_append_c(buf, (key_len >> 8) & 0xff);
	buffer_append_c(buf, key_len & 0xff);
	buffer_append_c(buf, 0); /* extras length */
	buffer_append_c(buf, MEMCACHED_DATA_TYPE_RAW);
	buffer_append_zero(buf, 2); /* vbucket id - we probably don't care? */
	buffer_append(buf, &body_len, sizeof(body_len));
	buffer_append_zero(buf, 4+8); /* opaque + cas */
	i_assert(buf->used == MEMCACHED_REQUEST_HDR_LENGTH);
}

static int
memcached_dict_lookup_real(struct memcached_dict *dict, pool_t pool,
			   const char *key, const char **value_r)
{
	struct ioloop *prev_ioloop = current_ioloop;
	struct timeout *to;
	unsigned int key_len;

	if (strncmp(key, DICT_PATH_SHARED, strlen(DICT_PATH_SHARED)) == 0)
		key += strlen(DICT_PATH_SHARED);
	else {
		i_error("memcached: Only shared keys supported currently");
		return -1;
	}
	if (*dict->key_prefix != '\0')
		key = t_strconcat(dict->key_prefix, key, NULL);
	key_len = strlen(key);
	if (key_len > 0xffff) {
		i_error("memcached: Key is too long (%u bytes): %s",
			key_len, key);
		return -1;
	}

	i_assert(dict->ioloop == NULL);

	dict->ioloop = io_loop_create();
	connection_switch_ioloop(&dict->conn.conn);

	if (dict->conn.conn.fd_in == -1 &&
	    connection_client_connect(&dict->conn.conn) < 0) {
		i_error("memcached: Couldn't connect to %s:%u",
			net_ip2addr(&dict->ip), dict->port);
	} else {
		to = timeout_add(dict->timeout_msecs,
				 memcached_dict_lookup_timeout, dict);
		if (!dict->connected) {
			/* wait for connection */
			io_loop_run(dict->ioloop);
		}

		if (dict->connected) {
			buffer_set_used_size(dict->conn.cmd, 0);
			memcached_add_header(dict->conn.cmd, key_len);
			buffer_append(dict->conn.cmd, key, key_len);

			o_stream_nsend(dict->conn.conn.output,
				       dict->conn.cmd->data,
				       dict->conn.cmd->used);

			memset(&dict->conn.reply, 0, sizeof(dict->conn.reply));
			io_loop_run(dict->ioloop);
		}
		timeout_remove(&to);
	}

	current_ioloop = prev_ioloop;
	connection_switch_ioloop(&dict->conn.conn);
	current_ioloop = dict->ioloop;
	io_loop_destroy(&dict->ioloop);

	if (!dict->conn.reply.reply_received) {
		/* we failed in some way. make sure we disconnect since the
		   connection state isn't known anymore */
		memcached_conn_destroy(&dict->conn.conn);
		return -1;
	}
	switch (dict->conn.reply.status) {
	case MEMCACHED_RESPONSE_OK:
		*value_r = p_strndup(pool, dict->conn.reply.value,
				     dict->conn.reply.value_len);
		return 1;
	case MEMCACHED_RESPONSE_NOTFOUND:
		return 0;
	case MEMCACHED_RESPONSE_INTERNALERROR:
		i_error("memcached: Lookup(%s) failed: Internal error", key);
		return -1;
	case MEMCACHED_RESPONSE_BUSY:
		i_error("memcached: Lookup(%s) failed: Busy", key);
		return -1;
	case MEMCACHED_RESPONSE_TEMPFAILURE:
		i_error("memcached: Lookup(%s) failed: Temporary failure", key);
		return -1;
	}

	i_error("memcached: Lookup(%s) failed: Error code=%u",
		key, dict->conn.reply.status);
	return -1;
}

static int memcached_dict_lookup(struct dict *_dict, pool_t pool,
				 const char *key, const char **value_r)
{
	struct memcached_dict *dict = (struct memcached_dict *)_dict;
	int ret;

	if (pool->datastack_pool)
		ret = memcached_dict_lookup_real(dict, pool, key, value_r);
	else T_BEGIN {
		ret = memcached_dict_lookup_real(dict, pool, key, value_r);
	} T_END;
	return ret;
}

struct dict dict_driver_memcached = {
	.name = "memcached",
	{
		memcached_dict_init,
		memcached_dict_deinit,
		NULL,
		memcached_dict_lookup,
		NULL,
		NULL,
		NULL,
		NULL,
		NULL,
		NULL,
		NULL,
		NULL,
		NULL
	}
};
