/* Copyright (c) 2003-2011 Dovecot authors, see the included COPYING file */

#include "lib.h"
#include "array.h"
#include "mail-index-private.h"

int mail_index_map_parse_extensions(struct mail_index_map *map)
{
	struct mail_index *index = map->index;
	const struct mail_index_ext_header *ext_hdr;
	unsigned int i, old_count, offset;
	const char *name, *error;
	uint32_t ext_id, ext_offset;

	/* extension headers always start from 64bit offsets, so if base header
	   doesn't happen to be 64bit aligned we'll skip some bytes */
	offset = MAIL_INDEX_HEADER_SIZE_ALIGN(map->hdr.base_header_size);
	if (offset >= map->hdr.header_size && map->extension_pool == NULL) {
		/* nothing to do, skip allocatations and all */
		return 0;
	}

	old_count = array_count(&index->extensions);
	mail_index_map_init_extbufs(map, old_count + 5);

	ext_id = (uint32_t)-1;
	for (i = 0; i < old_count; i++)
		array_append(&map->ext_id_map, &ext_id, 1);

	for (i = 0; offset < map->hdr.header_size; i++) {
		ext_offset = offset;

		if (mail_index_map_ext_get_next(map, &offset,
						&ext_hdr, &name) < 0) {
			mail_index_set_error(index, "Corrupted index file %s: "
				"Header extension #%d (%s) goes outside header",
				index->filepath, i, name);
			return -1;
		}

		if (mail_index_map_ext_hdr_check(&map->hdr, ext_hdr,
						 name, &error) < 0) {
			mail_index_set_error(index, "Corrupted index file %s: "
					     "Broken extension #%d (%s): %s",
					     index->filepath, i, name, error);
			return -1;
		}
		if (mail_index_map_lookup_ext(map, name, NULL)) {
			mail_index_set_error(index, "Corrupted index file %s: "
				"Duplicate header extension %s",
				index->filepath, name);
			return -1;
		}

		mail_index_map_register_ext(map, name, ext_offset, ext_hdr);
	}
	return 0;
}

int mail_index_map_parse_keywords(struct mail_index_map *map)
{
	struct mail_index *index = map->index;
	const struct mail_index_ext *ext;
	const struct mail_index_keyword_header *kw_hdr;
	const struct mail_index_keyword_header_rec *kw_rec;
	const char *name;
	unsigned int i, name_area_end_offset, old_count;
	uint32_t idx;

	if (!mail_index_map_lookup_ext(map, MAIL_INDEX_EXT_KEYWORDS, &idx)) {
		if (array_is_created(&map->keyword_idx_map))
			array_clear(&map->keyword_idx_map);
		return 0;
	}
	ext = array_idx(&map->extensions, idx);

	/* Extension header contains:
	   - struct mail_index_keyword_header
	   - struct mail_index_keyword_header_rec * keywords_count
	   - const char names[] * keywords_count
	*/
	i_assert(ext->hdr_offset < map->hdr.header_size);
	kw_hdr = CONST_PTR_OFFSET(map->hdr_base, ext->hdr_offset);
	kw_rec = (const void *)(kw_hdr + 1);
	name = (const char *)(kw_rec + kw_hdr->keywords_count);

	old_count = !array_is_created(&map->keyword_idx_map) ? 0 :
		array_count(&map->keyword_idx_map);

	/* Keywords can only be added into same mapping. Removing requires a
	   new mapping (recreating the index file) */
	if (kw_hdr->keywords_count == old_count) {
		/* nothing changed */
		return 0;
	}

	/* make sure the header is valid */
	if (kw_hdr->keywords_count < old_count) {
		mail_index_set_error(index, "Corrupted index file %s: "
				     "Keywords removed unexpectedly",
				     index->filepath);
		return -1;
	}

	if ((size_t)(name - (const char *)kw_hdr) > ext->hdr_size) {
		mail_index_set_error(index, "Corrupted index file %s: "
				     "keywords_count larger than header size",
				     index->filepath);
		return -1;
	}

	name_area_end_offset = (const char *)kw_hdr + ext->hdr_size - name;
	for (i = 0; i < kw_hdr->keywords_count; i++) {
		if (kw_rec[i].name_offset > name_area_end_offset) {
			mail_index_set_error(index, "Corrupted index file %s: "
				"name_offset points outside allocated header",
				index->filepath);
			return -1;
		}
	}
	if (name[name_area_end_offset-1] != '\0') {
		mail_index_set_error(index, "Corrupted index file %s: "
				     "Keyword header doesn't end with NUL",
				     index->filepath);
		return -1;
	}

	/* create file -> index mapping */
	if (!array_is_created(&map->keyword_idx_map)) 
		i_array_init(&map->keyword_idx_map, kw_hdr->keywords_count);

#ifdef DEBUG
	/* Check that existing headers are still the same. It's behind DEBUG
	   since it's pretty useless waste of CPU normally. */
	for (i = 0; i < array_count(&map->keyword_idx_map); i++) {
		const char *keyword = name + kw_rec[i].name_offset;
		const unsigned int *old_idx;
		unsigned int kw_idx;

		old_idx = array_idx(&map->keyword_idx_map, i);
		if (!mail_index_keyword_lookup(index, keyword, &kw_idx) ||
		    kw_idx != *old_idx) {
			mail_index_set_error(index, "Corrupted index file %s: "
					     "Keywords changed unexpectedly",
					     index->filepath);
			return -1;
		}
	}
#endif
	/* Register the newly seen keywords */
	i = array_count(&map->keyword_idx_map);
	for (; i < kw_hdr->keywords_count; i++) {
		const char *keyword = name + kw_rec[i].name_offset;
		unsigned int kw_idx;

		if (*keyword == '\0') {
			mail_index_set_error(index, "Corrupted index file %s: "
				"Empty keyword name in header",
				index->filepath);
			return -1;
		}
		mail_index_keyword_lookup_or_create(index, keyword, &kw_idx);
		array_append(&map->keyword_idx_map, &kw_idx, 1);
	}
	return 0;
}

