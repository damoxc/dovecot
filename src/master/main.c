/* Copyright (C) 2002 Timo Sirainen */

#include "common.h"
#include "ioloop.h"
#include "lib-signals.h"
#include "network.h"

#include "auth-process.h"
#include "login-process.h"

#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <syslog.h>
#include <sys/stat.h>
#include <sys/wait.h>

const char *process_names[PROCESS_TYPE_MAX] = {
	"unknown",
	"auth",
	"login",
	"imap"
};

static IOLoop ioloop;
static Timeout to_children;

HashTable *pids;
int null_fd, imap_fd, imaps_fd;

int validate_str(const char *str, int max_len)
{
	int i;

	for (i = 0; i < max_len; i++) {
		if (str[i] == '\0')
			return TRUE;
	}

	return FALSE;
}

void clean_child_process(void)
{
	extern char **environ;

	/* remove all environment, we don't need them */
	if (environ != NULL)
		*environ = NULL;

	/* set the failure log */
	if (set_log_path != NULL) {
		putenv((char *) t_strconcat("IMAP_LOGFILE=",
					    set_log_path, NULL));
	}
	if (set_log_timestamp != NULL) {
		putenv((char *) t_strconcat("IMAP_LOGSTAMP=",
					    set_log_timestamp, NULL));
	}

	(void)close(null_fd);
	(void)close(imap_fd);
	(void)close(imaps_fd);

	/* close fds for auth/login processes */
	login_processes_cleanup();
        auth_processes_cleanup();

	closelog();
}

static void sig_quit(int signo __attr_unused__)
{
	io_loop_stop(ioloop);
}

static void children_check_timeout(void *context __attr_unused__,
				   Timeout timeout __attr_unused__)
{
	const char *process_type_name;
	pid_t pid;
	int status, process_type;

	while ((pid = waitpid(-1, &status, WNOHANG)) > 0) {
		/* get the type and remove from hash */
		process_type = PID_GET_PROCESS_TYPE(pid);
		PID_REMOVE_PROCESS_TYPE(pid);

		if (process_type == PROCESS_TYPE_IMAP)
			imap_process_destroyed(pid);

		/* write errors to syslog */
		process_type_name = process_names[process_type];
		if (WIFEXITED(status)) {
			status = WEXITSTATUS(status);
			if (status != 0) {
				i_error("child %d (%s) returned error %d",
					(int)pid, process_type_name, status);
			}
		} else if (WIFSIGNALED(status)) {
			i_error("child %d (%s) killed with signal %d",
				(int)pid, process_type_name, WTERMSIG(status));
		}
	}

	if (pid == -1 && errno != EINTR && errno != ECHILD)
		i_warning("waitpid() failed: %m");
}

static IPADDR *resolve_ip(const char *name)
{
	IPADDR *ip;
	int ret, ips_count;

	if (set_imap_listen == NULL || *set_imap_listen == '\0')
		return NULL;

	ret = net_gethostbyname(name, &ip, &ips_count);
	if (ret != 0)
		i_fatal("Can't resolve address: %s", set_imap_listen);

	if (ips_count < 1)
		i_fatal("No IPs for address: %s", set_imap_listen);

	return ip;
}

static void open_fds(void)
{
	IPADDR *imap_ip, *imaps_ip;

	imap_ip = resolve_ip(set_imap_listen);
	imaps_ip = resolve_ip(set_imaps_listen);

	if (imaps_ip == NULL && set_imaps_listen == NULL)
		imaps_ip = imap_ip;

	null_fd = open("/dev/null", O_RDONLY);
	if (null_fd == -1)
		i_fatal("Can't open /dev/null: %m");

	imap_fd = set_imap_port == 0 ? dup(null_fd) :
		net_listen(imap_ip, &set_imap_port);
	if (imap_fd == -1) {
		i_fatal("listen(%d) failed: %ms", set_imap_port);
	}

	imaps_fd = set_ssl_cert_file == NULL || set_ssl_key_file == NULL ||
		set_imaps_port == 0 ? dup(null_fd) :
		net_listen(imaps_ip, &set_imaps_port);
	if (imaps_fd == -1) {
		i_fatal("listen(%d) failed: %m", set_imaps_port);
	}
}

static void main_init()
{
	lib_init_signals(sig_quit);

	/* deny file access from everyone else except owner */
        (void)umask(0077);

	if (set_log_path == NULL) {
		openlog("imap-master", LOG_NDELAY, LOG_MAIL);

		i_set_panic_handler(i_syslog_panic_handler);
		i_set_fatal_handler(i_syslog_fatal_handler);
		i_set_error_handler(i_syslog_error_handler);
		i_set_warning_handler(i_syslog_warning_handler);
	} else {
		/* log failures into specified log file */
		i_set_failure_file(set_log_path, "imap-master");
		i_set_failure_timestamp_format(set_log_timestamp);
	}

	pids = hash_create(default_pool, 128, NULL, NULL);
	to_children = timeout_add(100, children_check_timeout, NULL);

	auth_processes_init();
	login_processes_init();
}

static void main_deinit(void)
{
        if (lib_signal_kill != 0)
		i_warning("Killed with signal %d", lib_signal_kill);

	login_processes_deinit();
	auth_processes_deinit();

	timeout_remove(to_children);

	(void)close(null_fd);
	(void)close(imap_fd);
	(void)close(imaps_fd);

	hash_destroy(pids);
	closelog();
}

static void daemonize(void)
{
	pid_t pid;

	pid = fork();
	if (pid < 0)
		i_fatal("fork() failed: %m");

	if (pid != 0)
		_exit(0);
}

int main(int argc, char *argv[])
{
	/* parse arguments */
	const char *configfile = SYSCONFDIR "/" PACKAGE ".conf";
	int foreground = FALSE;
	int i;

	lib_init();

	for (i = 1; i < argc; i++) {
		if (strcmp(argv[i], "-F") == 0) {
			/* foreground */
			foreground = TRUE;
		} else if (strcmp(argv[i], "-c") == 0) {
			/* config file */
			i++;
			if (i == argc) i_fatal("Missing config file argument");
			configfile = argv[i];
		} else {
			i_fatal("Unknown argument: %s", argv[1]);
		}
	}

	/* read and verify settings before forking */
	settings_read(configfile);
	open_fds();

	if (!foreground)
		daemonize();

	ioloop = io_loop_create();

	main_init();
        io_loop_run(ioloop);
	main_deinit();

	io_loop_destroy(ioloop);
	lib_deinit();

        return 0;
}
