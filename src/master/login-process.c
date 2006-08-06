/* Copyright (C) 2002 Timo Sirainen */

#include "common.h"
#include "ioloop.h"
#include "network.h"
#include "ostream.h"
#include "fdpass.h"
#include "fd-close-on-exec.h"
#include "env-util.h"
#include "restrict-access.h"
#include "restrict-process-size.h"
#include "login-process.h"
#include "auth-process.h"
#include "mail-process.h"
#include "master-login-interface.h"
#include "log.h"
#include "ssl-init.h"

#include <unistd.h>
#include <syslog.h>

struct login_process {
	struct login_group *group;
	struct login_process *prev_prelogin, *next_prelogin;
	int refcount;

	pid_t pid;
	int fd;
	struct io *io;
	struct ostream *output;
	enum master_login_state state;

	unsigned int initialized:1;
	unsigned int destroyed:1;
	unsigned int inetd_child:1;
};

struct login_auth_request {
	struct login_process *process;
	unsigned int tag;

	unsigned int login_tag;
	int fd;

	struct ip_addr local_ip, remote_ip;
};

static unsigned int auth_id_counter, login_pid_counter;
static struct timeout *to;
static struct io *io_listen;
static bool logins_stalled = FALSE;

static struct hash_table *processes;
static struct login_group *login_groups;

static void login_process_destroy(struct login_process *p);
static void login_process_unref(struct login_process *p);
static bool login_process_init_group(struct login_process *p);
static void login_processes_start_missing(void *context);

static void login_group_create(struct settings *set)
{
	struct login_group *group;

	group = i_new(struct login_group, 1);
	group->set = set;
	group->process_type = set->protocol == MAIL_PROTOCOL_IMAP ?
		PROCESS_TYPE_IMAP : PROCESS_TYPE_POP3;

	group->next = login_groups;
	login_groups = group;
}

static void login_group_destroy(struct login_group *group)
{
	i_free(group);
}

void auth_master_callback(const char *user, const char *const *args,
			  void *context)
{
	struct login_auth_request *request = context;
	struct master_login_reply master_reply;
	ssize_t ret;

	if (user == NULL)
		master_reply.success = FALSE;
	else {
		struct login_group *group = request->process->group;

		t_push();
		master_reply.success =
			create_mail_process(group->process_type, group->set,
					    request->fd, &request->local_ip,
					    &request->remote_ip, user, args,
					    FALSE);
		t_pop();
	}

	/* reply to login */
	master_reply.tag = request->login_tag;

	ret = o_stream_send(request->process->output, &master_reply,
			    sizeof(master_reply));
	if (ret != sizeof(master_reply)) {
		if (ret >= 0) {
			i_warning("Login process %s transmit buffer full, "
				  "killing..", dec2str(request->process->pid));
		}
		login_process_destroy(request->process);
	}

	if (close(request->fd) < 0)
		i_error("close(mail client) failed: %m");
	login_process_unref(request->process);
	i_free(request);
}

static void process_remove_from_prelogin_lists(struct login_process *p)
{
	if (p->state != LOGIN_STATE_FULL_PRELOGINS)
		return;

	if (p->prev_prelogin == NULL)
		p->group->oldest_prelogin_process = p->next_prelogin;
	else
		p->prev_prelogin->next_prelogin = p->next_prelogin;

	if (p->next_prelogin == NULL)
		p->group->newest_prelogin_process = p->prev_prelogin;
	else
		p->next_prelogin->prev_prelogin = p->prev_prelogin;

	p->prev_prelogin = p->next_prelogin = NULL;
}

static void process_mark_nonlistening(struct login_process *p,
				      enum master_login_state new_state)
{
	if (p->group == NULL)
		return;

	if (p->state == LOGIN_STATE_LISTENING)
		p->group->listening_processes--;

