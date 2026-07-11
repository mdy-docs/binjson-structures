/*
 * bjcursor.h — shared read-side cursor over binjson wire-format bytes.
 *
 * Internal header for the C data structures (bplustree.c, rtree.c,
 * textlog.c, textindex.c), which previously each carried a near-verbatim
 * copy of these helpers. Everything is static inline and deliberately keeps
 * the original unprefixed names so the call sites read unchanged; structure-
 * specific readers (bpt_key, rbbox, textlog entries, ...) stay in their own
 * files on top of these primitives.
 *
 * A `cur` never owns its bytes: it walks a record held elsewhere (bjfile
 * read buffer, builder output), and readers that expose spans (take_string,
 * take_key) point into those bytes — valid only while they are.
 *
 * All readers return BJ_OK (0) or a negative BJ_ERR_* code and only advance
 * the cursor on success.
 */
#ifndef BJCURSOR_H
#define BJCURSOR_H

#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <math.h>

#include "binjson.h"

/* ---- Little-endian scalar readers ------------------------------------ */

static inline uint32_t rdu32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static inline uint64_t rdu64(const uint8_t *p) {
    uint64_t v = 0;
    for (int i = 7; i >= 0; i--) v = (v << 8) | p[i];
    return v;
}

/* A double that is exactly a JS-safe integer (|d| <= 2^53 - 1, integral). */
static inline int is_safe_int(double d) {
    if (!isfinite(d)) return 0;
    if (d < BJ_MIN_SAFE_INT || d > BJ_MAX_SAFE_INT) return 0;
    return d == floor(d);
}

/* ---- Cursor ----------------------------------------------------------- */

typedef struct { const uint8_t *d; size_t len; size_t pos; } cur;

static inline int cur_need(const cur *c, size_t n) {
    return n <= c->len - c->pos ? BJ_OK : BJ_ERR_EOF;
}
static inline int take_type(cur *c, uint8_t *t) {
    if (cur_need(c, 1)) return BJ_ERR_EOF;
    *t = c->d[c->pos++];
    return BJ_OK;
}
static inline int take_u32(cur *c, uint32_t *v) {
    if (cur_need(c, 4)) return BJ_ERR_EOF;
    *v = rdu32(c->d + c->pos);
    c->pos += 4;
    return BJ_OK;
}

static inline int name_eq(const uint8_t *p, uint32_t len, const char *s) {
    size_t sl = strlen(s);
    return len == sl && memcmp(p, s, sl) == 0;
}

/* ---- Typed value readers ---------------------------------------------- */

static inline int read_number(cur *c, double *out) {
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
static inline int read_bool(cur *c, int *out) {
    uint8_t t;
    if (take_type(c, &t)) return BJ_ERR_EOF;
    if (t == BJ_TYPE_TRUE)  { *out = 1; return BJ_OK; }
    if (t == BJ_TYPE_FALSE) { *out = 0; return BJ_OK; }
    return BJ_ERR_UNKNOWN_TYPE;
}
static inline int read_pointer(cur *c, uint64_t *out) {
    uint8_t t;
    if (take_type(c, &t)) return BJ_ERR_EOF;
    if (t != BJ_TYPE_POINTER) return BJ_ERR_UNKNOWN_TYPE;
    if (cur_need(c, 8)) return BJ_ERR_EOF;
    *out = rdu64(c->d + c->pos);
    c->pos += 8; return BJ_OK;
}
/* POINTER or NULL. Sets *has and, when present, *out. */
static inline int read_ptr_or_null(cur *c, int *has, uint64_t *out) {
    if (cur_need(c, 1)) return BJ_ERR_EOF;
    if (c->d[c->pos] == BJ_TYPE_NULL) { c->pos++; *has = 0; return BJ_OK; }
    *has = 1;
    return read_pointer(c, out);
}
static inline int read_date(cur *c, int64_t *out) {
    uint8_t t;
    if (take_type(c, &t)) return BJ_ERR_EOF;
    if (t != BJ_TYPE_DATE) return BJ_ERR_UNKNOWN_TYPE;
    if (cur_need(c, 8)) return BJ_ERR_EOF;
    *out = (int64_t)rdu64(c->d + c->pos);
    c->pos += 8; return BJ_OK;
}
/* Read a number that must be a non-negative integer (ids, counts and
 * versions travel as JS numbers on the wire but are integers by
 * construction; anything else is a malformed file). */
static inline int read_u64(cur *c, uint64_t *out) {
    double d;
    int e = read_number(c, &d);
    if (e) return e;
    if (!is_safe_int(d) || d < 0) return BJ_ERR_STATE;
    *out = (uint64_t)d;
    return BJ_OK;
}
/* Like read_u64, but the value must also fit in an int (small counts and
 * enums). Casting an unchecked double to int is undefined behavior for
 * out-of-range values, so hostile files must be rejected before the cast. */
static inline int read_int31(cur *c, int *out) {
    uint64_t u;
    int e = read_u64(c, &u);
    if (e) return e;
    if (u > 0x7fffffff) return BJ_ERR_STATE;
    *out = (int)u;
    return BJ_OK;
}
static inline int take_string(cur *c, const uint8_t **p, uint32_t *len) {
    uint8_t t;
    if (take_type(c, &t)) return BJ_ERR_EOF;
    if (t != BJ_TYPE_STRING) return BJ_ERR_UNKNOWN_TYPE;
    if (take_u32(c, len)) return BJ_ERR_EOF;
    if (cur_need(c, *len)) return BJ_ERR_EOF;
    *p = c->d + c->pos; c->pos += *len;
    return BJ_OK;
}

/* ---- Composite readers ------------------------------------------------ */

static inline int object_begin(cur *c, uint32_t *count) {
    uint8_t t;
    if (take_type(c, &t)) return BJ_ERR_EOF;
    if (t != BJ_TYPE_OBJECT) return BJ_ERR_UNKNOWN_TYPE;
    uint32_t size;
    if (take_u32(c, &size)) return BJ_ERR_EOF;
    return take_u32(c, count);
}
static inline int array_begin(cur *c, uint32_t *count) {
    uint8_t t;
    if (take_type(c, &t)) return BJ_ERR_EOF;
    if (t != BJ_TYPE_ARRAY) return BJ_ERR_UNKNOWN_TYPE;
    uint32_t size;
    if (take_u32(c, &size)) return BJ_ERR_EOF;
    return take_u32(c, count);
}
/* An object field name (u32 length + bytes; points into the record). */
static inline int take_key(cur *c, const uint8_t **kn, uint32_t *klen) {
    if (take_u32(c, klen)) return BJ_ERR_EOF;
    if (cur_need(c, *klen)) return BJ_ERR_EOF;
    *kn = c->d + c->pos;
    c->pos += *klen;
    return BJ_OK;
}
/* Skip one whole value of any type (sized via bj_value_size). */
static inline int skip_value(cur *c) {
    size_t sz;
    int e = bj_value_size(c->d, c->len, c->pos, &sz);
    if (e) return e;
    if (cur_need(c, sz)) return BJ_ERR_EOF;
    c->pos += sz; return BJ_OK;
}

#endif /* BJCURSOR_H */
