/*
 * hostio.c — see hostio.h.
 *
 * OPFS sync access handles are synchronous APIs, so each EM_JS body below is a
 * plain blocking call: `read` fills a HEAPU8 subarray view of the caller's
 * buffer (the OPFS read lands directly in WASM memory) and `write` hands the
 * host a view of the caller's bytes. Offsets and lengths cross the bridge as
 * doubles (lossless below 2^53, far beyond any OPFS file).
 *
 * The non-Emscripten fallbacks exist only so accidental native compilation
 * links; native hosts should supply their own bj_io.
 */
#include "hostio.h"
#include "binjson.h"

#ifdef __EMSCRIPTEN__
#include <emscripten/em_js.h>

EM_JS(double, bjio_js_size, (int fd), {
    return Module['bjioHandles'][fd].getSize();
});
EM_JS(int, bjio_js_read, (int fd, double off, uint8_t *buf, int len), {
    return Module['bjioHandles'][fd].read(HEAPU8.subarray(buf, buf + len), { at: off });
});
EM_JS(int, bjio_js_write, (int fd, double off, const uint8_t *buf, int len), {
    var wrote = Module['bjioHandles'][fd].write(HEAPU8.subarray(buf, buf + len), { at: off });
    return wrote === len ? 0 : -3 /* BJ_ERR_EOF: short write */;
});
EM_JS(void, bjio_js_truncate, (int fd, double len), {
    Module['bjioHandles'][fd].truncate(len);
});

#else /* !__EMSCRIPTEN__ */

static double bjio_js_size(int fd) { (void)fd; return 0; }
static int bjio_js_read(int fd, double off, uint8_t *buf, int len) {
    (void)fd; (void)off; (void)buf; (void)len; return BJ_ERR_STATE;
}
static int bjio_js_write(int fd, double off, const uint8_t *buf, int len) {
    (void)fd; (void)off; (void)buf; (void)len; return BJ_ERR_STATE;
}
static void bjio_js_truncate(int fd, double len) { (void)fd; (void)len; }

#endif /* __EMSCRIPTEN__ */

static uint64_t hio_size(void *ctx) {
    return (uint64_t)bjio_js_size((int)(intptr_t)ctx);
}
static int64_t hio_read(void *ctx, uint64_t off, uint8_t *buf, uint32_t len) {
    return (int64_t)bjio_js_read((int)(intptr_t)ctx, (double)off, buf, (int)len);
}
static int32_t hio_write(void *ctx, uint64_t off, const uint8_t *buf, uint32_t len) {
    return (int32_t)bjio_js_write((int)(intptr_t)ctx, (double)off, buf, (int)len);
}
static int32_t hio_truncate(void *ctx, uint64_t len) {
    bjio_js_truncate((int)(intptr_t)ctx, (double)len);
    return BJ_OK;
}

bj_io bjio_host(int fd) {
    bj_io io;
    io.ctx = (void *)(intptr_t)fd;
    io.size = hio_size;
    io.read = hio_read;
    io.write = hio_write;
    io.truncate = hio_truncate;
    return io;
}
