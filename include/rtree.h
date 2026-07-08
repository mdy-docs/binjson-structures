/*
 * rtree.h — C port of the persistent, append-only on-disk R-tree in
 * src/rtree.js.
 *
 * Design (mirrors bplustree.h / bplustree.c):
 *   - A tree owns an in-memory byte "image" mirroring the append-only file:
 *     nodes and metadata are appended verbatim, and reads resolve by offset into
 *     the image. The host (JS) loads the existing file bytes on open and writes
 *     the image back to storage on flush/close — no per-node host callbacks.
 *   - Nodes and metadata use the exact binjson wire format of rtree.js, so files
 *     stay byte-compatible (bin/rtree-decode.js can read them).
 *   - Spatial entries carry a point (lat/lng) and a 12-byte ObjectId. Bounding
 *     boxes, node splitting, choose-subtree and underflow handling are ported
 *     faithfully from the reference so results match.
 *
 * Haversine / radius math stays in JS (see src/rtree-wasm.js): C returns the
 * candidate entries within a query bounding box and JS applies the distance
 * filter, matching the reference exactly.
 *
 * All operations return BJ_OK (0) or a negative BJ_ERR_* code from binjson.h.
 */
#ifndef RTREE_H
#define RTREE_H

#include <stdint.h>
#include <stddef.h>

#include "binjson.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct rtree rtree;

/* Create a fresh empty tree (maxEntries >= 2). Returns NULL on OOM. */
rtree *rtree_create(int max_entries);
/* Load an existing tree from a file image (copies `len` bytes). Returns NULL on
 * OOM or if the image is too small / metadata is unreadable. */
rtree *rtree_load(const uint8_t *bytes, size_t len);
/* Free a tree and all its buffers. Safe to pass NULL. */
void rtree_free(rtree *t);

/* Accessors (mirror the JS metadata fields). */
double         rtree_size(const rtree *t);
int            rtree_max_entries(const rtree *t);
/* The full file image; writes its length through *len. */
const uint8_t *rtree_image(const rtree *t, size_t *len);
/* The last search / compact output; writes its length through *len. */
const uint8_t *rtree_out(const rtree *t, size_t *len);

/* Insert a point (lat, lng) with a 12-byte ObjectId. */
int rtree_insert(rtree *t, double lat, double lng, const uint8_t *oid12);
/* Remove the first entry matching `oid12`. Writes 1/0 to *removed. */
int rtree_remove(rtree *t, const uint8_t *oid12, int *removed);
/* Drop all entries by appending a fresh empty root. */
int rtree_clear(rtree *t);

/*
 * Collect all entries whose point intersects the query box, as a binjson ARRAY
 * of { objectId, lat, lng } objects. The encoded bytes are exposed via
 * out_ptr/out_len (valid until the next op).
 */
int rtree_search_bbox(rtree *t, double min_lat, double max_lat,
                      double min_lng, double max_lng,
                      const uint8_t **out_ptr, size_t *out_len);

/*
 * Rewrite the reachable nodes into a fresh, compacted image (dropping stale
 * append-only history). Exposes the new image bytes via out_ptr/out_len.
 */
int rtree_compact(rtree *t, const uint8_t **out_ptr, size_t *out_len);

#ifdef __cplusplus
}
#endif

#endif /* RTREE_H */
