/*
 * textindex.c - C port of src/textindex.js. See textindex.h.
 *
 * Tokenization, stop words, stemming and TF-IDF scoring are ported faithfully;
 * tree values (postings / term maps / lengths) are read and written as binjson
 * blobs. Small string->number dictionaries (insertion-ordered) mirror the JS
 * objects/Maps the reference builds.
 */
#include "textindex.h"
#include "bjcursor.h"
#include "bjfile.h"
#include "stemmer.h"

#include <stdlib.h>
#include <string.h>
#include <math.h>

/* ---- Insertion-ordered string -> number dictionary ------------------ */

/*
 * Entries live in insertion-ordered parallel arrays — iteration order must
 * match the JS reference's Map/object semantics (it fixes encoded field order
 * and sort tie-breaking). A linear-probing hash table over those arrays makes
 * lookups O(1): slots hold entry index + 1 (0 = empty), capacity is a power
 * of two kept at least 2x the entry count.
 */
typedef struct {
    char   **keys;   /* owned key bytes (not NUL-terminated) */
    int     *klen;
    double  *vals;
    int      n, cap;
    int     *slots;    /* hash index over entries; entry index + 1, 0 empty */
    int      slot_cap; /* power of two; 0 until first insert */
} dict;

static void dict_init(dict *d) { memset(d, 0, sizeof(*d)); }
static void dict_free(dict *d) {
    for (int i = 0; i < d->n; i++) free(d->keys[i]);
    free(d->keys); free(d->klen); free(d->vals); free(d->slots);
    memset(d, 0, sizeof(*d));
}
static uint32_t dict_hash(const char *k, int klen) {
    uint32_t h = 2166136261u; /* FNV-1a */
    for (int i = 0; i < klen; i++) { h ^= (uint8_t)k[i]; h *= 16777619u; }
    return h;
}
static int dict_find(const dict *d, const char *k, int klen) {
    if (!d->slot_cap) return -1;
    uint32_t m = (uint32_t)d->slot_cap - 1;
    for (uint32_t s = dict_hash(k, klen) & m;; s = (s + 1) & m) {
        int e = d->slots[s];
        if (!e) return -1;
        int i = e - 1;
        if (d->klen[i] == klen && memcmp(d->keys[i], k, (size_t)klen) == 0) return i;
    }
}
static void slot_insert(dict *d, int entry) {
    uint32_t m = (uint32_t)d->slot_cap - 1;
    uint32_t s = dict_hash(d->keys[entry], d->klen[entry]) & m;
    while (d->slots[s]) s = (s + 1) & m;
    d->slots[s] = entry + 1;
}
/* Rebuild the hash index from the entry arrays. Reuses the existing slots
 * allocation when it is still big enough, so callers that only shrink or
 * reorder entries (remove, compaction) cannot fail. */
static int dict_reindex(dict *d) {
    if (d->slot_cap < 2 * (d->n + 1)) {
        int nc = d->slot_cap ? d->slot_cap : 16;
        while (nc < 2 * (d->n + 1)) nc *= 2;
        int *ns = malloc((size_t)nc * sizeof(int));
        if (!ns) return BJ_ERR_OOM;
        free(d->slots);
        d->slots = ns; d->slot_cap = nc;
    }
    memset(d->slots, 0, (size_t)d->slot_cap * sizeof(int));
    for (int i = 0; i < d->n; i++) slot_insert(d, i);
    return BJ_OK;
}
static int dict_reserve(dict *d) {
    if (d->slot_cap < 2 * (d->n + 1) && dict_reindex(d)) return BJ_ERR_OOM;
    if (d->n < d->cap) return BJ_OK;
    int nc = d->cap ? d->cap * 2 : 8;
    char **nk = realloc(d->keys, (size_t)nc * sizeof(char *));
    int   *nl = realloc(d->klen, (size_t)nc * sizeof(int));
    double *nv = realloc(d->vals, (size_t)nc * sizeof(double));
    if (!nk || !nl || !nv) { free(nk); free(nl); free(nv); return BJ_ERR_OOM; }
    d->keys = nk; d->klen = nl; d->vals = nv; d->cap = nc;
    return BJ_OK;
}
static int dict_set(dict *d, const char *k, int klen, double val) {
    int i = dict_find(d, k, klen);
    if (i >= 0) { d->vals[i] = val; return BJ_OK; }
    if (dict_reserve(d)) return BJ_ERR_OOM;
    char *cp = malloc((size_t)klen ? (size_t)klen : 1);
    if (!cp) return BJ_ERR_OOM;
    if (klen) memcpy(cp, k, (size_t)klen);
    d->keys[d->n] = cp; d->klen[d->n] = klen; d->vals[d->n] = val;
    slot_insert(d, d->n); d->n++;
    return BJ_OK;
}
static int dict_inc(dict *d, const char *k, int klen) {
    int i = dict_find(d, k, klen);
    if (i >= 0) { d->vals[i] += 1; return BJ_OK; }
    return dict_set(d, k, klen, 1);
}
static void dict_remove(dict *d, const char *k, int klen) {
    int i = dict_find(d, k, klen);
    if (i < 0) return;
    free(d->keys[i]);
    for (int j = i; j < d->n - 1; j++) { d->keys[j] = d->keys[j + 1]; d->klen[j] = d->klen[j + 1]; d->vals[j] = d->vals[j + 1]; }
    d->n--;
    dict_reindex(d); /* entry indices shifted; cannot fail (table shrank) */
}

/* ---- Stop words (verbatim from src/textindex.js) -------------------- */

