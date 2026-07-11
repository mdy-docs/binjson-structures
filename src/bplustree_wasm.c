/*
 * bplustree_wasm.c — Emscripten glue over the host-agnostic tree in bplustree.c.
 *
 * A tree is created/opened against a JS-registered sync access handle (an `fd`
 * slot in Module.bjioHandles — see hostio.h) and its pointer handed back to JS
 * as an opaque integer handle (WASM pointers are 32-bit ints). All operations
 * take that handle plus a marshalled key: (type, num, strPtr, strLen) where
 * type 0 = number and type 1 = string. Values are pre-encoded binjson blobs
 * (ptr+len) produced by the JS side. Outputs (search value / entries / range)
 * are exposed through the tree's own output buffer via bptw_out_ptr /
 * bptw_out_len. All file reads and writes flow through the fd's handle; no
 * copy of the file lives in WASM memory.
 *
 * Memory: heap growth may swap HEAPU8's ArrayBuffer, so JS must re-read HEAPU8
 * after any call before touching a returned pointer.
 */
#include "bplustree.h"
#include "hostio.h"

#include <stdlib.h>

#ifdef __EMSCRIPTEN__
#include <emscripten/emscripten.h>
#else
#define EMSCRIPTEN_KEEPALIVE
#endif

static bpt_key make_key(int type, double num, const uint8_t *sptr, int slen) {
    bpt_key k;
    k.is_string = type ? 1 : 0;
    k.num = num;
    k.str = sptr;
    k.str_len = (uint32_t)slen;
    return k;
}

/* The last search/entries/range output for a tree lives in the tree's own
 * buffer; JS reads it right after the call that produced it. */
static const uint8_t *g_out_ptr = NULL;
static size_t         g_out_len = 0;

EMSCRIPTEN_KEEPALIVE bpt *bptw_create(int fd, int order) {
    bj_io io = bjio_host(fd);
    return bpt_create(&io, order);
}
EMSCRIPTEN_KEEPALIVE bpt *bptw_snapshot(bpt *t) {
    return bpt_snapshot(t);
}

EMSCRIPTEN_KEEPALIVE bpt *bptw_open_at(int fd, double len) {
    bj_io io = bjio_host(fd);
    return bpt_open_at(&io, (uint64_t)len);
}

EMSCRIPTEN_KEEPALIVE int bptw_boundaries(bpt *t) {
    g_out_ptr = NULL; g_out_len = 0;
    return bpt_boundaries(t, &g_out_ptr, &g_out_len);
}

EMSCRIPTEN_KEEPALIVE int bptw_is_snapshot(bpt *t) {
    return bpt_is_snapshot(t);
}

EMSCRIPTEN_KEEPALIVE bpt *bptw_open(int fd) {
    bj_io io = bjio_host(fd);
    return bpt_open(&io);
}
EMSCRIPTEN_KEEPALIVE void bptw_free(bpt *t) { bpt_free(t); }

EMSCRIPTEN_KEEPALIVE int bptw_add(bpt *t, int ktype, double knum,
                                  const uint8_t *kptr, int klen,
                                  const uint8_t *vptr, int vlen) {
    bpt_key k = make_key(ktype, knum, kptr, klen);
    return bpt_add(t, &k, vptr, (uint32_t)vlen);
}

EMSCRIPTEN_KEEPALIVE int bptw_delete(bpt *t, int ktype, double knum,
                                     const uint8_t *kptr, int klen) {
    bpt_key k = make_key(ktype, knum, kptr, klen);
    return bpt_delete(t, &k);
}

/* Returns 1 if found (value in out buffer), 0 if absent, negative on error. */
EMSCRIPTEN_KEEPALIVE int bptw_search(bpt *t, int ktype, double knum,
                                     const uint8_t *kptr, int klen) {
    bpt_key k = make_key(ktype, knum, kptr, klen);
    int found = 0;
    g_out_ptr = NULL; g_out_len = 0;
    int e = bpt_search(t, &k, &found, &g_out_ptr, &g_out_len);
    if (e) return e;
    return found ? 1 : 0;
}

EMSCRIPTEN_KEEPALIVE int bptw_entries(bpt *t) {
    g_out_ptr = NULL; g_out_len = 0;
    return bpt_entries(t, &g_out_ptr, &g_out_len);
}

EMSCRIPTEN_KEEPALIVE int bptw_range(bpt *t,
                                    int minType, double minNum, const uint8_t *minPtr, int minLen,
                                    int maxType, double maxNum, const uint8_t *maxPtr, int maxLen) {
    bpt_key mn = make_key(minType, minNum, minPtr, minLen);
    bpt_key mx = make_key(maxType, maxNum, maxPtr, maxLen);
    g_out_ptr = NULL; g_out_len = 0;
    return bpt_range(t, &mn, &mx, &g_out_ptr, &g_out_len);
}

/* Height (>= 0) or a negative error code. */
EMSCRIPTEN_KEEPALIVE int bptw_height(bpt *t) {
    int h = 0;
    int e = bpt_height(t, &h);
    return e ? e : h;
}

/* Stream a compacted (bulk-loaded) copy into the empty destination handle. */
EMSCRIPTEN_KEEPALIVE int bptw_compact(bpt *t, int dst_fd) {
    bj_io dst = bjio_host(dst_fd);
    return bpt_compact(t, &dst);
}

/*
 * Cursors: open over [min, max] where a key type of -1 means "no bound"
 * (both -1 = full scan). bptw_cursor_next pulls up to ~max_bytes of entries
 * as a binjson ARRAY of { key, value } into the shared out buffer and
 * returns the entry count (0 = end) or a negative error.
 */
EMSCRIPTEN_KEEPALIVE bpt_cursor *bptw_cursor_open(bpt *t,
        int minType, double minNum, const uint8_t *minPtr, int minLen,
        int maxType, double maxNum, const uint8_t *maxPtr, int maxLen) {
    bpt_key mn, mx;
    const bpt_key *pmn = NULL, *pmx = NULL;
    if (minType >= 0) { mn = make_key(minType, minNum, minPtr, minLen); pmn = &mn; }
    if (maxType >= 0) { mx = make_key(maxType, maxNum, maxPtr, maxLen); pmx = &mx; }
    return bpt_cursor_open(t, pmn, pmx);
}

EMSCRIPTEN_KEEPALIVE int bptw_cursor_next(bpt_cursor *cur, int max_bytes) {
    g_out_ptr = NULL; g_out_len = 0;
    int count = 0;
    int e = bpt_cursor_next_batch(cur, (size_t)max_bytes, &count,
                                  &g_out_ptr, &g_out_len);
    return e ? e : count;
}

EMSCRIPTEN_KEEPALIVE void bptw_cursor_free(bpt_cursor *cur) {
    bpt_cursor_close(cur);
}

EMSCRIPTEN_KEEPALIVE double bptw_size(bpt *t)    { return (double)bpt_size(t); }
EMSCRIPTEN_KEEPALIVE double bptw_root(bpt *t)    { return (double)bpt_root(t); }
EMSCRIPTEN_KEEPALIVE double bptw_next_id(bpt *t) { return (double)bpt_next_id(t); }
EMSCRIPTEN_KEEPALIVE int    bptw_order(bpt *t)   { return bpt_order(t); }

EMSCRIPTEN_KEEPALIVE const uint8_t *bptw_out_ptr(void) { return g_out_ptr; }
EMSCRIPTEN_KEEPALIVE int            bptw_out_len(void) { return (int)g_out_len; }
