/* Copyright (c) 2006-2013 Dovecot authors, see the included COPYING file */

#include "lib.h"
#include "ioloop.h"
#include "array.h"
#include "str.h"
#include "safe-mkstemp.h"
#include "execv-const.h"
#include "istream.h"
#include "ostream.h"
#include "master-service.h"
#include "lmtp-client.h"
#include "lda-settings.h"
#include "mail-deliver.h"
#include "smtp-client.h"

#include <unistd.h>
#include <sys/wait.h>
#include <sysexits.h>

#define DEFAULT_SUBMISSION_PORT 25

struct smtp_client {
	struct ostream *output;
	buffer_t *buf;
	int temp_fd;
	pid_t pid;

	bool use_smtp;
	bool success;
	bool finished;

	const struct lda_settings *set;
	char *temp_path;
	char *destination;
	char *return_path;
};

static struct smtp_client *smtp_client_devnull(struct ostream **output_r)
{
	struct smtp_client *client;
	
	client = i_new(struct smtp_client, 1);
	client->buf = buffer_create_dynamic(default_pool, 1);
	client->output = o_stream_create_buffer(client->buf);
	o_stream_close(client->output);
	client->pid = (pid_t)-1;

	*output_r = client->output;
	return client;
}

static void ATTR_NORETURN
smtp_client_run_sendmail(const struct lda_settings *set,
			 const char *destination,
			 const char *return_path, int fd)
{
	const char *const *sendmail_args, *const *argv, *str;
	ARRAY_TYPE(const_string) args;
	unsigned int i;

	sendmail_args = t_strsplit(set->sendmail_path, " ");
	t_array_init(&args, 16);
	for (i = 0; sendmail_args[i] != NULL; i++)
		array_append(&args, &sendmail_args[i], 1);

	str = "-i"; array_append(&args, &str, 1); /* ignore dots */
	str = "-f"; array_append(&args, &str, 1);
	str = return_path != NULL && *return_path != '\0' ?
		return_path : "<>";
	array_append(&args, &str, 1);

	str = "--"; array_append(&args, &str, 1);
	array_append(&args, &destination, 1);
	array_append_zero(&args);
	argv = array_idx(&args, 0);

	if (dup2(fd, STDIN_FILENO) < 0)
		i_fatal("dup2() failed: %m");

	master_service_env_clean();

	execv_const(argv[0], argv);
}

static struct smtp_client *
smtp_client_open_sendmail(const struct lda_settings *set,
			  const char *destination, const char *return_path,
			  struct ostream **output_r)
{
	struct smtp_client *client;
	int fd[2];
	pid_t pid;

	if (pipe(fd) < 0) {
		i_error("pipe() failed: %m");
		return smtp_client_devnull(output_r);
	}

	if ((pid = fork()) == (pid_t)-1) {
		i_error("fork() failed: %m");
		i_close_fd(&fd[0]); i_close_fd(&fd[1]);
		return smtp_client_devnull(output_r);
	}
	if (pid == 0) {
		/* child */
		i_close_fd(&fd[1]);
		smtp_client_run_sendmail(set, destination, return_path, fd[0]);
	}
	i_close_fd(&fd[0]);

	client = i_new(struct smtp_client, 1);
	client->output = o_stream_create_fd(fd[1], IO_BLOCK_SIZE, TRUE);
	o_stream_set_no_error_handling(client->output, TRUE);
	client->pid = pid;

	*output_r = client->output;
	return client;
}

static int create_temp_file(const char **path_r)
{
	string_t *path;
	int fd;

	path = t_str_new(128);
	str_append(path, "/tmp/dovecot.");
	str_append(path, master_service_get_name(master_service));
	str_append_c(path, '.');

	fd = safe_mkstemp(path, 0600, (uid_t)-1, (gid_t)-1);
	if (fd == -1) {
		i_error("safe_mkstemp(%s) failed: %m", str_c(path));
		return -1;
	}

	/* we just want the fd, unlink it */
	if (unlink(str_c(path)) < 0) {
		/* shouldn't happen.. */
		i_error("unlink(%s) failed: %m", str_c(path));
		i_close_fd(&fd);
		return -1;
	}

	*path_r = str_c(path);
	return fd;
}

