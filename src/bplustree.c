/*
 * bplustree.c — C port of src/bplustree.js. See bplustree.h.
 *
 * The tree is persistent, append-only and immutable: every mutation appends new
 * nodes (and fresh metadata) to the backing file and re-points the root,
 * exactly like the reference. Nodes/metadata use the binjson wire format from
 * binjson.c so the on-disk bytes match the JS implementation.
 *
 * All file access goes through bjfile (bjfile.h): reads fetch one record at a
 * time, and each mutating operation's appends are committed with a single host
 * write when the operation succeeds (or dropped whole when it fails).
 */
#include "bplustree.h"
#include "bjfile.h"
#include "bjcursor.h"
#include "dbuf.h"

#include <stdlib.h>
#include <string.h>
#include <math.h>

/* Fixed on-wire size of the metadata object (matches METADATA_SIZE in
 * bplustree.js): 6 fields, all fixed-width INT/POINTER values. */
#define BPT_METADATA_SIZE 135

/* Cap on tree depth while walking file-provided pointers: a corrupt file
 * whose child pointer loops back to an ancestor must error, not recurse
 * forever. Vastly deeper than any real tree (height is O(log n)). */
#define BPT_MAX_DEPTH 128

/*
 * Values larger than this are stored out-of-line: appended to the file once
 * as their own record, with the leaf holding a small marker instead of the
 * raw bytes. Every leaf rewrite (any insert/delete touching that leaf)
 * otherwise re-copies every sibling value on disk — order 32 with 1 KB
 * values costs ~32 KB of unchanged bytes per insert. 256 bytes keeps
 * ordinary small values (numbers, short strings, small documents) inline,
 * where the marker's own overhead wouldn't pay for itself.
 */
#define BPT_OOL_THRESHOLD 256

/* Out-of-line marker key: { "\0ool": Pointer(offset) }. The leading NUL
 * makes collision with a caller's real value astronomically unlikely (no
 * ordinary JS object key contains one) without spending a new wire type —
 * the same reserved-key convention textindex uses for its stats key. Marker
 * records are always small (well under the threshold above), so a real
 * value is never mistaken for one by size alone. */
static const uint8_t BPT_OOL_KEY[] = { 0x00, 'o', 'o', 'l' };
#define BPT_OOL_KEY_LEN 4

/* ---- Values, keys, nodes -------------------------------------------- */

typedef struct { uint8_t *bytes; uint32_t len; } bpt_blob;

typedef struct {
    uint64_t   id;
    int        is_leaf;
    int        n_keys;
    bpt_key   *keys;        /* n_keys                                      */
    int        n_values;
    bpt_blob  *values;      /* n_values (== n_keys on leaves)              */
    int        n_children;
    uint64_t  *children;    /* n_children (pointer offsets)                */
    uint8_t   *buf;         /* parsed nodes: one owned copy of the record
                               that keys/values point into, making parsing
                               O(1) allocations. NULL on built nodes, whose
                               keys/values own their bytes individually.   */
} bpt_node;

struct bpt {
    bjfile     f;                                     /* backing file      */
    dbuf       out;                                   /* last op output    */
    bj_builder *bld;                                  /* reused for saves  */
    uint64_t   root;
    uint64_t   next_id;
    int64_t    size;
    int        order;
    int        min_keys;
    int        read_only;   /* snapshot handle: mutations disabled */
};

/* ---- Key helpers ---------------------------------------------------- */

/* Mirrors JS comparison for numbers (numeric) and strings (lexicographic by
 * bytes). Differing types get a stable order (number < string); trees never mix
 * key types in practice. */
static int key_cmp(const bpt_key *a, const bpt_key *b) {
    if (a->is_string != b->is_string) return a->is_string ? 1 : -1;
    if (a->is_string) {
        uint32_t n = a->str_len < b->str_len ? a->str_len : b->str_len;
        int r = n ? memcmp(a->str, b->str, n) : 0;
        if (r) return r < 0 ? -1 : 1;
        if (a->str_len < b->str_len) return -1;
        if (a->str_len > b->str_len) return 1;
        return 0;
    }
    if (a->num < b->num) return -1;
    if (a->num > b->num) return 1;
    return 0;
}
static int key_eq(const bpt_key *a, const bpt_key *b) { return key_cmp(a, b) == 0; }

/*
 * Keys within a node are strictly ascending, so positions are found by
 * binary search. upper_bound: first index with keys[i] > key — the child
 * index the equal-keys-route-right descent rule selects. lower_bound:
 * first index with keys[i] >= key — the insert position, and the index of
 * an exact match when one exists.
 */
static int key_upper_bound(const bpt_key *keys, int n, const bpt_key *key) {
    int lo = 0, hi = n;
    while (lo < hi) {
        int mid = lo + (hi - lo) / 2;
        if (key_cmp(key, &keys[mid]) >= 0) lo = mid + 1;
        else hi = mid;
    }
    return lo;
}
static int key_lower_bound(const bpt_key *keys, int n, const bpt_key *key) {
    int lo = 0, hi = n;
    while (lo < hi) {
        int mid = lo + (hi - lo) / 2;
        if (key_cmp(key, &keys[mid]) > 0) lo = mid + 1;
        else hi = mid;
    }
    return lo;
}
/* Index of the exact match, or -1. */
static int key_find(const bpt_key *keys, int n, const bpt_key *key) {
    int i = key_lower_bound(keys, n, key);
    return (i < n && key_eq(key, &keys[i])) ? i : -1;
}

/*
 * NaN compares equal to everything in key_cmp (both < checks are false), so
 * a NaN key would silently overwrite or match arbitrary entries. Stored
 * keys must be finite; range/cursor bounds only exclude NaN — ±infinity is
 * a legitimate unbounded query.
 */
static int key_storable(const bpt_key *k) {
    return k->is_string || isfinite(k->num);
}
static int bound_valid(const bpt_key *k) {
    return k->is_string || !isnan(k->num);
}

static int key_copy(bpt_key *dst, const bpt_key *src) {
    dst->is_string = src->is_string;
    dst->num = src->num;
    dst->str = NULL;
    dst->str_len = 0;
    if (src->is_string) {
        uint8_t *s = (uint8_t *)malloc(src->str_len ? src->str_len : 1);
        if (!s) return BJ_ERR_OOM;
        if (src->str_len) memcpy(s, src->str, src->str_len);
        dst->str = s;
        dst->str_len = src->str_len;
    }
    return BJ_OK;
}
static void key_free(bpt_key *k) {
    if (k->is_string) free((void *)k->str);
    k->str = NULL;
    k->is_string = 0;
}

static int blob_copy(bpt_blob *dst, const uint8_t *b, uint32_t n) {
    uint8_t *p = (uint8_t *)malloc(n ? n : 1);
    if (!p) return BJ_ERR_OOM;
    if (n) memcpy(p, b, n);
    dst->bytes = p;
    dst->len = n;
    return BJ_OK;
}
static void blob_free(bpt_blob *b) { free(b->bytes); b->bytes = NULL; }

/* ---- Node lifecycle ------------------------------------------------- */

static void node_init(bpt_node *n) { memset(n, 0, sizeof(*n)); }

static void node_free(bpt_node *n) {
    if (!n->buf) {   /* built nodes own each key string and value blob */
        for (int i = 0; i < n->n_keys; i++) key_free(&n->keys[i]);
        for (int i = 0; i < n->n_values; i++) blob_free(&n->values[i]);
    }
    free(n->keys);
    free(n->values);
    free(n->children);
    free(n->buf);
    node_init(n);
}

/* Build a leaf/internal node by deep-copying the supplied arrays. */
static int node_build_leaf(bpt_node *out, uint64_t id, const bpt_key *keys,
                           const bpt_blob *vals, int n) {
    node_init(out);
    out->id = id; out->is_leaf = 1;
    if (n) {
        out->keys = (bpt_key *)malloc((size_t)n * sizeof(bpt_key));
        out->values = (bpt_blob *)malloc((size_t)n * sizeof(bpt_blob));
        if (!out->keys || !out->values) return BJ_ERR_OOM;
        for (int i = 0; i < n; i++) {
            int e = key_copy(&out->keys[i], &keys[i]);
            if (e) { out->n_keys = i; return e; }
            e = blob_copy(&out->values[i], vals[i].bytes, vals[i].len);
            if (e) { out->n_keys = i + 1; out->n_values = i; return e; }
        }
    }
    out->n_keys = n; out->n_values = n;
    return BJ_OK;
}

static int node_build_internal(bpt_node *out, uint64_t id, const bpt_key *keys,
                               int nk, const uint64_t *children, int nc) {
    node_init(out);
    out->id = id; out->is_leaf = 0;
    if (nk) {
        out->keys = (bpt_key *)malloc((size_t)nk * sizeof(bpt_key));
        if (!out->keys) return BJ_ERR_OOM;
        for (int i = 0; i < nk; i++) {
            int e = key_copy(&out->keys[i], &keys[i]);
            if (e) { out->n_keys = i; return e; }
        }
    }
    out->n_keys = nk;
    if (nc) {
        out->children = (uint64_t *)malloc((size_t)nc * sizeof(uint64_t));
        if (!out->children) return BJ_ERR_OOM;
        memcpy(out->children, children, (size_t)nc * sizeof(uint64_t));
    }
    out->n_children = nc;
    return BJ_OK;
}

/* ---- File append & output buffer ------------------------------------ */

