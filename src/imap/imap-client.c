/* Copyright (c) 2002-2009 Dovecot authors, see the included COPYING file */

#include "imap-common.h"
#include "ioloop.h"
#include "llist.h"
#include "str.h"
#include "hostpid.h"
#include "network.h"
#include "istream.h"
#include "ostream.h"
#include "var-expand.h"
#include "master-service.h"
#include "imap-resp-code.h"
#include "imap-util.h"
#include "mail-namespace.h"
#include "imap-commands.h"

#include <stdlib.h>
#include <unistd.h>

extern struct mail_storage_callbacks mail_storage_callbacks;
struct imap_module_register imap_module_register = { 0 };

static struct client *my_client; /* we don't need more than one currently */

static void client_idle_timeout(struct client *client)
{
	if (client->output_lock == NULL)
		client_send_line(client, "* BYE Disconnected for inactivity.");
	client_destroy(client, "Disconnected for inactivity");
}

struct client *client_create(int fd_in, int fd_out, struct mail_user *user,
			     const struct imap_settings *set)
{
	struct client *client;
	struct mail_namespace *ns;
	const char *ident;

	/* always use nonblocking I/O */
	net_set_nonblock(fd_in, TRUE);
	net_set_nonblock(fd_out, TRUE);

	client = i_new(struct client, 1);
	client->set = set;
	client->fd_in = fd_in;
	client->fd_out = fd_out;
	client->input = i_stream_create_fd(fd_in,
					   set->imap_max_line_length, FALSE);
	client->output = o_stream_create_fd(fd_out, (size_t)-1, FALSE);

	o_stream_set_flush_callback(client->output, client_output, client);

	client->io = io_add(fd_in, IO_READ, client_input, client);
        client->last_input = ioloop_time;
	client->to_idle = timeout_add(CLIENT_IDLE_TIMEOUT_MSECS,
				      client_idle_timeout, client);

	client->command_pool =
		pool_alloconly_create(MEMPOOL_GROWING"client command", 1024*12);
	client->user = user;

	for (ns = user->namespaces; ns != NULL; ns = ns->next) {
		mail_storage_set_callbacks(ns->storage,
					   &mail_storage_callbacks, client);
	}

	client->capability_string =
		str_new(default_pool, sizeof(CAPABILITY_STRING)+32);
	str_append(client->capability_string, *set->imap_capability != '\0' ?
		   set->imap_capability : CAPABILITY_STRING);

	ident = mail_user_get_anvil_userip_ident(client->user);
	if (ident != NULL) {
		master_service_anvil_send(service, t_strconcat("CONNECT\t",
			my_pid, "\t", ident, "/imap\n", NULL));
		client->anvil_sent = TRUE;
	}

	i_assert(my_client == NULL);
	my_client = client;

	if (hook_client_created != NULL)
		hook_client_created(&client);
	return client;
}

void client_command_cancel(struct client_command_context **_cmd)
{
	struct client_command_context *cmd = *_cmd;
	bool cmd_ret;

	switch (cmd->state) {
	case CLIENT_COMMAND_STATE_WAIT_INPUT:
		/* a bit kludgy check: cancel command only if it has context
		   set. currently only append command matches this check. all
		   other commands haven't even started the processing yet. */
		if (cmd->context == NULL)
			break;
		/* fall through */
	case CLIENT_COMMAND_STATE_WAIT_OUTPUT:
		cmd->cancel = TRUE;
		break;
	case CLIENT_COMMAND_STATE_WAIT_UNAMBIGUITY:
	case CLIENT_COMMAND_STATE_WAIT_SYNC:
		/* commands haven't started yet */
		break;
	case CLIENT_COMMAND_STATE_DONE:
		i_unreached();
	}

	cmd_ret = !cmd->cancel || cmd->func == NULL ? TRUE : cmd->func(cmd);
	if (!cmd_ret && cmd->state != CLIENT_COMMAND_STATE_DONE) {
		if (cmd->client->output->closed)
			i_panic("command didn't cancel itself: %s", cmd->name);
	} else {
		client_command_free(_cmd);
	}
}

