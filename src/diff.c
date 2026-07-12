/*
 * diff.c — C port of jsdiff 8.0.3 (createPatch / applyPatch / structuredPatch).
 * See diff.h. This mirrors the reference implementation closely, function by
 * function, so the output matches byte-for-byte.
 */
#include "diff.h"

#include <stdlib.h>
#include <string.h>

/* ---- Growable byte buffer ------------------------------------------- */

typedef struct { uint8_t *p; size_t len, cap; } sb;

static int sb_reserve(sb *b, size_t extra) {
    if (b->len + extra <= b->cap) return DIFF_OK;
    size_t nc = b->cap ? b->cap : 64;
    while (nc < b->len + extra) nc *= 2;
    uint8_t *np = (uint8_t *)realloc(b->p, nc);
    if (!np) return DIFF_ERR_OOM;
    b->p = np; b->cap = nc;
    return DIFF_OK;
}
static int sb_putn(sb *b, const uint8_t *s, size_t n) {
    int e = sb_reserve(b, n);
    if (e) return e;
    if (n) memcpy(b->p + b->len, s, n);
    b->len += n;
    return DIFF_OK;
}
static int sb_putc(sb *b, uint8_t c) { return sb_putn(b, &c, 1); }
static int sb_puts(sb *b, const char *s) { return sb_putn(b, (const uint8_t *)s, strlen(s)); }
static int sb_putlong(sb *b, long v) {
    char tmp[24]; int i = 0; unsigned long u;
    int neg = v < 0;
    u = neg ? (unsigned long)(-(v + 1)) + 1UL : (unsigned long)v;
    if (u == 0) tmp[i++] = '0';
    while (u) { tmp[i++] = (char)('0' + (u % 10)); u /= 10; }
    if (neg) tmp[i++] = '-';
    char rev[24];
    for (int k = 0; k < i; k++) rev[k] = tmp[i - 1 - k];
    return sb_putn(b, (const uint8_t *)rev, (size_t)i);
}

/* ---- Line tokenization (diff/line.js tokenize, default options) ----- */

typedef struct { size_t off, len; } rng;

/*
 * Split (s,n) into line tokens exactly like jsdiff's line tokenizer with
 * default options: value.split(/(\n|\r\n)/), drop a trailing empty piece, merge
 * each separator into the preceding content, then removeEmpty. Tokens are
 * contiguous ranges into s, each covering a line plus its trailing newline.
 */
static int tokenize_lines(const uint8_t *s, size_t n, rng **out, size_t *count) {
    /* Build parts (alternating content / separator) as ranges. */
    rng *parts = NULL; size_t np = 0, cap = 0;
    size_t i = 0;
    for (;;) {
        size_t j = i;
        while (j < n && !(s[j] == '\n' || (s[j] == '\r' && j + 1 < n && s[j + 1] == '\n'))) j++;
        if (np == cap) { cap = cap ? cap * 2 : 8; rng *t = realloc(parts, cap * sizeof(rng)); if (!t) { free(parts); return DIFF_ERR_OOM; } parts = t; }
        parts[np].off = i; parts[np].len = j - i; np++; /* content */
        if (j >= n) break;
        size_t seplen = (s[j] == '\n') ? 1 : 2;
        if (np == cap) { cap = cap ? cap * 2 : 8; rng *t = realloc(parts, cap * sizeof(rng)); if (!t) { free(parts); return DIFF_ERR_OOM; } parts = t; }
        parts[np].off = j; parts[np].len = seplen; np++; /* separator */
        i = j + seplen;
    }
    /* Ignore the final empty content token if the string ended with a newline. */
    if (np > 0 && parts[np - 1].len == 0) np--;
    /* Merge separators into the preceding content token, then removeEmpty. */
    rng *toks = NULL; size_t nt = 0, tcap = 0;
    for (size_t k = 0; k < np; k++) {
        if (k % 2 == 1) {
            /* separator: extend previous token */
            if (nt > 0) toks[nt - 1].len += parts[k].len;
        } else {
            if (nt == tcap) { tcap = tcap ? tcap * 2 : 8; rng *t = realloc(toks, tcap * sizeof(rng)); if (!t) { free(parts); free(toks); return DIFF_ERR_OOM; } toks = t; }
            toks[nt++] = parts[k];
        }
    }
    free(parts);
    /* removeEmpty */
    size_t w = 0;
    for (size_t k = 0; k < nt; k++) if (toks[k].len > 0) toks[w++] = toks[k];
    nt = w;
    *out = toks; *count = nt;
    return DIFF_OK;
}

/* ---- Myers diff (diff/base.js) -------------------------------------- */

typedef struct comp {
    long count;
    int  added, removed;
    struct comp *prev;
    const uint8_t *val; size_t vlen; /* filled by build_values */
} comp;

/* Arena for comp nodes: pointers must stay stable across the algorithm. */
typedef struct arena_block { struct arena_block *next; size_t used; comp items[256]; } arena_block;
typedef struct { arena_block *head; } arena;

static comp *arena_new(arena *a, long count, int added, int removed, comp *prev) {
    if (!a->head || a->head->used == 256) {
        arena_block *b = (arena_block *)malloc(sizeof(arena_block));
        if (!b) return NULL;
        b->next = a->head; b->used = 0; a->head = b;
    }
    comp *c = &a->head->items[a->head->used++];
    c->count = count; c->added = added; c->removed = removed; c->prev = prev;
    c->val = NULL; c->vlen = 0;
    return c;
}
static void arena_free(arena *a) {
    arena_block *b = a->head;
    while (b) { arena_block *n = b->next; free(b); b = n; }
    a->head = NULL;
}

typedef struct { long oldPos; comp *last; int present; } pathnode;

typedef struct {
    const uint8_t *sa; const rng *ta; size_t na; /* old tokens */
    const uint8_t *sb; const rng *tb; size_t nb; /* new tokens */
} diffctx;

static int tok_eq(const diffctx *d, size_t oi, size_t ni) {
    const rng *A = &d->ta[oi], *B = &d->tb[ni];
    return A->len == B->len && memcmp(d->sa + A->off, d->sb + B->off, A->len) == 0;
}