static int file_append_to(bjfile *dst, const uint8_t *b, size_t n, uint64_t *off) {
    return bjfile_append(dst, b, n, off);
}
static int set_out(bpt *t, const uint8_t *b, size_t n) {
    t->out.len = 0;
    return dbuf_put(&t->out, b, n);
}

/* ---- Wire-format readers (structure-specific; primitives in bjcursor.h) */

/* Read a key as a *view*: string bytes point into the cursor's buffer (the
 * parsed node's owned record copy), so no allocation happens per key. */
static int read_key(cur *c, bpt_key *out) {
    uint8_t t;
    if (take_type(c, &t)) return BJ_ERR_EOF;
    out->str = NULL; out->str_len = 0; out->num = 0; out->is_string = 0;
    if (t == BJ_TYPE_INT) {
        if (cur_need(c, 8)) return BJ_ERR_EOF;
        int64_t v = (int64_t)rdu64(c->d + c->pos);
        c->pos += 8; out->num = (double)v; return BJ_OK;
    }
    if (t == BJ_TYPE_FLOAT) {
        if (cur_need(c, 8)) return BJ_ERR_EOF;
        uint64_t bits = rdu64(c->d + c->pos);
        c->pos += 8; memcpy(&out->num, &bits, 8); return BJ_OK;
    }
    if (t == BJ_TYPE_STRING) {
        uint32_t n;
        if (take_u32(c, &n)) return BJ_ERR_EOF;
        if (cur_need(c, n)) return BJ_ERR_EOF;
        out->is_string = 1; out->str = c->d + c->pos; out->str_len = n;
        c->pos += n;
        return BJ_OK;
    }
    return BJ_ERR_UNKNOWN_TYPE;
}

/* ---- Node (de)serialization ----------------------------------------- */

/*
 * Decode the node object stored at `offset` in the file into `out`. The
 * record bytes are copied once into the node's own buffer and every key and
 * value is a view into it: parsing costs a fixed handful of allocations
 * instead of one malloc+memcpy per key and per value, node contents stay
 * valid across later reads and appends (bjfile reuses its read buffer and
 * may realloc the pending buffer), and node_free releases the one buffer.
 */
static int parse_node(bpt *t, uint64_t offset, bpt_node *out) {
    node_init(out);
    const uint8_t *rec; size_t rec_len;
    int err = bjfile_read_record(&t->f, offset, &rec, &rec_len);
    if (err) return err;
    out->buf = (uint8_t *)malloc(rec_len ? rec_len : 1);
    if (!out->buf) return BJ_ERR_OOM;
    memcpy(out->buf, rec, rec_len);
    cur c = { out->buf, rec_len, 0 };

    uint8_t type;
    if (take_type(&c, &type) || type != BJ_TYPE_OBJECT) return BJ_ERR_STATE;
    uint32_t size, count;
    if (take_u32(&c, &size)) return BJ_ERR_EOF;
    if (take_u32(&c, &count)) return BJ_ERR_EOF;

    for (uint32_t i = 0; i < count; i++) {
        uint32_t klen;
        if (take_u32(&c, &klen)) { node_free(out); return BJ_ERR_EOF; }
        if (cur_need(&c, klen)) { node_free(out); return BJ_ERR_EOF; }
        const uint8_t *kn = c.d + c.pos;
        c.pos += klen;
        int e = BJ_OK;
        if (name_eq(kn, klen, "id")) {
            e = read_u64(&c, &out->id);
        } else if (name_eq(kn, klen, "isLeaf")) {
            e = read_bool(&c, &out->is_leaf);
        } else if (name_eq(kn, klen, "keys")) {
            uint32_t n;
            if ((e = array_begin(&c, &n))) { node_free(out); return e; }
            if (n) {
                out->keys = (bpt_key *)calloc(n, sizeof(bpt_key));
                if (!out->keys) { node_free(out); return BJ_ERR_OOM; }
            }
            for (uint32_t j = 0; j < n; j++) {
                if ((e = read_key(&c, &out->keys[j]))) { out->n_keys = j; node_free(out); return e; }
            }
            out->n_keys = (int)n;
        } else if (name_eq(kn, klen, "values")) {
            uint32_t n;
            if ((e = array_begin(&c, &n))) { node_free(out); return e; }
            if (n) {
                out->values = (bpt_blob *)calloc(n, sizeof(bpt_blob));
                if (!out->values) { node_free(out); return BJ_ERR_OOM; }
            }
            for (uint32_t j = 0; j < n; j++) {
                size_t sz;
                if ((e = bj_value_size(c.d, c.len, c.pos, &sz))) { out->n_values = j; node_free(out); return e; }
                if (cur_need(&c, sz)) { out->n_values = j; node_free(out); return BJ_ERR_EOF; }
                out->values[j].bytes = out->buf + c.pos;   /* view */
                out->values[j].len = (uint32_t)sz;
                c.pos += sz;
            }
            out->n_values = (int)n;
        } else if (name_eq(kn, klen, "children")) {
            uint32_t n;
            if ((e = array_begin(&c, &n))) { node_free(out); return e; }
            if (n) {
                out->children = (uint64_t *)malloc((size_t)n * sizeof(uint64_t));
                if (!out->children) { node_free(out); return BJ_ERR_OOM; }
            }
            for (uint32_t j = 0; j < n; j++) {
                if ((e = read_pointer(&c, &out->children[j]))) { node_free(out); return e; }
            }
            out->n_children = (int)n;
        } else {
            /* Unknown key — including the legacy leaf "next" pointer, which is
             * no longer written or used: it is dead weight (inserts wrote it
             * null, deletes left a stale sibling pointer, nothing followed it).
             * skip_value consumes whatever a legacy file stored there. */
            e = skip_value(&c);
        }
        if (e) { node_free(out); return e; }
    }
    /* Structural invariants every writer (JS and C) maintains. A violating
     * node is a corrupt or hostile file and would otherwise cause
     * out-of-bounds child/value indexing in the traversals. */
    if (out->is_leaf ? (out->n_values != out->n_keys)
                     : (out->n_children != out->n_keys + 1)) {
        node_free(out);
        return BJ_ERR_STATE;
    }
    return BJ_OK;
}

static int emit_key(bj_builder *b, const bpt_key *k) {
    if (k->is_string) return bj_put_string(b, k->str, k->str_len);
    if (is_safe_int(k->num)) return bj_put_int(b, (int64_t)k->num);
    return bj_put_float(b, k->num);
}

/* ---- Out-of-line values (see BPT_OOL_THRESHOLD above) ---------------- */

/* True if `blob` is an out-of-line marker; writes the target offset to
 * *off. A real value only ever matches this by deliberately constructing
 * the exact reserved shape (see BPT_OOL_KEY). */
static int blob_is_ool(const bpt_blob *blob, uint64_t *off) {
    cur c = { blob->bytes, blob->len, 0 };
    uint32_t count;
    if (object_begin(&c, &count) || count != 1) return 0;
    const uint8_t *kn; uint32_t klen;
    if (take_key(&c, &kn, &klen)) return 0;
    if (klen != BPT_OOL_KEY_LEN || memcmp(kn, BPT_OOL_KEY, BPT_OOL_KEY_LEN) != 0) return 0;
    return read_pointer(&c, off) == BJ_OK;
}

/*
 * Resolve a leaf value for output. An inline blob is returned as a view
 * (no copy, as before); an out-of-line marker is dereferenced through
 * `src`, returning a view into its transient read buffer — valid only
 * until the next read on `src`, so the caller must consume it (copy into
 * an output buffer, or bj_put_raw into a builder) before triggering
 * another one.
 */
static int resolve_value(bjfile *src, const bpt_blob *blob,
                         const uint8_t **out_ptr, size_t *out_len) {
    uint64_t off;
    if (!blob_is_ool(blob, &off)) {
        *out_ptr = blob->bytes; *out_len = blob->len;
        return BJ_OK;
    }
    return bjfile_read_record(src, off, out_ptr, out_len);
}

/*
 * Store a value for a leaf entry, applying the out-of-line threshold: small
 * values are copied inline as an owned blob (unchanged behavior); values
 * over BPT_OOL_THRESHOLD are appended to `dst` once and the leaf holds a
 * small marker instead — the whole fix for the write-amplification problem,
 * since every later leaf rewrite that merely carries this entry along now
 * copies the marker, not the value. `dst` is the live file for a normal add,
 * or a compaction's destination file. Returns an owned blob (a copy of the
 * value, or the built marker) the caller must blob_free after use.
 */
static int store_value(bpt *t, bjfile *dst, const uint8_t *val, uint32_t vlen,
                       bpt_blob *out) {
    if (vlen <= BPT_OOL_THRESHOLD) return blob_copy(out, val, vlen);
    uint64_t off;
    int e = file_append_to(dst, val, vlen, &off);
    if (e) return e;
    bj_builder *b = t->bld;
    bj_builder_reset(b);
    bj_begin_object(b);
    bj_put_key(b, BPT_OOL_KEY, BPT_OOL_KEY_LEN);
    bj_put_pointer(b, off);
    bj_end_object(b);
    e = bj_builder_error(b);
    if (e) return e;
    size_t mlen;
    const uint8_t *m = bj_builder_data(b, &mlen);
    if (!m) return BJ_ERR_STATE;
    return blob_copy(out, m, mlen);
}