	if (new_state == LOGIN_STATE_FULL_PRELOGINS) {
		/* add to prelogin list */
		i_assert(p->state != new_state);

		p->prev_prelogin = p->group->newest_prelogin_process;
		if (p->group->newest_prelogin_process == NULL)
			p->group->oldest_prelogin_process = p;
		else
			p->group->newest_prelogin_process->next_prelogin = p;
		p->group->newest_prelogin_process = p;
	} else {
		process_remove_from_prelogin_lists(p);
	}
}

static void process_mark_listening(struct login_process *p)
{
	if (p->group == NULL)
		return;

	if (p->state != LOGIN_STATE_LISTENING)
		p->group->listening_processes++;

	process_remove_from_prelogin_lists(p);
}

static void
login_process_set_state(struct login_process *p, enum master_login_state state)
{
	if (state == p->state || state > LOGIN_STATE_COUNT ||
	    (state < p->state && p->group->set->login_process_per_connection)) {
		i_error("login: tried to change state %d -> %d "
			"(if you can't login at all, see src/lib/fdpass.c)",
			p->state, state);
		return;
	}

	if (state == LOGIN_STATE_LISTENING) {
		process_mark_listening(p);
	} else {
		process_mark_nonlistening(p, state);
	}

	p->state = state;
}

static void login_process_groups_create(void)
{
	struct server_settings *server;

	for (server = settings_root; server != NULL; server = server->next) {
		if (server->imap != NULL)
			login_group_create(server->imap);
		if (server->pop3 != NULL)
			login_group_create(server->pop3);
	}
}

static struct login_group *
login_group_process_find(const char *name, enum mail_protocol protocol)
{
	struct login_group *group;

	if (login_groups == NULL)
                login_process_groups_create();

	for (group = login_groups; group != NULL; group = group->next) {
		if (strcmp(group->set->server->name, name) == 0 &&
		    group->set->protocol == protocol)
			return group;
	}

	return NULL;
}

static bool login_process_read_group(struct login_process *p)
{
	struct login_group *group;
	const char *name, *proto;
	char buf[256];
	enum mail_protocol protocol;
	unsigned int len;
	ssize_t ret;

	/* read length */
	ret = read(p->fd, buf, 1);
	if (ret != 1)
		len = 0;
	else {
		len = buf[0];
		if (len >= sizeof(buf)) {
			i_error("login: Server name length too large");
			return FALSE;
		}

		ret = read(p->fd, buf, len);
	}

	if (ret < 0)
		i_error("login: read() failed: %m");
	else if (len == 0 || (size_t)ret != len)
		i_error("login: Server name wasn't sent");
	else {
		name = t_strndup(buf, len);
		proto = strchr(name, '/');
		if (proto == NULL) {
			proto = name;
			name = "default";
		} else {
			name = t_strdup_until(name, proto++);
		}

		if (strcmp(proto, "imap") == 0)
			protocol = MAIL_PROTOCOL_IMAP;
		else if (strcmp(proto, "pop3") == 0)
			protocol = MAIL_PROTOCOL_POP3;
		else {
			i_error("login: Unknown protocol '%s'", proto);
			return FALSE;
		}

		group = login_group_process_find(name, protocol);
		if (group == NULL) {
			i_error("login: Unknown server name '%s'", name);
			return FALSE;
		}

		p->group = group;
		return login_process_init_group(p);
	}
	return FALSE;
}

