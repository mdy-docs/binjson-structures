/*
 * snapstore_wasm.c — Emscripten glue over snapstore.h.
 *
 * The store handle carries its own output buffer, read via
 * sstw_out_ptr/sstw_out_len, in the convention every other structure here
 * uses. Directory listings arrive as one NUL-separated buffer, because
 * bj_ns has no list(): enumeration is asynchronous in OPFS, so the host
 * passes the listing in. Opening is the two-beat trampoline snapstore.h
 * describes -- sstw_scan, then sstw_try_manifest / sstw_confirm per
 * candidate -- with the host doing the awaiting between calls.
 *
 * Memory: heap growth may swap HEAPU8's ArrayBuffer, so JS must re-read
 * HEAPU8 after any call before touching a returned pointer.
 */
#include "snapstore.h"

#include <stdlib.h>

#ifdef __EMSCRIPTEN__
#include <emscripten/emscripten.h>
#else
#define EMSCRIPTEN_KEEPALIVE
#endif

typedef struct { sst *s; dbuf out; } sstw;

EMSCRIPTEN_KEEPALIVE sstw *sstw_new(const char *prefix, int prefix_len) {
    if (prefix_len < 0) return NULL;
    sstw *w = (sstw *)calloc(1, sizeof(sstw));
    if (!w) return NULL;
    w->s = sst_new(prefix, (uint32_t)prefix_len);
    if (!w->s) { free(w); return NULL; }
    return w;
}

EMSCRIPTEN_KEEPALIVE void sstw_free(sstw *w) {
    if (!w) return;
    sst_free(w->s);
    dbuf_free(&w->out);
    free(w);
}

EMSCRIPTEN_KEEPALIVE const uint8_t *sstw_out_ptr(sstw *w) { return w->out.data; }
EMSCRIPTEN_KEEPALIVE int sstw_out_len(sstw *w) { return (int)w->out.len; }

/*
 * The store itself, for another C component in the same module that
 * takes an `sst *` -- a Raft node serving and receiving snapshot
 * installs is the one that exists (raft_node.h's rn_set_snapstore).
 *
 * BORROWED, and deliberately the same store rather than a second one
 * over the same prefix: `latest` moves when an install commits, and two
 * stores scanning one directory would be two answers to "which
 * generation is live". The host still owns it; sstw_free still ends it,
 * and anything holding this pointer must be done first.
 */
EMSCRIPTEN_KEEPALIVE sst *sstw_store(sstw *w) { return w ? w->s : NULL; }

/* ---- names ------------------------------------------------------------- */

EMSCRIPTEN_KEEPALIVE int sstw_manifest_name(sstw *w, double gen) {
    w->out.len = 0;
    return sst_manifest_name(w->s, (uint64_t)gen, &w->out);
}
EMSCRIPTEN_KEEPALIVE int sstw_data_name(sstw *w, double gen, const char *role, int role_len) {
    w->out.len = 0;
    if (role_len < 0) return BJ_ERR_RANGE;
    return sst_data_name(w->s, (uint64_t)gen, role, (uint32_t)role_len, &w->out);
}
EMSCRIPTEN_KEEPALIVE int sstw_log_name(sstw *w, double gen) {
    w->out.len = 0;
    return sst_log_name(w->s, (uint64_t)gen, &w->out);
}

/* ---- open -------------------------------------------------------------- */

EMSCRIPTEN_KEEPALIVE int sstw_scan(sstw *w, const uint8_t *listing, int listing_len) {
    if (listing_len < 0) return BJ_ERR_RANGE;
    return sst_scan(w->s, listing, (uint32_t)listing_len);
}

EMSCRIPTEN_KEEPALIVE int sstw_candidate_count(sstw *w) {
    return (int)sst_candidate_count(w->s);
}

/* Candidate i's manifest name, into the out buffer. */
EMSCRIPTEN_KEEPALIVE int sstw_candidate_manifest(sstw *w, int i) {
    w->out.len = 0;
    uint32_t len;
    const char *n = sst_candidate_manifest(w->s, (uint32_t)i, &len);
    if (!n) return BJ_ERR_RANGE;
    return dbuf_put(&w->out, (const uint8_t *)n, len);
}

EMSCRIPTEN_KEEPALIVE int sstw_try_manifest(sstw *w, int i, const uint8_t *bytes, int len) {
    if (len < 0) return BJ_ERR_RANGE;
    return sst_try_manifest(w->s, (uint32_t)i, bytes, (uint32_t)len);
}

