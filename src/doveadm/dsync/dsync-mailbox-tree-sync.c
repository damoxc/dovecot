/* Copyright (c) 2012 Dovecot authors, see the included COPYING file */

#include "lib.h"
#include "array.h"
#include "buffer.h"
#include "str.h"
#include "md5.h"
#include "hex-binary.h"
#include "aqueue.h"
#include "hash.h"
#include "dsync-mailbox-tree-private.h"

#define TEMP_MAX_NAME_LEN 100
#define TEMP_SUFFIX_MAX_LEN (sizeof("temp-")-1 + 8)
#define TEMP_SUFFIX_FORMAT "temp-%x"

struct dsync_mailbox_tree_bfs_iter {
	struct dsync_mailbox_tree *tree;

	ARRAY(struct dsync_mailbox_node *) queue_arr;
	struct aqueue *queue;
	struct dsync_mailbox_node *cur;
};

struct dsync_mailbox_tree_sync_ctx {
	pool_t pool;
	struct dsync_mailbox_tree *local_tree, *remote_tree;

	ARRAY(struct dsync_mailbox_tree_sync_change) changes;
	unsigned int change_idx;
};

static struct dsync_mailbox_tree_bfs_iter *
dsync_mailbox_tree_bfs_iter_init(struct dsync_mailbox_tree *tree)
{
	struct dsync_mailbox_tree_bfs_iter *iter;

	iter = i_new(struct dsync_mailbox_tree_bfs_iter, 1);
	iter->tree = tree;
	i_array_init(&iter->queue_arr, 32);
	iter->queue = aqueue_init(&iter->queue_arr.arr);
	iter->cur = tree->root.first_child;
	return iter;
}

static bool
dsync_mailbox_tree_bfs_iter_next(struct dsync_mailbox_tree_bfs_iter *iter,
				 struct dsync_mailbox_node **node_r)
{
	struct dsync_mailbox_node *const *nodep;

	if (iter->cur == NULL) {
		if (aqueue_count(iter->queue) == 0)
			return FALSE;
		nodep = array_idx(&iter->queue_arr, aqueue_idx(iter->queue, 0));
		iter->cur = *nodep;
		aqueue_delete_tail(iter->queue);
	}
	*node_r = iter->cur;

	if (iter->cur->first_child != NULL)
		aqueue_append(iter->queue, &iter->cur->first_child);
	iter->cur = iter->cur->next;
	return TRUE;
}

static void
dsync_mailbox_tree_bfs_iter_deinit(struct dsync_mailbox_tree_bfs_iter **_iter)
{
	struct dsync_mailbox_tree_bfs_iter *iter = *_iter;

	*_iter = NULL;

	aqueue_deinit(&iter->queue);
	array_free(&iter->queue_arr);
	i_free(iter);
}

static void
sync_add_dir_change(struct dsync_mailbox_tree_sync_ctx *ctx,
		    const struct dsync_mailbox_node *node,
		    enum dsync_mailbox_tree_sync_type type)
{
	struct dsync_mailbox_tree_sync_change *change;
	const char *name;

	name = dsync_mailbox_node_get_full_name(ctx->local_tree, node);

	change = array_append_space(&ctx->changes);
	change->type = type;
	change->ns = node->ns;
	change->full_name = p_strdup(ctx->pool, name);
}

static void
sync_add_create_change(struct dsync_mailbox_tree_sync_ctx *ctx,
		       const struct dsync_mailbox_node *node, const char *name)
{
	struct dsync_mailbox_tree_sync_change *change;

	change = array_append_space(&ctx->changes);
	change->type = DSYNC_MAILBOX_TREE_SYNC_TYPE_CREATE_BOX;
	change->ns = node->ns;
	change->full_name = p_strdup(ctx->pool, name);
	memcpy(change->mailbox_guid, node->mailbox_guid,
	       sizeof(change->mailbox_guid));
	change->uid_validity = node->uid_validity;
}

static void sort_siblings(ARRAY_TYPE(dsync_mailbox_node) *siblings)
{
	struct dsync_mailbox_node *const *nodes;
	unsigned int i, count;

	array_sort(siblings, dsync_mailbox_node_name_cmp);

	nodes = array_get(siblings, &count);
	if (count == 0)
		return;

	nodes[0]->parent->first_child = nodes[0];
	for (i = 1; i < count; i++)
		nodes[i-1]->next = nodes[i];
	nodes[count-1]->next = NULL;
}

static void
sync_delete_mailbox(struct dsync_mailbox_tree_sync_ctx *ctx,
		    struct dsync_mailbox_tree *tree,
		    struct dsync_mailbox_node *node)
{
	struct dsync_mailbox_tree *other_tree;
	struct dsync_mailbox_node *other_node;
	struct dsync_mailbox_tree_sync_change *change;
	const char *name;
	const uint8_t *guid_p;

