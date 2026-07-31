/*
 * bjio_posix.c — bj_io and bj_ns over POSIX file descriptors.
 *
 * This is what a native or WASI build links INSTEAD of hostio.c: same
 * interfaces, no JavaScript, no EM_JS bridge. One source file serves both
 * targets, because wasi-libc implements openat/unlinkat/pread/pwrite/
 * fsync -- so the server story is not a second implementation, it is a
 * second adapter behind the same seam.
 *
 * Not compiled into the emscripten build (see the consumer's
 * build-common.sh exclusion list), which keeps hostio.c's OPFS bridge as
 * the browser's adapter.
 */
/* Ask for POSIX.1-2008 before any header is read. Consumers compile this
 * with -std=c11 rather than -std=gnu11, which defines __STRICT_ANSI__,
 * and glibc answers that by hiding everything this file is made of:
 * pread, pwrite, ftruncate, fdatasync, openat and unlinkat all become
 * implicit declarations and the build dies on -Werror. Darwin and
 * wasi-libc declare them regardless, which is why a macOS developer and
 * the WASI target never saw it and a Linux CI runner never saw anything
 * else. 200809L is the level that covers all six. */
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif

#include "bjio_posix.h"

#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* ---- bj_io over one fd -------------------------------------------------- */

/* The fd is boxed rather than stuffed into the pointer: a boxed handle
 * gives close() somewhere to record that it ran, and keeps the
 * (void*)(intptr_t)fd trick -- which is fine but unextendable -- out of a
 * file that will grow. */
typedef struct { int fd; } pfile;

static uint64_t pf_size(void *ctx) {
    struct stat st;
    if (fstat(((pfile *)ctx)->fd, &st) != 0) return 0;
    return (uint64_t)st.st_size;
}

static int64_t pf_read(void *ctx, uint64_t off, uint8_t *buf, uint32_t len) {
    size_t got = 0;
    while (got < len) {
        ssize_t n = pread(((pfile *)ctx)->fd, buf + got, len - got, (off_t)(off + got));
        if (n < 0) {
            if (errno == EINTR) continue;
            return BJ_ERR_STATE;
        }
        if (n == 0) break;             /* end of file: a short read is legal */
        got += (size_t)n;
    }
    return (int64_t)got;
}

static int32_t pf_write(void *ctx, uint64_t off, const uint8_t *buf, uint32_t len) {
    size_t put = 0;
    while (put < len) {
        ssize_t n = pwrite(((pfile *)ctx)->fd, buf + put, len - put, (off_t)(off + put));
        if (n < 0) {
            if (errno == EINTR) continue;
            return BJ_ERR_STATE;
        }
        if (n == 0) return BJ_ERR_EOF;  /* no progress: treat as a short write */
        put += (size_t)n;
    }
    return BJ_OK;
}

static int32_t pf_truncate(void *ctx, uint64_t len) {
    if (ftruncate(((pfile *)ctx)->fd, (off_t)len) != 0) return BJ_ERR_STATE;
    return BJ_OK;
}

static int32_t pf_sync(void *ctx) {
    /* fdatasync where available: the file's SIZE is metadata the structures
     * depend on, but pwrite already updated it, and fdatasync is specified
     * to flush metadata needed to read the data back. Fall back to fsync
     * where fdatasync is absent (macOS). */
#if defined(__APPLE__)
    if (fsync(((pfile *)ctx)->fd) != 0) return BJ_ERR_STATE;
#else
    if (fdatasync(((pfile *)ctx)->fd) != 0) return BJ_ERR_STATE;
#endif
    return BJ_OK;
}

static int32_t pf_close(void *ctx) {
    pfile *f = (pfile *)ctx;
    int rc = BJ_OK;
    if (f->fd >= 0 && close(f->fd) != 0) rc = BJ_ERR_STATE;
    f->fd = -1;
    free(f);
    return rc;
}

int bjio_posix_open_fd(int fd, bj_io *out) {
    pfile *f = (pfile *)calloc(1, sizeof(pfile));
    if (!f) return BJ_ERR_OOM;
    f->fd = fd;
    bj_io io = {
        .ctx      = f,
        .size     = pf_size,
        .read     = pf_read,
        .write    = pf_write,
        .truncate = pf_truncate,
        .sync     = pf_sync,
        .close    = pf_close,
    };
    *out = io;
    return BJ_OK;
}

/* ---- bj_ns over a directory fd ------------------------------------------ */

typedef struct { int dirfd; } pdir;

