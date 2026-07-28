/*
 * entrylog_wasm.c — Emscripten glue over the host-agnostic log in entrylog.c.
 *
 * A log is created/opened against a JS-registered sync access handle (an `fd`
 * slot in Module.bjioHandles — see hostio.h) and its pointer handed back to JS
 * as an opaque integer handle. Indexes and terms cross the boundary as doubles
 * (lossless to 2^53, matching their JS-safe-integer wire encoding); payloads
 * are (ptr, len) byte ranges. Read outputs (get / get_batch) are exposed
 * through the log's own output buffer via elw_out_ptr / elw_out_len.
 *
 * Memory: heap growth may swap HEAPU8's ArrayBuffer, so JS must re-read HEAPU8
 * after any call before touching a returned pointer.
 */
#include "entrylog.h"
#include "hostio.h"

#include <limits.h>

#ifdef __EMSCRIPTEN__
#include <emscripten/emscripten.h>
#else
#define EMSCRIPTEN_KEEPALIVE
#endif

EMSCRIPTEN_KEEPALIVE elog *elw_create_at(int fd, double base_index,
                                         double base_term) {
    bj_io io = bjio_host(fd);
    return elog_create_at(&io, (uint64_t)base_index, (uint64_t)base_term);
}
EMSCRIPTEN_KEEPALIVE elog *elw_open(int fd) {
    bj_io io = bjio_host(fd);
    return elog_open(&io);
}
EMSCRIPTEN_KEEPALIVE void elw_free(elog *t) { elog_free(t); }

/* Buffer one entry. Returns the assigned index, or a negative error code.
 * NOT durable until elw_sync. */
EMSCRIPTEN_KEEPALIVE double elw_append(elog *t, double term, int type,
                                       const uint8_t *payload, int len) {
    uint64_t index = 0;
    int e = elog_append(t, (uint64_t)term, type, payload, (uint32_t)len, &index);
    if (e) return (double)e;
    return (double)index;
}

EMSCRIPTEN_KEEPALIVE int elw_sync(elog *t) { return elog_sync(t); }

EMSCRIPTEN_KEEPALIVE int elw_set_hard_state(elog *t, double term,
                                            double voted_for) {
    return elog_set_hard_state(t, (uint64_t)term, (uint64_t)voted_for);
}
EMSCRIPTEN_KEEPALIVE int elw_set_commit_index(elog *t, double index) {
    return elog_set_commit_index(t, (uint64_t)index);
}

/* Read one entry. On BJ_OK the payload is in the output buffer and `slots`
 * (an 8-byte-aligned 16-byte scratch area in the heap) holds the entry's
 * term as an f64 at +0 and its type as an i32 at +8. */
EMSCRIPTEN_KEEPALIVE int elw_get(elog *t, double index, uint8_t *slots) {
    const uint8_t *p; size_t n;
    uint64_t term = 0; int type = 0;
    int e = elog_get(t, (uint64_t)index, &term, &type, &p, &n);
    if (e) return e;
    double td = (double)term;
    __builtin_memcpy(slots, &td, 8);
    __builtin_memcpy(slots + 8, &type, 4);
    return BJ_OK;
}

/* Encode entries from `from_index` as an ARRAY of { index, term, type,
 * payload } into the output buffer. Returns the entry count (0 = nothing at
 * or above from_index), or a negative error code. */
EMSCRIPTEN_KEEPALIVE int elw_get_batch(elog *t, double from_index,
                                       int max_bytes) {
    const uint8_t *p; size_t n;
    int count = 0;
    int e = elog_get_batch(t, (uint64_t)from_index, (size_t)max_bytes,
                           &count, &p, &n);
    if (e) return e;
    return count;
}

EMSCRIPTEN_KEEPALIVE int elw_truncate_from(elog *t, double index) {
    return elog_truncate_from(t, (uint64_t)index);
}

EMSCRIPTEN_KEEPALIVE int elw_compact(elog *t, int dst_fd,
                                     double new_base_index,
                                     double new_base_term) {
    bj_io io = bjio_host(dst_fd);
    return elog_compact(t, &io, (uint64_t)new_base_index,
                        (uint64_t)new_base_term);
}

EMSCRIPTEN_KEEPALIVE int elw_verify(elog *t) { return elog_verify(t); }

/* Term of entry `index` (base_index answers base_term). Returns the term as
 * a double, or a negative error code — terms are non-negative, so the sign
 * disambiguates. */
EMSCRIPTEN_KEEPALIVE double elw_term_at(elog *t, double index) {
    uint64_t term = 0;
    int e = elog_term_at(t, (uint64_t)index, &term);
    if (e) return (double)e;
    return (double)term;
}

EMSCRIPTEN_KEEPALIVE double elw_base_index(elog *t)   { return (double)elog_base_index(t); }
EMSCRIPTEN_KEEPALIVE double elw_base_term(elog *t)    { return (double)elog_base_term(t); }
EMSCRIPTEN_KEEPALIVE double elw_last_index(elog *t)   { return (double)elog_last_index(t); }
EMSCRIPTEN_KEEPALIVE double elw_last_term(elog *t)    { return (double)elog_last_term(t); }
EMSCRIPTEN_KEEPALIVE double elw_current_term(elog *t) { return (double)elog_current_term(t); }
EMSCRIPTEN_KEEPALIVE double elw_voted_for(elog *t)    { return (double)elog_voted_for(t); }
EMSCRIPTEN_KEEPALIVE double elw_commit_index(elog *t) { return (double)elog_commit_index(t); }
EMSCRIPTEN_KEEPALIVE double elw_file_len(elog *t)     { return (double)elog_file_len(t); }

EMSCRIPTEN_KEEPALIVE const uint8_t *elw_out_ptr(elog *t) {
    size_t n; return elog_out(t, &n);
}
/* Length of the last output, or BJ_ERR_INT_RANGE if it cannot cross the
 * boundary as an int (>= 2 GB) instead of a silently truncated number. */
EMSCRIPTEN_KEEPALIVE int elw_out_len(elog *t) {
    size_t n; elog_out(t, &n);
    return n > INT_MAX ? BJ_ERR_INT_RANGE : (int)n;
}
