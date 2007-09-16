/* Copyright (c) 2002-2007 Dovecot authors, see the included COPYING file */

#include "lib.h"
#include "ioloop.h"
#include "str.h"
#include "istream-internal.h"

void i_stream_destroy(struct istream **stream)
{
	i_stream_close(*stream);
	i_stream_unref(stream);
}

void i_stream_ref(struct istream *stream)
{
	io_stream_ref(&stream->real_stream->iostream);
}

void i_stream_unref(struct istream **stream)
{
	struct istream_private *_stream = (*stream)->real_stream;

	if (_stream->iostream.refcount == 1) {
		if (_stream->line_str != NULL)
			str_free(&_stream->line_str);
	}
	io_stream_unref(&(*stream)->real_stream->iostream);
	*stream = NULL;
}

int i_stream_get_fd(struct istream *stream)
{
	struct istream_private *_stream = stream->real_stream;

	return _stream->fd;
}

void i_stream_close(struct istream *stream)
{
	io_stream_close(&stream->real_stream->iostream);
	stream->closed = TRUE;
}

void i_stream_set_max_buffer_size(struct istream *stream, size_t max_size)
{
	io_stream_set_max_buffer_size(&stream->real_stream->iostream, max_size);
}

ssize_t i_stream_read(struct istream *stream)
{
	struct istream_private *_stream = stream->real_stream;

	if (stream->closed)
		return -1;

	stream->eof = FALSE;
	return _stream->read(_stream);
}

void i_stream_skip(struct istream *stream, uoff_t count)
{
	struct istream_private *_stream = stream->real_stream;
	size_t data_size;

	data_size = _stream->pos - _stream->skip;
	if (count <= data_size) {
		/* within buffer */
		stream->v_offset += count;
		_stream->skip += count;
		return;
	}

	/* have to seek forward */
	count -= data_size;
	_stream->skip = _stream->pos;
	stream->v_offset += data_size;

	if (stream->closed)
		return;

	_stream->seek(_stream, stream->v_offset + count, FALSE);
}

void i_stream_seek(struct istream *stream, uoff_t v_offset)
{
	struct istream_private *_stream = stream->real_stream;

	if (v_offset >= stream->v_offset) {
		i_stream_skip(stream, v_offset - stream->v_offset);
		return;
	}

	if (stream->closed)
		return;

	stream->eof = FALSE;
	_stream->seek(_stream, v_offset, FALSE);
}

void i_stream_seek_mark(struct istream *stream, uoff_t v_offset)
{
	struct istream_private *_stream = stream->real_stream;

	if (stream->closed)
		return;

	stream->eof = FALSE;
	_stream->seek(_stream, v_offset, TRUE);
}

void i_stream_sync(struct istream *stream)
{
	struct istream_private *_stream = stream->real_stream;

	if (!stream->closed && _stream->sync != NULL)
		_stream->sync(_stream);
}

const struct stat *i_stream_stat(struct istream *stream, bool exact)
{
	struct istream_private *_stream = stream->real_stream;

	if (stream->closed)
		return NULL;

	return _stream->stat(_stream, exact);
}

bool i_stream_have_bytes_left(struct istream *stream)
{
	struct istream_private *_stream = stream->real_stream;

	return !stream->eof || _stream->skip != _stream->pos;
}

static char *i_stream_next_line_finish(struct istream_private *stream, size_t i)
{
	char *ret;
	size_t end;

	if (i > 0 && stream->buffer[i-1] == '\r')
		end = i - 1;
	else
		end = i;

	if (stream->w_buffer != NULL) {
		/* modify the buffer directly */
		stream->w_buffer[end] = '\0';
		ret = (char *)stream->w_buffer + stream->skip;
	} else {
		/* use a temporary string to return it */
		if (stream->line_str == NULL)
			stream->line_str = str_new(default_pool, 256);
		str_truncate(stream->line_str, 0);
		str_append_n(stream->line_str, stream->buffer + stream->skip,
			     end - stream->skip);
		ret = str_c_modifiable(stream->line_str);
	}

	i++;
	stream->istream.v_offset += i - stream->skip;
	stream->skip = i;
	return ret;
}

