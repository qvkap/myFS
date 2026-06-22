#include "spermfs.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

static int btree_read_node(spermfs_context_t *ctx, uint64_t block,
                             spermfs_btree_node_t *node)
{
    size_t node_size = sizeof(spermfs_btree_node_t);
    int ret = spermfs_read_block(ctx, block, node, node_size, 0);
    if (ret != SPERMAFS_OK) return ret;

    uint64_t stored_csum = node->checksum;
    node->checksum = 0;
    uint64_t computed = spermfs_crc64(node, node_size, 0);
    if (stored_csum != 0 && stored_csum != computed)
        return SPERMAFS_ERR_CHECKSUM;

    return SPERMAFS_OK;
}

static int btree_write_node(spermfs_context_t *ctx, uint64_t block,
                             spermfs_btree_node_t *node)
{
    size_t node_size = sizeof(spermfs_btree_node_t);
    node->self_block = block;
    node->checksum = 0;
    node->checksum = spermfs_crc64(node, node_size, 0);
    return spermfs_write_block(ctx, block, node, node_size, 0);
}

int spermfs_btree_init(spermfs_context_t *ctx, uint64_t *root_block)
{
    if (!ctx || !root_block) return SPERMAFS_ERR_INVAL;

    uint64_t block;
    int ret = spermfs_alloc_blocks(ctx, 1, &block, 0);
    if (ret != SPERMAFS_OK) return ret;

    spermfs_btree_node_t *node = calloc(1, sizeof(spermfs_btree_node_t));
    if (!node) return SPERMAFS_ERR_NOMEM;

    node->is_leaf = 1;
    node->num_keys = 0;

    ret = btree_write_node(ctx, block, node);
    free(node);
    if (ret == SPERMAFS_OK)
        *root_block = block;
    return ret;
}

static int btree_split_child(spermfs_context_t *ctx, uint64_t parent_block,
                              spermfs_btree_node_t *parent, int idx)
{
    uint64_t child_block = parent->children[idx];
    spermfs_btree_node_t *child = calloc(1, sizeof(spermfs_btree_node_t));
    spermfs_btree_node_t *new_node = calloc(1, sizeof(spermfs_btree_node_t));
    if (!child || !new_node) {
        free(child); free(new_node);
        return SPERMAFS_ERR_NOMEM;
    }

    int ret = btree_read_node(ctx, child_block, child);
    if (ret != SPERMAFS_OK) goto cleanup;

    uint64_t new_block;
    ret = spermfs_alloc_blocks(ctx, 1, &new_block, 0);
    if (ret != SPERMAFS_OK) goto cleanup;

    new_node->is_leaf = child->is_leaf;
    new_node->num_keys = SPERMAFS_BTREE_ORDER / 2 - 1;

    for (int i = 0; i < new_node->num_keys; i++) {
        new_node->keys[i] = child->keys[i + SPERMAFS_BTREE_ORDER / 2];
        new_node->values[i] = child->values[i + SPERMAFS_BTREE_ORDER / 2];
    }

    if (!child->is_leaf) {
        for (int i = 0; i < SPERMAFS_BTREE_ORDER / 2; i++)
            new_node->children[i] = child->children[i + SPERMAFS_BTREE_ORDER / 2];
    }

    child->num_keys = SPERMAFS_BTREE_ORDER / 2 - 1;

    for (int i = parent->num_keys; i > idx; i--) {
        parent->keys[i] = parent->keys[i - 1];
        parent->values[i] = parent->values[i - 1];
        parent->children[i + 1] = parent->children[i];
    }

    parent->keys[idx] = child->keys[SPERMAFS_BTREE_ORDER / 2 - 1];
    parent->values[idx] = child->values[SPERMAFS_BTREE_ORDER / 2 - 1];
    parent->children[idx + 1] = new_block;
    parent->num_keys++;

    ret = btree_write_node(ctx, child_block, child);
    if (ret != SPERMAFS_OK) goto cleanup;
    ret = btree_write_node(ctx, new_block, new_node);
    if (ret != SPERMAFS_OK) goto cleanup;
    ret = btree_write_node(ctx, parent_block, parent);

cleanup:
    free(child);
    free(new_node);
    return ret;
}

