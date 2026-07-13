/*
 * rtree.c — C port of src/rtree.js. See rtree.h.
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
#include "rtree.h"
#include "bjfile.h"
#include "bjcursor.h"
#include "dbuf.h"
#include "geo.h"

#include <stdlib.h>
#include <string.h>
#include <math.h>

/* Fixed on-wire size of the metadata object (matches METADATA_SIZE in
 * rtree.js): 6 fields, all fixed-width INT/POINTER values. */
#define RT_METADATA_SIZE 135

/* Cap on tree depth while walking file-provided pointers: a corrupt file
 * whose child pointer loops back to an ancestor must error, not recurse
 * forever. Vastly deeper than any real tree (height is O(log n)). */
#define RT_MAX_DEPTH 128

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
    uint64_t  id;
    int       is_leaf;
    rbbox     bbox;          /* node bbox (valid flag) */
    int       n;             /* number of children */
    rentry   *entries;       /* leaf: n entries */
    uint64_t *children;      /* internal: n pointer offsets */
    rbbox    *child_bboxes;  /* internal: n child boxes when has_cb */
    int       has_cb;        /* childBBoxes present (see below)     */
} rnode;

/*
 * Internal nodes additionally persist a "childBBoxes" array — one bounding
 * box per child, in child order — so choose-subtree, bbox recomputation,
 * node splitting and search pruning never need to load child nodes. The
 * field is optional on read: nodes written by the JS reference (which
 * ignores the field and keeps its own behavior) or by older builds lack it,
 * and fall back to loading children; they are upgraded whenever rewritten.
 */

struct rtree {
    bjfile      f;           /* backing file                   */
    dbuf        out;         /* last op output                 */
    bj_builder *bld;         /* reused for node/metadata saves */
    uint64_t    root;
    uint64_t    next_id;
    int64_t     size;
    int         max_entries;
    int         min_entries;
};

static int set_out(rtree *t, const uint8_t *b, size_t n) {
    t->out.len = 0;
    return dbuf_put(&t->out, b, n);
}

/* ---- Wire-format readers (structure-specific; primitives in bjcursor.h) */

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
    free(n->child_bboxes);
    node_init(n);
}

