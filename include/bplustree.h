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

/* Accessors (mirror the JS metadata fields; the WASM glue converts to the
 * doubles JS expects). */
int64_t        bpt_size(const bpt *t);
uint64_t       bpt_root(const bpt *t);
uint64_t       bpt_next_id(const bpt *t);
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

/* Current length of the backing file (committed + pending bytes). */
uint64_t bpt_file_len(const bpt *t);
/*
 * Truncate the backing file to `len` — which must be a commit boundary
 * (the bytes at len-135 must be a valid metadata record) — and reload the
 * tree's state from it. Because the file is append-only, this atomically
 * rewinds the tree to the state it had when that commit landed. Used for
 * cross-file transaction rollback (textindex journal).
 */
int bpt_rewind(bpt *t, uint64_t len);

/*
 * Rewrite the live entries (dropping append-only history and deletion cruft)
 * into the destination file `dst`, which is expected to be empty, as a
 * minimal fully-packed tree via a bulk load. Records are streamed to the
 * host in chunks; memory use is O(height).
 */
int bpt_compact(bpt *t, const bj_io *dst);

/* ---- Cursors ---------------------------------------------------------- */

/*
 * A cursor streams entries in sorted key order with bounded memory: it holds
 * a descent stack plus the current leaf (O(height) state) and reads one leaf
 * at a time, never materializing the result set.
 *
 * Cursors pin the root pointer at open, so they iterate a consistent
 * snapshot: because the tree is append-only, a cursor stays valid across
 * concurrent bpt_add/bpt_delete calls on the same tree and simply does not
 * see them. Close every cursor before bpt_free.
 */
typedef struct bpt_cursor bpt_cursor;

/* Open a cursor over min <= key <= max; either bound may be NULL for an open
 * end (both NULL = full scan). Returns NULL on OOM or unreadable root. */
bpt_cursor *bpt_cursor_open(bpt *t, const bpt_key *min, const bpt_key *max);
/*
 * Advance to the next entry. Returns 1 with *key / *val / *val_len exposing
 * the entry (pointers valid until the next call on this cursor), 0 at end,
 * or a negative BJ_ERR_* code.
 */
int bpt_cursor_next(bpt_cursor *c, bpt_key *key,
                    const uint8_t **val, size_t *val_len);
/*
 * Pull entries in bulk: encode entries as a binjson ARRAY of { key, value }
 * objects into the tree's output buffer until roughly `max_bytes` of payload
 * is gathered or the cursor ends. Writes the entry count through *count
 * (0 = end); bytes are exposed via out_ptr/out_len (valid until the next
 * operation on the tree).
 */
int bpt_cursor_next_batch(bpt_cursor *c, size_t max_bytes, int *count,
                          const uint8_t **out_ptr, size_t *out_len);
/* Release a cursor (never touches the file). Safe to pass NULL. */
void bpt_cursor_close(bpt_cursor *c);

#ifdef __cplusplus
}
#endif

#endif /* BPLUSTREE_H */