/* extractCommon: advance along the diagonal over equal tokens. */
static int extract_common(arena *a, const diffctx *d, pathnode *path, long diagonal, long *newPosOut) {
    long oldLen = (long)d->na, newLen = (long)d->nb;
    long oldPos = path->oldPos, newPos = oldPos - diagonal, commonCount = 0;
    while (newPos + 1 < newLen && oldPos + 1 < oldLen &&
           tok_eq(d, (size_t)(oldPos + 1), (size_t)(newPos + 1))) {
        newPos++; oldPos++; commonCount++;
    }
    if (commonCount) {
        comp *c = arena_new(a, commonCount, 0, 0, path->last);
        if (!c) return DIFF_ERR_OOM;
        path->last = c;
    }
    path->oldPos = oldPos;
    *newPosOut = newPos;
    return DIFF_OK;
}

static int add_to_path(arena *a, pathnode *base, int added, int removed, long oldPosInc, pathnode *out) {
    comp *last = base->last, *nc;
    if (last && last->added == added && last->removed == removed) {
        nc = arena_new(a, last->count + 1, added, removed, last->prev);
    } else {
        nc = arena_new(a, 1, added, removed, last);
    }
    if (!nc) return DIFF_ERR_OOM;
    out->oldPos = base->oldPos + oldPosInc;
    out->last = nc;
    out->present = 1;
    return DIFF_OK;
}

/* buildValues: linked list -> ordered flat array with value ranges. */
static int build_values(const diffctx *d, comp *lastComponent, comp **out, size_t *ncomp) {
    size_t cnt = 0;
    for (comp *c = lastComponent; c; c = c->prev) cnt++;
    comp *arr = (comp *)malloc((cnt ? cnt : 1) * sizeof(comp));
    if (!arr) return DIFF_ERR_OOM;
    /* reverse into arr */
    size_t idx = cnt;
    for (comp *c = lastComponent; c; c = c->prev) arr[--idx] = *c;
    long newPos = 0, oldPos = 0;
    for (size_t i = 0; i < cnt; i++) {
        comp *component = &arr[i];
        long count = component->count;
        if (!component->removed) {
            /* value = join(newTokens[newPos .. newPos+count]) */
            if (count > 0) {
                size_t start = d->tb[newPos].off;
                size_t end = d->tb[newPos + count - 1].off + d->tb[newPos + count - 1].len;
                component->val = d->sb + start; component->vlen = end - start;
            } else { component->val = d->sb; component->vlen = 0; }
            newPos += count;
            if (!component->added) oldPos += count;
        } else {
            if (count > 0) {
                size_t start = d->ta[oldPos].off;
                size_t end = d->ta[oldPos + count - 1].off + d->ta[oldPos + count - 1].len;
                component->val = d->sa + start; component->vlen = end - start;
            } else { component->val = d->sa; component->vlen = 0; }
            oldPos += count;
        }
        component->prev = NULL;
    }
    *out = arr; *ncomp = cnt;
    return DIFF_OK;
}

/*
 * diffLines: returns the ordered change components. *out is malloc'd (caller
 * frees); each component's val points into `a` (removed) or `b` (added/common).
 */
static int diff_lines(const uint8_t *a, size_t alen, const uint8_t *b, size_t blen,
                      comp **out, size_t *ncomp) {
    rng *ta = NULL, *tb = NULL; size_t na = 0, nb = 0;
    int e = tokenize_lines(a, alen, &ta, &na);
    if (e) return e;
    e = tokenize_lines(b, blen, &tb, &nb);
    if (e) { free(ta); return e; }

    diffctx d = { a, ta, na, b, tb, nb };
    arena ar; ar.head = NULL;

    long oldLen = (long)na, newLen = (long)nb;
    long maxEditLength = oldLen + newLen;
    size_t sz = (size_t)(2 * maxEditLength + 3);
    long off = maxEditLength + 1;
    pathnode *best = (pathnode *)calloc(sz, sizeof(pathnode));
    if (!best) { free(ta); free(tb); return DIFF_ERR_OOM; }

    comp *resultLast = NULL; int haveResult = 0;

    /* Seed edit length 0. */
    best[off].oldPos = -1; best[off].last = NULL; best[off].present = 1;
    long newPos;
    e = extract_common(&ar, &d, &best[off], 0, &newPos);
    if (e) goto fail;
    if (best[off].oldPos + 1 >= oldLen && newPos + 1 >= newLen) {
        resultLast = best[off].last; haveResult = 1;
    }

    long minDiag = -1000000000L, maxDiag = 1000000000L;
    long editLength = 1;
    while (!haveResult && editLength <= maxEditLength) {
        long dlo = -editLength, dhi = editLength;
        if (dlo < minDiag) dlo = minDiag;
        if (dhi > maxDiag) dhi = maxDiag;
        for (long diagonal = dlo; diagonal <= dhi; diagonal += 2) {
            pathnode rp = best[off + diagonal - 1];
            pathnode ap = best[off + diagonal + 1];
            if (rp.present) best[off + diagonal - 1].present = 0;

            int canAdd = 0;
            if (ap.present) {
                long addPathNewPos = ap.oldPos - diagonal;
                canAdd = (0 <= addPathNewPos && addPathNewPos < newLen);
            }
            int canRemove = (rp.present && rp.oldPos + 1 < oldLen);
            if (!canAdd && !canRemove) { best[off + diagonal].present = 0; continue; }

            pathnode basePath;
            if (!canRemove || (canAdd && rp.oldPos < ap.oldPos)) {
                e = add_to_path(&ar, &ap, 1, 0, 0, &basePath);
            } else {
                e = add_to_path(&ar, &rp, 0, 1, 1, &basePath);
            }
            if (e) goto fail;

            e = extract_common(&ar, &d, &basePath, diagonal, &newPos);
            if (e) goto fail;

            if (basePath.oldPos + 1 >= oldLen && newPos + 1 >= newLen) {
                resultLast = basePath.last; haveResult = 1; break;
            }
            best[off + diagonal] = basePath;
            if (basePath.oldPos + 1 >= oldLen) { if (diagonal - 1 < maxDiag) maxDiag = diagonal - 1; }
            if (newPos + 1 >= newLen) { if (diagonal + 1 > minDiag) minDiag = diagonal + 1; }
        }
        editLength++;
    }

    if (haveResult) {
        e = build_values(&d, resultLast, out, ncomp);
    } else {
        /* Should be unreachable for finite input, but be safe. */
        e = DIFF_ERR_PARSE;
    }

fail:
    arena_free(&ar);
    free(best);
    free(ta); free(tb);
    return e;
}

