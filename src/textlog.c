/*
 * textlog.c — C port of src/textlog.js. See textlog.h.
 *
 * The log is persistent and append-only: every addVersion appends an entry
 * (full snapshot or diff) followed by a fresh metadata record to the in-memory
 * file image, exactly like the reference. Entries/metadata use the binjson wire
 * format from binjson.c.
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
#include "diff.h"

#include <stdlib.h>
#include <string.h>

/* ---- Dynamic byte buffer (mirrors rtree.c) -------------------------- */

typedef struct { uint8_t *data; size_t len, cap; } dbuf;

static int dbuf_ensure(dbuf *b, size_t extra) {
    if (b->len + extra <= b->cap) return BJ_OK;
    size_t nc = b->cap ? b->cap : 256;
    while (nc < b->len + extra) nc *= 2;
    uint8_t *nb = (uint8_t *)realloc(b->data, nc);
    if (!nb) return BJ_ERR_OOM;
    b->data = nb; b->cap = nc;
    return BJ_OK;
}
static int dbuf_put(dbuf *b, const uint8_t *p, size_t n) {
    int e = dbuf_ensure(b, n);
    if (e) return e;
    if (n) memcpy(b->data + b->len, p, n);
    b->len += n;
    return BJ_OK;
}
static void dbuf_free(dbuf *b) { free(b->data); b->data = NULL; b->len = b->cap = 0; }

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

/* ---- Little-endian readers (mirror rtree.c) ------------------------- */

static uint32_t rdu32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static uint64_t rdu64(const uint8_t *p) {
    uint64_t v = 0;
    for (int i = 7; i >= 0; i--) v = (v << 8) | p[i];
    return v;
}

typedef struct { const uint8_t *d; size_t len; size_t pos; } cur;

static int cur_need(const cur *c, size_t n) { return n <= c->len - c->pos ? BJ_OK : BJ_ERR_EOF; }
static int take_type(cur *c, uint8_t *t) {
    if (cur_need(c, 1)) return BJ_ERR_EOF;
    *t = c->d[c->pos++];
    return BJ_OK;
}
static int take_u32(cur *c, uint32_t *v) {
    if (cur_need(c, 4)) return BJ_ERR_EOF;
    *v = rdu32(c->d + c->pos);
    c->pos += 4;
    return BJ_OK;
}
static int name_eq(const uint8_t *p, uint32_t len, const char *s) {
    size_t sl = strlen(s);
    return len == sl && memcmp(p, s, sl) == 0;
}
static int read_number(cur *c, double *out) {
    uint8_t t;
    if (take_type(c, &t)) return BJ_ERR_EOF;
    if (t == BJ_TYPE_INT) {
        if (cur_need(c, 8)) return BJ_ERR_EOF;
        int64_t v = (int64_t)rdu64(c->d + c->pos);
        c->pos += 8; *out = (double)v; return BJ_OK;
    }
    if (t == BJ_TYPE_FLOAT) {
        if (cur_need(c, 8)) return BJ_ERR_EOF;
        uint64_t bits = rdu64(c->d + c->pos);
        c->pos += 8; memcpy(out, &bits, 8); return BJ_OK;
    }
    return BJ_ERR_UNKNOWN_TYPE;
}
static int read_pointer(cur *c, double *out) {
    uint8_t t;
    if (take_type(c, &t)) return BJ_ERR_EOF;
    if (t != BJ_TYPE_POINTER) return BJ_ERR_UNKNOWN_TYPE;
    if (cur_need(c, 8)) return BJ_ERR_EOF;
    *out = (double)rdu64(c->d + c->pos);
    c->pos += 8; return BJ_OK;
}
/* POINTER or NULL. Sets *has and, when present, *out. */
static int read_ptr_or_null(cur *c, int *has, double *out) {
    if (cur_need(c, 1)) return BJ_ERR_EOF;
    if (c->d[c->pos] == BJ_TYPE_NULL) { c->pos++; *has = 0; return BJ_OK; }
    *has = 1;
    return read_pointer(c, out);
}
static int read_date(cur *c, double *out) {
    uint8_t t;
    if (take_type(c, &t)) return BJ_ERR_EOF;
    if (t != BJ_TYPE_DATE) return BJ_ERR_UNKNOWN_TYPE;
    if (cur_need(c, 8)) return BJ_ERR_EOF;
    int64_t v = (int64_t)rdu64(c->d + c->pos);
    c->pos += 8; *out = (double)v; return BJ_OK;
}
static int take_string(cur *c, const uint8_t **p, uint32_t *len) {
    uint8_t t;
    if (take_type(c, &t)) return BJ_ERR_EOF;
    if (t != BJ_TYPE_STRING) return BJ_ERR_UNKNOWN_TYPE;
    if (take_u32(c, len)) return BJ_ERR_EOF;
    if (cur_need(c, *len)) return BJ_ERR_EOF;
    *p = c->d + c->pos; c->pos += *len;
    return BJ_OK;
}
static int object_begin(cur *c, uint32_t *count) {
    uint8_t t;
    if (take_type(c, &t)) return BJ_ERR_EOF;
    if (t != BJ_TYPE_OBJECT) return BJ_ERR_UNKNOWN_TYPE;
    uint32_t size;
    if (take_u32(c, &size)) return BJ_ERR_EOF;
    return take_u32(c, count);
}
static int take_key(cur *c, const uint8_t **kn, uint32_t *klen) {
    if (take_u32(c, klen)) return BJ_ERR_EOF;
    if (cur_need(c, *klen)) return BJ_ERR_EOF;
    *kn = c->d + c->pos;
    c->pos += *klen;
    return BJ_OK;
}
static int skip_value(cur *c) {
    size_t sz;
    int e = bj_value_size(c->d, c->len, c->pos, &sz);
    if (e) return e;
    if (cur_need(c, sz)) return BJ_ERR_EOF;
    c->pos += sz; return BJ_OK;
}

