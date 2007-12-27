#ifndef FAILURES_H
#define FAILURES_H

/* Default exit status codes that we could use. */
enum fatal_exit_status {
	FATAL_LOGOPEN	= 80, /* Can't open log file */
	FATAL_LOGWRITE  = 81, /* Can't write to log file */
	FATAL_LOGERROR  = 82, /* Internal logging error */
	FATAL_OUTOFMEM	= 83, /* Out of memory */
	FATAL_EXEC	= 84, /* exec() failed */

	FATAL_DEFAULT	= 89
};

enum log_type {
	LOG_TYPE_INFO,
	LOG_TYPE_WARNING,
	LOG_TYPE_ERROR,
	LOG_TYPE_FATAL,
	LOG_TYPE_PANIC
};

#define DEFAULT_FAILURE_STAMP_FORMAT "%b %d %H:%M:%S "

typedef void failure_callback_t(enum log_type type, const char *, va_list);
typedef void fatal_failure_callback_t(enum log_type type, int status,
				      const char *, va_list);

extern const char *failure_log_type_prefixes[];

void i_log_type(enum log_type type, const char *format, ...) ATTR_FORMAT(2, 3);

void i_panic(const char *format, ...) ATTR_FORMAT(1, 2) ATTR_NORETURN;
void i_fatal(const char *format, ...) ATTR_FORMAT(1, 2) ATTR_NORETURN;
void i_error(const char *format, ...) ATTR_FORMAT(1, 2);
void i_warning(const char *format, ...) ATTR_FORMAT(1, 2);
void i_info(const char *format, ...) ATTR_FORMAT(1, 2);

void i_fatal_status(int status, const char *format, ...)
	ATTR_FORMAT(2, 3) ATTR_NORETURN;

/* Change failure handlers. */
#ifndef __cplusplus
void i_set_fatal_handler(fatal_failure_callback_t *callback ATTR_NORETURN);
#else
/* Older g++ doesn't like attributes in parameters */
void i_set_fatal_handler(fatal_failure_callback_t *callback);
#endif
void i_set_error_handler(failure_callback_t *callback);
void i_set_info_handler(failure_callback_t *callback);

/* Send failures to syslog() */
void i_syslog_fatal_handler(enum log_type type, int status,
			    const char *fmt, va_list args)
	ATTR_NORETURN ATTR_FORMAT(3, 0);
void i_syslog_error_handler(enum log_type type, const char *fmt, va_list args)
	ATTR_FORMAT(2, 0);

/* Open syslog and set failure/info handlers to use it. */
void i_set_failure_syslog(const char *ident, int options, int facility);

/* Send failures to specified log file instead of stderr. */
void i_set_failure_file(const char *path, const char *prefix);

/* Send errors to stderr using internal error protocol. */
void i_set_failure_internal(void);

/* Send informational messages to specified log file. i_set_failure_*()
   functions modify the info file too, so call this function after them. */
void i_set_info_file(const char *path);

/* Set the failure prefix. This is used only when logging to a file. */
void i_set_failure_prefix(const char *prefix);

/* Prefix failures with a timestamp. fmt is in strftime() format. */
void i_set_failure_timestamp_format(const char *fmt);

/* Call the callback before exit()ing. The callback may update the status. */
void i_set_failure_exit_callback(void (*callback)(int *status));
/* Call the exit callback and exit() */
void failure_exit(int status) ATTR_NORETURN;

void failures_deinit(void);

#endif