/* Encode `nd` and append it to `dst`; return its offset via *off. */
static int encode_node_to(bpt *t, const bpt_node *nd, bjfile *dst, uint64_t *off) {
    bj_builder *b = t->bld;
    bj_builder_reset(b);
    bj_begin_object(b);
    bj_put_key(b, (const uint8_t *)"id", 2);        bj_put_int(b, (int64_t)nd->id);
    bj_put_key(b, (const uint8_t *)"isLeaf", 6);    bj_put_bool(b, nd->is_leaf);
    bj_put_key(b, (const uint8_t *)"keys", 4);
    bj_begin_array(b);
    for (int i = 0; i < nd->n_keys; i++) emit_key(b, &nd->keys[i]);
    bj_end_array(b);
    bj_put_key(b, (const uint8_t *)"values", 6);
    bj_begin_array(b);
    for (int i = 0; i < nd->n_values; i++) bj_put_raw(b, nd->values[i].bytes, nd->values[i].len);
    bj_end_array(b);
    bj_put_key(b, (const uint8_t *)"children", 8);
    bj_begin_array(b);
    for (int i = 0; i < nd->n_children; i++) bj_put_pointer(b, nd->children[i]);
    bj_end_array(b);
    bj_end_object(b);

    int e = bj_builder_error(b);
    if (e) return e;
    size_t len;
    const uint8_t *d = bj_builder_data(b, &len);
    if (!d) return BJ_ERR_STATE;
    return file_append_to(dst, d, len, off);
}

/* Append `nd` to the tree's live file. */
static int save_node(bpt *t, const bpt_node *nd, uint64_t *off) {
    return encode_node_to(t, nd, &t->f, off);
}

/* ---- Metadata ------------------------------------------------------- */

static int encode_metadata_to(bpt *t, bjfile *dst, uint64_t root,
                              uint64_t next_id, int64_t size) {
    bj_builder *b = t->bld;
    bj_builder_reset(b);
    bj_begin_object(b);
    bj_put_key(b, (const uint8_t *)"version", 7);      bj_put_int(b, 1);
    bj_put_key(b, (const uint8_t *)"maxEntries", 10);  bj_put_int(b, t->order);
    bj_put_key(b, (const uint8_t *)"minEntries", 10);  bj_put_int(b, t->min_keys);
    bj_put_key(b, (const uint8_t *)"size", 4);         bj_put_int(b, size);
    bj_put_key(b, (const uint8_t *)"rootPointer", 11); bj_put_pointer(b, root);
    bj_put_key(b, (const uint8_t *)"nextId", 6);       bj_put_int(b, (int64_t)next_id);
    bj_end_object(b);

    int e = bj_builder_error(b);
    if (e) return e;
    size_t len;
    const uint8_t *d = bj_builder_data(b, &len);
    if (!d) return BJ_ERR_STATE;
    /* Metadata ends every commit; a CRC trailer written just before it covers
     * all of the operation's appended bytes (bjfile_append_protected). */
    return bjfile_append_protected(dst, d, len);
}

static int save_metadata(bpt *t) {
    return encode_metadata_to(t, &t->f, t->root, t->next_id, t->size);
}

typedef struct {
    uint64_t root, next_id;
    int64_t  size;
    int      order, min_keys;
    int      have_root;
} bpt_meta;

/* Parse a metadata record's fields out of its bytes. */
static int parse_meta_rec(const uint8_t *rec, size_t rec_len, bpt_meta *m) {
    memset(m, 0, sizeof(*m));
    cur c = { rec, rec_len, 0 };
    uint8_t type;
    if (take_type(&c, &type) || type != BJ_TYPE_OBJECT) return BJ_ERR_STATE;
    uint32_t size, count;
    if (take_u32(&c, &size)) return BJ_ERR_EOF;
    if (take_u32(&c, &count)) return BJ_ERR_EOF;

    for (uint32_t i = 0; i < count; i++) {
        uint32_t klen;
        if (take_u32(&c, &klen)) return BJ_ERR_EOF;
        if (cur_need(&c, klen)) return BJ_ERR_EOF;
        const uint8_t *kn = c.d + c.pos;
        c.pos += klen;
        int e = BJ_OK;
        uint64_t u;
        if (name_eq(kn, klen, "maxEntries"))      { if ((e = read_int31(&c, &m->order))) return e; }
        else if (name_eq(kn, klen, "minEntries")) { if ((e = read_int31(&c, &m->min_keys))) return e; }
        else if (name_eq(kn, klen, "size"))       { if ((e = read_u64(&c, &u))) return e; m->size = (int64_t)u; }
        else if (name_eq(kn, klen, "nextId"))     { if ((e = read_u64(&c, &m->next_id))) return e; }
        else if (name_eq(kn, klen, "rootPointer")){ if ((e = read_pointer(&c, &m->root))) return e; m->have_root = 1; }
        else                                      { if ((e = skip_value(&c))) return e; }
    }
    return m->have_root ? BJ_OK : BJ_ERR_STATE;
}

/* Range-check metadata fields. `before` is the metadata record's own offset:
 * the root it points at must lie strictly before it. */
static int meta_valid(const bpt_meta *m, uint64_t before) {
    if (m->order < 3) return 0;
    if (m->min_keys < 1 || m->min_keys >= m->order) return 0;
    if (m->size < 0) return 0;
    if (m->root >= before) return 0;
    return 1;
}

static void meta_apply(bpt *t, const bpt_meta *m) {
    t->order = m->order;
    t->min_keys = m->min_keys;
    t->size = m->size;
    t->next_id = m->next_id;
    t->root = m->root;
}

/* ---- Insert (mirrors _addToNode / add in bplustree.js) -------------- */

typedef struct {
    int      is_split;
    int      updated;     /* 1 if an existing key's value was replaced   */
    bpt_node newn;        /* when !is_split                              */
    bpt_node left, right; /* when is_split (not yet saved)               */
    bpt_key  split_key;   /* when is_split                               */
} add_res;

static int add_node(bpt *t, uint64_t ptr, const bpt_key *key,
                    const uint8_t *val, uint32_t vlen, add_res *out, int depth) {
    memset(out, 0, sizeof(*out));
    if (depth > BPT_MAX_DEPTH) return BJ_ERR_DEPTH;
    bpt_node nd;
    int e = parse_node(t, ptr, &nd);
    if (e) return e;

    if (nd.is_leaf) {
        int insertIdx = key_lower_bound(nd.keys, nd.n_keys, key);
        int idx = (insertIdx < nd.n_keys && key_eq(key, &nd.keys[insertIdx]))
                      ? insertIdx : -1;

        if (idx >= 0) {
            /* Update existing key's value. */
            bpt_blob *wv = (bpt_blob *)malloc((size_t)nd.n_keys * sizeof(bpt_blob));
            if (!wv) { node_free(&nd); return BJ_ERR_OOM; }
            for (int i = 0; i < nd.n_keys; i++) wv[i] = nd.values[i];
            bpt_blob nb;
            e = store_value(t, &t->f, val, vlen, &nb);
            if (e) { free(wv); node_free(&nd); return e; }
            wv[idx] = nb;
            e = node_build_leaf(&out->newn, nd.id, nd.keys, wv, nd.n_keys);
            blob_free(&nb);
            free(wv);
            out->is_split = 0;
            out->updated = 1;
            node_free(&nd);
            return e;
        }

        int nlen = nd.n_keys + 1;
        bpt_key *wk = (bpt_key *)malloc((size_t)nlen * sizeof(bpt_key));
        bpt_blob *wv = (bpt_blob *)malloc((size_t)nlen * sizeof(bpt_blob));
        if (!wk || !wv) { free(wk); free(wv); node_free(&nd); return BJ_ERR_OOM; }
        for (int i = 0; i < insertIdx; i++) { wk[i] = nd.keys[i]; wv[i] = nd.values[i]; }
        wk[insertIdx] = *key;
        bpt_blob nb;
        e = store_value(t, &t->f, val, vlen, &nb);
        if (e) { free(wk); free(wv); node_free(&nd); return e; }
        wv[insertIdx] = nb;
        for (int i = insertIdx; i < nd.n_keys; i++) { wk[i + 1] = nd.keys[i]; wv[i + 1] = nd.values[i]; }

        if (nlen < t->order) {
            e = node_build_leaf(&out->newn, nd.id, wk, wv, nlen);
            out->is_split = 0;
        } else {
            int mid = (nlen + 1) / 2;   /* ceil(nlen/2) */
            e = node_build_leaf(&out->left, nd.id, wk, wv, mid);
            if (!e) e = node_build_leaf(&out->right, t->next_id++, wk + mid, wv + mid, nlen - mid);
            if (!e) e = key_copy(&out->split_key, &wk[mid]);
            out->is_split = 1;
        }
        blob_free(&nb);
        free(wk); free(wv);
        node_free(&nd);
        return e;
    }

    /* Internal node. */
    int childIdx = key_upper_bound(nd.keys, nd.n_keys, key);

    add_res cr;
    e = add_node(t, nd.children[childIdx], key, val, vlen, &cr, depth + 1);
    if (e) { node_free(&nd); return e; }

    if (!cr.is_split) {
        uint64_t ncp;
        e = save_node(t, &cr.newn, &ncp);
        node_free(&cr.newn);
        if (e) { node_free(&nd); return e; }
        uint64_t *wc = (uint64_t *)malloc((size_t)nd.n_children * sizeof(uint64_t));
        if (!wc) { node_free(&nd); return BJ_ERR_OOM; }
        memcpy(wc, nd.children, (size_t)nd.n_children * sizeof(uint64_t));
        wc[childIdx] = ncp;
        e = node_build_internal(&out->newn, nd.id, nd.keys, nd.n_keys, wc, nd.n_children);
        free(wc);
        out->is_split = 0;
        out->updated = cr.updated;
        node_free(&nd);
        return e;
    }

    /* Child split: save both halves, splice splitKey + the two child pointers. */
    uint64_t lp, rp;
    e = save_node(t, &cr.left, &lp);
    if (!e) e = save_node(t, &cr.right, &rp);
    node_free(&cr.left); node_free(&cr.right);
    if (e) { key_free(&cr.split_key); node_free(&nd); return e; }

    int nkeys = nd.n_keys + 1;
    int nch = nd.n_children + 1;
    bpt_key  *wk = (bpt_key *)malloc((size_t)nkeys * sizeof(bpt_key));
    uint64_t *wc = (uint64_t *)malloc((size_t)nch * sizeof(uint64_t));
    if (!wk || !wc) { free(wk); free(wc); key_free(&cr.split_key); node_free(&nd); return BJ_ERR_OOM; }
    for (int i = 0; i < childIdx; i++) wk[i] = nd.keys[i];
    wk[childIdx] = cr.split_key;
    for (int i = childIdx; i < nd.n_keys; i++) wk[i + 1] = nd.keys[i];
    for (int i = 0; i < childIdx; i++) wc[i] = nd.children[i];
    wc[childIdx] = lp; wc[childIdx + 1] = rp;
    for (int i = childIdx + 1; i < nd.n_children; i++) wc[i + 1] = nd.children[i];

    if (nkeys < t->order) {
        e = node_build_internal(&out->newn, nd.id, wk, nkeys, wc, nch);
        out->is_split = 0;
    } else {
        int mid = (nkeys + 1) / 2 - 1;   /* ceil(nkeys/2) - 1 */
        e = node_build_internal(&out->left, nd.id, wk, mid, wc, mid + 1);
        if (!e) e = node_build_internal(&out->right, t->next_id++, wk + mid + 1, nkeys - mid - 1, wc + mid + 1, nch - (mid + 1));
        if (!e) e = key_copy(&out->split_key, &wk[mid]);
        out->is_split = 1;
    }
    free(wk); free(wc);
    key_free(&cr.split_key);
    node_free(&nd);
    return e;
}