static const char *const STOPWORDS[] = {
    "a","about","after","all","also","am","an","and","another","any","are",
    "around","as","at","be","because","been","before","being","between","both",
    "but","by","came","can","come","could","did","do","each","for","from",
    "get","got","has","had","he","have","her","here","him","himself","his",
    "how","i","if","in","into","is","it","like","make","many","me","might",
    "more","most","much","must","my","never","now","of","on","only","or",
    "other","our","out","over","said","same","see","should","since","some",
    "still","such","take","than","that","the","their","them","then","there",
    "these","they","this","those","through","to","too","under","up","very",
    "was","way","we","well","were","what","where","which","while","who",
    "with","would","you","your"
};
static const int STOPWORDS_N = (int)(sizeof STOPWORDS / sizeof STOPWORDS[0]);

static int is_stopword(const char *w, int len) {
    for (int i = 0; i < STOPWORDS_N; i++) {
        const char *s = STOPWORDS[i];
        if ((int)strlen(s) == len && memcmp(s, w, (size_t)len) == 0) return 1;
    }
    return 0;
}

/* ASCII word characters plus every UTF-8 lead/continuation byte, so
 * non-ASCII text (accented Latin, Cyrillic, CJK, ...) forms tokens instead
 * of being silently dropped — the JS reference's \w keeps the old ASCII
 * behavior. Whole runs are one token; no Unicode segmentation. */
static int is_word_char(unsigned char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
           (c >= '0' && c <= '9') || c == '_' || c >= 0x80;
}

/*
 * Tokenize `text`: lowercase ASCII letters, split on non-word runs, drop
 * stop words, Porter-stem each surviving token and count stem frequencies
 * into `out` (insertion order = first-seen order, matching the reference's
 * Map/object semantics). Tokens containing non-ASCII bytes are indexed
 * verbatim: the Porter stemmer is English-only, and byte-level lowercasing
 * or suffix-stripping inside multibyte sequences would corrupt them (so no
 * case or accent folding beyond ASCII either).
 */
static int tokenize_count(const char *text, int len, dict *out) {
    char *low = NULL, *stem = NULL;
    int lowcap = 0, stemcap = 0, e = BJ_OK;
    int i = 0;
    while (i < len) {
        if (!is_word_char((unsigned char)text[i])) { i++; continue; }
        int s = i;
        while (i < len && is_word_char((unsigned char)text[i])) i++;
        int wlen = i - s;
        if (wlen + 1 > lowcap) { lowcap = wlen + 1; char *t = realloc(low, (size_t)lowcap); if (!t) { e = BJ_ERR_OOM; break; } low = t; }
        int ascii = 1;
        for (int j = 0; j < wlen; j++) {
            char c = text[s + j];
            if ((unsigned char)c >= 0x80) ascii = 0;
            if (c >= 'A' && c <= 'Z') c = (char)(c + 32);
            low[j] = c;
        }
        if (is_stopword(low, wlen)) continue;
        if (!ascii) {
            if ((e = dict_inc(out, low, wlen))) break;
            continue;
        }
        if (wlen + 2 > stemcap) { stemcap = wlen + 2; char *t = realloc(stem, (size_t)stemcap); if (!t) { e = BJ_ERR_OOM; break; } stem = t; }
        int slen = stemmer_stem(low, wlen, stem);
        if ((e = dict_inc(out, stem, slen))) break;
    }
    free(low); free(stem);
    return e;
}

/* ---- binjson decoding (wire primitives in bjcursor.h) ---------------- */

/* Decode a { string: number } object blob into `d`. */
static int decode_obj(const uint8_t *blob, size_t len, dict *d) {
    cur c = { blob, len, 0 };
    uint32_t count; int e = object_begin(&c, &count);
    if (e) return e;
    for (uint32_t i = 0; i < count; i++) {
        const uint8_t *kn; uint32_t klen; double v;
        if ((e = take_key(&c, &kn, &klen))) return e;
        if ((e = read_number(&c, &v))) return e;
        if ((e = dict_set(d, (const char *)kn, (int)klen, v))) return e;
    }
    return BJ_OK;
}

/* ---- Value encoders (binjson) --------------------------------------- */

/* Encode entries [from, to) of a dict as a { string: int } object into a
 * reusable builder; expose bytes via data/dlen (valid until the builder is
 * reset or freed). */
static int encode_obj_range(bj_builder *b, const dict *d, int from, int to,
                            const uint8_t **data, size_t *dlen) {
    bj_builder_reset(b);
    bj_begin_object(b);
    for (int i = from; i < to; i++) {
        bj_put_key(b, (const uint8_t *)d->keys[i], (uint32_t)d->klen[i]);
        bj_put_int(b, (int64_t)d->vals[i]);
    }
    bj_end_object(b);
    int e = bj_builder_error(b);
    if (e) return e;
    const uint8_t *p = bj_builder_data(b, dlen);
    if (!p) return BJ_ERR_STATE;
    *data = p;
    return BJ_OK;
}
static int encode_obj(bj_builder *b, const dict *d, const uint8_t **data, size_t *dlen) {
    return encode_obj_range(b, d, 0, d->n, data, dlen);
}

static bpt_key str_key(const char *s, int len) {
    bpt_key k; k.is_string = 1; k.num = 0; k.str = (const uint8_t *)s; k.str_len = (uint32_t)len; return k;
}

/* ---- Block-partitioned postings -------------------------------------- */