static int btree_insert_nonfull(spermfs_context_t *ctx, uint64_t node_block,
                                 spermfs_btree_node_t *node, uint64_t key,
                                 uint64_t value)
{
    int i = node->num_keys - 1;

    if (node->is_leaf) {
        while (i >= 0 && key < node->keys[i]) {
            node->keys[i + 1] = node->keys[i];
            node->values[i + 1] = node->values[i];
            i--;
        }
        node->keys[i + 1] = key;
        node->values[i + 1] = value;
        node->num_keys++;
        return btree_write_node(ctx, node_block, node);
    }

    while (i >= 0 && key < node->keys[i])
        i--;
    i++;

    spermfs_btree_node_t *child = calloc(1, sizeof(spermfs_btree_node_t));
    if (!child) return SPERMAFS_ERR_NOMEM;

    int ret = btree_read_node(ctx, node->children[i], child);
    if (ret != SPERMAFS_OK) { free(child); return ret; }

    if (child->num_keys == SPERMAFS_BTREE_ORDER - 1) {
        ret = btree_split_child(ctx, node_block, node, i);
        if (ret != SPERMAFS_OK) { free(child); return ret; }

        if (key > node->keys[i]) {
            i++;
            ret = btree_read_node(ctx, node->children[i], child);
            if (ret != SPERMAFS_OK) { free(child); return ret; }
        }
    }

    ret = btree_insert_nonfull(ctx, node->children[i], child, key, value);
    free(child);
    return ret;
}

int spermfs_btree_insert(spermfs_context_t *ctx, uint64_t root_block,
                          uint64_t key, uint64_t value)
{
    if (!ctx) return SPERMAFS_ERR_INVAL;

    spermfs_btree_node_t *root = calloc(1, sizeof(spermfs_btree_node_t));
    if (!root) return SPERMAFS_ERR_NOMEM;

    int ret = btree_read_node(ctx, root_block, root);
    if (ret != SPERMAFS_OK) { free(root); return ret; }

    if (root->num_keys == SPERMAFS_BTREE_ORDER - 1) {
        uint64_t new_block;
        ret = spermfs_alloc_blocks(ctx, 1, &new_block, 0);
        if (ret != SPERMAFS_OK) { free(root); return ret; }

        spermfs_btree_node_t *new_root = calloc(1, sizeof(spermfs_btree_node_t));
        if (!new_root) { free(root); return SPERMAFS_ERR_NOMEM; }

        new_root->is_leaf = 0;
        new_root->num_keys = 0;
        new_root->children[0] = root_block;

        ret = btree_write_node(ctx, new_block, new_root);
        if (ret != SPERMAFS_OK) { free(root); free(new_root); return ret; }

        ret = btree_split_child(ctx, new_block, new_root, 0);
        if (ret != SPERMAFS_OK) { free(root); free(new_root); return ret; }

        int idx = (key > new_root->keys[0]) ? 1 : 0;
        spermfs_btree_node_t *child = calloc(1, sizeof(spermfs_btree_node_t));
        if (!child) { free(root); free(new_root); return SPERMAFS_ERR_NOMEM; }

        ret = btree_read_node(ctx, new_root->children[idx], child);
        if (ret == SPERMAFS_OK)
            ret = btree_insert_nonfull(ctx, new_root->children[idx], child, key, value);
        free(child);

        if (ret == SPERMAFS_OK) {
            /* Update root pointer in superblock */
            ctx->superblock.root_tree_root = new_block;
        }
        free(new_root);
    } else {
        ret = btree_insert_nonfull(ctx, root_block, root, key, value);
    }

    free(root);
    return ret;
}

int spermfs_btree_lookup(spermfs_context_t *ctx, uint64_t root_block,
                          uint64_t key, uint64_t *value)
{
    if (!ctx || !value) return SPERMAFS_ERR_INVAL;

    spermfs_btree_node_t *node = calloc(1, sizeof(spermfs_btree_node_t));
    if (!node) return SPERMAFS_ERR_NOMEM;

    uint64_t current = root_block;
    int ret;

    while (1) {
        ret = btree_read_node(ctx, current, node);
        if (ret != SPERMAFS_OK) break;

        int i = 0;
        while (i < node->num_keys && key > node->keys[i])
            i++;

        if (i < node->num_keys && key == node->keys[i]) {
            *value = node->values[i];
            ret = SPERMAFS_OK;
            break;
        }

        if (node->is_leaf) {
            ret = SPERMAFS_ERR_NOENT;
            break;
        }

        current = node->children[i];
    }

    free(node);
    return ret;
}