static const char *client_stats(struct client *client)
{
	static struct var_expand_table static_tab[] = {
		{ 'i', NULL, "input" },
		{ 'o', NULL, "output" },
		{ '\0', NULL, NULL }
	};
	struct var_expand_table *tab;
	string_t *str;

	tab = t_malloc(sizeof(static_tab));
	memcpy(tab, static_tab, sizeof(static_tab));

	tab[0].value = dec2str(client->input->v_offset);
	tab[1].value = dec2str(client->output->offset);

	str = t_str_new(128);
	var_expand(str, client->set->imap_logout_format, tab);
	return str_c(str);
}

static const char *client_get_disconnect_reason(struct client *client)
{
	errno = client->input->stream_errno != 0 ?
		client->input->stream_errno :
		client->output->stream_errno;
	return errno == 0 || errno == EPIPE ? "Connection closed" :
		t_strdup_printf("Connection closed: %m");
}

void client_destroy(struct client *client, const char *reason)
{
	struct client_command_context *cmd;

	i_assert(!client->destroyed);
	client->destroyed = TRUE;

	if (!client->disconnected) {
		client->disconnected = TRUE;
		if (reason == NULL)
			reason = client_get_disconnect_reason(client);
		i_info("%s %s", reason, client_stats(client));
	}

	i_stream_close(client->input);
	o_stream_close(client->output);

	/* finish off all the queued commands. */
	if (client->output_lock != NULL)
		client_command_cancel(&client->output_lock);
	while (client->command_queue != NULL) {
		cmd = client->command_queue;
		client_command_cancel(&cmd);
	}
	/* handle the input_lock command last. it might have been waiting on
	   other queued commands (although we probably should just drop the
	   command at that point since it hasn't started running. but this may
	   change in future). */
	if (client->input_lock != NULL)
		client_command_cancel(&client->input_lock);

	if (client->mailbox != NULL) {
		client_search_updates_free(client);
		mailbox_close(&client->mailbox);
	}
	if (client->anvil_sent) {
		master_service_anvil_send(service, t_strconcat("DISCONNECT\t",
			my_pid, "\t",
			mail_user_get_anvil_userip_ident(client->user), "/imap"
			"\n", NULL));
	}
	mail_user_unref(&client->user);

	if (client->free_parser != NULL)
		imap_parser_destroy(&client->free_parser);
	if (client->io != NULL)
		io_remove(&client->io);
	if (client->to_idle_output != NULL)
		timeout_remove(&client->to_idle_output);
	timeout_remove(&client->to_idle);

	i_stream_destroy(&client->input);
	o_stream_destroy(&client->output);

	if (close(client->fd_in) < 0)
		i_error("close(client in) failed: %m");
	if (client->fd_in != client->fd_out) {
		if (close(client->fd_out) < 0)
			i_error("close(client out) failed: %m");
	}

	if (array_is_created(&client->search_saved_uidset))
		array_free(&client->search_saved_uidset);
	if (array_is_created(&client->search_updates))
		array_free(&client->search_updates);
	str_free(&client->capability_string);
	pool_unref(&client->command_pool);
	i_free(client);

	/* quit the program */
	my_client = NULL;
	master_service_stop(service);
}

void client_disconnect(struct client *client, const char *reason)
{
	i_assert(reason != NULL);

	if (client->disconnected)
		return;

	i_info("Disconnected: %s %s", reason, client_stats(client));
	client->disconnected = TRUE;
	(void)o_stream_flush(client->output);

	i_stream_close(client->input);
	o_stream_close(client->output);
}

void client_disconnect_with_error(struct client *client, const char *msg)
{
	client_send_line(client, t_strconcat("* BYE ", msg, NULL));
	client_disconnect(client, msg);
}