static int add_root(bpt *t, const bpt_key *key, const uint8_t *val, uint32_t vlen) {
    add_res res;
    int e = add_node(t, t->root, key, val, vlen, &res, 0);
    if (e) return e;

    bpt_node new_root;
    if (!res.is_split) {
        new_root = res.newn;   /* take ownership */
    } else {
        uint64_t lp, rp;
        e = save_node(t, &res.left, &lp);
        if (!e) e = save_node(t, &res.right, &rp);
        node_free(&res.left); node_free(&res.right);
        if (e) { key_free(&res.split_key); return e; }
        uint64_t children[2] = { lp, rp };
        e = node_build_internal(&new_root, t->next_id++, &res.split_key, 1, children, 2);
        key_free(&res.split_key);
        if (e) { node_free(&new_root); return e; }
    }

    uint64_t rootp;
    e = save_node(t, &new_root, &rootp);
    node_free(&new_root);
    if (e) return e;
    t->root = rootp;
    /* Updating an existing key leaves the number of distinct keys unchanged. */
    if (!res.updated) t->size += 1;
    return save_metadata(t);
}

/*
 * Public mutating operations commit all of the operation's appended records
 * with a single host write; on any failure the pending bytes are dropped and
 * the in-memory state is rolled back, leaving the file untouched.
 */
int bpt_add(bpt *t, const bpt_key *key, const uint8_t *val, uint32_t vlen) {
    if (t->read_only || !key_storable(key)) return BJ_ERR_STATE;
    uint64_t root = t->root, next_id = t->next_id;
    int64_t size = t->size;
    int e = add_root(t, key, val, vlen);
    if (!e) e = bjfile_commit(&t->f);
    if (e) {
        bjfile_discard(&t->f);
        t->root = root; t->next_id = next_id; t->size = size;
    }
    return e;
}

/* ---- Delete (mirrors _deleteFromNode / delete) ---------------------- */

typedef struct { int found; bpt_node node; } del_res;

/*
 * Child `i` of `par` fell below min_keys after a delete. Restore the
 * invariant against an adjacent sibling: concatenate the pair (internal
 * nodes pull the parent separator down between them) and, when the result
 * exceeds node capacity, split it back in two at the same midpoint insert
 * splits use. One path covers both classic cases — a rich sibling
 * redistributes (concatenate + split), a poor one merges outright — and
 * absorbs arbitrarily underfull nodes in files written before rebalancing
 * existed. `ch` is the child's new, not-yet-saved node; the parent's
 * replacement (also unsaved) is built into `out`.
 */
static int rebalance_child(bpt *t, const bpt_node *par, int i,
                           const bpt_node *ch, bpt_node *out) {
    int si = i > 0 ? i - 1 : i + 1;   /* adjacent sibling */
    int li = si < i ? si : i;         /* left node of the pair */
    bpt_node sib;
    int e = parse_node(t, par->children[si], &sib);
    if (e) return e;
    if (sib.is_leaf != ch->is_leaf) { node_free(&sib); return BJ_ERR_STATE; }
    const bpt_node *L = si < i ? &sib : ch;
    const bpt_node *R = si < i ? ch : &sib;

    /* Concatenated pair, as shallow scratch arrays (node_build_* deep-copies). */
    int leaf = ch->is_leaf;
    int nk = L->n_keys + R->n_keys + (leaf ? 0 : 1);
    int nc = L->n_children + R->n_children;
    bpt_key  *wk = (bpt_key *)malloc((size_t)(nk ? nk : 1) * sizeof(bpt_key));
    bpt_blob *wv = leaf ? (bpt_blob *)malloc((size_t)(nk ? nk : 1) * sizeof(bpt_blob)) : NULL;
    uint64_t *wc = leaf ? NULL : (uint64_t *)malloc((size_t)nc * sizeof(uint64_t));
    if (!wk || (leaf ? !wv : !wc)) {
        free(wk); free(wv); free(wc); node_free(&sib);
        return BJ_ERR_OOM;
    }
    int k = 0;
    for (int j = 0; j < L->n_keys; j++) wk[k++] = L->keys[j];
    if (!leaf) wk[k++] = par->keys[li];
    for (int j = 0; j < R->n_keys; j++) wk[k++] = R->keys[j];
    if (leaf) {
        for (int j = 0; j < L->n_keys; j++) wv[j] = L->values[j];
        for (int j = 0; j < R->n_keys; j++) wv[L->n_keys + j] = R->values[j];
    } else {
        memcpy(wc, L->children, (size_t)L->n_children * sizeof(uint64_t));
        memcpy(wc + L->n_children, R->children, (size_t)R->n_children * sizeof(uint64_t));
    }

    int split = nk >= t->order;
    uint64_t lp = 0, rp = 0;
    bpt_key promoted = {0};       /* owned separator copy when split */
    bpt_node nn;
    if (!split) {
        if (leaf) e = node_build_leaf(&nn, L->id, wk, wv, nk);
        else      e = node_build_internal(&nn, L->id, wk, nk, wc, nc);
        if (!e) e = save_node(t, &nn, &lp);
        node_free(&nn);
    } else if (leaf) {
        int mid = (nk + 1) / 2;   /* ceil(nk/2), as in insert */
        e = node_build_leaf(&nn, L->id, wk, wv, mid);
        if (!e) e = save_node(t, &nn, &lp);
        node_free(&nn);
        if (!e) {
            e = node_build_leaf(&nn, R->id, wk + mid, wv + mid, nk - mid);
            if (!e) e = save_node(t, &nn, &rp);
            node_free(&nn);
        }
        if (!e) e = key_copy(&promoted, &wk[mid]);
    } else {
        int mid = (nk + 1) / 2 - 1;   /* ceil(nk/2) - 1, as in insert */
        e = node_build_internal(&nn, L->id, wk, mid, wc, mid + 1);
        if (!e) e = save_node(t, &nn, &lp);
        node_free(&nn);
        if (!e) {
            e = node_build_internal(&nn, R->id, wk + mid + 1, nk - mid - 1,
                                    wc + mid + 1, nc - (mid + 1));
            if (!e) e = save_node(t, &nn, &rp);
            node_free(&nn);
        }
        if (!e) e = key_copy(&promoted, &wk[mid]);
    }
    free(wk); free(wv); free(wc);
    node_free(&sib);
    if (e) return e;

    /* Rebuild the parent: a merge drops separator li and one child; a split
     * replaces separator li and points at the two new halves. */
    int pnk = split ? par->n_keys : par->n_keys - 1;
    int pnc = split ? par->n_children : par->n_children - 1;
    bpt_key  *pk = (bpt_key *)malloc((size_t)(pnk ? pnk : 1) * sizeof(bpt_key));
    uint64_t *pc = (uint64_t *)malloc((size_t)pnc * sizeof(uint64_t));
    if (!pk || !pc) {
        free(pk); free(pc);
        if (split) key_free(&promoted);
        return BJ_ERR_OOM;
    }
    if (split) {
        for (int j = 0; j < par->n_keys; j++) pk[j] = par->keys[j];
        pk[li] = promoted;
        memcpy(pc, par->children, (size_t)par->n_children * sizeof(uint64_t));
        pc[li] = lp;
        pc[li + 1] = rp;
    } else {
        int m = 0;
        for (int j = 0; j < par->n_keys; j++) if (j != li) pk[m++] = par->keys[j];
        m = 0;
        for (int j = 0; j < par->n_children; j++) if (j != li + 1) pc[m++] = par->children[j];
        pc[li] = lp;
    }
    e = node_build_internal(out, par->id, pk, pnk, pc, pnc);
    free(pk); free(pc);
    if (split) key_free(&promoted);
    return e;
}

