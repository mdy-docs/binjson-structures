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

#include "binjson.h"   /* BJ_OK / BJ_ERR_STATE for bjio_check */

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

    /*
     * Make previously written bytes durable against process AND machine
     * failure -- a real fsync, not a buffer flush.
     *
     * NULL means "writes are already durable when write() returns". That is
     * true for a memory-backed io and is NEVER true for a real file, so a
     * file-backed adapter that leaves this NULL is silently not durable.
     * bjio_check exists to catch exactly that; build with
     * -DBJIO_REQUIRE_SYNC (the native and WASI builds do) to enforce it.
     *
     * Deliberately NOT called by bjfile_commit: every append commits, and
     * fsyncing every B+ tree insert would be a different database. It is
     * called at the declared durability points -- bjfile_sync, and through
     * it elog_sync, which docs/replicaton-roadmap.md calls out as the one
     * Raft safety depends on ("the server storage provider's flush path
     * must be a real fsync -- Raft safety depends on sync-before-ack
     * reaching disk").
     */
    int32_t (*sync)(void *ctx);

    /* Release the underlying handle. Optional: NULL means the namespace or
     * host owns the lifetime, which is the case for every io a bj_ns
     * hands out. */
    int32_t (*close)(void *ctx);
} bj_io;

/*
 * Reject an io that cannot honor the durability contract: writable but with
 * no sync. Called at open time by the structures, so a misconfigured
 * adapter fails loudly on the first open rather than silently losing a
 * commit on the first power cut.
 *
 * Without BJIO_REQUIRE_SYNC this is a no-op, because a memory-backed io
 * legitimately has no sync -- the fuzz harness and the native test
 * harness both rely on that.
 */
static inline int bjio_check(const bj_io *io) {
#ifdef BJIO_REQUIRE_SYNC
    if (io && io->write && !io->sync) return BJ_ERR_STATE;
#else
    (void)io;
#endif
    return BJ_OK;
}

#ifdef __cplusplus
}
#endif

#endif /* BJIO_H */