int client_send_line(struct client *client, const char *data)
{
	struct const_iovec iov[2];

	if (client->output->closed)
		return -1;

	iov[0].iov_base = data;
	iov[0].iov_len = strlen(data);
	iov[1].iov_base = "\r\n";
	iov[1].iov_len = 2;

	if (o_stream_sendv(client->output, iov, 2) < 0)
		return -1;
	client->last_output = ioloop_time;

	if (o_stream_get_buffer_used_size(client->output) >=
	    CLIENT_OUTPUT_OPTIMAL_SIZE) {
		/* buffer full, try flushing */
		return o_stream_flush(client->output);
	}
	return 1;
}

void client_send_tagline(struct client_command_context *cmd, const char *data)
{
	struct client *client = cmd->client;
	const char *tag = cmd->tag;

	if (client->output->closed || cmd->cancel)
		return;

	if (tag == NULL || *tag == '\0')
		tag = "*";

	(void)o_stream_send_str(client->output, tag);
	(void)o_stream_send(client->output, " ", 1);
	(void)o_stream_send_str(client->output, data);
	(void)o_stream_send(client->output, "\r\n", 2);

	client->last_output = ioloop_time;
}

void client_send_command_error(struct client_command_context *cmd,
			       const char *msg)
{
	struct client *client = cmd->client;
	const char *error, *cmd_name;
	bool fatal;

	if (msg == NULL) {
		msg = imap_parser_get_error(cmd->parser, &fatal);
		if (fatal) {
			client_disconnect_with_error(client, msg);
			return;
		}
	}

	if (cmd->tag == NULL)
		error = t_strconcat("BAD Error in IMAP tag: ", msg, NULL);
	else if (cmd->name == NULL)
		error = t_strconcat("BAD Error in IMAP command: ", msg, NULL);
	else {
		cmd_name = t_str_ucase(cmd->name);
		error = t_strconcat("BAD Error in IMAP command ",
				    cmd_name, ": ", msg, NULL);
	}

	client_send_tagline(cmd, error);

	if (++client->bad_counter >= CLIENT_MAX_BAD_COMMANDS) {
		client_disconnect_with_error(client,
			"Too many invalid IMAP commands.");
	}

	cmd->param_error = TRUE;
	/* client_read_args() failures rely on this being set, so that the
	   command processing is stopped even while command function returns
	   FALSE. */
	cmd->state = CLIENT_COMMAND_STATE_DONE;
}

bool client_read_args(struct client_command_context *cmd, unsigned int count,
		      unsigned int flags, const struct imap_arg **args_r)
{
	string_t *str;
	int ret;

	i_assert(count <= INT_MAX);

	ret = imap_parser_read_args(cmd->parser, count, flags, args_r);
	if (ret >= (int)count) {
		/* all parameters read successfully */
		i_assert(cmd->client->input_lock == NULL ||
			 cmd->client->input_lock == cmd);

		str = t_str_new(256);
		imap_write_args(str, *args_r);
		cmd->args = p_strdup(cmd->pool, str_c(str));

		cmd->client->input_lock = NULL;
		return TRUE;
	} else if (ret == -2) {
		/* need more data */
		if (cmd->client->input->closed) {
			/* disconnected */
			cmd->state = CLIENT_COMMAND_STATE_DONE;
		}
		return FALSE;
	} else {
		/* error, or missing arguments */
		client_send_command_error(cmd, ret < 0 ? NULL :
					  "Missing arguments");
		return FALSE;
	}
}

bool client_read_string_args(struct client_command_context *cmd,
			     unsigned int count, ...)
{
	const struct imap_arg *imap_args;
	va_list va;
	const char *str;
	unsigned int i;

	if (!client_read_args(cmd, count, 0, &imap_args))
		return FALSE;

	va_start(va, count);
	for (i = 0; i < count; i++) {
		const char **ret = va_arg(va, const char **);

		if (imap_args[i].type == IMAP_ARG_EOL) {
			client_send_command_error(cmd, "Missing arguments.");
			break;
		}

		str = imap_arg_string(&imap_args[i]);
		if (str == NULL) {
			client_send_command_error(cmd, "Invalid arguments.");
			break;
		}

		if (ret != NULL)
			*ret = str;
	}
	va_end(va);

	return i == count;
}

