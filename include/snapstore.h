/*
 * snapstore.h — the naming, manifest and adoption policy of a crash-safe
 * snapshot store.
 *
 * WHAT THIS IS, AND WHAT IT IS NOT
 *
 * It is NOT the store. The store opens, reads, writes and deletes files,
 * all of which the host does -- asynchronously in a browser, where OPFS
 * has neither rename nor multi-file atomicity, which is the whole reason
 * this design exists. What lives here is every DECISION the store makes:
 * what a generation's files are called, what a manifest says, when a
 * manifest is valid, which generation gets adopted, and what gets swept.
 *
 * That split matters because the decisions were the part that got copied.
 * The manifest's shape and the rule for validating staged files against
 * one appear in the JS store, in the replicated database's install path
 * (src/db-replicated.js) and in the Raft test harness -- three places
 * that must agree byte-for-byte about a checksum or a follower silently
 * adopts corrupt state. Nothing here is new policy; it is the policy the
 * JS store already had, in the one place all three can reach.
 *
 * THE COMMIT PROTOCOL (unchanged, restated because this file enforces it)
 *
 * A snapshot is one immutable *generation*:
 *
 *   <prefix>-<gen>-<role>.bj        one per structure, host-chosen roles
 *   <prefix>-<gen>.manifest.bj      written LAST -- the commit point
 *
 * The manifest is binjson followed by a CRC-32 of exactly those bytes, so
 * a torn manifest cannot validate and a generation without a valid
 * manifest never existed. The paired compacted entry log is
 * <prefix>-log-<gen>.bj and has its own lifecycle (sst_log_candidates).
 *
 * ADOPTION, AND THE TRAMPOLINE
 *
 * Deciding which generation to adopt needs manifest CONTENTS, and then
 * the SIZES of the files that manifest names. Both are the host's to
 * fetch -- asynchronously, in a browser. So open is the plan/open/execute
 * trampoline bjns.h describes, with two beats:
 *
 *   sst_scan(listing)                    pure: which generations exist
 *   for i in 0 .. sst_candidate_count:
 *       bytes = host reads sst_candidate_manifest(i)         <-- await
 *       if sst_try_manifest(i, bytes) != BJ_OK: continue
 *       sizes = host sizes sst_pending_name(0 .. count)      <-- await
 *       if sst_confirm(sizes) == BJ_OK: break
 *   sst_sweep_plan()                     pure: what to delete
 *
 * Two beats rather than one so the second touches only the files of the
 * candidate actually being considered. The alternative -- sizing every
 * name in the listing up front -- would mean opening every database file
 * on every startup to check a snapshot that usually adopts on the first
 * try. The loop itself almost always runs once, because the sweep leaves
 * at most one superseded generation behind.
 *
 * Sizes cross as double. They are compared against manifest values that
 * were themselves written from the host's own numbers, so the comparison
 * is exact wherever the host could have produced the value at all.
 */
#ifndef SNAPSTORE_H
#define SNAPSTORE_H

#include <stdint.h>
#include <stddef.h>

#include "binjson.h"
#include "dbuf.h"

