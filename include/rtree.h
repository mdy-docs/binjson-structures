/*
 * rtree.h — C port of the persistent, append-only on-disk R-tree in
 * src/rtree.js.
 *
 * Design (mirrors bplustree.h / bplustree.c):
 *   - The tree is file-resident: nodes and metadata are read from and appended
 *     to the backing file through the bj_io callbacks (bjio.h) supplied at
 *     create/open. No copy of the file is kept in memory — reads fetch one
 *     record at a time and each mutation appends its new nodes plus fresh
 *     metadata with a single write, exactly like src/rtree.js.
 *   - Nodes and metadata use the exact binjson wire format of rtree.js, so files
 *     stay byte-compatible (bin/rtree-decode.js can read them).
 *   - Spatial entries carry a point (lat/lng) and a 12-byte ObjectId. Bounding
 *     boxes, node splitting, choose-subtree and underflow handling are ported
 *     faithfully from the reference so results match.
 *
 * Radius search is fully in C: rtree_search_radius converts the radius to a
 * bounding box, traverses the tree and applies the haversine distance filter,
 * all using c/geo.c (WASM libm). Results may differ from the JS reference by a
 * few ULPs in the reported distances, which is acceptable.
 *
 * All operations return BJ_OK (0) or a negative BJ_ERR_* code from binjson.h.
 */
#ifndef RTREE_H
#define RTREE_H

#include <stdint.h>
#include <stddef.h>

#include "binjson.h"
#include "bjio.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct rtree rtree;

/* Create a fresh empty tree (maxEntries >= 2) on `io` (expected empty) and
 * write the initial root + metadata. Returns NULL on OOM or write failure. */
rtree *rtree_create(const bj_io *io, int max_entries);
/* Open an existing tree from `io`. Returns NULL on OOM or if the file is too
 * small / metadata is unreadable. */
rtree *rtree_open(const bj_io *io);
/* Free a tree and all its buffers (does not touch the file). Safe on NULL. */
void rtree_free(rtree *t);

/* Accessors (mirror the JS metadata fields). */
int64_t        rtree_size(const rtree *t);
int            rtree_max_entries(const rtree *t);
/* The last search output; writes its length through *len. */
const uint8_t *rtree_out(const rtree *t, size_t *len);

/* ---- Spatial cursor ---------------------------------------------------- */

/*
 * Streams bounding-box matches with bounded memory (a descent stack plus
 * one leaf) instead of materializing the result set. The root is pinned at
 * open: the tree being append-only, a cursor iterates a consistent snapshot
 * across concurrent mutations. Close every cursor before rtree_free.
 */
typedef struct rtree_cursor rtree_cursor;

/* Open a cursor over entries inside the box. NaN bounds are rejected
 * (returns NULL, as for OOM or an unreadable root). */
rtree_cursor *rtree_cursor_open(rtree *t, double min_lat, double max_lat,
                                double min_lng, double max_lng);
/* Advance: 1 = entry written to lat/lng/oid12, 0 = end, negative = error. */
int rtree_cursor_next(rtree_cursor *c, double *lat, double *lng, uint8_t oid12[12]);
/*
 * Pull entries in bulk as a binjson ARRAY of { objectId, lat, lng } (the
 * searchBBox result shape) until ~max_bytes accumulate or the cursor ends.
 * *count = 0 signals the end; bytes valid until the next op on the tree.
 */
int rtree_cursor_next_batch(rtree_cursor *c, size_t max_bytes, int *count,
                            const uint8_t **out_ptr, size_t *out_len);
void rtree_cursor_close(rtree_cursor *c);

/*
 * The k nearest entries to a point, best-first over node bounding boxes:
 * reads only the subtrees whose boxes can beat the current candidates,
 * instead of scanning. Result: binjson ARRAY of { objectId, lat, lng,
 * distance } sorted by ascending haversine distance (km), at most k long.
 */
int rtree_nearest(rtree *t, double lat, double lng, int k,
                  const uint8_t **out_ptr, size_t *out_len);

/*
 * Insert a point (lat, lng) with a 12-byte ObjectId.
 *
 * OID uniqueness is the caller's contract: the tree never checks for
 * duplicates, so inserting the same OID twice stores two independent
 * entries, both searches return both, and one remove takes out only one
 * (whichever probing finds first). A storage engine keying rows by OID
 * must guarantee uniqueness above this layer.
 */
int rtree_insert(rtree *t, double lat, double lng, const uint8_t *oid12);
/* Remove the first entry matching `oid12`. Writes 1/0 to *removed. */
int rtree_remove(rtree *t, const uint8_t *oid12, int *removed);
/*
 * Remove by OID with its known location. OIDs have no spatial locality, so
 * rtree_remove probes subtrees in order (worst-case a full-tree scan);
 * supplying the entry's stored coordinates lets bbox pruning skip every
 * subtree that cannot contain the point — O(height) on well-separated
 * trees. The point must equal the coordinates the entry was inserted with;
 * a different point simply finds nothing (*removed = 0).
 */
int rtree_remove_at(rtree *t, double lat, double lng, const uint8_t *oid12,
                    int *removed);
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
 * Collect all entries within `radius_km` of (lat, lng), as a binjson ARRAY of
 * { objectId, lat, lng, distance } objects (distance in km). The encoded bytes
 * are exposed via out_ptr/out_len (valid until the next op).
 */
int rtree_search_radius(rtree *t, double lat, double lng, double radius_km,
                        const uint8_t **out_ptr, size_t *out_len);

/*
 * Rewrite the reachable nodes (dropping stale append-only history) into the
 * destination file `dst`, which is expected to be empty. Records are streamed
 * to the host in chunks; nothing is retained in memory.
 */
int rtree_compact(rtree *t, const bj_io *dst);

#ifdef __cplusplus
}
#endif

#endif /* RTREE_H */