/* ---- Hunks / structuredPatch (patch/create.js) ---------------------- */

typedef struct { uint8_t *p; size_t n; } bline; /* owned line bytes */

typedef struct {
    long oldStart, oldLines, newStart, newLines;
    bline *lines; size_t n, cap;
} hunk;

typedef struct { hunk *hunks; size_t n, cap; } patchset;

static void patchset_free(patchset *ps) {
    for (size_t i = 0; i < ps->n; i++) {
        for (size_t j = 0; j < ps->hunks[i].n; j++) free(ps->hunks[i].lines[j].p);
        free(ps->hunks[i].lines);
    }
    free(ps->hunks);
    ps->hunks = NULL; ps->n = ps->cap = 0;
}

/* A growing list of output lines (each owned). */
typedef struct { bline *v; size_t n, cap; } linelist;

static int linelist_push_bytes(linelist *l, char prefix, const uint8_t *p, size_t n) {
    if (l->n == l->cap) { size_t nc = l->cap ? l->cap * 2 : 16; bline *t = realloc(l->v, nc * sizeof(bline)); if (!t) return DIFF_ERR_OOM; l->v = t; l->cap = nc; }
    uint8_t *buf = (uint8_t *)malloc(1 + n);
    if (!buf) return DIFF_ERR_OOM;
    buf[0] = (uint8_t)prefix;
    if (n) memcpy(buf + 1, p, n);
    l->v[l->n].p = buf; l->v[l->n].n = 1 + n; l->n++;
    return DIFF_OK;
}
static void linelist_free(linelist *l) {
    for (size_t i = 0; i < l->n; i++) free(l->v[i].p);
    free(l->v); l->v = NULL; l->n = l->cap = 0;
}

/* Split a component value into line ranges (patch/create.js splitLines). */
static int split_value_lines(const uint8_t *val, size_t vlen, rng **out, size_t *count) {
    rng *arr = NULL; size_t n = 0, cap = 0;
    size_t i = 0;
    while (i < vlen) {
        size_t j = i;
        while (j < vlen && val[j] != '\n') j++;
        if (j < vlen) j++; /* include '\n' */
        if (n == cap) { cap = cap ? cap * 2 : 8; rng *t = realloc(arr, cap * sizeof(rng)); if (!t) { free(arr); return DIFF_ERR_OOM; } arr = t; }
        arr[n].off = i; arr[n].len = j - i; n++;
        i = j;
    }
    *out = arr; *count = n;
    return DIFF_OK;
}

static int hunk_from_range(hunk *h, long oldStart, long oldLines, long newStart, long newLines, linelist *cur) {
    h->oldStart = oldStart; h->oldLines = oldLines; h->newStart = newStart; h->newLines = newLines;
    h->lines = cur->v; h->n = cur->n; h->cap = cur->cap;
    cur->v = NULL; cur->n = cur->cap = 0; /* transfer ownership */
    return DIFF_OK;
}

static int patchset_push(patchset *ps, hunk *h) {
    if (ps->n == ps->cap) { size_t nc = ps->cap ? ps->cap * 2 : 8; hunk *t = realloc(ps->hunks, nc * sizeof(hunk)); if (!t) return DIFF_ERR_OOM; ps->hunks = t; ps->cap = nc; }
    ps->hunks[ps->n++] = *h;
    return DIFF_OK;
}

/*
 * structuredPatch: build hunks from the diff components (context = 4). Mirrors
 * diffLinesResultToPatch, including step 2's "\ No newline at end of file".
 */
static int structured_patch(const uint8_t *a, size_t alen, const uint8_t *b, size_t blen,
                            patchset *ps) {
    const long context = 4;
    comp *comps = NULL; size_t ncomp = 0;
    int e = diff_lines(a, alen, b, blen, &comps, &ncomp);
    if (e) return e;

    /* Precompute each component's line ranges (splitLines(value)). */
    rng **clines = (rng **)calloc(ncomp + 1, sizeof(rng *));
    size_t *cn = (size_t *)calloc(ncomp + 1, sizeof(size_t));
    if (!clines || !cn) { free(clines); free(cn); free(comps); return DIFF_ERR_OOM; }
    for (size_t i = 0; i < ncomp; i++) {
        e = split_value_lines(comps[i].val, comps[i].vlen, &clines[i], &cn[i]);
        if (e) goto done;
    }
    /* Sentinel component index ncomp: added/removed false, zero lines. */

    {
        long oldRangeStart = 0, newRangeStart = 0, oldLine = 1, newLine = 1;
        linelist cur; memset(&cur, 0, sizeof(cur));
        size_t total = ncomp + 1; /* includes appended sentinel */
        for (size_t i = 0; i < total; i++) {
            int added = 0, removed = 0;
            rng *lines; size_t nlines;
            const uint8_t *val;
            if (i < ncomp) { added = comps[i].added; removed = comps[i].removed; lines = clines[i]; nlines = cn[i]; val = comps[i].val; }
            else { lines = NULL; nlines = 0; val = a; }

            if (added || removed) {
                if (!oldRangeStart) {
                    oldRangeStart = oldLine; newRangeStart = newLine;
                    if (i > 0) {
                        /* curRange = contextLines(prev.lines.slice(-context)) */
                        rng *pl = clines[i - 1]; size_t pn = cn[i - 1];
                        const uint8_t *pval = comps[i - 1].val;
                        size_t take = (size_t)context < pn ? (size_t)context : pn;
                        size_t startk = pn - take;
                        for (size_t k = startk; k < pn; k++) {
                            e = linelist_push_bytes(&cur, ' ', pval + pl[k].off, pl[k].len);
                            if (e) { linelist_free(&cur); goto done; }
                        }
                        oldRangeStart -= (long)take; newRangeStart -= (long)take;
                    }
                }
                for (size_t k = 0; k < nlines; k++) {
                    e = linelist_push_bytes(&cur, added ? '+' : '-', val + lines[k].off, lines[k].len);
                    if (e) { linelist_free(&cur); goto done; }
                }
                if (added) newLine += (long)nlines; else oldLine += (long)nlines;
            } else {
                if (oldRangeStart) {
                    if ((long)nlines <= context * 2 && i < total - 2) {
                        for (size_t k = 0; k < nlines; k++) {
                            e = linelist_push_bytes(&cur, ' ', val + lines[k].off, lines[k].len);
                            if (e) { linelist_free(&cur); goto done; }
                        }
                    } else {
                        long contextSize = (long)nlines < context ? (long)nlines : context;
                        for (long k = 0; k < contextSize; k++) {
                            e = linelist_push_bytes(&cur, ' ', val + lines[k].off, lines[k].len);
                            if (e) { linelist_free(&cur); goto done; }
                        }
                        hunk h;
                        hunk_from_range(&h, oldRangeStart, oldLine - oldRangeStart + contextSize,
                                        newRangeStart, newLine - newRangeStart + contextSize, &cur);
                        e = patchset_push(ps, &h);
                        if (e) { for (size_t j = 0; j < h.n; j++) free(h.lines[j].p); free(h.lines); goto done; }
                        oldRangeStart = 0; newRangeStart = 0;
                    }
                }
                oldLine += (long)nlines; newLine += (long)nlines;
            }
        }
        linelist_free(&cur);
    }

    /* Step 2: strip trailing '\n' from each line; add no-newline markers. */
    for (size_t hi = 0; hi < ps->n; hi++) {
        hunk *h = &ps->hunks[hi];
        for (size_t i = 0; i < h->n; i++) {
            bline *ln = &h->lines[i];
            if (ln->n > 0 && ln->p[ln->n - 1] == '\n') {
                ln->n -= 1; /* drop trailing newline (keep buffer) */
            } else {
                /* insert "\ No newline at end of file" after i */
                const char *marker = "\\ No newline at end of file";
                size_t mlen = strlen(marker);
                if (h->n == h->cap) { size_t nc = h->cap ? h->cap * 2 : h->n + 1; bline *t = realloc(h->lines, nc * sizeof(bline)); if (!t) { e = DIFF_ERR_OOM; goto done; } h->lines = t; h->cap = nc; ln = &h->lines[i]; }
                uint8_t *mp = (uint8_t *)malloc(mlen);
                if (!mp) { e = DIFF_ERR_OOM; goto done; }
                memcpy(mp, marker, mlen);
                memmove(&h->lines[i + 2], &h->lines[i + 1], (h->n - (i + 1)) * sizeof(bline));
                h->lines[i + 1].p = mp; h->lines[i + 1].n = mlen;
                h->n += 1;
                i++; /* skip inserted line */
            }
        }
    }

done:
    for (size_t i = 0; i < ncomp; i++) free(clines[i]);
    free(clines); free(cn); free(comps);
    if (e) patchset_free(ps);
    return e;
}

