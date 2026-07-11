/*
 * textlog.c — C port of src/textlog.js. See textlog.h.
 *
 * The log is persistent and append-only: every addVersion appends an entry
 * (full snapshot or diff) followed by a fresh metadata record to the backing
 * file, exactly like the reference. Entries/metadata use the binjson wire
 * format from binjson.c. All file access goes through bjfile (bjfile.h); an
 * in-memory index of entry offsets (built by one scan at open) lets version
 * reconstruction read only the snapshot + diff chain it actually needs.
 *
 * The on-disk format is byte-compatible with src/textlog.js, so files
 * interoperate with the JS log:
 *   - SHA-256 is implemented inline (matches crypto.createHash('sha256')).
 *   - DIFF entries store the exact unified-diff patch text that jsdiff's
 *     createPatch would produce (see diff.c), reconstructed via applyPatch.
 *   - getDiff mirrors textlog.js's structuredPatch-based formatting.
 * Entry and metadata records use the same binjson object shapes and key order
 * as the reference, so bytes match field-for-field.
 */
#include "textlog.h"
#include "bjfile.h"
#include "bjcursor.h"
#include "dbuf.h"
#include "diff.h"

#include <stdlib.h>
#include <string.h>

/* ---- SHA-256 -------------------------------------------------------- */

typedef struct {
    uint32_t state[8];
    uint64_t bitlen;
    uint8_t  buf[64];
    size_t   buflen;
} sha256_ctx;

static uint32_t sha_rotr(uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }

static const uint32_t SHA_K[64] = {
    0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
    0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
    0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
    0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
    0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
    0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
    0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
    0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2
};

static void sha256_init(sha256_ctx *c) {
    c->state[0] = 0x6a09e667; c->state[1] = 0xbb67ae85;
    c->state[2] = 0x3c6ef372; c->state[3] = 0xa54ff53a;
    c->state[4] = 0x510e527f; c->state[5] = 0x9b05688c;
    c->state[6] = 0x1f83d9ab; c->state[7] = 0x5be0cd19;
    c->bitlen = 0; c->buflen = 0;
}

