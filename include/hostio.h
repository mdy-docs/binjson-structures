/*
 * hostio.h — bj_io (bjio.h) backed by JS FileSystemSyncAccessHandle objects.
 *
 * The JS wrapper registers each open sync access handle in the module-level
 * table `Module.bjioHandles` under an integer slot ("fd") and passes that fd
 * into the *_wasm.c glue. Every callback is a single synchronous JS call that
 * reads into / writes out of WASM memory directly via HEAPU8.subarray — no
 * intermediate copies on either side of the bridge.
 */
#ifndef HOSTIO_H
#define HOSTIO_H

#include "bjio.h"

#ifdef __cplusplus
extern "C" {
#endif

/* A bj_io whose callbacks operate on Module.bjioHandles[fd]. */
bj_io bjio_host(int fd);

#ifdef __cplusplus
}
#endif

#endif /* HOSTIO_H */