/* Decode the node object stored at `offset` in the file into `out`. */
static int parse_node(rtree *t, uint64_t offset, rnode *out) {
    node_init(out);
    const uint8_t *rec; size_t rec_len;
    int err = bjfile_read_record(&t->f, offset, &rec, &rec_len);
    if (err) return err;
    cur c = { rec, rec_len, 0 };

    uint32_t count;
    int e = object_begin(&c, &count);
    if (e) return e;

    int cb_n = -1;     /* childBBoxes element count, -1 = field absent */
    int arr_leaf = -1; /* what the children array held, -1 = empty/absent */
    for (uint32_t i = 0; i < count; i++) {
        const uint8_t *kn; uint32_t klen;
        if ((e = take_key(&c, &kn, &klen))) { node_free(out); return e; }
        if (name_eq(kn, klen, "id")) {
            if ((e = read_u64(&c, &out->id))) { node_free(out); return e; }
        } else if (name_eq(kn, klen, "isLeaf")) {
            if ((e = read_bool(&c, &out->is_leaf))) { node_free(out); return e; }
        } else if (name_eq(kn, klen, "bbox")) {
            if ((e = read_bbox(&c, &out->bbox))) { node_free(out); return e; }
        } else if (name_eq(kn, klen, "childBBoxes")) {
            uint32_t n;
            if ((e = array_begin(&c, &n))) { node_free(out); return e; }
            free(out->child_bboxes);
            out->child_bboxes = NULL;
            if (n) {
                out->child_bboxes = (rbbox *)calloc(n, sizeof(rbbox));
                if (!out->child_bboxes) { node_free(out); return BJ_ERR_OOM; }
            }
            for (uint32_t j = 0; j < n; j++) {
                if ((e = read_bbox(&c, &out->child_bboxes[j]))) { node_free(out); return e; }
            }
            cb_n = (int)n;
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
                arr_leaf = is_leaf;
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
                    out->children = (uint64_t *)malloc((size_t)n * sizeof(uint64_t));
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
    /* The isLeaf flag must agree with what the children array actually held
     * (a corrupt/hostile file could claim internal over OBJECT entries and
     * send later code through the NULL children array, or vice versa). */
    if (arr_leaf >= 0 && arr_leaf != out->is_leaf) {
        node_free(out);
        return BJ_ERR_STATE;
    }
    /* childBBoxes is trusted only when it matches the child count exactly. */
    out->has_cb = (!out->is_leaf && cb_n == out->n && cb_n >= 0);
    if (!out->has_cb) { free(out->child_bboxes); out->child_bboxes = NULL; }
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
static int encode_node(rtree *t, const rnode *nd, bjfile *dst, uint64_t *off) {
    bj_builder *b = t->bld;
    bj_builder_reset(b);
    bj_begin_object(b);
    bj_put_key(b, (const uint8_t *)"id", 2);     bj_put_int(b, (int64_t)nd->id);
    bj_put_key(b, (const uint8_t *)"isLeaf", 6); bj_put_bool(b, nd->is_leaf);
    bj_put_key(b, (const uint8_t *)"children", 8);
    bj_begin_array(b);
    if (nd->is_leaf) {
        for (int i = 0; i < nd->n; i++) emit_entry(b, &nd->entries[i]);
    } else {
        for (int i = 0; i < nd->n; i++) bj_put_pointer(b, nd->children[i]);
    }
    bj_end_array(b);
    if (!nd->is_leaf && nd->has_cb) {
        bj_put_key(b, (const uint8_t *)"childBBoxes", 11);
        bj_begin_array(b);
        for (int i = 0; i < nd->n; i++) emit_bbox(b, &nd->child_bboxes[i]);
        bj_end_array(b);
    }
    bj_put_key(b, (const uint8_t *)"bbox", 4);    emit_bbox(b, &nd->bbox);
    bj_end_object(b);

    int e = bj_builder_error(b);
    if (e) return e;
    size_t len;
    const uint8_t *d = bj_builder_data(b, &len);
    if (!d) return BJ_ERR_STATE;
    return bjfile_append(dst, d, len, off);
}

/* Append `nd` to the tree's live file. */
static int save_node(rtree *t, const rnode *nd, uint64_t *off) {
    return encode_node(t, nd, &t->f, off);
}

/* ---- Metadata ------------------------------------------------------- */

static int encode_metadata(rtree *t, bjfile *dst, uint64_t root) {
    bj_builder *b = t->bld;
    bj_builder_reset(b);
    bj_begin_object(b);
    bj_put_key(b, (const uint8_t *)"version", 7);      bj_put_int(b, 1);
    bj_put_key(b, (const uint8_t *)"maxEntries", 10);  bj_put_int(b, t->max_entries);
    bj_put_key(b, (const uint8_t *)"minEntries", 10);  bj_put_int(b, t->min_entries);
    bj_put_key(b, (const uint8_t *)"size", 4);         bj_put_int(b, t->size);
    bj_put_key(b, (const uint8_t *)"rootPointer", 11); bj_put_pointer(b, root);
    bj_put_key(b, (const uint8_t *)"nextId", 6);       bj_put_int(b, (int64_t)t->next_id);
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
static int save_metadata(rtree *t) { return encode_metadata(t, &t->f, t->root); }

typedef struct {
    uint64_t root, next_id;
    int64_t  size;
    int      max_entries, min_entries;
    int      have_root;
} rt_meta;

/* Parse a metadata record's fields out of its bytes. */
static int parse_meta_rec(const uint8_t *rec, size_t rec_len, rt_meta *m) {
    memset(m, 0, sizeof(*m));
    cur c = { rec, rec_len, 0 };
    uint32_t count;
    int e = object_begin(&c, &count);
    if (e) return e;
    for (uint32_t i = 0; i < count; i++) {
        const uint8_t *kn; uint32_t klen;
        if ((e = take_key(&c, &kn, &klen))) return e;
        uint64_t u;
        if      (name_eq(kn, klen, "maxEntries")) { if ((e = read_int31(&c, &m->max_entries))) return e; }
        else if (name_eq(kn, klen, "minEntries")) { if ((e = read_int31(&c, &m->min_entries))) return e; }
        else if (name_eq(kn, klen, "size"))       { if ((e = read_u64(&c, &u))) return e; m->size = (int64_t)u; }
        else if (name_eq(kn, klen, "nextId"))     { if ((e = read_u64(&c, &m->next_id))) return e; }
        else if (name_eq(kn, klen, "rootPointer")){ if ((e = read_pointer(&c, &m->root))) return e; m->have_root = 1; }
        else                                      { if ((e = skip_value(&c))) return e; }
    }
    return m->have_root ? BJ_OK : BJ_ERR_STATE;
}

/* Range-check metadata fields. `before` is the metadata record's own offset:
 * the root it points at must lie strictly before it. */
static int meta_valid(const rt_meta *m, uint64_t before) {
    if (m->max_entries < 2) return 0;
    if (m->min_entries < 1 || m->min_entries > m->max_entries) return 0;
    if (m->size < 0) return 0;
    if (m->root >= before) return 0;
    return 1;
}

static void meta_apply(rtree *t, const rt_meta *m) {
    t->max_entries = m->max_entries;
    t->min_entries = m->min_entries;
    t->size = m->size;
    t->next_id = m->next_id;
    t->root = m->root;
}

/* ---- Node construction & bbox --------------------------------------- */

/* The bounding box of child `i`: leaf children carry their own point box,
 * internal children theirs in childBBoxes; only legacy nodes (no
 * childBBoxes on disk) fall back to loading the referenced node. */
static int child_bbox(rtree *t, const rnode *nd, int i, rbbox *out) {
    if (nd->is_leaf) { *out = nd->entries[i].bbox; return BJ_OK; }
    if (nd->has_cb) { *out = nd->child_bboxes[i]; return BJ_OK; }
    rnode ch;
    int e = parse_node(t, nd->children[i], &ch);
    if (e) return e;
    *out = ch.bbox;
    node_free(&ch);
    return BJ_OK;
}

/* Materialize childBBoxes on a legacy internal node (one child load each);
 * the node carries them from its next save onward. */
static int ensure_child_bboxes(rtree *t, rnode *nd) {
    if (nd->is_leaf || nd->has_cb) return BJ_OK;
    rbbox *cb = NULL;
    if (nd->n) {
        cb = (rbbox *)calloc((size_t)nd->n, sizeof(rbbox));
        if (!cb) return BJ_ERR_OOM;
    }
    for (int i = 0; i < nd->n; i++) {
        rnode ch;
        int e = parse_node(t, nd->children[i], &ch);
        if (e) { free(cb); return e; }
        cb[i] = ch.bbox;
        node_free(&ch);
    }
    free(nd->child_bboxes);
    nd->child_bboxes = cb;
    nd->has_cb = 1;
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

static int make_leaf(rnode *out, uint64_t id, const rentry *entries, int n) {
    node_init(out);
    out->id = id; out->is_leaf = 1; out->n = n;
    if (n) {
        out->entries = (rentry *)malloc((size_t)n * sizeof(rentry));
        if (!out->entries) return BJ_ERR_OOM;
        memcpy(out->entries, entries, (size_t)n * sizeof(rentry));
    }
    return BJ_OK;
}
/* Build an internal node from parallel child-pointer and child-bbox arrays. */
static int make_internal(rnode *out, uint64_t id, const uint64_t *children,
                         const rbbox *cbs, int n) {
    node_init(out);
    out->id = id; out->is_leaf = 0; out->n = n;
    if (n) {
        out->children = (uint64_t *)malloc((size_t)n * sizeof(uint64_t));
        out->child_bboxes = (rbbox *)malloc((size_t)n * sizeof(rbbox));
        if (!out->children || !out->child_bboxes) return BJ_ERR_OOM;
        memcpy(out->children, children, (size_t)n * sizeof(uint64_t));
        memcpy(out->child_bboxes, cbs, (size_t)n * sizeof(rbbox));
    }
    out->has_cb = 1;
    return BJ_OK;
}

/* ---- Split (mirrors _split) ----------------------------------------- */

typedef struct {
    int      split;
    uint64_t ptr, ptr2;    /* saved node offset(s)                 */
    rbbox    bbox, bbox2;  /* their bounding boxes (for the parent) */
} ins_res;

/*
 * Split the overflowing in-memory `nd` into two saved nodes with Guttman's
 * quadratic split. An R*-style split (margin-minimizing axis choice +
 * least-overlap distribution, including the paper's relaxed 40% fill)
 * was implemented and benchmarked on clustered point data — 6,000 points
 * in 30 clusters, 500 cluster-sized bbox queries, host reads counted —
 * and measured at parity or slightly worse (fanout 9: 10.7 reads/query
 * quadratic vs 12.2 R* under this format's ceil(max/2) min-fill, 10.6 at
 * 40% fill; fanout 16: 7.6 vs 8.0 vs 7.5). With childBBoxes pruning the
 * descent, split topology isn't the bottleneck for point entries, so the
 * simpler split stays.
 */
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
        uint64_t *p1 = (uint64_t *)malloc((size_t)c1 * sizeof(uint64_t));
        uint64_t *p2 = (uint64_t *)malloc((size_t)c2 * sizeof(uint64_t));
        rbbox  *b1 = (rbbox *)malloc((size_t)c1 * sizeof(rbbox));
        rbbox  *b2 = (rbbox *)malloc((size_t)c2 * sizeof(rbbox));
        if (!p1 || !p2 || !b1 || !b2) {
            free(p1); free(p2); free(b1); free(b2); free(cb); free(grp);
            return BJ_ERR_OOM;
        }
        int k1 = 0, k2 = 0;
        for (int i = 0; i < n; i++) {
            if (grp[i] == 1) { b1[k1] = cb[i]; p1[k1++] = nd->children[i]; }
            else             { b2[k2] = cb[i]; p2[k2++] = nd->children[i]; }
        }
        e = make_internal(&n1, nd->id, p1, b1, k1);
        if (!e) e = make_internal(&n2, t->next_id++, p2, b2, k2);
        free(p1); free(p2); free(b1); free(b2);
    }
    free(cb); free(grp);
    if (e) { node_free(&n1); node_free(&n2); return e; }

    n1.bbox = gb1; n2.bbox = gb2;
    uint64_t lp, rp;
    e = save_node(t, &n1, &lp);
    if (!e) e = save_node(t, &n2, &rp);
    node_free(&n1); node_free(&n2);
    if (e) return e;
    out->split = 1;
    out->ptr = lp;   out->bbox = gb1;
    out->ptr2 = rp;  out->bbox2 = gb2;
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

static int insert_node(rtree *t, uint64_t ptr, const rentry *entry, ins_res *out,
                       int depth) {
    memset(out, 0, sizeof(*out));
    if (depth > RT_MAX_DEPTH) return BJ_ERR_DEPTH;
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
            if (!e) { out->split = 0; e = save_node(t, &nd, &out->ptr); out->bbox = nd.bbox; }
        }
        node_free(&nd);
        return e;
    }

    /* Upgrade legacy nodes once — the rewrite below persists the boxes. */
    e = ensure_child_bboxes(t, &nd);
    if (e) { node_free(&nd); return e; }

    int idx;
    e = choose_subtree(t, &nd, &entry->bbox, &idx);
    if (!e && idx < 0) e = BJ_ERR_STATE;   /* childless internal node */
    if (e) { node_free(&nd); return e; }

    ins_res cr;
    e = insert_node(t, nd.children[idx], entry, &cr, depth + 1);
    if (e) { node_free(&nd); return e; }

    if (cr.split) {
        uint64_t *nc = (uint64_t *)realloc(nd.children, (size_t)(nd.n + 1) * sizeof(uint64_t));
        if (!nc) { node_free(&nd); return BJ_ERR_OOM; }
        nd.children = nc;
        rbbox *ncb = (rbbox *)realloc(nd.child_bboxes, (size_t)(nd.n + 1) * sizeof(rbbox));
        if (!ncb) { node_free(&nd); return BJ_ERR_OOM; }
        nd.child_bboxes = ncb;
        nd.children[idx] = cr.ptr;      nd.child_bboxes[idx] = cr.bbox;
        nd.children[nd.n] = cr.ptr2;    nd.child_bboxes[nd.n] = cr.bbox2;
        nd.n++;
        if (nd.n > t->max_entries) {
            e = split_node(t, &nd, out);
        } else {
            e = compute_bbox(t, &nd);
            if (!e) { out->split = 0; e = save_node(t, &nd, &out->ptr); out->bbox = nd.bbox; }
        }
    } else {
        nd.children[idx] = cr.ptr;
        nd.child_bboxes[idx] = cr.bbox;
        e = compute_bbox(t, &nd);
        if (!e) { out->split = 0; e = save_node(t, &nd, &out->ptr); out->bbox = nd.bbox; }
    }
    node_free(&nd);
    return e;
}

static int insert_root(rtree *t, double lat, double lng, const uint8_t *oid12) {
    rentry entry;
    memset(&entry, 0, sizeof(entry));
    entry.bbox.valid = 1;
    entry.bbox.min_lat = entry.bbox.max_lat = lat;
    entry.bbox.min_lng = entry.bbox.max_lng = lng;
    entry.lat = lat; entry.lng = lng;
    memcpy(entry.oid, oid12, 12);

    ins_res res;
    int e = insert_node(t, t->root, &entry, &res, 0);
    if (e) return e;

    if (res.split) {
        uint64_t children[2] = { res.ptr, res.ptr2 };
        rbbox  cbs[2] = { res.bbox, res.bbox2 };
        rnode nr;
        e = make_internal(&nr, t->next_id++, children, cbs, 2);
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

/*
 * Public mutating operations commit all of the operation's appended records
 * with a single host write; on any failure the pending bytes are dropped and
 * the in-memory state is rolled back, leaving the file untouched.
 */
int rtree_insert(rtree *t, double lat, double lng, const uint8_t *oid12) {
    uint64_t root = t->root, next_id = t->next_id;
    int64_t size = t->size;
    int e = insert_root(t, lat, lng, oid12);
    if (!e) e = bjfile_commit(&t->f);
    if (e) {
        bjfile_discard(&t->f);
        t->root = root; t->next_id = next_id; t->size = size;
    }
    return e;
}

/* ---- Remove (mirrors _remove / _handleUnderflow / remove) ----------- */

typedef struct {
    int      found;
    int      underflow;
    uint64_t ptr;    /* saved updated node */
    rnode    node;   /* updated node contents (owned when found) */
} del_res;

static int oid_eq(const uint8_t *a, const uint8_t *b) { return memcmp(a, b, 12) == 0; }

/*
 * Redistribute or merge an underflowing child with a sibling. On success sets
 * *merged; when merged, out_children/out_bboxes/out_count hold the parent's
 * new child pointer and bbox lists (caller takes ownership of both arrays).
 * The parent must have its childBBoxes materialized (ensure_child_bboxes).
 */
static int handle_underflow(rtree *t, const rnode *parent, int child_index,
                            del_res *cr, uint64_t **out_children,
                            rbbox **out_bboxes, int *out_count, int *merged) {
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

    /* Internal children combine their childBBoxes lists; materialize them on
     * any legacy participant first. */
    if (!is_leaf) {
        e = ensure_child_bboxes(t, &cr->node);
        for (int s = 0; s < nsib && !e; s++) e = ensure_child_bboxes(t, &sib[s]);
        if (e) goto done;
    }

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
            uint64_t *all = (uint64_t *)malloc((size_t)total * sizeof(uint64_t));
            rbbox *allb = (rbbox *)malloc((size_t)total * sizeof(rbbox));
            if (!all || !allb) { free(all); free(allb); e = BJ_ERR_OOM; goto done; }
            memcpy(all, cr->node.children, (size_t)cr->node.n * sizeof(uint64_t));
            memcpy(all + cr->node.n, sib[s].children, (size_t)sib[s].n * sizeof(uint64_t));
            memcpy(allb, cr->node.child_bboxes, (size_t)cr->node.n * sizeof(rbbox));
            memcpy(allb + cr->node.n, sib[s].child_bboxes, (size_t)sib[s].n * sizeof(rbbox));
            e = make_internal(&nc1, cr->node.id, all, allb, mid);
            if (!e) e = make_internal(&nc2, sib[s].id, all + mid, allb + mid, total - mid);
            free(all); free(allb);
        }
        if (!e) e = compute_bbox(t, &nc1);
        if (!e) e = compute_bbox(t, &nc2);
        rbbox bb1 = nc1.bbox, bb2 = nc2.bbox;
        uint64_t p1 = 0, p2 = 0;
        if (!e) e = save_node(t, &nc1, &p1);
        if (!e) e = save_node(t, &nc2, &p2);
        node_free(&nc1); node_free(&nc2);
        if (e) goto done;

        uint64_t *newc = (uint64_t *)malloc((size_t)parent->n * sizeof(uint64_t));
        rbbox *newb = (rbbox *)malloc((size_t)parent->n * sizeof(rbbox));
        if (!newc || !newb) { free(newc); free(newb); e = BJ_ERR_OOM; goto done; }
        memcpy(newc, parent->children, (size_t)parent->n * sizeof(uint64_t));
        memcpy(newb, parent->child_bboxes, (size_t)parent->n * sizeof(rbbox));
        int lo = child_index < sib_idx[s] ? child_index : sib_idx[s];
        int hi = child_index < sib_idx[s] ? sib_idx[s] : child_index;
        newc[lo] = p1; newb[lo] = bb1;
        newc[hi] = p2; newb[hi] = bb2;
        *out_children = newc; *out_bboxes = newb; *out_count = parent->n; *merged = 1;
        goto done;
    }

    /* Can't borrow: merge with the first sibling. When max_entries == 2 the
     * clamped min equals max, so the merged node can exceed capacity — split
     * it back in two instead of saving an oversized node. */
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
            uint64_t *all = (uint64_t *)malloc((size_t)total * sizeof(uint64_t));
            rbbox *allb = (rbbox *)malloc((size_t)total * sizeof(rbbox));
            if (!all || !allb) { free(all); free(allb); e = BJ_ERR_OOM; goto done; }
            memcpy(all, cr->node.children, (size_t)cr->node.n * sizeof(uint64_t));
            memcpy(all + cr->node.n, sib[s].children, (size_t)sib[s].n * sizeof(uint64_t));
            memcpy(allb, cr->node.child_bboxes, (size_t)cr->node.n * sizeof(rbbox));
            memcpy(allb + cr->node.n, sib[s].child_bboxes, (size_t)sib[s].n * sizeof(rbbox));
            e = make_internal(&m, t->next_id++, all, allb, total);
            free(all); free(allb);
        }
        if (!e) e = compute_bbox(t, &m);
        rbbox mb = m.bbox, mb2;
        uint64_t mp = 0, mp2 = 0;
        int halves = 1;
        if (!e && total > t->max_entries) {
            ins_res sp;
            e = split_node(t, &m, &sp);
            if (!e) { mp = sp.ptr; mb = sp.bbox; mp2 = sp.ptr2; mb2 = sp.bbox2; halves = 2; }
        } else if (!e) {
            e = save_node(t, &m, &mp);
        }
        node_free(&m);
        if (e) goto done;

        /* Two slots leave, one or two arrive: parent->n slots suffice. */
        uint64_t *newc = (uint64_t *)malloc((size_t)parent->n * sizeof(uint64_t));
        rbbox *newb = (rbbox *)malloc((size_t)parent->n * sizeof(rbbox));
        if (!newc || !newb) { free(newc); free(newb); e = BJ_ERR_OOM; goto done; }
        int k = 0;
        for (int i = 0; i < parent->n; i++) {
            if (i == child_index || i == sib_idx[s]) continue;
            newb[k] = parent->child_bboxes[i];
            newc[k++] = parent->children[i];
        }
        newb[k] = mb;
        newc[k++] = mp;
        if (halves == 2) {
            newb[k] = mb2;
            newc[k++] = mp2;
        }
        *out_children = newc; *out_bboxes = newb; *out_count = k; *merged = 1;
    }

done:
    for (int k = 0; k < nsib; k++) node_free(&sib[k]);
    return e;
}