char *i_stream_next_line(struct istream *stream)
{
	struct istream_private *_stream = stream->real_stream;
	char *ret_buf;
        size_t i;

	if (_stream->skip >= _stream->pos) {
		stream->stream_errno = 0;
		return NULL;
	}

	if (_stream->w_buffer == NULL) {
		i_error("i_stream_next_line() called for unmodifiable stream");
		return NULL;
	}

	/* @UNSAFE */
	ret_buf = NULL;
	for (i = _stream->skip; i < _stream->pos; i++) {
		if (_stream->buffer[i] == 10) {
			/* got it */
			ret_buf = i_stream_next_line_finish(_stream, i);
                        break;
		}
	}

        return ret_buf;
}

char *i_stream_read_next_line(struct istream *stream)
{
	char *line;

	line = i_stream_next_line(stream);
	if (line != NULL)
		return line;

	if (i_stream_read(stream) > 0)
		line = i_stream_next_line(stream);
	return line;
}

const unsigned char *i_stream_get_data(struct istream *stream, size_t *size_r)
{
	struct istream_private *_stream = stream->real_stream;

	if (_stream->skip >= _stream->pos) {
		*size_r = 0;
		return NULL;
	}

        *size_r = _stream->pos - _stream->skip;
        return _stream->buffer + _stream->skip;
}

unsigned char *i_stream_get_modifiable_data(struct istream *stream,
					    size_t *size_r)
{
	struct istream_private *_stream = stream->real_stream;

	if (_stream->skip >= _stream->pos || _stream->w_buffer == NULL) {
		*size_r = 0;
		return NULL;
	}

        *size_r = _stream->pos - _stream->skip;
        return _stream->w_buffer + _stream->skip;
}

int i_stream_read_data(struct istream *stream, const unsigned char **data_r,
		       size_t *size_r, size_t threshold)
{
	ssize_t ret = 0;
	bool read_more = FALSE;

	do {
		*data_r = i_stream_get_data(stream, size_r);
		if (*size_r > threshold)
			return 1;

		/* we need more data */
		ret = i_stream_read(stream);
		if (ret > 0)
			read_more = TRUE;
	} while (ret > 0);

	*data_r = i_stream_get_data(stream, size_r);
	if (ret == -2)
		return -2;

	if (read_more || ret == 0) {
		i_assert(!stream->blocking || stream->eof);
		return 0;
	}
	return -1;
}

void i_stream_compress(struct istream_private *stream)
{
	memmove(stream->w_buffer, stream->w_buffer + stream->skip,
		stream->pos - stream->skip);
	stream->pos -= stream->skip;

	stream->skip = 0;
}

void i_stream_grow_buffer(struct istream_private *stream, size_t bytes)
{
	size_t old_size;

	old_size = stream->buffer_size;

	stream->buffer_size = stream->pos + bytes;
	if (stream->buffer_size <= I_STREAM_MIN_SIZE)
		stream->buffer_size = I_STREAM_MIN_SIZE;
	else
		stream->buffer_size = nearest_power(stream->buffer_size);

	if (stream->max_buffer_size > 0 &&
	    stream->buffer_size > stream->max_buffer_size)
		stream->buffer_size = stream->max_buffer_size;

	stream->buffer = stream->w_buffer =
		i_realloc(stream->w_buffer, old_size, stream->buffer_size);
}

static void
i_stream_default_set_max_buffer_size(struct iostream_private *stream,
				     size_t max_size)
{
	struct istream_private *_stream = (struct istream_private *)stream;

	_stream->max_buffer_size = max_size;
}

static const struct stat *
i_stream_default_stat(struct istream_private *stream, bool exact ATTR_UNUSED)
{
	return &stream->statbuf;
}

struct istream *
i_stream_create(struct istream_private *_stream,
		int fd, uoff_t abs_start_offset)
{
	_stream->fd = fd;
	_stream->abs_start_offset = abs_start_offset;
	_stream->istream.real_stream = _stream;

	if (_stream->stat == NULL)
		_stream->stat = i_stream_default_stat;
	if (_stream->iostream.set_max_buffer_size == NULL) {
		_stream->iostream.set_max_buffer_size =
			i_stream_default_set_max_buffer_size;
	}

	memset(&_stream->statbuf, 0, sizeof(_stream->statbuf));
	_stream->statbuf.st_size = -1;
	_stream->statbuf.st_atime =
		_stream->statbuf.st_mtime =
		_stream->statbuf.st_ctime = ioloop_time;

	io_stream_init(&_stream->iostream);
	return &_stream->istream;
}

