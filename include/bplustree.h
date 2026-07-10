/*
 * bplustree.h — C port of the persistent, append-only, immutable B+ tree in
 * src/bplustree.js.
 *
 * Design (see the plan and src/bplustree.js):
 *   - The tree is file-resident: nodes and metadata are read from and appended
 *     to the backing file through the bj_io callbacks (bjio.h) supplied at
 *     create/open. No copy of the file is kept in memory — reads fetch one
 *     record at a time and each mutating operation appends its new nodes plus
 *     fresh metadata with a single write, exactly like src/bplustree.js.
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
#include "bjio.h"

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

/* Create a fresh tree (order >= 3) on `io` (expected empty) and write the
 * initial root + metadata. Returns NULL on OOM or write failure. */
bpt *bpt_create(const bj_io *io, int order);
/* Open an existing tree from `io`. Returns NULL on OOM or if the file is too
 * small / metadata is unreadable. */
bpt *bpt_open(const bj_io *io);
/* Free a tree and all its buffers (does not touch the file). Safe on NULL. */
void bpt_free(bpt *t);

/* Accessors (mirror the JS metadata fields). */
double         bpt_size(const bpt *t);
double         bpt_root(const bpt *t);
double         bpt_next_id(const bpt *t);
int            bpt_order(const bpt *t);

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
