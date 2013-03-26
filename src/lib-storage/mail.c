/* Copyright (c) 2002-2013 Dovecot authors, see the included COPYING file */

#include "lib.h"
#include "ioloop.h"
#include "buffer.h"
#include "hash.h"
#include "hex-binary.h"
#include "crc32.h"
#include "sha1.h"
#include "hostpid.h"
#include "mail-cache.h"
#include "mail-storage-private.h"

#include <time.h>

struct mail *mail_alloc(struct mailbox_transaction_context *t,
			enum mail_fetch_field wanted_fields,
			struct mailbox_header_lookup_ctx *wanted_headers)
{
	struct mail *mail;

	T_BEGIN {
		mail = t->box->v.mail_alloc(t, wanted_fields, wanted_headers);
		hook_mail_allocated(mail);
	} T_END;

	return mail;
}

void mail_free(struct mail **mail)
{
	struct mail_private *p = (struct mail_private *)*mail;

	p->v.free(*mail);
	*mail = NULL;
}

void mail_set_seq(struct mail *mail, uint32_t seq)
{
	struct mail_private *p = (struct mail_private *)mail;

	p->v.set_seq(mail, seq, FALSE);
}

void mail_set_seq_saving(struct mail *mail, uint32_t seq)
{
	struct mail_private *p = (struct mail_private *)mail;

	p->v.set_seq(mail, seq, TRUE);
}

bool mail_set_uid(struct mail *mail, uint32_t uid)
{
	struct mail_private *p = (struct mail_private *)mail;

	return p->v.set_uid(mail, uid);
}

bool mail_prefetch(struct mail *mail)
{
	struct mail_private *p = (struct mail_private *)mail;

	return p->v.prefetch(mail);
}

void mail_add_temp_wanted_fields(struct mail *mail,
				 enum mail_fetch_field fields,
				 struct mailbox_header_lookup_ctx *headers)
{
	struct mail_private *p = (struct mail_private *)mail;

	p->v.add_temp_wanted_fields(mail, fields, headers);
}

enum mail_flags mail_get_flags(struct mail *mail)
{
	struct mail_private *p = (struct mail_private *)mail;

	return p->v.get_flags(mail);
}

uint64_t mail_get_modseq(struct mail *mail)
{
	struct mail_private *p = (struct mail_private *)mail;

	return p->v.get_modseq(mail);
}

uint64_t mail_get_pvt_modseq(struct mail *mail)
{
	struct mail_private *p = (struct mail_private *)mail;

	return p->v.get_pvt_modseq(mail);
}

const char *const *mail_get_keywords(struct mail *mail)
{
	struct mail_private *p = (struct mail_private *)mail;

	return p->v.get_keywords(mail);
}

const ARRAY_TYPE(keyword_indexes) *mail_get_keyword_indexes(struct mail *mail)
{
	struct mail_private *p = (struct mail_private *)mail;

	return p->v.get_keyword_indexes(mail);
}

int mail_get_parts(struct mail *mail, struct message_part **parts_r)
{
	struct mail_private *p = (struct mail_private *)mail;

	return p->v.get_parts(mail, parts_r);
}

int mail_get_date(struct mail *mail, time_t *date_r, int *timezone_r)
{
	struct mail_private *p = (struct mail_private *)mail;

	return p->v.get_date(mail, date_r, timezone_r);
}

int mail_get_received_date(struct mail *mail, time_t *date_r)
{
	struct mail_private *p = (struct mail_private *)mail;

	return p->v.get_received_date(mail, date_r);
}

int mail_get_save_date(struct mail *mail, time_t *date_r)
{
	struct mail_private *p = (struct mail_private *)mail;

	return p->v.get_save_date(mail, date_r);
}

int mail_get_virtual_size(struct mail *mail, uoff_t *size_r)
{
	struct mail_private *p = (struct mail_private *)mail;

	return p->v.get_virtual_size(mail, size_r);
}

int mail_get_physical_size(struct mail *mail, uoff_t *size_r)
{
	struct mail_private *p = (struct mail_private *)mail;

	return p->v.get_physical_size(mail, size_r);
}

int mail_get_first_header(struct mail *mail, const char *field,
			  const char **value_r)
{
	struct mail_private *p = (struct mail_private *)mail;

	return p->v.get_first_header(mail, field, FALSE, value_r);
}

int mail_get_first_header_utf8(struct mail *mail, const char *field,
			       const char **value_r)
{
	struct mail_private *p = (struct mail_private *)mail;

	return p->v.get_first_header(mail, field, TRUE, value_r);
}

int mail_get_headers(struct mail *mail, const char *field,
		     const char *const **value_r)
{
	struct mail_private *p = (struct mail_private *)mail;

	return p->v.get_headers(mail, field, FALSE, value_r);
}

int mail_get_headers_utf8(struct mail *mail, const char *field,
			  const char *const **value_r)
{
	struct mail_private *p = (struct mail_private *)mail;

	return p->v.get_headers(mail, field, TRUE, value_r);
}

int mail_get_header_stream(struct mail *mail,
			   struct mailbox_header_lookup_ctx *headers,
			   struct istream **stream_r)
{
	struct mail_private *p = (struct mail_private *)mail;

	return p->v.get_header_stream(mail, headers, stream_r);
}