/* Names are length-counted; openat wants a C string. PATH_MAX is not
 * portable enough to rely on, and these names are short by construction
 * (db_names.h), so bound them explicitly. */
#define PNAME_MAX 512

static int name_to_cstr(const char *name, uint32_t len, char *out) {
    if (len == 0 || len >= PNAME_MAX) return BJ_ERR_RANGE;
    /* A name containing '/' would escape the scope, and a NUL would
     * truncate it -- both are refused by db_validate.h upstream, but this
     * adapter is the last line before a real syscall. */
    for (uint32_t i = 0; i < len; i++) {
        if (name[i] == '/' || name[i] == '\0') return BJ_ERR_RANGE;
    }
    memcpy(out, name, len);
    out[len] = '\0';
    return BJ_OK;
}

static int32_t pd_open(void *ctx, const char *name, uint32_t name_len,
                       uint32_t flags, bj_io *out) {
    char path[PNAME_MAX];
    int e = name_to_cstr(name, name_len, path);
    if (e) return (int32_t)e;

    int oflags = O_RDWR;
    if (flags & BJ_NS_CREATE) oflags |= O_CREAT;
    if (flags & BJ_NS_EXCL)   oflags |= O_EXCL;
    if (flags & BJ_NS_TRUNC)  oflags |= O_TRUNC;

    int fd;
    do { fd = openat(((pdir *)ctx)->dirfd, path, oflags, 0644); }
    while (fd < 0 && errno == EINTR);
    if (fd < 0) return BJ_ERR_STATE;

    e = bjio_posix_open_fd(fd, out);
    if (e) { close(fd); return (int32_t)e; }
    return BJ_OK;
}

static int32_t pd_close(void *ctx, bj_io *io) {
    (void)ctx;
    if (!io || !io->close) return BJ_OK;
    int32_t rc = io->close(io->ctx);
    memset(io, 0, sizeof(*io));
    return rc;
}

static int32_t pd_remove(void *ctx, const char *name, uint32_t name_len) {
    char path[PNAME_MAX];
    int e = name_to_cstr(name, name_len, path);
    if (e) return (int32_t)e;
    if (unlinkat(((pdir *)ctx)->dirfd, path, 0) != 0) {
        /* Already gone is not an error -- it matches the providers'
         * swallow-NotFoundError behavior, and a sweep that races another
         * sweep must not fail. */
        if (errno == ENOENT) return BJ_OK;
        return BJ_ERR_STATE;
    }
    return BJ_OK;
}

static int32_t pd_sync(void *ctx) {
    if (fsync(((pdir *)ctx)->dirfd) != 0) {
        /* Some filesystems refuse to fsync a directory; best-effort, the
         * same stance src/db-node.js's _fsyncDir takes. */
        if (errno == EINVAL || errno == ENOTSUP) return BJ_OK;
#ifdef __wasi__
        /* And under WASI the refusal is the host's, not the filesystem's.
         * Preview1 is rights-based: fd_sync needs RIGHT_FD_SYNC, which a
         * preopened DIRECTORY is not granted, so the call cannot even
         * reach the filesystem. wasmtime answers BADF; another host may
         * answer NOTCAPABLE, preview1's own spelling for a missing right.
         * Node's host forwards fsync to the real descriptor and succeeds,
         * which is why this went unnoticed -- it is the only WASI host
         * this library was ever run under.
         *
         * Tolerated HERE and only here. On a real POSIX system EBADF from
         * fsync means a closed or invalid descriptor, and answering BJ_OK
         * to that would report a durability step as done when the fd was
         * never valid to begin with -- a bug hidden, not handled. Under
         * WASI it means the capability does not exist, which is exactly
         * what the EINVAL/ENOTSUP arm above already exists to survive. */
        if (errno == EBADF) return BJ_OK;
#ifdef ENOTCAPABLE
        if (errno == ENOTCAPABLE) return BJ_OK;
#endif
#endif
        return BJ_ERR_STATE;
    }
    return BJ_OK;
}

int bjns_posix_open(int dirfd, bj_ns *out) {
    pdir *d = (pdir *)calloc(1, sizeof(pdir));
    if (!d) return BJ_ERR_OOM;
    d->dirfd = dirfd;
    bj_ns ns = {
        .ctx    = d,
        .open   = pd_open,
        .close  = pd_close,
        .remove = pd_remove,
        .sync   = pd_sync,
    };
    *out = ns;
    return BJ_OK;
}

void bjns_posix_free(bj_ns *ns) {
    if (!ns || !ns->ctx) return;
    free(ns->ctx);
    memset(ns, 0, sizeof(*ns));
}
