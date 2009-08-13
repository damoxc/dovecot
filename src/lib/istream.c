/* Copyright (c) 2002-2009 Dovecot authors, see the included COPYING file */

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

#undef i_stream_set_destroy_callback
void i_stream_set_destroy_callback(struct istream *stream,
				   istream_callback_t *callback, void *context)
{
	struct iostream_private *iostream = &stream->real_stream->iostream;

	iostream->destroy_callback = callback;
	iostream->destroy_context = context;
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

	if (stream->stream_errno == 0)
		stream->stream_errno = ENOENT;
}

void i_stream_set_init_buffer_size(struct istream *stream, size_t size)
{
	stream->real_stream->init_buffer_size = size;
}

void i_stream_set_max_buffer_size(struct istream *stream, size_t max_size)
{
	io_stream_set_max_buffer_size(&stream->real_stream->iostream, max_size);
}

void i_stream_set_return_partial_line(struct istream *stream, bool set)
{
	stream->real_stream->return_nolf_line = set;
}

ssize_t i_stream_read(struct istream *stream)
{
	struct istream_private *_stream = stream->real_stream;
	size_t old_size;
	ssize_t ret;

	if (unlikely(stream->closed))
		return -1;

	stream->eof = FALSE;
	stream->stream_errno = 0;

	old_size = _stream->pos - _stream->skip;
	ret = _stream->read(_stream);
	switch (ret) {
	case -2:
		i_assert(_stream->skip != _stream->pos);
		break;
	case -1:
		if (stream->stream_errno != 0) {
			/* error handling should be easier if we now just
			   assume the stream is now at EOF */
			stream->eof = TRUE;
		} else {
			i_assert(stream->eof);
		}
		break;
	case 0:
		i_assert(!stream->blocking);
		break;
	default:
		i_assert(ret > 0);
		i_assert((size_t)ret+old_size == _stream->pos - _stream->skip);
		break;
	}
	return ret;
}

ssize_t i_stream_read_copy_from_parent(struct istream *istream)
{
	struct istream_private *stream = istream->real_stream;
	size_t pos;
	ssize_t ret;

	stream->pos -= stream->skip;
	stream->skip = 0;

	stream->buffer = i_stream_get_data(stream->parent, &pos);
	if (pos > stream->pos)
		ret = 0;
	else do {
		if ((ret = i_stream_read(stream->parent)) == -2)
			return -2;

		stream->istream.stream_errno = stream->parent->stream_errno;
		stream->istream.eof = stream->parent->eof;
		stream->buffer = i_stream_get_data(stream->parent, &pos);
		/* check again, in case the parent stream had been seeked
		   backwards and the previous read() didn't get us far
		   enough. */
	} while (pos <= stream->pos && ret > 0);

	ret = pos > stream->pos ? (ssize_t)(pos - stream->pos) :
		(ret == 0 ? 0 : -1);
	stream->pos = pos;
	i_assert(ret != -1 || stream->istream.eof ||
		 stream->istream.stream_errno != 0);
	return ret;
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

	if (unlikely(stream->closed))
		return;

	stream->stream_errno = 0;
	_stream->seek(_stream, stream->v_offset + count, FALSE);
}

static bool i_stream_can_optimize_seek(struct istream *stream)
{
	uoff_t expected_offset;

	if (stream->real_stream->parent == NULL)
		return TRUE;

	/* use the fast route only if the parent stream is at the
	   expected offset */
	expected_offset = stream->real_stream->parent_start_offset +
		stream->v_offset - stream->real_stream->skip;
	if (stream->real_stream->parent->v_offset != expected_offset)
		return FALSE;

	return i_stream_can_optimize_seek(stream->real_stream->parent);
}

void i_stream_seek(struct istream *stream, uoff_t v_offset)
{
	struct istream_private *_stream = stream->real_stream;

	if (v_offset >= stream->v_offset &&
	    i_stream_can_optimize_seek(stream)) {
		i_stream_skip(stream, v_offset - stream->v_offset);
		return;
	}

	if (unlikely(stream->closed))
		return;

	stream->eof = FALSE;
	_stream->seek(_stream, v_offset, FALSE);
}

