/* Copyright (C) 2005-2007 Timo Sirainen */

#include "lib.h"
#include "file-lock.h"
#include "mail-storage.h"
#include "mail-namespace.h"

#include <stdlib.h>

static void namespace_init_storage(struct mail_namespace *ns)
{
	ns->list = mail_storage_get_list(ns->storage);
	ns->prefix_len = strlen(ns->prefix);
	ns->real_sep = mailbox_list_get_hierarchy_sep(ns->list);

	if (ns->sep == '\0')
                ns->sep = ns->real_sep;

	if (ns->sep == '"' || ns->sep == '\\') {
		ns->sep_str[0] = '\\';
		ns->sep_str[1] = ns->sep;
	} else {
		ns->sep_str[0] = ns->sep;
	}
}

static struct mail_namespace *
namespace_add_env(pool_t pool, const char *data, unsigned int num,
		  const char *user, enum mail_storage_flags flags,
		  enum file_lock_method lock_method)
{
        struct mail_namespace *ns;
	const char *sep, *type, *prefix;

	ns = p_new(pool, struct mail_namespace, 1);

	sep = getenv(t_strdup_printf("NAMESPACE_%u_SEP", num));
	type = getenv(t_strdup_printf("NAMESPACE_%u_TYPE", num));
	prefix = getenv(t_strdup_printf("NAMESPACE_%u_PREFIX", num));
	if (getenv(t_strdup_printf("NAMESPACE_%u_INBOX", num)) != NULL)
		ns->flags |= NAMESPACE_FLAG_INBOX;
	if (getenv(t_strdup_printf("NAMESPACE_%u_HIDDEN", num)) != NULL)
		ns->flags |= NAMESPACE_FLAG_HIDDEN;
	if ((ns->flags & NAMESPACE_FLAG_HIDDEN) == 0 && *prefix != '\0' &&
	    getenv(t_strdup_printf("NAMESPACE_%u_LIST", num)) != NULL)
		ns->flags |= NAMESPACE_FLAG_LIST;

	if (type == NULL || *type == '\0' || strncmp(type, "private", 7) == 0)
		ns->type = NAMESPACE_PRIVATE;
	else if (strncmp(type, "shared", 6) == 0)
		ns->type = NAMESPACE_SHARED;
	else if (strncmp(type, "public", 6) == 0)
		ns->type = NAMESPACE_PUBLIC;
	else {
		i_error("Unknown namespace type: %s", type);
		return NULL;
	}

	if (prefix == NULL)
		prefix = "";

	if ((flags & MAIL_STORAGE_FLAG_DEBUG) != 0) {
		i_info("Namespace: type=%s, prefix=%s, sep=%s, "
		       "inbox=%s, hidden=%s, list=%s",
		       type == NULL ? "" : type, prefix, sep == NULL ? "" : sep,
		       (ns->flags & NAMESPACE_FLAG_INBOX) ? "yes" : "no",
		       (ns->flags & NAMESPACE_FLAG_HIDDEN) ? "yes" : "no",
		       (ns->flags & NAMESPACE_FLAG_LIST) ? "yes" : "no");
	}

	ns->prefix = p_strdup(pool, prefix);
	if (mail_storage_create(ns, NULL, data, user, flags, lock_method) < 0) {
		i_error("Failed to create storage for '%s' with data: %s",
			ns->prefix, data);
		return NULL;
	}

	if (sep != NULL)
		ns->sep = *sep;
        namespace_init_storage(ns);
	return ns;
}