static void login_process_input(void *context)
{
	struct login_process *p = context;
	struct auth_process *auth_process;
	struct login_auth_request *authreq;
	struct master_login_request req;
	int client_fd;
	ssize_t ret;

	if (p->group == NULL) {
		/* we want to read the group */
		if (!login_process_read_group(p))
			login_process_destroy(p);
		return;
	}

	ret = fd_read(p->fd, &req, sizeof(req), &client_fd);
	if (ret >= (ssize_t)sizeof(req.version) &&
	    req.version != MASTER_LOGIN_PROTOCOL_VERSION) {
		i_error("login: Protocol version mismatch "
			"(mixed old and new binaries?)");
		login_process_destroy(p);
		return;
	}

	if (ret != sizeof(req)) {
		if (ret == 0) {
			/* disconnected, ie. the login process died */
		} else if (ret > 0) {
			/* req wasn't fully read */
			i_error("login: fd_read() couldn't read all req");
		} else {
			i_error("login: fd_read() failed: %m");
		}

		if (client_fd != -1) {
			if (close(client_fd) < 0)
				i_error("close(mail client) failed: %m");
		}

		login_process_destroy(p);
		return;
	}

	if (client_fd == -1) {
		/* just a notification that the login process */
		enum master_login_state state = req.tag;

		if (!p->initialized) {
			/* initialization notify */
			p->initialized = TRUE;;
		} else {
			/* change "listening for new connections" status */
			login_process_set_state(p, state);
		}
		return;
	}

	fd_close_on_exec(client_fd, TRUE);

	/* ask the cookie from the auth process */
	authreq = i_new(struct login_auth_request, 1);
	p->refcount++;
	authreq->process = p;
	authreq->tag = ++auth_id_counter;
	authreq->login_tag = req.tag;
	authreq->fd = client_fd;
	authreq->local_ip = req.local_ip;
	authreq->remote_ip = req.remote_ip;

	auth_process = auth_process_find(req.auth_pid);
	if (auth_process == NULL) {
		i_error("login: Authentication process %u doesn't exist",
			req.auth_pid);
		auth_master_callback(NULL, NULL, authreq);
	} else {
		auth_process_request(auth_process, p->pid,
				     req.auth_id, authreq);
	}
}

static struct login_process *
login_process_new(struct login_group *group, pid_t pid, int fd)
{
	struct login_process *p;

	i_assert(pid != 0);

	p = i_new(struct login_process, 1);
	p->group = group;
	p->refcount = 1;
	p->pid = pid;
	p->fd = fd;
	p->io = io_add(fd, IO_READ, login_process_input, p);
	p->output = o_stream_create_file(fd, default_pool,
					 sizeof(struct master_login_reply)*10,
					 FALSE);

	PID_ADD_PROCESS_TYPE(pid, PROCESS_TYPE_LOGIN);
	hash_insert(processes, POINTER_CAST(pid), p);

	p->state = LOGIN_STATE_LISTENING;

	if (p->group != NULL) {
		p->group->processes++;
		p->group->listening_processes++;
	}
	return p;
}

static void login_process_exited(struct login_process *p)
{
	if (p->group != NULL)
		p->group->processes--;

	hash_remove(processes, POINTER_CAST(p->pid));
	login_process_unref(p);
}

static void login_process_destroy(struct login_process *p)
{
	if (p->destroyed)
		return;
	p->destroyed = TRUE;

	if (!p->initialized && io_loop_is_running(ioloop)) {
		i_error("Login process died too early - shutting down");
		io_loop_stop(ioloop);
	}

	o_stream_close(p->output);
	io_remove(&p->io);
	if (close(p->fd) < 0)
		i_error("close(login) failed: %m");

	process_mark_nonlistening(p, LOGIN_STATE_FULL_LOGINS);

	if (p->inetd_child)
		login_process_exited(p);
}

static void login_process_unref(struct login_process *p)
{
	if (--p->refcount > 0)
		return;

	o_stream_unref(&p->output);
	i_free(p);
}

