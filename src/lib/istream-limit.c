/* Copyright (c) 2003-2009 Dovecot authors, see the included COPYING file */

#include "lib.h"
#include "istream-internal.h"

struct limit_istream {
	struct istream_private istream;

	uoff_t v_size;
};

static void i_stream_limit_destroy(struct iostream_private *stream)
{
	struct limit_istream *lstream = (struct limit_istream *) stream;
	uoff_t v_offset;

	v_offset = lstream->istream.parent_start_offset +
		lstream->istream.istream.v_offset;
	if (lstream->istream.parent->seekable ||
	    v_offset > lstream->istream.parent->v_offset) {
		/* get to same position in parent stream */
		i_stream_seek(lstream->istream.parent, v_offset);
	}
	i_stream_unref(&lstream->istream.parent);
}

static void
i_stream_limit_set_max_buffer_size(struct iostream_private *stream,
				   size_t max_size)
{
	struct limit_istream *lstream = (struct limit_istream *) stream;

	lstream->istream.max_buffer_size = max_size;
	i_stream_set_max_buffer_size(lstream->istream.parent, max_size);
}

static ssize_t i_stream_limit_read(struct istream_private *stream)
{
	struct limit_istream *lstream = (struct limit_istream *) stream;
	uoff_t left;
	ssize_t ret;
	size_t pos;

	if (stream->istream.v_offset +
	    (stream->pos - stream->skip) >= lstream->v_size) {
		stream->istream.eof = TRUE;
		return -1;
	}

	if (stream->parent->v_offset !=
	    lstream->istream.parent_start_offset + stream->istream.v_offset) {
		i_stream_seek(stream->parent,
			      lstream->istream.parent_start_offset +
			      stream->istream.v_offset);
	}

	stream->buffer = i_stream_get_data(stream->parent, &pos);
	if (pos <= stream->pos) {
		if ((ret = i_stream_read(stream->parent)) == -2)
			return -2;

		stream->istream.stream_errno = stream->parent->stream_errno;
		stream->istream.eof = stream->parent->eof;
		stream->buffer = i_stream_get_data(stream->parent, &pos);
	} else {
		ret = 0;
	}

	stream->pos -= stream->skip;
	stream->skip = 0;

	if (lstream->v_size != (uoff_t)-1) {
		left = lstream->v_size - stream->istream.v_offset;
		if (pos >= left) {
			pos = left;
			stream->istream.eof = TRUE;
		}
	}

	ret = pos > stream->pos ? (ssize_t)(pos - stream->pos) :
		(ret == 0 ? 0 : -1);
	stream->pos = pos;
	i_assert(ret != -1 || stream->istream.eof ||
		 stream->istream.stream_errno != 0);
	return ret;
}

static void i_stream_limit_seek(struct istream_private *stream, uoff_t v_offset,
				bool mark ATTR_UNUSED)
{
	struct limit_istream *lstream = (struct limit_istream *) stream;

	i_assert(v_offset <= lstream->v_size);

	stream->istream.v_offset = v_offset;
	stream->skip = stream->pos = 0;
}

static const struct stat *
i_stream_limit_stat(struct istream_private *stream, bool exact)
{
	struct limit_istream *lstream = (struct limit_istream *) stream;
	const struct stat *st;

	st = i_stream_stat(stream->parent, exact);
	if (st == NULL)
		return NULL;

	stream->statbuf = *st;
	if (lstream->v_size != (uoff_t)-1)
		stream->statbuf.st_size = lstream->v_size;
	return &stream->statbuf;
}

struct istream *i_stream_create_limit(struct istream *input, uoff_t v_size)
{
	struct limit_istream *lstream;

	i_stream_ref(input);

	lstream = i_new(struct limit_istream, 1);
	lstream->v_size = v_size;
	lstream->istream.max_buffer_size = input->real_stream->max_buffer_size;

	lstream->istream.iostream.destroy = i_stream_limit_destroy;
	lstream->istream.iostream.set_max_buffer_size =
		i_stream_limit_set_max_buffer_size;

	lstream->istream.parent = input;
	lstream->istream.read = i_stream_limit_read;
	lstream->istream.seek = i_stream_limit_seek;
	lstream->istream.stat = i_stream_limit_stat;

	lstream->istream.istream.blocking = input->blocking;
	lstream->istream.istream.seekable = input->seekable;
	return i_stream_create(&lstream->istream, input,
			       i_stream_get_fd(input));
}
