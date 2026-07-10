/*
 * bjio.h — host file I/O interface for the persistent structures.
 *
 * The B+ tree, R-tree and text log are file-resident: they never hold a copy
 * of the file in memory. Every read and append goes through this callback
 * table, which the host backs with its storage primitive (an OPFS
 * FileSystemSyncAccessHandle in the WASM build — see hostio.h — or plain file
 * descriptors in a native build). All callbacks are synchronous.
 */
#ifndef BJIO_H
#define BJIO_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct bj_io {
    void *ctx;
    /* Current file size in bytes. */
    uint64_t (*size)(void *ctx);
    /* Read up to `len` bytes at `off` into `buf`. Returns the number of bytes
     * read (short only at end of file) or a negative BJ_ERR_* code. */
    int64_t (*read)(void *ctx, uint64_t off, uint8_t *buf, uint32_t len);
    /* Write `len` bytes at `off`, extending the file as needed. Returns BJ_OK
     * or a negative BJ_ERR_* code. */
    int32_t (*write)(void *ctx, uint64_t off, const uint8_t *buf, uint32_t len);
    /* Truncate the file to `len` bytes. Optional: may be NULL. */
    int32_t (*truncate)(void *ctx, uint64_t len);
} bj_io;

#ifdef __cplusplus
}
#endif

#endif /* BJIO_H */
