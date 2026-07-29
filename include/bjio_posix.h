/*
 * bjio_posix.h — the native / WASI adapter for bjio.h and bjns.h.
 *
 * Linked instead of hostio.c by any build that is not emscripten. One
 * implementation serves both plain POSIX and wasm32-wasi, because
 * wasi-libc provides openat/unlinkat/pread/pwrite/fsync -- the server
 * target is a second adapter, not a second implementation.
 *
 * The host owns the descriptors it hands in: a directory fd for the
 * namespace (from open(dir, O_RDONLY) natively, or a WASI preopen), and
 * the process's own lifetime for the namespace itself. Files opened
 * through the namespace ARE owned by their bj_io and released by its
 * close().
 */
#ifndef BJIO_POSIX_H
#define BJIO_POSIX_H

#include "bjio.h"
#include "bjns.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Wrap an already-open file descriptor as a bj_io. Takes ownership: the
 * returned io's close() closes `fd`. Returns BJ_OK or BJ_ERR_OOM.
 *
 * Exposed separately from the namespace so a host can hand in a
 * descriptor it opened itself -- a WASI preopen, an inherited fd, or a
 * path resolved by policy the database has no business knowing about.
 */
int bjio_posix_open_fd(int fd, bj_io *out);

/*
 * A namespace scoped to `dirfd`. Does NOT take ownership of `dirfd`; the
 * caller closes it after bjns_posix_free.
 */
int bjns_posix_open(int dirfd, bj_ns *out);
void bjns_posix_free(bj_ns *ns);

#ifdef __cplusplus
}
#endif

#endif /* BJIO_POSIX_H */
