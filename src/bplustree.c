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

/* ---- Values, keys, nodes -------------------------------------------- */

typedef struct { uint8_t *bytes; uint32_t len; } bpt_blob;

typedef struct {
    double     id;
    int        is_leaf;
    int        n_keys;
    bpt_key   *keys;        /* n_keys (string keys own their bytes)        */
    int        n_values;
    bpt_blob  *values;      /* n_values (== n_keys on leaves)              */
    int        n_children;
    double    *children;    /* n_children (pointer offsets)                */
    int        has_next;
    double     next;
} bpt_node;

struct bpt {
    bjfile     f;                                     /* backing file      */
    uint8_t   *out; size_t out_len; size_t out_cap;   /* last op output    */
    bj_builder *bld;                                  /* reused for saves  */
    double     root;
    double     next_id;
    double     size;
    int        order;
    int        min_keys;
};

/* ---- Little-endian readers ------------------------------------------ */

static uint32_t rdu32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static uint64_t rdu64(const uint8_t *p) {
    uint64_t v = 0;
    for (int i = 7; i >= 0; i--) v = (v << 8) | p[i];
    return v;
}

/* ---- Key helpers ---------------------------------------------------- */

static int is_safe_int(double d) {
    if (!isfinite(d)) return 0;
    if (d < BJ_MIN_SAFE_INT || d > BJ_MAX_SAFE_INT) return 0;
    return d == floor(d);
}

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
    for (int i = 0; i < n->n_keys; i++) key_free(&n->keys[i]);
    free(n->keys);
    for (int i = 0; i < n->n_values; i++) blob_free(&n->values[i]);
    free(n->values);
    free(n->children);
    node_init(n);
}

/* Build a leaf/internal node by deep-copying the supplied arrays. */
static int node_build_leaf(bpt_node *out, double id, const bpt_key *keys,
                           const bpt_blob *vals, int n, int has_next, double next) {
    node_init(out);
    out->id = id; out->is_leaf = 1; out->has_next = has_next; out->next = next;
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

static int node_build_internal(bpt_node *out, double id, const bpt_key *keys,
                               int nk, const double *children, int nc) {
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
        out->children = (double *)malloc((size_t)nc * sizeof(double));
        if (!out->children) return BJ_ERR_OOM;
        memcpy(out->children, children, (size_t)nc * sizeof(double));
    }
    out->n_children = nc;
    return BJ_OK;
}

/* ---- File append & output buffer ------------------------------------ */

static int file_append_to(bjfile *dst, const uint8_t *b, size_t n, double *off) {
    uint64_t o;
    int e = bjfile_append(dst, b, n, &o);
    if (e) return e;
    if (off) *off = (double)o;
    return BJ_OK;
}
static int set_out(bpt *t, const uint8_t *b, size_t n) {
    if (n > t->out_cap) {
        uint8_t *nb = (uint8_t *)realloc(t->out, n ? n : 1);
        if (!nb) return BJ_ERR_OOM;
        t->out = nb; t->out_cap = n;
    }
    if (n) memcpy(t->out, b, n);
    t->out_len = n;
    return BJ_OK;
}

/* ---- Wire-format cursor readers ------------------------------------- */

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
static int read_bool(cur *c, int *out) {
    uint8_t t;
    if (take_type(c, &t)) return BJ_ERR_EOF;
    if (t == BJ_TYPE_TRUE)  { *out = 1; return BJ_OK; }
    if (t == BJ_TYPE_FALSE) { *out = 0; return BJ_OK; }
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
        uint8_t *s = (uint8_t *)malloc(n ? n : 1);
        if (!s) return BJ_ERR_OOM;
        if (n) memcpy(s, c->d + c->pos, n);
        c->pos += n; out->is_string = 1; out->str = s; out->str_len = n;
        return BJ_OK;
    }
    return BJ_ERR_UNKNOWN_TYPE;
}
static int array_begin(cur *c, uint32_t *count) {
    uint8_t t;
    if (take_type(c, &t)) return BJ_ERR_EOF;
    if (t != BJ_TYPE_ARRAY) return BJ_ERR_UNKNOWN_TYPE;
    uint32_t size;
    if (take_u32(c, &size)) return BJ_ERR_EOF;
    return take_u32(c, count);
}
static int skip_value(cur *c) {
    size_t sz;
    int e = bj_value_size(c->d, c->len, c->pos, &sz);
    if (e) return e;
    if (cur_need(c, sz)) return BJ_ERR_EOF;
    c->pos += sz; return BJ_OK;
}