static struct client_command_context *
client_command_find_with_flags(struct client_command_context *new_cmd,
			       enum command_flags flags)
{
	struct client_command_context *cmd;

	cmd = new_cmd->client->command_queue;
	for (; cmd != NULL; cmd = cmd->next) {
		if (cmd != new_cmd && (cmd->cmd_flags & flags) != 0)
			return cmd;
	}
	return NULL;
}

static bool client_command_check_ambiguity(struct client_command_context *cmd)
{
	enum command_flags flags;
	bool broken_client = FALSE;

	if ((cmd->cmd_flags & COMMAND_FLAG_BREAKS_MAILBOX) ==
	    COMMAND_FLAG_BREAKS_MAILBOX) {
		/* there must be no other command running that uses the
		   selected mailbox */
		flags = COMMAND_FLAG_USES_MAILBOX;
	} else if ((cmd->cmd_flags & COMMAND_FLAG_USES_SEQS) != 0) {
		/* no existing command must be breaking sequences */
		flags = COMMAND_FLAG_BREAKS_SEQS;
		broken_client = TRUE;
	} else if ((cmd->cmd_flags & COMMAND_FLAG_BREAKS_SEQS) != 0) {
		/* if existing command uses sequences, we'll have to block */
		flags = COMMAND_FLAG_USES_SEQS;
	} else {
		return FALSE;
	}

	if (client_command_find_with_flags(cmd, flags) == NULL) {
		if (cmd->client->syncing) {
			/* don't do anything until syncing is finished */
			return TRUE;
		}
		if (cmd->client->mailbox_change_lock != NULL &&
		    cmd->client->mailbox_change_lock != cmd) {
			/* don't do anything until mailbox is fully
			   opened/closed */
			return TRUE;
		}
		return FALSE;
	}

	if (broken_client) {
		client_send_line(cmd->client,
				 "* BAD ["IMAP_RESP_CODE_CLIENTBUG"] "
				 "Command pipelining results in ambiguity.");
	}

	return TRUE;
}

static struct client_command_context *
client_command_new(struct client *client)
{
	struct client_command_context *cmd;

	cmd = p_new(client->command_pool, struct client_command_context, 1);
	cmd->client = client;
	cmd->pool = client->command_pool;
	p_array_init(&cmd->module_contexts, cmd->pool, 5);

	if (client->free_parser != NULL) {
		cmd->parser = client->free_parser;
		client->free_parser = NULL;
	} else {
		cmd->parser =
			imap_parser_create(client->input, client->output,
					   client->set->imap_max_line_length);
	}

	DLLIST_PREPEND(&client->command_queue, cmd);
	client->command_queue_size++;

	return cmd;
}

void client_command_free(struct client_command_context **_cmd)
{
	struct client_command_context *cmd = *_cmd;
	struct client *client = cmd->client;

	*_cmd = NULL;

	/* reset input idle time because command output might have taken a
	   long time and we don't want to disconnect client immediately then */
	client->last_input = ioloop_time;
	timeout_reset(client->to_idle);

	if (cmd->cancel) {
		cmd->cancel = FALSE;
		client_send_tagline(cmd, "NO Command cancelled.");
	}

	if (!cmd->param_error)
		client->bad_counter = 0;

	if (client->input_lock == cmd)
		client->input_lock = NULL;
	if (client->output_lock == cmd)
		client->output_lock = NULL;
	if (client->mailbox_change_lock == cmd)
		client->mailbox_change_lock = NULL;

	if (client->free_parser != NULL)
		imap_parser_destroy(&cmd->parser);
	else {
		imap_parser_reset(cmd->parser);
		client->free_parser = cmd->parser;
	}

	client->command_queue_size--;
	DLLIST_REMOVE(&client->command_queue, cmd);
	cmd = NULL;

	if (client->command_queue == NULL) {
		/* no commands left in the queue, we can clear the pool */
		p_clear(client->command_pool);
		if (client->to_idle_output != NULL)
			timeout_remove(&client->to_idle_output);
	}
}