/*
 * A term's posting list is stored as fixed-capacity blocks so that adding a
 * document rewrites O(1) bytes instead of the whole list (the legacy layout
 * rewrote a blob that grows with every matching document: O(d²) total bytes
 * for a term matching d docs). Keys in the index tree:
 *
 *   "term"           legacy single { docId: tf } blob (the JS layout) —
 *                    still read, and migrated to blocks when next written
 *   "term\0"         header: block count as one INT value
 *   "term\0<%08x>"   block i: { docId: tf } object of at most
 *                    TIX_BLOCK_DOCS entries, docs in add order
 *
 * Stems never contain NUL (the tokenizer only emits word characters), so
 * the "\0" suffix cannot collide with another term. New entries go to the
 * highest block; a document re-added later can therefore appear in an old
 * block too, and readers merge blocks in order into one dict where the
 * later entry replaces the earlier (the same tf-wins, position-kept
 * semantics as updating the legacy blob). A removal that empties every
 * block deletes the whole chain, mirroring the legacy key deletion.
 */
#define TIX_BLOCK_DOCS 16
#define TIX_MAX_BLOCKS (1 << 24)   /* sanity cap against hostile headers */

/* Reusable key buffer holding term + '\0' + 8 hex digits. The bpt_key
 * returned by the accessors points into the buffer: valid until the next
 * accessor call or tkey_free. */
typedef struct { char *buf; int term_len; } tkey;

static int tkey_init(tkey *k, const char *term, int tlen) {
    k->buf = (char *)malloc((size_t)tlen + 9);
    if (!k->buf) return BJ_ERR_OOM;
    memcpy(k->buf, term, (size_t)tlen);
    k->buf[tlen] = '\0';
    k->term_len = tlen;
    return BJ_OK;
}
static void tkey_free(tkey *k) { free(k->buf); k->buf = NULL; }
static bpt_key tkey_header(const tkey *k) { return str_key(k->buf, k->term_len + 1); }
static bpt_key tkey_block(tkey *k, int i) {
    static const char H[] = "0123456789abcdef";
    for (int j = 0; j < 8; j++)
        k->buf[k->term_len + 1 + j] = H[(i >> ((7 - j) * 4)) & 0xf];
    return str_key(k->buf, k->term_len + 9);
}
/* Upper bound for a range scan over the term's chain: 0xff sorts after
 * every hex digit, so [header, max] covers the header and all blocks and
 * nothing of any other term. */
static bpt_key tkey_range_max(tkey *k) {
    k->buf[k->term_len + 1] = (char)0xff;
    return str_key(k->buf, k->term_len + 2);
}
/* Parse the block index out of a full block key (term + NUL + 8 hex). */
static int tkey_parse_index(const bpt_key *k, int term_len, int *out) {
    int64_t v = 0;
    for (int j = 0; j < 8; j++) {
        uint8_t c = k->str[term_len + 1 + j];
        int d;
        if (c >= '0' && c <= '9') d = c - '0';
        else if (c >= 'a' && c <= 'f') d = c - 'a' + 10;
        else return BJ_ERR_STATE;
        v = (v << 4) | d;
    }
    if (v >= TIX_MAX_BLOCKS) return BJ_ERR_STATE;
    *out = (int)v;
    return BJ_OK;
}

/* Decode a header value into a validated block count. */
static int decode_block_count(const uint8_t *vp, size_t vl, int *out) {
    cur c = { vp, vl, 0 };
    double d;
    int e = read_number(&c, &d);
    if (e) return e;
    if (!(d >= 1) || d > TIX_MAX_BLOCKS || (double)(int)d != d) return BJ_ERR_STATE;
    *out = (int)d;
    return BJ_OK;
}

static int write_block(bpt *index, bj_builder *b, tkey *k, int i,
                       const dict *d, int from, int to) {
    const uint8_t *data; size_t dlen;
    int e = encode_obj_range(b, d, from, to, &data, &dlen);
    if (e) return e;
    bpt_key bk = tkey_block(k, i);
    return bpt_add(index, &bk, data, (uint32_t)dlen);
}
static int write_header(bpt *index, bj_builder *b, const tkey *k, int nblocks) {
    bj_builder_reset(b);
    bj_put_int(b, nblocks);
    int e = bj_builder_error(b);
    if (e) return e;
    size_t dlen;
    const uint8_t *data = bj_builder_data(b, &dlen);
    if (!data) return BJ_ERR_STATE;
    bpt_key hk = tkey_header(k);
    return bpt_add(index, &hk, data, (uint32_t)dlen);
}

/*
 * Load a term's full posting list into `post`: blocks merged in key order
 * (later entries replace earlier), or the legacy blob. A range cursor over
 * [header, term\0\xff] reads each tree leaf once instead of paying a
 * root-to-leaf descent per block. *state reports what was found:
 * 1 = blocks, -1 = legacy blob, 0 = no postings.
 */
static int load_postings(bpt *index, const char *term, int tlen,
                         dict *post, int *state) {
    *state = 0;
    tkey k;
    int e = tkey_init(&k, term, tlen);
    if (e) return e;

    bpt_key mn = tkey_header(&k);
    bpt_key mx = tkey_range_max(&k);
    bpt_cursor *c = bpt_cursor_open(index, &mn, &mx);
    if (!c) { tkey_free(&k); return BJ_ERR_OOM; }
    int r, have_header = 0;
    bpt_key ck; const uint8_t *v; size_t vl;
    while ((r = bpt_cursor_next(c, &ck, &v, &vl)) == 1) {
        if (!ck.is_string) continue;
        if (ck.str_len == (uint32_t)tlen + 1) {          /* header */
            have_header = 1;
        } else if (ck.str_len == (uint32_t)tlen + 9) {   /* a block */
            if ((e = decode_obj(v, vl, post))) break;
            *state = 1;
        }
    }
    bpt_cursor_close(c);
    if (!e && r < 0) e = r;

    /* The header is the migration commit point: blocks without one are
     * leftovers of an interrupted legacy migration, and the legacy blob
     * (deleted only after the header lands) is still authoritative. */
    if (!e && *state == 1 && !have_header) {
        dict_free(post);
        dict_init(post);
        *state = 0;
    }

    if (!e && *state == 0) {
        int found; const uint8_t *vp;
        bpt_key tk = str_key(term, tlen);
        e = bpt_search(index, &tk, &found, &vp, &vl);
        if (!e && found) {
            e = decode_obj(vp, vl, post);
            if (!e) *state = -1;
        }
    }
    tkey_free(&k);
    return e;
}