struct smtp_client *
smtp_client_open(const struct lda_settings *set, const char *destination,
		 const char *return_path, struct ostream **output_r)
{
	struct smtp_client *client;
	const char *path;
	int fd;

	if (*set->submission_host == '\0') {
		return smtp_client_open_sendmail(set, destination,
						 return_path, output_r);
	}

	if ((fd = create_temp_file(&path)) == -1)
		return smtp_client_devnull(output_r);

	client = i_new(struct smtp_client, 1);
	client->set = set;
	client->temp_path = i_strdup(path);
	client->destination = i_strdup(destination);
	client->return_path = i_strdup(return_path);
	client->temp_fd = fd;
	client->output = o_stream_create_fd(fd, IO_BLOCK_SIZE, TRUE);
	o_stream_set_no_error_handling(client->output, TRUE);
	client->use_smtp = TRUE;

	*output_r = client->output;
	return client;
}

static int smtp_client_close_sendmail(struct smtp_client *client)
{
	int ret = EX_TEMPFAIL, status;

	o_stream_destroy(&client->output);

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
	if (client->buf != NULL)
		buffer_free(&client->buf);
	i_free(client);
	return ret;
}

static void smtp_client_send_finished(void *context)
{
	struct smtp_client *smtp_client = context;

	smtp_client->finished = TRUE;
	io_loop_stop(current_ioloop);
}

static void
rcpt_to_callback(bool success, const char *reply, void *context)
{
	struct smtp_client *smtp_client = context;

	if (!success) {
		i_error("smtp(%s): RCPT TO failed: %s",
			smtp_client->set->submission_host, reply);
		smtp_client_send_finished(smtp_client);
	}
}

static void
data_callback(bool success, const char *reply, void *context)
{
	struct smtp_client *smtp_client = context;

	if (!success) {
		i_error("smtp(%s): DATA failed: %s",
			smtp_client->set->submission_host, reply);
		smtp_client_send_finished(smtp_client);
	} else {
		smtp_client->success = TRUE;
	}
}

static int smtp_client_send(struct smtp_client *smtp_client)
{
	struct lmtp_client_settings client_set;
	struct lmtp_client *client;
	struct ioloop *ioloop;
	struct istream *input;
	const char *host, *p;
	unsigned int port = DEFAULT_SUBMISSION_PORT;

	host = smtp_client->set->submission_host;
	p = strchr(host, ':');
	if (p != NULL) {
		host = t_strdup_until(host, p);
		if (str_to_uint(p + 1, &port) < 0 ||
		    port == 0 || port > 65535) {
			i_error("Invalid port in submission_host: %s", p+1);
			return -1;
		}
	}

	if (o_stream_nfinish(smtp_client->output) < 0) {
		i_error("write(%s) failed: %m", smtp_client->temp_path);
		return -1;
	}

	if (o_stream_seek(smtp_client->output, 0) < 0) {
		i_error("lseek(%s) failed: %m", smtp_client->temp_path);
		return -1;
	}

	memset(&client_set, 0, sizeof(client_set));
	client_set.mail_from = smtp_client->return_path == NULL ? "<>" :
		t_strconcat("<", smtp_client->return_path, ">", NULL);
	client_set.my_hostname = smtp_client->set->hostname;

	ioloop = io_loop_create();
	client = lmtp_client_init(&client_set, smtp_client_send_finished,
				  smtp_client);

	if (lmtp_client_connect_tcp(client, LMTP_CLIENT_PROTOCOL_SMTP,
				    host, port) < 0) {
		lmtp_client_deinit(&client);
		io_loop_destroy(&ioloop);
		return -1;
	}

	lmtp_client_add_rcpt(client, smtp_client->destination,
			     rcpt_to_callback, data_callback, smtp_client);

	input = i_stream_create_fd(smtp_client->temp_fd, (size_t)-1, FALSE);
	lmtp_client_send(client, input);
	i_stream_unref(&input);

	if (!smtp_client->finished)
		io_loop_run(ioloop);
	io_loop_destroy(&ioloop);
	return smtp_client->success ? 0 : -1;
}

int smtp_client_close(struct smtp_client *client)
{
	int ret;

	if (!client->use_smtp)
		return smtp_client_close_sendmail(client);

	/* the mail has been written to a file. now actually send it. */
	ret = smtp_client_send(client);

	o_stream_destroy(&client->output);
	i_free(client->return_path);
	i_free(client->destination);
	i_free(client->temp_path);
	i_free(client);
	return ret < 0 ? EX_TEMPFAIL : 0;
}
