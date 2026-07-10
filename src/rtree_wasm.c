/*
 * rtree_wasm.c — Emscripten glue over the host-agnostic tree in rtree.c.
 *
 * A tree is created/opened against a JS-registered sync access handle (an `fd`
 * slot in Module.bjioHandles — see hostio.h) and its pointer handed back to JS
 * as an opaque integer handle (WASM pointers are 32-bit ints). Points are
 * passed as (lat, lng) doubles plus a 12-byte ObjectId (ptr). Search outputs
 * are exposed through the tree's own output buffer via rtw_out_ptr /
 * rtw_out_len. All file reads and writes flow through the fd's handle; no copy
 * of the file lives in WASM memory.
 *
 * Memory: heap growth may swap HEAPU8's ArrayBuffer, so JS must re-read HEAPU8
 * after any call before touching a returned pointer.
 */
#include "rtree.h"
#include "hostio.h"
#include "geo.h"

#ifdef __EMSCRIPTEN__
#include <emscripten/emscripten.h>
#else
#define EMSCRIPTEN_KEEPALIVE
#endif

EMSCRIPTEN_KEEPALIVE rtree *rtw_create(int fd, int max_entries) {
    bj_io io = bjio_host(fd);
    return rtree_create(&io, max_entries);
}
EMSCRIPTEN_KEEPALIVE rtree *rtw_open(int fd) {
    bj_io io = bjio_host(fd);
    return rtree_open(&io);
}
EMSCRIPTEN_KEEPALIVE void rtw_free(rtree *t) { rtree_free(t); }

EMSCRIPTEN_KEEPALIVE int rtw_insert(rtree *t, double lat, double lng, const uint8_t *oid) {
    return rtree_insert(t, lat, lng, oid);
}

/* Returns 1 if an entry was removed, 0 if not found, negative on error. */
EMSCRIPTEN_KEEPALIVE int rtw_remove(rtree *t, const uint8_t *oid) {
    int removed = 0;
    int e = rtree_remove(t, oid, &removed);
    if (e) return e;
    return removed ? 1 : 0;
}

EMSCRIPTEN_KEEPALIVE int rtw_clear(rtree *t) { return rtree_clear(t); }

EMSCRIPTEN_KEEPALIVE int rtw_search(rtree *t, double min_lat, double max_lat,
                                    double min_lng, double max_lng) {
    const uint8_t *p; size_t n;
    return rtree_search_bbox(t, min_lat, max_lat, min_lng, max_lng, &p, &n);
}

EMSCRIPTEN_KEEPALIVE int rtw_search_radius(rtree *t, double lat, double lng, double radius_km) {
    const uint8_t *p; size_t n;
    return rtree_search_radius(t, lat, lng, radius_km, &p, &n);
}

/* Standalone haversine (km) for the exported haversineDistance utility. */
EMSCRIPTEN_KEEPALIVE double rtw_haversine(double lat1, double lng1, double lat2, double lng2) {
    return geo_haversine_distance(lat1, lng1, lat2, lng2);
}

/* Stream a compacted copy into the (empty) destination handle `dst_fd`. */
EMSCRIPTEN_KEEPALIVE int rtw_compact(rtree *t, int dst_fd) {
    bj_io dst = bjio_host(dst_fd);
    return rtree_compact(t, &dst);
}

EMSCRIPTEN_KEEPALIVE double rtw_size(rtree *t)        { return rtree_size(t); }
EMSCRIPTEN_KEEPALIVE int    rtw_max_entries(rtree *t) { return rtree_max_entries(t); }

EMSCRIPTEN_KEEPALIVE const uint8_t *rtw_out_ptr(rtree *t) {
    size_t n; return rtree_out(t, &n);
}
EMSCRIPTEN_KEEPALIVE int rtw_out_len(rtree *t) {
    size_t n; rtree_out(t, &n); return (int)n;
}
