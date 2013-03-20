/* Copyright (c) 2011-2013 Dovecot authors, see the included COPYING file */

#include "lib.h"
#include "iostream-rawlog-private.h"
#include "istream-private.h"
#include "istream-rawlog.h"

struct rawlog_istream {
	struct istream_private istream;
	struct rawlog_iostream riostream;
};

static void i_stream_rawlog_close(struct iostream_private *stream,
				  bool close_parent)
{
	struct rawlog_istream *rstream = (struct rawlog_istream *)stream;

	iostream_rawlog_close(&rstream->riostream);
	if (close_parent)
		i_stream_close(rstream->istream.parent);
}

static void i_stream_rawlog_destroy(struct iostream_private *stream)
{
	struct rawlog_istream *rstream = (struct rawlog_istream *)stream;
       uoff_t v_offset;

       v_offset = rstream->istream.parent_start_offset +
               rstream->istream.istream.v_offset;
       if (rstream->istream.parent->seekable ||
           v_offset > rstream->istream.parent->v_offset) {
               /* get to same position in parent stream */
               i_stream_seek(rstream->istream.parent, v_offset);
       }
       i_stream_unref(&rstream->istream.parent);
}

static ssize_t i_stream_rawlog_read(struct istream_private *stream)
{
	struct rawlog_istream *rstream = (struct rawlog_istream *)stream;
	ssize_t ret;
	size_t pos;

	i_stream_seek(stream->parent, rstream->istream.parent_start_offset +
		      stream->istream.v_offset);

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
	} while (pos <= stream->pos && ret > 0);

	if (pos <= stream->pos)
		ret = ret == 0 ? 0 : -1;
	else {
		ret = (ssize_t)(pos - stream->pos);
		iostream_rawlog_write(&rstream->riostream,
				      stream->buffer + stream->pos, ret);
	}
	stream->pos = pos;
	i_assert(ret != -1 || stream->istream.eof ||
		 stream->istream.stream_errno != 0);
	return ret;
}

struct istream *
i_stream_create_rawlog(struct istream *input, const char *rawlog_path,
		       int rawlog_fd, enum iostream_rawlog_flags flags)
{
	struct rawlog_istream *rstream;

	i_assert(rawlog_path != NULL);
	i_assert(rawlog_fd != -1);

	rstream = i_new(struct rawlog_istream, 1);
	rstream->istream.max_buffer_size = input->real_stream->max_buffer_size;
	rstream->istream.stream_size_passthrough = TRUE;

	rstream->riostream.rawlog_path = i_strdup(rawlog_path);
	rstream->riostream.rawlog_fd = rawlog_fd;
	iostream_rawlog_init(&rstream->riostream, flags, TRUE);

	rstream->istream.read = i_stream_rawlog_read;
	rstream->istream.iostream.close = i_stream_rawlog_close;
	rstream->istream.iostream.destroy = i_stream_rawlog_destroy;

	rstream->istream.istream.readable_fd = input->readable_fd;
	rstream->istream.istream.blocking = input->blocking;
	rstream->istream.istream.seekable = input->seekable;
	return i_stream_create(&rstream->istream, input,
			       i_stream_get_fd(input));
}