/* ---- formatPatch (createPatch) -------------------------------------- */

/* The exact underline jsdiff emits with INCLUDE_HEADERS. */
#define DIFF_UNDERLINE "==================================================================="

static int format_patch(const patchset *ps, const char *file_name, sb *out) {
    /* ret joined by '\n', with a trailing '\n'. Build directly. */
    linelist ll; memset(&ll, 0, sizeof(ll)); /* reuse push helper to collect */
    /* We instead assemble into a temporary list of (ptr,len) then join. */
    /* Simpler: track whether we've written a line to insert '\n' separators. */
    int wrote = 0; int e;
    #define EMIT_LINE(fn) do { if (wrote) { if ((e = sb_putc(out, '\n'))) return e; } if ((e = (fn))) return e; wrote = 1; } while (0)

    /* Index: <file> (oldFileName == newFileName always here). */
    EMIT_LINE(sb_puts(out, "Index: "));
    if ((e = sb_puts(out, file_name))) return e;
    /* underline */
    EMIT_LINE(sb_puts(out, DIFF_UNDERLINE));
    /* file headers (oldHeader/newHeader undefined -> no tab suffix). */
    EMIT_LINE(sb_puts(out, "--- "));
    if ((e = sb_puts(out, file_name))) return e;
    EMIT_LINE(sb_puts(out, "+++ "));
    if ((e = sb_puts(out, file_name))) return e;

    for (size_t hi = 0; hi < ps->n; hi++) {
        const hunk *h = &ps->hunks[hi];
        long oldStart = h->oldStart, newStart = h->newStart;
        if (h->oldLines == 0) oldStart -= 1;
        if (h->newLines == 0) newStart -= 1;
        EMIT_LINE(sb_puts(out, "@@ -"));
        if ((e = sb_putlong(out, oldStart))) return e;
        if ((e = sb_putc(out, ','))) return e;
        if ((e = sb_putlong(out, h->oldLines))) return e;
        if ((e = sb_puts(out, " +"))) return e;
        if ((e = sb_putlong(out, newStart))) return e;
        if ((e = sb_putc(out, ','))) return e;
        if ((e = sb_putlong(out, h->newLines))) return e;
        if ((e = sb_puts(out, " @@"))) return e;
        for (size_t i = 0; i < h->n; i++) {
            EMIT_LINE(sb_putn(out, h->lines[i].p, h->lines[i].n));
        }
    }
    (void)ll;
    #undef EMIT_LINE
    /* trailing newline */
    return sb_putc(out, '\n');
}

int diff_create_patch(const char *file_name,
                      const uint8_t *a, size_t alen,
                      const uint8_t *b, size_t blen,
                      uint8_t **out, size_t *outlen) {
    patchset ps; memset(&ps, 0, sizeof(ps));
    int e = structured_patch(a, alen, b, blen, &ps);
    if (e) return e;
    sb s; memset(&s, 0, sizeof(s));
    e = format_patch(&ps, file_name, &s);
    patchset_free(&ps);
    if (e) { free(s.p); return e; }
    *out = s.p; *outlen = s.len;
    return DIFF_OK;
}

