/*
 * textlog.h — C port of the persistent, append-only versioned text log in
 * src/textlog.js.
 *
 * Design (mirrors rtree.h / rtree.c):
 *   - A log owns an in-memory byte "image" mirroring the append-only file:
 *     entries (full snapshots or diffs) and metadata records are appended
 *     verbatim, and the host (JS) writes the image back to storage on
 *     flush/close. Entries and metadata use the binjson wire format from
 *     binjson.c.
 *   - Unlike src/textlog.js — which delegates diffing to the `diff` npm package
 *     and hashing to node's crypto — everything lives in C here: SHA-256, the
 *     internal (opaque) prefix/suffix diff used to store DIFF entries, and the
 *     line-based unified diff produced by getDiff. The internal diff format is
 *     private to this implementation (only round-trip correctness is observable,
 *     so files need not interoperate with the JS log).
 *
 * The host validates version ranges and reports the human-readable errors; the
 * C API assumes valid arguments and returns BJ_OK (0) or a negative BJ_ERR_*
 * code from binjson.h.
 */
#ifndef TEXTLOG_H
#define TEXTLOG_H

#include <stdint.h>
#include <stddef.h>

#include "binjson.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct textlog textlog;

/* Entry type bytes (mirror ENTRY_TYPE in textlog.js). */
#define TL_FULL_SNAPSHOT 0x01
#define TL_DIFF          0x02

/* Create a fresh empty log (diffs_per_snapshot >= 1). Writes the initial
 * metadata record. Returns NULL on OOM or bad argument. */
textlog *textlog_create(int diffs_per_snapshot);
/* Load an existing log from a file image (copies `len` bytes). Returns NULL on
 * OOM or if no valid metadata record is found. */
textlog *textlog_load(const uint8_t *bytes, size_t len);
/* Free a log and all its buffers. Safe to pass NULL. */
void textlog_free(textlog *t);

/* Accessors (mirror the JS metadata fields). */
double         textlog_version(const textlog *t);
int            textlog_diffs_per_snapshot(const textlog *t);
/* The full file image; writes its length through *len. */
const uint8_t *textlog_image(const textlog *t, size_t *len);
/* The last read output (getVersion / getVersionHash / getDiff); *len set. */
const uint8_t *textlog_out(const textlog *t, size_t *len);

/*
 * Append a new version. `text`/`text_len` are the full UTF-8 bytes of the
 * document at this version; `ts_ms` is a timestamp in milliseconds. The hash is
 * computed here (SHA-256). Writes the new version number through *out_version.
 */
int textlog_add_version(textlog *t, const uint8_t *text, uint32_t text_len,
                        int64_t ts_ms, double *out_version);

/*
 * Reconstruct the full text of `version` into the output buffer, exposed via
 * out_ptr/out_len (valid until the next op). `version` must be in 1..current.
 */
int textlog_get_version(textlog *t, double version,
                        const uint8_t **out_ptr, size_t *out_len);

/*
 * Write the 64-char lowercase hex SHA-256 of `version` into the output buffer.
 * `version` must be in 1..current.
 */
int textlog_get_version_hash(textlog *t, double version,
                             const uint8_t **out_ptr, size_t *out_len);

/*
 * Produce a human-readable unified diff between two versions into the output
 * buffer. Both versions must be in 1..current.
 */
int textlog_get_diff(textlog *t, double from_version, double to_version,
                     const uint8_t **out_ptr, size_t *out_len);

#ifdef __cplusplus
}
#endif

#endif /* TEXTLOG_H */