static void login_process_init_env(struct login_group *group, pid_t pid)
{
	struct settings *set = group->set;

	child_process_init_env();

	/* setup access environment - needs to be done after
	   clean_child_process() since it clears environment. Don't set user
	   parameter since we don't want to call initgroups() for login
	   processes. */
	restrict_access_set_env(NULL, set->login_uid,
				set->server->login_gid,
				set->login_chroot ? set->login_dir : NULL,
				0, 0, NULL);

	env_put("DOVECOT_MASTER=1");

	if (!set->ssl_disable) {
		const char *ssl_key_password;

		ssl_key_password = *set->ssl_key_password != '\0' ?
			set->ssl_key_password : ssl_manual_key_password;

		if (*set->ssl_ca_file != '\0') {
			env_put(t_strconcat("SSL_CA_FILE=",
					    set->ssl_ca_file, NULL));
		}
		env_put(t_strconcat("SSL_CERT_FILE=",
				    set->ssl_cert_file, NULL));
		env_put(t_strconcat("SSL_KEY_FILE=",
				    set->ssl_key_file, NULL));
		env_put(t_strconcat("SSL_KEY_PASSWORD=",
				    ssl_key_password, NULL));
		env_put("SSL_PARAM_FILE="SSL_PARAMETERS_FILENAME);
		if (*set->ssl_cipher_list != '\0') {
			env_put(t_strconcat("SSL_CIPHER_LIST=",
					    set->ssl_cipher_list, NULL));
		}
		if (set->ssl_verify_client_cert)
			env_put("SSL_VERIFY_CLIENT_CERT=1");
	}

	if (set->disable_plaintext_auth)
		env_put("DISABLE_PLAINTEXT_AUTH=1");
	if (set->verbose_proctitle)
		env_put("VERBOSE_PROCTITLE=1");
	if (set->verbose_ssl)
		env_put("VERBOSE_SSL=1");
	if (set->server->auths->verbose)
		env_put("VERBOSE_AUTH=1");

	if (set->login_process_per_connection) {
		env_put("PROCESS_PER_CONNECTION=1");
		env_put("MAX_LOGGING_USERS=1");
	} else {
		env_put(t_strdup_printf("MAX_CONNECTIONS=%u",
					set->login_max_connections));
	}

	env_put(t_strconcat("PROCESS_UID=", dec2str(pid), NULL));
	env_put(t_strconcat("GREETING=", set->login_greeting, NULL));
	env_put(t_strconcat("LOG_FORMAT_ELEMENTS=",
			    set->login_log_format_elements, NULL));
	env_put(t_strconcat("LOG_FORMAT=", set->login_log_format, NULL));
	if (set->login_greeting_capability)
		env_put("GREETING_CAPABILITY=1");

	if (group->process_type == PROCESS_TYPE_IMAP) {
		env_put(t_strconcat("CAPABILITY_STRING=",
				    *set->imap_capability != '\0' ?
				    set->imap_capability :
				    set->imap_generated_capability, NULL));
	}
}

static pid_t create_login_process(struct login_group *group)
{
	struct log_io *log;
	unsigned int max_log_lines_per_sec;
	const char *prefix;
	pid_t pid;
	int fd[2], log_fd;

	if (group->set->login_uid == 0)
		i_fatal("Login process must not run as root");

	/* create communication to process with a socket pair */
	if (socketpair(AF_UNIX, SOCK_STREAM, 0, fd) < 0) {
		i_error("socketpair() failed: %m");
		return -1;
	}

	max_log_lines_per_sec =
		group->set->login_process_per_connection ? 10 : 0;
	log_fd = log_create_pipe(&log, max_log_lines_per_sec);
	if (log_fd < 0)
		pid = -1;
	else {
		pid = fork();
		if (pid < 0)
			i_error("fork() failed: %m");
	}

	if (pid < 0) {
		(void)close(fd[0]);
		(void)close(fd[1]);
		(void)close(log_fd);
		return -1;
	}

	if (pid != 0) {
		/* master */
		prefix = t_strdup_printf("%s-login: ",
					 process_names[group->process_type]);
		log_set_prefix(log, prefix);

		net_set_nonblock(fd[0], TRUE);
		fd_close_on_exec(fd[0], TRUE);
		(void)login_process_new(group, pid, fd[0]);
		(void)close(fd[1]);
		(void)close(log_fd);
		return pid;
	}

	prefix = t_strdup_printf("master-%s-login: ",
				 process_names[group->process_type]);
	log_set_prefix(log, prefix);

	/* move the listen handle */
	if (dup2(group->set->listen_fd, LOGIN_LISTEN_FD) < 0)
		i_fatal("dup2(listen_fd) failed: %m");
	fd_close_on_exec(LOGIN_LISTEN_FD, FALSE);

	/* move the SSL listen handle */
	if (dup2(group->set->ssl_listen_fd, LOGIN_SSL_LISTEN_FD) < 0)
		i_fatal("dup2(ssl_listen_fd) failed: %m");
	fd_close_on_exec(LOGIN_SSL_LISTEN_FD, FALSE);

	/* move communication handle */
	if (dup2(fd[1], LOGIN_MASTER_SOCKET_FD) < 0)
		i_fatal("dup2(master) failed: %m");
	fd_close_on_exec(LOGIN_MASTER_SOCKET_FD, FALSE);

	if (dup2(log_fd, 2) < 0)
		i_fatal("dup2(stderr) failed: %m");
	fd_close_on_exec(2, FALSE);

	(void)close(fd[0]);
	(void)close(fd[1]);

	login_process_init_env(group, getpid());

	if (!group->set->login_chroot) {
		/* no chrooting, but still change to the directory */
		if (chdir(group->set->login_dir) < 0) {
			i_fatal("chdir(%s) failed: %m",
				group->set->login_dir);
		}
	}

	restrict_process_size(group->set->login_process_size, (unsigned int)-1);

	/* make sure we don't leak syslog fd, but do it last so that
	   any errors above will be logged */
	closelog();

	client_process_exec(group->set->login_executable, "");
	i_fatal_status(FATAL_EXEC, "execv(%s) failed: %m",
		       group->set->login_executable);
	return -1;
}