static bool namespaces_check(struct mail_namespace *namespaces)
{
	struct mail_namespace *ns, *inbox_ns = NULL, *private_ns = NULL;
	unsigned int private_ns_count = 0;
	char list_sep = '\0';

	for (ns = namespaces; ns != NULL; ns = ns->next) {
		if ((ns->flags & NAMESPACE_FLAG_INBOX) != 0) {
			if (inbox_ns != NULL) {
				i_error("namespace configuration error: "
					"There can be only one namespace with "
					"inbox=yes");
				return FALSE;
			}
			inbox_ns = ns;
		}
		if (ns->type == NAMESPACE_PRIVATE) {
			private_ns = ns;
			private_ns_count++;
		}
		if ((ns->flags & NAMESPACE_FLAG_LIST) != 0 &&
		    ns->prefix[strlen(ns->prefix)-1] != ns->sep) {
			i_error("namespace configuration error: "
				"list=yes requires prefix=%s "
				"to end with separator", ns->prefix);
			return FALSE;
		}
		if ((ns->flags & NAMESPACE_FLAG_LIST) != 0) {
			if (list_sep == '\0')
				list_sep = ns->sep;
			else if (list_sep != ns->sep) {
				i_error("namespace configuration error: "
					"All list=yes namespaces must use "
					"the same separator");
				return FALSE;
			}
		}
	}

	if (inbox_ns == NULL) {
		if (private_ns_count == 1) {
			/* just one private namespace. we'll assume it's
			   the INBOX namespace. */
			private_ns->flags |= NAMESPACE_FLAG_INBOX;
		} else {
			i_error("namespace configuration error: "
				"inbox=yes namespace missing");
			return FALSE;
		}
	}
	if (list_sep == '\0') {
		i_error("namespace configuration error: "
			"no list=yes namespaces");
		return FALSE;
	}
	return TRUE;
}

static struct mail_namespace *
namespaces_sort(struct mail_namespace *src)
{
	struct mail_namespace **tmp, *next, *dest = NULL;

	for (; src != NULL; src = next) {
		next = src->next;

		for (tmp = &dest; *tmp != NULL; tmp = &(*tmp)->next) {
			if (strlen(src->prefix) < strlen((*tmp)->prefix))
				break;
		}
		src->next = *tmp;
		*tmp = src;
	}
	return dest;
}

int mail_namespaces_init(pool_t pool, const char *user,
			 struct mail_namespace **namespaces_r)
{
	struct mail_namespace *namespaces, *ns, **ns_p;
	enum mail_storage_flags flags;
        enum file_lock_method lock_method;
	const char *mail, *data;
	unsigned int i;

	mail_storage_parse_env(&flags, &lock_method);
        namespaces = NULL; ns_p = &namespaces;

	/* first try NAMESPACE_* environments */
	for (i = 1; ; i++) {
		t_push();
		data = getenv(t_strdup_printf("NAMESPACE_%u", i));
		t_pop();

		if (data == NULL)
			break;

		t_push();
		*ns_p = namespace_add_env(pool, data, i, user, flags,
					  lock_method);
		t_pop();

		if (*ns_p == NULL)
			return -1;

		ns_p = &(*ns_p)->next;
	}

	if (namespaces != NULL) {
		if (!namespaces_check(namespaces))
			return -1;
		namespaces = namespaces_sort(namespaces);
		*namespaces_r = namespaces;
		return 0;
	}

	/* fallback to MAIL */
	mail = getenv("MAIL");
	if (mail == NULL) {
		/* support also maildir-specific environment */
		mail = getenv("MAILDIR");
		if (mail != NULL)
			mail = t_strconcat("maildir:", mail, NULL);
	}

	ns = p_new(pool, struct mail_namespace, 1);
	ns->type = NAMESPACE_PRIVATE;
	ns->flags = NAMESPACE_FLAG_INBOX | NAMESPACE_FLAG_LIST;
	ns->prefix = "";

	if (mail_storage_create(ns, NULL, mail, user, flags, lock_method) < 0) {
		if (mail != NULL && *mail != '\0')
			i_error("Failed to create storage with data: %s", mail);
		else {
			const char *home;

			home = getenv("HOME");
			if (home == NULL) home = "not set";

			i_error("MAIL environment missing and "
				"autodetection failed (home %s)", home);
		}
		return -1;
	}

	namespace_init_storage(ns);
	*namespaces_r = ns;
	return 0;
}

struct mail_namespace *mail_namespaces_init_empty(pool_t pool)
{
	struct mail_namespace *ns;

	ns = p_new(pool, struct mail_namespace, 1);
	ns->prefix = "";
	ns->flags = NAMESPACE_FLAG_INBOX | NAMESPACE_FLAG_LIST;
	return ns;
}

void mail_namespaces_deinit(struct mail_namespace **_namespaces)
{
	struct mail_namespace *namespaces = *_namespaces;

	*_namespaces = NULL;
	while (namespaces != NULL) {
		if (namespaces->storage != NULL)
			mail_storage_destroy(&namespaces->storage);
		namespaces = namespaces->next;
	}
}

