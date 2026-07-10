/*
 * bjfile.c — see bjfile.h.
 */
#include "bjfile.h"

#include <stdlib.h>
#include <string.h>

/* Initial speculative read size; grows to fit the largest record seen. */
#define BJFILE_RD_HINT0 512

/* Tail commits larger than this are accepted structurally at open without
 * CRC verification (e.g. the single whole-file commit a compaction writes);
 * the recovery scan still verifies them when suspicion arises. */
#define BJFILE_VERIFY_CAP (8u << 20)

/* ---- CRC32 (IEEE 802.3, zlib-style incremental) ---------------------- */

static uint32_t crc_table[256];
static int crc_ready = 0;

static void crc_init(void) {
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t c = i;
        for (int k = 0; k < 8; k++) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
        crc_table[i] = c;
    }
    crc_ready = 1;
}

static uint32_t crc32_update(uint32_t crc, const uint8_t *p, size_t n) {
    if (!crc_ready) crc_init();
    crc = ~crc;
    while (n--) crc = crc_table[(crc ^ *p++) & 0xffu] ^ (crc >> 8);
    return ~crc;
}

/* ---- Little-endian scalar helpers ------------------------------------ */

static uint32_t rd32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static void wr32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}

int bjfile_init(bjfile *f, const bj_io *io) {
    memset(f, 0, sizeof(*f));
    f->io = *io;
    f->flen = io->size(io->ctx);
    f->rd_hint = BJFILE_RD_HINT0;
    return BJ_OK;
}

void bjfile_dispose(bjfile *f) {
    free(f->wb);
    free(f->rd);
    memset(f, 0, sizeof(*f));
}

uint64_t bjfile_len(const bjfile *f) { return f->flen + f->wb_len; }

static int grow(uint8_t **buf, size_t *cap, size_t need) {
    if (need <= *cap) return BJ_OK;
    size_t nc = *cap ? *cap : 256;
    while (nc < need) {
        if (nc > ((size_t)-1) / 2) return BJ_ERR_OOM;   /* overflow guard */
        nc *= 2;
    }
    uint8_t *nb = (uint8_t *)realloc(*buf, nc);
    if (!nb) return BJ_ERR_OOM;
    *buf = nb; *cap = nc;
    return BJ_OK;
}

/* Append `n` bytes; `account_crc` controls whether they enter the running
 * commit CRC (trailer bytes must not — the trailer excludes itself). */
static int append_bytes(bjfile *f, const uint8_t *b, size_t n, uint64_t *off,
                        int account_crc) {
    if (n > ((size_t)-1) - f->wb_len) return BJ_ERR_OOM;
    int e = grow(&f->wb, &f->wb_cap, f->wb_len + n);
    if (e) return e;
    if (off) *off = f->flen + f->wb_len;
    memcpy(f->wb + f->wb_len, b, n);
    f->wb_len += n;
    if (account_crc) {
        f->crc = crc32_update(f->crc, b, n);
        f->crc_len += n;
    }
    if (f->autoflush && f->wb_len >= f->autoflush) return bjfile_commit(f);
    return BJ_OK;
}

int bjfile_append(bjfile *f, const uint8_t *b, size_t n, uint64_t *off) {
    return append_bytes(f, b, n, off, 1);
}

int bjfile_commit(bjfile *f) {
    if (f->wb_len == 0) {
        f->crc_committed = f->crc;
        f->crc_len_committed = f->crc_len;
        return BJ_OK;
    }
    if (f->wb_len > UINT32_MAX) return BJ_ERR_OOM;
    int32_t e = f->io.write(f->io.ctx, f->flen, f->wb, (uint32_t)f->wb_len);
    if (e) return (int)e;
    f->flen += f->wb_len;
    f->wb_len = 0;
    f->crc_committed = f->crc;
    f->crc_len_committed = f->crc_len;
    return BJ_OK;
}

void bjfile_discard(bjfile *f) {
    f->wb_len = 0;
    f->crc = f->crc_committed;
    f->crc_len = f->crc_len_committed;
}

int bjfile_set_len(bjfile *f, uint64_t len) {
    if (f->wb_len || len > f->flen) return BJ_ERR_STATE;
    f->flen = len;
    if (f->io.truncate) return (int)f->io.truncate(f->io.ctx, len);
    return BJ_OK;
}