	other_tree = tree == ctx->local_tree ?
		ctx->remote_tree : ctx->local_tree;
	guid_p = node->mailbox_guid;
	other_node = hash_table_lookup(other_tree->guid_hash, guid_p);
	if (other_node == NULL) {
		/* doesn't exist / already deleted */
	} else if (other_tree == ctx->local_tree) {
		/* delete this mailbox locally */
		change = array_append_space(&ctx->changes);
		change->type = DSYNC_MAILBOX_TREE_SYNC_TYPE_DELETE_BOX;
		change->ns = other_node->ns;
		name = dsync_mailbox_node_get_full_name(other_tree, other_node);
		change->full_name = p_strdup(ctx->pool, name);
		memcpy(change->mailbox_guid, node->mailbox_guid,
		       sizeof(change->mailbox_guid));
	}

	/* for the rest of this sync assume that the mailbox has
	   already been deleted */
	if (other_node != NULL) {
		hash_table_remove(other_tree->guid_hash, guid_p);
		other_node->existence = DSYNC_MAILBOX_NODE_DELETED;
		memset(other_node->mailbox_guid, 0,
		       sizeof(other_node->mailbox_guid));
	}
	memset(node->mailbox_guid, 0, sizeof(node->mailbox_guid));
	node->uid_validity = 0;
}

static void
sync_tree_sort_and_delete_mailboxes(struct dsync_mailbox_tree_sync_ctx *ctx,
				    struct dsync_mailbox_tree *tree)
{
	struct dsync_mailbox_tree_bfs_iter *iter;
	struct dsync_mailbox_node *node, *parent = NULL;
	ARRAY_TYPE(dsync_mailbox_node) siblings;

	t_array_init(&siblings, 64);

	iter = dsync_mailbox_tree_bfs_iter_init(tree);
	while (dsync_mailbox_tree_bfs_iter_next(iter, &node)) {
		if (node->parent != parent) {
			sort_siblings(&siblings);
			array_clear(&siblings);
			parent = node->parent;
		}
		if (node->existence == DSYNC_MAILBOX_NODE_DELETED &&
		    !dsync_mailbox_node_is_dir(node))
			sync_delete_mailbox(ctx, tree, node);
		array_append(&siblings, &node, 1);
	}
	sort_siblings(&siblings);
	dsync_mailbox_tree_bfs_iter_deinit(&iter);
}

static bool node_names_equal(const struct dsync_mailbox_node *n1,
			     const struct dsync_mailbox_node *n2)
{
	while (n1 != NULL && n2 != NULL) {
		if (strcmp(n1->name, n2->name) != 0)
			return FALSE;
		n1 = n1->parent;
		n2 = n2->parent;
	}
	return n1 == NULL && n2 == NULL;
}

static void
dsync_mailbox_tree_node_attach_sorted(struct dsync_mailbox_node *node,
				      struct dsync_mailbox_node *parent)
{
	struct dsync_mailbox_node **p;

	node->parent = parent;
	for (p = &parent->first_child; *p != NULL; p = &(*p)->next) {
		if (dsync_mailbox_node_name_cmp(p, &node) > 0)
			break;
	}
	node->next = *p;
	*p = node;
}

static void
dsync_mailbox_tree_node_move_sorted(struct dsync_mailbox_node *node,
				    struct dsync_mailbox_node *parent)
{
	/* detach from old parent */
	dsync_mailbox_tree_node_detach(node);
	/* attach to new parent */
	dsync_mailbox_tree_node_attach_sorted(node, parent);
}

static struct dsync_mailbox_node *
sorted_tree_get(struct dsync_mailbox_tree *tree, const char *name)
{
	struct dsync_mailbox_node *node, *parent, *ret;

	node = ret = dsync_mailbox_tree_get(tree, name);
	while (node->parent != NULL &&
	       node->existence == DSYNC_MAILBOX_NODE_NONEXISTENT) {
		parent = node->parent;
		dsync_mailbox_tree_node_detach(node);
		dsync_mailbox_tree_node_attach_sorted(node, parent);
		node = parent;
	}
	return ret;
}

static struct dsync_mailbox_node *
sync_node_new(struct dsync_mailbox_tree *tree,
	      struct dsync_mailbox_node **pos,
	      struct dsync_mailbox_node *parent,
	      const struct dsync_mailbox_node *src)
{
	struct dsync_mailbox_node *node;

	node = p_new(tree->pool, struct dsync_mailbox_node, 1);
	node->existence = DSYNC_MAILBOX_NODE_NONEXISTENT;
	node->name = p_strdup(tree->pool, src->name);
	node->sync_temporary_name = src->sync_temporary_name;
	node->ns = src->ns;
	node->parent = parent;
	node->next = *pos;
	*pos = node;
	return node;
}

static struct dsync_mailbox_node *
sorted_tree_get_by_node_name(struct dsync_mailbox_tree *tree,
			     struct dsync_mailbox_tree *other_tree,
			     struct dsync_mailbox_node *other_node)
{
	const char *parent_name;

	if (other_node == &other_tree->root)
		return &tree->root;

	parent_name = dsync_mailbox_node_get_full_name(other_tree, other_node);
	return sorted_tree_get(tree, parent_name);
}

static bool node_has_child(struct dsync_mailbox_node *parent, const char *name)
{
	struct dsync_mailbox_node *node;

	for (node = parent->first_child; node != NULL; node = node->next) {
		if (strcmp(node->name, name) == 0)
			return TRUE;
	}
	return FALSE;
}

