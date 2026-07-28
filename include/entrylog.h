/*
 * entrylog.h — persistent, append-only replicated-command log (the Raft log,
 * which is also the database's write-ahead log: a command is durable here
 * before it is applied to any state-machine structure).
 *
 * Design (mirrors textlog.h / textlog.c):
 *   - The log is file-resident: entries and metadata records are read from and
 *     appended to the backing file through the bj_io callbacks (bjio.h)
 *     supplied at create/open. No copy of the file is kept in memory; the only
 *     in-memory state is an index of entry offsets built during open, so
 *     elog_get fetches exactly one record. Entries and metadata use the
 *     binjson wire format, and every commit ends with the fixed-size metadata
 *     record + CRC trailer from bjfile.h; the open-time scan verifies every
 *     protected commit's CRC, recovers a torn tail by truncating back to the
 *     last good commit, and refuses files with verifiable commits beyond a
 *     damaged region (bjfile_scan_commits).
 *   - Each entry is (index, term, type, payload). Indexes are assigned by the
 *     log and strictly contiguous: an append always produces last_index + 1.
 *     Terms must be monotonically non-decreasing (BJ_ERR_STATE otherwise).
 *     Payloads are opaque pre-encoded bytes; the log never interprets them.
 *   - Raft hard state (current_term, voted_for) lives in the same metadata
 *     record as the log bounds. A term bump or vote is therefore a
 *     metadata-only commit on this file — one durability point covers hard
 *     state and any entries appended in the same batch.
 *   - Suffix truncation (a follower discarding entries that conflict with the
 *     leader) is LOGICAL: elog_truncate_from commits a metadata record that
 *     moves last_index back, leaving the file append-only. Later appends write
 *     the replacement entries after the dead bytes; readers use the offset
 *     index, which drops truncated entries. Dead bytes are reclaimed by
 *     elog_compact or a tile roll — never in place.
 *   - Prefix compaction (after a state-machine snapshot) uses the same tiling
 *     pattern as textlog_create_at: a tile owns indexes (base_index, ...] and
 *     also records base_term — the term of entry base_index, kept so the
 *     AppendEntries consistency check works at the snapshot boundary. The
 *     host routes indexes to tiles and decides when to roll; the format
 *     carries only each tile's (base_index, base_term).
 *
 * Durability contract: elog_append only buffers; an entry is durable once
 * elog_sync returns BJ_OK (one host write + flush). Raft requires entries to
 * be durable before they are acknowledged to the leader, and hard state to be
 * durable before answering any RPC — elog_set_hard_state therefore commits
 * immediately rather than waiting for a sync.
 *
 * All calls return BJ_OK (0) or a negative BJ_ERR_* code from binjson.h.
 */
#ifndef ENTRYLOG_H
#define ENTRYLOG_H

#include <stdint.h>
#include <stddef.h>

#include "binjson.h"
#include "bjio.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct elog elog;

/* Entry type bytes. Stored and returned verbatim, never interpreted here —
 * these are conventions for the host's apply loop, chosen now so the format
 * needs no versioning later. Values >= 0x10 are free for host use. */
#define EL_NORMAL 0x01   /* state-machine command (opaque payload)         */
#define EL_NOOP   0x02   /* leader's empty entry committed on election     */
#define EL_CONFIG 0x03   /* cluster membership change                      */

/* voted_for is a host-assigned nonzero node id; 0 means "none this term". */
#define EL_VOTED_NONE 0

/* Create a fresh empty log on `io` (expected empty): indexes start at 1,
 * term 0, no vote. Returns NULL on OOM/bad argument/write failure. */
elog *elog_create(const bj_io *io);
/*
 * Create a fresh log tile that continues after a snapshot or an earlier tile:
 * it owns indexes (base_index, ...], and base_term must be the term of entry
 * base_index (the snapshot's last_included_term). base_index == 0 with
 * base_term == 0 is identical to elog_create. Hard state starts at
 * (current_term = base_term, voted_for = none); restore the real values with
 * elog_set_hard_state before serving RPCs.
 */
elog *elog_create_at(const bj_io *io, uint64_t base_index, uint64_t base_term);
/* Open an existing log from `io`, scanning it once to index entry offsets
 * (entries above a logical truncation are dropped from the index). Returns
 * NULL on OOM or if no valid metadata record is found. */
elog *elog_open(const bj_io *io);
/* Free a log and all its buffers (does not touch the file). Safe on NULL. */
void elog_free(elog *t);

/* ---- Accessors --------------------------------------------------------- */

/* Tile base: the log holds indexes (base_index, last_index]. base_index is 0
 * for an uncompacted log; elog_term_at(base_index) answers base_term so the
 * AppendEntries prev-entry check works at the boundary. */
uint64_t elog_base_index(const elog *t);
uint64_t elog_base_term(const elog *t);
/* Highest index in the log (== base_index when empty). */
uint64_t elog_last_index(const elog *t);
/* Term of the highest entry (== base_term when empty). */
uint64_t elog_last_term(const elog *t);
/* Term of entry `index`. `index` must be in base_index..last_index
 * (BJ_ERR_RANGE otherwise); base_index itself answers base_term. Served from
 * the in-memory offset index — no file read. */
int elog_term_at(const elog *t, uint64_t index, uint64_t *out_term);

/* Raft hard state, as of the last committed metadata record. */
uint64_t elog_current_term(const elog *t);
uint64_t elog_voted_for(const elog *t);
/* Advisory commit index (see elog_set_commit_index); 0 if never recorded. */
uint64_t elog_commit_index(const elog *t);

