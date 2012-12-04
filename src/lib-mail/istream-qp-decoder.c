/* Copyright (c) 2012 Dovecot authors, see the included COPYING file */

#include "lib.h"
#include "buffer.h"
#include "istream-private.h"
#include "quoted-printable.h"
#include "istream-qp.h"

struct qp_decoder_istream {
	struct istream_private istream;
};

static int
i_stream_read_parent(struct istream_private *stream, size_t *prev_size)
{
	size_t size;
	ssize_t ret;

	size = i_stream_get_data_size(stream->parent);
	if (size >= 4 && size != *prev_size) {
		*prev_size = size;
		return 1;
	}

	/* we have less than one qp block.
	   see if there is more data available. */
	ret = i_stream_read(stream->parent);
	if (ret <= 0) {
		stream->istream.stream_errno = stream->parent->stream_errno;
		stream->istream.eof = stream->parent->eof;
		return ret;
	}
	*prev_size = i_stream_get_data_size(stream->parent);
	return 1;
}

static int
i_stream_qp_try_decode_block(struct qp_decoder_istream *bstream)
{
	struct istream_private *stream = &bstream->istream;
	const unsigned char *data;
	size_t size, avail, buffer_avail, pos;
	buffer_t buf;

	data = i_stream_get_data(stream->parent, &size);
	if (size == 0)
		return 0;

	i_stream_try_alloc(stream, (size+3)/4*3, &avail);
	buffer_avail = stream->buffer_size - stream->pos;

	if ((size + 3) / 4 * 3 > buffer_avail) {
		/* can't fit everything to destination buffer.
		   write as much as we can. */
		size = (buffer_avail / 3) * 4;
		if (size == 0)
			return -2;
	}

	buffer_create_from_data(&buf, stream->w_buffer + stream->pos,
				buffer_avail);
	quoted_printable_decode(data, size, &pos, &buf);

	stream->pos += buf.used;
	i_stream_skip(stream->parent, pos);
	return pos > 0 ? 1 : 0;
}

static ssize_t i_stream_qp_decoder_read(struct istream_private *stream)
{
	struct qp_decoder_istream *bstream =
		(struct qp_decoder_istream *)stream;
	const unsigned char *data;
	size_t size;
	size_t pre_count, post_count;
	int ret;
	size_t prev_size = 0;

	do {
		ret = i_stream_read_parent(stream, &prev_size);
		if (ret <= 0) {
			if (ret != -1 || stream->istream.stream_errno != 0)
				return 0;

			data = i_stream_get_data(stream->parent, &size);
			if (size == 0)
				return -1;

			if (size == 1 && data[0] == '=') {
				/* ends with "=". normally this would be
				   followed by LF, but it's not really an
				   error even without. */
				i_stream_skip(stream->parent, 1);
				stream->istream.eof = TRUE;
				return -1;
			}
			/* qp input with a partial block */
			stream->istream.stream_errno = EINVAL;
			return -1;
		}

		/* encode as many blocks as fits into destination buffer */
		pre_count = stream->pos - stream->skip;
		while ((ret = i_stream_qp_try_decode_block(bstream)) > 0) ;
		post_count = stream->pos - stream->skip;
	} while (ret == 0 && pre_count == post_count);

	if (ret < 0)
		return ret;

	i_assert(post_count > pre_count);
	return post_count - pre_count;
}

static void
i_stream_qp_decoder_seek(struct istream_private *stream,
			     uoff_t v_offset, bool mark)
{
	if (v_offset < stream->istream.v_offset) {
		/* seeking backwards - go back to beginning and seek
		   forward from there. */
		stream->parent_expected_offset = stream->parent_start_offset;
		stream->skip = stream->pos = 0;
		stream->istream.v_offset = 0;
		i_stream_seek(stream->parent, 0);
	}
	i_stream_default_seek_nonseekable(stream, v_offset, mark);
}

struct istream *i_stream_create_qp_decoder(struct istream *input)
{
	struct qp_decoder_istream *bstream;

	bstream = i_new(struct qp_decoder_istream, 1);
	bstream->istream.max_buffer_size = input->real_stream->max_buffer_size;

	bstream->istream.read = i_stream_qp_decoder_read;
	bstream->istream.seek = i_stream_qp_decoder_seek;

	bstream->istream.istream.readable_fd = FALSE;
	bstream->istream.istream.blocking = input->blocking;
	bstream->istream.istream.seekable = input->seekable;
	return i_stream_create(&bstream->istream, input,
			       i_stream_get_fd(input));
}