static void client_add_missing_io(struct client *client)
{
	if (client->io == NULL && !client->disconnected) {
		client->io = io_add(client->fd_in,
				    IO_READ, client_input, client);
	}
}

void client_continue_pending_input(struct client **_client)
{
	struct client *client = *_client;
	size_t size;

	i_assert(!client->handling_input);

	if (client->disconnected) {
		if (!client->destroyed)
			client_destroy(client, NULL);
		*_client = NULL;
		return;
	}

	if (client->input_lock != NULL) {
		/* there's a command that has locked the input */
		struct client_command_context *cmd = client->input_lock;

		if (cmd->state != CLIENT_COMMAND_STATE_WAIT_UNAMBIGUITY)
			return;

		/* the command is waiting for existing ambiguity causing
		   commands to finish. */
		if (client_command_check_ambiguity(cmd))
			return;
		cmd->state = CLIENT_COMMAND_STATE_WAIT_INPUT;
	}

	client_add_missing_io(client);

	/* if there's unread data in buffer, handle it. */
	(void)i_stream_get_data(client->input, &size);
	if (size > 0)
		(void)client_handle_input(client);
}

/* Skip incoming data until newline is found,
   returns TRUE if newline was found. */
static bool client_skip_line(struct client *client)
{
	const unsigned char *data;
	size_t i, data_size;

	data = i_stream_get_data(client->input, &data_size);

	for (i = 0; i < data_size; i++) {
		if (data[i] == '\n') {
			client->input_skip_line = FALSE;
			i++;
			break;
		}
	}

	i_stream_skip(client->input, i);
	return !client->input_skip_line;
}

static void client_idle_output_timeout(struct client *client)
{
	client_destroy(client,
		       "Disconnected for inactivity in reading our output");
}

bool client_handle_unfinished_cmd(struct client_command_context *cmd)
{
	if (cmd->state == CLIENT_COMMAND_STATE_WAIT_INPUT) {
		/* need more input */
		return FALSE;
	}
	if (cmd->state != CLIENT_COMMAND_STATE_WAIT_OUTPUT) {
		/* waiting for something */
		if (cmd->state == CLIENT_COMMAND_STATE_WAIT_SYNC) {
			/* this is mainly for APPEND. */
			client_add_missing_io(cmd->client);
		}
		return TRUE;
	}

	/* output is blocking, we can execute more commands */
	o_stream_set_flush_pending(cmd->client->output, TRUE);
	if (cmd->client->to_idle_output == NULL) {
		/* disconnect sooner if client isn't reading our output */
		cmd->client->to_idle_output =
			timeout_add(CLIENT_OUTPUT_TIMEOUT_MSECS,
				    client_idle_output_timeout, cmd->client);
	}
	return TRUE;
}

static bool client_command_input(struct client_command_context *cmd)
{
	struct client *client = cmd->client;
	struct command *command;

        if (cmd->func != NULL) {
		/* command is being executed - continue it */
		if (cmd->func(cmd) || cmd->state == CLIENT_COMMAND_STATE_DONE) {
			/* command execution was finished */
			client_command_free(&cmd);
			client_add_missing_io(client);
			return TRUE;
		}

		return client_handle_unfinished_cmd(cmd);
	}

	if (cmd->tag == NULL) {
                cmd->tag = imap_parser_read_word(cmd->parser);
		if (cmd->tag == NULL)
			return FALSE; /* need more data */
		cmd->tag = p_strdup(cmd->pool, cmd->tag);
	}

	if (cmd->name == NULL) {
		cmd->name = imap_parser_read_word(cmd->parser);
		if (cmd->name == NULL)
			return FALSE; /* need more data */
		cmd->name = p_strdup(cmd->pool, cmd->name);
	}

	client->input_skip_line = TRUE;

	if (cmd->name == '\0') {
		/* command not given - cmd_func is already NULL. */
	} else if ((command = command_find(cmd->name)) != NULL) {
		cmd->func = command->func;
		cmd->cmd_flags = command->flags;
		if (client_command_check_ambiguity(cmd)) {
			/* do nothing until existing commands are finished */
			i_assert(cmd->state == CLIENT_COMMAND_STATE_WAIT_INPUT);
			cmd->state = CLIENT_COMMAND_STATE_WAIT_UNAMBIGUITY;
			io_remove(&client->io);
			return FALSE;
		}
	}

	if (cmd->func == NULL) {
		/* unknown command */
		client_send_command_error(cmd, "Unknown command.");
		cmd->param_error = TRUE;
		client_command_free(&cmd);
		return TRUE;
	} else {
		i_assert(!client->disconnected);

		return client_command_input(cmd);
	}
}

