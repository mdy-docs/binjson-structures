/*
 * dbuf.h — shared growable byte buffer.
 *
 * Internal header for the C data structures, which previously each carried
 * their own copy. Zero-initialize (memset or `dbuf b = {0}`) before use;
 * dbuf_free releases the storage and resets to the empty state.
 */
#ifndef DBUF_H
#define DBUF_H

#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#include "binjson.h"

typedef struct { uint8_t *data; size_t len, cap; } dbuf;

static inline int dbuf_ensure(dbuf *b, size_t extra) {
    if (extra <= b->cap - b->len) return BJ_OK;   /* len <= cap invariant */
    if (extra > ((size_t)-1) - b->len) return BJ_ERR_OOM;
    size_t need = b->len + extra;
    size_t nc = b->cap ? b->cap : 256;
    while (nc < need) {
        if (nc > ((size_t)-1) / 2) return BJ_ERR_OOM;   /* overflow guard */
        nc *= 2;
    }
    uint8_t *nb = (uint8_t *)realloc(b->data, nc);
    if (!nb) return BJ_ERR_OOM;
    b->data = nb; b->cap = nc;
    return BJ_OK;
}
static inline int dbuf_put(dbuf *b, const uint8_t *p, size_t n) {
    int e = dbuf_ensure(b, n);
    if (e) return e;
    if (n) memcpy(b->data + b->len, p, n);
    b->len += n;
    return BJ_OK;
}
static inline void dbuf_free(dbuf *b) {
    free(b->data);
    b->data = NULL;
    b->len = b->cap = 0;
}

/* A freshly malloc'd, independently-owned copy of p[0..n) -- for handing a
 * result out of a function whose input buffers (a tree's transient output,
 * a cursor batch, ...) won't outlive the call. */
static inline int dbuf_dup(const uint8_t *p, size_t n, uint8_t **out, size_t *out_len) {
    uint8_t *buf = (uint8_t *)malloc(n ? n : 1);
    if (!buf) return BJ_ERR_OOM;
    if (n) memcpy(buf, p, n);
    *out = buf;
    *out_len = n;
    return BJ_OK;
}

#endif /* DBUF_H */