const char *mail_namespace_fix_sep(struct mail_namespace *ns, const char *name)
{
	char *ret, *p;

	if (ns->sep == ns->real_sep)
		return name;

	ret = p_strdup(unsafe_data_stack_pool, name);
	for (p = ret; *p != '\0'; p++) {
		if (*p == ns->sep)
			*p = ns->real_sep;
	}
	return ret;
}

char mail_namespace_get_root_sep(struct mail_namespace *namespaces)
{
	while ((namespaces->flags & NAMESPACE_FLAG_LIST) == 0)
		namespaces = namespaces->next;
	return namespaces->sep;
}

static struct mail_namespace *
mail_namespace_find_int(struct mail_namespace *namespaces, const char **mailbox,
			bool show_hidden)
{
#define CHECK_VISIBILITY(ns, show_hidden) \
	(((ns)->flags & NAMESPACE_FLAG_HIDDEN) == 0 || (show_hidden))
        struct mail_namespace *ns = namespaces;
	const char *box = *mailbox;
	struct mail_namespace *best = NULL;
	size_t best_len = 0;
	bool inbox;

	inbox = strncasecmp(box, "INBOX", 5) == 0;
	if (inbox && box[5] == '\0') {
		/* find the INBOX namespace */
		*mailbox = "INBOX";
		while (ns != NULL) {
			if ((ns->flags & NAMESPACE_FLAG_INBOX) != 0 &&
			    CHECK_VISIBILITY(ns, show_hidden))
				return ns;
			if (*ns->prefix == '\0')
				best = ns;
			ns = ns->next;
		}
		return best;
	}

	for (; ns != NULL; ns = ns->next) {
		if (ns->prefix_len >= best_len &&
		    (strncmp(ns->prefix, box, ns->prefix_len) == 0 ||
		     (inbox && strncmp(ns->prefix, "INBOX", 5) == 0 &&
		      strncmp(ns->prefix+5, box+5, ns->prefix_len-5) == 0)) &&
		    CHECK_VISIBILITY(ns, show_hidden)) {
			best = ns;
			best_len = ns->prefix_len;
		}
	}

	if (best != NULL) {
		if (best_len > 0)
			*mailbox += best_len;
		else if (inbox && (box[5] == best->sep || box[5] == '\0'))
			*mailbox = t_strconcat("INBOX", box+5, NULL);

		*mailbox = mail_namespace_fix_sep(best, *mailbox);
	}

	return best;
}

struct mail_namespace *
mail_namespace_find(struct mail_namespace *namespaces, const char **mailbox)
{
	return mail_namespace_find_int(namespaces, mailbox, TRUE);
}

struct mail_namespace *
mail_namespace_find_visible(struct mail_namespace *namespaces,
			    const char **mailbox)
{
	return mail_namespace_find_int(namespaces, mailbox, FALSE);
}

struct mail_namespace *
mail_namespace_find_inbox(struct mail_namespace *namespaces)
{
	while ((namespaces->flags & NAMESPACE_FLAG_INBOX) == 0)
		namespaces = namespaces->next;
	return namespaces;
}

bool mail_namespace_update_name(struct mail_namespace *ns,
				const char **mailbox)
{
	struct mail_namespace tmp_ns = *ns;

	/* FIXME: a bit kludgy.. */
	tmp_ns.next = NULL;
	return mail_namespace_find_int(&tmp_ns, mailbox, TRUE) != NULL;
}

struct mail_namespace *
mail_namespace_find_prefix(struct mail_namespace *namespaces,
			   const char *prefix)
{
        struct mail_namespace *ns;
	unsigned int len = strlen(prefix);

	for (ns = namespaces; ns != NULL; ns = ns->next) {
		if (ns->prefix_len == len &&
		    strcmp(ns->prefix, prefix) == 0)
			return ns;
	}
	return NULL;
}

struct mail_namespace *
mail_namespace_find_prefix_nosep(struct mail_namespace *namespaces,
				 const char *prefix)
{
        struct mail_namespace *ns;
	unsigned int len = strlen(prefix);

	for (ns = namespaces; ns != NULL; ns = ns->next) {
		if (ns->prefix_len == len + 1 &&
		    strncmp(ns->prefix, prefix, len) == 0 &&
		    ns->prefix[len] == ns->sep)
			return ns;
	}
	return NULL;
}
