#ifndef MAIL_NAMESPACE_H
#define MAIL_NAMESPACE_H

#include "mail-user.h"

enum namespace_type {
	NAMESPACE_PRIVATE,
	NAMESPACE_SHARED,
	NAMESPACE_PUBLIC
};

enum namespace_flags {
	/* Namespace contains the INBOX mailbox (there can be only one) */
	NAMESPACE_FLAG_INBOX		= 0x01,
	/* Namespace is visible only by explicitly using its full prefix */
	NAMESPACE_FLAG_HIDDEN		= 0x02,
	/* Namespace prefix is visible with LIST */
	NAMESPACE_FLAG_LIST_PREFIX	= 0x04,
	/* Namespace prefix isn't visible with LIST, but child mailboxes are */
	NAMESPACE_FLAG_LIST_CHILDREN	= 0x08,
	/* Namespace uses its own subscriptions. */
	NAMESPACE_FLAG_SUBSCRIPTIONS	= 0x10,

	/* Namespace is created for internal use only. */
	NAMESPACE_FLAG_INTERNAL		= 0x1000,
	/* Namespace was created automatically (for shared mailboxes) */
	NAMESPACE_FLAG_AUTOCREATED	= 0x2000,
	/* Namespace has at least some usable mailboxes. Autocreated namespaces
	   that don't have usable mailboxes may be removed automatically. */
	NAMESPACE_FLAG_USABLE		= 0x4000,
	/* Automatically created namespace for a user that doesn't exist. */
	NAMESPACE_FLAG_UNUSABLE		= 0x8000
};

struct mail_namespace {
	/* Namespaces are sorted by their prefix length, "" comes first */
	struct mail_namespace *next;

        enum namespace_type type;
	char sep, real_sep, sep_str[3];
	enum namespace_flags flags;

	char *prefix;
	size_t prefix_len;

	/* If non-NULL, this points to a namespace with identical mail location
	   and it should be considered as the primary way to access the
	   mailboxes. This allows for example FTS plugin to avoid duplicating
	   indexes for same mailboxes when they're accessed via different
	   namespaces. */
	struct mail_namespace *alias_for;
	/* alias_for->alias_chain_next starts each chain. The chain goes
	   through all namespaces that have the same alias_for. */
	struct mail_namespace *alias_chain_next;

	struct mail_user *user, *owner;
	struct mailbox_list *list;
	/* FIXME: we should support multiple storages in one namespace */
	struct mail_storage *storage;
};

/* Called after namespaces has been created */
extern void (*hook_mail_namespaces_created)(struct mail_namespace *namespaces);

int mail_namespaces_init(struct mail_user *user);
struct mail_namespace *mail_namespaces_init_empty(struct mail_user *user);
/* Deinitialize all namespaces. mail_user_deinit() calls this automatically
   for user's namespaces. */
void mail_namespaces_deinit(struct mail_namespace **namespaces);

/* Destroy a single namespace and remove it from user's namespaces list. */
void mail_namespace_destroy(struct mail_namespace *ns);

/* Update hierarchy separators in given name to real_sep characters. */
const char *mail_namespace_fix_sep(struct mail_namespace *ns, const char *name);
/* Write virtual mailbox name to dest and return it. Separators are changed to
   virtual ones and namespace prefix is inserted except for INBOX. */
const char *mail_namespace_get_vname(struct mail_namespace *ns, string_t *dest,
				     const char *name);

/* Returns the hierarchy separator for mailboxes that are listed at root. */
char mail_namespace_get_root_sep(const struct mail_namespace *namespaces)
	ATTR_PURE;

/* Returns namespace based on the mailbox name's prefix. Updates mailbox to
   be a valid name inside the namespace (prefix is skipped, hierarchy separator
   is changed to real_sep). If no namespaces were found, returns NULL. */
struct mail_namespace *
mail_namespace_find(struct mail_namespace *namespaces, const char **mailbox);
/* Like above, but ignore hidden namespaces. */
struct mail_namespace *
mail_namespace_find_visible(struct mail_namespace *namespaces,
			    const char **mailbox);
/* Like above, but find only from namespaces with subscriptions flag set. */
struct mail_namespace *
mail_namespace_find_subscribable(struct mail_namespace *namespaces,
				 const char **mailbox);
/* Like above, but find only from namespaces with subscriptions flag not set. */
struct mail_namespace *
mail_namespace_find_unsubscribable(struct mail_namespace *namespaces,
				   const char **mailbox);
/* Returns the INBOX namespace */
struct mail_namespace *
mail_namespace_find_inbox(struct mail_namespace *namespaces);
/* Returns TRUE if the given namespace matches the mailbox's prefix.
   Updates mailbox name to be a valid name inside the namespace. */
bool mail_namespace_update_name(const struct mail_namespace *ns,
				const char **mailbox);

/* Find a namespace with given prefix. */
struct mail_namespace *
mail_namespace_find_prefix(struct mail_namespace *namespaces,
			   const char *prefix);
/* Like _find_prefix(), but ignore trailing separator */
struct mail_namespace *
mail_namespace_find_prefix_nosep(struct mail_namespace *namespaces,
				 const char *prefix);

/* Called internally by mail_storage_create(). */
void mail_namespace_init_storage(struct mail_namespace *ns);

#endif