void mail_set_aborted(struct mail *mail)
{
	mail_storage_set_error(mail->box->storage, MAIL_ERROR_NOTPOSSIBLE,
			       "Mail field not cached");
}

int mail_get_stream(struct mail *mail, struct message_size *hdr_size,
		    struct message_size *body_size, struct istream **stream_r)
{
	struct mail_private *p = (struct mail_private *)mail;
	int ret;

	if (mail->lookup_abort != MAIL_LOOKUP_ABORT_NEVER) {
		mail_set_aborted(mail);
		return -1;
	}
	T_BEGIN {
		ret = p->v.get_stream(mail, TRUE, hdr_size, body_size, stream_r);
	} T_END;
	return ret;
}

int mail_get_hdr_stream(struct mail *mail, struct message_size *hdr_size,
			struct istream **stream_r)
{
	struct mail_private *p = (struct mail_private *)mail;
	int ret;

	if (mail->lookup_abort != MAIL_LOOKUP_ABORT_NEVER) {
		mail_set_aborted(mail);
		return -1;
	}
	T_BEGIN {
		ret = p->v.get_stream(mail, FALSE, hdr_size, NULL, stream_r);
	} T_END;
	return ret;
}

int mail_get_binary_stream(struct mail *mail, const struct message_part *part,
			   bool include_hdr, uoff_t *size_r,
			   bool *binary_r, struct istream **stream_r)
{
	struct mail_private *p = (struct mail_private *)mail;
	int ret;

	if (mail->lookup_abort != MAIL_LOOKUP_ABORT_NEVER) {
		mail_set_aborted(mail);
		return -1;
	}
	T_BEGIN {
		ret = p->v.get_binary_stream(mail, part, include_hdr,
					     size_r, NULL, binary_r, stream_r);
	} T_END;
	return ret;
}

int mail_get_binary_size(struct mail *mail, const struct message_part *part,
			 bool include_hdr, uoff_t *size_r,
			 unsigned int *lines_r)
{
	struct mail_private *p = (struct mail_private *)mail;
	bool binary;
	int ret;

	T_BEGIN {
		ret = p->v.get_binary_stream(mail, part, include_hdr,
					     size_r, lines_r, &binary, NULL);
	} T_END;
	return ret;
}

int mail_get_special(struct mail *mail, enum mail_fetch_field field,
		     const char **value_r)
{
	struct mail_private *p = (struct mail_private *)mail;

	if (p->v.get_special(mail, field, value_r) < 0)
		return -1;
	i_assert(*value_r != NULL);
	return 0;
}

struct mail *mail_get_real_mail(struct mail *mail)
{
	struct mail_private *p = (struct mail_private *)mail;

	return p->v.get_real_mail(mail);
}

void mail_update_flags(struct mail *mail, enum modify_type modify_type,
		       enum mail_flags flags)
{
	struct mail_private *p = (struct mail_private *)mail;

	p->v.update_flags(mail, modify_type, flags);
}

void mail_update_keywords(struct mail *mail, enum modify_type modify_type,
			  struct mail_keywords *keywords)
{
	struct mail_private *p = (struct mail_private *)mail;

	p->v.update_keywords(mail, modify_type, keywords);
}

void mail_update_modseq(struct mail *mail, uint64_t min_modseq)
{
	struct mail_private *p = (struct mail_private *)mail;

	p->v.update_modseq(mail, min_modseq);
}

void mail_update_pvt_modseq(struct mail *mail, uint64_t min_pvt_modseq)
{
	struct mail_private *p = (struct mail_private *)mail;

	p->v.update_pvt_modseq(mail, min_pvt_modseq);
}

void mail_update_pop3_uidl(struct mail *mail, const char *uidl)
{
	struct mail_private *p = (struct mail_private *)mail;

	if (p->v.update_pop3_uidl != NULL)
		p->v.update_pop3_uidl(mail, uidl);
}

void mail_expunge(struct mail *mail)
{
	struct mail_private *p = (struct mail_private *)mail;

	p->v.expunge(mail);
}

void mail_set_expunged(struct mail *mail)
{
	mail_storage_set_error(mail->box->storage, MAIL_ERROR_EXPUNGED,
			       "Message was expunged");
	mail->expunged = TRUE;
}

void mail_precache(struct mail *mail)
{
	struct mail_private *p = (struct mail_private *)mail;

	p->v.precache(mail);
}

void mail_set_cache_corrupted(struct mail *mail, enum mail_fetch_field field)
{
	struct mail_private *p = (struct mail_private *)mail;

	p->v.set_cache_corrupted(mail, field);
}

void mail_generate_guid_128_hash(const char *guid, guid_128_t guid_128_r)
{
	unsigned char sha1_sum[SHA1_RESULTLEN];
	buffer_t buf;

	if (guid_128_from_string(guid, guid_128_r) < 0) {
		/* not 128bit hex. use a hash of it instead. */
		buffer_create_from_data(&buf, guid_128_r, GUID_128_SIZE);
		buffer_set_used_size(&buf, 0);
		sha1_get_digest(guid, strlen(guid), sha1_sum);
#if SHA1_RESULTLEN < DBOX_GUID_BIN_LEN
#  error not possible
#endif
		buffer_append(&buf,
			      sha1_sum + SHA1_RESULTLEN - GUID_128_SIZE,
			      GUID_128_SIZE);
	}
}
