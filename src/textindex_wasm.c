/*
 * textindex_wasm.c - Emscripten glue over textindex.c.
 *
 * The three B+ trees are created/loaded via the bplustree glue (bptw_*, also
 * exported from this module) and their opaque handles are passed into the
 * textindex operations here. Query outputs are freshly malloc'd binjson
 * buffers held in a per-index output slot (tixw_out_new / tixw_out_free, one
 * per open index so two indexes never clobber each other's unread result),
 * read by JS via tixw_out_ptr / tixw_out_len and freed on the next query.
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

#include <limits.h>
#include <stdlib.h>

#ifdef __EMSCRIPTEN__
#include <emscripten/emscripten.h>
#else
#define EMSCRIPTEN_KEEPALIVE
#endif

/* One query-output slot per open index. */
typedef struct { uint8_t *buf; size_t len; } tixw_out;

EMSCRIPTEN_KEEPALIVE tixw_out *tixw_out_new(void) {
    return (tixw_out *)calloc(1, sizeof(tixw_out));
}
EMSCRIPTEN_KEEPALIVE void tixw_out_free(tixw_out *o) {
    if (!o) return;
    free(o->buf);
    free(o);
}

static void reset_out(tixw_out *o) { free(o->buf); o->buf = NULL; o->len = 0; }

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

EMSCRIPTEN_KEEPALIVE int tixw_query(tixw_out *o, bpt *index, bpt *doc_terms,
                                    bpt *doc_lengths, const char *query, int query_len) {
    reset_out(o);
    return tix_query(index, doc_terms, doc_lengths, query, query_len, &o->buf, &o->len);
}

EMSCRIPTEN_KEEPALIVE int tixw_query_all(tixw_out *o, bpt *index, bpt *doc_terms,
                                        bpt *doc_lengths, const char *query, int query_len) {
    reset_out(o);
    return tix_query_all(index, doc_terms, doc_lengths, query, query_len, &o->buf, &o->len);
}

EMSCRIPTEN_KEEPALIVE double tixw_term_count(bpt *index) {
    int64_t n = 0;
    int e = tix_term_count(index, &n);
    return e ? (double)e : (double)n;
}

EMSCRIPTEN_KEEPALIVE const uint8_t *tixw_out_ptr(tixw_out *o) { return o->buf; }
/* Length of the slot's last output, or BJ_ERR_INT_RANGE if it cannot cross
 * the boundary as an int (>= 2 GB) instead of a silently truncated number. */
EMSCRIPTEN_KEEPALIVE int tixw_out_len(tixw_out *o) {
    return o->len > INT_MAX ? BJ_ERR_INT_RANGE : (int)o->len;
}