static bool
node_has_existent_children(struct dsync_mailbox_node *node, bool dirs_ok)
{
	for (node = node->first_child; node != NULL; node = node->next) {
		if (node->existence == DSYNC_MAILBOX_NODE_EXISTS &&
		    (dirs_ok || !dsync_mailbox_node_is_dir(node)))
			return TRUE;
		if (node_has_existent_children(node, dirs_ok))
			return TRUE;
	}
	return FALSE;
}

static bool node_is_existent(struct dsync_mailbox_node *node)
{
	if (node->existence == DSYNC_MAILBOX_NODE_EXISTS)
		return TRUE;
	return node_has_existent_children(node, TRUE);
}

static void
sync_rename_node_to_temp(struct dsync_mailbox_tree_sync_ctx *ctx,
			 struct dsync_mailbox_tree *tree,
			 struct dsync_mailbox_node *node,
			 struct dsync_mailbox_node *new_parent)
{
	struct dsync_mailbox_tree_sync_change *change;
	const char *old_name, *new_name, *p;
	char name[TEMP_MAX_NAME_LEN+1];
	buffer_t buf;
	unsigned int prefix_len, max_prefix_len, counter = 1;

	buffer_create_from_data(&buf, name, sizeof(name));
	max_prefix_len = TEMP_MAX_NAME_LEN - TEMP_SUFFIX_MAX_LEN - 1;
	if (node->sync_temporary_name) {
		/* the source name was also a temporary name. drop the
		 -<suffix> from it */
		p = strrchr(node->name, '-');
		i_assert(p != NULL);
		if (max_prefix_len > (size_t)(p - node->name))
			max_prefix_len = p - node->name;
	}
	str_append_n(&buf, node->name, max_prefix_len);
	str_append_c(&buf, '-');
	prefix_len = buf.used;

	do {
		str_truncate(&buf, prefix_len);
		str_printfa(&buf, TEMP_SUFFIX_FORMAT, counter++);
		/* the generated name is quite unlikely to exist,
		   but check anyway.. */
	} while (node_has_child(node->parent, str_c(&buf)));

	old_name = tree != ctx->local_tree ? NULL :
		dsync_mailbox_node_get_full_name(tree, node);

	node->name = p_strdup(tree->pool, str_c(&buf));
	node->sync_temporary_name = TRUE;
	node->last_renamed_or_created = 0;
	dsync_mailbox_tree_node_move_sorted(node, new_parent);

	if (tree == ctx->local_tree && node_is_existent(node)) {
		/* we're modifying a local tree. remember this change. */
		new_name = dsync_mailbox_node_get_full_name(tree, node);

		change = array_append_space(&ctx->changes);
		change->type = DSYNC_MAILBOX_TREE_SYNC_TYPE_RENAME;
		change->ns = node->ns;
		change->full_name = p_strdup(ctx->pool, old_name);
		change->rename_dest_name = p_strdup(ctx->pool, new_name);
	}
}

static bool node_has_parent(struct dsync_mailbox_node *node,
			    struct dsync_mailbox_node *parent)
{
	for (; node != NULL; node = node->parent) {
		if (node == parent)
			return TRUE;
	}
	return FALSE;
}

static void
sync_rename_node(struct dsync_mailbox_tree_sync_ctx *ctx,
		 struct dsync_mailbox_tree *tree,
		 struct dsync_mailbox_node *temp_node,
		 struct dsync_mailbox_node *node,
		 struct dsync_mailbox_node *other_node)
{
	struct dsync_mailbox_tree_sync_change *change;
	struct dsync_mailbox_tree *other_tree;
	struct dsync_mailbox_node *parent;
	const char *name, *other_name;

	/* move/rename node in the tree, so that its position/name is identical
	   to other_node (in other_tree) */
	other_tree = tree == ctx->local_tree ?
		ctx->remote_tree : ctx->local_tree;

	parent = sorted_tree_get_by_node_name(tree, other_tree,
					      other_node->parent);
	if (node_has_parent(parent, node)) {
		/* don't introduce a loop. temporarily rename node
		   under root. */
		sync_rename_node_to_temp(ctx, tree, node, &tree->root);
		return;
	}
	sync_rename_node_to_temp(ctx, tree, temp_node, temp_node->parent);

	/* get the old name before it's modified */
	name = dsync_mailbox_node_get_full_name(tree, node);

	/* set the new name */
	node->name = p_strdup(tree->pool, other_node->name);
	node->sync_temporary_name = other_node->sync_temporary_name;
	node->last_renamed_or_created = other_node->last_renamed_or_created;
	/* change node's parent if necessary. in any case detach+reattach it
	   sorted, because the nodes must be sorted by name, and the node's
	   name (or its parent) changed. */
	dsync_mailbox_tree_node_move_sorted(node, parent);

	if (tree == ctx->local_tree && node_is_existent(node)) {
		/* we're modifying a local tree. remember this change. */
		other_name = dsync_mailbox_node_get_full_name(other_tree, other_node);

		change = array_append_space(&ctx->changes);
		change->type = DSYNC_MAILBOX_TREE_SYNC_TYPE_RENAME;
		change->ns = node->ns;
		change->full_name = p_strdup(ctx->pool, name);
		change->rename_dest_name = p_strdup(ctx->pool, other_name);
	}
}