/*
 * Merge (doc_id -> tfv) into a term's postings, touching only the active
 * (highest) block. Legacy blobs are migrated to blocks first: split into
 * capacity-sized blocks, header written after the blocks (a crash between
 * leaves unreferenced block keys, never lost data), legacy key deleted
 * last so one of the two layouts is always complete.
 */
static int post_add(bpt *index, bj_builder *b, const char *term, int tlen,
                    const char *doc_id, int doc_id_len, double tfv) {
    tkey k;
    int e = tkey_init(&k, term, tlen);
    if (e) return e;
    dict blk; dict_init(&blk);
    int found; const uint8_t *vp; size_t vl;

    bpt_key hk = tkey_header(&k);
    if ((e = bpt_search(index, &hk, &found, &vp, &vl))) goto out;

    if (found) {
        int n;
        if ((e = decode_block_count(vp, vl, &n))) goto out;
        bpt_key bk = tkey_block(&k, n - 1);
        int bf;
        e = bpt_search(index, &bk, &bf, &vp, &vl);
        if (!e && bf) e = decode_obj(vp, vl, &blk);
        if (e) goto out;
        if (dict_find(&blk, doc_id, doc_id_len) < 0 && blk.n >= TIX_BLOCK_DOCS) {
            /* Active block full: this doc starts block n. */
            dict_free(&blk);
            dict_init(&blk);
            if ((e = dict_set(&blk, doc_id, doc_id_len, tfv))) goto out;
            if ((e = write_block(index, b, &k, n, &blk, 0, blk.n))) goto out;
            e = write_header(index, b, &k, n + 1);
        } else {
            if ((e = dict_set(&blk, doc_id, doc_id_len, tfv))) goto out;
            e = write_block(index, b, &k, n - 1, &blk, 0, blk.n);
        }
        goto out;
    }

    /* No header: legacy blob (migrate to blocks) or a brand-new term. */
    {
        bpt_key tk = str_key(term, tlen);
        if ((e = bpt_search(index, &tk, &found, &vp, &vl))) goto out;
        if (found && (e = decode_obj(vp, vl, &blk))) goto out;
        if ((e = dict_set(&blk, doc_id, doc_id_len, tfv))) goto out;
        int n = (blk.n + TIX_BLOCK_DOCS - 1) / TIX_BLOCK_DOCS;
        for (int i = 0; i < n && !e; i++) {
            int from = i * TIX_BLOCK_DOCS;
            int to = from + TIX_BLOCK_DOCS < blk.n ? from + TIX_BLOCK_DOCS : blk.n;
            e = write_block(index, b, &k, i, &blk, from, to);
        }
        if (!e) e = write_header(index, b, &k, n);
        if (!e && found) e = bpt_delete(index, &tk);
    }

out:
    dict_free(&blk);
    tkey_free(&k);
    return e;
}

/* Remove doc_id from a term's postings; deletes the whole chain (or the
 * legacy key) when no documents remain, like the legacy layout did. */