static int del_node(bpt *t, uint64_t ptr, const bpt_key *key, del_res *out,
                    int depth) {
    memset(out, 0, sizeof(*out));
    if (depth > BPT_MAX_DEPTH) return BJ_ERR_DEPTH;
    bpt_node nd;
    int e = parse_node(t, ptr, &nd);
    if (e) return e;

    if (nd.is_leaf) {
        int idx = key_find(nd.keys, nd.n_keys, key);
        if (idx < 0) { out->found = 0; node_free(&nd); return BJ_OK; }

        int nlen = nd.n_keys - 1;
        bpt_key *wk = nlen ? (bpt_key *)malloc((size_t)nlen * sizeof(bpt_key)) : NULL;
        bpt_blob *wv = nlen ? (bpt_blob *)malloc((size_t)nlen * sizeof(bpt_blob)) : NULL;
        if (nlen && (!wk || !wv)) { free(wk); free(wv); node_free(&nd); return BJ_ERR_OOM; }
        int k = 0;
        for (int i = 0; i < nd.n_keys; i++) {
            if (i == idx) continue;
            wk[k] = nd.keys[i]; wv[k] = nd.values[i]; k++;
        }
        e = node_build_leaf(&out->node, nd.id, wk, wv, nlen);
        free(wk); free(wv);
        out->found = 1;
        node_free(&nd);
        return e;
    }

    int i = key_upper_bound(nd.keys, nd.n_keys, key);
    del_res cr;
    e = del_node(t, nd.children[i], key, &cr, depth + 1);
    if (e) { node_free(&nd); return e; }
    if (!cr.found) { out->found = 0; node_free(&nd); return BJ_OK; }

    if (cr.node.n_keys < t->min_keys && nd.n_children >= 2) {
        /* Underflow: restore min_keys against a sibling instead of leaving
         * an ever-sparser node referenced forever. */
        e = rebalance_child(t, &nd, i, &cr.node, &out->node);
        node_free(&cr.node);
        node_free(&nd);
        if (e) return e;
        out->found = 1;
        return BJ_OK;
    }

    uint64_t ncp;
    e = save_node(t, &cr.node, &ncp);
    node_free(&cr.node);
    if (e) { node_free(&nd); return e; }
    uint64_t *wc = (uint64_t *)malloc((size_t)nd.n_children * sizeof(uint64_t));
    if (!wc) { node_free(&nd); return BJ_ERR_OOM; }
    memcpy(wc, nd.children, (size_t)nd.n_children * sizeof(uint64_t));
    wc[i] = ncp;
    e = node_build_internal(&out->node, nd.id, nd.keys, nd.n_keys, wc, nd.n_children);
    free(wc);
    out->found = 1;
    node_free(&nd);
    return e;
}

static int delete_root(bpt *t, const bpt_key *key) {
    del_res res;
    int e = del_node(t, t->root, key, &res, 0);
    if (e) return e;
    if (!res.found) return BJ_OK;   /* key not present: no-op */

    bpt_node final_root = res.node;   /* take ownership */
    /* Collapse empty internal roots down to their only child — merging the
     * root's last two children leaves it with one. Bounded like every other
     * walk over file-provided pointers. */
    for (int d = 0; final_root.n_keys == 0 && !final_root.is_leaf &&
                    final_root.n_children > 0; d++) {
        if (d > BPT_MAX_DEPTH) { node_free(&final_root); return BJ_ERR_DEPTH; }
        uint64_t child = final_root.children[0];
        node_free(&final_root);
        e = parse_node(t, child, &final_root);
        if (e) return e;
    }

    uint64_t rootp;
    e = save_node(t, &final_root, &rootp);
    node_free(&final_root);
    if (e) return e;
    t->root = rootp;
    t->size -= 1;
    return save_metadata(t);
}

int bpt_delete(bpt *t, const bpt_key *key) {
    if (t->read_only || !key_storable(key)) return BJ_ERR_STATE;
    uint64_t root = t->root, next_id = t->next_id;
    int64_t size = t->size;
    int e = delete_root(t, key);
    if (!e) e = bjfile_commit(&t->f);
    if (e) {
        bjfile_discard(&t->f);
        t->root = root; t->next_id = next_id; t->size = size;
    }
    return e;
}

/* ---- Search / traversal --------------------------------------------- */

int bpt_search(bpt *t, const bpt_key *key, int *found,
               const uint8_t **out_ptr, size_t *out_len) {
    if (!key_storable(key)) return BJ_ERR_STATE;
    uint64_t ptr = t->root;
    for (int depth = 0; ; depth++) {
        if (depth > BPT_MAX_DEPTH) return BJ_ERR_DEPTH;
        bpt_node nd;
        int e = parse_node(t, ptr, &nd);
        if (e) return e;
        if (nd.is_leaf) {
            int fi = key_find(nd.keys, nd.n_keys, key);
            if (fi >= 0) {
                /* One copy: straight from the resolved bytes (inline view,
                 * or the out-of-line record) into the output buffer. */
                const uint8_t *vp; size_t vl;
                e = resolve_value(&t->f, &nd.values[fi], &vp, &vl);
                if (!e) e = set_out(t, vp, vl);
                node_free(&nd);
                if (e) return e;
                *found = 1; *out_ptr = t->out.data; *out_len = t->out.len;
                return BJ_OK;
            }
            node_free(&nd);
            *found = 0;
            return BJ_OK;
        }
        int i = key_upper_bound(nd.keys, nd.n_keys, key);
        uint64_t child = nd.children[i];
        node_free(&nd);
        ptr = child;
    }
}

/*
 * In-order emit of { key, value } objects, optionally filtered to [mn, mx].
 * Internal nodes prune the descent with their routing keys: child i covers
 * [keys[i-1], keys[i]) (open ends at the edges), so only children whose span
 * can overlap the range are visited — a range scan reads O(height) nodes
 * plus the leaves that actually hold matches, not the whole tree.
 */
static int collect(bpt *t, uint64_t ptr, bj_builder *b, int filt,
                   const bpt_key *mn, const bpt_key *mx, int depth) {
    if (depth > BPT_MAX_DEPTH) return BJ_ERR_DEPTH;
    bpt_node nd;
    int e = parse_node(t, ptr, &nd);
    if (e) return e;
    if (nd.is_leaf) {
        for (int i = 0; i < nd.n_keys; i++) {
            if (filt && (key_cmp(&nd.keys[i], mn) < 0 || key_cmp(&nd.keys[i], mx) > 0)) continue;
            const uint8_t *vp; size_t vl;
            if ((e = resolve_value(&t->f, &nd.values[i], &vp, &vl))) { node_free(&nd); return e; }
            bj_begin_object(b);
            bj_put_key(b, (const uint8_t *)"key", 3);   emit_key(b, &nd.keys[i]);
            bj_put_key(b, (const uint8_t *)"value", 5); bj_put_raw(b, vp, vl);
            bj_end_object(b);
        }
    } else {
        int lo = 0, hi = nd.n_children - 1;
        if (filt) {
            /* Same descent rule as bpt_search (equal keys route right):
             * children before mn's child hold only keys < mn; children
             * after mx's child hold only keys > mx. */
            lo = key_upper_bound(nd.keys, nd.n_keys, mn);
            hi = key_upper_bound(nd.keys, nd.n_keys, mx);
        }
        for (int i = lo; i <= hi && i < nd.n_children; i++) {
            e = collect(t, nd.children[i], b, filt, mn, mx, depth + 1);
            if (e) { node_free(&nd); return e; }
        }
    }
    node_free(&nd);
    return bj_builder_error(b);
}

static int collect_to_out(bpt *t, int filt, const bpt_key *mn, const bpt_key *mx,
                          const uint8_t **out_ptr, size_t *out_len) {
    bj_builder *b = bj_builder_new();
    if (!b) return BJ_ERR_OOM;
    bj_begin_array(b);
    int e = collect(t, t->root, b, filt, mn, mx, 0);
    bj_end_array(b);
    if (!e) e = bj_builder_error(b);
    if (!e) {
        size_t len;
        const uint8_t *d = bj_builder_data(b, &len);
        if (!d) e = BJ_ERR_STATE;
        else if (!(e = set_out(t, d, len))) { *out_ptr = t->out.data; *out_len = t->out.len; }
    }
    bj_builder_free(b);
    return e;
}

int bpt_entries(bpt *t, const uint8_t **out_ptr, size_t *out_len) {
    return collect_to_out(t, 0, NULL, NULL, out_ptr, out_len);
}
int bpt_range(bpt *t, const bpt_key *min, const bpt_key *max,
              const uint8_t **out_ptr, size_t *out_len) {
    if (!bound_valid(min) || !bound_valid(max)) return BJ_ERR_STATE;
    return collect_to_out(t, 1, min, max, out_ptr, out_len);
}

/* ---- Cursors --------------------------------------------------------- */

typedef struct {
    bpt_node node;   /* parsed internal node (owned by the cursor) */
    int      idx;    /* next child index to visit                  */
} cur_frame;

struct bpt_cursor {
    bpt      *t;
    cur_frame stack[BPT_MAX_DEPTH + 1];
    int       depth;
    bpt_node  leaf;
    int       has_leaf;
    int       leaf_pos;
    bpt_key   min, max;
    int       has_min, has_max;
    int       done;
};

/*
 * Descend from `ptr` to a leaf, pushing the internal nodes visited onto the
 * stack. With `seek_min`, the descent follows the child that may contain the
 * cursor's lower bound (same equal-keys-route-right rule as bpt_search) and
 * positions the leaf at the first key >= min; otherwise it goes leftmost.
 */
