/*
 * bjfile_wasm.c — Emscripten glue for the shared file-layer helpers in
 * bjfile.c that the JS side reuses directly. Currently just the incremental
 * CRC-32 (zlib polynomial) that protects every commit, exposed so
 * JS-written artifacts (the SnapshotStore manifest and its file checksums)
 * use the identical routine instead of a duplicated implementation.
 */
#include "bjfile.h"

#ifdef __EMSCRIPTEN__
#include <emscripten/emscripten.h>
#else
#define EMSCRIPTEN_KEEPALIVE
#endif

/* Incremental: pass 0 to start, the previous return value to continue.
 * Crosses the boundary as a signed i32; JS callers apply >>> 0. */
EMSCRIPTEN_KEEPALIVE uint32_t bjfw_crc32(uint32_t crc, const uint8_t *p, int n) {
    return bjfile_crc32(crc, p, (size_t)n);
}