void i_stream_seek_mark(struct istream *stream, uoff_t v_offset)
{
	struct istream_private *_stream = stream->real_stream;

	if (unlikely(stream->closed))
		return;

	stream->eof = FALSE;
	_stream->seek(_stream, v_offset, TRUE);
}

void i_stream_sync(struct istream *stream)
{
	struct istream_private *_stream = stream->real_stream;

	if (unlikely(stream->closed))
		return;

	if (_stream->sync != NULL)
		_stream->sync(_stream);
}

const struct stat *i_stream_stat(struct istream *stream, bool exact)
{
	struct istream_private *_stream = stream->real_stream;

	if (unlikely(stream->closed))
		return NULL;

	return _stream->stat(_stream, exact);
}

int i_stream_get_size(struct istream *stream, bool exact, uoff_t *size_r)
{
	struct istream_private *_stream = stream->real_stream;

	if (unlikely(stream->closed))
		return -1;

	return _stream->get_size(_stream, exact, size_r);
}

bool i_stream_have_bytes_left(const struct istream *stream)
{
	const struct istream_private *_stream = stream->real_stream;

	return !stream->eof || _stream->skip != _stream->pos;
}

bool i_stream_is_eof(struct istream *stream)
{
	const struct istream_private *_stream = stream->real_stream;

	if (_stream->skip == _stream->pos)
		(void)i_stream_read(stream);
	return !i_stream_have_bytes_left(stream);
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

	if (i < stream->pos)
		i++;
	stream->istream.v_offset += i - stream->skip;
	stream->skip = i;
	return ret;
}