/* ---- Node (de)serialization ----------------------------------------- */

/* Decode the node object stored at `offset` in the file into `out`. */
static int parse_node(bpt *t, double offset, bpt_node *out) {
    node_init(out);
    const uint8_t *rec; size_t rec_len;
    int err = bjfile_read_record(&t->f, (uint64_t)offset, &rec, &rec_len);
    if (err) return err;
    cur c = { rec, rec_len, 0 };

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
            e = read_number(&c, &out->id);
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
                if ((e = blob_copy(&out->values[j], c.d + c.pos, (uint32_t)sz))) { out->n_values = j; node_free(out); return e; }
                c.pos += sz;
            }
            out->n_values = (int)n;
        } else if (name_eq(kn, klen, "children")) {
            uint32_t n;
            if ((e = array_begin(&c, &n))) { node_free(out); return e; }
            if (n) {
                out->children = (double *)malloc((size_t)n * sizeof(double));
                if (!out->children) { node_free(out); return BJ_ERR_OOM; }
            }
            for (uint32_t j = 0; j < n; j++) {
                if ((e = read_pointer(&c, &out->children[j]))) { node_free(out); return e; }
            }
            out->n_children = (int)n;
        } else if (name_eq(kn, klen, "next")) {
            uint8_t nt;
            if (take_type(&c, &nt)) { node_free(out); return BJ_ERR_EOF; }
            if (nt == BJ_TYPE_NULL) {
                out->has_next = 0;
            } else if (nt == BJ_TYPE_POINTER) {
                if (cur_need(&c, 8)) { node_free(out); return BJ_ERR_EOF; }
                out->next = (double)rdu64(c.d + c.pos);
                c.pos += 8; out->has_next = 1;
            } else { node_free(out); return BJ_ERR_UNKNOWN_TYPE; }
        } else {
            e = skip_value(&c);
        }
        if (e) { node_free(out); return e; }
    }
    return BJ_OK;
}

static int emit_key(bj_builder *b, const bpt_key *k) {
    if (k->is_string) return bj_put_string(b, k->str, k->str_len);
    if (is_safe_int(k->num)) return bj_put_int(b, (int64_t)k->num);
    return bj_put_float(b, k->num);
}

/* Encode `nd` and append it to `dst`; return its offset via *off. */
static int encode_node_to(bpt *t, const bpt_node *nd, bjfile *dst, double *off) {
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
    for (int i = 0; i < nd->n_children; i++) bj_put_pointer(b, (uint64_t)nd->children[i]);
    bj_end_array(b);
    bj_put_key(b, (const uint8_t *)"next", 4);
    if (nd->has_next) bj_put_pointer(b, (uint64_t)nd->next);
    else bj_put_null(b);
    bj_end_object(b);

    int e = bj_builder_error(b);
    if (e) return e;
    size_t len;
    const uint8_t *d = bj_builder_data(b, &len);
    if (!d) return BJ_ERR_STATE;
    return file_append_to(dst, d, len, off);
}

/* Append `nd` to the tree's live file. */
static int save_node(bpt *t, const bpt_node *nd, double *off) {
    return encode_node_to(t, nd, &t->f, off);
}

/* ---- Metadata ------------------------------------------------------- */

