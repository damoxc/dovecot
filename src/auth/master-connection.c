/* Copyright (C) 2002 Timo Sirainen */

#include "common.h"
#include "buffer.h"
#include "hash.h"
#include "ioloop.h"
#include "ostream.h"
#include "network.h"
#include "mech.h"
#include "userdb.h"
#include "login-connection.h"
#include "master-connection.h"
#include "auth-master-interface.h"

#define MAX_OUTBUF_SIZE (1024*50)

static struct auth_master_reply failure_reply;

static struct ostream *output;
static struct io *io_master;

static unsigned int master_pos;
static char master_buf[sizeof(struct auth_master_request)];

static size_t reply_add(buffer_t *buf, const char *str)
{
	size_t index;

	if (str == NULL || *str == '\0')
		return (size_t)-1;

	index = buffer_get_used_size(buf) - sizeof(struct auth_master_reply);
	buffer_append(buf, str, strlen(str)+1);
	return index;
}

static struct auth_master_reply *
fill_reply(const struct user_data *user, size_t *reply_size)
{
	struct auth_master_reply reply, *reply_p;
	buffer_t *buf;
	char *p;

	buf = buffer_create_dynamic(data_stack_pool,
				    sizeof(reply) + 256, (size_t)-1);
	memset(&reply, 0, sizeof(reply));
	buffer_append(buf, &reply, sizeof(reply));

	reply.success = TRUE;

	reply.uid = user->uid;
	reply.gid = user->gid;

	reply.system_user_idx = reply_add(buf, user->system_user);
	reply.virtual_user_idx = reply_add(buf, user->virtual_user);
	reply.mail_idx = reply_add(buf, user->mail);

	p = strstr(user->home, "/./");
	if (p == NULL) {
		reply.home_idx = reply_add(buf, user->home);
		reply.chroot_idx = reply_add(buf, NULL);
	} else {
		/* wu-ftpd like <chroot>/./<home> */
		reply.chroot_idx =
			reply_add(buf, t_strdup_until(user->home, p));
		reply.home_idx = reply_add(buf, p + 3);
	}

	*reply_size = buffer_get_used_size(buf);
	reply.data_size = *reply_size - sizeof(reply);

	reply_p = buffer_get_space_unsafe(buf, 0, sizeof(reply));
	*reply_p = reply;

	return reply_p;
}

static void send_reply(struct auth_master_reply *reply, size_t reply_size,
		       unsigned int tag)
{
	ssize_t ret;

	reply->tag = tag;
	for (;;) {
		ret = o_stream_send(output, reply, reply_size);
		if (ret < 0) {
			/* master died, kill ourself too */
			io_loop_stop(ioloop);
			break;
		}

		if ((size_t)ret == reply_size)
			break;

		/* buffer full, we have to block */
		i_warning("Master transmit buffer full, blocking..");
		if (o_stream_flush(output) < 0) {
			/* transmit error, probably master died */
			io_loop_stop(ioloop);
			break;
		}
	}
}

static void userdb_callback(struct user_data *user, void *context)
{
	unsigned int tag = POINTER_CAST_TO(context, unsigned int);
	struct auth_master_reply *reply;
	size_t reply_size;

	if (user == NULL)
		send_reply(&failure_reply, sizeof(failure_reply), tag);
	else {
		reply = fill_reply(user, &reply_size);
		send_reply(reply, reply_size, tag);
	}
}

static void master_handle_request(struct auth_master_request *request)
{
	struct login_connection *login_conn;
	struct auth_request *auth_request;

	login_conn = login_connection_lookup(request->login_pid);
	auth_request = login_conn == NULL ? NULL :
		hash_lookup(login_conn->auth_requests,
			    POINTER_CAST(request->id));

	if (auth_request == NULL) {
		if (verbose) {
			i_info("Master request %u.%u not found",
			       request->login_pid, request->id);
		}
		send_reply(&failure_reply, sizeof(failure_reply), request->tag);
	} else {
		userdb->lookup(auth_request->user, userdb_callback,
			       POINTER_CAST(request->tag));
		mech_request_free(login_conn, auth_request, request->id);
	}
}

static void master_input(void *context __attr_unused__)
{
	int ret;

	ret = net_receive(MASTER_SOCKET_FD, master_buf + master_pos,
			  sizeof(master_buf) - master_pos);
	if (ret < 0) {
		/* master died, kill ourself too */
		io_loop_stop(ioloop);
		return;
	}

	master_pos += ret;
	if (master_pos < sizeof(master_buf))
		return;

	/* reply is now read */
	master_handle_request((struct auth_master_request *) master_buf);
	master_pos = 0;
}

void master_connection_init(void)
{
	memset(&failure_reply, 0, sizeof(failure_reply));

	master_pos = 0;
	output = o_stream_create_file(MASTER_SOCKET_FD, default_pool,
				      MAX_OUTBUF_SIZE, IO_PRIORITY_DEFAULT,
				      FALSE);
	io_master = io_add(MASTER_SOCKET_FD, IO_READ, master_input, NULL);

	/* just a note to master that we're ok. if we die before,
	   master should shutdown itself. */
	o_stream_send(output, "O", 1);
}

void master_connection_deinit(void)
{
	o_stream_unref(output);
	io_remove(io_master);
}