int bjfile_read_record(bjfile *f, uint64_t off, const uint8_t **rec, size_t *rec_len) {
    uint64_t total = f->flen + f->wb_len;
    if (off >= total) return BJ_ERR_EOF;

    /* Pending append: serve in place. Records are appended (and committed)
     * whole, so a record never straddles the committed/pending boundary. */
    if (off >= f->flen) {
        size_t rel = (size_t)(off - f->flen);
        size_t sz;
        int e = bj_value_size(f->wb, f->wb_len, rel, &sz);
        if (e) return e;
        if (sz > f->wb_len - rel) return BJ_ERR_EOF;
        *rec = f->wb + rel;
        *rec_len = sz;
        return BJ_OK;
    }

    uint64_t avail64 = f->flen - off;
    size_t avail = avail64 > (size_t)-1 ? (size_t)-1 : (size_t)avail64;
    size_t want = f->rd_hint < avail ? f->rd_hint : avail;
    int e = grow(&f->rd, &f->rd_cap, want);
    if (e) return e;
    int64_t got = f->io.read(f->io.ctx, off, f->rd, (uint32_t)want);
    if (got < 0) return (int)got;

    /* Size the record from its header (type byte + optional u32 size). */
    size_t sz;
    e = bj_value_size(f->rd, (size_t)got, 0, &sz);
    if (e) return e;
    if (sz > avail) return BJ_ERR_EOF;   /* record extends past EOF: corrupt */

    if (sz > (size_t)got) {
        /* Record outgrew the speculative read: fetch the remainder and adapt
         * the hint so the next read is a single host call again. */
        e = grow(&f->rd, &f->rd_cap, sz);
        if (e) return e;
        int64_t more = f->io.read(f->io.ctx, off + (uint64_t)got,
                                  f->rd + got, (uint32_t)(sz - (size_t)got));
        if (more < 0) return (int)more;
        if ((size_t)got + (size_t)more < sz) return BJ_ERR_EOF;
        if (sz > f->rd_hint) f->rd_hint = sz;
    }

    *rec = f->rd;
    *rec_len = sz;
    return BJ_OK;
}

/* ---- Commit protection & recovery ------------------------------------ */

/* Read exactly [off, off+n) into the read buffer (committed or pending). */
static int read_range(bjfile *f, uint64_t off, size_t n, const uint8_t **p) {
    uint64_t total = f->flen + f->wb_len;
    if (off > total || n > total - off) return BJ_ERR_EOF;
    if (off >= f->flen) {
        *p = f->wb + (size_t)(off - f->flen);
        return BJ_OK;
    }
    if (off + n > f->flen) return BJ_ERR_EOF;   /* straddling never happens */
    int e = grow(&f->rd, &f->rd_cap, n);
    if (e) return e;
    int64_t got = f->io.read(f->io.ctx, off, f->rd, (uint32_t)n);
    if (got < 0) return (int)got;
    if ((size_t)got < n) return BJ_ERR_EOF;
    *p = f->rd;
    return BJ_OK;
}

static const uint8_t TRAILER_MAGIC[4] = { 'B', 'J', 'C', '1' };

/* Strict trailer recognizer: BINARY record, 12-byte payload, magic. */
static int is_trailer(const uint8_t *rec, size_t len, uint32_t *crc, uint32_t *clen) {
    if (len != BJFILE_TRAILER_SIZE) return 0;
    if (rec[0] != BJ_TYPE_BINARY) return 0;
    if (rd32(rec + 1) != 12) return 0;
    if (memcmp(rec + 5, TRAILER_MAGIC, 4) != 0) return 0;
    if (crc) *crc = rd32(rec + 9);
    if (clen) *clen = rd32(rec + 13);
    return 1;
}

int bjfile_append_protected(bjfile *f, const uint8_t *md, size_t md_len) {
    uint64_t commit_len = f->crc_len + md_len + BJFILE_TRAILER_SIZE;
    int e;
    if (commit_len > UINT32_MAX) {
        /* Trailer fields are u32; oversized commits fall back to an
         * unverified (legacy-shaped) commit rather than failing. */
        e = append_bytes(f, md, md_len, NULL, 0);
    } else {
        uint32_t crc = crc32_update(f->crc, md, md_len);
        uint8_t tr[BJFILE_TRAILER_SIZE];
        tr[0] = BJ_TYPE_BINARY;
        wr32(tr + 1, 12);
        memcpy(tr + 5, TRAILER_MAGIC, 4);
        wr32(tr + 9, crc);
        wr32(tr + 13, (uint32_t)commit_len);
        e = append_bytes(f, tr, sizeof tr, NULL, 0);
        if (!e) e = append_bytes(f, md, md_len, NULL, 0);
    }
    if (e) return e;
    f->crc = 0;
    f->crc_len = 0;
    return BJ_OK;
}

int bjfile_append_header(bjfile *f, bj_builder *b, const char *type) {
    bj_builder_reset(b);
    bj_begin_object(b);
    bj_put_key(b, (const uint8_t *)"binjson", 7);
    bj_put_string(b, (const uint8_t *)type, (uint32_t)strlen(type));
    bj_put_key(b, (const uint8_t *)"fmt", 3);
    bj_put_int(b, 1);
    bj_end_object(b);
    int e = bj_builder_error(b);
    if (e) return e;
    size_t len;
    const uint8_t *d = bj_builder_data(b, &len);
    if (!d) return BJ_ERR_STATE;
    return bjfile_append(f, d, len, NULL);
}

