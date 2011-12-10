/* Copyright (c) 2010-2011 Dovecot authors, see the included COPYING file */

#include "lib.h"

#ifdef HAVE_BZLIB

#include "ostream-private.h"
#include "ostream-zlib.h"
#include <bzlib.h>

#define CHUNK_SIZE (1024*64)

struct bzlib_ostream {
	struct ostream_private ostream;
	bz_stream zs;

	char outbuf[CHUNK_SIZE];
	struct ostream *output;

	unsigned int flushed:1;
};

static void o_stream_bzlib_close(struct iostream_private *stream)
{
	struct bzlib_ostream *zstream = (struct bzlib_ostream *)stream;

	o_stream_flush(&zstream->ostream.ostream);
	(void)BZ2_bzCompressEnd(&zstream->zs);
}

static ssize_t
o_stream_bzlib_send_chunk(struct bzlib_ostream *zstream,
			  const void *data, size_t size)
{
	bz_stream *zs = &zstream->zs;
	ssize_t ret;

	zs->next_in = (void *)data;
	zs->avail_in = size;
	while (zs->avail_in > 0) {
		if (zs->avail_out == 0) {
			zs->next_out = zstream->outbuf;
			zs->avail_out = sizeof(zstream->outbuf);

			ret = o_stream_send(zstream->ostream.parent,
					    zstream->outbuf,
					    sizeof(zstream->outbuf));
			if (ret != (ssize_t)sizeof(zstream->outbuf)) {
				o_stream_copy_error_from_parent(&zstream->ostream);
				return -1;
			}
		}

		switch (BZ2_bzCompress(zs, BZ_RUN)) {
		case BZ_RUN_OK:
			break;
		default:
			i_unreached();
		}
	}
	zstream->flushed = FALSE;
	return 0;
}

static int o_stream_bzlib_send_flush(struct bzlib_ostream *zstream)
{
	bz_stream *zs = &zstream->zs;
	unsigned int len;
	bool done = FALSE;
	int ret;

	if (zs->avail_in != 0) {
		i_assert(zstream->ostream.ostream.last_failed_errno != 0);
		zstream->ostream.ostream.stream_errno =
			zstream->ostream.ostream.last_failed_errno;
		return -1;
	}

	if (zstream->flushed)
		return 0;

	do {
		len = sizeof(zstream->outbuf) - zs->avail_out;
		if (len != 0) {
			zs->next_out = zstream->outbuf;
			zs->avail_out = sizeof(zstream->outbuf);

			ret = o_stream_send(zstream->ostream.parent,
					    zstream->outbuf, len);
			if (ret != (int)len) {
				o_stream_copy_error_from_parent(&zstream->ostream);
				return -1;
			}
			if (done)
				break;
		}

		ret = BZ2_bzCompress(zs, BZ_FINISH);
		switch (ret) {
		case BZ_STREAM_END:
			done = TRUE;
			break;
		case BZ_FINISH_OK:
			break;
		default:
			i_unreached();
		}
	} while (zs->avail_out != sizeof(zstream->outbuf));

	zstream->flushed = TRUE;
	return 0;
}

static int o_stream_bzlib_flush(struct ostream_private *stream)
{
	struct bzlib_ostream *zstream = (struct bzlib_ostream *)stream;
	int ret;

	if (o_stream_bzlib_send_flush(zstream) < 0)
		return -1;

	ret = o_stream_flush(stream->parent);
	if (ret < 0)
		o_stream_copy_error_from_parent(stream);
	return ret;
}

static ssize_t
o_stream_bzlib_sendv(struct ostream_private *stream,
		    const struct const_iovec *iov, unsigned int iov_count)
{
	struct bzlib_ostream *zstream = (struct bzlib_ostream *)stream;
	ssize_t bytes = 0;
	unsigned int i;

	for (i = 0; i < iov_count; i++) {
		if (o_stream_bzlib_send_chunk(zstream, iov[i].iov_base,
					      iov[i].iov_len) < 0)
			return -1;
		bytes += iov[i].iov_len;
	}

	stream->ostream.offset += bytes;
	return bytes;
}

struct ostream *o_stream_create_bz2(struct ostream *output, int level)
{
	struct bzlib_ostream *zstream;
	int ret;

	i_assert(level >= 1 && level <= 9);

	zstream = i_new(struct bzlib_ostream, 1);
	zstream->ostream.sendv = o_stream_bzlib_sendv;
	zstream->ostream.flush = o_stream_bzlib_flush;
	zstream->ostream.iostream.close = o_stream_bzlib_close;

	ret = BZ2_bzCompressInit(&zstream->zs, level, 0, 0);
	switch (ret) {
	case BZ_OK:
		break;
	case BZ_MEM_ERROR:
		i_fatal_status(FATAL_OUTOFMEM,
			       "bzlib: Out of memory");
	case BZ_CONFIG_ERROR:
		i_fatal("Wrong bzlib library version (broken compilation)");
	case BZ_PARAM_ERROR:
		i_fatal("bzlib: Invalid parameters");
	default:
		i_fatal("BZ2_bzCompressInit() failed with %d", ret);
	}

	zstream->zs.next_out = zstream->outbuf;
	zstream->zs.avail_out = sizeof(zstream->outbuf);
	return o_stream_create(&zstream->ostream, output);
}
#endif
