#ifndef __IMAP_MESSAGE_CACHE_H
#define __IMAP_MESSAGE_CACHE_H

/* IMAP message cache. Caches are mailbox-specific and must be cleared
   if UID validity changes. Also if message data may have changed,
   imap_msgcache_close() must be called.

   Caching is mostly done to avoid parsing the same message multiple times
   when client fetches the message in parts.
*/

#include "message-parser.h"

typedef enum {
	IMAP_CACHE_BODY			= 0x0001,
	IMAP_CACHE_BODYSTRUCTURE	= 0x0002,
	IMAP_CACHE_ENVELOPE		= 0x0004,
	IMAP_CACHE_INTERNALDATE		= 0x0008,
	IMAP_CACHE_VIRTUAL_SIZE		= 0x0010,

	IMAP_CACHE_MESSAGE_OPEN		= 0x0200,
	IMAP_CACHE_MESSAGE_PART		= 0x0400,
	IMAP_CACHE_MESSAGE_HDR_SIZE	= 0x0800,
	IMAP_CACHE_MESSAGE_BODY_SIZE	= 0x0100
} ImapCacheField;

typedef struct {
	/* Open mail for reading. */
	IStream *(*open_mail)(void *context);
	/* Rewind stream to beginning, possibly closing the old stream
	   if it can't directly be rewinded. */
	IStream *(*stream_rewind)(IStream *stream, void *context);

	/* Returns field if it's already cached, or NULL. */
	const char *(*get_cached_field)(ImapCacheField field, void *context);
	/* Returns MessagePart if it's already cached, or NULL. */
	MessagePart *(*get_cached_parts)(Pool pool, void *context);

	/* Returns message's internal date, or (time_t)-1 if error. */
	time_t (*get_internal_date)(void *context);
} ImapMessageCacheIface;

typedef struct _ImapMessageCache ImapMessageCache;

ImapMessageCache *imap_msgcache_alloc(ImapMessageCacheIface *iface);
void imap_msgcache_clear(ImapMessageCache *cache);
void imap_msgcache_free(ImapMessageCache *cache);

/* Open the specified message. Set vp_*_size if both physical and virtual
   sizes are same, otherwise (uoff_t)-1. If full_virtual_size isn't known,
   set it to (uoff_t)-1. Returns TRUE if all specified fields were cached.
   Even if FALSE is returned, it's possible to use the cached data,
   imap_msgcache_get() just returns NULL for those that weren't. */
int imap_msgcache_open(ImapMessageCache *cache, unsigned int uid,
		       ImapCacheField fields,
		       uoff_t vp_header_size, uoff_t vp_body_size,
		       uoff_t full_virtual_size, void *context);

/* Close the IOStream for opened message. */
void imap_msgcache_close(ImapMessageCache *cache);

/* Returns the field from cache, or NULL if it's not cached. */
const char *imap_msgcache_get(ImapMessageCache *cache, ImapCacheField field);

/* Returns the root MessagePart for message, or NULL if failed. */
MessagePart *imap_msgcache_get_parts(ImapMessageCache *cache);

/* Returns the virtual size of message, or (uoff_t)-1 if failed. */
uoff_t imap_msgcache_get_virtual_size(ImapMessageCache *cache);

/* Returns the internal date of message, or (time_t)-1 if failed. */
time_t imap_msgcache_get_internal_date(ImapMessageCache *cache);

/* Returns TRUE if successful. If stream is not NULL, it's set to point to
   beginning of message, or to beginning of message body if hdr_size is NULL. */
int imap_msgcache_get_rfc822(ImapMessageCache *cache, IStream **stream,
			     MessageSize *hdr_size, MessageSize *body_size);

/* Returns TRUE if successful. *stream is set to point to the first non-skipped
   character. size is set to specify the actual message size in
   virtual_skip..max_virtual_size range. cr_skipped is set to TRUE if first
   character in stream is LF, and we should NOT treat it as CR+LF. */
int imap_msgcache_get_rfc822_partial(ImapMessageCache *cache,
				     uoff_t virtual_skip,
				     uoff_t max_virtual_size,
				     int get_header, MessageSize *size,
				     IStream **stream, int *cr_skipped);

/* Returns TRUE if successful. *stream is set to point to beginning of
   message. */
int imap_msgcache_get_data(ImapMessageCache *cache, IStream **stream);

#endif