static bool client_handle_next_command(struct client *client, bool *remove_io_r)
{
	size_t size;

	*remove_io_r = FALSE;

	if (client->input_lock != NULL) {
		if (client->input_lock->state ==
		    CLIENT_COMMAND_STATE_WAIT_UNAMBIGUITY) {
			*remove_io_r = TRUE;
			return FALSE;
		}
		return client_command_input(client->input_lock);
	}

	if (client->input_skip_line) {
		/* first eat the previous command line */
		if (!client_skip_line(client))
			return FALSE;
		client->input_skip_line = FALSE;
	}

	/* don't bother creating a new client command before there's at least
	   some input */
	(void)i_stream_get_data(client->input, &size);
	if (size == 0)
		return FALSE;

	/* beginning a new command */
	if (client->command_queue_size >= CLIENT_COMMAND_QUEUE_MAX_SIZE ||
	    client->output_lock != NULL) {
		/* wait for some of the commands to finish */
		*remove_io_r = TRUE;
		return FALSE;
	}

	client->input_lock = client_command_new(client);
	return client_command_input(client->input_lock);
}

bool client_handle_input(struct client *client)
{
	bool ret, remove_io, handled_commands = FALSE;

	client->handling_input = TRUE;
	do {
		T_BEGIN {
			ret = client_handle_next_command(client, &remove_io);
		} T_END;
		if (ret)
			handled_commands = TRUE;
	} while (ret && !client->disconnected && client->io != NULL);
	client->handling_input = FALSE;

	if (client->output->closed) {
		client_destroy(client, NULL);
		return TRUE;
	} else {
		if (remove_io)
			io_remove(&client->io);
		else
			client_add_missing_io(client);
		if (!handled_commands)
			return FALSE;

		ret = cmd_sync_delayed(client);
		if (ret)
			client_continue_pending_input(&client);
		return TRUE;
	}
}

void client_input(struct client *client)
{
	struct client_command_context *cmd;
	struct ostream *output = client->output;
	ssize_t bytes;

	i_assert(client->io != NULL);

	client->last_input = ioloop_time;
	timeout_reset(client->to_idle);

	bytes = i_stream_read(client->input);
	if (bytes == -1) {
		/* disconnected */
		client_destroy(client, NULL);
		return;
	}

	o_stream_ref(output);
	o_stream_cork(output);
	if (!client_handle_input(client) && bytes == -2) {
		/* parameter word is longer than max. input buffer size.
		   this is most likely an error, so skip the new data
		   until newline is found. */
		client->input_skip_line = TRUE;

		cmd = client->input_lock != NULL ? client->input_lock :
			client_command_new(client);
		cmd->param_error = TRUE;
		client_send_command_error(cmd, "Too long argument.");
		client_command_free(&cmd);
	}
	o_stream_uncork(output);
	o_stream_unref(&output);
}

static void client_output_cmd(struct client_command_context *cmd)
{
	bool finished;

	/* continue processing command */
	finished = cmd->func(cmd) || cmd->state == CLIENT_COMMAND_STATE_DONE;

	if (!finished)
		(void)client_handle_unfinished_cmd(cmd);
	else {
		/* command execution was finished */
		client_command_free(&cmd);
	}
}