EMSCRIPTEN_KEEPALIVE int sstw_pending_count(sstw *w) {
    return (int)sst_pending_count(w->s);
}

/* Pending file i's name, into the out buffer. */
EMSCRIPTEN_KEEPALIVE int sstw_pending_name(sstw *w, int i) {
    w->out.len = 0;
    uint32_t len;
    const char *n = sst_pending_name(w->s, (uint32_t)i, &len);
    if (!n) return BJ_ERR_RANGE;
    return dbuf_put(&w->out, (const uint8_t *)n, len);
}

/* `sizes` is a heap array of f64, one per pending file, in order. */
EMSCRIPTEN_KEEPALIVE int sstw_confirm(sstw *w, const double *sizes, int n) {
    if (n < 0) return BJ_ERR_RANGE;
    return sst_confirm(w->s, sizes, (uint32_t)n);
}

EMSCRIPTEN_KEEPALIVE int sstw_sweep_plan(sstw *w) {
    w->out.len = 0;
    return sst_sweep_plan(w->s, &w->out);
}

/* The adopted manifest, or an empty out buffer when there is none. */
EMSCRIPTEN_KEEPALIVE int sstw_latest(sstw *w) {
    w->out.len = 0;
    int has = 0;
    return sst_latest(w->s, &w->out, &has);
}
EMSCRIPTEN_KEEPALIVE int sstw_has_latest(sstw *w) { return sst_has_latest(w->s); }
EMSCRIPTEN_KEEPALIVE double sstw_latest_gen(sstw *w) { return (double)sst_latest_gen(w->s); }
EMSCRIPTEN_KEEPALIVE double sstw_next_gen(sstw *w) { return (double)sst_next_gen(w->s); }

/* ---- commit ------------------------------------------------------------ */

EMSCRIPTEN_KEEPALIVE int sstw_manifest_encode(sstw *w, double last_index, double last_term,
                                              const uint8_t *config, int config_len,
                                              const uint8_t *files, int files_len) {
    w->out.len = 0;
    if (config_len < 0 || files_len < 0) return BJ_ERR_RANGE;
    return sst_manifest_encode((uint64_t)last_index, (uint64_t)last_term,
                               config, (uint32_t)config_len,
                               files, (uint32_t)files_len, &w->out);
}

/* Adopt a just-written generation; the out buffer receives the previous
 * generation's files to delete (NUL-separated, possibly empty). */
EMSCRIPTEN_KEEPALIVE int sstw_adopt_committed(sstw *w, double gen,
                                              const uint8_t *manifest, int len) {
    w->out.len = 0;
    if (len < 0) return BJ_ERR_RANGE;
    return sst_adopt_committed(w->s, (uint64_t)gen, manifest, (uint32_t)len, &w->out);
}

/* ---- validation -------------------------------------------------------- */

/*
 * BJ_OK, or SST_ERR_CHECKSUM with the offending role left in the out
 * buffer so the caller can name it. A role rather than a filename because
 * a manifest arriving from a leader has no filenames in it -- what both
 * sides agree on is roles.
 */
EMSCRIPTEN_KEEPALIVE int sstw_check_files(sstw *w,
                                          const uint8_t *manifest, int manifest_len,
                                          const uint8_t *actual, int actual_len) {
    w->out.len = 0;
    if (manifest_len < 0 || actual_len < 0) return BJ_ERR_RANGE;
    const uint8_t *bad = NULL; uint32_t bad_len = 0;
    int e = sst_check_files(manifest, (uint32_t)manifest_len,
                            actual, (uint32_t)actual_len, &bad, &bad_len);
    if (bad && bad_len) dbuf_put(&w->out, bad, bad_len);
    return e;
}

/* ---- paired entry logs -------------------------------------------------- */

EMSCRIPTEN_KEEPALIVE int sstw_log_candidates(sstw *w, const uint8_t *listing, int listing_len) {
    w->out.len = 0;
    if (listing_len < 0) return BJ_ERR_RANGE;
    return sst_log_candidates(w->s, listing, (uint32_t)listing_len, &w->out);
}

EMSCRIPTEN_KEEPALIVE int sstw_prune_logs_plan(sstw *w, const uint8_t *listing, int listing_len,
                                              const char *keep, int keep_len) {
    w->out.len = 0;
    if (listing_len < 0 || keep_len < 0) return BJ_ERR_RANGE;
    return sst_prune_logs_plan(w->s, listing, (uint32_t)listing_len,
                               keep, (uint32_t)keep_len, &w->out);
}
