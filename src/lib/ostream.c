/* Copyright (c) 2002-2003 Timo Sirainen */

#include "lib.h"
#include "istream.h"
#include "ostream-internal.h"

void o_stream_ref(struct ostream *stream)
{
	_io_stream_ref(&stream->real_stream->iostream);
}

void o_stream_unref(struct ostream *stream)
{
	_io_stream_unref(&stream->real_stream->iostream);
}

void o_stream_close(struct ostream *stream)
{
	_io_stream_close(&stream->real_stream->iostream);
	stream->closed = TRUE;
}

void o_stream_set_max_buffer_size(struct ostream *stream, size_t max_size)
{
	_io_stream_set_max_buffer_size(&stream->real_stream->iostream,
				       max_size);
}

void o_stream_set_blocking(struct ostream *stream, int timeout_msecs,
			   void (*timeout_cb)(void *), void *context)
{
	_io_stream_set_blocking(&stream->real_stream->iostream, timeout_msecs,
				timeout_cb, context);
}

void o_stream_cork(struct ostream *stream)
{
	struct _ostream *_stream = stream->real_stream;

	if (stream->closed)
		return;

	_stream->cork(_stream);
}

int o_stream_flush(struct ostream *stream)
{
	struct _ostream *_stream = stream->real_stream;

	if (stream->closed)
		return -1;

	return _stream->flush(_stream);
}

int o_stream_have_space(struct ostream *stream, size_t size)
{
	struct _ostream *_stream = stream->real_stream;

	return _stream->have_space(_stream, size);
}

int o_stream_seek(struct ostream *stream, uoff_t offset)
{
	struct _ostream *_stream = stream->real_stream;

	if (stream->closed)
		return -1;

	return _stream->seek(_stream, offset);
}

ssize_t o_stream_send(struct ostream *stream, const void *data, size_t size)
{
	struct _ostream *_stream = stream->real_stream;

	if (stream->closed)
		return -1;

	if (size == 0)
		return 0;

	return _stream->send(_stream, data, size);
}

ssize_t o_stream_send_str(struct ostream *stream, const char *str)
{
	return o_stream_send(stream, str, strlen(str));
}

off_t o_stream_send_istream(struct ostream *outstream,
			    struct istream *instream)
{
	struct _ostream *_outstream = outstream->real_stream;

	if (outstream->closed || instream->closed)
		return -1;

	return _outstream->send_istream(_outstream, instream);
}

struct ostream *_o_stream_create(struct _ostream *_stream, pool_t pool)
{
	_stream->ostream.real_stream = _stream;

	_io_stream_init(pool, &_stream->iostream);
	return &_stream->ostream;
}