/* ---- Records -------------------------------------------------------- */

typedef struct {
    int is_entry;                 /* has "type" key                    */
    int is_metadata;              /* has "diffsPerSnapshot" key        */
    int type;                     /* entry type (snapshot / diff)      */
    double version;
    const uint8_t *hash; uint32_t hashlen;  /* into image              */
    const uint8_t *data; uint32_t datalen;  /* into image              */
    double ts;
    /* metadata fields */
    int has_snap; double snap;
    int has_latest; double latest;
    double diff_count;
    int diffs_per_snapshot;
} trec;

/* Parse the object at (img+off) into *r. */
static int parse_record(const uint8_t *img, size_t len, size_t off, trec *r) {
    memset(r, 0, sizeof(*r));
    if (off > len) return BJ_ERR_EOF;
    cur c = { img + off, len - off, 0 };
    uint32_t count;
    int e = object_begin(&c, &count);
    if (e) return e;
    for (uint32_t i = 0; i < count; i++) {
        const uint8_t *kn; uint32_t klen;
        if ((e = take_key(&c, &kn, &klen))) return e;
        double d;
        if (name_eq(kn, klen, "type")) {
            if ((e = read_number(&c, &d))) return e;
            r->is_entry = 1; r->type = (int)d;
        } else if (name_eq(kn, klen, "version")) {
            if ((e = read_number(&c, &r->version))) return e;
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
            if ((e = read_number(&c, &r->diff_count))) return e;
        } else if (name_eq(kn, klen, "diffsPerSnapshot")) {
            if ((e = read_number(&c, &d))) return e;
            r->is_metadata = 1; r->diffs_per_snapshot = (int)d;
        } else {
            if ((e = skip_value(&c))) return e;
        }
    }
    return BJ_OK;
}

/* ---- Log state ------------------------------------------------------ */

struct textlog {
    dbuf        img;             /* file image                          */
    dbuf        out;             /* last read output                    */
    bj_builder *bld;             /* reused for entry/metadata encoding  */
    double      version;
    double      diff_count;
    int         diffs_per_snapshot;
    int         has_snapshot; double snapshot_ptr;
    int         has_latest;   double latest_ptr;
};

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
    return dbuf_put(&t->img, d, len);
}

static int encode_entry(textlog *t, int type, double version,
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
    if (t->has_snapshot) bj_put_pointer(b, (uint64_t)t->snapshot_ptr); else bj_put_null(b);
    bj_put_key(b, (const uint8_t *)"latestPointer", 13);
    if (t->has_latest) bj_put_pointer(b, (uint64_t)t->latest_ptr); else bj_put_null(b);
    bj_put_key(b, (const uint8_t *)"diffCount", 9);
    bj_put_int(b, (int64_t)t->diff_count);
    bj_put_key(b, (const uint8_t *)"diffsPerSnapshot", 16);
    bj_put_int(b, t->diffs_per_snapshot);
    bj_end_object(b);
    return append_builder(t);
}

/* ---- Version reconstruction ----------------------------------------- */

static int diff_err_to_bj(int e) {
    return (e == DIFF_ERR_OOM) ? BJ_ERR_OOM : BJ_ERR_STATE;
}

/*
 * Rebuild the full text of `version` into *text (reset first): start from the
 * latest snapshot at or before `version` and apply each subsequent DIFF entry's
 * patch in order, exactly like textlog.js getVersion.
 */
static int reconstruct_version(textlog *t, double version, dbuf *text) {
    text->len = 0;
    double last_snap = -1;
    size_t off = 0;
    int e = BJ_OK;
    while (off < t->img.len) {
        size_t sz;
        e = bj_value_size(t->img.data, t->img.len, off, &sz);
        if (e) break;
        trec r;
        e = parse_record(t->img.data, t->img.len, off, &r);
        if (e) break;
        off += sz;
        if (!r.is_entry || r.version > version) continue;
        if (r.type == TL_FULL_SNAPSHOT) {
            text->len = 0;
            if ((e = dbuf_put(text, r.data, r.datalen))) break;
            last_snap = r.version;
        } else if (r.type == TL_DIFF && last_snap >= 1 && r.version > last_snap) {
            uint8_t *nt = NULL; size_t ntl = 0; int applied = 0;
            int de = diff_apply_patch(text->data, text->len, r.data, r.datalen, &nt, &ntl, &applied);
            if (de) { e = diff_err_to_bj(de); break; }
            if (!applied) { free(nt); e = BJ_ERR_STATE; break; }
            text->len = 0;
            e = dbuf_put(text, nt, ntl);
            free(nt);
            if (e) break;
        }
    }
    return e;
}

