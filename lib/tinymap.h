#ifndef _TINYMAP_H_
#define _TINYMAP_H_

#include "stddef.h"

struct tinymap_node {
  long int value;
  /* key_len indicates existence */
  size_t key_off, key_len;
  unsigned int key_hash;
  int children[2], parent, factor;
};

struct tinymap {
  struct tinymap_node *nodes; int nodes_len, nodes_cap;
  char *keys; size_t keys_len, keys_size;
  int root;

  /* states */
  unsigned int hash; int index;
};

int tinymap_init(struct tinymap *map);
void tinymap_fini(struct tinymap *map);

int tinymap_add(struct tinymap *map, const char *key, size_t len, long int value);
int tinymap_get(struct tinymap *map, long int *value, const char *key, size_t len);
int tinymap_set(struct tinymap *map, const char *key, size_t len, long int value);
int tinymap_len(struct tinymap *map);
void tinymap_clear(struct tinymap *map);

/* low level */
int tinymap_find(struct tinymap *map, struct tinymap_node **node, const char *key, size_t len);
int tinymap_enum(struct tinymap *map, struct tinymap_node **node);
const char *tinymap_node_key(struct tinymap *map, struct tinymap_node *node);

#endif

