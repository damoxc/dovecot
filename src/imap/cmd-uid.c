/* Copyright (c) 2002-2007 Dovecot authors, see the included COPYING file */

#include "common.h"
#include "commands.h"

bool cmd_uid(struct client_command_context *cmd)
{
	struct command *command;
	const char *cmd_name;

	/* UID <command> <args> */
	cmd_name = imap_parser_read_word(cmd->parser);
	if (cmd_name == NULL)
		return FALSE;

	command = command_find(t_strconcat("UID ", cmd_name, NULL));
	if (command == NULL) {
		client_send_tagline(cmd, t_strconcat(
			"BAD Unknown UID command ", cmd_name, NULL));
		return TRUE;
	}

	cmd->cmd_flags = command->flags;
	cmd->func = command->func;
	cmd->uid = TRUE;
	return cmd->func(cmd);
}