static int cursor_descend(bpt_cursor *c, uint64_t ptr, int seek_min) {
    for (;;) {
        if (c->depth > BPT_MAX_DEPTH) return BJ_ERR_DEPTH;
        bpt_node nd;
        int e = parse_node(c->t, ptr, &nd);
        if (e) return e;
        if (nd.is_leaf) {
            c->leaf = nd;   /* take ownership */
            c->has_leaf = 1;
            c->leaf_pos = 0;
            if (seek_min && c->has_min)
                c->leaf_pos = key_lower_bound(c->leaf.keys, c->leaf.n_keys, &c->min);
            return BJ_OK;
        }
        if (nd.n_children <= 0) { node_free(&nd); return BJ_ERR_STATE; }
        int lo = 0;
        if (seek_min && c->has_min) {
            lo = key_upper_bound(nd.keys, nd.n_keys, &c->min);
            if (lo >= nd.n_children) lo = nd.n_children - 1;
        }
        uint64_t child = nd.children[lo];
        c->stack[c->depth].node = nd;   /* take ownership */
        c->stack[c->depth].idx = lo + 1;
        c->depth++;
        ptr = child;
    }
}

bpt_cursor *bpt_cursor_open(bpt *t, const bpt_key *min, const bpt_key *max) {
    if ((min && !bound_valid(min)) || (max && !bound_valid(max))) return NULL;
    bpt_cursor *c = (bpt_cursor *)calloc(1, sizeof(bpt_cursor));
    if (!c) return NULL;
    c->t = t;
    if (min) {
        if (key_copy(&c->min, min)) { free(c); return NULL; }
        c->has_min = 1;
    }
    if (max) {
        if (key_copy(&c->max, max)) { key_free(&c->min); free(c); return NULL; }
        c->has_max = 1;
    }
    /* Pin the current root: the cursor iterates this snapshot regardless of
     * later mutations (append-only nodes are never overwritten). */
    if (cursor_descend(c, t->root, 1)) { bpt_cursor_close(c); return NULL; }
    return c;
}

static void cursor_release(bpt_cursor *c) {
    if (c->has_leaf) { node_free(&c->leaf); c->has_leaf = 0; }
    while (c->depth > 0) node_free(&c->stack[--c->depth].node);
}

int bpt_cursor_next(bpt_cursor *c, bpt_key *key,
                    const uint8_t **val, size_t *val_len) {
    if (c->done) return 0;
    for (;;) {
        if (c->has_leaf && c->leaf_pos < c->leaf.n_keys) {
            bpt_key *k = &c->leaf.keys[c->leaf_pos];
            if (c->has_max && key_cmp(k, &c->max) > 0) break;   /* past max */
            /* Resolved value is valid only until the next read on this
             * tree — callers (bpt_cursor_next_batch) consume it immediately. */
            int e = resolve_value(&c->t->f, &c->leaf.values[c->leaf_pos], val, val_len);
            if (e) { c->done = 1; cursor_release(c); return e; }
            *key = *k;
            c->leaf_pos++;
            return 1;
        }
        /* Leaf exhausted (deletions can leave empty leaves — skip them):
         * ascend to the nearest frame with children left, descend leftmost. */
        if (c->has_leaf) { node_free(&c->leaf); c->has_leaf = 0; }
        int advanced = 0;
        while (c->depth > 0) {
            cur_frame *f = &c->stack[c->depth - 1];
            if (f->idx < f->node.n_children) {
                uint64_t ptr = f->node.children[f->idx++];
                int e = cursor_descend(c, ptr, 0);
                if (e) { c->done = 1; cursor_release(c); return e; }
                advanced = 1;
                break;
            }
            node_free(&f->node);
            c->depth--;
        }
        if (!advanced) break;   /* whole tree exhausted */
    }
    c->done = 1;
    cursor_release(c);
    return 0;
}

int bpt_cursor_next_batch(bpt_cursor *c, size_t max_bytes, int *count,
                          const uint8_t **out_ptr, size_t *out_len) {
    bpt *t = c->t;
    bj_builder *b = t->bld;
    bj_builder_reset(b);
    bj_begin_array(b);
    int n = 0;
    size_t approx = 0;
    while (approx < max_bytes) {
        bpt_key k; const uint8_t *v; size_t vl;
        int r = bpt_cursor_next(c, &k, &v, &vl);
        if (r < 0) return r;
        if (r == 0) break;
        bj_begin_object(b);
        bj_put_key(b, (const uint8_t *)"key", 3);   emit_key(b, &k);
        bj_put_key(b, (const uint8_t *)"value", 5); bj_put_raw(b, v, vl);
        bj_end_object(b);
        n++;
        approx += vl + (k.is_string ? (size_t)k.str_len + 5 : 9) + 26;
    }
    bj_end_array(b);
    int e = bj_builder_error(b);
    if (e) return e;
    size_t len;
    const uint8_t *d = bj_builder_data(b, &len);
    if (!d) return BJ_ERR_STATE;
    e = set_out(t, d, len);
    if (e) return e;
    *count = n;
    *out_ptr = t->out.data;
    *out_len = t->out.len;
    return BJ_OK;
}

void bpt_cursor_close(bpt_cursor *c) {
    if (!c) return;
    cursor_release(c);
    if (c->has_min) key_free(&c->min);
    if (c->has_max) key_free(&c->max);
    free(c);
}

int bpt_height(bpt *t, int *out_height) {
    int h = 0;
    uint64_t ptr = t->root;
    for (;;) {
        if (h > BPT_MAX_DEPTH) return BJ_ERR_DEPTH;
        bpt_node nd;
        int e = parse_node(t, ptr, &nd);
        if (e) return e;
        if (nd.is_leaf) { node_free(&nd); break; }
        h++;
        uint64_t child = nd.children[0];
        node_free(&nd);
        ptr = child;
    }
    *out_height = h;
    return BJ_OK;
}

/* ---- Invariant verification (see bplustree.h for the contract) -------- */

typedef struct {
    bpt     *t;
    int64_t  entries;      /* leaf entries seen so far            */
    int      leaf_depth;   /* depth of the first leaf, -1 until then */
} verify_state;

/* `lower`/`upper` are the routing bounds inherited from ancestors (NULL =
 * unbounded): every key in this subtree must satisfy lower <= k < upper. */
static int verify_node(verify_state *vs, uint64_t ptr, const bpt_key *lower,
                       const bpt_key *upper, int depth) {
    if (depth > BPT_MAX_DEPTH) return BJ_ERR_DEPTH;
    bpt_node nd;
    int e = parse_node(vs->t, ptr, &nd);
    if (e) return e;

    /* Capacity (writers split at order) and strictly ascending keys inside
     * the ancestors' bounds. Internal nodes with zero keys (one child) are
     * legal: the bulk loader's rightmost spine emits them at level tails. */
    if (nd.n_keys >= vs->t->order) e = BJ_ERR_VERIFY;
    for (int i = 0; !e && i < nd.n_keys; i++) {
        if (i > 0 && key_cmp(&nd.keys[i - 1], &nd.keys[i]) >= 0) e = BJ_ERR_VERIFY;
        else if (lower && key_cmp(&nd.keys[i], lower) < 0)       e = BJ_ERR_VERIFY;
        else if (upper && key_cmp(&nd.keys[i], upper) >= 0)      e = BJ_ERR_VERIFY;
    }

    if (!e && nd.is_leaf) {
        if (vs->leaf_depth < 0) vs->leaf_depth = depth;
        else if (vs->leaf_depth != depth) e = BJ_ERR_VERIFY;   /* uneven height */
        vs->entries += nd.n_keys;
        /* Out-of-line value pointers are append-only writes too: same
         * before-the-node ordering check as child pointers. */
        for (int i = 0; !e && i < nd.n_values; i++) {
            uint64_t off;
            if (blob_is_ool(&nd.values[i], &off) && off >= ptr) e = BJ_ERR_VERIFY;
        }
    } else if (!e) {
        for (int i = 0; i <= nd.n_keys && !e; i++) {
            /* Append-only writers emit children before the node that points
             * at them, so offsets strictly decrease downward — a forward or
             * self pointer is corruption, and checking it here also bounds
             * the walk against cycles. */
            if (nd.children[i] >= ptr) { e = BJ_ERR_VERIFY; break; }
            e = verify_node(vs, nd.children[i],
                            i == 0 ? lower : &nd.keys[i - 1],
                            i == nd.n_keys ? upper : &nd.keys[i],
                            depth + 1);
        }
    }
    node_free(&nd);
    return e;
}

int bpt_verify(bpt *t) {
    verify_state vs = { t, 0, -1 };
    int e = verify_node(&vs, t->root, NULL, NULL, 0);
    if (e) return e;
    return vs.entries == t->size ? BJ_OK : BJ_ERR_VERIFY;
}

/* ---- Compaction (bulk load) ------------------------------------------ */

/*
 * Compaction rebuilds the tree into a destination file with a classic B+ tree
 * bulk load: source entries are streamed in key order into per-level node
 * accumulators that are written the moment they reach capacity. The result is
 * a minimal, fully-packed tree — no append-only history, no per-entry
 * metadata records, and none of the empty-leaf cruft deletions leave behind —
 * produced in O(N) time with O(height) memory.
 *
 * Separator invariant (matches what insert splits produce): the key stored in
 * an internal node between children i and i+1 is the smallest key of child
 * i+1's subtree; leaf separators stay duplicated in the right leaf, internal
 * separators are promoted upward and not kept in the node.
 */