static int node_mailbox_guids_cmp2(struct dsync_mailbox_node *node1,
				   struct dsync_mailbox_node *node2)
{
	int ret;

	while (node1 != NULL && node2 != NULL) {
		if (node1->existence == DSYNC_MAILBOX_NODE_EXISTS &&
		    node2->existence != DSYNC_MAILBOX_NODE_EXISTS)
			return -1;
		if (node2->existence == DSYNC_MAILBOX_NODE_EXISTS &&
		    node1->existence != DSYNC_MAILBOX_NODE_EXISTS)
			return 1;

		ret = memcmp(node1->mailbox_guid, node2->mailbox_guid,
			     sizeof(node1->mailbox_guid));
		if (ret != 0)
			return ret;

		ret = node_mailbox_guids_cmp2(node1->first_child,
					      node2->first_child);
		if (ret != 0)
			return ret;
		node1 = node1->next;
		node2 = node2->next;
	}
	if (node1 == NULL && node2 == NULL)
		return 0;
	return node1 != NULL ? -1 : 1;
}

static int node_mailbox_guids_cmp(struct dsync_mailbox_node *node1,
				  struct dsync_mailbox_node *node2)
{
	int ret;

	ret = node_mailbox_guids_cmp2(node1, node2);
	i_assert(ret != 0);
	return ret;
}

static time_t nodes_get_timestamp(struct dsync_mailbox_node *node1,
				  struct dsync_mailbox_node *node2)
{
	time_t ts = 0;

	/* avoid using temporary names in case all the timestamps are 0 */
	if (node1 != NULL && !node1->sync_temporary_name)
		ts = node1->last_renamed_or_created + 1;
	if (node2 != NULL && !node2->sync_temporary_name &&
	    ts <= node2->last_renamed_or_created)
		ts = node2->last_renamed_or_created + 1;
	return ts;
}

static bool ATTR_NULL(3, 4)
sync_rename_lower_ts(struct dsync_mailbox_tree_sync_ctx *ctx,
		     struct dsync_mailbox_node *local_node1,
		     struct dsync_mailbox_node *remote_node1,
		     struct dsync_mailbox_node *local_node2,
		     struct dsync_mailbox_node *remote_node2)
{
	time_t local_ts, remote_ts;

	/* We're scanning the tree at the position of local_node1
	   and remote_node2. They have identical names. We also know that
	   local_node1&remote_node1 and local_node2&remote_node2 are "the same"
	   either because their GUIDs or (in case of one being a directory)
	   their childrens' GUIDs match. We don't know where local_node2 or
	   remote_node1 are located in the mailbox tree, or if they exist
	   at all. Note that node1 and node2 may be the same node pointers. */
	i_assert(strcmp(local_node1->name, remote_node2->name) == 0);

	local_ts = nodes_get_timestamp(local_node1, local_node2);
	remote_ts = nodes_get_timestamp(remote_node1, remote_node2);

	/* The algorithm must be deterministic regardless of the sync direction,
	   so in case the timestamps are equal we need to resort to looking at
	   the other data. We'll start by looking at the nodes' mailbox GUIDs,
	   but if both of them don't exist continue looking into their
	   children. */
	if (local_ts > remote_ts ||
	    (local_ts == remote_ts &&
	     node_mailbox_guids_cmp(local_node1, remote_node2) < 0)) {
		/* local nodes have a higher timestamp. we only want to do
		   renames where the destination parent is the current node's
		   (local_node1/remote_node2) parent. */

		/* Numbers are GUIDs, letters are mailbox names:

		   local 1A <-name conflict-> remote 2A
		   local 2B <- potentially -> remote 1[BC]

		   Here we want to preserve the local 1A & 2B names: */
		if (local_node2 == NULL) {
			/* local : 1A
			   remote: 1B, 2A -> 2A-temp, 1A */
			sync_rename_node(ctx, ctx->remote_tree, remote_node2,
					 remote_node1, local_node1);
			return TRUE;
		} else if (remote_node1 == remote_node2) {
			/* FIXME: this fixes an infinite loop when in
			   rename2 test, think it through why :) */
		} else if (remote_node1 != NULL) {
			/* a) local_node1->parent == local_node2->parent

			   local : 1A, 2B
			   remote: 1B, 2A     -> 2A-temp, 1A(, 2B)
			   remote: 1C, 2A     -> 2B, 1A
			   remote: 1C, 2A, 3B -> 2A-temp, 1A(, 3B-temp, 2B)

			   b) local_node1->parent != local_node2->parent

			   local : 1X/A, 2Y/B
			   remote: 1Y/B, 2X/A       -> 2X/A-temp, 1X/A(, 2Y/B)
			   remote: 1Z/C, 2X/A       -> 2X/A-temp, 1X/A
			   remote: 1Z/C, 2X/A, 3Y/B -> 2X/A-temp, 1X/A

			   We can handle all of these more easily by simply
			   always renaming 2 to a temporary name and handling
			   it when we reach B handling. */
			sync_rename_node(ctx, ctx->remote_tree, remote_node2,
					 remote_node1, local_node1);
			return TRUE;
		} else {
			/* local : 1A, 2B
			   remote:     2A     -> (2B)
			   remote:     2A, 3B -> (3B-temp, 2B) */
		}
	} else {
		/* remote nodes have a higher timestamp */
		if (remote_node1 == NULL) {
			sync_rename_node(ctx, ctx->local_tree, local_node1,
					 local_node2, remote_node2);
			return TRUE;
		} else if (local_node2 == local_node1) {
		} else if (local_node2 != NULL) {
			sync_rename_node(ctx, ctx->local_tree, local_node1,
					 local_node2, remote_node2);
			return TRUE;
		}
	}
	return FALSE;
}