/* `q` (the removed entry's point, when the caller knows it) prunes the
 * descent: subtrees whose bbox cannot contain the point are skipped instead
 * of probed. NULL falls back to the in-order full probe. */
static int remove_node(rtree *t, uint64_t ptr, const uint8_t *oid,
                       const rbbox *q, del_res *out, int depth) {
    memset(out, 0, sizeof(*out));
    if (depth > RT_MAX_DEPTH) return BJ_ERR_DEPTH;
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
    uint64_t *updated = (uint64_t *)malloc((size_t)nd.n * sizeof(uint64_t));
    if (!updated) { node_free(&nd); return BJ_ERR_OOM; }
    memcpy(updated, nd.children, (size_t)nd.n * sizeof(uint64_t));

    for (int i = 0; i < nd.n; i++) {
        del_res cr;
        if (q) {
            rbbox cb;
            e = child_bbox(t, &nd, i, &cb);
            if (e) { free(updated); node_free(&nd); return e; }
            if (!cb.valid || !bbox_intersects(q, &cb)) continue;
        }
        e = remove_node(t, updated[i], oid, q, &cr, depth + 1);
        if (e) { free(updated); node_free(&nd); return e; }
        if (!cr.found) { continue; }

        /* This node will be rewritten: materialize its child boxes so the
         * update below (and the saved copy) carries them. */
        e = ensure_child_bboxes(t, &nd);
        if (e) { node_free(&cr.node); free(updated); node_free(&nd); return e; }
        rbbox *updated_b = (rbbox *)malloc((size_t)nd.n * sizeof(rbbox));
        if (!updated_b) { node_free(&cr.node); free(updated); node_free(&nd); return BJ_ERR_OOM; }
        memcpy(updated_b, nd.child_bboxes, (size_t)nd.n * sizeof(rbbox));

        uint64_t *newc = NULL; rbbox *newb = NULL; int newcount = 0, merged = 0;
        if (cr.underflow) {
            e = handle_underflow(t, &nd, i, &cr, &newc, &newb, &newcount, &merged);
            if (e) { node_free(&cr.node); free(updated); free(updated_b); node_free(&nd); return e; }
            if (!merged) { updated[i] = cr.ptr; updated_b[i] = cr.node.bbox; }
        } else {
            updated[i] = cr.ptr;
            updated_b[i] = cr.node.bbox;
        }
        node_free(&cr.node);

        rnode un;
        uint64_t *use = merged ? newc : updated;
        rbbox *useb = merged ? newb : updated_b;
        int count = merged ? newcount : nd.n;
        e = make_internal(&un, nd.id, use, useb, count);   /* deep-copies */
        free(updated); free(updated_b);
        free(newc); free(newb);                 /* NULL-safe when not merged */
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

static int remove_root(rtree *t, const uint8_t *oid12, const rbbox *q, int *removed) {
    del_res res;
    int e = remove_node(t, t->root, oid12, q, &res, 0);
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

static int remove_with(rtree *t, const uint8_t *oid12, const rbbox *q, int *removed) {
    uint64_t root = t->root, next_id = t->next_id;
    int64_t size = t->size;
    int e = remove_root(t, oid12, q, removed);
    if (!e) e = bjfile_commit(&t->f);
    if (e) {
        bjfile_discard(&t->f);
        t->root = root; t->next_id = next_id; t->size = size;
    }
    return e;
}

int rtree_remove(rtree *t, const uint8_t *oid12, int *removed) {
    return remove_with(t, oid12, NULL, removed);
}

int rtree_remove_at(rtree *t, double lat, double lng, const uint8_t *oid12,
                    int *removed) {
    rbbox q = { 1, lat, lat, lng, lng };
    return remove_with(t, oid12, &q, removed);
}

/* ---- Clear ---------------------------------------------------------- */

int rtree_clear(rtree *t) {
    uint64_t sv_root = t->root, next_id = t->next_id;
    int64_t size = t->size;
    rnode root;
    int e = make_leaf(&root, t->next_id++, NULL, 0);
    if (!e) {
        root.bbox.valid = 0;
        e = save_node(t, &root, &t->root);
        node_free(&root);
    }
    if (!e) {
        t->size = 0;
        e = save_metadata(t);
    }
    if (!e) e = bjfile_commit(&t->f);
    if (e) {
        bjfile_discard(&t->f);
        t->root = sv_root; t->next_id = next_id; t->size = size;
    }
    return e;
}

/* ---- Search --------------------------------------------------------- */

static int search_rec(rtree *t, uint64_t ptr, const rbbox *q, bj_builder *b,
                      int depth) {
    if (depth > RT_MAX_DEPTH) return BJ_ERR_DEPTH;
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
            /* childBBoxes lets non-overlapping subtrees be skipped without
             * even parsing their root node. */
            if (nd.has_cb &&
                (!nd.child_bboxes[i].valid || !bbox_intersects(q, &nd.child_bboxes[i])))
                continue;
            e = search_rec(t, nd.children[i], q, b, depth + 1);
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
    int e = search_rec(t, t->root, &q, b, 0);
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

/* Like search_rec, but filters leaf entries by haversine distance and emits a
 * `distance` field (mirrors src/rtree.js searchRadius / _searchBBoxEntries). */
static int search_radius_rec(rtree *t, uint64_t ptr, const rbbox *q,
                             double lat, double lng, double radius_km,
                             bj_builder *b, int depth) {
    if (depth > RT_MAX_DEPTH) return BJ_ERR_DEPTH;
    rnode nd;
    int e = parse_node(t, ptr, &nd);
    if (e) return e;
    if (!nd.bbox.valid || !bbox_intersects(q, &nd.bbox)) { node_free(&nd); return BJ_OK; }

    if (nd.is_leaf) {
        for (int i = 0; i < nd.n; i++) {
            if (!bbox_intersects(q, &nd.entries[i].bbox)) continue;
            double dist = geo_haversine_distance(lat, lng, nd.entries[i].lat, nd.entries[i].lng);
            if (dist > radius_km) continue;
            bj_begin_object(b);
            bj_put_key(b, (const uint8_t *)"objectId", 8); bj_put_oid(b, nd.entries[i].oid);
            bj_put_key(b, (const uint8_t *)"lat", 3);       emit_number(b, nd.entries[i].lat);
            bj_put_key(b, (const uint8_t *)"lng", 3);       emit_number(b, nd.entries[i].lng);
            bj_put_key(b, (const uint8_t *)"distance", 8);  emit_number(b, dist);
            bj_end_object(b);
        }
    } else {
        for (int i = 0; i < nd.n; i++) {
            if (nd.has_cb &&
                (!nd.child_bboxes[i].valid || !bbox_intersects(q, &nd.child_bboxes[i])))
                continue;
            e = search_radius_rec(t, nd.children[i], q, lat, lng, radius_km,
                                  b, depth + 1);
            if (e) { node_free(&nd); return e; }
        }
    }
    node_free(&nd);
    return bj_builder_error(b);
}

int rtree_search_radius(rtree *t, double lat, double lng, double radius_km,
                        const uint8_t **out_ptr, size_t *out_len) {
    double min_lat, max_lat, min_lng, max_lng;
    geo_radius_to_bbox(lat, lng, radius_km, &min_lat, &max_lat, &min_lng, &max_lng);

    /*
     * The raw box is naive about the sphere; stored points live in
     * [-90, 90] x [-180, 180], so make the query cover the right region:
     * clamp latitude, and when the circle encloses a pole (lat range
     * touches ±90) or the cos-scaled longitude delta spans the globe,
     * every longitude qualifies. A box that merely crosses the
     * antimeridian splits into two — [min_lng, 180] and [-180, max_lng] —
     * so a search near ±180° no longer silently misses points on the
     * other side. The haversine filter stays the source of truth; the
     * boxes only prune. Both boxes never contain the same stored point.
     */
    if (min_lat < -90.0) min_lat = -90.0;
    if (max_lat > 90.0) max_lat = 90.0;
    rbbox q[2];
    int nq = 1;
    if (min_lat <= -90.0 || max_lat >= 90.0 ||
        !(max_lng - min_lng < 360.0) /* also catches inf/NaN deltas */) {
        rbbox all = { 1, min_lat, max_lat, -180.0, 180.0 };
        q[0] = all;
    } else if (min_lng < -180.0) {
        rbbox east = { 1, min_lat, max_lat, min_lng + 360.0, 180.0 };
        rbbox west = { 1, min_lat, max_lat, -180.0, max_lng };
        q[0] = east; q[1] = west; nq = 2;
    } else if (max_lng > 180.0) {
        rbbox east = { 1, min_lat, max_lat, min_lng, 180.0 };
        rbbox west = { 1, min_lat, max_lat, -180.0, max_lng - 360.0 };
        q[0] = east; q[1] = west; nq = 2;
    } else {
        rbbox one = { 1, min_lat, max_lat, min_lng, max_lng };
        q[0] = one;
    }

    bj_builder *b = bj_builder_new();
    if (!b) return BJ_ERR_OOM;
    bj_begin_array(b);
    int e = BJ_OK;
    for (int i = 0; i < nq && !e; i++)
        e = search_radius_rec(t, t->root, &q[i], lat, lng, radius_km, b, 0);
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

/* ---- Spatial cursor --------------------------------------------------- */

/*
 * Streams bbox matches with bounded memory: a descent stack plus one leaf
 * (O(height) state), reading one node at a time instead of materializing
 * the result set. The root is pinned at open, so — the tree being
 * append-only — a cursor iterates a consistent snapshot across concurrent
 * inserts/removes. childBBoxes prune subtrees without parsing them.
 */
typedef struct { rnode node; int idx; } rt_frame;

struct rtree_cursor {
    rtree   *t;
    rbbox    q;
    rt_frame stack[RT_MAX_DEPTH + 1];
    int      depth;
    rnode    leaf;
    int      has_leaf, leaf_pos;
    int      done;
};

/* Parse `ptr` onto the cursor: leaves become the active leaf, internal
 * nodes a stack frame; nodes outside the query are dropped silently. */
static int rtc_visit(rtree_cursor *c, uint64_t ptr) {
    if (c->depth > RT_MAX_DEPTH) return BJ_ERR_DEPTH;
    rnode nd;
    int e = parse_node(c->t, ptr, &nd);
    if (e) return e;
    if (!nd.bbox.valid || !bbox_intersects(&c->q, &nd.bbox)) {
        node_free(&nd);
        return BJ_OK;
    }
    if (nd.is_leaf) {
        c->leaf = nd;   /* take ownership */
        c->has_leaf = 1;
        c->leaf_pos = 0;
        return BJ_OK;
    }
    c->stack[c->depth].node = nd;   /* take ownership */
    c->stack[c->depth].idx = 0;
    c->depth++;
    return BJ_OK;
}

static void rtc_release(rtree_cursor *c) {
    if (c->has_leaf) { node_free(&c->leaf); c->has_leaf = 0; }
    while (c->depth > 0) node_free(&c->stack[--c->depth].node);
}

rtree_cursor *rtree_cursor_open(rtree *t, double min_lat, double max_lat,
                                double min_lng, double max_lng) {
    if (isnan(min_lat) || isnan(max_lat) || isnan(min_lng) || isnan(max_lng))
        return NULL;
    rtree_cursor *c = (rtree_cursor *)calloc(1, sizeof(rtree_cursor));
    if (!c) return NULL;
    c->t = t;
    c->q.valid = 1;
    c->q.min_lat = min_lat; c->q.max_lat = max_lat;
    c->q.min_lng = min_lng; c->q.max_lng = max_lng;
    if (rtc_visit(c, t->root)) { rtree_cursor_close(c); return NULL; }
    if (!c->has_leaf && c->depth == 0) c->done = 1;   /* root outside query */
    return c;
}

int rtree_cursor_next(rtree_cursor *c, double *lat, double *lng, uint8_t oid12[12]) {
    if (c->done) return 0;
    for (;;) {
        if (c->has_leaf) {
            while (c->leaf_pos < c->leaf.n) {
                rentry *en = &c->leaf.entries[c->leaf_pos++];
                if (!bbox_intersects(&c->q, &en->bbox)) continue;
                *lat = en->lat;
                *lng = en->lng;
                memcpy(oid12, en->oid, 12);
                return 1;
            }
            node_free(&c->leaf);
            c->has_leaf = 0;
        }
        int advanced = 0;
        while (c->depth > 0 && !advanced) {
            rt_frame *f = &c->stack[c->depth - 1];
            if (f->idx < f->node.n) {
                int i = f->idx++;
                if (f->node.has_cb &&
                    (!f->node.child_bboxes[i].valid ||
                     !bbox_intersects(&c->q, &f->node.child_bboxes[i])))
                    continue;
                int before = c->depth;
                int e = rtc_visit(c, f->node.children[i]);
                if (e) { rtc_release(c); c->done = 1; return e; }
                if (c->has_leaf || c->depth > before) advanced = 1;
            } else {
                node_free(&f->node);
                c->depth--;
            }
        }
        if (!advanced) { c->done = 1; return 0; }
    }
}

int rtree_cursor_next_batch(rtree_cursor *c, size_t max_bytes, int *count,
                            const uint8_t **out_ptr, size_t *out_len) {
    rtree *t = c->t;
    bj_builder *b = t->bld;
    bj_builder_reset(b);
    bj_begin_array(b);
    int n = 0;
    size_t approx = 0;
    while (approx < max_bytes) {
        double la, ln;
        uint8_t oid[12];
        int r = rtree_cursor_next(c, &la, &ln, oid);
        if (r < 0) return r;
        if (r == 0) break;
        bj_begin_object(b);
        bj_put_key(b, (const uint8_t *)"objectId", 8); bj_put_oid(b, oid);
        bj_put_key(b, (const uint8_t *)"lat", 3);      emit_number(b, la);
        bj_put_key(b, (const uint8_t *)"lng", 3);      emit_number(b, ln);
        bj_end_object(b);
        n++;
        approx += 60;
    }
    bj_end_array(b);
    int e = bj_builder_error(b);
    if (e) return e;
    size_t len;
    const uint8_t *d = bj_builder_data(b, &len);
    if (!d) return BJ_ERR_STATE;
    if ((e = set_out(t, d, len))) return e;
    *count = n;
    *out_ptr = t->out.data;
    *out_len = t->out.len;
    return BJ_OK;
}

void rtree_cursor_close(rtree_cursor *c) {
    if (!c) return;
    rtc_release(c);
    free(c);
}

/* ---- k-nearest neighbors (best-first) --------------------------------- */

typedef struct {
    double   dist;
    int      is_entry;
    uint64_t ptr;      /* node offset when !is_entry */
    rentry   ent;      /* when is_entry              */
} knn_item;

typedef struct { knn_item *a; int n, cap; } knn_heap;

static int heap_push(knn_heap *h, knn_item it) {
    if (h->n == h->cap) {
        int nc = h->cap ? h->cap * 2 : 64;
        knn_item *na = (knn_item *)realloc(h->a, (size_t)nc * sizeof(knn_item));
        if (!na) return BJ_ERR_OOM;
        h->a = na;
        h->cap = nc;
    }
    int i = h->n++;
    while (i > 0) {
        int p = (i - 1) / 2;
        if (h->a[p].dist <= it.dist) break;
        h->a[i] = h->a[p];
        i = p;
    }
    h->a[i] = it;
    return BJ_OK;
}

static knn_item heap_pop(knn_heap *h) {
    knn_item top = h->a[0];
    knn_item last = h->a[--h->n];
    int i = 0;
    for (;;) {
        int l = 2 * i + 1, r = l + 1, m = i;
        if (l < h->n && h->a[l].dist < last.dist) m = l;
        if (r < h->n && h->a[r].dist < (m == i ? last.dist : h->a[l].dist)) m = r;
        if (m == i) break;
        h->a[i] = h->a[m];
        i = m;
    }
    h->a[i] = last;
    return top;
}

/* Minimum haversine distance from a point to a lat/lng box: clamp the point
 * into the box, then measure. Longitude is periodic, so the numerically
 * nearest edge can be the wrong way around the antimeridian (query at 179°
 * vs a box ending at -180°: the naive clamp measures ~359° instead of ~1°) —
 * clamping the query at ±360° as well and keeping the best distance makes
 * the bound tight in every direction, so kNN never mis-prunes near ±180°. */
static double mindist_to_box(double lat, double lng, const rbbox *b) {
    double clat = lat < b->min_lat ? b->min_lat : (lat > b->max_lat ? b->max_lat : lat);
    double best = INFINITY;
    for (int s = -1; s <= 1; s++) {
        double l = lng + 360.0 * s;
        double clng = l < b->min_lng ? b->min_lng : (l > b->max_lng ? b->max_lng : l);
        double d = geo_haversine_distance(lat, lng, clat, clng);
        if (d < best) best = d;
    }
    return best;
}

int rtree_nearest(rtree *t, double lat, double lng, int k,
                  const uint8_t **out_ptr, size_t *out_len) {
    if (isnan(lat) || isnan(lng) || k < 0) return BJ_ERR_STATE;
    knn_heap h;
    memset(&h, 0, sizeof(h));
    bj_builder *b = bj_builder_new();
    if (!b) return BJ_ERR_OOM;
    bj_begin_array(b);

    /* Nodes are >= a few dozen bytes; a hostile pointer cycle would revisit
     * offsets forever, so cap node visits by what the file could hold. */
    uint64_t budget = bjfile_len(&t->f) / 8 + 64;

    int e = BJ_OK, emitted = 0;
    knn_item root_it;
    memset(&root_it, 0, sizeof(root_it));
    root_it.ptr = t->root;
    if (k > 0) e = heap_push(&h, root_it);

    while (!e && h.n > 0 && emitted < k) {
        knn_item it = heap_pop(&h);
        if (it.is_entry) {
            bj_begin_object(b);
            bj_put_key(b, (const uint8_t *)"objectId", 8); bj_put_oid(b, it.ent.oid);
            bj_put_key(b, (const uint8_t *)"lat", 3);      emit_number(b, it.ent.lat);
            bj_put_key(b, (const uint8_t *)"lng", 3);      emit_number(b, it.ent.lng);
            bj_put_key(b, (const uint8_t *)"distance", 8); emit_number(b, it.dist);
            bj_end_object(b);
            if ((e = bj_builder_error(b))) break;
            emitted++;
            continue;
        }
        if (budget-- == 0) { e = BJ_ERR_STATE; break; }
        rnode nd;
        if ((e = parse_node(t, it.ptr, &nd))) break;
        if (nd.is_leaf) {
            for (int i = 0; i < nd.n && !e; i++) {
                knn_item en;
                memset(&en, 0, sizeof(en));
                en.is_entry = 1;
                en.ent = nd.entries[i];
                en.dist = geo_haversine_distance(lat, lng, nd.entries[i].lat, nd.entries[i].lng);
                e = heap_push(&h, en);
            }
        } else {
            for (int i = 0; i < nd.n && !e; i++) {
                rbbox cb;
                if ((e = child_bbox(t, &nd, i, &cb))) break;
                if (!cb.valid) continue;
                knn_item ch;
                memset(&ch, 0, sizeof(ch));
                ch.ptr = nd.children[i];
                ch.dist = mindist_to_box(lat, lng, &cb);
                e = heap_push(&h, ch);
            }
        }
        node_free(&nd);
    }

    bj_end_array(b);
    if (!e) e = bj_builder_error(b);
    if (!e) {
        size_t len;
        const uint8_t *d = bj_builder_data(b, &len);
        if (!d) e = BJ_ERR_STATE;
        else if (!(e = set_out(t, d, len))) {
            *out_ptr = t->out.data;
            *out_len = t->out.len;
        }
    }
    free(h.a);
    bj_builder_free(b);
    return e;
}

/* ---- Compaction ----------------------------------------------------- */

/* Open-addressing old-offset -> new-offset map (offsets are exact u64s). */
typedef struct { uint64_t *olds, *news; uint8_t *used; size_t n, cap; } ptrmap;

static size_t ptrmap_hash(uint64_t k, size_t cap) {
    k ^= k >> 33; k *= 0xff51afd7ed558ccdULL;
    k ^= k >> 33; k *= 0xc4ceb9fe1a85ec53ULL;
    k ^= k >> 33;
    return (size_t)k & (cap - 1);
}
static int ptrmap_get(const ptrmap *m, uint64_t old, uint64_t *found) {
    if (!m->cap) return 0;
    for (size_t i = ptrmap_hash(old, m->cap); m->used[i]; i = (i + 1) & (m->cap - 1)) {
        if (m->olds[i] == old) { *found = m->news[i]; return 1; }
    }
    return 0;
}
static int ptrmap_put(ptrmap *m, uint64_t old, uint64_t neu) {
    if (m->n * 2 >= m->cap) {
        size_t nc = m->cap ? m->cap * 2 : 64;
        uint64_t *no = (uint64_t *)malloc(nc * sizeof(uint64_t));
        uint64_t *nn = (uint64_t *)malloc(nc * sizeof(uint64_t));
        uint8_t  *nu = (uint8_t *)calloc(nc, 1);
        if (!no || !nn || !nu) { free(no); free(nn); free(nu); return BJ_ERR_OOM; }
        for (size_t i = 0; i < m->cap; i++) {
            if (!m->used[i]) continue;
            size_t j = ptrmap_hash(m->olds[i], nc);
            while (nu[j]) j = (j + 1) & (nc - 1);
            no[j] = m->olds[i]; nn[j] = m->news[i]; nu[j] = 1;
        }
        free(m->olds); free(m->news); free(m->used);
        m->olds = no; m->news = nn; m->used = nu; m->cap = nc;
    }
    size_t i = ptrmap_hash(old, m->cap);
    while (m->used[i]) {
        if (m->olds[i] == old) { m->news[i] = neu; return BJ_OK; }
        i = (i + 1) & (m->cap - 1);
    }
    m->olds[i] = old; m->news[i] = neu; m->used[i] = 1; m->n++;
    return BJ_OK;
}
static void ptrmap_free(ptrmap *m) {
    free(m->olds); free(m->news); free(m->used);
    memset(m, 0, sizeof(*m));
}

static int clone_node(rtree *t, bjfile *dst, ptrmap *m, uint64_t old_off,
                      uint64_t *new_off, rbbox *out_bbox, int depth) {
    if (depth > RT_MAX_DEPTH) return BJ_ERR_DEPTH;
    if (ptrmap_get(m, old_off, new_off)) {
        if (out_bbox) {   /* dedup never hits on a well-formed tree */
            rnode nd;
            int e = parse_node(t, old_off, &nd);
            if (e) return e;
            *out_bbox = nd.bbox;
            node_free(&nd);
        }
        return BJ_OK;
    }
    rnode nd;
    int e = parse_node(t, old_off, &nd);
    if (e) return e;
    if (!nd.is_leaf) {
        /* Rebuild childBBoxes from the cloned children — this refreshes them
         * and upgrades legacy nodes as a side effect of compaction. */
        if (!nd.child_bboxes && nd.n) {
            nd.child_bboxes = (rbbox *)calloc((size_t)nd.n, sizeof(rbbox));
            if (!nd.child_bboxes) { node_free(&nd); return BJ_ERR_OOM; }
        }
        for (int i = 0; i < nd.n; i++) {
            uint64_t nc; rbbox cbb;
            e = clone_node(t, dst, m, nd.children[i], &nc, &cbb, depth + 1);
            if (e) { node_free(&nd); return e; }
            nd.children[i] = nc;
            nd.child_bboxes[i] = cbb;
        }
        nd.has_cb = 1;
    }
    if (out_bbox) *out_bbox = nd.bbox;
    e = encode_node(t, &nd, dst, new_off);
    node_free(&nd);
    if (e) return e;
    return ptrmap_put(m, old_off, *new_off);
}

int rtree_compact(rtree *t, const bj_io *dst_io) {
    bjfile dst;
    bjfile_init(&dst, dst_io);
    dst.autoflush = 1u << 18;   /* stream to the host in ~256 KB chunks */
    ptrmap m; memset(&m, 0, sizeof(m));
    uint64_t new_root = 0;
    int e = bjfile_append_header(&dst, t->bld, "rtree");
    if (!e) e = clone_node(t, &dst, &m, t->root, &new_root, NULL, 0);
    if (!e) e = encode_metadata(t, &dst, new_root);
    if (!e) e = bjfile_commit(&dst);
    ptrmap_free(&m);
    bjfile_dispose(&dst);
    return e;
}

/* ---- Lifecycle & accessors ------------------------------------------ */

static int min_entries_for(int max_entries) {
    int half = (max_entries + 1) / 2;   /* ceil(max/2) */
    return half < 2 ? 2 : half;
}

rtree *rtree_create(const bj_io *io, int max_entries) {
    if (max_entries < 2) return NULL;
    rtree *t = (rtree *)calloc(1, sizeof(rtree));
    if (!t) return NULL;
    t->bld = bj_builder_new();
    if (!t->bld) { free(t); return NULL; }
    bjfile_init(&t->f, io);
    t->max_entries = max_entries;
    t->min_entries = min_entries_for(max_entries);
    t->next_id = 1;
    t->size = 0;

    if (bjfile_append_header(&t->f, t->bld, "rtree")) { rtree_free(t); return NULL; }
    rnode root;
    if (make_leaf(&root, 0, NULL, 0)) { rtree_free(t); return NULL; }
    root.bbox.valid = 0;
    if (save_node(t, &root, &t->root)) { node_free(&root); rtree_free(t); return NULL; }
    node_free(&root);
    if (save_metadata(t) || bjfile_commit(&t->f)) { rtree_free(t); return NULL; }
    return t;
}

/* Commit-scan callback: a commit ends at each record that parses and
 * validates as a metadata record of the fixed on-wire size. */
static int scan_cb(void *ctx, uint64_t off, const uint8_t *rec,
                   size_t rec_len, int *is_commit_end) {
    (void)ctx;
    if (rec_len == RT_METADATA_SIZE) {
        rt_meta m;
        if (parse_meta_rec(rec, rec_len, &m) == BJ_OK && meta_valid(&m, off))
            *is_commit_end = 1;
    }
    return BJ_OK;
}

/*
 * Open: verify the file identifies as an R-tree (when it carries a header;
 * files from the JS reference have none and are accepted), then take the fast
 * path — parse + validate the metadata at the fixed tail offset and verify
 * the last commit's CRC. Any failure falls back to a full recovery scan
 * (torn tails are truncated to the last good commit; verifiable data beyond
 * a damaged region refuses to open). Mirrors bpt_open.
 */
rtree *rtree_open(const bj_io *io) {
    rtree *t = (rtree *)calloc(1, sizeof(rtree));
    if (!t) return NULL;
    t->bld = bj_builder_new();
    if (!t->bld) { free(t); return NULL; }
    bjfile_init(&t->f, io);

    if (bjfile_check_header(&t->f, "rtree") < 0) { rtree_free(t); return NULL; }

    const uint8_t *md; size_t md_len;
    rt_meta m;
    uint64_t flen = bjfile_len(&t->f);
    if (bjfile_check_tail(&t->f, RT_METADATA_SIZE, &md, &md_len) == BJ_OK &&
        parse_meta_rec(md, md_len, &m) == BJ_OK &&
        meta_valid(&m, flen - RT_METADATA_SIZE)) {
        meta_apply(t, &m);
        return t;
    }

    /* Recovery. */
    uint64_t good = 0;
    if (bjfile_scan_commits(&t->f, scan_cb, NULL, &good)) { rtree_free(t); return NULL; }
    if (good < RT_METADATA_SIZE) { rtree_free(t); return NULL; }
    const uint8_t *rec; size_t rec_len;
    if (bjfile_read_record(&t->f, good - RT_METADATA_SIZE, &rec, &rec_len) ||
        rec_len != RT_METADATA_SIZE ||
        parse_meta_rec(rec, rec_len, &m) != BJ_OK ||
        !meta_valid(&m, good - RT_METADATA_SIZE)) {
        rtree_free(t);
        return NULL;
    }
    if (good < flen && bjfile_set_len(&t->f, good)) { rtree_free(t); return NULL; }
    meta_apply(t, &m);
    return t;
}

void rtree_free(rtree *t) {
    if (!t) return;
    bj_builder_free(t->bld);
    bjfile_dispose(&t->f);
    free(t->out.data);
    free(t);
}

int64_t        rtree_size(const rtree *t)        { return t->size; }
int            rtree_max_entries(const rtree *t) { return t->max_entries; }
const uint8_t *rtree_out(const rtree *t, size_t *len)   { if (len) *len = t->out.len; return t->out.data; }

uint64_t rtree_file_len(const rtree *t) { return bjfile_len(&t->f); }

int rtree_rewind(rtree *t, uint64_t len) {
    uint64_t cur = bjfile_len(&t->f);
    if (len == cur) return BJ_OK;
    if (len > cur || len < RT_METADATA_SIZE) return BJ_ERR_STATE;
    const uint8_t *rec; size_t rec_len;
    int e = bjfile_read_record(&t->f, len - RT_METADATA_SIZE, &rec, &rec_len);
    if (e) return e;
    rt_meta m;
    if (rec_len != RT_METADATA_SIZE ||
        parse_meta_rec(rec, rec_len, &m) != BJ_OK ||
        !meta_valid(&m, len - RT_METADATA_SIZE)) return BJ_ERR_STATE;
    if ((e = bjfile_set_len(&t->f, len))) return e;
    meta_apply(t, &m);
    return BJ_OK;
}