typedef struct {
    bpt_key  *keys;     int n_keys;       /* up to order-1                  */
    bpt_blob *vals;     int n_vals;       /* level 0 only                   */
    uint64_t *children; int n_children;   /* levels > 0, up to order        */
    bpt_key   node_sep; int has_sep;      /* separator preceding this node  */
    int       emitted;                    /* nodes already written at level */
} bl_level;

typedef struct {
    bpt      *t;
    bjfile   *dst;
    bl_level *levels;
    int       n_levels, cap_levels;
    uint64_t  next_id;
    int64_t   fed;                        /* entries streamed in            */
} bulk_loader;

static int bl_ensure_level(bulk_loader *bl, int L) {
    if (L < bl->n_levels) return BJ_OK;
    if (L >= bl->cap_levels) {
        int nc = bl->cap_levels ? bl->cap_levels * 2 : 8;
        while (nc <= L) nc *= 2;
        bl_level *nl = (bl_level *)realloc(bl->levels, (size_t)nc * sizeof(bl_level));
        if (!nl) return BJ_ERR_OOM;
        bl->levels = nl;
        bl->cap_levels = nc;
    }
    while (bl->n_levels <= L) {
        bl_level *lev = &bl->levels[bl->n_levels];
        memset(lev, 0, sizeof(*lev));
        int order = bl->t->order;
        lev->keys = (bpt_key *)calloc((size_t)order, sizeof(bpt_key));
        if (!lev->keys) return BJ_ERR_OOM;
        if (bl->n_levels == 0) {
            lev->vals = (bpt_blob *)calloc((size_t)order, sizeof(bpt_blob));
            if (!lev->vals) return BJ_ERR_OOM;
        } else {
            lev->children = (uint64_t *)calloc((size_t)order, sizeof(uint64_t));
            if (!lev->children) return BJ_ERR_OOM;
        }
        bl->n_levels++;
    }
    return BJ_OK;
}

/* Write level L's in-progress node to the destination; contents are consumed
 * (freed) but the arrays are kept for reuse. */
static int bl_write_node(bulk_loader *bl, int L, uint64_t *ptr) {
    bl_level *lev = &bl->levels[L];
    bpt_node nd;
    node_init(&nd);
    nd.id = bl->next_id++;
    nd.is_leaf = (L == 0);
    nd.keys = lev->keys;         nd.n_keys = lev->n_keys;
    nd.values = lev->vals;       nd.n_values = lev->n_vals;
    nd.children = lev->children; nd.n_children = lev->n_children;
    int e = encode_node_to(bl->t, &nd, bl->dst, ptr);
    for (int i = 0; i < lev->n_keys; i++) key_free(&lev->keys[i]);
    for (int i = 0; i < lev->n_vals; i++) blob_free(&lev->vals[i]);
    lev->n_keys = lev->n_vals = lev->n_children = 0;
    lev->emitted++;
    return e;
}

/*
 * Add a finished child node to level L. `sep` is the separator preceding
 * `ptr` (ownership transferred; has_sep 0 for a level's very first child).
 * When the level's node is full it is written out first, its own preceding
 * separator promoted upward with it, and the new node starts with `ptr`.
 */
static int bl_add_child(bulk_loader *bl, int L, int has_sep, bpt_key *sep, uint64_t ptr) {
    int e = bl_ensure_level(bl, L);
    if (e) { if (has_sep) key_free(sep); return e; }
    bl_level *lev = &bl->levels[L];

    if (lev->n_children == 0) {
        /* Very first child of this level. */
        lev->children[lev->n_children++] = ptr;
        lev->has_sep = has_sep;
        if (has_sep) lev->node_sep = *sep;
        return BJ_OK;
    }
    if (lev->n_children == bl->t->order) {
        uint64_t p;
        e = bl_write_node(bl, L, &p);
        if (e) { if (has_sep) key_free(sep); return e; }
        int up_has = lev->has_sep;
        bpt_key up = lev->node_sep;
        lev->has_sep = has_sep;
        if (has_sep) lev->node_sep = *sep;
        lev->children[lev->n_children++] = ptr;
        return bl_add_child(bl, L + 1, up_has, &up, p);
    }
    /* Every non-first child carries a separator by construction. */
    if (!has_sep) return BJ_ERR_STATE;
    lev->keys[lev->n_keys++] = *sep;   /* room: separator joins the node */
    lev->children[lev->n_children++] = ptr;
    return BJ_OK;
}

/* Stream one entry (key ordering is the caller's responsibility, and
 * `val` must be the entry's real bytes — resolved, not an out-of-line
 * marker). The loader takes its own copies: callers pass views into a
 * parsed node that dies before the accumulating level node fills. */
static int bl_add_entry(bulk_loader *bl, const bpt_key *key, const bpt_blob *val) {
    int e = bl_ensure_level(bl, 0);
    if (e) return e;
    bl_level *lev = &bl->levels[0];

    if (lev->n_keys == bl->t->order - 1) {
        uint64_t p;
        e = bl_write_node(bl, 0, &p);
        if (e) return e;
        int up_has = lev->has_sep;
        bpt_key up = lev->node_sep;
        /* The incoming key starts (and precedes) the new leaf. */
        e = key_copy(&lev->node_sep, key);
        if (e) { if (up_has) key_free(&up); return e; }
        lev->has_sep = 1;
        e = bl_add_child(bl, 1, up_has, &up, p);
        if (e) return e;
    }
    e = key_copy(&lev->keys[lev->n_keys], key);
    if (e) return e;
    /* Re-applies the out-of-line threshold against the destination file:
     * self-healing regardless of how the source stored the value (inline,
     * already out-of-line, or from before the threshold existed). */
    e = store_value(bl->t, bl->dst, val->bytes, val->len, &lev->vals[lev->n_vals]);
    if (e) { key_free(&lev->keys[lev->n_keys]); return e; }
    lev->n_keys++;
    lev->n_vals++;
    bl->fed += 1;
    return BJ_OK;
}

/* Flush remaining partial nodes bottom-up; the sole node at the highest
 * populated level becomes the root. */
static int bl_finish(bulk_loader *bl, uint64_t *root) {
    int e = bl_ensure_level(bl, 0);   /* empty tree: emit an empty leaf */
    if (e) return e;
    for (int L = 0; ; L++) {
        bl_level *lev = &bl->levels[L];
        if (lev->emitted == 0 && L == bl->n_levels - 1) {
            return bl_write_node(bl, L, root);
        }
        uint64_t p;
        e = bl_write_node(bl, L, &p);
        if (e) return e;
        int up_has = lev->has_sep;
        bpt_key up = lev->node_sep;
        lev->has_sep = 0;
        e = bl_add_child(bl, L + 1, up_has, &up, p);
        if (e) return e;
    }
}

static void bl_dispose(bulk_loader *bl) {
    for (int L = 0; L < bl->n_levels; L++) {
        bl_level *lev = &bl->levels[L];
        for (int i = 0; i < lev->n_keys; i++) key_free(&lev->keys[i]);
        for (int i = 0; i < lev->n_vals; i++) blob_free(&lev->vals[i]);
        if (lev->has_sep) key_free(&lev->node_sep);
        free(lev->keys);
        free(lev->vals);
        free(lev->children);
    }
    free(bl->levels);
}

/* In-order walk of the source tree, streaming leaf entries into the loader
 * (which copies them out of the transient parsed node). */
static int compact_walk(bpt *t, uint64_t ptr, bulk_loader *bl, int depth) {
    if (depth > BPT_MAX_DEPTH) return BJ_ERR_DEPTH;
    bpt_node nd;
    int e = parse_node(t, ptr, &nd);
    if (e) return e;
    if (nd.is_leaf) {
        for (int i = 0; i < nd.n_keys && !e; i++) {
            const uint8_t *vp; size_t vl;
            e = resolve_value(&t->f, &nd.values[i], &vp, &vl);
            if (!e) {
                bpt_blob resolved = { (uint8_t *)vp, (uint32_t)vl };
                e = bl_add_entry(bl, &nd.keys[i], &resolved);
            }
        }
    } else {
        for (int i = 0; i < nd.n_children && !e; i++)
            e = compact_walk(t, nd.children[i], bl, depth + 1);
    }
    node_free(&nd);
    return e;
}

int bpt_compact(bpt *t, const bj_io *dst_io) {
    bjfile dst;
    bjfile_init(&dst, dst_io);
    dst.autoflush = 1u << 18;   /* stream to the host in ~256 KB chunks */
    bulk_loader bl;
    memset(&bl, 0, sizeof(bl));
    bl.t = t;
    bl.dst = &dst;

    int e = bjfile_append_header(&dst, t->bld, "bplustree");
    if (!e) e = compact_walk(t, t->root, &bl, 0);
    uint64_t root = 0;
    if (!e) e = bl_finish(&bl, &root);
    if (!e) e = encode_metadata_to(t, &dst, root, bl.next_id, bl.fed);
    if (!e) e = bjfile_commit(&dst);
    bl_dispose(&bl);
    bjfile_dispose(&dst);
    return e;
}

/* ---- Lifecycle & accessors ------------------------------------------ */

/* Write a fresh header + empty root + metadata (shared by create/reset). */
static int init_empty(bpt *t) {
    int e = bjfile_append_header(&t->f, t->bld, "bplustree");
    if (e) return e;
    bpt_node root;
    if ((e = node_build_leaf(&root, 0, NULL, NULL, 0))) {
        node_free(&root);
        return e;
    }
    t->next_id = 1;
    t->size = 0;
    uint64_t rp;
    e = save_node(t, &root, &rp);
    node_free(&root);
    if (e) return e;
    t->root = rp;
    e = save_metadata(t);
    if (!e) e = bjfile_commit(&t->f);
    return e;
}