void login_process_destroyed(pid_t pid, bool abnormal_exit)
{
	struct login_process *p;

	p = hash_lookup(processes, POINTER_CAST(pid));
	if (p == NULL)
		i_panic("Lost login process PID %s", dec2str(pid));
	i_assert(!p->inetd_child);

	if (abnormal_exit) {
		/* don't start raising the process count if they're dying all
		   the time */
		if (p->group != NULL)
			p->group->wanted_processes_count = 0;
	}

	login_process_destroy(p);
	login_process_exited(p);
}

void login_processes_destroy_all(bool unref)
{
	struct hash_iterate_context *iter;
	void *key, *value;

	iter = hash_iterate_init(processes);
	while (hash_iterate(iter, &key, &value)) {
		login_process_destroy(value);
		if (unref) login_process_unref(value);
	}
	hash_iterate_deinit(iter);

	while (login_groups != NULL) {
		struct login_group *group = login_groups;

		login_groups = group->next;
		login_group_destroy(group);
	}
}

static void login_processes_notify_group(struct login_group *group)
{
	struct hash_iterate_context *iter;
	struct master_login_reply reply;
	void *key, *value;

	memset(&reply, 0, sizeof(reply));

	iter = hash_iterate_init(processes);
	while (hash_iterate(iter, &key, &value)) {
		struct login_process *p = value;

		if (p->group == group)
			(void)o_stream_send(p->output, &reply, sizeof(reply));
	}
	hash_iterate_deinit(iter);
}

static int login_group_start_missings(struct login_group *group)
{
	if (group->set->login_process_per_connection &&
	    group->processes >= group->set->login_max_processes_count &&
	    group->listening_processes == 0) {
		/* destroy the oldest listening process. non-listening
		   processes are logged in users who we don't want to kick out
		   because someone's started flooding */
		if (group->oldest_prelogin_process != NULL)
			login_process_destroy(group->oldest_prelogin_process);
	}

	/* we want to respond fast when multiple clients are connecting
	   at once, but we also want to prevent fork-bombing. use the
	   same method as apache: check once a second if we need new
	   processes. if yes and we've used all the existing processes,
	   double their amount (unless we've hit the high limit).
	   Then for each second that didn't use all existing processes,
	   drop the max. process count by one. */
	if (group->wanted_processes_count < group->set->login_processes_count) {
		group->wanted_processes_count =
			group->set->login_processes_count;
	} else if (group->listening_processes == 0)
		group->wanted_processes_count *= 2;
	else if (group->wanted_processes_count >
		 group->set->login_processes_count)
		group->wanted_processes_count--;

	while (group->listening_processes < group->wanted_processes_count &&
	       group->processes < group->set->login_max_processes_count) {
		if (create_login_process(group) < 0)
			return -1;
	}

	if (group->listening_processes == 0 &&
	    !group->set->login_process_per_connection) {
		/* we've reached our limit. notify the processes to start
		   listening again which makes them kill some of their
		   oldest clients when accepting the next connection */
		login_processes_notify_group(group);
	}
	return 0;
}

