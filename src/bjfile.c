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

/*
 * CONST, and built at compile time rather than on first use.
 *
 * This was a lazily-initialised mutable table behind a `crc_ready` flag,
 * which every reader reaches through bjfile_check_header. Two threads
 * arriving together both write it -- with identical values, so it is
 * benign in practice and undefined behaviour on paper, and a
 * ThreadSanitizer report on the first run of any threaded host. A table
 * that is never written races with nothing.
 *
 * The values are the standard reflected CRC-32 (IEEE 802.3) byte table:
 * entry i is i divided bit-by-bit by the reversed polynomial 0xEDB88320,
 * eight times. Regenerate with:
 *
 *   for i in 0..255: c = i; 8 times: c = (c & 1) ? 0xEDB88320 ^ (c>>1) : c>>1
 */
static const uint32_t crc_table[256] = {
    0x00000000u, 0x77073096u, 0xEE0E612Cu, 0x990951BAu, 0x076DC419u, 0x706AF48Fu,
    0xE963A535u, 0x9E6495A3u, 0x0EDB8832u, 0x79DCB8A4u, 0xE0D5E91Eu, 0x97D2D988u,
    0x09B64C2Bu, 0x7EB17CBDu, 0xE7B82D07u, 0x90BF1D91u, 0x1DB71064u, 0x6AB020F2u,
    0xF3B97148u, 0x84BE41DEu, 0x1ADAD47Du, 0x6DDDE4EBu, 0xF4D4B551u, 0x83D385C7u,
    0x136C9856u, 0x646BA8C0u, 0xFD62F97Au, 0x8A65C9ECu, 0x14015C4Fu, 0x63066CD9u,
    0xFA0F3D63u, 0x8D080DF5u, 0x3B6E20C8u, 0x4C69105Eu, 0xD56041E4u, 0xA2677172u,
    0x3C03E4D1u, 0x4B04D447u, 0xD20D85FDu, 0xA50AB56Bu, 0x35B5A8FAu, 0x42B2986Cu,
    0xDBBBC9D6u, 0xACBCF940u, 0x32D86CE3u, 0x45DF5C75u, 0xDCD60DCFu, 0xABD13D59u,
    0x26D930ACu, 0x51DE003Au, 0xC8D75180u, 0xBFD06116u, 0x21B4F4B5u, 0x56B3C423u,
    0xCFBA9599u, 0xB8BDA50Fu, 0x2802B89Eu, 0x5F058808u, 0xC60CD9B2u, 0xB10BE924u,
    0x2F6F7C87u, 0x58684C11u, 0xC1611DABu, 0xB6662D3Du, 0x76DC4190u, 0x01DB7106u,
    0x98D220BCu, 0xEFD5102Au, 0x71B18589u, 0x06B6B51Fu, 0x9FBFE4A5u, 0xE8B8D433u,
    0x7807C9A2u, 0x0F00F934u, 0x9609A88Eu, 0xE10E9818u, 0x7F6A0DBBu, 0x086D3D2Du,
    0x91646C97u, 0xE6635C01u, 0x6B6B51F4u, 0x1C6C6162u, 0x856530D8u, 0xF262004Eu,
    0x6C0695EDu, 0x1B01A57Bu, 0x8208F4C1u, 0xF50FC457u, 0x65B0D9C6u, 0x12B7E950u,
    0x8BBEB8EAu, 0xFCB9887Cu, 0x62DD1DDFu, 0x15DA2D49u, 0x8CD37CF3u, 0xFBD44C65u,
    0x4DB26158u, 0x3AB551CEu, 0xA3BC0074u, 0xD4BB30E2u, 0x4ADFA541u, 0x3DD895D7u,
    0xA4D1C46Du, 0xD3D6F4FBu, 0x4369E96Au, 0x346ED9FCu, 0xAD678846u, 0xDA60B8D0u,
    0x44042D73u, 0x33031DE5u, 0xAA0A4C5Fu, 0xDD0D7CC9u, 0x5005713Cu, 0x270241AAu,
    0xBE0B1010u, 0xC90C2086u, 0x5768B525u, 0x206F85B3u, 0xB966D409u, 0xCE61E49Fu,
    0x5EDEF90Eu, 0x29D9C998u, 0xB0D09822u, 0xC7D7A8B4u, 0x59B33D17u, 0x2EB40D81u,
    0xB7BD5C3Bu, 0xC0BA6CADu, 0xEDB88320u, 0x9ABFB3B6u, 0x03B6E20Cu, 0x74B1D29Au,
    0xEAD54739u, 0x9DD277AFu, 0x04DB2615u, 0x73DC1683u, 0xE3630B12u, 0x94643B84u,
    0x0D6D6A3Eu, 0x7A6A5AA8u, 0xE40ECF0Bu, 0x9309FF9Du, 0x0A00AE27u, 0x7D079EB1u,
    0xF00F9344u, 0x8708A3D2u, 0x1E01F268u, 0x6906C2FEu, 0xF762575Du, 0x806567CBu,
    0x196C3671u, 0x6E6B06E7u, 0xFED41B76u, 0x89D32BE0u, 0x10DA7A5Au, 0x67DD4ACCu,
    0xF9B9DF6Fu, 0x8EBEEFF9u, 0x17B7BE43u, 0x60B08ED5u, 0xD6D6A3E8u, 0xA1D1937Eu,
    0x38D8C2C4u, 0x4FDFF252u, 0xD1BB67F1u, 0xA6BC5767u, 0x3FB506DDu, 0x48B2364Bu,
    0xD80D2BDAu, 0xAF0A1B4Cu, 0x36034AF6u, 0x41047A60u, 0xDF60EFC3u, 0xA867DF55u,
    0x316E8EEFu, 0x4669BE79u, 0xCB61B38Cu, 0xBC66831Au, 0x256FD2A0u, 0x5268E236u,
    0xCC0C7795u, 0xBB0B4703u, 0x220216B9u, 0x5505262Fu, 0xC5BA3BBEu, 0xB2BD0B28u,
    0x2BB45A92u, 0x5CB36A04u, 0xC2D7FFA7u, 0xB5D0CF31u, 0x2CD99E8Bu, 0x5BDEAE1Du,
    0x9B64C2B0u, 0xEC63F226u, 0x756AA39Cu, 0x026D930Au, 0x9C0906A9u, 0xEB0E363Fu,
    0x72076785u, 0x05005713u, 0x95BF4A82u, 0xE2B87A14u, 0x7BB12BAEu, 0x0CB61B38u,
    0x92D28E9Bu, 0xE5D5BE0Du, 0x7CDCEFB7u, 0x0BDBDF21u, 0x86D3D2D4u, 0xF1D4E242u,
    0x68DDB3F8u, 0x1FDA836Eu, 0x81BE16CDu, 0xF6B9265Bu, 0x6FB077E1u, 0x18B74777u,
    0x88085AE6u, 0xFF0F6A70u, 0x66063BCAu, 0x11010B5Cu, 0x8F659EFFu, 0xF862AE69u,
    0x616BFFD3u, 0x166CCF45u, 0xA00AE278u, 0xD70DD2EEu, 0x4E048354u, 0x3903B3C2u,
    0xA7672661u, 0xD06016F7u, 0x4969474Du, 0x3E6E77DBu, 0xAED16A4Au, 0xD9D65ADCu,
    0x40DF0B66u, 0x37D83BF0u, 0xA9BCAE53u, 0xDEBB9EC5u, 0x47B2CF7Fu, 0x30B5FFE9u,
    0xBDBDF21Cu, 0xCABAC28Au, 0x53B39330u, 0x24B4A3A6u, 0xBAD03605u, 0xCDD70693u,
    0x54DE5729u, 0x23D967BFu, 0xB3667A2Eu, 0xC4614AB8u, 0x5D681B02u, 0x2A6F2B94u,
    0xB40BBE37u, 0xC30C8EA1u, 0x5A05DF1Bu, 0x2D02EF8Du
};

