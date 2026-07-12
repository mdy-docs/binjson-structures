/*
 * textlog.h — C port of the persistent, append-only versioned text log in
 * src/textlog.js.
 *
 * Design (mirrors rtree.h / rtree.c):
 *   - The log is file-resident: entries (full snapshots or diffs) and metadata
 *     records are read from and appended to the backing file through the bj_io
 *     callbacks (bjio.h) supplied at create/open. No copy of the file is kept
 *     in memory; the only in-memory state is a small index of entry offsets
 *     built during open, so reads fetch exactly the records a version needs.
 *     Entries and metadata use the binjson wire format from binjson.c.
 *   - If the file ends in a torn/partial record (e.g. a crash mid-append),
 *     open recovers to the last valid record and truncates the tail.
 *   - Everything lives in C here: SHA-256, the binary copy/insert delta used to
 *     store DIFF entries (TL_DIFF_BIN, diff.h — a private format; only
 *     round-trip correctness is observable), and the line-based unified diff
 *     produced by getDiff. Legacy DIFF entries written as jsdiff unified-patch
 *     text (TL_DIFF, e.g. by older builds or the removed JS log) are still
 *     applied on read.
 *
 * Version arguments are validated here (outside 1..current -> BJ_ERR_RANGE),
 * so a host that forgets its own checks gets a distinct error instead of
 * silently empty or latest text. Reconstructed text is verified against the
 * entry's stored SHA-256 (BJ_ERR_VERIFY on mismatch), catching diff-chain
 * corruption at read time. The current version's text is cached in memory,
 * so consecutive addVersion calls and reads of the latest version perform no
 * reconstruction reads. All calls return BJ_OK (0) or a negative BJ_ERR_*
 * code from binjson.h.
 */
#ifndef TEXTLOG_H
#define TEXTLOG_H

#include <stdint.h>
#include <stddef.h>

#include "binjson.h"
#include "bjio.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct textlog textlog;

/* Entry type bytes. TL_DIFF (jsdiff unified-patch text) is the legacy delta
 * format, still read from older / JS-written files; new DIFF entries are
 * written as TL_DIFF_BIN (a compact binary copy/insert delta, diff.h). */
#define TL_FULL_SNAPSHOT 0x01
#define TL_DIFF          0x02
#define TL_DIFF_BIN      0x03

/* Create a fresh empty log (diffs_per_snapshot >= 1) on `io` (expected empty)
 * and write the initial metadata record. Returns NULL on OOM/bad argument. */
textlog *textlog_create(const bj_io *io, int diffs_per_snapshot);
/*
 * Create a fresh log tile whose global version numbering continues after an
 * earlier tile: it owns versions (base_version, ...], so the first
 * textlog_add_version produces base_version + 1 (a full snapshot, so the tile
 * reconstructs independently of any other tile). base_version == 0 is
 * identical to textlog_create. The host routes each global version to the tile
 * that owns it (see textlog_base_version) and decides when to roll to a new
 * tile — the log format itself carries only each tile's base. */
textlog *textlog_create_at(const bj_io *io, int diffs_per_snapshot,
                           uint64_t base_version);
/* Open an existing log from `io`, scanning it once to index entry offsets.
 * Returns NULL on OOM or if no valid metadata record is found. */
textlog *textlog_open(const bj_io *io);
/* Free a log and all its buffers (does not touch the file). Safe on NULL. */
void textlog_free(textlog *t);

/* Accessors (mirror the JS metadata fields). */
uint64_t       textlog_version(const textlog *t);
/* Highest global version owned by an earlier tile (0 for a standalone log): a
 * tile serves versions in (base_version, version]. */
uint64_t       textlog_base_version(const textlog *t);
int            textlog_diffs_per_snapshot(const textlog *t);
/* The last read output (getVersion / getVersionHash / getDiff); *len set. */
const uint8_t *textlog_out(const textlog *t, size_t *len);

/*
 * Append a new version. `text`/`text_len` are the full UTF-8 bytes of the
 * document at this version; `ts_ms` is a timestamp in milliseconds. The hash is
 * computed here (SHA-256). Writes the new version number through *out_version.
 */
int textlog_add_version(textlog *t, const uint8_t *text, uint32_t text_len,
                        int64_t ts_ms, uint64_t *out_version);

/*
 * Reconstruct the full text of `version` into the output buffer, exposed via
 * out_ptr/out_len (valid until the next op). `version` must be in 1..current.
 */
int textlog_get_version(textlog *t, uint64_t version,
                        const uint8_t **out_ptr, size_t *out_len);

/*
 * Write the 64-char lowercase hex SHA-256 of `version` into the output buffer.
 * `version` must be in 1..current.
 */
int textlog_get_version_hash(textlog *t, uint64_t version,
                             const uint8_t **out_ptr, size_t *out_len);

/*
 * Produce a human-readable unified diff between two versions into the output
 * buffer. Both versions must be in 1..current.
 */
int textlog_get_diff(textlog *t, uint64_t from_version, uint64_t to_version,
                     const uint8_t **out_ptr, size_t *out_len);

#ifdef __cplusplus
}
#endif

#endif /* TEXTLOG_H */
