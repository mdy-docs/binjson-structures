/*
 * rtree.c — C port of src/rtree.js. See rtree.h.
 *
 * The tree is persistent, append-only and immutable: every mutation appends new
 * nodes (and fresh metadata) to the in-memory file image and re-points the root,
 * exactly like the reference. Nodes/metadata use the binjson wire format from
 * binjson.c so the on-disk bytes match the JS implementation.
 */
#include "rtree.h"

#include <stdlib.h>
#include <string.h>
#include <math.h>

/* Fixed on-wire size of the metadata object (matches METADATA_SIZE in
 * rtree.js): 6 fields, all fixed-width INT/POINTER values. */
#define RT_METADATA_SIZE 135

/* ---- Bounding boxes ------------------------------------------------- */

typedef struct {
    int    valid;                     /* 0 == null bbox                  */
    double min_lat, max_lat, min_lng, max_lng;
} rbbox;

static double bbox_area(const rbbox *b) {
    return (b->max_lat - b->min_lat) * (b->max_lng - b->min_lng);
}
static rbbox bbox_union(const rbbox *a, const rbbox *b) {
    rbbox r;
    r.valid = 1;
    r.min_lat = a->min_lat < b->min_lat ? a->min_lat : b->min_lat;
    r.max_lat = a->max_lat > b->max_lat ? a->max_lat : b->max_lat;
    r.min_lng = a->min_lng < b->min_lng ? a->min_lng : b->min_lng;
    r.max_lng = a->max_lng > b->max_lng ? a->max_lng : b->max_lng;
    return r;
}
static double bbox_enlargement(const rbbox *a, const rbbox *b) {
    rbbox u = bbox_union(a, b);
    return bbox_area(&u) - bbox_area(a);
}
static int bbox_intersects(const rbbox *a, const rbbox *b) {
    return !(a->max_lat < b->min_lat || a->min_lat > b->max_lat ||
             a->max_lng < b->min_lng || a->min_lng > b->max_lng);
}

/* ---- Nodes ---------------------------------------------------------- */

typedef struct {
    rbbox   bbox;            /* point bbox (min==max) */
    double  lat, lng;
    uint8_t oid[12];
} rentry;

typedef struct {
    double  id;
    int     is_leaf;
    rbbox   bbox;            /* node bbox (valid flag) */
    int     n;               /* number of children */
    rentry *entries;         /* leaf: n entries */
    double *children;        /* internal: n pointer offsets */
} rnode;

/* ---- Dynamic byte buffer -------------------------------------------- */

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
static int dbuf_append(dbuf *b, const uint8_t *p, size_t n, double *off) {
    int e = dbuf_ensure(b, n);
    if (e) return e;
    if (off) *off = (double)b->len;
    memcpy(b->data + b->len, p, n);
    b->len += n;
    return BJ_OK;
}

struct rtree {
    dbuf        img;         /* file image                     */
    dbuf        out;         /* last op output                 */
    bj_builder *bld;         /* reused for node/metadata saves */
    double      root;
    double      next_id;
    double      size;
    int         max_entries;
    int         min_entries;
};

static int set_out(rtree *t, const uint8_t *b, size_t n) {
    t->out.len = 0;
    return dbuf_append(&t->out, b, n, NULL);
}

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

