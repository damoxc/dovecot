#ifndef __MAIL_TREE_H
#define __MAIL_TREE_H

typedef struct _MailTreeHeader MailTreeHeader;
typedef struct _MailTreeNode MailTreeNode;

enum nodecolor { BLACK = 0, RED };

struct _MailTree {
	MailIndex *index;

	int fd;
	char *filepath;

	void *mmap_base;
	MailTreeNode *node_base;
	size_t mmap_used_length;
	size_t mmap_full_length;

        MailTreeHeader *header;
	unsigned int anon_mmap:1;
	unsigned int modified:1;
};

struct _MailTreeHeader {
	unsigned int indexid;

	unsigned int root;
	unsigned int unused_root;

	unsigned int alignment;
	uoff_t used_file_size;
};

struct _MailTreeNode {
	unsigned int left;
	unsigned int right;
	unsigned int up;
	unsigned int color;

	/* number of child nodes + 1, used to figure out message
	   sequence numbers */
	unsigned int node_count;

	unsigned int key;
	unsigned int value;
};

int mail_tree_create(MailIndex *index);
int mail_tree_open_or_create(MailIndex *index);
void mail_tree_free(MailTree *tree);

int mail_tree_rebuild(MailTree *tree);
int mail_tree_sync_file(MailTree *tree);

/* Find first existing UID in range. Returns (unsigned int)-1 if not found. */
unsigned int mail_tree_lookup_uid_range(MailTree *tree, unsigned int *seq_r,
					unsigned int first_uid,
					unsigned int last_uid);

/* Find message by sequence number. Returns (unsigned int)-1 if not found. */
unsigned int mail_tree_lookup_sequence(MailTree *tree, unsigned int seq);

/* Insert a new record in tree. */
int mail_tree_insert(MailTree *tree, unsigned int uid, unsigned int index);

/* Update existing record in tree. */
int mail_tree_update(MailTree *tree, unsigned int uid, unsigned int index);

/* Delete record from tree. */
void mail_tree_delete(MailTree *tree, unsigned int uid);

/* private: */
int _mail_tree_set_corrupted(MailTree *tree, const char *fmt, ...);
int _mail_tree_grow(MailTree *tree);

#endif