static void
add_missing_mailbox(struct dsync_mailbox_tree_sync_ctx *ctx,
		    struct dsync_mailbox_tree *tree,
		    const struct dsync_mailbox_node *src)
{
	struct dsync_mailbox_node *node;

	node = sync_node_new(tree, &tree->root.first_child, &tree->root, src);
	sync_rename_node_to_temp(ctx, tree, node, node->parent);

	node->existence = DSYNC_MAILBOX_NODE_EXISTS;
	node->uid_validity = src->uid_validity;
	memcpy(node->mailbox_guid, src->mailbox_guid,
	       sizeof(node->mailbox_guid));
	if (tree == ctx->local_tree) {
		sync_add_create_change(ctx, node,
			dsync_mailbox_node_get_full_name(tree, node));
	}
	if (dsync_mailbox_tree_guid_hash_add(tree, node) < 0)
		i_unreached();
}

static bool sync_rename_conflict(struct dsync_mailbox_tree_sync_ctx *ctx,
				 struct dsync_mailbox_node *local_node1,
				 struct dsync_mailbox_node *remote_node2)
{
	struct dsync_mailbox_node *local_node2, *remote_node1;
	const uint8_t *guid_p;

	guid_p = local_node1->mailbox_guid;
	remote_node1 = hash_table_lookup(ctx->remote_tree->guid_hash, guid_p);
	guid_p = remote_node2->mailbox_guid;
	local_node2 = hash_table_lookup(ctx->local_tree->guid_hash, guid_p);

	/* FIXME: kludge to avoid problems where one of the mailboxes
	 doesn't exist yet */
	if (local_node2 == NULL &&
	    remote_node2->existence == DSYNC_MAILBOX_NODE_EXISTS &&
	    !dsync_mailbox_node_is_dir(remote_node2)) {
		add_missing_mailbox(ctx, ctx->local_tree, remote_node2);
		return TRUE;
	}
	if (remote_node1 == NULL &&
	    local_node1->existence == DSYNC_MAILBOX_NODE_EXISTS &&
	    !dsync_mailbox_node_is_dir(local_node1)) {
		add_missing_mailbox(ctx, ctx->remote_tree, local_node1);
		return TRUE;
	}

	if (remote_node1 != NULL || local_node2 != NULL) {
		/* conflicting name, rename the one with lower timestamp */
		return sync_rename_lower_ts(ctx, local_node1, remote_node1,
					    local_node2, remote_node2);
	} else if (dsync_mailbox_node_is_dir(local_node1) ||
		   dsync_mailbox_node_is_dir(remote_node2)) {
		/* one of the nodes is a directory, and the other is a mailbox
		   that doesn't exist on the other side. there is no conflict,
		   we'll just need to create the mailbox later. */
		return FALSE;
	} else {
		/* both nodes are mailboxes that don't exist on the other side.
		   we'll merge these mailboxes together later and change their
		   GUIDs and UIDVALIDITYs to be the same */
		local_node1->sync_delayed_guid_change = TRUE;
		remote_node2->sync_delayed_guid_change = TRUE;
		return FALSE;
	}
}

static struct dsync_mailbox_node *
sync_find_branch(struct dsync_mailbox_tree *tree,
		 struct dsync_mailbox_tree *other_tree,
		 struct dsync_mailbox_node *dir_node)
{
	struct dsync_mailbox_node *node, *other_node;
	const uint8_t *guid_p;

	for (node = dir_node->first_child; node != NULL; node = node->next) {
		if (dsync_mailbox_node_is_dir(node)) {
			other_node = sync_find_branch(tree, other_tree, node);
			if (other_node != NULL)
				return other_node;
		} else {
			guid_p = node->mailbox_guid;
			other_node = hash_table_lookup(other_tree->guid_hash,
						       guid_p);
			if (other_node != NULL)
				return other_node->parent;
		}
	}
	return NULL;
}

static bool sync_rename_directory(struct dsync_mailbox_tree_sync_ctx *ctx,
				  struct dsync_mailbox_node *local_node1,
				  struct dsync_mailbox_node *remote_node2)
{
	struct dsync_mailbox_node *remote_node1, *local_node2;