int bjfile_check_header(bjfile *f, const char *type) {
    if (f->flen + f->wb_len == 0) return 0;
    const uint8_t *rec; size_t len;
    if (bjfile_read_record(f, 0, &rec, &len)) return 0;  /* recovery decides */
    /* OBJECT: type byte + u32 size + u32 count, then key/value pairs. */
    if (len < 9 + 4 + 7 || rec[0] != BJ_TYPE_OBJECT) return 0;
    size_t pos = 9;
    uint32_t klen = rd32(rec + pos); pos += 4;
    if (klen != 7 || pos + 7 > len) return 0;
    if (memcmp(rec + pos, "binjson", 7) != 0) return 0;  /* legacy first record */
    pos += 7;
    /* Value must be a STRING equal to `type`. */
    if (pos + 5 > len || rec[pos] != BJ_TYPE_STRING) return BJ_ERR_STATE;
    uint32_t slen = rd32(rec + pos + 1); pos += 5;
    size_t tlen = strlen(type);
    if (pos + slen > len) return BJ_ERR_STATE;
    if (slen != tlen || memcmp(rec + pos, type, tlen) != 0) return BJ_ERR_STATE;
    pos += slen;
    /* Optional "fmt": refuse formats newer than this code understands. */
    if (pos + 4 <= len) {
        uint32_t k2 = rd32(rec + pos); pos += 4;
        if (k2 == 3 && pos + 3 + 9 <= len && memcmp(rec + pos, "fmt", 3) == 0) {
            pos += 3;
            if (rec[pos] == BJ_TYPE_INT) {
                int64_t fmt = 0;
                for (int i = 7; i >= 0; i--) fmt = (fmt << 8) | rec[pos + 1 + i];
                if (fmt > 1) return BJ_ERR_STATE;
            }
        }
    }
    return 1;
}

/* CRC the committed range [from, to) in chunks through `buf` (cap bytes). */
static int crc_over_range(bjfile *f, uint64_t from, uint64_t to,
                          uint8_t *buf, size_t cap, uint32_t *crc) {
    while (from < to) {
        size_t n = (to - from) < (uint64_t)cap ? (size_t)(to - from) : cap;
        int64_t got = f->io.read(f->io.ctx, from, buf, (uint32_t)n);
        if (got < 0) return (int)got;
        if ((size_t)got < n) return BJ_ERR_EOF;
        *crc = crc32_update(*crc, buf, n);
        from += n;
    }
    return BJ_OK;
}

int bjfile_check_tail(bjfile *f, size_t meta_size,
                      const uint8_t **md, size_t *md_len) {
    uint64_t flen = f->flen + f->wb_len;
    if (flen < meta_size) return BJ_ERR_EOF;
    uint64_t md_off = flen - meta_size;

    /* Verify the commit CRC when a trailer precedes the metadata. */
    if (flen >= meta_size + BJFILE_TRAILER_SIZE) {
        uint64_t tr_off = md_off - BJFILE_TRAILER_SIZE;
        const uint8_t *tr;
        int e = read_range(f, tr_off, BJFILE_TRAILER_SIZE, &tr);
        if (e) return e;
        uint32_t want_crc, clen;
        if (is_trailer(tr, BJFILE_TRAILER_SIZE, &want_crc, &clen)) {
            if ((uint64_t)clen > flen || clen < meta_size + BJFILE_TRAILER_SIZE)
                return BJ_ERR_STATE;
            if (clen <= BJFILE_VERIFY_CAP) {
                /* CRC covers the commit minus the trailer's own bytes. */
                uint64_t start = flen - clen;
                uint32_t crc = 0;
                uint8_t chunk[4096];
                e = crc_over_range(f, start, tr_off, chunk, sizeof chunk, &crc);
                if (!e) e = crc_over_range(f, md_off, flen, chunk, sizeof chunk, &crc);
                if (e) return e;
                if (crc != want_crc) return BJ_ERR_STATE;
            }
            /* Oversized commits (e.g. a compaction's single whole-file
             * commit) are accepted structurally; the recovery scan still
             * verifies them when the tail is suspect. */
        }
    }

    const uint8_t *rec; size_t rl;
    int e = bjfile_read_record(f, md_off, &rec, &rl);
    if (e) return e;
    if (rl != meta_size) return BJ_ERR_STATE;
    *md = rec;
    *md_len = rl;
    return BJ_OK;
}

