/*
 * textindex_wasm.c - Emscripten glue over textindex.c.
 *
 * The three B+ trees are created/loaded via the bplustree glue (bptw_*, also
 * exported from this module) and their opaque handles are passed into the
 * textindex operations here. Query outputs are freshly malloc'd binjson buffers
 * held in a module-static slot, read by JS via tixw_out_ptr / tixw_out_len and
 * freed on the next query.
 *
 * Memory: heap growth may swap HEAPU8's ArrayBuffer, so JS must re-read HEAPU8
 * after any call before touching a returned pointer.
 */
#include "textindex.h"

#include <stdlib.h>

#ifdef __EMSCRIPTEN__
#include <emscripten/emscripten.h>
#else
#define EMSCRIPTEN_KEEPALIVE
#endif

static uint8_t *g_out = NULL;
static size_t   g_out_len = 0;

static void reset_out(void) { free(g_out); g_out = NULL; g_out_len = 0; }

EMSCRIPTEN_KEEPALIVE int tixw_add(bpt *index, bpt *doc_terms, bpt *doc_lengths,
                                  const char *doc_id, int doc_id_len,
                                  const char *text, int text_len) {
    return tix_add(index, doc_terms, doc_lengths, doc_id, doc_id_len, text, text_len);
}

/* Returns 1 if removed, 0 if not found, negative on error. */
EMSCRIPTEN_KEEPALIVE int tixw_remove(bpt *index, bpt *doc_terms, bpt *doc_lengths,
                                     const char *doc_id, int doc_id_len) {
    int removed = 0;
    int e = tix_remove(index, doc_terms, doc_lengths, doc_id, doc_id_len, &removed);
    if (e) return e;
    return removed ? 1 : 0;
}

EMSCRIPTEN_KEEPALIVE int tixw_clear(bpt *index, bpt *doc_terms, bpt *doc_lengths) {
    return tix_clear(index, doc_terms, doc_lengths);
}

EMSCRIPTEN_KEEPALIVE int tixw_query(bpt *index, bpt *doc_terms, bpt *doc_lengths,
                                    const char *query, int query_len) {
    reset_out();
    return tix_query(index, doc_terms, doc_lengths, query, query_len, &g_out, &g_out_len);
}

EMSCRIPTEN_KEEPALIVE int tixw_query_all(bpt *index, bpt *doc_terms, bpt *doc_lengths,
                                        const char *query, int query_len) {
    reset_out();
    return tix_query_all(index, doc_terms, doc_lengths, query, query_len, &g_out, &g_out_len);
}

EMSCRIPTEN_KEEPALIVE const uint8_t *tixw_out_ptr(void) { return g_out; }
EMSCRIPTEN_KEEPALIVE int            tixw_out_len(void) { return (int)g_out_len; }
