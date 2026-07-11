/*
 * textindex_wasm.c - Emscripten glue over textindex.c.
 *
 * The three B+ trees are created/loaded via the bplustree glue (bptw_*, also
 * exported from this module) and their opaque handles are passed into the
 * textindex operations here. Query outputs are freshly malloc'd binjson buffers
 * held in a module-static slot, read by JS via tixw_out_ptr / tixw_out_len and
 * freed on the next query.
 *
 * The mutating operations take a journal file descriptor (a registered
 * Module.bjioHandles slot, like the tree fds) for cross-tree crash atomicity;
 * pass a negative fd to disable journaling. tixw_recover runs the journal
 * reconciliation and must be called right after the three trees are opened.
 *
 * Memory: heap growth may swap HEAPU8's ArrayBuffer, so JS must re-read HEAPU8
 * after any call before touching a returned pointer.
 */
#include "textindex.h"
#include "hostio.h"

#include <stdlib.h>

#ifdef __EMSCRIPTEN__
#include <emscripten/emscripten.h>
#else
#define EMSCRIPTEN_KEEPALIVE
#endif

static uint8_t *g_out = NULL;
static size_t   g_out_len = 0;

static void reset_out(void) { free(g_out); g_out = NULL; g_out_len = 0; }

EMSCRIPTEN_KEEPALIVE int tixw_recover(int jfd, bpt *index, bpt *doc_terms,
                                      bpt *doc_lengths) {
    if (jfd < 0) return BJ_OK;
    bj_io jio = bjio_host(jfd);
    return tix_recover(&jio, index, doc_terms, doc_lengths);
}

EMSCRIPTEN_KEEPALIVE int tixw_add(bpt *index, bpt *doc_terms, bpt *doc_lengths,
                                  int jfd, const char *doc_id, int doc_id_len,
                                  const char *text, int text_len) {
    bj_io jio;
    const bj_io *jp = NULL;
    if (jfd >= 0) { jio = bjio_host(jfd); jp = &jio; }
    return tix_add(index, doc_terms, doc_lengths, jp, doc_id, doc_id_len, text, text_len);
}

/* Returns 1 if removed, 0 if not found, negative on error. */
EMSCRIPTEN_KEEPALIVE int tixw_remove(bpt *index, bpt *doc_terms, bpt *doc_lengths,
                                     int jfd, const char *doc_id, int doc_id_len) {
    bj_io jio;
    const bj_io *jp = NULL;
    if (jfd >= 0) { jio = bjio_host(jfd); jp = &jio; }
    int removed = 0;
    int e = tix_remove(index, doc_terms, doc_lengths, jp, doc_id, doc_id_len, &removed);
    if (e) return e;
    return removed ? 1 : 0;
}

EMSCRIPTEN_KEEPALIVE int tixw_clear(bpt *index, bpt *doc_terms, bpt *doc_lengths,
                                    int jfd) {
    bj_io jio;
    const bj_io *jp = NULL;
    if (jfd >= 0) { jio = bjio_host(jfd); jp = &jio; }
    return tix_clear(index, doc_terms, doc_lengths, jp);
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

EMSCRIPTEN_KEEPALIVE double tixw_term_count(bpt *index) {
    int64_t n = 0;
    int e = tix_term_count(index, &n);
    return e ? (double)e : (double)n;
}

EMSCRIPTEN_KEEPALIVE const uint8_t *tixw_out_ptr(void) { return g_out; }
EMSCRIPTEN_KEEPALIVE int            tixw_out_len(void) { return (int)g_out_len; }