/*
 * After a scan stopped at a damaged region, decide tear vs corruption: search
 * [from, flen) for a trailer whose whole commit CRC-verifies. Returns 1 when
 * one exists (mid-file corruption — refuse), 0 when none (torn tail — safe to
 * truncate), or a negative I/O error.
 */
static int find_verified_commit(bjfile *f, uint64_t from, uint64_t flen) {
    enum { CH = 65536 };
    uint8_t *buf = (uint8_t *)malloc(CH);
    if (!buf) return BJ_ERR_OOM;
    int found = 0;
    uint64_t pos = from;
    while (pos < flen && !found) {
        size_t n = (flen - pos) < CH ? (size_t)(flen - pos) : CH;
        int64_t got = f->io.read(f->io.ctx, pos, buf, (uint32_t)n);
        if (got < 0) { free(buf); return (int)got; }
        n = (size_t)got;
        if (n < BJFILE_TRAILER_SIZE) break;
        for (size_t i = 0; i + BJFILE_TRAILER_SIZE <= n && !found; i++) {
            if (buf[i] != BJ_TYPE_BINARY || rd32(buf + i + 1) != 12 ||
                memcmp(buf + i + 5, TRAILER_MAGIC, 4) != 0)
                continue;
            uint32_t want_crc = rd32(buf + i + 9);
            uint32_t clen = rd32(buf + i + 13);
            uint64_t tr_off = pos + i;
            /* Locate the metadata record after the trailer and the commit
             * span; verify the CRC without assuming anything else. */
            const uint8_t *mrec; size_t mlen;
            if (bjfile_read_record(f, tr_off + BJFILE_TRAILER_SIZE, &mrec, &mlen))
                goto next;
            {
                uint64_t md_end = tr_off + BJFILE_TRAILER_SIZE + mlen;
                if (md_end > flen || (uint64_t)clen > md_end) goto next;
                uint64_t start = md_end - clen;
                if (start > tr_off) goto next;
                uint32_t crc = 0;
                uint8_t chunk[4096];
                if (crc_over_range(f, start, tr_off, chunk, sizeof chunk, &crc))
                    goto next;
                if (crc_over_range(f, tr_off + BJFILE_TRAILER_SIZE, md_end,
                                   chunk, sizeof chunk, &crc))
                    goto next;
                if (crc == want_crc) found = 1;
            }
        next:
            /* bjfile_read_record clobbered f->rd, not `buf`; keep scanning. */
            ;
        }
        pos += (n > BJFILE_TRAILER_SIZE) ? n - (BJFILE_TRAILER_SIZE - 1) : n;
    }
    free(buf);
    return found;
}

int bjfile_scan_commits(bjfile *f, bjfile_scan_cb cb, void *ctx,
                        uint64_t *last_good) {
    uint64_t flen = f->flen + f->wb_len;
    uint64_t pos = 0;
    uint64_t good = 0;          /* end of the last accepted commit          */
    uint64_t epoch = 0;         /* start of the commit being accumulated    */
    uint32_t crc = 0;
    int have_tr = 0;
    uint32_t tr_crc = 0, tr_clen = 0;
    int damaged = 0;

    while (pos < flen) {
        const uint8_t *rec; size_t rl;
        int re = bjfile_read_record(f, pos, &rec, &rl);
        if (re == BJ_ERR_OOM) return re;       /* resource, not file damage */
        if (re) { damaged = 1; break; }

        uint32_t c1, c2;
        if (is_trailer(rec, rl, &c1, &c2)) {
            if (have_tr) { damaged = 1; break; }   /* two trailers in a row */
            have_tr = 1; tr_crc = c1; tr_clen = c2;
            pos += rl;
            continue;
        }

        int is_end = 0;
        int e = cb(ctx, pos, rec, rl, &is_end);
        if (e) return e;
        crc = crc32_update(crc, rec, rl);
        uint64_t end = pos + rl;

        if (have_tr) {
            /* A trailer must be immediately followed by the metadata record
             * that ends its commit, and the CRC must match. */
            if (!is_end || (uint64_t)tr_clen != end - epoch || crc != tr_crc) {
                damaged = 1;
                break;
            }
        }
        if (is_end) {
            good = end;
            epoch = end;
            crc = 0;
            have_tr = 0;
        }
        pos = end;
    }

    /* Trailing records without a final metadata record (or a damaged region)
     * mean the file does not end at a commit boundary. */
    if (damaged || good < flen) {
        int v = find_verified_commit(f, good, flen);
        if (v < 0) return v;
        if (v) {
            /* A verifiable commit exists beyond the damage: check it isn't
             * simply the region the forward scan already blessed... it can't
             * be — the search starts at `good`. Refuse: truncating here
             * would destroy intact data. */
            return BJ_ERR_STATE;
        }
    }
    *last_good = good;
    return BJ_OK;
}
