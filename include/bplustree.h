/*
 * bplustree.h — C port of the persistent, append-only, immutable B+ tree in
 * src/bplustree.js.
 *
 * Design (see the plan and src/bplustree.js):
 *   - A tree owns an in-memory byte "image" mirroring the append-only file:
 *     nodes and metadata are appended verbatim, and reads resolve by offset into
 *     the image. The host (JS) loads the existing file bytes on open and writes
 *     the image back to storage on flush/close — no per-node host callbacks.
 *   - Nodes and metadata use the exact binjson wire format of bplustree.js, so
 *     files stay byte-compatible (bin/bplustree-decode.js can read them).
 *   - The tree compares only *keys* (number or string); *values* are opaque,
 *     carried as raw pre-encoded binjson blobs and never interpreted here.
 *
 * All operations return BJ_OK (0) or a negative BJ_ERR_* code from binjson.h.
 */
#ifndef BPLUSTREE_H
#define BPLUSTREE_H

#include <stdint.h>
#include <stddef.h>

#include "binjson.h"

#ifdef __cplusplus
extern "C" {
#endif

/* A comparable key: a JS number or a JS string. */
typedef struct {
    int      is_string;   /* 0 = number, 1 = string        */
    double   num;         /* when is_string == 0           */
    const uint8_t *str;   /* when is_string == 1 (utf8)    */
    uint32_t str_len;
} bpt_key;

typedef struct bpt bpt;

/* Create a fresh empty tree (order >= 3). Returns NULL on OOM. */
bpt *bpt_create(int order);
/* Load an existing tree from a file image (copies `len` bytes). Returns NULL on
 * OOM or if the image is too small / metadata is unreadable. */
bpt *bpt_load(const uint8_t *bytes, size_t len);
/* Free a tree and all its buffers. Safe to pass NULL. */
void bpt_free(bpt *t);

/* Accessors (mirror the JS metadata fields). */
double         bpt_size(const bpt *t);
double         bpt_root(const bpt *t);
double         bpt_next_id(const bpt *t);
int            bpt_order(const bpt *t);
/* The full file image; writes its length through *len. */
const uint8_t *bpt_image(const bpt *t, size_t *len);

/* Insert/update. `val`/`val_len` is one pre-encoded binjson value (opaque). */
int bpt_add(bpt *t, const bpt_key *key, const uint8_t *val, uint32_t val_len);
/* Delete a key (no-op if absent). */
int bpt_delete(bpt *t, const bpt_key *key);
/*
 * Search. On BJ_OK, *found is 1/0; when found, out_ptr/out_len point at the
 * value blob held in the tree's output buffer (valid until the next operation).
 */
int bpt_search(bpt *t, const bpt_key *key, int *found,
               const uint8_t **out_ptr, size_t *out_len);

/*
 * Collect all entries (bpt_entries) or those with min <= key <= max
 * (bpt_range), in sorted order, as a binjson ARRAY of { key, value } objects.
 * The encoded bytes are exposed via out_ptr/out_len (valid until the next op).
 */
int bpt_entries(bpt *t, const uint8_t **out_ptr, size_t *out_len);
int bpt_range(bpt *t, const bpt_key *min, const bpt_key *max,
              const uint8_t **out_ptr, size_t *out_len);

/* Tree height (0 for a single leaf). */
int bpt_height(bpt *t, int *out_height);

#ifdef __cplusplus
}
#endif

#endif /* BPLUSTREE_H */