static int post_remove(bpt *index, bj_builder *b, const char *term, int tlen,
                       const char *doc_id, int doc_id_len) {
    tkey k;
    int e = tkey_init(&k, term, tlen);
    if (e) return e;
    int found; const uint8_t *vp; size_t vl;

    bpt_key hk = tkey_header(&k);
    if ((e = bpt_search(index, &hk, &found, &vp, &vl))) { tkey_free(&k); return e; }

    if (found) {
        int n;
        if ((e = decode_block_count(vp, vl, &n))) { tkey_free(&k); return e; }

        /* One range scan: count surviving entries and note the indices of
         * blocks holding the doc (usually one; re-adds can leave more). */
        int *hits = NULL, n_hits = 0, cap_hits = 0, remaining = 0;
        bpt_key mn = tkey_header(&k);
        bpt_key mx = tkey_range_max(&k);
        bpt_cursor *c = bpt_cursor_open(index, &mn, &mx);
        if (!c) { tkey_free(&k); return BJ_ERR_OOM; }
        int r;
        bpt_key ck; const uint8_t *v; size_t cvl;
        while ((r = bpt_cursor_next(c, &ck, &v, &cvl)) == 1) {
            if (!ck.is_string || ck.str_len != (uint32_t)tlen + 9) continue;
            dict blk; dict_init(&blk);
            if ((e = decode_obj(v, cvl, &blk))) { dict_free(&blk); break; }
            if (dict_find(&blk, doc_id, doc_id_len) >= 0) {
                int idx;
                if ((e = tkey_parse_index(&ck, tlen, &idx))) { dict_free(&blk); break; }
                if (n_hits == cap_hits) {
                    int nc = cap_hits ? cap_hits * 2 : 4;
                    int *nh = (int *)realloc(hits, (size_t)nc * sizeof(int));
                    if (!nh) { dict_free(&blk); e = BJ_ERR_OOM; break; }
                    hits = nh; cap_hits = nc;
                }
                hits[n_hits++] = idx;
                remaining += blk.n - 1;
            } else {
                remaining += blk.n;
            }
            dict_free(&blk);
        }
        bpt_cursor_close(c);
        if (!e && r < 0) e = r;

        /* Rewrite each block that held the doc (cursor closed: writes ok). */
        for (int h = 0; h < n_hits && !e; h++) {
            bpt_key bk = tkey_block(&k, hits[h]);
            int bf;
            if ((e = bpt_search(index, &bk, &bf, &vp, &vl))) break;
            if (!bf) continue;
            dict blk; dict_init(&blk);
            e = decode_obj(vp, vl, &blk);
            if (!e) {
                dict_remove(&blk, doc_id, doc_id_len);
                e = write_block(index, b, &k, hits[h], &blk, 0, blk.n);
            }
            dict_free(&blk);
        }
        free(hits);

        if (!e && remaining == 0) {
            for (int i = 0; i < n && !e; i++) {
                bpt_key bk = tkey_block(&k, i);
                e = bpt_delete(index, &bk);
            }
            if (!e) e = bpt_delete(index, &hk);
        }
    } else {
        bpt_key tk = str_key(term, tlen);
        e = bpt_search(index, &tk, &found, &vp, &vl);
        if (!e && found) {
            dict post; dict_init(&post);
            e = decode_obj(vp, vl, &post);
            if (!e) {
                dict_remove(&post, doc_id, doc_id_len);
                if (post.n == 0) {
                    e = bpt_delete(index, &tk);
                } else {
                    const uint8_t *data; size_t dlen;
                    if (!(e = encode_obj(b, &post, &data, &dlen)))
                        e = bpt_add(index, &tk, data, (uint32_t)dlen);
                }
            }
            dict_free(&post);
        }
    }
    tkey_free(&k);
    return e;
}

/* ---- Cross-tree commit journal (crash atomicity) ---------------------- */

/*
 * One tix_add / tix_remove / tix_clear spans many individual tree commits
 * across three append-only files; a crash in between leaves the index
 * internally inconsistent (postings without a document length, half-updated
 * terms). The journal makes the operations atomic: after all tree writes
 * land, the three file lengths are recorded, and tix_recover rewinds each
 * tree to the newest recorded triple — rewinding an append-only file to a
 * commit boundary restores exactly the state at that commit, so a partially
 * applied operation disappears whole.
 *
 * Layout: two fixed 48-byte slots written alternately (ping-pong), so a
 * torn slot write can only damage the slot being replaced while the other
 * still holds the previous transaction. An empty journal imposes no
 * constraint (pre-journal or freshly compacted files are adopted as-is).
 */
#define TIXJ_SLOT 48

static void wr32le(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}
static void wr64le(uint8_t *p, uint64_t v) {
    for (int i = 0; i < 8; i++) p[i] = (uint8_t)(v >> (i * 8));
}

static void tixj_encode(uint8_t s[TIXJ_SLOT], uint64_t txn, const uint64_t lens[3]) {
    memset(s, 0, TIXJ_SLOT);
    memcpy(s, "TIXJ", 4);
    wr32le(s + 4, 1);   /* version */
    wr64le(s + 8, txn);
    wr64le(s + 16, lens[0]);
    wr64le(s + 24, lens[1]);
    wr64le(s + 32, lens[2]);
    wr32le(s + 40, bjfile_crc32(0, s, 40));
}
static int tixj_decode(const uint8_t *s, uint64_t *txn, uint64_t lens[3]) {
    if (memcmp(s, "TIXJ", 4) != 0) return 0;
    if (rdu32(s + 4) != 1) return 0;
    if (rdu32(s + 40) != bjfile_crc32(0, s, 40)) return 0;
    *txn = rdu64(s + 8);
    lens[0] = rdu64(s + 16);
    lens[1] = rdu64(s + 24);
    lens[2] = rdu64(s + 32);
    return 1;
}

/* Read the valid slots, newest first. Returns 0..2 or a negative error. */
static int tixj_read(const bj_io *j, uint64_t txn[2], uint64_t lens[2][3]) {
    uint8_t buf[2 * TIXJ_SLOT];
    memset(buf, 0, sizeof(buf));
    uint64_t sz = j->size(j->ctx);
    if (sz > 0) {
        uint32_t want = sz < sizeof(buf) ? (uint32_t)sz : (uint32_t)sizeof(buf);
        int64_t got = j->read(j->ctx, 0, buf, want);
        if (got < 0) return (int)got;
    }
    uint64_t t0, l0[3], t1, l1[3];
    int v0 = tixj_decode(buf, &t0, l0);
    int v1 = tixj_decode(buf + TIXJ_SLOT, &t1, l1);
    if (v0 && v1) {
        int newer0 = t0 >= t1;
        txn[0] = newer0 ? t0 : t1;
        memcpy(lens[0], newer0 ? l0 : l1, sizeof(l0));
        txn[1] = newer0 ? t1 : t0;
        memcpy(lens[1], newer0 ? l1 : l0, sizeof(l0));
        return 2;
    }
    if (v0 || v1) {
        txn[0] = v0 ? t0 : t1;
        memcpy(lens[0], v0 ? l0 : l1, sizeof(l0));
        return 1;
    }
    return 0;
}

