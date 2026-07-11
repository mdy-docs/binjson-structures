/*
 * textlog_wasm.c — Emscripten glue over the host-agnostic log in textlog.c.
 *
 * A log is created/opened against a JS-registered sync access handle (an `fd`
 * slot in Module.bjioHandles — see hostio.h) and its pointer handed back to JS
 * as an opaque integer handle. Text is passed as (ptr, len) UTF-8 byte ranges;
 * read outputs (getVersion / getVersionHash / getDiff) are exposed through the
 * log's own output buffer via tlw_out_ptr / tlw_out_len. All file reads and
 * writes flow through the fd's handle; no copy of the file lives in WASM
 * memory.
 *
 * Memory: heap growth may swap HEAPU8's ArrayBuffer, so JS must re-read HEAPU8
 * after any call before touching a returned pointer.
 */
#include "textlog.h"
#include "hostio.h"

#include <limits.h>

#ifdef __EMSCRIPTEN__
#include <emscripten/emscripten.h>
#else
#define EMSCRIPTEN_KEEPALIVE
#endif

EMSCRIPTEN_KEEPALIVE textlog *tlw_create(int fd, int diffs_per_snapshot) {
    bj_io io = bjio_host(fd);
    return textlog_create(&io, diffs_per_snapshot);
}
EMSCRIPTEN_KEEPALIVE textlog *tlw_open(int fd) {
    bj_io io = bjio_host(fd);
    return textlog_open(&io);
}
EMSCRIPTEN_KEEPALIVE void tlw_free(textlog *t) { textlog_free(t); }

/* Append a version. Returns the new version number, or a negative error code. */
EMSCRIPTEN_KEEPALIVE double tlw_add_version(textlog *t, const uint8_t *text,
                                            int len, double ts_ms) {
    uint64_t v = 0;
    int e = textlog_add_version(t, text, (uint32_t)len, (int64_t)ts_ms, &v);
    if (e) return (double)e;
    return (double)v;
}

EMSCRIPTEN_KEEPALIVE int tlw_get_version(textlog *t, double version) {
    const uint8_t *p; size_t n;
    return textlog_get_version(t, (uint64_t)version, &p, &n);
}
EMSCRIPTEN_KEEPALIVE int tlw_get_version_hash(textlog *t, double version) {
    const uint8_t *p; size_t n;
    return textlog_get_version_hash(t, (uint64_t)version, &p, &n);
}
EMSCRIPTEN_KEEPALIVE int tlw_get_diff(textlog *t, double from_v, double to_v) {
    const uint8_t *p; size_t n;
    return textlog_get_diff(t, (uint64_t)from_v, (uint64_t)to_v, &p, &n);
}

EMSCRIPTEN_KEEPALIVE double tlw_version(textlog *t)            { return (double)textlog_version(t); }
EMSCRIPTEN_KEEPALIVE int    tlw_diffs_per_snapshot(textlog *t) { return textlog_diffs_per_snapshot(t); }

EMSCRIPTEN_KEEPALIVE const uint8_t *tlw_out_ptr(textlog *t) {
    size_t n; return textlog_out(t, &n);
}
/* Length of the last output, or BJ_ERR_INT_RANGE if it cannot cross the
 * boundary as an int (>= 2 GB) instead of a silently truncated number. */
EMSCRIPTEN_KEEPALIVE int tlw_out_len(textlog *t) {
    size_t n; textlog_out(t, &n);
    return n > INT_MAX ? BJ_ERR_INT_RANGE : (int)n;
}