int spermfs_btree_delete(spermfs_context_t *ctx, uint64_t root_block,
                          uint64_t key)
{
    /* Simple deletion: find and remove from leaf */
    spermfs_btree_node_t *node = calloc(1, sizeof(spermfs_btree_node_t));
    if (!node) return SPERMAFS_ERR_NOMEM;

    uint64_t current = root_block;
    int ret;

    while (1) {
        ret = btree_read_node(ctx, current, node);
        if (ret != SPERMAFS_OK) break;

        int i = 0;
        while (i < node->num_keys && key > node->keys[i])
            i++;

        if (i < node->num_keys && key == node->keys[i]) {
            if (!node->is_leaf) {
                /* For simplicity, just mark deleted - real impl would merge */
                node->values[i] = 0;
                ret = btree_write_node(ctx, current, node);
            } else {
                /* Remove from leaf */
                for (int j = i; j < node->num_keys - 1; j++) {
                    node->keys[j] = node->keys[j + 1];
                    node->values[j] = node->values[j + 1];
                }
                node->num_keys--;
                ret = btree_write_node(ctx, current, node);
            }
            break;
        }

        if (node->is_leaf) {
            ret = SPERMAFS_ERR_NOENT;
            break;
        }

        current = node->children[i];
    }

    free(node);
    return ret;
}

int spermfs_btree_update(spermfs_context_t *ctx, uint64_t root_block,
                          uint64_t key, uint64_t value)
{
    return spermfs_btree_insert(ctx, root_block, key, value);
}

int spermfs_btree_iterate(spermfs_context_t *ctx, uint64_t root_block,
                            int (*cb)(uint64_t key, uint64_t value, void *priv),
                            void *priv)
{
    if (!ctx || !cb) return SPERMAFS_ERR_INVAL;

    /* Iterative inorder traversal using a stack */
    uint64_t stack[256];
    int sp = 0;
    uint64_t current = root_block;
    spermfs_btree_node_t *node = calloc(1, sizeof(spermfs_btree_node_t));
    if (!node) return SPERMAFS_ERR_NOMEM;

    while (1) {
        if (current != 0) {
            stack[sp++] = current;
            int ret = btree_read_node(ctx, current, node);
            if (ret != SPERMAFS_OK) { free(node); return ret; }
            current = node->is_leaf ? 0 : node->children[0];
        } else if (sp > 0) {
            current = stack[--sp];
            int ret = btree_read_node(ctx, current, node);
            if (ret != SPERMAFS_OK) { free(node); return ret; }

            for (int i = 0; i < node->num_keys; i++) {
                cb(node->keys[i], node->values[i], priv);
            }

            current = node->is_leaf ? 0 : node->children[node->num_keys];
        } else {
            break;
        }
    }

    free(node);
    return SPERMAFS_OK;
}

void spermfs_btree_print(spermfs_context_t *ctx, uint64_t root_block)
{
    printf("B+Tree at block %llu:\n", (unsigned long long)root_block);

    /* BFS traversal */
    uint64_t queue[256];
    int head = 0, tail = 0;
    queue[tail++] = root_block;

    spermfs_btree_node_t *node = calloc(1, sizeof(spermfs_btree_node_t));
    if (!node) return;

    while (head < tail) {
        uint64_t current = queue[head++];
        if (btree_read_node(ctx, current, node) != SPERMAFS_OK) continue;

        printf("  Node %llu: leaf=%d keys=%d [",
               (unsigned long long)current, node->is_leaf, node->num_keys);
        for (int i = 0; i < node->num_keys; i++)
            printf(" %llu:%llu", (unsigned long long)node->keys[i],
                   (unsigned long long)node->values[i]);
        printf(" ]\n");

        if (!node->is_leaf) {
            for (int i = 0; i <= node->num_keys && tail < 256; i++)
                queue[tail++] = node->children[i];
        }
    }

    free(node);
}
