/*
 * keyenc_wasm.c — Emscripten glue over keyenc.h.
 *
 * A composite key is built incrementally (one part per call), so this
 * exposes a small reusable builder rather than a one-shot function: JS
 * allocates one qkw with qkw_new, and every orderedKey/compositeKey/
 * compositeUpperBound call resets it, appends its parts, and reads the
 * bytes back out through qkw_ptr/qkw_len. Same shape as the dcw_out /
 * tixw_out slots elsewhere, and for the same reason -- one buffer whose
 * capacity survives across calls, rather than a malloc per key.
 *
 * Only the parts the JS surface actually builds are exposed: number,
 * string and the upper-bound sentinel. qk_put_date and qk_put_id are
 * deliberately not here -- they exist for the document layer, which
 * reaches keyenc.h directly through real C calls, and adding them would
 * grow this package's public JS API in what is meant to be a
 * de-duplication.
 *
 * Memory: heap growth may swap HEAPU8's ArrayBuffer, so JS must re-read
 * HEAPU8 after any call before touching a pointer returned by qkw_ptr.
 */
#include "keyenc.h"
#include "dbuf.h"

#include <limits.h>
#include <stdlib.h>

#ifdef __EMSCRIPTEN__
#include <emscripten/emscripten.h>
#else
#define EMSCRIPTEN_KEEPALIVE
#endif

typedef struct { dbuf buf; } qkw;

EMSCRIPTEN_KEEPALIVE qkw *qkw_new(void) {
    return (qkw *)calloc(1, sizeof(qkw));
}

EMSCRIPTEN_KEEPALIVE void qkw_free(qkw *k) {
    if (!k) return;
    dbuf_free(&k->buf);
    free(k);
}

/* Rewind to empty, keeping the allocation for the next key. */
EMSCRIPTEN_KEEPALIVE void qkw_reset(qkw *k) { k->buf.len = 0; }

EMSCRIPTEN_KEEPALIVE int qkw_put_number(qkw *k, double v) {
    return qk_put_number(&k->buf, v);
}

EMSCRIPTEN_KEEPALIVE int qkw_put_string(qkw *k, const uint8_t *utf8, int len) {
    if (len < 0) return BJ_ERR_RANGE;
    return qk_put_string(&k->buf, utf8, (uint32_t)len);
}

EMSCRIPTEN_KEEPALIVE int qkw_put_upper_bound(qkw *k) {
    return qk_put_upper_bound(&k->buf);
}

EMSCRIPTEN_KEEPALIVE const uint8_t *qkw_ptr(const qkw *k) { return k->buf.data; }

/* Negative on overflow rather than a truncated length, matching the
 * *_out_len convention in this package's other glue. */
EMSCRIPTEN_KEEPALIVE int qkw_len(const qkw *k) {
    return k->buf.len > (size_t)INT_MAX ? BJ_ERR_INT_RANGE : (int)k->buf.len;
}
