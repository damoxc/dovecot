#ifndef GUID_H
#define GUID_H

#define GUID_128_SIZE 16
typedef uint8_t guid_128_t[GUID_128_SIZE];

#define GUID_128_HOST_HASH_SIZE 4

/* Generate a GUID (contains host name) */
const char *guid_generate(void);
/* Generate 128 bit GUID */
void guid_128_generate(guid_128_t guid_r);
/* Returns TRUE if GUID is empty (not set / unknown). */
bool guid_128_is_empty(const guid_128_t guid);
/* Returns TRUE if two GUIDs are equal. */
bool guid_128_equals(const guid_128_t guid1, const guid_128_t guid2);

/* Returns GUID as a hex string. */
const char *guid_128_to_string(const guid_128_t guid);
/* Parse GUID from a string. Returns 0 if ok, -1 if GUID isn't valid. */
int guid_128_from_string(const char *str, guid_128_t guid_r);

/* guid_128 hash/cmp functions for hash.h */
unsigned int guid_128_hash(const uint8_t *guid);
int guid_128_cmp(const uint8_t *guid1, const uint8_t *guid2);

/* Return the hash of host used by guid_128_generate(). */
void guid_128_host_hash_get(const char *host,
			    unsigned char hash_r[GUID_128_HOST_HASH_SIZE]);

#endif
