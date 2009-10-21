/* Copyright (c) 2002-2009 Dovecot authors, see the included COPYING file */

#include "pop3-common.h"
#include "ioloop.h"
#include "buffer.h"
#include "istream.h"
#include "ostream.h"
#include "base64.h"
#include "restrict-access.h"
#include "master-service.h"
#include "master-login.h"
#include "master-interface.h"
#include "var-expand.h"
#include "mail-storage-service.h"

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#define IS_STANDALONE() \
        (getenv(MASTER_UID_ENV) == NULL)

static struct mail_storage_service_ctx *storage_service;
static struct master_login *master_login = NULL;

void (*hook_client_created)(struct client **client) = NULL;

static void client_add_input(struct client *client, const buffer_t *buf)
{
	struct ostream *output;

	if (buf != NULL && buf->used > 0) {
		if (!i_stream_add_data(client->input, buf->data, buf->used))
			i_panic("Couldn't add client input to stream");
	}

	output = client->output;
	o_stream_ref(output);
	o_stream_cork(output);
	if (!IS_STANDALONE())
		client_send_line(client, "+OK Logged in.");
	(void)client_handle_input(client);
	o_stream_uncork(output);
	o_stream_unref(&output);
}

static void
main_stdio_init_user(const struct pop3_settings *set,
		     struct mail_user *mail_user,
		     struct mail_storage_service_user *user)
{
	struct client *client;
	buffer_t *input_buf;
	const char *input_base64;

	input_base64 = getenv("CLIENT_INPUT");
	input_buf = input_base64 == NULL ? NULL :
		t_base64_decode_str(input_base64);

	client = client_create(STDIN_FILENO, STDOUT_FILENO,
			       mail_user, user, set);
	client_add_input(client, input_buf);
}

static void main_stdio_run(void)
{
	struct mail_storage_service_input input;
	struct mail_user *mail_user;
	const struct pop3_settings *set;
	const char *value;
	struct mail_storage_service_user *user;
	const char *error;
	pool_t user_pool;
	int ret;

	memset(&input, 0, sizeof(input));
	input.module = input.service = "pop3";
	input.username = getenv("USER");
	if (input.username == NULL && IS_STANDALONE())
		input.username = getlogin();
	if (input.username == NULL)
		i_fatal("USER environment missing");
	if ((value = getenv("IP")) != NULL)
		net_addr2ip(value, &input.remote_ip);
	if ((value = getenv("LOCAL_IP")) != NULL)
		net_addr2ip(value, &input.local_ip);

	user_pool = pool_alloconly_create("storage user pool", 512);
	ret = mail_storage_service_lookup(storage_service, &input,
					  &user, &error);
	if (ret <= 0)
		i_fatal("User lookup failed: %s", error);
	if (mail_storage_service_next(storage_service,
				      user, &mail_user, &error) < 0)
		i_fatal("User init failed: %s", error);
	set = mail_storage_service_user_get_set(user)[1];

	restrict_access_allow_coredumps(TRUE);
	if (set->shutdown_clients)
		master_service_set_die_with_master(master_service, TRUE);

	/* fake that we're running, so we know if client was destroyed
	   while handling its initial input */
	io_loop_set_running(current_ioloop);
	main_stdio_init_user(set, mail_user, user);
}

static void
login_client_connected(const struct master_login_client *client,
		       const char *username, const char *const *extra_fields)
{
	struct mail_storage_service_input input;
	struct mail_storage_service_user *user;
	struct mail_user *mail_user;
	struct client *pop3_client;
	const struct pop3_settings *set;
	const char *error;
	buffer_t input_buf;

	if (pop3_clients != NULL) {
		i_error("Can't handle more than one connection currently");
		(void)close(client->fd);
		return;
	}

	memset(&input, 0, sizeof(input));
	input.module = input.service = "pop3";
	input.local_ip = client->auth_req.local_ip;
	input.remote_ip = client->auth_req.remote_ip;
	input.username = username;
	input.userdb_fields = extra_fields;

	if (input.username == NULL) {
		i_error("login client: Username missing from auth reply");
		(void)close(client->fd);
		return;
	}
	master_login_deinit(&master_login);

	if (mail_storage_service_lookup_next(storage_service, &input,
					     &user, &mail_user, &error) <= 0)
		i_fatal("%s", error);
	set = mail_storage_service_user_get_set(user)[1];

	restrict_access_allow_coredumps(TRUE);
	if (set->shutdown_clients)
		master_service_set_die_with_master(master_service, TRUE);

	/* fake that we're running, so we know if client was destroyed
	   while handling its initial input */
	io_loop_set_running(current_ioloop);

	buffer_create_const_data(&input_buf, client->data,
				 client->auth_req.data_size);
	pop3_client = client_create(client->fd, client->fd, mail_user,
				    user, set);
	T_BEGIN {
		client_add_input(pop3_client, &input_buf);
	} T_END;
}

static void client_connected(const struct master_service_connection *conn)
{
	if (master_login == NULL) {
		/* running standalone, we shouldn't even get here */
		(void)close(conn->fd);
	} else {
		master_login_add(master_login, conn->fd);
	}
}

int main(int argc, char *argv[])
{
	static const struct setting_parser_info *set_roots[] = {
		&pop3_setting_parser_info,
		NULL
	};
	enum master_service_flags service_flags = 0;
	enum mail_storage_service_flags storage_service_flags = 0;

	if (IS_STANDALONE() && getuid() == 0 &&
	    net_getpeername(1, NULL, NULL) == 0) {
		printf("-ERR pop3 binary must not be started from "
		       "inetd, use pop3-login instead.\n");
		return 1;
	}

	if (IS_STANDALONE()) {
		service_flags |= MASTER_SERVICE_FLAG_STANDALONE |
			MASTER_SERVICE_FLAG_STD_CLIENT;
	} else {
		storage_service_flags |=
			MAIL_STORAGE_SERVICE_FLAG_DISALLOW_ROOT;
	}

	master_service = master_service_init("pop3", service_flags,
					     &argc, &argv, NULL);
	if (master_getopt(master_service) > 0)
		return FATAL_DEFAULT;
	master_service_init_finish(master_service);

	storage_service =
		mail_storage_service_init(master_service,
					  set_roots, storage_service_flags);

	if (IS_STANDALONE()) {
		T_BEGIN {
			main_stdio_run();
		} T_END;
	} else {
		master_login = master_login_init("auth-master",
						 login_client_connected);
		io_loop_set_running(current_ioloop);
	}

	if (io_loop_is_running(current_ioloop))
		master_service_run(master_service, client_connected);
	clients_destroy_all();

	if (master_login != NULL)
		master_login_deinit(&master_login);
	mail_storage_service_deinit(&storage_service);
	master_service_deinit(&master_service);
	return 0;
}