static void login_processes_stall(void)
{
	if (logins_stalled)
		return;

	i_error("Temporary failure in creating login processes, "
		"slowing down for now");
	logins_stalled = TRUE;

	timeout_remove(&to);
	to = timeout_add(60*1000, login_processes_start_missing, NULL);
}


static void
login_processes_start_missing(void *context __attr_unused__)
{
	struct login_group *group;

	if (login_groups == NULL)
		login_process_groups_create();

	for (group = login_groups; group != NULL; group = group->next) {
		if (login_group_start_missings(group) < 0) {
			login_processes_stall();
			return;
		}
	}

	if (logins_stalled) {
		/* processes were created successfully */
		i_info("Created login processes successfully, unstalling");

		logins_stalled = FALSE;
		timeout_remove(&to);
		to = timeout_add(1000, login_processes_start_missing, NULL);
	}
}

static int login_process_send_env(struct login_process *p)
{
	extern char **environ;
	char **env;
	ssize_t len;
	int ret = 0;

	/* this will clear our environment. luckily we don't need it. */
	login_process_init_env(p->group, p->pid);

	for (env = environ; *env != NULL; env++) {
		len = strlen(*env);

		if (o_stream_send(p->output, *env, len) != len ||
		    o_stream_send(p->output, "\n", 1) != 1) {
			ret = -1;
			break;
		}
	}

	if (!p->group->set->login_chroot) {
		/* if we're not chrooting, we need to tell login process
		   where its base directory is */
		const char *str = t_strdup_printf("LOGIN_DIR=%s\n",
						  p->group->set->login_dir);
		len = strlen(str);
		if (o_stream_send(p->output, str, len) != len)
			ret = -1;
	}

	if (ret == 0 && o_stream_send(p->output, "\n", 1) != 1)
		ret = -1;

	env_clean();
	return ret;
}

static bool login_process_init_group(struct login_process *p)
{
	p->group->processes++;
	p->group->listening_processes++;

	if (login_process_send_env(p) < 0) {
		i_error("login: Couldn't send environment");
		return FALSE;
	}

	return TRUE;
}

static void inetd_login_accept(void *context __attr_unused__)
{
        struct login_process *p;
	int fd;

	fd = net_accept(inetd_login_fd, NULL, NULL);
	if (fd < 0) {
		if (fd < -1)
			i_fatal("accept(inetd_login_fd) failed: %m");
	} else {
		net_set_nonblock(fd, TRUE);
		fd_close_on_exec(fd, TRUE);

		p = login_process_new(NULL, ++login_pid_counter, fd);
		p->initialized = TRUE;
		p->inetd_child = TRUE;
	}
}

void login_processes_init(void)
{
	auth_id_counter = 0;
        login_pid_counter = 0;
	login_groups = NULL;

	processes = hash_create(default_pool, default_pool, 128, NULL, NULL);
	if (!IS_INETD()) {
		to = timeout_add(1000, login_processes_start_missing, NULL);
		io_listen = NULL;
	} else {
		to = NULL;
		io_listen = io_add(inetd_login_fd, IO_READ,
				   inetd_login_accept, NULL);
	}
}

void login_processes_deinit(void)
{
	if (to != NULL)
		timeout_remove(&to);
	if (io_listen != NULL)
		io_remove(&io_listen);

        login_processes_destroy_all(TRUE);
	hash_destroy(processes);
}