/* Record the current tree lengths as one committed transaction. */
static int tixj_commit(const bj_io *j, bpt *index, bpt *doc_terms, bpt *doc_lengths) {
    uint64_t txn[2], lens[2][3];
    int n = tixj_read(j, txn, lens);
    if (n < 0) return n;
    uint64_t next = n ? txn[0] + 1 : 1;
    uint64_t cur[3] = {
        bpt_file_len(index), bpt_file_len(doc_terms), bpt_file_len(doc_lengths)
    };
    uint8_t s[TIXJ_SLOT];
    tixj_encode(s, next, cur);
    /* txn 1 lands in slot 0 (empty file: no write gap), then alternate. */
    int32_t w = j->write(j->ctx, (next & 1) ? 0 : TIXJ_SLOT, s, TIXJ_SLOT);
    return w ? (int)w : BJ_OK;
}

int tix_recover(const bj_io *journal, bpt *index, bpt *doc_terms, bpt *doc_lengths) {
    if (!journal) return BJ_OK;
    uint64_t txn[2], lens[2][3];
    int n = tixj_read(journal, txn, lens);
    if (n < 0) return n;
    if (n == 0) return BJ_OK;   /* fresh or reset journal: adopt files as-is */
    bpt *trees[3] = { index, doc_terms, doc_lengths };
    for (int c = 0; c < n; c++) {
        int ok = 1;
        for (int i = 0; i < 3; i++)
            if (bpt_file_len(trees[i]) < lens[c][i]) ok = 0;
        if (!ok) continue;   /* a tree lost this transaction: try the older one */
        int e = BJ_OK;
        for (int i = 0; i < 3 && !e; i++) e = bpt_rewind(trees[i], lens[c][i]);
        return e;
    }
    /* The trees are behind every recorded transaction — more was lost than
     * the journal can reconcile. Refuse rather than serve inconsistency. */
    return BJ_ERR_STATE;
}

/* ---- add ------------------------------------------------------------ */

int tix_add(bpt *index, bpt *doc_terms, bpt *doc_lengths, const bj_io *journal,
            const char *doc_id, int doc_id_len,
            const char *text, int text_len) {
    dict tf; dict_init(&tf);
    int e = tokenize_count(text, text_len, &tf);
    if (e) { dict_free(&tf); return e; }

    bj_builder *b = bj_builder_new();
    if (!b) { dict_free(&tf); return BJ_ERR_OOM; }

    /*
     * Re-adding an existing id replaces the document (a true diff): the doc
     * is removed from the postings of terms that vanished from the new
     * text, and documentTerms/documentLengths reflect only the new text.
     * The JS reference instead merges new terms over old, leaving stale
     * postings behind — a deliberate divergence (re-add must equal
     * remove-then-add for a database index). Terms present in both
     * versions are simply overwritten by post_add below.
     */
    {
        bpt_key kd = str_key(doc_id, doc_id_len);
        int found; const uint8_t *vp; size_t vl;
        e = bpt_search(doc_terms, &kd, &found, &vp, &vl);
        if (!e && found) {
            dict old; dict_init(&old);
            e = decode_obj(vp, vl, &old);
            for (int i = 0; i < old.n && !e; i++) {
                if (dict_find(&tf, old.keys[i], old.klen[i]) >= 0) continue;
                e = post_remove(index, b, old.keys[i], old.klen[i], doc_id, doc_id_len);
            }
            dict_free(&old);
        }
    }

    /* Postings: merge each term's (docId, tf) into its active block. */
    for (int i = 0; i < tf.n && !e; i++)
        e = post_add(index, b, tf.keys[i], tf.klen[i], doc_id, doc_id_len, tf.vals[i]);

    /* documentTerms: exactly the new term map. */
    if (!e) {
        bpt_key kd = str_key(doc_id, doc_id_len);
        double doc_len = 0;
        for (int i = 0; i < tf.n; i++) doc_len += tf.vals[i];
        const uint8_t *data; size_t dlen;
        if (!(e = encode_obj(b, &tf, &data, &dlen)))
            e = bpt_add(doc_terms, &kd, data, (uint32_t)dlen);
        /* documentLengths: total term count as an int value. */
        if (!e) {
            bj_builder_reset(b);
            bj_put_int(b, (int64_t)doc_len);
            e = bj_builder_error(b);
            if (!e) {
                size_t dlen2; const uint8_t *data2 = bj_builder_data(b, &dlen2);
                if (!data2) e = BJ_ERR_STATE;
                else e = bpt_add(doc_lengths, &kd, data2, (uint32_t)dlen2);
            }
        }
    }

    if (!e && journal) e = tixj_commit(journal, index, doc_terms, doc_lengths);
    bj_builder_free(b);
    dict_free(&tf);
    return e;
}

/* ---- remove --------------------------------------------------------- */

int tix_remove(bpt *index, bpt *doc_terms, bpt *doc_lengths, const bj_io *journal,
               const char *doc_id, int doc_id_len, int *removed) {
    *removed = 0;
    bpt_key kd = str_key(doc_id, doc_id_len);
    int found; const uint8_t *vp; size_t vl;
    int e = bpt_search(doc_terms, &kd, &found, &vp, &vl);
    if (e) return e;
    if (!found) return BJ_OK;

    dict terms; dict_init(&terms);
    e = decode_obj(vp, vl, &terms);

    bj_builder *b = bj_builder_new();
    if (!b) { dict_free(&terms); return BJ_ERR_OOM; }

    for (int i = 0; i < terms.n && !e; i++)
        e = post_remove(index, b, terms.keys[i], terms.klen[i], doc_id, doc_id_len);

    if (!e) e = bpt_delete(doc_terms, &kd);
    if (!e) e = bpt_delete(doc_lengths, &kd);
    if (!e && journal) e = tixj_commit(journal, index, doc_terms, doc_lengths);
    if (!e) *removed = 1;

    bj_builder_free(b);
    dict_free(&terms);
    return e;
}