#ifdef __cplusplus
extern "C" {
#endif

/* A manifest that is torn, mis-shaped, or written by a version whose
 * records this one does not recognize. Never fatal: it means "this
 * generation did not commit", which the protocol is built to expect. */
#define SST_ERR_MANIFEST  (-40)
/* A file's bytes do not match what the manifest records for it. Fatal to
 * whatever was relying on them -- an install that reports this has been
 * handed corrupt state and must not adopt it. */
#define SST_ERR_CHECKSUM  (-41)
/* A role name outside [A-Za-z0-9_-]+, or one used twice in a generation.
 * Roles become filenames, so this is a naming rule, not a style rule. */
#define SST_ERR_ROLE      (-42)

typedef struct sst sst;

/* `prefix` is copied. Returns NULL on OOM. */
sst *sst_new(const char *prefix, uint32_t prefix_len);
void sst_free(sst *s);

/* ---- names ------------------------------------------------------------ */

/*
 * Append a generation's file name to `out`. The three shapes are the
 * store's entire naming convention; nothing else may construct them.
 * sst_data_name is SST_ERR_ROLE for an unusable role.
 */
int sst_manifest_name(const sst *s, uint64_t gen, dbuf *out);
int sst_data_name(const sst *s, uint64_t gen, const char *role, uint32_t role_len, dbuf *out);
int sst_log_name(const sst *s, uint64_t gen, dbuf *out);

/* ---- open: scan, adopt, sweep ----------------------------------------- */

/*
 * Classify a directory listing. `listing` is NUL-separated names (the
 * same shape dc_sweep_plan takes, and for the same reason: bj_ns has no
 * list() because OPFS enumeration is asynchronous, so the host passes the
 * listing IN).
 *
 * Pure. Records every generation this store owns, forgets every name it
 * does not, and computes the candidate list -- generations that have a
 * manifest file, newest first. Safe to call again; it resets.
 */
int sst_scan(sst *s, const uint8_t *listing, uint32_t listing_len);

uint32_t sst_candidate_count(const sst *s);
/* Candidate i's manifest file name, i in [0, count). NULL out of range.
 * The pointer belongs to the store and dies with it or with the next
 * sst_scan. */
const char *sst_candidate_manifest(const sst *s, uint32_t i, uint32_t *len);

/*
 * Beat one: validate candidate i's manifest -- the trailing CRC, the
 * record's shape, and that every file it names is present in the listing
 * under the name this store would give that role.
 *
 * SST_ERR_MANIFEST means this generation did not commit; move to the next
 * candidate. A host that reaches the end of the list has no snapshot,
 * which is a normal state, not an error.
 *
 * BJ_OK does NOT adopt: it leaves the manifest pending, with the files
 * whose sizes decide it listed by sst_pending_name.
 */
int sst_try_manifest(sst *s, uint32_t i, const uint8_t *bytes, uint32_t len);

/* The pending manifest's files, in the order sst_confirm expects their
 * sizes. Valid only between sst_try_manifest and sst_confirm. */
uint32_t sst_pending_count(const sst *s);
const char *sst_pending_name(const sst *s, uint32_t i, uint32_t *len);

/*
 * Beat two: `sizes` are the pending files' actual lengths, in
 * sst_pending_name order. Adopts if every one matches what the manifest
 * recorded -- a short file means the write was torn, and the generation
 * did not commit after all (SST_ERR_MANIFEST, try the next candidate).
 */
int sst_confirm(sst *s, const double *sizes, uint32_t n);

/*
 * Every file to delete once adoption is settled: the data and manifest
 * files of every generation except the adopted one -- crashed attempts
 * (data files, no valid manifest) and superseded snapshots alike.
 * NUL-separated names appended to `out`. Log files are NOT included; they
 * have their own lifecycle (sst_prune_logs_plan).
 *
 * Deleting these is best-effort by design: a name that fails to unlink is
 * swept at the next open, and no failure here can lose what was adopted.
 */
int sst_sweep_plan(const sst *s, dbuf *out);

/* The adopted generation's manifest, re-encoded for the host as
 * {gen, lastIncludedIndex, lastIncludedTerm, config, files:[...]}, or
 * nothing appended when none was adopted. *has is 0/1. */
int sst_latest(const sst *s, dbuf *out, int *has);
int sst_has_latest(const sst *s);
uint64_t sst_latest_gen(const sst *s);

/* One past the highest generation seen by the scan -- the number a new
 * begin() takes. Monotonic across a crash because it comes from the
 * names on disk, not from memory. */
uint64_t sst_next_gen(const sst *s);

/* ---- commit ----------------------------------------------------------- */

/*
 * Encode a manifest: the binjson record followed by a little-endian
 * CRC-32 of exactly those record bytes. Writing this file IS the commit.
 *
 * `files` is a binjson ARRAY of {role, name, size, crc} -- the host built
 * each entry as it wrote and checksummed the file. Rejects a role that
 * cannot be a filename or that repeats, here rather than at the next
 * open, when the generation would silently fail to adopt.
 *
 * `config` may be NULL for none.
 */
int sst_manifest_encode(uint64_t last_included_index, uint64_t last_included_term,
                        const uint8_t *config, uint32_t config_len,
                        const uint8_t *files, uint32_t files_len,
                        dbuf *out);

/*
 * Adopt a just-committed generation in memory, so the store answers for
 * it without re-scanning, and report the previous generation's files for
 * the caller to delete (NUL-separated, appended to `sweep`).
 */
int sst_adopt_committed(sst *s, uint64_t gen, const uint8_t *manifest, uint32_t len,
                        dbuf *sweep);

/* ---- validation ------------------------------------------------------- */

/*
 * Check staged or stored files against a manifest: for every entry in
 * `manifest` (the encoded record, without the trailing CRC -- as
 * sst_latest emits, or as a leader sent), the corresponding entry in
 * `actual` (a binjson ARRAY of {role, size, crc}) must match on both.
 *
 * SST_ERR_CHECKSUM naming the offending role through *bad_role /
 * *bad_role_len, which point into `manifest`.
 *
 * This is the rule the JS store's verify(), the replicated install path
 * and the Raft harness each had their own copy of. A follower deciding
 * whether a transferred snapshot is intact is not a place for three
 * opinions.
 */
int sst_check_files(const uint8_t *manifest, uint32_t manifest_len,
                    const uint8_t *actual, uint32_t actual_len,
                    const uint8_t **bad_role, uint32_t *bad_role_len);

/* ---- paired entry logs ------------------------------------------------ */

/*
 * Entry-log file names in `listing` belonging to this store, newest
 * generation first, NUL-separated into `out`. The host tries each with
 * elog_open and adopts the first that succeeds: a crash mid-compaction
 * leaves a torn newest file, and its predecessor is only ever deleted
 * once a successor is durable.
 */
int sst_log_candidates(const sst *s, const uint8_t *listing, uint32_t listing_len,
                       dbuf *out);

/* Every log file in `listing` except `keep` -- to delete once the newly
 * compacted log is durable. */
int sst_prune_logs_plan(const sst *s, const uint8_t *listing, uint32_t listing_len,
                        const char *keep, uint32_t keep_len, dbuf *out);

#ifdef __cplusplus
}
#endif

#endif /* SNAPSTORE_H */