	/* see if we can find matching mailbox branches based on the nodes'
	   child mailboxes (with GUIDs). */
	remote_node1 = sync_find_branch(ctx->local_tree,
					ctx->remote_tree, local_node1);
	local_node2 = sync_find_branch(ctx->remote_tree, ctx->local_tree,
				       remote_node2);
	if (remote_node1 == NULL || local_node2 == NULL ||
	    remote_node1 == &ctx->remote_tree->root ||
	    local_node2 == &ctx->local_tree->root ||
	    node_names_equal(remote_node1, local_node2))
		return FALSE;

	return sync_rename_lower_ts(ctx, local_node1, remote_node1,
				    local_node2, remote_node2);
}

static bool sync_rename_mailboxes(struct dsync_mailbox_tree_sync_ctx *ctx,
				  struct dsync_mailbox_node *local_parent,
				  struct dsync_mailbox_node *remote_parent)
{
	struct dsync_mailbox_node **local_nodep = &local_parent->first_child;
	struct dsync_mailbox_node **remote_nodep = &remote_parent->first_child;
	struct dsync_mailbox_node *local_node, *remote_node;

	/* the nodes are sorted by their names. */
	while (*local_nodep != NULL || *remote_nodep != NULL) {
		local_node = *local_nodep;
		remote_node = *remote_nodep;

		if (local_node == NULL ||
		    (remote_node != NULL &&
		     strcmp(local_node->name, remote_node->name) > 0)) {
			/* add a missing local node */
			local_node = sync_node_new(ctx->local_tree, local_nodep,
						   local_parent, remote_node);
		}
		if (remote_node == NULL ||
		    strcmp(remote_node->name, local_node->name) > 0) {
			/* add a missing remote node */
			remote_node = sync_node_new(ctx->remote_tree, remote_nodep,
						    remote_parent, local_node);
		}
		i_assert(strcmp(local_node->name, remote_node->name) == 0);

		if (dsync_mailbox_node_is_dir(local_node) &&
		    dsync_mailbox_node_is_dir(remote_node)) {
			/* both nodes are directories (or other side is
			   nonexistent). see if we can match them by their
			   child mailboxes */
			if (sync_rename_directory(ctx, local_node, remote_node))
				return TRUE;
		} else if (dsync_mailbox_node_guids_equal(local_node,
							  remote_node)) {
			/* mailboxes are equal, no need to rename */
		} else {
			/* mailbox naming conflict */
			if (sync_rename_conflict(ctx, local_node, remote_node))
				return TRUE;
		}
		/* handle children, if there are any */
		if (sync_rename_mailboxes(ctx, local_node, remote_node))
			return TRUE;

		local_nodep = &local_node->next;
		remote_nodep = &remote_node->next;
	}
	return FALSE;
}

static bool mailbox_node_hash_first_child(struct dsync_mailbox_node *node,
					  struct md5_context *md5)
{
	for (node = node->first_child; node != NULL; node = node->next) {
		if (node->existence == DSYNC_MAILBOX_NODE_EXISTS) {
			md5_update(md5, node->mailbox_guid,
				   sizeof(node->mailbox_guid));
			md5_update(md5, node->name, strlen(node->name));
			return TRUE;
		}
		if (node->first_child != NULL) {
			if (mailbox_node_hash_first_child(node, md5))
				return TRUE;
		}
	}
	return FALSE;
}

static const char *
mailbox_node_generate_suffix(struct dsync_mailbox_node *node)
{
	struct md5_context md5;
	unsigned char digest[MD5_RESULTLEN];

	if (!dsync_mailbox_node_is_dir(node))
		return guid_128_to_string(node->mailbox_guid);

	md5_init(&md5);
	if (!mailbox_node_hash_first_child(node, &md5))
		i_unreached(); /* we would have deleted it */
	md5_final(&md5, digest);
	return binary_to_hex(digest, sizeof(digest));
}

static void suffix_inc(string_t *str)
{
	char *data;
	unsigned int i;

	data = str_c_modifiable(str) + str_len(str)-1;
	for (i = str_len(str); i > 0; i--, data--) {
		if ((*data >= '0' && *data < '9') ||
		    (*data >= 'a' && *data < 'f')) {
			*data += 1;
			return;
		} else if (*data == '9') {
			*data = 'a';
			return;
		} else if (*data != 'f') {
			i_unreached();
		}
	}
	i_unreached();
}

static void
sync_rename_temp_mailbox_node(pool_t pool,
			      struct dsync_mailbox_node *node)
{
	const char *p, *new_suffix;
	string_t *str = t_str_new(256);
	unsigned int max_prefix_len;

	/* The name is currently <oldname>-<temp>. Both sides need to
	   use equivalent names, so we'll replace the <temp> if possible
	   with a) mailbox GUID, b) sha1 of childrens' (GUID|name)s. In the
	   very unlikely case of such name already existing, just increase
	   the last letters until it's not found. */
	new_suffix = mailbox_node_generate_suffix(node);

	p = strrchr(node->name, '-');
	i_assert(p != NULL);
	p++;
	max_prefix_len = TEMP_MAX_NAME_LEN - strlen(new_suffix) - 1;
	if (max_prefix_len > (size_t)(p-node->name))
		max_prefix_len = p-node->name;
	str_append_n(str, node->name, max_prefix_len);
	str_append(str, new_suffix);
	while (node_has_child(node->parent, str_c(str)))
		suffix_inc(str);
	node->name = p_strdup(pool, str_c(str));

