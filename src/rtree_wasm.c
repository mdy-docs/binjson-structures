/*
 * rtree_wasm.c — Emscripten glue over the host-agnostic tree in rtree.c.
 *
 * A tree is created/loaded on the C side and its pointer handed back to JS as an
 * opaque integer handle (WASM pointers are 32-bit ints). Points are passed as
 * (lat, lng) doubles plus a 12-byte ObjectId (ptr). Search/compact outputs are
 * exposed through the tree's own output buffer via rtw_out_ptr / rtw_out_len.
 *
 * Memory: heap growth may swap HEAPU8's ArrayBuffer, so JS must re-read HEAPU8
 * after any call before touching a returned pointer.
 */
#include "rtree.h"

#ifdef __EMSCRIPTEN__
#include <emscripten/emscripten.h>
#else
#define EMSCRIPTEN_KEEPALIVE
#endif

EMSCRIPTEN_KEEPALIVE rtree *rtw_create(int max_entries) { return rtree_create(max_entries); }
EMSCRIPTEN_KEEPALIVE rtree *rtw_load(const uint8_t *bytes, int len) {
    return rtree_load(bytes, (size_t)len);
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

EMSCRIPTEN_KEEPALIVE int rtw_compact(rtree *t) {
    const uint8_t *p; size_t n;
    return rtree_compact(t, &p, &n);
}

EMSCRIPTEN_KEEPALIVE double rtw_size(rtree *t)        { return rtree_size(t); }
EMSCRIPTEN_KEEPALIVE int    rtw_max_entries(rtree *t) { return rtree_max_entries(t); }

EMSCRIPTEN_KEEPALIVE const uint8_t *rtw_out_ptr(rtree *t) {
    size_t n; return rtree_out(t, &n);
}
EMSCRIPTEN_KEEPALIVE int rtw_out_len(rtree *t) {
    size_t n; rtree_out(t, &n); return (int)n;
}
EMSCRIPTEN_KEEPALIVE const uint8_t *rtw_image_ptr(rtree *t) {
    size_t n; return rtree_image(t, &n);
}
EMSCRIPTEN_KEEPALIVE int rtw_image_len(rtree *t) {
    size_t n; rtree_image(t, &n); return (int)n;
}
