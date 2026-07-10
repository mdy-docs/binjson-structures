/*
 * bjfile.h — record-oriented access to an append-only binjson file through a
 * bj_io callback table (bjio.h).
 *
 * This is the only file-access path the persistent structures use; none of
 * them keeps a resident copy of the file. Two mechanisms keep the host-call
 * overhead low:
 *
 *   - Reads: bjfile_read_record fetches one complete top-level binjson value
 *     with a single speculative host read sized by an adaptive hint (a second
 *     read tops up only when a record outgrows the hint, which then adapts).
 *   - Writes: appends are buffered in a pending-write buffer and flushed to
 *     the host with one write call per public operation (bjfile_commit). A
 *     failed operation drops the buffer (bjfile_discard) and the file is
 *     untouched.
 *
 * Reads of offsets at or past the committed length are served directly out of
 * the pending buffer, so an operation can re-read nodes it appended earlier in
 * the same operation.
 *
 * Durability (see C_DATABASE_REVIEW.md §1.2): the file layer also provides
 * commit protection that stays byte-compatible with the JS implementations,
 * which read metadata at a fixed tail offset and ignore unknown records:
 *
 *   - bjfile_append_header writes a type/format identification record at
 *     offset 0 of new files ({ binjson: "<type>", fmt: 1 }).
 *   - bjfile_append_protected ends each commit with a fixed-size CRC trailer
 *     record placed immediately BEFORE the metadata record (so metadata stays
 *     the last bytes of the file). The CRC covers every byte the commit
 *     appended, metadata included.
 *   - bjfile_check_tail verifies the last commit at open in O(1) reads;
 *     bjfile_scan_commits walks and verifies the whole file for recovery,
 *     distinguishing a torn tail (recover by truncating to the last good
 *     commit) from mid-file corruption (verifiable commits exist beyond the
 *     damage: refuse rather than silently truncate good data).
 *
 * Files written by the JS reference (no header, no trailers) remain fully
 * readable: their commits are simply accepted unverified.
 */
#ifndef BJFILE_H
#define BJFILE_H

#include <stdint.h>
#include <stddef.h>

#include "bjio.h"
#include "binjson.h"

#ifdef __cplusplus
extern "C" {
#endif

/* On-wire size of a commit-protection trailer record:
 * BINARY type byte + u32 length (12) + "BJC1" + u32 crc32 + u32 commit_len. */
#define BJFILE_TRAILER_SIZE 17

typedef struct bjfile {
    bj_io    io;
    uint64_t flen;                       /* committed file length            */
    uint8_t *wb; size_t wb_len, wb_cap;  /* appends pending commit           */
    uint8_t *rd; size_t rd_cap;          /* reusable read buffer             */
    size_t   rd_hint;                    /* speculative read size            */
    size_t   autoflush;                  /* commit when wb_len exceeds this
                                            (0 = manual commit only)         */
    uint32_t crc;                        /* crc32 of bytes appended since the
                                            last protected append            */
    uint64_t crc_len;                    /* byte count behind `crc`          */
    uint32_t crc_committed;              /* crc/crc_len at the last commit,  */
    uint64_t crc_len_committed;          /*   restored by bjfile_discard     */
} bjfile;

/* Bind `f` to `io` and capture the current file size. Returns BJ_OK. */
int bjfile_init(bjfile *f, const bj_io *io);
/* Release buffers (does not touch the file). */
void bjfile_dispose(bjfile *f);

/* Logical end of file: committed bytes plus pending appends. */
uint64_t bjfile_len(const bjfile *f);

/* Buffer an append of `n` bytes; writes its logical offset through *off. */
int bjfile_append(bjfile *f, const uint8_t *b, size_t n, uint64_t *off);
/* Write all pending appends to the host in one call. */
int bjfile_commit(bjfile *f);
/* Drop pending appends without writing (failed-operation path). */
void bjfile_discard(bjfile *f);

/* Cut the file back to `len` bytes (torn-tail recovery on load). Requires an
 * empty pending buffer; truncates via the host when it supports it. */
int bjfile_set_len(bjfile *f, uint64_t len);

/*
 * Read the complete binjson value that starts at `off`. On BJ_OK, *rec /
 * *rec_len describe the record bytes, valid until the next bjfile call.
 */
int bjfile_read_record(bjfile *f, uint64_t off, const uint8_t **rec, size_t *rec_len);

/* ---- Commit protection & recovery ------------------------------------ */

/* Append the file-identification header ({ binjson: type, fmt: 1 }) using
 * the caller's builder. Must be the first append of a new file. */
int bjfile_append_header(bjfile *f, bj_builder *b, const char *type);

/*
 * Check the header record at offset 0. Returns 1 when a valid header for
 * `type` is present, 0 for a legacy file (no header — e.g. written by the JS
 * reference), and BJ_ERR_STATE for a header of a different type or a newer
 * format than this code understands.
 */
int bjfile_check_header(bjfile *f, const char *type);

/*
 * Append `md` (one encoded metadata record) preceded by a CRC trailer that
 * covers every byte appended since the last protected append (or since open),
 * plus `md` itself. Callers end every mutating operation with this.
 */
int bjfile_append_protected(bjfile *f, const uint8_t *md, size_t md_len);

/*
 * Fast tail verification for structures whose metadata record has the fixed
 * size `meta_size` and ends the file. On BJ_OK, *md and *md_len expose the
 * metadata record bytes (valid until the next bjfile call) and, when the
 * commit carries a trailer, its CRC has been verified. Any parse or CRC
 * failure returns a negative code; the caller should fall back to
 * bjfile_scan_commits recovery.
 */
int bjfile_check_tail(bjfile *f, size_t meta_size,
                      const uint8_t **md, size_t *md_len);

/*
 * Record visitor for bjfile_scan_commits. Called for every record except
 * trailers. Sets *is_commit_end = 1 when the record is a metadata record
 * (i.e. ends a commit). A negative return aborts the scan with that error.
 */
typedef int (*bjfile_scan_cb)(void *ctx, uint64_t off, const uint8_t *rec,
                              size_t rec_len, int *is_commit_end);

/*
 * Walk all records from offset 0, verifying every trailer-protected commit's
 * CRC (legacy commits without trailers are accepted unverified). On return,
 * *last_good is the end offset of the last good commit:
 *   - a clean file yields *last_good == bjfile_len(f);
 *   - a torn tail yields *last_good < bjfile_len(f) — the caller truncates
 *     via bjfile_set_len and continues from the last good commit.
 * Returns BJ_ERR_STATE when verifiable commits exist BEYOND a damaged region
 * (mid-file corruption): recovery by truncation would destroy good data, so
 * the file must not be opened. Callback errors are returned as-is.
 */
int bjfile_scan_commits(bjfile *f, bjfile_scan_cb cb, void *ctx,
                        uint64_t *last_good);

#ifdef __cplusplus
}
#endif

#endif /* BJFILE_H */