int diff_get_diff(long from_version, long to_version,
                  const uint8_t *a, size_t alen,
                  const uint8_t *b, size_t blen,
                  uint8_t **out, size_t *outlen) {
    patchset ps; memset(&ps, 0, sizeof(ps));
    int e = structured_patch(a, alen, b, blen, &ps);
    if (e) return e;
    sb s; memset(&s, 0, sizeof(s));
    /* Mirror textlog.js getDiff formatting (raw hunk numbers, no 0-adjust). */
    if (!(e = sb_puts(&s, "--- version ")) && !(e = sb_putlong(&s, from_version)) && !(e = sb_putc(&s, '\n')) &&
        !(e = sb_puts(&s, "+++ version ")) && !(e = sb_putlong(&s, to_version)) && !(e = sb_putc(&s, '\n'))) {
        for (size_t hi = 0; hi < ps.n && !e; hi++) {
            const hunk *h = &ps.hunks[hi];
            if ((e = sb_puts(&s, "@@ -"))) break;
            if ((e = sb_putlong(&s, h->oldStart))) break;
            if ((e = sb_putc(&s, ','))) break;
            if ((e = sb_putlong(&s, h->oldLines))) break;
            if ((e = sb_puts(&s, " +"))) break;
            if ((e = sb_putlong(&s, h->newStart))) break;
            if ((e = sb_putc(&s, ','))) break;
            if ((e = sb_putlong(&s, h->newLines))) break;
            if ((e = sb_puts(&s, " @@\n"))) break;
            for (size_t i = 0; i < h->n && !e; i++) {
                if ((e = sb_putn(&s, h->lines[i].p, h->lines[i].n))) break;
                e = sb_putc(&s, '\n');
            }
        }
    }
    patchset_free(&ps);
    if (e) { free(s.p); return e; }
    *out = s.p; *outlen = s.len;
    return DIFF_OK;
}

/* ---- parsePatch (patch/parse.js) ------------------------------------ */

typedef struct { const uint8_t *p; size_t n; } linref;

/* Split raw text into '\n'-separated line refs (no trailing newline included). */
static int split_nl(const uint8_t *s, size_t n, linref **out, size_t *count) {
    linref *arr = NULL; size_t c = 0, cap = 0;
    size_t i = 0;
    for (;;) {
        size_t j = i;
        while (j < n && s[j] != '\n') j++;
        if (c == cap) { cap = cap ? cap * 2 : 16; linref *t = realloc(arr, cap * sizeof(linref)); if (!t) { free(arr); return DIFF_ERR_OOM; } arr = t; }
        arr[c].p = s + i; arr[c].n = j - i; c++;
        if (j >= n) break;
        i = j + 1;
    }
    *out = arr; *count = c;
    return DIFF_OK;
}

/* Parse a single-file unified diff into hunks (owned line copies). */
static int parse_patch(const uint8_t *patch, size_t plen, patchset *ps) {
    linref *ls = NULL; size_t nl = 0;
    int e = split_nl(patch, plen, &ls, &nl);
    if (e) return e;
    size_t i = 0;

    /* Skip metadata until a ---/+++/@@ header (followed by whitespace). */
    while (i < nl) {
        const linref *L = &ls[i];
        if ((L->n >= 4 && (memcmp(L->p, "--- ", 4) == 0 || memcmp(L->p, "+++ ", 4) == 0)) ||
            (L->n >= 3 && memcmp(L->p, "@@ ", 3) == 0) ||
            (L->n == 3 && memcmp(L->p, "@@\t", 3) == 0)) {
            break;
        }
        /* @@ may be followed by any whitespace; but our own output uses "@@ ". */
        i++;
    }
    /* Skip the two file-header lines if present. */
    if (i < nl && ls[i].n >= 4 && memcmp(ls[i].p, "--- ", 4) == 0) i++;
    if (i < nl && ls[i].n >= 4 && memcmp(ls[i].p, "+++ ", 4) == 0) i++;

    while (i < nl) {
        const linref *L = &ls[i];
        /* End of this file's hunks on a new file/index/underline marker. */
        if ((L->n >= 4 && (memcmp(L->p, "--- ", 4) == 0)) ||
            (L->n >= 4 && memcmp(L->p, "+++ ", 4) == 0) ||
            (L->n >= 7 && memcmp(L->p, "Index: ", 7) == 0) ||
            (L->n >= 5 && memcmp(L->p, "diff ", 5) == 0) ||
            (L->n == strlen(DIFF_UNDERLINE) && memcmp(L->p, DIFF_UNDERLINE, L->n) == 0)) {
            break;
        }
        if (L->n >= 2 && L->p[0] == '@' && L->p[1] == '@') {
            /* Parse "@@ -oldStart[,oldLines] +newStart[,newLines] @@" */
            hunk h; memset(&h, 0, sizeof(h));
            const uint8_t *p = L->p; size_t pn = L->n, k = 0;
            long os = 0, ol = 1, nsv = 0, nln = 1; int haveOl = 0, haveNl = 0;
            /* skip "@@ -" */
            while (k < pn && p[k] != '-') k++;
            if (k < pn) k++;
            while (k < pn && p[k] >= '0' && p[k] <= '9') { os = os * 10 + (p[k] - '0'); k++; }
            if (k < pn && p[k] == ',') { k++; ol = 0; haveOl = 1; while (k < pn && p[k] >= '0' && p[k] <= '9') { ol = ol * 10 + (p[k] - '0'); k++; } }
            while (k < pn && p[k] != '+') k++;
            if (k < pn) k++;
            while (k < pn && p[k] >= '0' && p[k] <= '9') { nsv = nsv * 10 + (p[k] - '0'); k++; }
            if (k < pn && p[k] == ',') { k++; nln = 0; haveNl = 1; while (k < pn && p[k] >= '0' && p[k] <= '9') { nln = nln * 10 + (p[k] - '0'); k++; } }
            (void)haveOl; (void)haveNl;
            h.oldStart = os; h.oldLines = ol; h.newStart = nsv; h.newLines = nln;
            if (h.oldLines == 0) h.oldStart += 1;
            if (h.newLines == 0) h.newStart += 1;
            i++;
            long addCount = 0, removeCount = 0;
            for (; i < nl && (removeCount < h.oldLines || addCount < h.newLines ||
                              (ls[i].n > 0 && ls[i].p[0] == '\\')); i++) {
                const linref *HL = &ls[i];
                char op = (HL->n == 0 && i != nl - 1) ? ' ' : (HL->n > 0 ? (char)HL->p[0] : '\0');
                if (op == '+' || op == '-' || op == ' ' || op == '\\') {
                    if (h.n == h.cap) { size_t nc = h.cap ? h.cap * 2 : 8; bline *t = realloc(h.lines, nc * sizeof(bline)); if (!t) { e = DIFF_ERR_OOM; free(h.lines); goto fail; } h.lines = t; h.cap = nc; }
                    uint8_t *cp = (uint8_t *)malloc(HL->n ? HL->n : 1);
                    if (!cp) { e = DIFF_ERR_OOM; free(h.lines); goto fail; }
                    if (HL->n) memcpy(cp, HL->p, HL->n);
                    h.lines[h.n].p = cp; h.lines[h.n].n = HL->n; h.n++;
                    if (op == '+') addCount++;
                    else if (op == '-') removeCount++;
                    else if (op == ' ') { addCount++; removeCount++; }
                } else {
                    /* invalid line */
                    for (size_t j = 0; j < h.n; j++) free(h.lines[j].p);
                    free(h.lines);
                    e = DIFF_ERR_PARSE; goto fail;
                }
            }
            if (!addCount && h.newLines == 1) h.newLines = 0;
            if (!removeCount && h.oldLines == 1) h.oldLines = 0;
            if ((e = patchset_push(ps, &h))) { for (size_t j = 0; j < h.n; j++) free(h.lines[j].p); free(h.lines); goto fail; }
        } else if (L->n == 0) {
            i++;
        } else {
            e = DIFF_ERR_PARSE; goto fail;
        }
    }

fail:
    free(ls);
    if (e) patchset_free(ps);
    return e;
}