	dsync_mailbox_tree_node_move_sorted(node, node->parent);
	node->sync_temporary_name = FALSE;
}

static void
sync_rename_delete_node_dirs(struct dsync_mailbox_tree_sync_ctx *ctx,
			     struct dsync_mailbox_tree *tree,
			     struct dsync_mailbox_node *node)
{
	struct dsync_mailbox_node *child;

	for (child = node->first_child; child != NULL; child = child->next)
		sync_rename_delete_node_dirs(ctx, tree, child);

	node->existence = DSYNC_MAILBOX_NODE_NONEXISTENT;
	node->sync_temporary_name = FALSE;
	if (tree == ctx->local_tree) {
		sync_add_dir_change(ctx, node,
				    DSYNC_MAILBOX_TREE_SYNC_TYPE_DELETE_DIR);
	}
}

static bool
sync_rename_temp_mailboxes(struct dsync_mailbox_tree_sync_ctx *ctx,
			   struct dsync_mailbox_tree *tree,
			   struct dsync_mailbox_node *node)
{
	for (; node != NULL; node = node->next) {
		while (sync_rename_temp_mailboxes(ctx, tree, node->first_child)) ;

		if (!node->sync_temporary_name) {
		} else if (dsync_mailbox_node_is_dir(node) &&
			   (node->first_child == NULL ||
			    !node_has_existent_children(node, FALSE))) {
			/* we can just delete this directory and
			   any child directories it may have */
			sync_rename_delete_node_dirs(ctx, tree, node);
		} else {
			T_BEGIN {
				sync_rename_temp_mailbox_node(tree->pool, node);
			} T_END;
			return TRUE;
		}
	}
	return FALSE;
}

static void sync_create_mailboxes(struct dsync_mailbox_tree_sync_ctx *ctx,
				  struct dsync_mailbox_tree *tree)
{
	struct dsync_mailbox_tree *other_tree;
	struct dsync_mailbox_tree_iter *iter;
	struct dsync_mailbox_node *node, *other_node;
	const char *name;
	const uint8_t *guid_p;

	other_tree = tree == ctx->local_tree ?
		ctx->remote_tree : ctx->local_tree;

	iter = dsync_mailbox_tree_iter_init(tree);
	while (dsync_mailbox_tree_iter_next(iter, &name, &node)) {
		/* make sure the renaming handled everything */
		i_assert(!node->sync_temporary_name);
		if (dsync_mailbox_node_is_dir(node))
			continue;

		i_assert(node->existence == DSYNC_MAILBOX_NODE_EXISTS);

		guid_p = node->mailbox_guid;
		other_node = hash_table_lookup(other_tree->guid_hash, guid_p);
		if (other_node == NULL)
			other_node = sorted_tree_get(other_tree, name);
		if (!dsync_mailbox_node_is_dir(other_node)) {
			/* already exists */
			i_assert(node->existence == DSYNC_MAILBOX_NODE_EXISTS);
			// FIXME: remove this assert? for conflicting GUIDs
			/*i_assert(memcmp(node->mailbox_guid,
					other_node->mailbox_guid,
					sizeof(node->mailbox_guid)) == 0);*/
		} else {
			other_node->existence = DSYNC_MAILBOX_NODE_EXISTS;
			other_node->ns = node->ns;
			other_node->uid_validity = node->uid_validity;
			memcpy(other_node->mailbox_guid, node->mailbox_guid,
			       sizeof(other_node->mailbox_guid));
			if (other_tree == ctx->local_tree)
				sync_add_create_change(ctx, other_node, name);
		}
	}
	dsync_mailbox_tree_iter_deinit(&iter);
}

static void
sync_subscription(struct dsync_mailbox_tree_sync_ctx *ctx,
		  struct dsync_mailbox_node *local_node,
		  struct dsync_mailbox_node *remote_node)
{
	if (local_node->last_subscription_change >
	    remote_node->last_subscription_change ||
	    (local_node->last_subscription_change ==
	     remote_node->last_subscription_change && local_node->subscribed)) {
		/* use local subscription state */
		remote_node->subscribed = local_node->subscribed;
	} else {
		/* use remote subscription state */
		local_node->subscribed = remote_node->subscribed;
		sync_add_dir_change(ctx, local_node, local_node->subscribed ?
				    DSYNC_MAILBOX_TREE_SYNC_TYPE_SUBSCRIBE :
				    DSYNC_MAILBOX_TREE_SYNC_TYPE_UNSUBSCRIBE);
	}
}

static void sync_mailbox_child_dirs(struct dsync_mailbox_tree_sync_ctx *ctx,
				    struct dsync_mailbox_node *local_parent,
				    struct dsync_mailbox_node *remote_parent)
{
	struct dsync_mailbox_node **local_nodep = &local_parent->first_child;
	struct dsync_mailbox_node **remote_nodep = &remote_parent->first_child;
	struct dsync_mailbox_node *local_node, *remote_node;
	int ret;

