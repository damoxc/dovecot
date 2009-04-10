/* Copyright (c) 2006-2009 Dovecot authors, see the included COPYING file */

#include "lib.h"
#include "deliver.h"
#include "master-service.h"
#include "smtp-client.h"

#include <unistd.h>
#include <sys/wait.h>

struct smtp_client {
	FILE *f;
	pid_t pid;
};

static struct smtp_client *smtp_client_devnull(FILE **file_r)
{
	struct smtp_client *client;

	client = i_new(struct smtp_client, 1);
	client->f = *file_r = fopen("/dev/null", "w");
	if (client->f == NULL)
		i_fatal("fopen() failed: %m");
	client->pid = (pid_t)-1;
	return client;
}

static void ATTR_NORETURN
smtp_client_run_sendmail(const char *destination,
			 const char *return_path, int fd)
{
	const char *argv[7], *sendmail_path;

	/* deliver_set's contents may point to environment variables.
	   deliver_env_clean() cleans them up, so they have to be copied. */
	sendmail_path = t_strdup(deliver_set->sendmail_path);

	argv[0] = sendmail_path;
	argv[1] = "-i"; /* ignore dots */
	argv[2] = "-f";
	argv[3] = return_path != NULL && *return_path != '\0' ?
		return_path : "<>";
	argv[4] = "--";
	argv[5] = destination;
	argv[6] = NULL;

	if (dup2(fd, STDIN_FILENO) < 0)
		i_fatal("dup2() failed: %m");

	master_service_env_clean(TRUE);

	(void)execv(sendmail_path, (void *)argv);
	i_fatal("execv(%s) failed: %m", sendmail_path);
}

struct smtp_client *smtp_client_open(const char *destination,
				     const char *return_path, FILE **file_r)
{
	struct smtp_client *client;
	int fd[2];
	pid_t pid;

	if (pipe(fd) < 0) {
		i_error("pipe() failed: %m");
		return smtp_client_devnull(file_r);
	}

	if ((pid = fork()) == (pid_t)-1) {
		i_error("fork() failed: %m");
		(void)close(fd[0]); (void)close(fd[1]);
		return smtp_client_devnull(file_r);
	}
	if (pid == 0) {
		/* child */
		(void)close(fd[1]);
		smtp_client_run_sendmail(destination, return_path, fd[0]);
	}
	(void)close(fd[0]);

	client = i_new(struct smtp_client, 1);
	client->f = *file_r = fdopen(fd[1], "w");
	if (client->f == NULL)
		i_fatal("fdopen() failed: %m");
	return client;
}

int smtp_client_close(struct smtp_client *client)
{
	int ret = EX_TEMPFAIL, status;

	fclose(client->f);
	if (client->pid == (pid_t)-1) {
		/* smtp_client_open() failed already */
	} else if (waitpid(client->pid, &status, 0) < 0)
		i_error("waitpid() failed: %m");
	else if (WIFEXITED(status)) {
		ret = WEXITSTATUS(status);
		if (ret != 0) {
			i_error("Sendmail process terminated abnormally, "
				"exit status %d", ret);
		}
	} else if (WIFSIGNALED(status)) {
		i_error("Sendmail process terminated abnormally, "
				"signal %d", WTERMSIG(status));
	} else if (WIFSTOPPED(status)) {
		i_error("Sendmail process stopped, signal %d",
			WSTOPSIG(status));
	} else {
		i_error("Sendmail process terminated abnormally, "
			"return status %d", status);
	}

	i_free(client);
	return ret;
}
