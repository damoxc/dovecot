/* Copyright (c) 2009 Dovecot authors, see the included COPYING file */

#include "lib.h"
#include "sha1.h"
#include "dsync-data.h"
#include "test-dsync-common.h"

const uint8_t test_mailbox_guid1[MAILBOX_GUID_SIZE] = {
	0x12, 0x34, 0x45, 0x67, 0x89, 0xab, 0xcd, 0xef,
	0x21, 0x43, 0x54, 0x76, 0x98, 0xba, 0xdc, 0xfe
};

const uint8_t test_mailbox_guid2[MAILBOX_GUID_SIZE] = {
	0xa3, 0xbd, 0x78, 0x24, 0xde, 0xfe, 0x08, 0xf7,
	0xac, 0xc7, 0xca, 0x8c, 0xe7, 0x39, 0xdb, 0xca
};

bool dsync_messages_equal(const struct dsync_message *m1,
			  const struct dsync_message *m2)
{
	unsigned int i;

	if (strcmp(m1->guid, m2->guid) != 0 ||
	    m1->uid != m2->uid || m1->flags != m2->flags ||
	    m1->modseq != m2->modseq || m1->save_date != m2->save_date)
		return FALSE;

	if (m1->keywords == m2->keywords)
		return TRUE;
	if (m1->keywords == NULL)
		return m2->keywords == NULL || m2->keywords[0] == NULL;
	if (m2->keywords == NULL)
		return m1->keywords[0] == NULL;

	for (i = 0; m1->keywords[i] != NULL && m2->keywords[i] != NULL; i++) {
		if (strcasecmp(m1->keywords[i], m2->keywords[i]) != 0)
			return FALSE;
	}
	return m1->keywords[i] == NULL && m2->keywords[i] == NULL;
}

bool dsync_mailboxes_equal(const struct dsync_mailbox *box1,
			   const struct dsync_mailbox *box2)
{
	if (strcmp(box1->name, box2->name) != 0 ||
	    memcmp(box1->guid.guid, box2->guid.guid, sizeof(box1->guid.guid)) != 0 ||
	    box1->uid_validity != box2->uid_validity ||
	    box1->uid_next != box2->uid_next ||
	    box1->highest_modseq != box2->highest_modseq)
		return FALSE;
	return TRUE;
}

void mail_generate_guid_128_hash(const char *guid,
				 uint8_t guid_128[MAIL_GUID_128_SIZE])
{
	unsigned char sha1_sum[SHA1_RESULTLEN];

	sha1_get_digest(guid, strlen(guid), sha1_sum);
	memcpy(guid_128, sha1_sum, MAIL_GUID_128_SIZE);
}

bool mail_guid_128_is_empty(const uint8_t guid_128[MAIL_GUID_128_SIZE] ATTR_UNUSED)
{
	return FALSE;
}