#ifdef STREAM_TEST
/* gcc istream.c -o teststream liblib.a -Wall -DHAVE_CONFIG_H -DSTREAM_TEST -g */

#include <fcntl.h>
#include <unistd.h>
#include "ostream.h"

#define BUF_VALUE(offset) \
        (((offset) % 256) ^ ((offset) / 256))

static void check_buffer(const unsigned char *data, size_t size, size_t offset)
{
	size_t i;

	for (i = 0; i < size; i++)
		i_assert(data[i] == BUF_VALUE(i+offset));
}

int main(void)
{
	struct istream *input, *l_input;
	struct ostream *output1, *output2;
	int i, fd1, fd2;
	unsigned char buf[1024];
	const unsigned char *data;
	size_t size;

	lib_init();

	fd1 = open("teststream.1", O_RDWR | O_CREAT | O_TRUNC, 0600);
	if (fd1 < 0)
		i_fatal("open() failed: %m");
	fd2 = open("teststream.2", O_RDWR | O_CREAT | O_TRUNC, 0600);
	if (fd2 < 0)
		i_fatal("open() failed: %m");

	/* write initial data */
	for (i = 0; i < sizeof(buf); i++)
		buf[i] = BUF_VALUE(i);
	write(fd1, buf, sizeof(buf));

	/* test reading */
	input = i_stream_create_fd(fd1, 512, FALSE);
	i_assert(i_stream_get_size(input) == sizeof(buf));

	i_assert(i_stream_read_data(input, &data, &size, 0) > 0);
	i_assert(size == 512);
	check_buffer(data, size, 0);

	i_stream_seek(input, 256);
	i_assert(i_stream_read_data(input, &data, &size, 0) > 0);
	i_assert(size == 512);
	check_buffer(data, size, 256);

	i_stream_seek(input, 0);
	i_assert(i_stream_read_data(input, &data, &size, 512) == -2);
	i_assert(size == 512);
	check_buffer(data, size, 0);

	i_stream_skip(input, 900);
	i_assert(i_stream_read_data(input, &data, &size, 0) > 0);
	i_assert(size == sizeof(buf) - 900);
	check_buffer(data, size, 900);

	/* test moving data */
	output1 = o_stream_create_fd(fd1, 512, FALSE);
	output2 = o_stream_create_fd(fd2, 512, FALSE);

	i_stream_seek(input, 1); size = sizeof(buf)-1;
	i_assert(o_stream_send_istream(output2, input) == size);
	o_stream_flush(output2);

	lseek(fd2, 0, SEEK_SET);
	i_assert(read(fd2, buf, sizeof(buf)) == size);
	check_buffer(buf, size, 1);

	i_stream_seek(input, 0);
	o_stream_seek(output1, sizeof(buf));
	i_assert(o_stream_send_istream(output1, input) == sizeof(buf));

	/* test moving with limits */
	l_input = i_stream_create_limit(input, sizeof(buf)/2, 512);
	i_stream_seek(l_input, 0);
	o_stream_seek(output1, 10);
	i_assert(o_stream_send_istream(output1, l_input) == 512);

	i_stream_set_max_buffer_size(input, sizeof(buf));

	i_stream_seek(input, 0);
	i_assert(i_stream_read_data(input, &data, &size, sizeof(buf)-1) > 0);
	i_assert(size == sizeof(buf));
	check_buffer(data, 10, 0);
	check_buffer(data + 10, 512, sizeof(buf)/2);
	check_buffer(data + 10 + 512,
		     size - (10 + 512), 10 + 512);

	/* reading within limits */
	i_stream_seek(l_input, 0);
	i_assert(i_stream_read_data(l_input, &data, &size, 511) > 0);
	i_assert(size == 512);
	i_assert(i_stream_read_data(l_input, &data, &size, 512) == -2);
	i_assert(size == 512);
	i_stream_skip(l_input, 511);
	i_assert(i_stream_read_data(l_input, &data, &size, 0) > 0);
	i_assert(size == 1);
	i_stream_skip(l_input, 1);
	i_assert(i_stream_read_data(l_input, &data, &size, 0) == -1);
	i_assert(size == 0);

	unlink("teststream.1");
	unlink("teststream.2");
	return 0;
}
#endif