static void sha256_block(sha256_ctx *c, const uint8_t *p) {
    uint32_t w[64];
    for (int i = 0; i < 16; i++) {
        w[i] = ((uint32_t)p[i * 4] << 24) | ((uint32_t)p[i * 4 + 1] << 16) |
               ((uint32_t)p[i * 4 + 2] << 8) | (uint32_t)p[i * 4 + 3];
    }
    for (int i = 16; i < 64; i++) {
        uint32_t s0 = sha_rotr(w[i - 15], 7) ^ sha_rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
        uint32_t s1 = sha_rotr(w[i - 2], 17) ^ sha_rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    uint32_t a = c->state[0], b = c->state[1], cc = c->state[2], d = c->state[3];
    uint32_t e = c->state[4], f = c->state[5], g = c->state[6], h = c->state[7];
    for (int i = 0; i < 64; i++) {
        uint32_t S1 = sha_rotr(e, 6) ^ sha_rotr(e, 11) ^ sha_rotr(e, 25);
        uint32_t ch = (e & f) ^ (~e & g);
        uint32_t t1 = h + S1 + ch + SHA_K[i] + w[i];
        uint32_t S0 = sha_rotr(a, 2) ^ sha_rotr(a, 13) ^ sha_rotr(a, 22);
        uint32_t maj = (a & b) ^ (a & cc) ^ (b & cc);
        uint32_t t2 = S0 + maj;
        h = g; g = f; f = e; e = d + t1; d = cc; cc = b; b = a; a = t1 + t2;
    }
    c->state[0] += a; c->state[1] += b; c->state[2] += cc; c->state[3] += d;
    c->state[4] += e; c->state[5] += f; c->state[6] += g; c->state[7] += h;
}

static void sha256_update(sha256_ctx *c, const uint8_t *p, size_t n) {
    c->bitlen += (uint64_t)n * 8;
    while (n) {
        size_t take = 64 - c->buflen;
        if (take > n) take = n;
        memcpy(c->buf + c->buflen, p, take);
        c->buflen += take; p += take; n -= take;
        if (c->buflen == 64) { sha256_block(c, c->buf); c->buflen = 0; }
    }
}

static void sha256_final(sha256_ctx *c, uint8_t out[32]) {
    uint64_t bits = c->bitlen;
    uint8_t pad = 0x80;
    sha256_update(c, &pad, 1);
    /* sha256_update bumped bitlen; restore it for the length field below by
     * tracking separately is simpler: recompute isn't needed — bits captured. */
    uint8_t zero = 0;
    while (c->buflen != 56) sha256_update(c, &zero, 1);
    uint8_t len8[8];
    for (int i = 0; i < 8; i++) len8[i] = (uint8_t)(bits >> (56 - i * 8));
    sha256_update(c, len8, 8);
    for (int i = 0; i < 8; i++) {
        out[i * 4]     = (uint8_t)(c->state[i] >> 24);
        out[i * 4 + 1] = (uint8_t)(c->state[i] >> 16);
        out[i * 4 + 2] = (uint8_t)(c->state[i] >> 8);
        out[i * 4 + 3] = (uint8_t)(c->state[i]);
    }
}

/* Write the lowercase hex SHA-256 of (p,n) into hex[64]. */
static void sha256_hex(const uint8_t *p, size_t n, uint8_t hex[64]) {
    static const char H[] = "0123456789abcdef";
    sha256_ctx c;
    sha256_init(&c);
    sha256_update(&c, p, n);
    uint8_t digest[32];
    sha256_final(&c, digest);
    for (int i = 0; i < 32; i++) {
        hex[i * 2]     = (uint8_t)H[digest[i] >> 4];
        hex[i * 2 + 1] = (uint8_t)H[digest[i] & 0xf];
    }
}

/* ---- Records (wire primitives in bjcursor.h) ------------------------ */

typedef struct {
    int is_entry;                 /* has "type" key                    */
    int is_metadata;              /* has "diffsPerSnapshot" key        */
    int type;                     /* entry type (snapshot / diff)      */
    uint64_t version;
    const uint8_t *hash; uint32_t hashlen;  /* into image              */
    const uint8_t *data; uint32_t datalen;  /* into image              */
    int64_t ts;
    /* metadata fields */
    int has_snap; uint64_t snap;
    int has_latest; uint64_t latest;
    int64_t diff_count;
    int diffs_per_snapshot;
} trec;

/* Parse the record bytes (rec, len) into *r. The string fields point into
 * `rec` and are only valid while those bytes are. */
static int parse_record(const uint8_t *rec, size_t len, trec *r) {
    memset(r, 0, sizeof(*r));
    cur c = { rec, len, 0 };
    uint32_t count;
    int e = object_begin(&c, &count);
    if (e) return e;
    for (uint32_t i = 0; i < count; i++) {
        const uint8_t *kn; uint32_t klen;
        if ((e = take_key(&c, &kn, &klen))) return e;
        if (name_eq(kn, klen, "type")) {
            if ((e = read_int31(&c, &r->type))) return e;
            r->is_entry = 1;
        } else if (name_eq(kn, klen, "version")) {
            if ((e = read_u64(&c, &r->version))) return e;
        } else if (name_eq(kn, klen, "hash")) {
            if ((e = take_string(&c, &r->hash, &r->hashlen))) return e;
        } else if (name_eq(kn, klen, "data")) {
            if ((e = take_string(&c, &r->data, &r->datalen))) return e;
        } else if (name_eq(kn, klen, "timestamp")) {
            if ((e = read_date(&c, &r->ts))) return e;
        } else if (name_eq(kn, klen, "snapshotPointer")) {
            if ((e = read_ptr_or_null(&c, &r->has_snap, &r->snap))) return e;
        } else if (name_eq(kn, klen, "latestPointer")) {
            if ((e = read_ptr_or_null(&c, &r->has_latest, &r->latest))) return e;
        } else if (name_eq(kn, klen, "diffCount")) {
            uint64_t u;
            if ((e = read_u64(&c, &u))) return e;
            r->diff_count = (int64_t)u;
        } else if (name_eq(kn, klen, "diffsPerSnapshot")) {
            if ((e = read_int31(&c, &r->diffs_per_snapshot))) return e;
            r->is_metadata = 1;
        } else {
            if ((e = skip_value(&c))) return e;
        }
    }
    return BJ_OK;
}

/* ---- Log state ------------------------------------------------------ */

/* Index of one entry record: where it lives and what it is. Built by the
 * open-time scan and extended on every addVersion, so reconstruction reads
 * only the records a version actually needs. */
typedef struct {
    uint64_t off;
    uint64_t version;
    uint8_t  type;               /* TL_FULL_SNAPSHOT / TL_DIFF          */
} tl_ent;

struct textlog {
    bjfile      f;               /* backing file                        */
    dbuf        out;             /* last read output                    */
    bj_builder *bld;             /* reused for entry/metadata encoding  */
    uint64_t    version;
    int64_t     diff_count;
    int         diffs_per_snapshot;
    int         has_snapshot; uint64_t snapshot_ptr;
    int         has_latest;   uint64_t latest_ptr;
    tl_ent     *ents; int n_ents, cap_ents;   /* entry index            */
};

static int ents_reserve(textlog *t, int need) {
    if (need <= t->cap_ents) return BJ_OK;
    int nc = t->cap_ents ? t->cap_ents * 2 : 16;
    while (nc < need) nc *= 2;
    tl_ent *ne = (tl_ent *)realloc(t->ents, (size_t)nc * sizeof(tl_ent));
    if (!ne) return BJ_ERR_OOM;
    t->ents = ne; t->cap_ents = nc;
    return BJ_OK;
}

static int set_out(textlog *t, const uint8_t *b, size_t n) {
    t->out.len = 0;
    return dbuf_put(&t->out, b, n);
}

/* ---- Entry / metadata encoding -------------------------------------- */

static int append_builder(textlog *t) {
    int e = bj_builder_error(t->bld);
    if (e) return e;
    size_t len;
    const uint8_t *d = bj_builder_data(t->bld, &len);
    if (!d) return BJ_ERR_STATE;
    return bjfile_append(&t->f, d, len, NULL);
}

static int encode_entry(textlog *t, int type, uint64_t version,
                        const uint8_t *hash, uint32_t hashlen,
                        const uint8_t *data, uint32_t datalen, int64_t ts_ms) {
    bj_builder *b = t->bld;
    bj_builder_reset(b);
    bj_begin_object(b);
    bj_put_key(b, (const uint8_t *)"type", 4);      bj_put_int(b, type);
    bj_put_key(b, (const uint8_t *)"version", 7);   bj_put_int(b, (int64_t)version);
    bj_put_key(b, (const uint8_t *)"hash", 4);      bj_put_string(b, hash, hashlen);
    bj_put_key(b, (const uint8_t *)"data", 4);      bj_put_string(b, data, datalen);
    bj_put_key(b, (const uint8_t *)"timestamp", 9); bj_put_date(b, ts_ms);
    bj_end_object(b);
    return append_builder(t);
}

static int save_metadata(textlog *t) {
    bj_builder *b = t->bld;
    bj_builder_reset(b);
    bj_begin_object(b);
    bj_put_key(b, (const uint8_t *)"version", 7);
    bj_put_int(b, (int64_t)t->version);
    bj_put_key(b, (const uint8_t *)"snapshotPointer", 15);
    if (t->has_snapshot) bj_put_pointer(b, t->snapshot_ptr); else bj_put_null(b);
    bj_put_key(b, (const uint8_t *)"latestPointer", 13);
    if (t->has_latest) bj_put_pointer(b, t->latest_ptr); else bj_put_null(b);
    bj_put_key(b, (const uint8_t *)"diffCount", 9);
    bj_put_int(b, t->diff_count);
    bj_put_key(b, (const uint8_t *)"diffsPerSnapshot", 16);
    bj_put_int(b, t->diffs_per_snapshot);
    bj_end_object(b);
    int e = bj_builder_error(b);
    if (e) return e;
    size_t len;
    const uint8_t *d = bj_builder_data(b, &len);
    if (!d) return BJ_ERR_STATE;
    /* Metadata ends every commit; a CRC trailer written just before it covers
     * all of the operation's appended bytes (bjfile_append_protected). */
    return bjfile_append_protected(&t->f, d, len);
}

/* ---- Version reconstruction ----------------------------------------- */

static int diff_err_to_bj(int e) {
    return (e == DIFF_ERR_OOM) ? BJ_ERR_OOM : BJ_ERR_STATE;
}

/*
 * Rebuild the full text of `version` into *text (reset first): start from the
 * latest snapshot at or before `version` and apply each subsequent DIFF entry's
 * patch in order, exactly like textlog.js getVersion. The entry index makes
 * this read only the snapshot + diff chain, not the whole file.
 */
static int reconstruct_version(textlog *t, uint64_t version, dbuf *text) {
    text->len = 0;
    int hi = t->n_ents - 1;
    while (hi >= 0 && t->ents[hi].version > version) hi--;
    if (hi < 0) return BJ_OK;   /* nothing at or before `version` */
    int start = hi;
    while (start >= 0 && t->ents[start].type != TL_FULL_SNAPSHOT) start--;
    if (start < 0) return BJ_ERR_STATE;   /* diff chain without a snapshot */

    for (int i = start; i <= hi; i++) {
        const uint8_t *rec; size_t rec_len;
        int e = bjfile_read_record(&t->f, t->ents[i].off, &rec, &rec_len);
        if (e) return e;
        trec r;
        e = parse_record(rec, rec_len, &r);
        if (e) return e;
        if (r.type == TL_FULL_SNAPSHOT) {
            text->len = 0;
            if ((e = dbuf_put(text, r.data, r.datalen))) return e;
        } else if (r.type == TL_DIFF) {
            uint8_t *nt = NULL; size_t ntl = 0; int applied = 0;
            int de = diff_apply_patch(text->data, text->len, r.data, r.datalen, &nt, &ntl, &applied);
            if (de) return diff_err_to_bj(de);
            if (!applied) { free(nt); return BJ_ERR_STATE; }
            text->len = 0;
            e = dbuf_put(text, nt, ntl);
            free(nt);
            if (e) return e;
        }
    }
    return BJ_OK;
}

/* ---- addVersion ----------------------------------------------------- */

static int add_version_inner(textlog *t, const uint8_t *text, uint32_t text_len,
                             int64_t ts_ms, uint64_t offset, uint8_t *out_type) {
    uint8_t hash[64];
    sha256_hex(text, text_len, hash);

    uint64_t new_version = t->version + 1;
    int should_snapshot = (t->diff_count >= t->diffs_per_snapshot) || !t->has_latest;
    int e;

    if (should_snapshot) {
        e = encode_entry(t, TL_FULL_SNAPSHOT, new_version, hash, 64, text, text_len, ts_ms);
        if (e) return e;
        t->diff_count = 0;
        t->has_snapshot = 1;
        t->snapshot_ptr = offset;
        *out_type = TL_FULL_SNAPSHOT;
    } else {
        dbuf prev; memset(&prev, 0, sizeof(prev));
        e = reconstruct_version(t, t->version, &prev);
        if (e) { dbuf_free(&prev); return e; }
        uint8_t *patch = NULL; size_t plen = 0;
        int de = diff_create_patch("document", prev.data, prev.len, text, text_len, &patch, &plen);
        dbuf_free(&prev);
        if (de) { free(patch); return diff_err_to_bj(de); }
        e = encode_entry(t, TL_DIFF, new_version, hash, 64, patch, (uint32_t)plen, ts_ms);
        free(patch);
        if (e) return e;
        t->diff_count += 1;
        *out_type = TL_DIFF;
    }

    t->has_latest = 1;
    t->latest_ptr = offset;
    t->version = new_version;
    return save_metadata(t);
}

int textlog_add_version(textlog *t, const uint8_t *text, uint32_t text_len,
                        int64_t ts_ms, uint64_t *out_version) {
    /* Snapshot rollback state; commit the whole entry + metadata with one
     * host write, or leave the file (and this state) untouched on failure. */
    uint64_t sv_version = t->version;
    int64_t sv_diff = t->diff_count;
    uint64_t sv_sp = t->snapshot_ptr, sv_lp = t->latest_ptr;
    int sv_hs = t->has_snapshot, sv_hl = t->has_latest;

    int e = ents_reserve(t, t->n_ents + 1);
    if (e) return e;

    uint64_t offset = bjfile_len(&t->f);
    uint8_t type = 0;
    e = add_version_inner(t, text, text_len, ts_ms, offset, &type);
    if (!e) e = bjfile_commit(&t->f);
    if (e) {
        bjfile_discard(&t->f);
        t->version = sv_version; t->diff_count = sv_diff;
        t->snapshot_ptr = sv_sp; t->latest_ptr = sv_lp;
        t->has_snapshot = sv_hs; t->has_latest = sv_hl;
        return e;
    }

    tl_ent ent = { offset, t->version, type };
    t->ents[t->n_ents++] = ent;   /* capacity reserved above */
    if (out_version) *out_version = t->version;
    return BJ_OK;
}

int textlog_get_version(textlog *t, uint64_t version,
                        const uint8_t **out_ptr, size_t *out_len) {
    dbuf text; memset(&text, 0, sizeof(text));
    int e = reconstruct_version(t, version, &text);
    if (!e) e = set_out(t, text.data, text.len);
    dbuf_free(&text);
    if (e) return e;
    *out_ptr = t->out.data; *out_len = t->out.len;
    return BJ_OK;
}

int textlog_get_version_hash(textlog *t, uint64_t version,
                             const uint8_t **out_ptr, size_t *out_len) {
    for (int i = t->n_ents - 1; i >= 0; i--) {
        if (t->ents[i].version != version) continue;
        const uint8_t *rec; size_t rec_len;
        int e = bjfile_read_record(&t->f, t->ents[i].off, &rec, &rec_len);
        if (e) return e;
        trec r;
        e = parse_record(rec, rec_len, &r);
        if (e) return e;
        e = set_out(t, r.hash, r.hashlen);
        if (e) return e;
        *out_ptr = t->out.data; *out_len = t->out.len;
        return BJ_OK;
    }
    return BJ_ERR_STATE; /* not found (host validates range, so unreachable) */
}

/* ---- getDiff -------------------------------------------------------- */

int textlog_get_diff(textlog *t, uint64_t from_version, uint64_t to_version,
                     const uint8_t **out_ptr, size_t *out_len) {
    dbuf from_text; memset(&from_text, 0, sizeof(from_text));
    dbuf to_text; memset(&to_text, 0, sizeof(to_text));
    int e = reconstruct_version(t, from_version, &from_text);
    if (!e) e = reconstruct_version(t, to_version, &to_text);
    if (!e) {
        uint8_t *buf = NULL; size_t len = 0;
        int de = diff_get_diff((long)from_version, (long)to_version,
                               from_text.data, from_text.len,
                               to_text.data, to_text.len, &buf, &len);
        if (de) e = diff_err_to_bj(de);
        else { e = set_out(t, buf, len); free(buf); }
    }
    dbuf_free(&from_text); dbuf_free(&to_text);
    if (e) return e;
    *out_ptr = t->out.data; *out_len = t->out.len;
    return BJ_OK;
}

/* ---- Lifecycle & accessors ------------------------------------------ */

textlog *textlog_create(const bj_io *io, int diffs_per_snapshot) {
    if (diffs_per_snapshot < 1) return NULL;
    textlog *t = (textlog *)calloc(1, sizeof(textlog));
    if (!t) return NULL;
    t->bld = bj_builder_new();
    if (!t->bld) { free(t); return NULL; }
    bjfile_init(&t->f, io);
    t->version = 0;
    t->diff_count = 0;
    t->diffs_per_snapshot = diffs_per_snapshot;
    t->has_snapshot = 0;
    t->has_latest = 0;
    if (bjfile_append_header(&t->f, t->bld, "textlog") ||
        save_metadata(t) || bjfile_commit(&t->f)) {
        textlog_free(t);
        return NULL;
    }
    return t;
}

/*
 * Commit-scan state: entries are indexed provisionally (trimmed back to the
 * last good commit afterwards) and the last two metadata candidates are kept
 * — at most one metadata record can sit in a rejected tail commit, so the
 * one ending exactly at the last good offset is always among them. The
 * pointer fields of a trec are unused for metadata, so a value copy is safe.
 */
typedef struct {
    textlog *t;
    trec     md[2];
    uint64_t md_end[2];
    int      n_md;
} tl_scan;

static int tl_scan_cb(void *ctx, uint64_t off, const uint8_t *rec,
                      size_t rec_len, int *is_commit_end) {
    tl_scan *s = (tl_scan *)ctx;
    trec r;
    if (parse_record(rec, rec_len, &r)) return BJ_OK;  /* not an object: skip */
    if (r.is_entry) {
        if (ents_reserve(s->t, s->t->n_ents + 1)) return BJ_ERR_OOM;
        tl_ent ent = { off, r.version, (uint8_t)r.type };
        s->t->ents[s->t->n_ents++] = ent;
    }
    if (r.is_metadata && r.diffs_per_snapshot >= 1) {
        s->md[s->n_md & 1] = r;
        s->md_end[s->n_md & 1] = off + rec_len;
        s->n_md++;
        *is_commit_end = 1;
    }
    return BJ_OK;
}

/*
 * Open: verify the file identifies as a text log (when it carries a header;
 * files from the JS reference have none and are accepted), then scan it once
 * — indexing entry offsets and verifying every protected commit's CRC. A torn
 * tail is truncated back to the last good commit; verifiable data beyond a
 * damaged region refuses to open rather than silently truncating good
 * commits away.
 */
textlog *textlog_open(const bj_io *io) {
    textlog *t = (textlog *)calloc(1, sizeof(textlog));
    if (!t) return NULL;
    t->bld = bj_builder_new();
    if (!t->bld) { free(t); return NULL; }
    bjfile_init(&t->f, io);

    if (bjfile_check_header(&t->f, "textlog") < 0) { textlog_free(t); return NULL; }

    tl_scan s;
    memset(&s, 0, sizeof(s));
    s.t = t;
    uint64_t good = 0, flen = bjfile_len(&t->f);
    if (bjfile_scan_commits(&t->f, tl_scan_cb, &s, &good)) { textlog_free(t); return NULL; }

    /* Adopt the metadata record that ends the last good commit. */
    trec *adopt = NULL;
    for (int i = 0; i < 2 && i < s.n_md; i++)
        if (s.md_end[i] == good) adopt = &s.md[i];
    if (!adopt ||
        (adopt->has_snap && adopt->snap >= good) ||
        (adopt->has_latest && adopt->latest >= good) ||
        adopt->diff_count < 0) {
        textlog_free(t);
        return NULL;
    }

    /* Drop provisionally indexed entries from a rejected tail commit. */
    while (t->n_ents && t->ents[t->n_ents - 1].off >= good) t->n_ents--;

    t->version = adopt->version;
    t->has_snapshot = adopt->has_snap; t->snapshot_ptr = adopt->snap;
    t->has_latest = adopt->has_latest; t->latest_ptr = adopt->latest;
    t->diff_count = adopt->diff_count;
    t->diffs_per_snapshot = adopt->diffs_per_snapshot;

    if (good < flen && bjfile_set_len(&t->f, good)) { textlog_free(t); return NULL; }
    return t;
}

void textlog_free(textlog *t) {
    if (!t) return;
    bj_builder_free(t->bld);
    bjfile_dispose(&t->f);
    free(t->out.data);
    free(t->ents);
    free(t);
}

uint64_t       textlog_version(const textlog *t)            { return t->version; }
int            textlog_diffs_per_snapshot(const textlog *t) { return t->diffs_per_snapshot; }
const uint8_t *textlog_out(const textlog *t, size_t *len)   { if (len) *len = t->out.len; return t->out.data; }