/* ---- Line-ending detection / conversion (patch/line-endings.js) ----- */

static int str_has_crlf(const uint8_t *s, size_t n) {
    for (size_t i = 0; i + 1 < n; i++) if (s[i] == '\r' && s[i + 1] == '\n') return 1;
    return 0;
}
static int has_only_win(const uint8_t *s, size_t n) {
    if (!str_has_crlf(s, n)) return 0;
    if (n > 0 && s[0] == '\n') return 0;
    for (size_t i = 0; i < n; i++) if (s[i] == '\n') { if (i == 0 || s[i - 1] != '\r') return 0; }
    return 1;
}
static int has_only_unix(const uint8_t *s, size_t n) {
    if (str_has_crlf(s, n)) return 0;
    for (size_t i = 0; i < n; i++) if (s[i] == '\n') return 1;
    return 0;
}
static int hline_ends_cr(const bline *l) { return l->n > 0 && l->p[l->n - 1] == '\r'; }
static int hline_starts_bs(const bline *l) { return l->n > 0 && l->p[0] == '\\'; }

static int patch_is_unix(const patchset *ps) {
    for (size_t i = 0; i < ps->n; i++)
        for (size_t j = 0; j < ps->hunks[i].n; j++) {
            const bline *l = &ps->hunks[i].lines[j];
            if (!hline_starts_bs(l) && hline_ends_cr(l)) return 0;
        }
    return 1;
}
static int patch_is_win(const patchset *ps) {
    int some = 0;
    for (size_t i = 0; i < ps->n; i++)
        for (size_t j = 0; j < ps->hunks[i].n; j++)
            if (hline_ends_cr(&ps->hunks[i].lines[j])) some = 1;
    if (!some) return 0;
    for (size_t i = 0; i < ps->n; i++)
        for (size_t j = 0; j < ps->hunks[i].n; j++) {
            const hunk *h = &ps->hunks[i];
            const bline *l = &h->lines[j];
            int nextBs = (j + 1 < h->n) && hline_starts_bs(&h->lines[j + 1]);
            if (!(hline_starts_bs(l) || hline_ends_cr(l) || nextBs)) return 0;
        }
    return 1;
}
static int patch_unix_to_win(patchset *ps) {
    for (size_t i = 0; i < ps->n; i++) {
        hunk *h = &ps->hunks[i];
        for (size_t j = 0; j < h->n; j++) {
            bline *l = &h->lines[j];
            int nextBs = (j + 1 < h->n) && hline_starts_bs(&h->lines[j + 1]);
            if (hline_starts_bs(l) || hline_ends_cr(l) || nextBs) continue;
            uint8_t *np = (uint8_t *)realloc(l->p, l->n + 1);
            if (!np) return DIFF_ERR_OOM;
            np[l->n] = '\r'; l->p = np; l->n += 1;
        }
    }
    return DIFF_OK;
}
static void patch_win_to_unix(patchset *ps) {
    for (size_t i = 0; i < ps->n; i++)
        for (size_t j = 0; j < ps->hunks[i].n; j++) {
            bline *l = &ps->hunks[i].lines[j];
            if (hline_ends_cr(l)) l->n -= 1;
        }
}

/* ---- applyPatch (patch/apply.js, fuzzFactor 0) ---------------------- */

typedef struct { long start, minL, maxL; int wantForward, backExh, fwdExh; long localOffset; } distiter;
static void distiter_init(distiter *it, long start, long minL, long maxL) {
    it->start = start; it->minL = minL; it->maxL = maxL;
    it->wantForward = 1; it->backExh = 0; it->fwdExh = 0; it->localOffset = 1;
}
static int distiter_next(distiter *it, long *out) {
    for (;;) {
        if (it->wantForward && !it->fwdExh) {
            if (it->backExh) it->localOffset++;
            else it->wantForward = 0;
            if (it->start + it->localOffset <= it->maxL) { *out = it->start + it->localOffset; return 1; }
            it->fwdExh = 1;
        }
        if (!it->backExh) {
            if (!it->fwdExh) it->wantForward = 1;
            if (it->minL <= it->start - it->localOffset) { *out = it->start - it->localOffset; it->localOffset++; return 1; }
            it->backExh = 1;
            continue; /* return iterator() */
        }
        return 0;
    }
}

static int lref_eq(const linref *a, const linref *b) {
    return a->n == b->n && (a->n == 0 || memcmp(a->p, b->p, a->n) == 0);
}

/*
 * applyHunk for fuzzFactor 0 (maxErrors == 0): a single linear pass with exact
 * context matching. Fills patched[] (linrefs) and returns 1 on success.
 */