int client_output(struct client *client)
{
	struct client_command_context *cmd;
	int ret;

	i_assert(!client->destroyed);

	client->last_output = ioloop_time;
	timeout_reset(client->to_idle);
	if (client->to_idle_output != NULL)
		timeout_reset(client->to_idle_output);

	if ((ret = o_stream_flush(client->output)) < 0) {
		client_destroy(client, NULL);
		return 1;
	}

	/* mark all commands non-executed */
	for (cmd = client->command_queue; cmd != NULL; cmd = cmd->next)
		cmd->temp_executed = FALSE;

	o_stream_cork(client->output);
	if (client->output_lock != NULL) {
		client->output_lock->temp_executed = TRUE;
		client_output_cmd(client->output_lock);
	}
	while (client->output_lock == NULL) {
		/* go through the entire commands list every time in case
		   multiple commands were freed. temp_executed keeps track of
		   which messages we've called so far */
		cmd = client->command_queue;
		for (; cmd != NULL; cmd = cmd->next) {
			if (!cmd->temp_executed &&
			    cmd->state == CLIENT_COMMAND_STATE_WAIT_OUTPUT) {
				cmd->temp_executed = TRUE;
				client_output_cmd(cmd);
				break;
			}
		}
		if (cmd == NULL) {
			/* all commands executed */
			break;
		}
	}

	if (client->output->closed) {
		client_destroy(client, NULL);
		return 1;
	} else {
		(void)cmd_sync_delayed(client);
		o_stream_uncork(client->output);
		client_continue_pending_input(&client);
		return ret;
	}
}

bool client_handle_search_save_ambiguity(struct client_command_context *cmd)
{
	struct client_command_context *old_cmd = cmd->next;

	/* search only commands that were added before this command
	   (commands are prepended to the queue, so they're after ourself) */
	for (; old_cmd != NULL; old_cmd = old_cmd->next) {
		if (old_cmd->search_save_result)
			break;
	}
	if (old_cmd == NULL)
		return FALSE;

	/* ambiguity, wait until it's over */
	i_assert(cmd->state == CLIENT_COMMAND_STATE_WAIT_INPUT);
	cmd->client->input_lock = cmd;
	cmd->state = CLIENT_COMMAND_STATE_WAIT_UNAMBIGUITY;
	io_remove(&cmd->client->io);
	return TRUE;
}

void client_enable(struct client *client, enum mailbox_feature features)
{
	struct mailbox_status status;

	if ((client->enabled_features & features) == features)
		return;

	client->enabled_features |= features;
	if (client->mailbox == NULL)
		return;

	mailbox_enable(client->mailbox, features);
	if ((features & MAILBOX_FEATURE_CONDSTORE) != 0) {
		/* CONDSTORE being enabled while mailbox is selected.
		   Notify client of the latest HIGHESTMODSEQ. */
		mailbox_get_status(client->mailbox,
				   STATUS_HIGHESTMODSEQ, &status);
		client_send_line(client, t_strdup_printf(
			"* OK [HIGHESTMODSEQ %llu]",
			(unsigned long long)status.highest_modseq));
	}
}

struct imap_search_update *
client_search_update_lookup(struct client *client, const char *tag,
			    unsigned int *idx_r)
{
	struct imap_search_update *updates;
	unsigned int i, count;

	if (!array_is_created(&client->search_updates))
		return NULL;

	updates = array_get_modifiable(&client->search_updates, &count);
	for (i = 0; i < count; i++) {
		if (strcmp(updates[i].tag, tag) == 0) {
			*idx_r = i;
			return &updates[i];
		}
	}
	return NULL;
}

void client_search_updates_free(struct client *client)
{
	struct imap_search_update *updates;
	unsigned int i, count;

	if (!array_is_created(&client->search_updates))
		return;

	updates = array_get_modifiable(&client->search_updates, &count);
	for (i = 0; i < count; i++) {
		i_free(updates[i].tag);
		mailbox_search_result_free(&updates[i].result);
	}
	array_clear(&client->search_updates);
}

void clients_init(void)
{
	my_client = NULL;
}

void clients_deinit(void)
{
	if (my_client != NULL) {
		client_send_line(my_client, "* BYE Server shutting down.");
		client_destroy(my_client, "Server shutting down");
	}
}