	/* NOTE: the nodes are always sorted. renaming created all of the
	   interesting nodes, but it may have left some extra nonexistent nodes
	   lying around, which we will delete. */
	while (*local_nodep != NULL && *remote_nodep != NULL) {
		local_node = *local_nodep;
		remote_node = *remote_nodep;

		ret = strcmp(local_node->name, remote_node->name);
		if (ret < 0) {
			i_assert(!node_is_existent(local_node));
			*local_nodep = local_node->next;
			continue;
		}
		if (ret > 0) {
			i_assert(!node_is_existent(remote_node));
			*remote_nodep = remote_node->next;
			continue;
		}

		if (local_node->existence == DSYNC_MAILBOX_NODE_EXISTS &&
		    remote_node->existence == DSYNC_MAILBOX_NODE_NONEXISTENT) {
			/* create to remote */
			remote_node->existence = DSYNC_MAILBOX_NODE_EXISTS;
		}
		if (remote_node->existence == DSYNC_MAILBOX_NODE_EXISTS &&
		    local_node->existence == DSYNC_MAILBOX_NODE_NONEXISTENT) {
			/* create to local */
			local_node->existence = DSYNC_MAILBOX_NODE_EXISTS;
			sync_add_dir_change(ctx, local_node,
				DSYNC_MAILBOX_TREE_SYNC_TYPE_CREATE_DIR);
		}

		/* create/delete child dirs */
		sync_mailbox_child_dirs(ctx, local_node, remote_node);

		if (local_node->subscribed != remote_node->subscribed)
			sync_subscription(ctx, local_node, remote_node);

		if (local_node->existence == DSYNC_MAILBOX_NODE_DELETED &&
		    local_node->first_child == NULL &&
		    remote_node->existence == DSYNC_MAILBOX_NODE_EXISTS) {
			/* delete from remote */
			i_assert(remote_node->first_child == NULL);
			remote_node->existence = DSYNC_MAILBOX_NODE_NONEXISTENT;
		}
		if (remote_node->existence == DSYNC_MAILBOX_NODE_DELETED &&
		    remote_node->first_child == NULL &&
		    local_node->existence == DSYNC_MAILBOX_NODE_EXISTS) {
			/* delete from local */
			i_assert(local_node->first_child == NULL);
			local_node->existence = DSYNC_MAILBOX_NODE_NONEXISTENT;
			sync_add_dir_change(ctx, local_node,
				DSYNC_MAILBOX_TREE_SYNC_TYPE_DELETE_DIR);
		}

		local_nodep = &local_node->next;
		remote_nodep = &remote_node->next;
	}
	while (*local_nodep != NULL) {
		i_assert(!node_is_existent(*local_nodep));
		*local_nodep = (*local_nodep)->next;
	}
	while (*remote_nodep != NULL) {
		i_assert(!node_is_existent(*remote_nodep));
		*remote_nodep = (*remote_nodep)->next;
	}
}

static void sync_mailbox_dirs(struct dsync_mailbox_tree_sync_ctx *ctx)
{
	sync_mailbox_child_dirs(ctx, &ctx->local_tree->root,
				&ctx->remote_tree->root);
}

struct dsync_mailbox_tree_sync_ctx *
dsync_mailbox_trees_sync_init(struct dsync_mailbox_tree *local_tree,
			      struct dsync_mailbox_tree *remote_tree)
{
	struct dsync_mailbox_tree_sync_ctx *ctx;
	pool_t pool;

	i_assert(hash_table_is_created(local_tree->guid_hash));
	i_assert(hash_table_is_created(remote_tree->guid_hash));

	pool = pool_alloconly_create(MEMPOOL_GROWING"dsync mailbox trees sync",
				     1024*64);
	ctx = p_new(pool, struct dsync_mailbox_tree_sync_ctx, 1);
	ctx->pool = pool;
	ctx->local_tree = local_tree;
	ctx->remote_tree = remote_tree;
	i_array_init(&ctx->changes, 128);

	sync_tree_sort_and_delete_mailboxes(ctx, remote_tree);
	sync_tree_sort_and_delete_mailboxes(ctx, local_tree);
	while (sync_rename_mailboxes(ctx, &local_tree->root, &remote_tree->root)) ;
	while (sync_rename_temp_mailboxes(ctx, local_tree, &local_tree->root)) ;
	while (sync_rename_temp_mailboxes(ctx, remote_tree, &remote_tree->root)) ;
	sync_create_mailboxes(ctx, remote_tree);
	sync_create_mailboxes(ctx, local_tree);
	sync_mailbox_dirs(ctx);
	return ctx;
}

const struct dsync_mailbox_tree_sync_change *
dsync_mailbox_trees_sync_next(struct dsync_mailbox_tree_sync_ctx *ctx)
{
	if (ctx->change_idx == array_count(&ctx->changes))
		return NULL;
	return array_idx(&ctx->changes, ctx->change_idx++);
}

void dsync_mailbox_trees_sync_deinit(struct dsync_mailbox_tree_sync_ctx **_ctx)
{
	struct dsync_mailbox_tree_sync_ctx *ctx = *_ctx;

	*_ctx = NULL;

	array_free(&ctx->changes);
	pool_unref(&ctx->pool);
}
