#ifndef __ISTREAM_H
#define __ISTREAM_H

/* Note that some systems (Solaris) may use a macro to redefine struct stat */
#include <sys/stat.h>

struct istream {
	uoff_t v_offset;

	int stream_errno;
	unsigned int mmaped:1; /* be careful when copying data */
	unsigned int blocking:1; /* read() shouldn't return 0 */
	unsigned int closed:1;
	unsigned int seekable:1; /* we can seek() backwards */
	unsigned int eof:1; /* read() has reached to end of file
	                       (but may still be data available in buffer) */

	struct _istream *real_stream;
};

struct istream *i_stream_create_fd(int fd, size_t max_buffer_size,
				   bool autoclose_fd);
struct istream *i_stream_create_mmap(int fd, size_t block_size,
				     uoff_t start_offset, uoff_t v_size,
				     bool autoclose_fd);
struct istream *i_stream_create_from_data(const void *data, size_t size);
struct istream *i_stream_create_limit(struct istream *input,
				      uoff_t v_start_offset, uoff_t v_size);

/* i_stream_close() + i_stream_unref() */
void i_stream_destroy(struct istream **stream);

/* Reference counting. References start from 1, so calling i_stream_unref()
   destroys the stream if i_stream_ref() is never used. */
void i_stream_ref(struct istream *stream);
/* Unreferences the stream and sets stream pointer to NULL. */
void i_stream_unref(struct istream **stream);

/* Return file descriptor for stream, or -1 if none is available. */
int i_stream_get_fd(struct istream *stream);

/* Mark the stream closed. Any reads after this will return -1. The data
   already read can still be used. */
void i_stream_close(struct istream *stream);
/* Sync the stream with the underlying backend, ie. if a file has been
   modified, flush any cached data. */
void i_stream_sync(struct istream *stream);

/* Change the maximum size for stream's input buffer to grow. Useful only
   for buffered streams (currently only file). */
void i_stream_set_max_buffer_size(struct istream *stream, size_t max_size);

/* Returns number of bytes read if read was ok, -1 if EOF or error, -2 if the
   input buffer is full. */
ssize_t i_stream_read(struct istream *stream);
/* Skip forward a number of bytes. Never fails, the next read tells if it
   was successful. */
void i_stream_skip(struct istream *stream, uoff_t count);
/* Seek to specified position from beginning of file. Never fails, the next
   read tells if it was successful. This works only for files. */
void i_stream_seek(struct istream *stream, uoff_t v_offset);
/* Like i_stream_seek(), but also giving a hint that after reading some data
   we could be seeking back to this mark or somewhere after it. If input
   stream's implementation is slow in seeking backwards, it can use this hint
   to cache some of the data in memory. */
void i_stream_seek_mark(struct istream *stream, uoff_t v_offset);
/* Returns struct stat, or NULL if error. As the underlying stream may not be
   a file, only some of the fields might be set, others would be zero.
   st_size is always set, and if it's not known, it's -1.

   If exact=FALSE, the stream may not return exactly correct values, but the
   returned values can be compared to see if anything had changed (eg. in
   compressed stream st_size could be compressed size) */
const struct stat *i_stream_stat(struct istream *stream, bool exact);
/* Returns TRUE if there are any bytes left to be read or in buffer. */
bool i_stream_have_bytes_left(struct istream *stream);

/* Gets the next line from stream and returns it, or NULL if more data is
   needed to make a full line. Note that if the stream ends with LF not being
   the last character, this function doesn't return the last line. */
char *i_stream_next_line(struct istream *stream);
/* Like i_stream_next_line(), but reads for more data if needed. Returns NULL
   if more data is needed or error occurred. */
char *i_stream_read_next_line(struct istream *stream);

/* Returns pointer to beginning of read data, or NULL if there's no data
   buffered. */
const unsigned char *i_stream_get_data(struct istream *stream, size_t *size_r);
/* Like i_stream_get_data(), but returns non-const data. This only works with
   buffered streams (currently only file), others return NULL. */
unsigned char *i_stream_get_modifiable_data(struct istream *stream,
					    size_t *size_r);
/* Like i_stream_get_data(), but read more when needed. Returns 1 if more
   than threshold bytes are available, 0 if less, -1 if error or EOF with no
   bytes read that weren't already in buffer, or -2 if stream's input buffer
   is full. */
int i_stream_read_data(struct istream *stream, const unsigned char **data_r,
		       size_t *size_r, size_t threshold);

#endif
