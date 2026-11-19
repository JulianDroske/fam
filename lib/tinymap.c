/*
  tinymap - map implementation with binary search based on simplified AVL
  adapted from my private SC project - 2026 @ juliandroske
*/

#include "tinymap.h"

#include "stddef.h"
#include "stdlib.h"

void tinymap_fini(struct tinymap *map) {
  if (map->nodes) { free(map->nodes); map->nodes = NULL; }
  if (map->keys) { free(map->keys); map->keys = NULL; }
}

int tinymap_init(struct tinymap *map) {
  if (!map) return 0;

  map->nodes = NULL; map->keys = NULL;
  size_t nodes_cap = 16, keys_size = 256;
  map->nodes = malloc(nodes_cap * sizeof(struct tinymap_node));
  if (!map->nodes) return 0;
  map->keys = calloc(keys_size, 1);
  if (!map->keys) { tinymap_fini(map); return 0; }

  map->nodes_len = 0; map->nodes_cap = nodes_cap;
  map->keys_len = 0; map->keys_size = keys_size;
  map->root = -1;
  return 1;
}

int tinymap_cmp(const char *key1, size_t len1, const char *key2, size_t len2) {
  size_t n = len1 < len2? len1: len2;
  for (size_t i = 0; i < n; ++i) {
    char k = *key2 - *key1; if (k) return k;
    ++key1, ++key2;
  }
  return len2 - len1;
}

int tinymap_find(struct tinymap *map, struct tinymap_node **dest_node, const char *key, size_t len) {
  /* hash, based on FNV-1a */
  unsigned int hash = 2166136261;
  for (size_t i = 0; i < len; ++i) hash = (hash ^ key[i]) * 16777619;
  map->hash = hash;

  int index = map->index = map->root;
  while (index >= 0) {
    map->index = index;
    struct tinymap_node *node = &map->nodes[map->index];
    if (hash == node->key_hash &&
      !tinymap_cmp(map->keys + node->key_off, node->key_len, key, len)) {
      if (dest_node) *dest_node = node;
      return 1;
    } else index = node->children[node->key_hash < hash];
  }
  return 0;
}

/* dir: { -1: ->, 1: <- } */
int tinymap_rot(struct tinymap *map, int *root_index, int dir) {
  if (!dir) return 0;
  int i = dir > 0; dir = i? 1: -1;
  int index = *root_index;

  struct tinymap_node *node = &map->nodes[index];
  int child_index = node->children[i], parent_index = node->parent;
  struct tinymap_node *child = &map->nodes[child_index];
  int cc_index = child->children[1 - i];

  node->parent = child_index; node->children[i] = cc_index;
  child->parent = parent_index; child->children[1 - i] = index;

  if (cc_index >= 0) map->nodes[cc_index].parent = index;
  if (parent_index >= 0) {
    struct tinymap_node *parent = &map->nodes[parent_index];
    parent->children[index == parent->children[1]] = child_index;
  }

  node->factor -= (child->factor * dir > 0? child->factor: 0) + dir;
  child->factor += (node->factor * dir < 0? node->factor: 0) - dir;

  if (map->root == index) map->root = child_index;
  *root_index = child_index;
  return 1;
}

/* ref: https://github.com/xieqing/avl-tree */
int tinymap_fix(struct tinymap *map, int index) {
  int parent_index = map->nodes[index].parent;
  while (parent_index >= 0) {
    struct tinymap_node *parent = &map->nodes[parent_index];
    parent->factor += index == parent->children[0]? -1: 1;
    if (!parent->factor) break;
    else if (parent->factor == -2 || parent->factor == 2) {
      /* node_index is always equal to index for insertion */
      int node_index = parent->children[parent->factor > 0];
      struct tinymap_node *node = &map->nodes[node_index];
      if (node->factor * parent->factor < 0)
        if (!tinymap_rot(map, &node_index, -parent->factor)) return 0;
      node = &map->nodes[node_index];
      if (!tinymap_rot(map, &parent_index, parent->factor)) return 0;
      parent = &map->nodes[parent_index];
      /* should not happen */
      if (parent->factor) return 0;
      break;
    }
    /* ??? */
    else if (parent->factor < -2 || parent->factor > 2) return 0;
    index = parent_index; parent_index = parent->parent;
  }
  return 1;
}

int tinymap_add(struct tinymap *map, const char *key, size_t len, long int value) {
  if (len <= 0) return 0;

  if (map->keys_len + len >= map->keys_size) {
    size_t new_size = map->keys_len + len + 512;
    void *new_mem = realloc(map->keys, new_size);
    if (!new_mem) return 0;
    map->keys = (char *) new_mem; map->keys_size = new_size;
  }

  if (map->nodes_len + 1 >= map->nodes_cap) {
    size_t new_cap = map->nodes_cap + 32;
    void *new_mem = realloc(map->nodes, new_cap * sizeof(struct tinymap_node));
    if (!new_mem) return 0;
    map->nodes = (struct tinymap_node *) new_mem; map->nodes_cap = new_cap;
  }

  if (tinymap_find(map, NULL, key, len)) return 0;
  int index = map->nodes_len;
  struct tinymap_node *node = &map->nodes[map->nodes_len++];
  node->value = value; node->key_off = map->keys_len; node->key_len = len;
  node->key_hash = map->hash;
  node->children[0] = node->children[1] = -1;
  node->parent = map->index; node->factor = 0;

  int plen = len;
  for (char *p = map->keys + map->keys_len; plen; --plen) *p++ = *key++;
  map->keys_len += len;

  /* root node */
  if (map->root < 0) { map->root = index; return 1; }

  struct tinymap_node *p = &map->nodes[node->parent];
  p->children[map->hash >= p->key_hash] = index;
  return tinymap_fix(map, index);
}

/* utils */

int tinymap_get(struct tinymap *map, long int *value, const char *key, size_t len) {
  struct tinymap_node *node = NULL;
  if (!tinymap_find(map, &node, key, len)) return 0;
  if (value) *value = node->value;
  return 1;
}

int tinymap_set(struct tinymap *map, const char *key, size_t len, long int value) {
  struct tinymap_node *node = NULL;
  if (!tinymap_find(map, &node, key, len)) return 0;
  node->value = value;
  return 1;
}

int tinymap_len(struct tinymap *map) {
  return map->nodes_len;
}

void tinymap_clear(struct tinymap *map) {
  map->root = -1;
  map->nodes_len = 0; map->keys_len = 0;
}

int tinymap_enum(struct tinymap *map, struct tinymap_node **node) {
  if (!node || !map->nodes_len) return 0;
  if (!*node) { *node = map->nodes; return 1; }
  if (*node < map->nodes + map->nodes_len - 1) { ++*node; return 1; }
  return 0;
}

const char *tinymap_node_key(struct tinymap *map, struct tinymap_node *node) {
  return map->keys + node->key_off;
}