/* ---- clear ---------------------------------------------------------- */

/*
 * Clear by resetting each tree to a fresh empty file (bpt_reset) — O(1),
 * where the old per-key deletion appended a rewritten path per key and
 * could more than double the file it was clearing.
 *
 * Unlike add/remove, clear is destructive truncation and therefore not
 * crash-atomic: the journal is emptied first (so recovery adopts whatever
 * state the trees are in rather than refusing against stale lengths), and
 * a crash mid-clear can leave some trees emptied and others not — re-run
 * clear to finish. The final journal record restores normal atomicity for
 * everything that follows.
 */
int tix_clear(bpt *index, bpt *doc_terms, bpt *doc_lengths, const bj_io *journal) {
    if (journal && journal->truncate) {
        int32_t te = journal->truncate(journal->ctx, 0);
        if (te) return (int)te;
    }
    int e = bpt_reset(index);
    if (!e) e = bpt_reset(doc_terms);
    if (!e) e = bpt_reset(doc_lengths);
    if (!e && journal) e = tixj_commit(journal, index, doc_terms, doc_lengths);
    return e;
}

/* ---- query helpers -------------------------------------------------- */

/*
 * A document's length, fetched on first use and memoized in `lenmap`
 * (missing or zero -> 1, matching the reference). Queries touch only the
 * lengths of documents that actually appear in a matched posting — the old
 * code loaded the whole documentLengths tree (O(corpus)) per query.
 */
static int doc_length(bpt *doc_lengths, dict *lenmap,
                      const char *id, int idl, double *out) {
    int i = dict_find(lenmap, id, idl);
    if (i >= 0) { *out = lenmap->vals[i]; return BJ_OK; }
    bpt_key kd = str_key(id, idl);
    int found; const uint8_t *vp; size_t vl;
    int e = bpt_search(doc_lengths, &kd, &found, &vp, &vl);
    if (e) return e;
    double v = 1;
    if (found) {
        cur c = { vp, vl, 0 };
        if ((e = read_number(&c, &v))) return e;
        if (v == 0) v = 1;
    }
    if ((e = dict_set(lenmap, id, idl, v))) return e;
    *out = v;
    return BJ_OK;
}

/* Stable sort indices [0..n) by descending score (insertion order preserved
 * for equal scores, matching JS's stable Array.prototype.sort). */
static void stable_sort_desc(int *idx, const double *score, int n) {
    for (int i = 1; i < n; i++) {
        int cur_i = idx[i];
        double s = score[cur_i];
        int j = i - 1;
        while (j >= 0 && score[idx[j]] < s) { idx[j + 1] = idx[j]; j--; }
        idx[j + 1] = cur_i;
    }
}

/* Emit an empty binjson ARRAY. */
static int emit_empty_array(uint8_t **out, size_t *out_len) {
    bj_builder *b = bj_builder_new();
    if (!b) return BJ_ERR_OOM;
    bj_begin_array(b); bj_end_array(b);
    int e = bj_builder_error(b);
    size_t dlen; const uint8_t *d = e ? NULL : bj_builder_data(b, &dlen);
    if (!d) { bj_builder_free(b); return e ? e : BJ_ERR_STATE; }
    uint8_t *cp = malloc(dlen);
    if (!cp) { bj_builder_free(b); return BJ_ERR_OOM; }
    memcpy(cp, d, dlen);
    *out = cp; *out_len = dlen;
    bj_builder_free(b);
    return BJ_OK;
}

/* ---- query (relevance) ---------------------------------------------- */