bool mail_index_check_header_compat(struct mail_index *index,
				    const struct mail_index_header *hdr,
				    uoff_t file_size)
{
        enum mail_index_header_compat_flags compat_flags = 0;

#if !WORDS_BIGENDIAN
	compat_flags |= MAIL_INDEX_COMPAT_LITTLE_ENDIAN;
#endif

	if (hdr->major_version != MAIL_INDEX_MAJOR_VERSION) {
		/* major version change - handle silently(?) */
		return FALSE;
	}
	if ((hdr->flags & MAIL_INDEX_HDR_FLAG_CORRUPTED) != 0) {
		/* we've already complained about it */
		return FALSE;
	}

	if (hdr->compat_flags != compat_flags) {
		/* architecture change */
		mail_index_set_error(index, "Rebuilding index file %s: "
				     "CPU architecture changed",
				     index->filepath);
		return FALSE;
	}

	if (hdr->base_header_size < MAIL_INDEX_HEADER_MIN_SIZE ||
	    hdr->header_size < hdr->base_header_size) {
		mail_index_set_error(index, "Corrupted index file %s: "
				     "Corrupted header sizes (base %u, full %u)",
				     index->filepath, hdr->base_header_size,
				     hdr->header_size);
		return FALSE;
	}
	if (hdr->header_size > file_size) {
		mail_index_set_error(index, "Corrupted index file %s: "
				     "Corrupted header size (%u > %"PRIuUOFF_T")",
				     index->filepath, hdr->header_size,
				     file_size);
		return FALSE;
	}

	if (hdr->indexid != index->indexid) {
		if (index->indexid != 0) {
			mail_index_set_error(index, "Index file %s: "
					     "indexid changed: %u -> %u",
					     index->filepath, index->indexid,
					     hdr->indexid);
		}
		index->indexid = hdr->indexid;
		mail_transaction_log_indexid_changed(index->log);
	}

	return TRUE;
}

static void mail_index_map_clear_recent_flags(struct mail_index_map *map)
{
	struct mail_index_record *rec;
	unsigned int i;

	for (i = 0; i < map->hdr.messages_count; i++) {
		rec = MAIL_INDEX_MAP_IDX(map, i);
		rec->flags &= ~MAIL_RECENT;
	}
}

int mail_index_map_check_header(struct mail_index_map *map)
{
	struct mail_index *index = map->index;
	const struct mail_index_header *hdr = &map->hdr;

	if (!mail_index_check_header_compat(index, hdr, (uoff_t)-1))
		return -1;

	/* following some extra checks that only take a bit of CPU */
	if (hdr->record_size < sizeof(struct mail_index_record)) {
		mail_index_set_error(index, "Corrupted index file %s: "
				     "record_size too small: %u < %"PRIuSIZE_T,
				     index->filepath, hdr->record_size,
				     sizeof(struct mail_index_record));
		return -1;
	}

	if (hdr->uid_validity == 0 && hdr->next_uid != 1)
		return 0;
	if (hdr->next_uid == 0)
		return 0;
	if (hdr->messages_count > map->rec_map->records_count)
		return 0;

	if (hdr->seen_messages_count > hdr->messages_count ||
	    hdr->deleted_messages_count > hdr->messages_count)
		return 0;
	switch (hdr->minor_version) {
	case 0:
		/* upgrade silently from v1.0 */
		map->hdr.unused_old_recent_messages_count = 0;
		if (hdr->first_recent_uid == 0)
			map->hdr.first_recent_uid = 1;
		index->need_recreate = TRUE;
		/* fall through */
	case 1:
		/* pre-v1.1.rc6: make sure the \Recent flags are gone */
		mail_index_map_clear_recent_flags(map);
		map->hdr.minor_version = MAIL_INDEX_MINOR_VERSION;
	}
	if (hdr->first_recent_uid == 0 ||
	    hdr->first_recent_uid > hdr->next_uid ||
	    hdr->first_unseen_uid_lowwater > hdr->next_uid ||
	    hdr->first_deleted_uid_lowwater > hdr->next_uid)
		return 0;

	if (hdr->messages_count > 0) {
		/* last message's UID must be smaller than next_uid.
		   also make sure it's not zero. */
		const struct mail_index_record *rec;

		rec = MAIL_INDEX_MAP_IDX(map, hdr->messages_count-1);
		if (rec->uid == 0 || rec->uid >= hdr->next_uid)
			return 0;
	}

	return 1;
}