static uint32_t crc32_update(uint32_t crc, const uint8_t *p, size_t n) {
    crc = ~crc;
    while (n--) crc = crc_table[(crc ^ *p++) & 0xffu] ^ (crc >> 8);
    return ~crc;
}

uint32_t bjfile_crc32(uint32_t crc, const uint8_t *p, size_t n) {
    return crc32_update(crc, p, n);
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
    /* Zero BEFORE the check, not after. A caller whose io is refused still
     * owns `f` and will hand it to bjfile_dispose on the way out, so it
     * has to be in a defined state on the failure path too -- and leaving
     * f->io holding whatever was on the caller's stack means the next
     * f->io.write() is a jump into it. */
    memset(f, 0, sizeof(*f));
    /* Every file-resident structure funnels through here, so this is the
     * one place that can refuse an io which cannot honor the durability
     * contract -- writable with no sync. A no-op unless the build asked
     * for enforcement (BJIO_REQUIRE_SYNC); see bjio.h. */
    int e = bjio_check(io);
    if (e) return e;
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

/* An adapter reports failure as a negative count (bjio.h), and every
 * caller here narrows that to int. The narrowing is lossy in principle:
 * a negative whose low 32 bits are zero would arrive as BJ_OK, and the
 * caller would read a buffer nothing had filled. One line, in one place,
 * so no read site has to remember. */
static int io_read_err(int64_t got) {
    int rc = (int)got;
    return rc ? rc : BJ_ERR_STATE;
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

int bjfile_sync(bjfile *f) {
    int e = bjfile_commit(f);
    if (e) return e;
    /* No sync callback means the io is already durable on write (memory
     * backed). A real file adapter without one is rejected at open by
     * bjio_check under BJIO_REQUIRE_SYNC. */
    if (f->io.sync) return (int)f->io.sync(f->io.ctx);
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
    if (got < 0) return io_read_err(got);

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
        if (more < 0) return io_read_err(more);
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
    /* Answered on every path, including the failing ones: a caller that
     * checked the code and a caller that did not both see a pointer that
     * was written by this function rather than whatever the stack held.
     * gcc could not prove that either -- -Wmaybe-uninitialized on the
     * trailer read in bjfile_check_tail is what asked the question. */
    *p = NULL;
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
    if (got < 0) return io_read_err(got);
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
                uint64_t ufmt = 0;
                for (int i = 7; i >= 0; i--) ufmt = (ufmt << 8) | rec[pos + 1 + i];
                int64_t fmt = (int64_t)ufmt;
                if (fmt < 0 || fmt > 1) return BJ_ERR_STATE;
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
        if (got < 0) return io_read_err(got);
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
        if (got < 0) { free(buf); return io_read_err(got); }
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
