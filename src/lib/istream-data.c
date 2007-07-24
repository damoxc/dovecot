/* Copyright (c) 2002-2003 Timo Sirainen */

#include "lib.h"
#include "istream-internal.h"

static void _close(struct _iostream *stream __attr_unused__)
{
}

static void _destroy(struct _iostream *stream __attr_unused__)
{
}

static ssize_t _read(struct _istream *stream __attr_unused__)
{
	stream->istream.eof = TRUE;
	return -1;
}

static void _seek(struct _istream *stream, uoff_t v_offset,
		  bool mark __attr_unused__)
{
	stream->skip = v_offset;
	stream->istream.v_offset = v_offset;
}

struct istream *i_stream_create_from_data(const void *data, size_t size)
{
	struct _istream *stream;

	stream = i_new(struct _istream, 1);
	stream->buffer = data;
	stream->pos = size;

	stream->iostream.close = _close;
	stream->iostream.destroy = _destroy;

	stream->read = _read;
	stream->seek = _seek;

	stream->istream.blocking = TRUE;
	stream->istream.seekable = TRUE;
	(void)_i_stream_create(stream, -1, 0);
	stream->statbuf.st_size = size;
	return &stream->istream;
}
