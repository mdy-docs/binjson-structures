/*
 * bplustree_wasm.c — Emscripten glue over the host-agnostic tree in bplustree.c.
 *
 * A tree is created/loaded on the C side and its pointer handed back to JS as an
 * opaque integer handle (WASM pointers are 32-bit ints). All operations take that
 * handle plus a marshalled key: (type, num, strPtr, strLen) where type 0 = number
 * and type 1 = string. Values are pre-encoded binjson blobs (ptr+len) produced by
 * the JS side. Outputs (search value / entries / range) are exposed through the
 * tree's own output buffer via bptw_out_ptr / bptw_out_len.
 *
 * Memory: heap growth may swap HEAPU8's ArrayBuffer, so JS must re-read HEAPU8
 * after any call before touching a returned pointer.
 */
#include "bplustree.h"

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

EMSCRIPTEN_KEEPALIVE bpt *bptw_create(int order) { return bpt_create(order); }
EMSCRIPTEN_KEEPALIVE bpt *bptw_load(const uint8_t *bytes, int len) {
    return bpt_load(bytes, (size_t)len);
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

EMSCRIPTEN_KEEPALIVE double bptw_size(bpt *t)    { return bpt_size(t); }
EMSCRIPTEN_KEEPALIVE double bptw_root(bpt *t)    { return bpt_root(t); }
EMSCRIPTEN_KEEPALIVE double bptw_next_id(bpt *t) { return bpt_next_id(t); }
EMSCRIPTEN_KEEPALIVE int    bptw_order(bpt *t)   { return bpt_order(t); }

EMSCRIPTEN_KEEPALIVE const uint8_t *bptw_out_ptr(void) { return g_out_ptr; }
EMSCRIPTEN_KEEPALIVE int            bptw_out_len(void) { return (int)g_out_len; }

EMSCRIPTEN_KEEPALIVE const uint8_t *bptw_image_ptr(bpt *t) {
    size_t len; return bpt_image(t, &len);
}
EMSCRIPTEN_KEEPALIVE int bptw_image_len(bpt *t) {
    size_t len; bpt_image(t, &len); return (int)len;
}