static int apply_hunk0(const hunk *h, const linref *lines, size_t nlines, long toPos,
                       linref *patched, size_t *patchedLen, long *oldLineLastI) {
    size_t plen = 0;
    long nConsec = 0;
    int lastCtxMatched = 1, nextCtxMustMatch = 0;
    for (size_t k = 0; k < h->n; k++) {
        const bline *hl = &h->lines[k];
        char op = hl->n > 0 ? (char)hl->p[0] : ' ';
        linref content; content.p = hl->n > 0 ? hl->p + 1 : hl->p; content.n = hl->n > 0 ? hl->n - 1 : 0;
        if (op == '-') {
            linref cur; int inb = (toPos >= 0 && (size_t)toPos < nlines);
            if (inb) { cur = lines[toPos]; }
            if (inb && lref_eq(&cur, &content)) { toPos++; nConsec = 0; }
            else return 0;
        }
        if (op == '+') {
            if (!lastCtxMatched) return 0;
            patched[plen].p = content.p; patched[plen].n = content.n; plen++;
            nConsec = 0; nextCtxMustMatch = 1;
        }
        if (op == ' ') {
            nConsec++;
            int inb = (toPos >= 0 && (size_t)toPos < nlines);
            if (inb) patched[plen] = lines[toPos];
            if (inb && lref_eq(&lines[toPos], &content)) { plen++; lastCtxMatched = 1; nextCtxMustMatch = 0; toPos++; }
            else { (void)nextCtxMustMatch; return 0; }
        }
        /* op == '\\' : ignored */
    }
    plen -= (size_t)nConsec;
    toPos -= nConsec;
    *patchedLen = plen;
    *oldLineLastI = toPos - 1;
    return 1;
}

int diff_apply_patch(const uint8_t *source, size_t srclen,
                     const uint8_t *patch, size_t patchlen,
                     uint8_t **out, size_t *outlen, int *applied) {
    *applied = 0; *out = NULL; *outlen = 0;
    patchset ps; memset(&ps, 0, sizeof(ps));
    int e = parse_patch(patch, patchlen, &ps);
    if (e) return e;

    /* autoConvertLineEndings (default on). */
    if (has_only_win(source, srclen) && patch_is_unix(&ps)) {
        if ((e = patch_unix_to_win(&ps))) { patchset_free(&ps); return e; }
    } else if (has_only_unix(source, srclen) && patch_is_win(&ps)) {
        patch_win_to_unix(&ps);
    }

    linref *lines = NULL; size_t nlines = 0;
    e = split_nl(source, srclen, &lines, &nlines);
    if (e) { patchset_free(&ps); return e; }

    /* Empty patch => source unchanged. */
    if (ps.n == 0) {
        uint8_t *cp = (uint8_t *)malloc(srclen ? srclen : 1);
        if (!cp) { free(lines); patchset_free(&ps); return DIFF_ERR_OOM; }
        if (srclen) memcpy(cp, source, srclen);
        *out = cp; *outlen = srclen; *applied = 1;
        free(lines); patchset_free(&ps);
        return DIFF_OK;
    }

    /* EOFNL handling from the final hunk. */
    int removeEOFNL = 0, addEOFNL = 0;
    {
        const hunk *last = &ps.hunks[ps.n - 1];
        const bline *prev = NULL;
        for (size_t i = 0; i < last->n; i++) {
            const bline *l = &last->lines[i];
            if (l->n > 0 && l->p[0] == '\\') {
                if (prev && prev->n > 0 && prev->p[0] == '+') removeEOFNL = 1;
                else if (prev && prev->n > 0 && prev->p[0] == '-') addEOFNL = 1;
            }
            prev = l;
        }
    }
    int lastIsEmpty = (nlines > 0 && lines[nlines - 1].n == 0);
    if (removeEOFNL) {
        if (addEOFNL) {
            if (lastIsEmpty) { free(lines); patchset_free(&ps); return DIFF_OK; /* applied=0 => false */ }
        } else if (lastIsEmpty) {
            nlines -= 1; /* pop */
        } else {
            free(lines); patchset_free(&ps); return DIFF_OK; /* false */
        }
    } else if (addEOFNL) {
        if (!lastIsEmpty) {
            linref *t = realloc(lines, (nlines + 1) * sizeof(linref));
            if (!t) { free(lines); patchset_free(&ps); return DIFF_ERR_OOM; }
            lines = t; lines[nlines].p = source; lines[nlines].n = 0; nlines += 1; /* push '' */
        } else {
            free(lines); patchset_free(&ps); return DIFF_OK; /* false */
        }
    }

    /* Result assembly. */
    linref *result = NULL; size_t rn = 0, rcap = 0;
    #define RPUSH(lr) do { if (rn == rcap) { size_t nc = rcap ? rcap * 2 : 32; linref *t = realloc(result, nc * sizeof(linref)); if (!t) { e = DIFF_ERR_OOM; goto applyfail; } result = t; rcap = nc; } result[rn++] = (lr); } while (0)

    long minLine = 0, prevHunkOffset = 0;
    /* scratch patched buffer sized to worst case */
    size_t patchedCap = nlines + 4;
    for (size_t i = 0; i < ps.n; i++) patchedCap += ps.hunks[i].n;
    linref *patched = (linref *)malloc(patchedCap * sizeof(linref));
    if (!patched) { e = DIFF_ERR_OOM; goto applyfail; }

    for (size_t hi = 0; hi < ps.n; hi++) {
        const hunk *h = &ps.hunks[hi];
        long maxLine = (long)nlines - h->oldLines; /* + fuzz(0) */
        long toPos = h->oldStart + prevHunkOffset - 1;
        size_t plen = 0; long oldLineLastI = 0; int ok = 0;
        distiter it; distiter_init(&it, toPos, minLine, maxLine);
        long cur = toPos; int have = 1;
        while (have) {
            if (apply_hunk0(h, lines, nlines, cur, patched, &plen, &oldLineLastI)) { toPos = cur; ok = 1; break; }
            have = distiter_next(&it, &cur);
        }
        if (!ok) { free(patched); /* applied stays 0 => false */ e = DIFF_OK; goto applydone_false; }
        for (long k = minLine; k < toPos; k++) RPUSH(lines[k]);
        for (size_t k = 0; k < plen; k++) RPUSH(patched[k]);
        minLine = oldLineLastI + 1;
        prevHunkOffset = toPos + 1 - h->oldStart;
    }
    for (long k = minLine; k < (long)nlines; k++) RPUSH(lines[k]);
    free(patched);

    /* join(result, '\n') */
    {
        sb s; memset(&s, 0, sizeof(s));
        for (size_t k = 0; k < rn; k++) {
            if (k && (e = sb_putc(&s, '\n'))) { free(s.p); goto applyfail; }
            if ((e = sb_putn(&s, result[k].p, result[k].n))) { free(s.p); goto applyfail; }
        }
        *out = s.p ? s.p : (uint8_t *)calloc(1, 1);
        *outlen = s.len;
        *applied = 1;
    }
    free(result); free(lines); patchset_free(&ps);
    return DIFF_OK;

applydone_false:
    free(result); free(lines); patchset_free(&ps);
    return e;
applyfail:
    free(result); free(lines); patchset_free(&ps);
    return e;
    #undef RPUSH
}