/* ---- addVersion ----------------------------------------------------- */

int textlog_add_version(textlog *t, const uint8_t *text, uint32_t text_len,
                        int64_t ts_ms, double *out_version) {
    uint8_t hash[64];
    sha256_hex(text, text_len, hash);

    double new_version = t->version + 1;
    int should_snapshot = (t->diff_count >= t->diffs_per_snapshot) || !t->has_latest;
    size_t offset = t->img.len;
    int e;

    if (should_snapshot) {
        e = encode_entry(t, TL_FULL_SNAPSHOT, new_version, hash, 64, text, text_len, ts_ms);
        if (e) return e;
        t->diff_count = 0;
        t->has_snapshot = 1;
        t->snapshot_ptr = (double)offset;
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
    }

    t->has_latest = 1;
    t->latest_ptr = (double)offset;
    t->version = new_version;

    e = save_metadata(t);
    if (e) return e;
    if (out_version) *out_version = new_version;
    return BJ_OK;
}

int textlog_get_version(textlog *t, double version,
                        const uint8_t **out_ptr, size_t *out_len) {
    dbuf text; memset(&text, 0, sizeof(text));
    int e = reconstruct_version(t, version, &text);
    if (!e) e = set_out(t, text.data, text.len);
    dbuf_free(&text);
    if (e) return e;
    *out_ptr = t->out.data; *out_len = t->out.len;
    return BJ_OK;
}

int textlog_get_version_hash(textlog *t, double version,
                             const uint8_t **out_ptr, size_t *out_len) {
    size_t off = 0;
    while (off < t->img.len) {
        size_t sz;
        int e = bj_value_size(t->img.data, t->img.len, off, &sz);
        if (e) return e;
        trec r;
        e = parse_record(t->img.data, t->img.len, off, &r);
        if (e) return e;
        off += sz;
        if (r.is_entry && r.version == version) {
            e = set_out(t, r.hash, r.hashlen);
            if (e) return e;
            *out_ptr = t->out.data; *out_len = t->out.len;
            return BJ_OK;
        }
    }
    return BJ_ERR_STATE; /* not found (host validates range, so unreachable) */
}

/* ---- getDiff -------------------------------------------------------- */

int textlog_get_diff(textlog *t, double from_version, double to_version,
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

textlog *textlog_create(int diffs_per_snapshot) {
    if (diffs_per_snapshot < 1) return NULL;
    textlog *t = (textlog *)calloc(1, sizeof(textlog));
    if (!t) return NULL;
    t->bld = bj_builder_new();
    if (!t->bld) { free(t); return NULL; }
    t->version = 0;
    t->diff_count = 0;
    t->diffs_per_snapshot = diffs_per_snapshot;
    t->has_snapshot = 0;
    t->has_latest = 0;
    if (save_metadata(t)) { textlog_free(t); return NULL; }
    return t;
}

textlog *textlog_load(const uint8_t *bytes, size_t len) {
    textlog *t = (textlog *)calloc(1, sizeof(textlog));
    if (!t) return NULL;
    t->bld = bj_builder_new();
    if (!t->bld) { free(t); return NULL; }
    if (len) {
        t->img.data = (uint8_t *)malloc(len);
        if (!t->img.data) { textlog_free(t); return NULL; }
        memcpy(t->img.data, bytes, len);
        t->img.len = len;
        t->img.cap = len;
    }

    /* Find the last metadata record. */
    int found = 0;
    size_t off = 0;
    while (off < t->img.len) {
        size_t sz;
        if (bj_value_size(t->img.data, t->img.len, off, &sz)) break;
        trec r;
        if (parse_record(t->img.data, t->img.len, off, &r)) break;
        off += sz;
        if (r.is_metadata) {
            t->version = r.version;
            t->has_snapshot = r.has_snap; t->snapshot_ptr = r.snap;
            t->has_latest = r.has_latest; t->latest_ptr = r.latest;
            t->diff_count = r.diff_count;
            t->diffs_per_snapshot = r.diffs_per_snapshot;
            found = 1;
        }
    }
    if (!found) { textlog_free(t); return NULL; }
    return t;
}

void textlog_free(textlog *t) {
    if (!t) return;
    bj_builder_free(t->bld);
    free(t->img.data);
    free(t->out.data);
    free(t);
}

double         textlog_version(const textlog *t)            { return t->version; }
int            textlog_diffs_per_snapshot(const textlog *t) { return t->diffs_per_snapshot; }
const uint8_t *textlog_image(const textlog *t, size_t *len) { if (len) *len = t->img.len; return t->img.data; }
const uint8_t *textlog_out(const textlog *t, size_t *len)   { if (len) *len = t->out.len; return t->out.data; }