static int encode_metadata_to(bpt *t, bjfile *dst, double root,
                              double next_id, double size) {
    bj_builder *b = t->bld;
    bj_builder_reset(b);
    bj_begin_object(b);
    bj_put_key(b, (const uint8_t *)"version", 7);      bj_put_int(b, 1);
    bj_put_key(b, (const uint8_t *)"maxEntries", 10);  bj_put_int(b, t->order);
    bj_put_key(b, (const uint8_t *)"minEntries", 10);  bj_put_int(b, t->min_keys);
    bj_put_key(b, (const uint8_t *)"size", 4);         bj_put_int(b, (int64_t)size);
    bj_put_key(b, (const uint8_t *)"rootPointer", 11); bj_put_pointer(b, (uint64_t)root);
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
    double root, next_id, size;
    int    order, min_keys;
    int    have_root;
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
        double d;
        if (name_eq(kn, klen, "maxEntries"))      { if ((e = read_number(&c, &d))) return e; m->order = (int)d; }
        else if (name_eq(kn, klen, "minEntries")) { if ((e = read_number(&c, &d))) return e; m->min_keys = (int)d; }
        else if (name_eq(kn, klen, "size"))       { if ((e = read_number(&c, &m->size))) return e; }
        else if (name_eq(kn, klen, "nextId"))     { if ((e = read_number(&c, &m->next_id))) return e; }
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
    if (!(m->size >= 0)) return 0;
    if (!(m->next_id >= 0)) return 0;
    if (!(m->root >= 0) || m->root >= (double)before) return 0;
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

static int add_node(bpt *t, double ptr, const bpt_key *key,
                    const uint8_t *val, uint32_t vlen, add_res *out) {
    memset(out, 0, sizeof(*out));
    bpt_node nd;
    int e = parse_node(t, ptr, &nd);
    if (e) return e;

    if (nd.is_leaf) {
        int idx = -1;
        for (int i = 0; i < nd.n_keys; i++) if (key_eq(key, &nd.keys[i])) { idx = i; break; }

        if (idx >= 0) {
            /* Update existing key's value. */
            bpt_blob *wv = (bpt_blob *)malloc((size_t)nd.n_keys * sizeof(bpt_blob));
            if (!wv) { node_free(&nd); return BJ_ERR_OOM; }
            for (int i = 0; i < nd.n_keys; i++) wv[i] = nd.values[i];
            bpt_blob nb = { (uint8_t *)val, vlen };
            wv[idx] = nb;
            e = node_build_leaf(&out->newn, nd.id, nd.keys, wv, nd.n_keys, 0, 0);
            free(wv);
            out->is_split = 0;
            out->updated = 1;
            node_free(&nd);
            return e;
        }

        int insertIdx = 0;
        while (insertIdx < nd.n_keys && key_cmp(key, &nd.keys[insertIdx]) > 0) insertIdx++;
        int nlen = nd.n_keys + 1;
        bpt_key *wk = (bpt_key *)malloc((size_t)nlen * sizeof(bpt_key));
        bpt_blob *wv = (bpt_blob *)malloc((size_t)nlen * sizeof(bpt_blob));
        if (!wk || !wv) { free(wk); free(wv); node_free(&nd); return BJ_ERR_OOM; }
        for (int i = 0; i < insertIdx; i++) { wk[i] = nd.keys[i]; wv[i] = nd.values[i]; }
        wk[insertIdx] = *key;
        { bpt_blob nb = { (uint8_t *)val, vlen }; wv[insertIdx] = nb; }
        for (int i = insertIdx; i < nd.n_keys; i++) { wk[i + 1] = nd.keys[i]; wv[i + 1] = nd.values[i]; }

        if (nlen < t->order) {
            e = node_build_leaf(&out->newn, nd.id, wk, wv, nlen, 0, 0);
            out->is_split = 0;
        } else {
            int mid = (nlen + 1) / 2;   /* ceil(nlen/2) */
            e = node_build_leaf(&out->left, nd.id, wk, wv, mid, 0, 0);
            if (!e) e = node_build_leaf(&out->right, t->next_id++, wk + mid, wv + mid, nlen - mid, 0, 0);
            if (!e) e = key_copy(&out->split_key, &wk[mid]);
            out->is_split = 1;
        }
        free(wk); free(wv);
        node_free(&nd);
        return e;
    }

    /* Internal node. */
    int childIdx = 0;
    while (childIdx < nd.n_keys && key_cmp(key, &nd.keys[childIdx]) >= 0) childIdx++;

    add_res cr;
    e = add_node(t, nd.children[childIdx], key, val, vlen, &cr);
    if (e) { node_free(&nd); return e; }

    if (!cr.is_split) {
        double ncp;
        e = save_node(t, &cr.newn, &ncp);
        node_free(&cr.newn);
        if (e) { node_free(&nd); return e; }
        double *wc = (double *)malloc((size_t)nd.n_children * sizeof(double));
        if (!wc) { node_free(&nd); return BJ_ERR_OOM; }
        memcpy(wc, nd.children, (size_t)nd.n_children * sizeof(double));
        wc[childIdx] = ncp;
        e = node_build_internal(&out->newn, nd.id, nd.keys, nd.n_keys, wc, nd.n_children);
        free(wc);
        out->is_split = 0;
        out->updated = cr.updated;
        node_free(&nd);
        return e;
    }

    /* Child split: save both halves, splice splitKey + the two child pointers. */
    double lp, rp;
    e = save_node(t, &cr.left, &lp);
    if (!e) e = save_node(t, &cr.right, &rp);
    node_free(&cr.left); node_free(&cr.right);
    if (e) { key_free(&cr.split_key); node_free(&nd); return e; }

    int nkeys = nd.n_keys + 1;
    int nch = nd.n_children + 1;
    bpt_key *wk = (bpt_key *)malloc((size_t)nkeys * sizeof(bpt_key));
    double  *wc = (double *)malloc((size_t)nch * sizeof(double));
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
    int e = add_node(t, t->root, key, val, vlen, &res);
    if (e) return e;

    bpt_node new_root;
    if (!res.is_split) {
        new_root = res.newn;   /* take ownership */
    } else {
        double lp, rp;
        e = save_node(t, &res.left, &lp);
        if (!e) e = save_node(t, &res.right, &rp);
        node_free(&res.left); node_free(&res.right);
        if (e) { key_free(&res.split_key); return e; }
        double children[2] = { lp, rp };
        e = node_build_internal(&new_root, t->next_id++, &res.split_key, 1, children, 2);
        key_free(&res.split_key);
        if (e) { node_free(&new_root); return e; }
    }

    double rootp;
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
    double root = t->root, next_id = t->next_id, size = t->size;
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

static int del_node(bpt *t, double ptr, const bpt_key *key, del_res *out) {
    memset(out, 0, sizeof(*out));
    bpt_node nd;
    int e = parse_node(t, ptr, &nd);
    if (e) return e;

    if (nd.is_leaf) {
        int idx = -1;
        for (int i = 0; i < nd.n_keys; i++) if (key_eq(key, &nd.keys[i])) { idx = i; break; }
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
        e = node_build_leaf(&out->node, nd.id, wk, wv, nlen, nd.has_next, nd.next);
        free(wk); free(wv);
        out->found = 1;
        node_free(&nd);
        return e;
    }

    int i = 0;
    while (i < nd.n_keys && key_cmp(key, &nd.keys[i]) >= 0) i++;
    del_res cr;
    e = del_node(t, nd.children[i], key, &cr);
    if (e) { node_free(&nd); return e; }
    if (!cr.found) { out->found = 0; node_free(&nd); return BJ_OK; }

    double ncp;
    e = save_node(t, &cr.node, &ncp);
    node_free(&cr.node);
    if (e) { node_free(&nd); return e; }
    double *wc = (double *)malloc((size_t)nd.n_children * sizeof(double));
    if (!wc) { node_free(&nd); return BJ_ERR_OOM; }
    memcpy(wc, nd.children, (size_t)nd.n_children * sizeof(double));
    wc[i] = ncp;
    e = node_build_internal(&out->node, nd.id, nd.keys, nd.n_keys, wc, nd.n_children);
    free(wc);
    out->found = 1;
    node_free(&nd);
    return e;
}

static int delete_root(bpt *t, const bpt_key *key) {
    del_res res;
    int e = del_node(t, t->root, key, &res);
    if (e) return e;
    if (!res.found) return BJ_OK;   /* key not present: no-op */

    bpt_node final_root = res.node;   /* take ownership */
    /* Collapse a now-empty internal root down to its only child. */
    if (final_root.n_keys == 0 && !final_root.is_leaf && final_root.n_children > 0) {
        double child = final_root.children[0];
        node_free(&final_root);
        e = parse_node(t, child, &final_root);
        if (e) return e;
    }

    double rootp;
    e = save_node(t, &final_root, &rootp);
    node_free(&final_root);
    if (e) return e;
    t->root = rootp;
    t->size -= 1;
    return save_metadata(t);
}

int bpt_delete(bpt *t, const bpt_key *key) {
    double root = t->root, next_id = t->next_id, size = t->size;
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
    double ptr = t->root;
    for (;;) {
        bpt_node nd;
        int e = parse_node(t, ptr, &nd);
        if (e) return e;
        if (nd.is_leaf) {
            int fi = -1;
            for (int i = 0; i < nd.n_keys; i++) if (key_eq(key, &nd.keys[i])) { fi = i; break; }
            if (fi >= 0) {
                e = set_out(t, nd.values[fi].bytes, nd.values[fi].len);
                node_free(&nd);
                if (e) return e;
                *found = 1; *out_ptr = t->out; *out_len = t->out_len;
                return BJ_OK;
            }
            node_free(&nd);
            *found = 0;
            return BJ_OK;
        }
        int i = 0;
        while (i < nd.n_keys && key_cmp(key, &nd.keys[i]) >= 0) i++;
        double child = nd.children[i];
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
static int collect(bpt *t, double ptr, bj_builder *b, int filt,
                   const bpt_key *mn, const bpt_key *mx, int depth) {
    if (depth > BPT_MAX_DEPTH) return BJ_ERR_DEPTH;
    bpt_node nd;
    int e = parse_node(t, ptr, &nd);
    if (e) return e;
    if (nd.is_leaf) {
        for (int i = 0; i < nd.n_keys; i++) {
            if (filt && (key_cmp(&nd.keys[i], mn) < 0 || key_cmp(&nd.keys[i], mx) > 0)) continue;
            bj_begin_object(b);
            bj_put_key(b, (const uint8_t *)"key", 3);   emit_key(b, &nd.keys[i]);
            bj_put_key(b, (const uint8_t *)"value", 5); bj_put_raw(b, nd.values[i].bytes, nd.values[i].len);
            bj_end_object(b);
        }
    } else {
        int lo = 0, hi = nd.n_children - 1;
        if (filt) {
            /* Same descent rule as bpt_search (equal keys route right):
             * children before mn's child hold only keys < mn; children
             * after mx's child hold only keys > mx. */
            lo = 0;
            while (lo < nd.n_keys && key_cmp(mn, &nd.keys[lo]) >= 0) lo++;
            hi = 0;
            while (hi < nd.n_keys && key_cmp(mx, &nd.keys[hi]) >= 0) hi++;
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
        else if (!(e = set_out(t, d, len))) { *out_ptr = t->out; *out_len = t->out_len; }
    }
    bj_builder_free(b);
    return e;
}

int bpt_entries(bpt *t, const uint8_t **out_ptr, size_t *out_len) {
    return collect_to_out(t, 0, NULL, NULL, out_ptr, out_len);
}
int bpt_range(bpt *t, const bpt_key *min, const bpt_key *max,
              const uint8_t **out_ptr, size_t *out_len) {
    return collect_to_out(t, 1, min, max, out_ptr, out_len);
}

int bpt_height(bpt *t, int *out_height) {
    int h = 0;
    double ptr = t->root;
    for (;;) {
        bpt_node nd;
        int e = parse_node(t, ptr, &nd);
        if (e) return e;
        if (nd.is_leaf) { node_free(&nd); break; }
        h++;
        double child = nd.children[0];
        node_free(&nd);
        ptr = child;
    }
    *out_height = h;
    return BJ_OK;
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
    double   *children; int n_children;   /* levels > 0, up to order        */
    bpt_key   node_sep; int has_sep;      /* separator preceding this node  */
    int       emitted;                    /* nodes already written at level */
} bl_level;

typedef struct {
    bpt      *t;
    bjfile   *dst;
    bl_level *levels;
    int       n_levels, cap_levels;
    double    next_id;
    double    fed;                        /* entries streamed in            */
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
            lev->children = (double *)calloc((size_t)order, sizeof(double));
            if (!lev->children) return BJ_ERR_OOM;
        }
        bl->n_levels++;
    }
    return BJ_OK;
}

/* Write level L's in-progress node to the destination; contents are consumed
 * (freed) but the arrays are kept for reuse. */
static int bl_write_node(bulk_loader *bl, int L, double *ptr) {
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
static int bl_add_child(bulk_loader *bl, int L, int has_sep, bpt_key *sep, double ptr) {
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
        double p;
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

/* Stream one entry (key ordering is the caller's responsibility; ownership of
 * key/val transfers on success). */
static int bl_add_entry(bulk_loader *bl, bpt_key *key, bpt_blob *val) {
    int e = bl_ensure_level(bl, 0);
    if (e) return e;
    bl_level *lev = &bl->levels[0];

    if (lev->n_keys == bl->t->order - 1) {
        double p;
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
    lev->keys[lev->n_keys++] = *key;
    lev->vals[lev->n_vals++] = *val;
    bl->fed += 1;
    return BJ_OK;
}

/* Flush remaining partial nodes bottom-up; the sole node at the highest
 * populated level becomes the root. */
static int bl_finish(bulk_loader *bl, double *root) {
    int e = bl_ensure_level(bl, 0);   /* empty tree: emit an empty leaf */
    if (e) return e;
    for (int L = 0; ; L++) {
        bl_level *lev = &bl->levels[L];
        if (lev->emitted == 0 && L == bl->n_levels - 1) {
            return bl_write_node(bl, L, root);
        }
        double p;
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

/* In-order walk of the source tree, streaming leaf entries into the loader.
 * Ownership of each entry's key/value moves into the loader. */
static int compact_walk(bpt *t, double ptr, bulk_loader *bl, int depth) {
    if (depth > BPT_MAX_DEPTH) return BJ_ERR_DEPTH;
    bpt_node nd;
    int e = parse_node(t, ptr, &nd);
    if (e) return e;
    if (nd.is_leaf) {
        for (int i = 0; i < nd.n_keys && !e; i++) {
            e = bl_add_entry(bl, &nd.keys[i], &nd.values[i]);
            if (!e) {
                /* consumed: stop node_free from double-freeing */
                nd.keys[i].is_string = 0; nd.keys[i].str = NULL;
                nd.values[i].bytes = NULL;
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
    double root = 0;
    if (!e) e = bl_finish(&bl, &root);
    if (!e) e = encode_metadata_to(t, &dst, root, bl.next_id, bl.fed);
    if (!e) e = bjfile_commit(&dst);
    bl_dispose(&bl);
    bjfile_dispose(&dst);
    return e;
}

/* ---- Lifecycle & accessors ------------------------------------------ */

bpt *bpt_create(const bj_io *io, int order) {
    if (order < 3) return NULL;
    bpt *t = (bpt *)calloc(1, sizeof(bpt));
    if (!t) return NULL;
    t->bld = bj_builder_new();
    if (!t->bld) { free(t); return NULL; }
    bjfile_init(&t->f, io);
    t->order = order;
    t->min_keys = (order + 1) / 2 - 1;   /* ceil(order/2) - 1 */

    if (bjfile_append_header(&t->f, t->bld, "bplustree")) { bpt_free(t); return NULL; }
    bpt_node root;
    if (node_build_leaf(&root, 0, NULL, NULL, 0, 0, 0)) { node_free(&root); bpt_free(t); return NULL; }
    t->next_id = 1;
    t->size = 0;
    double rp;
    if (save_node(t, &root, &rp)) { node_free(&root); bpt_free(t); return NULL; }
    node_free(&root);
    t->root = rp;
    if (save_metadata(t) || bjfile_commit(&t->f)) { bpt_free(t); return NULL; }
    return t;
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
    free(t->out);
    free(t);
}

double         bpt_size(const bpt *t)     { return t->size; }
double         bpt_root(const bpt *t)     { return t->root; }
double         bpt_next_id(const bpt *t)  { return t->next_id; }
int            bpt_order(const bpt *t)    { return t->order; }