bpt *bpt_create(const bj_io *io, int order) {
    if (order < 3) return NULL;
    bpt *t = (bpt *)calloc(1, sizeof(bpt));
    if (!t) return NULL;
    t->bld = bj_builder_new();
    if (!t->bld) { free(t); return NULL; }
    bjfile_init(&t->f, io);
    t->order = order;
    t->min_keys = (order + 1) / 2 - 1;   /* ceil(order/2) - 1 */
    if (init_empty(t)) { bpt_free(t); return NULL; }
    return t;
}

int bpt_reset(bpt *t) {
    if (t->read_only) return BJ_ERR_STATE;
    int e = bjfile_set_len(&t->f, 0);
    if (e) return e;
    return init_empty(t);
}

/* Commit-scan callback: a commit ends at each record that parses and
 * validates as a metadata record of the fixed on-wire size. */
static int scan_cb(void *ctx, uint64_t off, const uint8_t *rec,
                   size_t rec_len, int *is_commit_end) {
    (void)ctx;
    if (rec_len == BPT_METADATA_SIZE) {
        bpt_meta m;
        if (parse_meta_rec(rec, rec_len, &m) == BJ_OK && meta_valid(&m, off))
            *is_commit_end = 1;
    }
    return BJ_OK;
}

/*
 * Open: verify the file identifies as a B+ tree (when it carries a header;
 * files from the JS reference have none and are accepted), then take the fast
 * path — parse + validate the metadata at the fixed tail offset and verify
 * the last commit's CRC. Any failure falls back to a full recovery scan that
 * verifies every protected commit: a torn tail is truncated back to the last
 * good commit; verifiable data beyond a damaged region refuses to open rather
 * than silently truncating good commits away.
 */
bpt *bpt_open(const bj_io *io) {
    bpt *t = (bpt *)calloc(1, sizeof(bpt));
    if (!t) return NULL;
    t->bld = bj_builder_new();
    if (!t->bld) { free(t); return NULL; }
    bjfile_init(&t->f, io);

    if (bjfile_check_header(&t->f, "bplustree") < 0) { bpt_free(t); return NULL; }

    const uint8_t *md; size_t md_len;
    bpt_meta m;
    uint64_t flen = bjfile_len(&t->f);
    if (bjfile_check_tail(&t->f, BPT_METADATA_SIZE, &md, &md_len) == BJ_OK &&
        parse_meta_rec(md, md_len, &m) == BJ_OK &&
        meta_valid(&m, flen - BPT_METADATA_SIZE)) {
        meta_apply(t, &m);
        return t;
    }

    /* Recovery. */
    uint64_t good = 0;
    if (bjfile_scan_commits(&t->f, scan_cb, NULL, &good)) { bpt_free(t); return NULL; }
    if (good < BPT_METADATA_SIZE) { bpt_free(t); return NULL; }
    const uint8_t *rec; size_t rec_len;
    if (bjfile_read_record(&t->f, good - BPT_METADATA_SIZE, &rec, &rec_len) ||
        rec_len != BPT_METADATA_SIZE ||
        parse_meta_rec(rec, rec_len, &m) != BJ_OK ||
        !meta_valid(&m, good - BPT_METADATA_SIZE)) {
        bpt_free(t);
        return NULL;
    }
    if (good < flen && bjfile_set_len(&t->f, good)) { bpt_free(t); return NULL; }
    meta_apply(t, &m);
    return t;
}

void bpt_free(bpt *t) {
    if (!t) return;
    bj_builder_free(t->bld);
    bjfile_dispose(&t->f);
    dbuf_free(&t->out);
    free(t);
}

uint64_t bpt_file_len(const bpt *t) { return bjfile_len(&t->f); }

/* ---- Snapshots (MVCC) ------------------------------------------------- */

int bpt_is_snapshot(const bpt *t) { return t->read_only; }

bpt *bpt_snapshot(const bpt *t) {
    bpt *s = (bpt *)calloc(1, sizeof(bpt));
    if (!s) return NULL;
    s->bld = bj_builder_new();
    if (!s->bld) { free(s); return NULL; }
    bjfile_init(&s->f, &t->f.io);
    s->order = t->order;
    s->min_keys = t->min_keys;
    s->root = t->root;
    s->next_id = t->next_id;
    s->size = t->size;
    s->read_only = 1;
    return s;
}

bpt *bpt_open_at(const bj_io *io, uint64_t len) {
    bpt *t = (bpt *)calloc(1, sizeof(bpt));
    if (!t) return NULL;
    t->bld = bj_builder_new();
    if (!t->bld) { free(t); return NULL; }
    bjfile_init(&t->f, io);
    if (bjfile_check_header(&t->f, "bplustree") < 0) { bpt_free(t); return NULL; }
    if (len < BPT_METADATA_SIZE || len > bjfile_len(&t->f)) { bpt_free(t); return NULL; }
    const uint8_t *rec; size_t rec_len;
    bpt_meta m;
    if (bjfile_read_record(&t->f, len - BPT_METADATA_SIZE, &rec, &rec_len) ||
        rec_len != BPT_METADATA_SIZE ||
        parse_meta_rec(rec, rec_len, &m) != BJ_OK ||
        !meta_valid(&m, len - BPT_METADATA_SIZE)) {
        bpt_free(t);
        return NULL;
    }
    meta_apply(t, &m);
    t->read_only = 1;
    return t;
}

/* Boundary accumulator for the commit scan. */
typedef struct {
    uint64_t *offs; int64_t *sizes;
    int n, cap;
} bnd_list;

static int boundaries_cb(void *ctx, uint64_t off, const uint8_t *rec,
                         size_t rec_len, int *is_commit_end) {
    bnd_list *bl = (bnd_list *)ctx;
    if (rec_len != BPT_METADATA_SIZE) return BJ_OK;
    bpt_meta m;
    if (parse_meta_rec(rec, rec_len, &m) != BJ_OK || !meta_valid(&m, off)) return BJ_OK;
    *is_commit_end = 1;
    if (bl->n == bl->cap) {
        int nc = bl->cap ? bl->cap * 2 : 32;
        uint64_t *no = (uint64_t *)realloc(bl->offs, (size_t)nc * sizeof(uint64_t));
        int64_t *ns = (int64_t *)realloc(bl->sizes, (size_t)nc * sizeof(int64_t));
        if (no) bl->offs = no;
        if (ns) bl->sizes = ns;
        if (!no || !ns) return BJ_ERR_OOM;
        bl->cap = nc;
    }
    bl->offs[bl->n] = off + rec_len;
    bl->sizes[bl->n] = m.size;
    bl->n++;
    return BJ_OK;
}

int bpt_boundaries(bpt *t, const uint8_t **out_ptr, size_t *out_len) {
    bnd_list bl;
    memset(&bl, 0, sizeof(bl));
    uint64_t good = 0;
    int e = bjfile_scan_commits(&t->f, boundaries_cb, &bl, &good);
    if (!e) {
        /* Keep only boundaries inside verified commits. */
        while (bl.n > 0 && bl.offs[bl.n - 1] > good) bl.n--;
        bj_builder *b = bj_builder_new();
        if (!b) e = BJ_ERR_OOM;
        else {
            bj_begin_array(b);
            for (int i = 0; i < bl.n; i++) {
                bj_begin_object(b);
                bj_put_key(b, (const uint8_t *)"offset", 6);
                bj_put_int(b, (int64_t)bl.offs[i]);
                bj_put_key(b, (const uint8_t *)"size", 4);
                bj_put_int(b, bl.sizes[i]);
                bj_end_object(b);
            }
            bj_end_array(b);
            e = bj_builder_error(b);
            if (!e) {
                size_t len;
                const uint8_t *d = bj_builder_data(b, &len);
                if (!d) e = BJ_ERR_STATE;
                else if (!(e = set_out(t, d, len))) {
                    *out_ptr = t->out.data;
                    *out_len = t->out.len;
                }
            }
            bj_builder_free(b);
        }
    }
    free(bl.offs);
    free(bl.sizes);
    return e;
}

int bpt_rewind(bpt *t, uint64_t len) {
    if (t->read_only) return BJ_ERR_STATE;
    uint64_t cur = bjfile_len(&t->f);
    if (len == cur) return BJ_OK;
    if (len > cur || len < BPT_METADATA_SIZE) return BJ_ERR_STATE;
    const uint8_t *rec; size_t rec_len;
    int e = bjfile_read_record(&t->f, len - BPT_METADATA_SIZE, &rec, &rec_len);
    if (e) return e;
    bpt_meta m;
    if (rec_len != BPT_METADATA_SIZE ||
        parse_meta_rec(rec, rec_len, &m) != BJ_OK ||
        !meta_valid(&m, len - BPT_METADATA_SIZE)) return BJ_ERR_STATE;
    if ((e = bjfile_set_len(&t->f, len))) return e;
    meta_apply(t, &m);
    return BJ_OK;
}

const uint8_t *bpt_out(const bpt *t, size_t *len) { if (len) *len = t->out.len; return t->out.data; }
int64_t        bpt_size(const bpt *t)     { return t->size; }
uint64_t       bpt_root(const bpt *t)     { return t->root; }
uint64_t       bpt_next_id(const bpt *t)  { return t->next_id; }
int            bpt_order(const bpt *t)    { return t->order; }
