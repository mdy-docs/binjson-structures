/*
 * bjns.h — one directory-scoped file namespace.
 *
 * bjio.h abstracts reading and writing an ALREADY-OPEN file. This
 * abstracts naming one: opening, unlinking, and making the directory
 * entry durable. It is the seam that lets the catalog, the compaction
 * generation flip and the orphan sweep live in C, because a layer that
 * cannot name a file cannot own a catalog.
 *
 * Four verbs, deliberately not a VFS. Every verb is one more thing four
 * adapters must implement, and every adapter is a place for them to
 * disagree.
 *
 * The synchronous-open problem
 * ----------------------------
 * In a browser, opening a file is asynchronous: OPFS getFileHandle() and
 * createSyncAccessHandle() both return promises, and so do removeEntry()
 * and directory enumeration. WASM cannot block on a promise without
 * Asyncify or JSPI, neither of which exists in a native or WASI build --
 * so relying on either would mean two different control-flow models for
 * the browser and the server, which is the exact problem this whole
 * effort exists to remove.
 *
 * The resolution is a discipline rather than a mechanism: C PLANS, the
 * host OPENS, C EXECUTES. Every file-touching operation splits into a
 * pure call that returns the names it will need, and a synchronous call
 * that does the work over the handles the host opened in between. So
 * `open` below is required to be synchronous, and callers may only ask
 * for names a preceding plan call declared.
 *
 * Under WASI and native POSIX that discipline costs nothing: openat is
 * already synchronous, so the adapter opens on demand and the plan phase
 * is pure bookkeeping. In the browser the adapter serves `open` from a
 * table the plan phase caused the host to populate, and returns
 * BJ_ERR_STATE for a name nobody declared -- which the discipline
 * guarantees never happens, and which the native build asserts on.
 *
 * What is deliberately absent
 * ---------------------------
 *   - list(). Directory enumeration is async in OPFS, and a callback form
 *     would need JS function pointers in the WASM table, which
 *     -sALLOW_TABLE_GROWTH=0 forbids on purpose. Listings are passed IN as
 *     a NUL-separated buffer to the operations that need one.
 *   - sub(). getDirectoryHandle is async; scoping is the host's job, and
 *     Client.db(name) simply hands C a different bj_ns.
 *   - rename, stat, mkdir, paths. Not needed. Resist.
 */
#ifndef BJNS_H
#define BJNS_H

#include <stdint.h>
#include <stddef.h>

#include "binjson.h"
#include "bjio.h"

#ifdef __cplusplus
extern "C" {
#endif

#define BJ_NS_CREATE 0x1u   /* create if missing                    */
#define BJ_NS_EXCL   0x2u   /* fail if already present              */
#define BJ_NS_TRUNC  0x4u   /* truncate to zero length on open      */

typedef struct bj_ns {
    void *ctx;

    /*
     * Open `name` within this scope and fill *out. MUST be synchronous --
     * see the header comment. Names are length-counted, not NUL-terminated:
     * they come from user data by way of the naming scheme.
     */
    int32_t (*open)(void *ctx, const char *name, uint32_t name_len,
                    uint32_t flags, bj_io *out);

    /* Release an io obtained from open(). */
    int32_t (*close)(void *ctx, bj_io *io);

    /*
     * Unlink `name`. MAY BE DEFERRED: a host that cannot unlink
     * synchronously queues the name and drains the queue after the current
     * C call returns.
     *
     * That is safe because every deletion in this codebase is a space
     * optimization performed AFTER an atomic catalog commit -- an
     * undeleted file is an orphan the next sweep collects, never a
     * correctness problem. What is NOT safe is ordering a create against a
     * remove, so no caller may do that; use BJ_NS_TRUNC instead.
     */
    int32_t (*remove)(void *ctx, const char *name, uint32_t name_len);

    /*
     * Make the DIRECTORY ENTRY durable -- on POSIX, fsync the directory fd
     * after a create or unlink. Optional (NULL when the host has no such
     * notion). Without it, a crash can lose a freshly created file's
     * directory entry even though the file's own bytes were fsynced;
     * src/db-node.js already does this and documents why.
     *
     * BEST-EFFORT, and BJ_OK does not mean a sync happened: a platform
     * that refuses to sync a directory (some filesystems answer EINVAL;
     * a WASI host need not grant fd_sync on a preopened directory at all)
     * reports success, because an adapter cannot make a promise its host
     * will not keep and every caller would have to ignore the refusal
     * anyway. An error means a sync that COULD have happened did not.
     */
    int32_t (*sync)(void *ctx);
} bj_ns;

#ifdef __cplusplus
}
#endif

#endif /* BJNS_H */