/* The last read output (elog_get / elog_get_batch); *len set. Valid until
 * the next operation on this log. */
const uint8_t *elog_out(const elog *t, size_t *len);

/* ---- Appending (the WAL write path) ------------------------------------ */

/*
 * Buffer one entry with index last_index + 1 and the given term/type. `term`
 * must be >= the current last_term and <= current_term (BJ_ERR_STATE). The
 * payload is opaque bytes (a pre-encoded binjson command, but any bytes are
 * legal). Writes the assigned index through *out_index. NOT durable until
 * elog_sync — a crash before sync loses buffered entries, which is correct:
 * they were never acknowledged.
 */
int elog_append(elog *t, uint64_t term, int type,
                const uint8_t *payload, uint32_t payload_len,
                uint64_t *out_index);

/*
 * Commit everything buffered since the last sync as one protected commit
 * (entries + metadata + CRC trailer, one host write). The durability point:
 * acknowledge an AppendEntries RPC, or count local persistence toward a
 * quorum, only after this returns BJ_OK. On error the buffered entries are
 * discarded and the file is untouched. A sync with nothing buffered writes a
 * metadata-only commit (used to persist a commit-index update).
 */
int elog_sync(elog *t);

/*
 * Persist Raft hard state. Commits IMMEDIATELY (metadata-only commit, plus
 * any entries already buffered): the caller may answer the RequestVote /
 * AppendEntries RPC as soon as this returns. `term` must be >= the current
 * term; voted_for is EL_VOTED_NONE or a nonzero node id. A new term resets
 * any previous vote; within the current term, changing an existing vote to a
 * different node is refused (BJ_ERR_STATE — one vote per term).
 */
int elog_set_hard_state(elog *t, uint64_t term, uint64_t voted_for);

/*
 * Record the commit index (highest index known replicated to a quorum).
 * Advisory and NOT durability-critical — Raft rederives it after restart —
 * so this only stages the value; it rides along with the next elog_sync.
 * Recording it lets recovery start applying immediately instead of waiting
 * for the next leader heartbeat. Must be in base_index..last_index.
 */
int elog_set_commit_index(elog *t, uint64_t index);

/* ---- Reading (replication fan-out and the apply loop) ------------------ */

/*
 * Read entry `index` (one file read via the offset index). On BJ_OK, *term
 * and *type describe the entry and out_ptr/out_len expose the payload in the
 * log's output buffer (valid until the next op). `index` must be in
 * base_index+1 .. last_index (BJ_ERR_RANGE — the base entry's payload lives
 * in the snapshot, not the log).
 */
int elog_get(elog *t, uint64_t index, uint64_t *term, int *type,
             const uint8_t **out_ptr, size_t *out_len);

/*
 * Pull entries in bulk for an AppendEntries batch or the apply loop: encode
 * entries from `from_index` upward as a binjson ARRAY of { index, term,
 * type, payload } objects until roughly `max_bytes` of payload is gathered
 * (always at least one entry) or the log ends. `from_index` must be above
 * base_index (BJ_ERR_RANGE — compacted entries live in the snapshot; the
 * host answers with InstallSnapshot instead). from_index > last_index is an
 * empty ARRAY with *count = 0. Bytes are exposed via out_ptr/out_len (valid
 * until the next op).
 */
int elog_get_batch(elog *t, uint64_t from_index, size_t max_bytes, int *count,
                   const uint8_t **out_ptr, size_t *out_len);

/* ---- Truncation & compaction ------------------------------------------- */

/*
 * Discard entries from `index` upward (the Raft conflict rule: a follower
 * cuts its log back to the last entry that matches the leader before
 * accepting replacements). `index` must be in base_index+1 .. last_index+1
 * (== last_index+1 is a no-op). Logical and committed immediately: the
 * metadata's last bounds move back; the dead bytes stay in the file until
 * elog_compact / a tile roll. Entries at or below the recorded commit index
 * may not be truncated, and buffered (unsynced) appends must be synced or
 * discarded first (both BJ_ERR_STATE).
 */
int elog_truncate_from(elog *t, uint64_t index);

/*
 * Rewrite the live entries above `new_base_index` (whose term must be
 * `new_base_term` — i.e. the just-taken snapshot's last included entry) into
 * the destination file `dst`, which is expected to be empty, dropping
 * compacted-away entries, logically-truncated dead bytes, and superseded
 * metadata. Hard state carries over; the commit index carries over raised to
 * at least new_base_index (snapshotted entries are committed by definition).
 * Requires no buffered (unsynced) appends. The host adopts `dst` in place of
 * the old file (same pattern as bpt_compact).
 */
int elog_compact(elog *t, const bj_io *dst,
                 uint64_t new_base_index, uint64_t new_base_term);

/* ---- Integrity --------------------------------------------------------- */

/*
 * Verify the log by walking every live record: indexes contiguous from
 * base_index+1, terms monotonically non-decreasing and starting >= base_term,
 * last entry matching the metadata bounds, hard-state term >= last entry's
 * term, and commit index within bounds. Returns BJ_OK, BJ_ERR_VERIFY on a
 * violated invariant, or an I/O or parse error. Reads every entry (O(N)).
 */
int elog_verify(elog *t);

/* Current length of the backing file (committed + pending bytes). Exposed so
 * a host doing physical catch-up (shipping appended bytes to a follower
 * restoring from this node's snapshot) can name commit boundaries. */
uint64_t elog_file_len(const elog *t);

#ifdef __cplusplus
}
#endif

#endif /* ENTRYLOG_H */