static char *i_stream_last_line(struct istream_private *_stream)
{
	if (_stream->istream.eof && _stream->skip != _stream->pos &&
	    _stream->return_nolf_line) {
		/* the last line is missing LF and we want to return it. */
		return i_stream_next_line_finish(_stream, _stream->pos);
	}
	return NULL;
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

	if (unlikely(_stream->w_buffer == NULL)) {
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
	if (ret_buf == NULL)
		return i_stream_last_line(_stream);
        return ret_buf;
}

char *i_stream_read_next_line(struct istream *stream)
{
	char *line;

	for (;;) {
		line = i_stream_next_line(stream);
		if (line != NULL)
			break;

		if (i_stream_read(stream) <= 0)
			return i_stream_last_line(stream->real_stream);
	}
	return line;
}

const unsigned char *
i_stream_get_data(const struct istream *stream, size_t *size_r)
{
	const struct istream_private *_stream = stream->real_stream;

	if (_stream->skip >= _stream->pos) {
		*size_r = 0;
		return NULL;
	}

        *size_r = _stream->pos - _stream->skip;
        return _stream->buffer + _stream->skip;
}

unsigned char *i_stream_get_modifiable_data(const struct istream *stream,
					    size_t *size_r)
{
	const struct istream_private *_stream = stream->real_stream;

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

	if (ret == 0) {
		/* need to read more */
		i_assert(!stream->blocking);
		return 0;
	}
	if (stream->eof) {
		if (read_more) {
			/* we read at least some new data */
			return 0;
		}
	} else {
		i_assert(stream->stream_errno != 0);
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
	if (stream->buffer_size <= stream->init_buffer_size)
		stream->buffer_size = stream->init_buffer_size;
	else
		stream->buffer_size = nearest_power(stream->buffer_size);

	if (stream->max_buffer_size > 0 &&
	    stream->buffer_size > stream->max_buffer_size)
		stream->buffer_size = stream->max_buffer_size;

	stream->buffer = stream->w_buffer =
		i_realloc(stream->w_buffer, old_size, stream->buffer_size);
}

bool i_stream_get_buffer_space(struct istream_private *stream,
			       size_t wanted_size, size_t *size_r)
{
	i_assert(wanted_size > 0);

	if (wanted_size > stream->buffer_size - stream->pos) {
		if (stream->skip > 0) {
			/* remove the unused bytes from beginning of buffer */
                        i_stream_compress(stream);
		} else if (stream->max_buffer_size == 0 ||
			   stream->buffer_size < stream->max_buffer_size) {
			/* buffer is full - grow it */
			i_stream_grow_buffer(stream, I_STREAM_MIN_SIZE);
		}
	}

	if (size_r != NULL)
		*size_r = stream->buffer_size - stream->pos;
	return stream->pos != stream->buffer_size;
}

bool i_stream_add_data(struct istream *_stream, const unsigned char *data,
		       size_t size)
{
	struct istream_private *stream = _stream->real_stream;
	size_t size2;

	(void)i_stream_get_buffer_space(stream, size, &size2);
	if (size > size2)
		return FALSE;

	memcpy(stream->w_buffer + stream->pos, data, size);
	stream->pos += size;
	return TRUE;
}

static void
i_stream_default_set_max_buffer_size(struct iostream_private *stream,
				     size_t max_size)
{
	struct istream_private *_stream = (struct istream_private *)stream;

	_stream->max_buffer_size = max_size;
	if (_stream->parent != NULL)
		i_stream_set_max_buffer_size(_stream->parent, max_size);
}

static void i_stream_default_destroy(struct iostream_private *stream)
{
	struct istream_private *_stream = (struct istream_private *)stream;

	i_free(_stream->w_buffer);
	if (_stream->parent != NULL)
		i_stream_unref(&_stream->parent);
}

static void
i_stream_default_seek(struct istream_private *stream,
		      uoff_t v_offset, bool mark ATTR_UNUSED)
{
	size_t available;

	if (stream->istream.v_offset > v_offset)
		i_panic("stream doesn't support seeking backwards");

	while (stream->istream.v_offset < v_offset) {
		(void)i_stream_read(&stream->istream);

		available = stream->pos - stream->skip;
		if (available == 0) {
			stream->istream.stream_errno = ESPIPE;
			return;
		}
		if (available <= v_offset - stream->istream.v_offset)
			i_stream_skip(&stream->istream, available);
		else {
			i_stream_skip(&stream->istream,
				      v_offset - stream->istream.v_offset);
		}
	}
}
static const struct stat *
i_stream_default_stat(struct istream_private *stream, bool exact ATTR_UNUSED)
{
	return &stream->statbuf;
}

static int
i_stream_default_get_size(struct istream_private *stream,
			  bool exact, uoff_t *size_r)
{
	const struct stat *st;

	st = stream->stat(stream, exact);
	if (st == NULL)
		return -1;
	if (st->st_size == -1)
		return 0;

	*size_r = st->st_size;
	return 1;
}

struct istream *
i_stream_create(struct istream_private *_stream, struct istream *parent, int fd)
{
	_stream->fd = fd;
	if (parent != NULL) {
		_stream->parent = parent;
		_stream->parent_start_offset = parent->v_offset;
		_stream->abs_start_offset = parent->v_offset +
			parent->real_stream->abs_start_offset;
		i_stream_ref(parent);
	}
	_stream->istream.real_stream = _stream;

	if (_stream->iostream.destroy == NULL)
		_stream->iostream.destroy = i_stream_default_destroy;
	if (_stream->seek == NULL) {
		i_assert(!_stream->istream.seekable);
		_stream->seek = i_stream_default_seek;
	}
	if (_stream->stat == NULL)
		_stream->stat = i_stream_default_stat;
	if (_stream->get_size == NULL)
		_stream->get_size = i_stream_default_get_size;
	if (_stream->iostream.set_max_buffer_size == NULL) {
		_stream->iostream.set_max_buffer_size =
			i_stream_default_set_max_buffer_size;
	}
	if (_stream->init_buffer_size == 0)
		_stream->init_buffer_size = I_STREAM_MIN_SIZE;

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