static int is_safe_int(double d) {
    if (!isfinite(d)) return 0;
    if (d < BJ_MIN_SAFE_INT || d > BJ_MAX_SAFE_INT) return 0;
    return d == floor(d);
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
static int object_begin(cur *c, uint32_t *count) {
    uint8_t t;
    if (take_type(c, &t)) return BJ_ERR_EOF;
    if (t != BJ_TYPE_OBJECT) return BJ_ERR_UNKNOWN_TYPE;
    uint32_t size;
    if (take_u32(c, &size)) return BJ_ERR_EOF;
    return take_u32(c, count);
}
static int array_begin(cur *c, uint32_t *count) {
    uint8_t t;
    if (take_type(c, &t)) return BJ_ERR_EOF;
    if (t != BJ_TYPE_ARRAY) return BJ_ERR_UNKNOWN_TYPE;
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

/* Read a bbox: either NULL or an object { minLat, maxLat, minLng, maxLng }. */
static int read_bbox(cur *c, rbbox *out) {
    if (cur_need(c, 1)) return BJ_ERR_EOF;
    if (c->d[c->pos] == BJ_TYPE_NULL) { c->pos++; out->valid = 0; return BJ_OK; }
    uint32_t count;
    int e = object_begin(c, &count);
    if (e) return e;
    out->valid = 1;
    out->min_lat = out->max_lat = out->min_lng = out->max_lng = 0;
    for (uint32_t i = 0; i < count; i++) {
        const uint8_t *kn; uint32_t klen;
        if ((e = take_key(c, &kn, &klen))) return e;
        double v;
        if      (name_eq(kn, klen, "minLat")) { if ((e = read_number(c, &v))) return e; out->min_lat = v; }
        else if (name_eq(kn, klen, "maxLat")) { if ((e = read_number(c, &v))) return e; out->max_lat = v; }
        else if (name_eq(kn, klen, "minLng")) { if ((e = read_number(c, &v))) return e; out->min_lng = v; }
        else if (name_eq(kn, klen, "maxLng")) { if ((e = read_number(c, &v))) return e; out->max_lng = v; }
        else                                  { if ((e = skip_value(c))) return e; }
    }
    return BJ_OK;
}

/* Read a leaf entry object { bbox, lat, lng, objectId }. */
static int read_entry(cur *c, rentry *out) {
    memset(out, 0, sizeof(*out));
    uint32_t count;
    int e = object_begin(c, &count);
    if (e) return e;
    for (uint32_t i = 0; i < count; i++) {
        const uint8_t *kn; uint32_t klen;
        if ((e = take_key(c, &kn, &klen))) return e;
        if (name_eq(kn, klen, "bbox")) {
            if ((e = read_bbox(c, &out->bbox))) return e;
        } else if (name_eq(kn, klen, "lat")) {
            if ((e = read_number(c, &out->lat))) return e;
        } else if (name_eq(kn, klen, "lng")) {
            if ((e = read_number(c, &out->lng))) return e;
        } else if (name_eq(kn, klen, "objectId")) {
            uint8_t t;
            if (take_type(c, &t)) return BJ_ERR_EOF;
            if (t != BJ_TYPE_OID) return BJ_ERR_UNKNOWN_TYPE;
            if (cur_need(c, 12)) return BJ_ERR_EOF;
            memcpy(out->oid, c->d + c->pos, 12);
            c->pos += 12;
        } else {
            if ((e = skip_value(c))) return e;
        }
    }
    return BJ_OK;
}

/* ---- Node lifecycle ------------------------------------------------- */

static void node_init(rnode *n) { memset(n, 0, sizeof(*n)); }
static void node_free(rnode *n) {
    free(n->entries);
    free(n->children);
    node_init(n);
}

/* Decode the node object stored at `offset` in the image into `out`. */
static int parse_node(rtree *t, double offset, rnode *out) {
    node_init(out);
    size_t off = (size_t)offset;
    if (off > t->img.len) return BJ_ERR_EOF;
    cur c = { t->img.data + off, t->img.len - off, 0 };

    uint32_t count;
    int e = object_begin(&c, &count);
    if (e) return e;

    for (uint32_t i = 0; i < count; i++) {
        const uint8_t *kn; uint32_t klen;
        if ((e = take_key(&c, &kn, &klen))) { node_free(out); return e; }
        if (name_eq(kn, klen, "id")) {
            if ((e = read_number(&c, &out->id))) { node_free(out); return e; }
        } else if (name_eq(kn, klen, "isLeaf")) {
            if ((e = read_bool(&c, &out->is_leaf))) { node_free(out); return e; }
        } else if (name_eq(kn, klen, "bbox")) {
            if ((e = read_bbox(&c, &out->bbox))) { node_free(out); return e; }
        } else if (name_eq(kn, klen, "children")) {
            uint32_t n;
            if ((e = array_begin(&c, &n))) { node_free(out); return e; }
            /* Distinguish leaf entries (OBJECT) from internal pointers (POINTER)
             * by the element type byte, so parsing does not depend on field
             * order relative to "isLeaf". */
            int is_leaf = 1;
            if (n > 0) {
                if (cur_need(&c, 1)) { node_free(out); return BJ_ERR_EOF; }
                is_leaf = c.d[c.pos] != BJ_TYPE_POINTER;
            } else {
                is_leaf = out->is_leaf; /* empty: trust the flag read earlier */
            }
            if (is_leaf) {
                if (n) {
                    out->entries = (rentry *)calloc(n, sizeof(rentry));
                    if (!out->entries) { node_free(out); return BJ_ERR_OOM; }
                }
                for (uint32_t j = 0; j < n; j++) {
                    if ((e = read_entry(&c, &out->entries[j]))) { out->n = (int)j; node_free(out); return e; }
                }
            } else {
                if (n) {
                    out->children = (double *)malloc((size_t)n * sizeof(double));
                    if (!out->children) { node_free(out); return BJ_ERR_OOM; }
                }
                for (uint32_t j = 0; j < n; j++) {
                    if ((e = read_pointer(&c, &out->children[j]))) { out->n = (int)j; node_free(out); return e; }
                }
            }
            out->n = (int)n;
        } else {
            if ((e = skip_value(&c))) { node_free(out); return e; }
        }
    }
    return BJ_OK;
}

/* ---- Node encoding -------------------------------------------------- */

static int emit_number(bj_builder *b, double v) {
    if (is_safe_int(v)) return bj_put_int(b, (int64_t)v);
    return bj_put_float(b, v);
}
static void emit_bbox(bj_builder *b, const rbbox *bb) {
    if (!bb->valid) { bj_put_null(b); return; }
    bj_begin_object(b);
    bj_put_key(b, (const uint8_t *)"minLat", 6); emit_number(b, bb->min_lat);
    bj_put_key(b, (const uint8_t *)"maxLat", 6); emit_number(b, bb->max_lat);
    bj_put_key(b, (const uint8_t *)"minLng", 6); emit_number(b, bb->min_lng);
    bj_put_key(b, (const uint8_t *)"maxLng", 6); emit_number(b, bb->max_lng);
    bj_end_object(b);
}
static void emit_entry(bj_builder *b, const rentry *en) {
    bj_begin_object(b);
    bj_put_key(b, (const uint8_t *)"bbox", 4);     emit_bbox(b, &en->bbox);
    bj_put_key(b, (const uint8_t *)"lat", 3);      emit_number(b, en->lat);
    bj_put_key(b, (const uint8_t *)"lng", 3);      emit_number(b, en->lng);
    bj_put_key(b, (const uint8_t *)"objectId", 8); bj_put_oid(b, en->oid);
    bj_end_object(b);
}

/* Encode `nd` and append it to `dst`; return its offset via *off. */
static int encode_node(rtree *t, const rnode *nd, dbuf *dst, double *off) {
    bj_builder *b = t->bld;
    bj_builder_reset(b);
    bj_begin_object(b);
    bj_put_key(b, (const uint8_t *)"id", 2);     emit_number(b, nd->id);
    bj_put_key(b, (const uint8_t *)"isLeaf", 6); bj_put_bool(b, nd->is_leaf);
    bj_put_key(b, (const uint8_t *)"children", 8);
    bj_begin_array(b);
    if (nd->is_leaf) {
        for (int i = 0; i < nd->n; i++) emit_entry(b, &nd->entries[i]);
    } else {
        for (int i = 0; i < nd->n; i++) bj_put_pointer(b, (uint64_t)nd->children[i]);
    }
    bj_end_array(b);
    bj_put_key(b, (const uint8_t *)"bbox", 4);    emit_bbox(b, &nd->bbox);
    bj_end_object(b);

    int e = bj_builder_error(b);
    if (e) return e;
    size_t len;
    const uint8_t *d = bj_builder_data(b, &len);
    if (!d) return BJ_ERR_STATE;
    return dbuf_append(dst, d, len, off);
}

/* Append `nd` to the tree's live image. */
static int save_node(rtree *t, const rnode *nd, double *off) {
    return encode_node(t, nd, &t->img, off);
}

/* ---- Metadata ------------------------------------------------------- */

static int encode_metadata(rtree *t, dbuf *dst, double root) {
    bj_builder *b = t->bld;
    bj_builder_reset(b);
    bj_begin_object(b);
    bj_put_key(b, (const uint8_t *)"version", 7);      bj_put_int(b, 1);
    bj_put_key(b, (const uint8_t *)"maxEntries", 10);  bj_put_int(b, t->max_entries);
    bj_put_key(b, (const uint8_t *)"minEntries", 10);  bj_put_int(b, t->min_entries);
    bj_put_key(b, (const uint8_t *)"size", 4);         bj_put_int(b, (int64_t)t->size);
    bj_put_key(b, (const uint8_t *)"rootPointer", 11); bj_put_pointer(b, (uint64_t)root);
    bj_put_key(b, (const uint8_t *)"nextId", 6);       bj_put_int(b, (int64_t)t->next_id);
    bj_end_object(b);

    int e = bj_builder_error(b);
    if (e) return e;
    size_t len;
    const uint8_t *d = bj_builder_data(b, &len);
    if (!d) return BJ_ERR_STATE;
    double off;
    return dbuf_append(dst, d, len, &off);
}
static int save_metadata(rtree *t) { return encode_metadata(t, &t->img, t->root); }

static int parse_metadata(rtree *t) {
    if (t->img.len < RT_METADATA_SIZE) return BJ_ERR_EOF;
    cur c = { t->img.data + (t->img.len - RT_METADATA_SIZE), RT_METADATA_SIZE, 0 };
    uint32_t count;
    int e = object_begin(&c, &count);
    if (e) return e;
    int have_root = 0;
    for (uint32_t i = 0; i < count; i++) {
        const uint8_t *kn; uint32_t klen;
        if ((e = take_key(&c, &kn, &klen))) return e;
        double d;
        if      (name_eq(kn, klen, "maxEntries")) { if ((e = read_number(&c, &d))) return e; t->max_entries = (int)d; }
        else if (name_eq(kn, klen, "minEntries")) { if ((e = read_number(&c, &d))) return e; t->min_entries = (int)d; }
        else if (name_eq(kn, klen, "size"))       { if ((e = read_number(&c, &t->size))) return e; }
        else if (name_eq(kn, klen, "nextId"))     { if ((e = read_number(&c, &t->next_id))) return e; }
        else if (name_eq(kn, klen, "rootPointer")){ if ((e = read_pointer(&c, &t->root))) return e; have_root = 1; }
        else                                      { if ((e = skip_value(&c))) return e; }
    }
    return have_root ? BJ_OK : BJ_ERR_STATE;
}

/* ---- Node construction & bbox --------------------------------------- */

/* The bounding box of child `i`: leaf children carry their own point box;
 * internal children require loading the referenced node. */
static int child_bbox(rtree *t, const rnode *nd, int i, rbbox *out) {
    if (nd->is_leaf) { *out = nd->entries[i].bbox; return BJ_OK; }
    rnode ch;
    int e = parse_node(t, nd->children[i], &ch);
    if (e) return e;
    *out = ch.bbox;
    node_free(&ch);
    return BJ_OK;
}

/* Recompute node->bbox as the union of its children's boxes (null if empty). */
static int compute_bbox(rtree *t, rnode *nd) {
    rbbox acc; acc.valid = 0;
    for (int i = 0; i < nd->n; i++) {
        rbbox cb;
        int e = child_bbox(t, nd, i, &cb);
        if (e) return e;
        if (!cb.valid) continue;
        acc = acc.valid ? bbox_union(&acc, &cb) : cb;
    }
    nd->bbox = acc;
    return BJ_OK;
}

static int make_leaf(rnode *out, double id, const rentry *entries, int n) {
    node_init(out);
    out->id = id; out->is_leaf = 1; out->n = n;
    if (n) {
        out->entries = (rentry *)malloc((size_t)n * sizeof(rentry));
        if (!out->entries) return BJ_ERR_OOM;
        memcpy(out->entries, entries, (size_t)n * sizeof(rentry));
    }
    return BJ_OK;
}
static int make_internal(rnode *out, double id, const double *children, int n) {
    node_init(out);
    out->id = id; out->is_leaf = 0; out->n = n;
    if (n) {
        out->children = (double *)malloc((size_t)n * sizeof(double));
        if (!out->children) return BJ_ERR_OOM;
        memcpy(out->children, children, (size_t)n * sizeof(double));
    }
    return BJ_OK;
}

/* ---- Split (mirrors _split) ----------------------------------------- */

typedef struct { int split; double ptr, ptr2; } ins_res;

/* Split the overflowing in-memory `nd` into two saved nodes. */
static int split_node(rtree *t, const rnode *nd, ins_res *out) {
    int n = nd->n;
    rbbox *cb = (rbbox *)malloc((size_t)n * sizeof(rbbox));
    int *grp = (int *)malloc((size_t)n * sizeof(int)); /* 1 or 2 */
    if (!cb || !grp) { free(cb); free(grp); return BJ_ERR_OOM; }
    int e = BJ_OK;
    for (int i = 0; i < n; i++) {
        if ((e = child_bbox(t, nd, i, &cb[i]))) { free(cb); free(grp); return e; }
    }

    /* Seeds: the most distant pair (largest union area). */
    double max_dist = -INFINITY;
    int s1 = 0, s2 = n > 1 ? 1 : 0;
    for (int i = 0; i < n; i++) {
        for (int j = i + 1; j < n; j++) {
            rbbox u = bbox_union(&cb[i], &cb[j]);
            double d = bbox_area(&u);
            if (d > max_dist) { max_dist = d; s1 = i; s2 = j; }
        }
    }

    for (int i = 0; i < n; i++) grp[i] = 0;
    grp[s1] = 1; grp[s2] = 2;
    rbbox gb1 = cb[s1], gb2 = cb[s2];
    int c1 = 1, c2 = 1;
    for (int i = 0; i < n; i++) {
        if (grp[i]) continue;
        double enl1 = gb1.valid ? bbox_enlargement(&gb1, &cb[i]) : 0;
        double enl2 = gb2.valid ? bbox_enlargement(&gb2, &cb[i]) : 0;
        int to1;
        if (enl1 < enl2) to1 = 1;
        else if (enl2 < enl1) to1 = 0;
        else to1 = (c1 <= c2);
        if (to1) { grp[i] = 1; gb1 = bbox_union(&gb1, &cb[i]); c1++; }
        else     { grp[i] = 2; gb2 = bbox_union(&gb2, &cb[i]); c2++; }
    }

    rnode n1, n2;
    node_init(&n1); node_init(&n2);
    if (nd->is_leaf) {
        rentry *e1 = (rentry *)malloc((size_t)c1 * sizeof(rentry));
        rentry *e2 = (rentry *)malloc((size_t)c2 * sizeof(rentry));
        if (!e1 || !e2) { free(e1); free(e2); free(cb); free(grp); return BJ_ERR_OOM; }
        int k1 = 0, k2 = 0;
        for (int i = 0; i < n; i++) {
            if (grp[i] == 1) e1[k1++] = nd->entries[i];
            else             e2[k2++] = nd->entries[i];
        }
        e = make_leaf(&n1, nd->id, e1, k1);
        if (!e) e = make_leaf(&n2, t->next_id++, e2, k2);
        free(e1); free(e2);
    } else {
        double *p1 = (double *)malloc((size_t)c1 * sizeof(double));
        double *p2 = (double *)malloc((size_t)c2 * sizeof(double));
        if (!p1 || !p2) { free(p1); free(p2); free(cb); free(grp); return BJ_ERR_OOM; }
        int k1 = 0, k2 = 0;
        for (int i = 0; i < n; i++) {
            if (grp[i] == 1) p1[k1++] = nd->children[i];
            else             p2[k2++] = nd->children[i];
        }
        e = make_internal(&n1, nd->id, p1, k1);
        if (!e) e = make_internal(&n2, t->next_id++, p2, k2);
        free(p1); free(p2);
    }
    free(cb); free(grp);
    if (e) { node_free(&n1); node_free(&n2); return e; }

    n1.bbox = gb1; n2.bbox = gb2;
    double lp, rp;
    e = save_node(t, &n1, &lp);
    if (!e) e = save_node(t, &n2, &rp);
    node_free(&n1); node_free(&n2);
    if (e) return e;
    out->split = 1; out->ptr = lp; out->ptr2 = rp;
    return BJ_OK;
}

/* ---- Insert (mirrors _insert / insert) ------------------------------ */

static int choose_subtree(rtree *t, const rnode *nd, const rbbox *bb, int *out_idx) {
    double min_enl = INFINITY, min_area = INFINITY;
    int best = -1;
    for (int i = 0; i < nd->n; i++) {
        rbbox cb;
        int e = child_bbox(t, nd, i, &cb);
        if (e) return e;
        double enl = bbox_enlargement(&cb, bb);
        double ar = bbox_area(&cb);
        if (enl < min_enl || (enl == min_enl && ar < min_area)) {
            min_enl = enl; min_area = ar; best = i;
        }
    }
    *out_idx = best;
    return BJ_OK;
}

static int insert_node(rtree *t, double ptr, const rentry *entry, ins_res *out) {
    memset(out, 0, sizeof(*out));
    rnode nd;
    int e = parse_node(t, ptr, &nd);
    if (e) return e;

    if (nd.is_leaf) {
        rentry *ne = (rentry *)realloc(nd.entries, (size_t)(nd.n + 1) * sizeof(rentry));
        if (!ne) { node_free(&nd); return BJ_ERR_OOM; }
        nd.entries = ne;
        nd.entries[nd.n] = *entry;
        nd.n++;
        if (nd.n > t->max_entries) {
            e = split_node(t, &nd, out);
        } else {
            e = compute_bbox(t, &nd);
            if (!e) { out->split = 0; e = save_node(t, &nd, &out->ptr); }
        }
        node_free(&nd);
        return e;
    }

    int idx;
    e = choose_subtree(t, &nd, &entry->bbox, &idx);
    if (e) { node_free(&nd); return e; }

    ins_res cr;
    e = insert_node(t, nd.children[idx], entry, &cr);
    if (e) { node_free(&nd); return e; }

    if (cr.split) {
        double *nc = (double *)realloc(nd.children, (size_t)(nd.n + 1) * sizeof(double));
        if (!nc) { node_free(&nd); return BJ_ERR_OOM; }
        nd.children = nc;
        nd.children[idx] = cr.ptr;
        nd.children[nd.n] = cr.ptr2;
        nd.n++;
        if (nd.n > t->max_entries) {
            e = split_node(t, &nd, out);
        } else {
            e = compute_bbox(t, &nd);
            if (!e) { out->split = 0; e = save_node(t, &nd, &out->ptr); }
        }
    } else {
        nd.children[idx] = cr.ptr;
        e = compute_bbox(t, &nd);
        if (!e) { out->split = 0; e = save_node(t, &nd, &out->ptr); }
    }
    node_free(&nd);
    return e;
}

int rtree_insert(rtree *t, double lat, double lng, const uint8_t *oid12) {
    rentry entry;
    memset(&entry, 0, sizeof(entry));
    entry.bbox.valid = 1;
    entry.bbox.min_lat = entry.bbox.max_lat = lat;
    entry.bbox.min_lng = entry.bbox.max_lng = lng;
    entry.lat = lat; entry.lng = lng;
    memcpy(entry.oid, oid12, 12);

    ins_res res;
    int e = insert_node(t, t->root, &entry, &res);
    if (e) return e;

    if (res.split) {
        double children[2] = { res.ptr, res.ptr2 };
        rnode nr;
        e = make_internal(&nr, t->next_id++, children, 2);
        if (e) return e;
        e = compute_bbox(t, &nr);
        if (!e) e = save_node(t, &nr, &t->root);
        node_free(&nr);
        if (e) return e;
    } else {
        t->root = res.ptr;
    }
    t->size += 1;
    return save_metadata(t);
}

/* ---- Remove (mirrors _remove / _handleUnderflow / remove) ----------- */

typedef struct {
    int   found;
    int   underflow;
    double ptr;      /* saved updated node */
    rnode node;      /* updated node contents (owned when found) */
} del_res;

static int oid_eq(const uint8_t *a, const uint8_t *b) { return memcmp(a, b, 12) == 0; }

/*
 * Redistribute or merge an underflowing child with a sibling. On success sets
 * *merged; when merged, out_children/out_count hold the parent's new child
 * pointer list (caller takes ownership of the malloc'd array).
 */
static int handle_underflow(rtree *t, const rnode *parent, int child_index,
                            const del_res *cr, double **out_children,
                            int *out_count, int *merged) {
    *merged = 0;
    /* Gather up to two siblings (prev then next). */
    int sib_idx[2]; rnode sib[2]; int nsib = 0;
    if (child_index > 0) {
        int e = parse_node(t, parent->children[child_index - 1], &sib[nsib]);
        if (e) return e;
        sib_idx[nsib] = child_index - 1; nsib++;
    }
    if (child_index < parent->n - 1) {
        int e = parse_node(t, parent->children[child_index + 1], &sib[nsib]);
        if (e) { for (int k = 0; k < nsib; k++) node_free(&sib[k]); return e; }
        sib_idx[nsib] = child_index + 1; nsib++;
    }

    int is_leaf = cr->node.is_leaf;
    int e = BJ_OK;

    /* Try to borrow from a sibling with spare children (redistribute). */
    for (int s = 0; s < nsib; s++) {
        if (sib[s].n <= t->min_entries) continue;
        int total = cr->node.n + sib[s].n;
        int mid = (total + 1) / 2;   /* ceil(total/2) */
        rnode nc1, nc2;
        node_init(&nc1); node_init(&nc2);
        if (is_leaf) {
            rentry *all = (rentry *)malloc((size_t)total * sizeof(rentry));
            if (!all) { e = BJ_ERR_OOM; goto done; }
            memcpy(all, cr->node.entries, (size_t)cr->node.n * sizeof(rentry));
            memcpy(all + cr->node.n, sib[s].entries, (size_t)sib[s].n * sizeof(rentry));
            e = make_leaf(&nc1, cr->node.id, all, mid);
            if (!e) e = make_leaf(&nc2, sib[s].id, all + mid, total - mid);
            free(all);
        } else {
            double *all = (double *)malloc((size_t)total * sizeof(double));
            if (!all) { e = BJ_ERR_OOM; goto done; }
            memcpy(all, cr->node.children, (size_t)cr->node.n * sizeof(double));
            memcpy(all + cr->node.n, sib[s].children, (size_t)sib[s].n * sizeof(double));
            e = make_internal(&nc1, cr->node.id, all, mid);
            if (!e) e = make_internal(&nc2, sib[s].id, all + mid, total - mid);
            free(all);
        }
        if (!e) e = compute_bbox(t, &nc1);
        if (!e) e = compute_bbox(t, &nc2);
        double p1 = 0, p2 = 0;
        if (!e) e = save_node(t, &nc1, &p1);
        if (!e) e = save_node(t, &nc2, &p2);
        node_free(&nc1); node_free(&nc2);
        if (e) goto done;

        double *newc = (double *)malloc((size_t)parent->n * sizeof(double));
        if (!newc) { e = BJ_ERR_OOM; goto done; }
        memcpy(newc, parent->children, (size_t)parent->n * sizeof(double));
        int lo = child_index < sib_idx[s] ? child_index : sib_idx[s];
        int hi = child_index < sib_idx[s] ? sib_idx[s] : child_index;
        newc[lo] = p1; newc[hi] = p2;
        *out_children = newc; *out_count = parent->n; *merged = 1;
        goto done;
    }

    /* Can't borrow: merge with the first sibling. */
    if (nsib > 0) {
        int s = 0;
        int total = cr->node.n + sib[s].n;
        rnode m;
        node_init(&m);
        if (is_leaf) {
            rentry *all = (rentry *)malloc((size_t)total * sizeof(rentry));
            if (!all) { e = BJ_ERR_OOM; goto done; }
            memcpy(all, cr->node.entries, (size_t)cr->node.n * sizeof(rentry));
            memcpy(all + cr->node.n, sib[s].entries, (size_t)sib[s].n * sizeof(rentry));
            e = make_leaf(&m, t->next_id++, all, total);
            free(all);
        } else {
            double *all = (double *)malloc((size_t)total * sizeof(double));
            if (!all) { e = BJ_ERR_OOM; goto done; }
            memcpy(all, cr->node.children, (size_t)cr->node.n * sizeof(double));
            memcpy(all + cr->node.n, sib[s].children, (size_t)sib[s].n * sizeof(double));
            e = make_internal(&m, t->next_id++, all, total);
            free(all);
        }
        if (!e) e = compute_bbox(t, &m);
        double mp = 0;
        if (!e) e = save_node(t, &m, &mp);
        node_free(&m);
        if (e) goto done;

        double *newc = (double *)malloc((size_t)parent->n * sizeof(double));
        if (!newc) { e = BJ_ERR_OOM; goto done; }
        int k = 0;
        for (int i = 0; i < parent->n; i++) {
            if (i == child_index || i == sib_idx[s]) continue;
            newc[k++] = parent->children[i];
        }
        newc[k++] = mp;
        *out_children = newc; *out_count = k; *merged = 1;
    }

done:
    for (int k = 0; k < nsib; k++) node_free(&sib[k]);
    return e;
}

static int remove_node(rtree *t, double ptr, const uint8_t *oid, del_res *out) {
    memset(out, 0, sizeof(*out));
    rnode nd;
    int e = parse_node(t, ptr, &nd);
    if (e) return e;

    if (nd.is_leaf) {
        int idx = -1;
        for (int i = 0; i < nd.n; i++) if (oid_eq(oid, nd.entries[i].oid)) { idx = i; break; }
        if (idx < 0) { out->found = 0; node_free(&nd); return BJ_OK; }
        /* Remove entry idx in place. */
        for (int i = idx; i < nd.n - 1; i++) nd.entries[i] = nd.entries[i + 1];
        nd.n--;
        e = compute_bbox(t, &nd);
        if (!e) e = save_node(t, &nd, &out->ptr);
        if (e) { node_free(&nd); return e; }
        out->found = 1;
        out->underflow = (nd.n < t->min_entries && nd.n > 0);
        out->node = nd;   /* transfer ownership */
        return BJ_OK;
    }

    /* Internal node: find the child containing the entry. */
    double *updated = (double *)malloc((size_t)nd.n * sizeof(double));
    if (!updated) { node_free(&nd); return BJ_ERR_OOM; }
    memcpy(updated, nd.children, (size_t)nd.n * sizeof(double));

    for (int i = 0; i < nd.n; i++) {
        del_res cr;
        e = remove_node(t, updated[i], oid, &cr);
        if (e) { free(updated); node_free(&nd); return e; }
        if (!cr.found) { continue; }

        double *newc = NULL; int newcount = 0, merged = 0;
        if (cr.underflow) {
            e = handle_underflow(t, &nd, i, &cr, &newc, &newcount, &merged);
            if (e) { node_free(&cr.node); free(updated); node_free(&nd); return e; }
            if (!merged) updated[i] = cr.ptr;
        } else {
            updated[i] = cr.ptr;
        }
        node_free(&cr.node);

        rnode un;
        double *use = merged ? newc : updated;
        int count = merged ? newcount : nd.n;
        e = make_internal(&un, nd.id, use, count);   /* deep-copies `use` */
        free(updated);
        free(newc);                                  /* NULL-safe when not merged */
        if (!e) e = compute_bbox(t, &un);
        if (!e) e = save_node(t, &un, &out->ptr);
        if (e) { node_free(&un); node_free(&nd); return e; }

        node_free(&nd);
        out->found = 1;
        out->underflow = (un.n < t->min_entries && un.n > 0);
        out->node = un;   /* transfer ownership */
        return BJ_OK;
    }

    free(updated);
    node_free(&nd);
    out->found = 0;
    return BJ_OK;
}

int rtree_remove(rtree *t, const uint8_t *oid12, int *removed) {
    del_res res;
    int e = remove_node(t, t->root, oid12, &res);
    if (e) return e;
    if (!res.found) { *removed = 0; return BJ_OK; }

    /* Collapse an internal root left with a single child down to that child. */
    if (res.underflow && !res.node.is_leaf && res.node.n == 1) {
        t->root = res.node.children[0];
    } else {
        t->root = res.ptr;
    }
    node_free(&res.node);
    t->size -= 1;
    *removed = 1;
    return save_metadata(t);
}

/* ---- Clear ---------------------------------------------------------- */

int rtree_clear(rtree *t) {
    rnode root;
    int e = make_leaf(&root, t->next_id++, NULL, 0);
    if (e) return e;
    root.bbox.valid = 0;
    e = save_node(t, &root, &t->root);
    node_free(&root);
    if (e) return e;
    t->size = 0;
    return save_metadata(t);
}

/* ---- Search --------------------------------------------------------- */

static int search_rec(rtree *t, double ptr, const rbbox *q, bj_builder *b) {
    rnode nd;
    int e = parse_node(t, ptr, &nd);
    if (e) return e;
    if (!nd.bbox.valid || !bbox_intersects(q, &nd.bbox)) { node_free(&nd); return BJ_OK; }

    if (nd.is_leaf) {
        for (int i = 0; i < nd.n; i++) {
            if (!bbox_intersects(q, &nd.entries[i].bbox)) continue;
            bj_begin_object(b);
            bj_put_key(b, (const uint8_t *)"objectId", 8); bj_put_oid(b, nd.entries[i].oid);
            bj_put_key(b, (const uint8_t *)"lat", 3);       emit_number(b, nd.entries[i].lat);
            bj_put_key(b, (const uint8_t *)"lng", 3);       emit_number(b, nd.entries[i].lng);
            bj_end_object(b);
        }
    } else {
        for (int i = 0; i < nd.n; i++) {
            e = search_rec(t, nd.children[i], q, b);
            if (e) { node_free(&nd); return e; }
        }
    }
    node_free(&nd);
    return bj_builder_error(b);
}

int rtree_search_bbox(rtree *t, double min_lat, double max_lat,
                      double min_lng, double max_lng,
                      const uint8_t **out_ptr, size_t *out_len) {
    rbbox q = { 1, min_lat, max_lat, min_lng, max_lng };
    bj_builder *b = bj_builder_new();
    if (!b) return BJ_ERR_OOM;
    bj_begin_array(b);
    int e = search_rec(t, t->root, &q, b);
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

/* ---- Compaction ----------------------------------------------------- */

typedef struct { double *olds, *news; int n, cap; } ptrmap;

static int ptrmap_get(ptrmap *m, double old, double *found) {
    for (int i = 0; i < m->n; i++) if (m->olds[i] == old) { *found = m->news[i]; return 1; }
    return 0;
}
static int ptrmap_put(ptrmap *m, double old, double neu) {
    if (m->n == m->cap) {
        int nc = m->cap ? m->cap * 2 : 16;
        double *no = (double *)realloc(m->olds, (size_t)nc * sizeof(double));
        double *nn = (double *)realloc(m->news, (size_t)nc * sizeof(double));
        if (!no || !nn) { free(no != m->olds ? no : NULL); return BJ_ERR_OOM; }
        m->olds = no; m->news = nn; m->cap = nc;
    }
    m->olds[m->n] = old; m->news[m->n] = neu; m->n++;
    return BJ_OK;
}

static int clone_node(rtree *t, dbuf *dst, ptrmap *m, double old_off, double *new_off) {
    if (ptrmap_get(m, old_off, new_off)) return BJ_OK;
    rnode nd;
    int e = parse_node(t, old_off, &nd);
    if (e) return e;
    if (!nd.is_leaf) {
        for (int i = 0; i < nd.n; i++) {
            double nc;
            e = clone_node(t, dst, m, nd.children[i], &nc);
            if (e) { node_free(&nd); return e; }
            nd.children[i] = nc;
        }
    }
    e = encode_node(t, &nd, dst, new_off);
    node_free(&nd);
    if (e) return e;
    return ptrmap_put(m, old_off, *new_off);
}

int rtree_compact(rtree *t, const uint8_t **out_ptr, size_t *out_len) {
    dbuf dst; memset(&dst, 0, sizeof(dst));
    ptrmap m; memset(&m, 0, sizeof(m));
    double new_root = 0;
    int e = clone_node(t, &dst, &m, t->root, &new_root);
    if (!e) e = encode_metadata(t, &dst, new_root);
    if (!e) e = set_out(t, dst.data, dst.len);
    free(dst.data);
    free(m.olds); free(m.news);
    if (e) return e;
    *out_ptr = t->out.data; *out_len = t->out.len;
    return BJ_OK;
}

/* ---- Lifecycle & accessors ------------------------------------------ */

static int min_entries_for(int max_entries) {
    int half = (max_entries + 1) / 2;   /* ceil(max/2) */
    return half < 2 ? 2 : half;
}

rtree *rtree_create(int max_entries) {
    if (max_entries < 2) return NULL;
    rtree *t = (rtree *)calloc(1, sizeof(rtree));
    if (!t) return NULL;
    t->bld = bj_builder_new();
    if (!t->bld) { free(t); return NULL; }
    t->max_entries = max_entries;
    t->min_entries = min_entries_for(max_entries);
    t->next_id = 1;
    t->size = 0;

    rnode root;
    if (make_leaf(&root, 0, NULL, 0)) { rtree_free(t); return NULL; }
    root.bbox.valid = 0;
    if (save_node(t, &root, &t->root)) { node_free(&root); rtree_free(t); return NULL; }
    node_free(&root);
    if (save_metadata(t)) { rtree_free(t); return NULL; }
    return t;
}

rtree *rtree_load(const uint8_t *bytes, size_t len) {
    if (len < RT_METADATA_SIZE) return NULL;
    rtree *t = (rtree *)calloc(1, sizeof(rtree));
    if (!t) return NULL;
    t->bld = bj_builder_new();
    if (!t->bld) { free(t); return NULL; }
    t->img.data = (uint8_t *)malloc(len);
    if (!t->img.data) { rtree_free(t); return NULL; }
    memcpy(t->img.data, bytes, len);
    t->img.len = len;
    t->img.cap = len;
    if (parse_metadata(t)) { rtree_free(t); return NULL; }
    return t;
}

void rtree_free(rtree *t) {
    if (!t) return;
    bj_builder_free(t->bld);
    free(t->img.data);
    free(t->out.data);
    free(t);
}

double         rtree_size(const rtree *t)        { return t->size; }
int            rtree_max_entries(const rtree *t) { return t->max_entries; }
const uint8_t *rtree_image(const rtree *t, size_t *len) { if (len) *len = t->img.len; return t->img.data; }
const uint8_t *rtree_out(const rtree *t, size_t *len)   { if (len) *len = t->out.len; return t->out.data; }