/* ---- Binary copy/insert delta (private format) ---------------------- */
/*
 * Wire: a sequence of instructions. Each starts with an unsigned LEB128
 * control word; the low bit is the op (0 = INSERT, 1 = COPY) and the rest is a
 * length. INSERT is followed by `length` literal bytes; COPY is followed by a
 * LEB128 absolute source offset. Rebuilding walks the instructions, appending
 * literals or copying source[offset .. offset+length). Lengths/offsets are
 * bounds-checked on apply, so a corrupt delta fails cleanly (applied = 0).
 */

#define DELTA_K 16   /* min match length; also the hash-gram width */

static int sb_putvar(sb *b, uint64_t v) {
    uint8_t tmp[10]; int n = 0;
    do { uint8_t byte = (uint8_t)(v & 0x7f); v >>= 7; if (v) byte |= 0x80; tmp[n++] = byte; } while (v);
    return sb_putn(b, tmp, (size_t)n);
}

/* Read a LEB128 varint in [p,end); returns bytes consumed, or 0 if truncated
 * or malformed (> 64 bits). */
static int read_var(const uint8_t *p, const uint8_t *end, uint64_t *out) {
    uint64_t v = 0; int shift = 0; const uint8_t *s = p;
    while (p < end) {
        uint8_t byte = *p++;
        v |= (uint64_t)(byte & 0x7f) << shift;
        if (!(byte & 0x80)) { *out = v; return (int)(p - s); }
        shift += 7;
        if (shift >= 64) return 0;   /* overrun: malformed */
    }
    return 0;   /* truncated */
}

static uint64_t delta_hash(const uint8_t *p) {
    uint64_t h = 1469598103934665603ULL;   /* FNV-1a over DELTA_K bytes */
    for (int i = 0; i < DELTA_K; i++) { h ^= p[i]; h *= 1099511628211ULL; }
    return h;
}

static int emit_insert(sb *b, const uint8_t *p, size_t n) {
    int e = sb_putvar(b, (uint64_t)n << 1);   /* op bit 0 */
    if (e) return e;
    return sb_putn(b, p, n);
}
static int emit_copy(sb *b, size_t off, size_t len) {
    int e = sb_putvar(b, ((uint64_t)len << 1) | 1);   /* op bit 1 */
    if (e) return e;
    return sb_putvar(b, (uint64_t)off);
}

int diff_create_delta(const uint8_t *src, size_t srclen,
                      const uint8_t *tgt, size_t tgtlen,
                      uint8_t **out, size_t *outlen) {
    sb b; memset(&b, 0, sizeof(b));

    /* Hash index of source K-grams -> last start position (+1; 0 = empty).
     * Skipped for tiny/huge sources; the target is then one INSERT. */
    uint32_t *table = NULL; uint64_t mask = 0;
    if (srclen >= (size_t)DELTA_K && srclen <= 0xffffffffULL) {
        uint64_t cap = 16;
        while (cap < srclen * 2) cap <<= 1;
        table = (uint32_t *)calloc((size_t)cap, sizeof(uint32_t));
        if (!table) return DIFF_ERR_OOM;
        mask = cap - 1;
        for (size_t i = 0; i + DELTA_K <= srclen; i++)
            table[delta_hash(src + i) & mask] = (uint32_t)(i + 1);
    }

    int e = DIFF_OK;
    size_t j = 0, ins = 0;   /* pending insert = tgt[ins .. j) */
    while (table && j + (size_t)DELTA_K <= tgtlen) {
        uint32_t pe = table[delta_hash(tgt + j) & mask];
        size_t sp = pe ? (size_t)pe - 1 : 0;
        if (pe && sp + (size_t)DELTA_K <= srclen &&
            memcmp(src + sp, tgt + j, (size_t)DELTA_K) == 0) {
            size_t mlen = (size_t)DELTA_K;
            while (sp + mlen < srclen && j + mlen < tgtlen && src[sp + mlen] == tgt[j + mlen]) mlen++;
            /* absorb pending-insert bytes the match also covers backwards */
            while (sp > 0 && j > ins && src[sp - 1] == tgt[j - 1]) { sp--; j--; mlen++; }
            if (j > ins && (e = emit_insert(&b, tgt + ins, j - ins))) goto done;
            if ((e = emit_copy(&b, sp, mlen))) goto done;
            j += mlen; ins = j;
        } else {
            j++;
        }
    }
    if (tgtlen > ins && (e = emit_insert(&b, tgt + ins, tgtlen - ins))) goto done;

done:
    free(table);
    if (e) { free(b.p); return e; }
    *out = b.p ? b.p : (uint8_t *)calloc(1, 1);
    if (!*out) return DIFF_ERR_OOM;
    *outlen = b.len;
    return DIFF_OK;
}

int diff_apply_delta(const uint8_t *src, size_t srclen,
                     const uint8_t *delta, size_t deltalen,
                     uint8_t **out, size_t *outlen, int *applied) {
    *applied = 0;
    sb b; memset(&b, 0, sizeof(b));
    const uint8_t *p = delta, *end = delta + deltalen;
    int e = DIFF_OK;
    while (p < end) {
        uint64_t ctrl; int n = read_var(p, end, &ctrl);
        if (!n) goto bad;
        p += n;
        uint64_t len = ctrl >> 1;
        if ((ctrl & 1) == 0) {   /* INSERT */
            if ((uint64_t)(end - p) < len) goto bad;
            if ((e = sb_putn(&b, p, (size_t)len))) goto err;
            p += len;
        } else {                 /* COPY */
            uint64_t off; n = read_var(p, end, &off);
            if (!n) goto bad;
            p += n;
            if (off > srclen || len > srclen - off) goto bad;   /* off+len <= srclen */
            if ((e = sb_putn(&b, src + off, (size_t)len))) goto err;
        }
    }
    *out = b.p ? b.p : (uint8_t *)calloc(1, 1);
    if (!*out) { e = DIFF_ERR_OOM; goto err; }
    *outlen = b.len;
    *applied = 1;
    return DIFF_OK;

bad:
    free(b.p);
    return DIFF_OK;   /* applied stays 0 — malformed delta, like a patch that doesn't fit */
err:
    free(b.p);
    return e;
}