int tix_query(bpt *index, bpt *doc_terms, bpt *doc_lengths,
              const char *query, int query_len,
              uint8_t **out, size_t *out_len) {
    (void)doc_terms;   /* kept in the API; coverage no longer re-reads it */
    dict tf; dict_init(&tf); /* unique query terms (insertion order) */
    int e = tokenize_count(query, query_len, &tf);
    if (e) { dict_free(&tf); return e; }
    if (tf.n == 0) { dict_free(&tf); return emit_empty_array(out, out_len); }

    /* The doc count is the documentLengths entry count — an O(1) accessor,
     * not a tree scan. */
    double total = (double)bpt_size(doc_lengths);

    dict lenmap; dict_init(&lenmap);   /* lazily fetched doc lengths */
    dict scores; dict_init(&scores);
    dict matches; dict_init(&matches); /* docId -> # query terms matched */

    /* idf + tf*idf accumulation, decoding each term's postings once. */
    for (int t = 0; t < tf.n && !e; t++) {
        dict post; dict_init(&post);
        int nb;
        e = load_postings(index, tf.keys[t], tf.klen[t], &post, &nb);
        double idf = (!e && post.n > 0) ? log(total / (double)post.n) : 0;
        for (int j = 0; j < post.n && !e; j++) {
            double dl;
            if ((e = doc_length(doc_lengths, &lenmap, post.keys[j], post.klen[j], &dl)))
                break;
            double tfv = post.vals[j] / dl;
            int si = dict_find(&scores, post.keys[j], post.klen[j]);
            double prev = si >= 0 ? scores.vals[si] : 0;
            if ((e = dict_set(&scores, post.keys[j], post.klen[j], prev + tfv * idf)))
                break;
            /* Each doc appears at most once per merged posting, so this
             * counts distinct matched query terms per doc. */
            e = dict_inc(&matches, post.keys[j], post.klen[j]);
        }
        dict_free(&post);
    }

    /*
     * Coverage boost from the counts gathered above. The reference probed
     * each scored doc's full term map instead; since a doc's term map and
     * its posting appearances always agree (both the C replace semantics
     * and the JS merge update the two sides together), the results are
     * identical — without re-reading a term map per scored document.
     */
    for (int s = 0; s < scores.n && !e; s++) {
        int mi = dict_find(&matches, scores.keys[s], scores.klen[s]);
        double matching = mi >= 0 ? matches.vals[mi] : 0;
        scores.vals[s] *= (1.0 + matching / (double)tf.n);
    }

    /* sort + emit ARRAY of { id, score } */
    if (!e) {
        int *idx = malloc((size_t)(scores.n ? scores.n : 1) * sizeof(int));
        bj_builder *b = bj_builder_new();
        if (!idx || !b) { e = BJ_ERR_OOM; free(idx); if (b) bj_builder_free(b); }
        else {
            for (int i = 0; i < scores.n; i++) idx[i] = i;
            stable_sort_desc(idx, scores.vals, scores.n);
            bj_begin_array(b);
            for (int i = 0; i < scores.n; i++) {
                int r = idx[i];
                bj_begin_object(b);
                bj_put_key(b, (const uint8_t *)"id", 2);
                bj_put_string(b, (const uint8_t *)scores.keys[r], (uint32_t)scores.klen[r]);
                bj_put_key(b, (const uint8_t *)"score", 5);
                bj_put_float(b, scores.vals[r]);
                bj_end_object(b);
            }
            bj_end_array(b);
            e = bj_builder_error(b);
            if (!e) {
                size_t dlen; const uint8_t *d = bj_builder_data(b, &dlen);
                if (!d) e = BJ_ERR_STATE;
                else {
                    uint8_t *cp = malloc(dlen ? dlen : 1);
                    if (!cp) e = BJ_ERR_OOM;
                    else { memcpy(cp, d, dlen); *out = cp; *out_len = dlen; }
                }
            }
            free(idx);
            bj_builder_free(b);
        }
    }

    dict_free(&scores);
    dict_free(&matches);
    dict_free(&lenmap);
    dict_free(&tf);
    return e;
}

/* ---- term count ------------------------------------------------------ */

/*
 * Count distinct terms: one key per term identifies it — the block header
 * (ends with the '\0' suffix) or a legacy blob key (contains no '\0' at
 * all). Block keys ('\0' followed by hex digits) are skipped.
 */
int tix_term_count(bpt *index, int64_t *out) {
    bpt_cursor *c = bpt_cursor_open(index, NULL, NULL);
    if (!c) return BJ_ERR_OOM;
    int64_t n = 0;
    int r;
    bpt_key k; const uint8_t *v; size_t vl;
    while ((r = bpt_cursor_next(c, &k, &v, &vl)) == 1) {
        if (!k.is_string) continue;
        if (k.str_len && k.str[k.str_len - 1] == 0) n++;                 /* header */
        else if (!memchr(k.str, 0, k.str_len)) n++;                      /* legacy */
    }
    bpt_cursor_close(c);
    if (r < 0) return r;
    *out = n;
    return BJ_OK;
}

/* ---- query (requireAll) --------------------------------------------- */

int tix_query_all(bpt *index, bpt *doc_terms, bpt *doc_lengths,
                  const char *query, int query_len,
                  uint8_t **out, size_t *out_len) {
    (void)doc_terms; (void)doc_lengths;
    dict tf; dict_init(&tf);
    int e = tokenize_count(query, query_len, &tf);
    if (e) { dict_free(&tf); return e; }
    if (tf.n == 0) { dict_free(&tf); return emit_empty_array(out, out_len); }

    /* Candidate set = docIds of the first term's postings (in order). */
    dict cand; dict_init(&cand);
    {
        int nb;
        e = load_postings(index, tf.keys[0], tf.klen[0], &cand, &nb);
    }

    /* Intersect with each remaining term. */
    for (int t = 1; t < tf.n && !e && cand.n > 0; t++) {
        dict setT; dict_init(&setT);
        int nb;
        e = load_postings(index, tf.keys[t], tf.klen[t], &setT, &nb);
        int w = 0;
        for (int i = 0; i < cand.n; i++) {
            if (dict_find(&setT, cand.keys[i], cand.klen[i]) < 0) {
                free(cand.keys[i]);
            } else {
                cand.keys[w] = cand.keys[i]; cand.klen[w] = cand.klen[i]; cand.vals[w] = cand.vals[i]; w++;
            }
        }
        cand.n = w;
        dict_reindex(&cand); /* cannot fail: entry count only shrank */
        dict_free(&setT);
    }

    if (!e) {
        bj_builder *b = bj_builder_new();
        if (!b) e = BJ_ERR_OOM;
        else {
            bj_begin_array(b);
            for (int i = 0; i < cand.n; i++)
                bj_put_string(b, (const uint8_t *)cand.keys[i], (uint32_t)cand.klen[i]);
            bj_end_array(b);
            e = bj_builder_error(b);
            if (!e) {
                size_t dlen; const uint8_t *d = bj_builder_data(b, &dlen);
                if (!d) e = BJ_ERR_STATE;
                else { uint8_t *cp = malloc(dlen ? dlen : 1); if (!cp) e = BJ_ERR_OOM; else { memcpy(cp, d, dlen); *out = cp; *out_len = dlen; } }
            }
            bj_builder_free(b);
        }
    }

    dict_free(&cand);
    dict_free(&tf);
    return e;
}
