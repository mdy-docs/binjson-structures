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

static int is_word_char(unsigned char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
           (c >= '0' && c <= '9') || c == '_';
}

/*
 * Tokenize `text`: lowercase, split on non-word runs, drop stop words, Porter-
 * stem each surviving token and count stem frequencies into `out` (insertion
 * order = first-seen order, matching the reference's Map/object semantics).
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
        for (int j = 0; j < wlen; j++) {
            char c = text[s + j];
            if (c >= 'A' && c <= 'Z') c = (char)(c + 32);
            low[j] = c;
        }
        if (is_stopword(low, wlen)) continue;
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

/* Encode a dict as a { string: int } object into a reusable builder; expose
 * bytes via data/dlen (valid until the builder is reset or freed). */
static int encode_obj(bj_builder *b, const dict *d, const uint8_t **data, size_t *dlen) {
    bj_builder_reset(b);
    bj_begin_object(b);
    for (int i = 0; i < d->n; i++) {
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

static bpt_key str_key(const char *s, int len) {
    bpt_key k; k.is_string = 1; k.num = 0; k.str = (const uint8_t *)s; k.str_len = (uint32_t)len; return k;
}

/* ---- add ------------------------------------------------------------ */

int tix_add(bpt *index, bpt *doc_terms, bpt *doc_lengths,
            const char *doc_id, int doc_id_len,
            const char *text, int text_len) {
    dict tf; dict_init(&tf);
    int e = tokenize_count(text, text_len, &tf);
    if (e) { dict_free(&tf); return e; }

    bj_builder *b = bj_builder_new();
    if (!b) { dict_free(&tf); return BJ_ERR_OOM; }

    /* Postings: for each term, merge in this doc's frequency. */
    for (int i = 0; i < tf.n && !e; i++) {
        bpt_key kt = str_key(tf.keys[i], tf.klen[i]);
        int found; const uint8_t *vp; size_t vl;
        if ((e = bpt_search(index, &kt, &found, &vp, &vl))) break;
        dict post; dict_init(&post);
        if (found) e = decode_obj(vp, vl, &post);
        if (!e) e = dict_set(&post, doc_id, doc_id_len, tf.vals[i]);
        if (!e) {
            const uint8_t *data; size_t dlen;
            if (!(e = encode_obj(b, &post, &data, &dlen)))
                e = bpt_add(index, &kt, data, (uint32_t)dlen);
        }
        dict_free(&post);
    }

    /* documentTerms: merge new terms over existing. */
    if (!e) {
        bpt_key kd = str_key(doc_id, doc_id_len);
        int found; const uint8_t *vp; size_t vl;
        e = bpt_search(doc_terms, &kd, &found, &vp, &vl);
        dict merged; dict_init(&merged);
        if (!e && found) e = decode_obj(vp, vl, &merged);
        for (int i = 0; i < tf.n && !e; i++)
            e = dict_set(&merged, tf.keys[i], tf.klen[i], tf.vals[i]);
        double doc_len = 0;
        for (int i = 0; i < merged.n; i++) doc_len += merged.vals[i];
        if (!e) {
            const uint8_t *data; size_t dlen;
            if (!(e = encode_obj(b, &merged, &data, &dlen)))
                e = bpt_add(doc_terms, &kd, data, (uint32_t)dlen);
        }
        /* documentLengths: total term count as an int value. */
        if (!e) {
            bj_builder_reset(b);
            bj_put_int(b, (int64_t)doc_len);
            e = bj_builder_error(b);
            if (!e) {
                size_t dlen; const uint8_t *data = bj_builder_data(b, &dlen);
                if (!data) e = BJ_ERR_STATE;
                else e = bpt_add(doc_lengths, &kd, data, (uint32_t)dlen);
            }
        }
        dict_free(&merged);
    }

    bj_builder_free(b);
    dict_free(&tf);
    return e;
}

/* ---- remove --------------------------------------------------------- */

int tix_remove(bpt *index, bpt *doc_terms, bpt *doc_lengths,
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

    for (int i = 0; i < terms.n && !e; i++) {
        bpt_key kt = str_key(terms.keys[i], terms.klen[i]);
        int f2; const uint8_t *vp2; size_t vl2;
        if ((e = bpt_search(index, &kt, &f2, &vp2, &vl2))) break;
        dict post; dict_init(&post);
        if (f2) e = decode_obj(vp2, vl2, &post);
        if (!e) {
            dict_remove(&post, doc_id, doc_id_len);
            if (post.n == 0) {
                e = bpt_delete(index, &kt);
            } else {
                const uint8_t *data; size_t dlen;
                if (!(e = encode_obj(b, &post, &data, &dlen)))
                    e = bpt_add(index, &kt, data, (uint32_t)dlen);
            }
        }
        dict_free(&post);
    }

    if (!e) e = bpt_delete(doc_terms, &kd);
    if (!e) e = bpt_delete(doc_lengths, &kd);
    if (!e) *removed = 1;

    bj_builder_free(b);
    dict_free(&terms);
    return e;
}

/* ---- clear ---------------------------------------------------------- */

/* Collect every key of `t` (as owned copies), then delete each. */
static int clear_tree(bpt *t) {
    const uint8_t *ap; size_t al;
    int e = bpt_entries(t, &ap, &al);
    if (e) return e;
    dict keys; dict_init(&keys); /* value unused */
    cur c = { ap, al, 0 };
    uint32_t count;
    if ((e = array_begin(&c, &count))) { dict_free(&keys); return e; }
    for (uint32_t i = 0; i < count && !e; i++) {
        uint32_t fields;
        if ((e = object_begin(&c, &fields))) break;
        for (uint32_t f = 0; f < fields && !e; f++) {
            const uint8_t *kn; uint32_t klen;
            if ((e = take_key(&c, &kn, &klen))) break;
            if (name_eq(kn, klen, "key")) {
                const uint8_t *sp; uint32_t sl;
                if ((e = take_string(&c, &sp, &sl))) break;
                e = dict_set(&keys, (const char *)sp, (int)sl, 0);
            } else {
                e = skip_value(&c);
            }
        }
    }
    for (int i = 0; i < keys.n && !e; i++) {
        bpt_key k = str_key(keys.keys[i], keys.klen[i]);
        e = bpt_delete(t, &k);
    }
    dict_free(&keys);
    return e;
}

int tix_clear(bpt *index, bpt *doc_terms, bpt *doc_lengths) {
    int e = clear_tree(index);
    if (!e) e = clear_tree(doc_terms);
    if (!e) e = clear_tree(doc_lengths);
    return e;
}

/* ---- query helpers -------------------------------------------------- */

/* Parse documentLengths entries into `lenmap` (docId -> max(length,1)); returns
 * the document count through *total. */
static int load_lengths(bpt *doc_lengths, dict *lenmap, int *total) {
    const uint8_t *ap; size_t al;
    int e = bpt_entries(doc_lengths, &ap, &al);
    if (e) return e;
    cur c = { ap, al, 0 };
    uint32_t count;
    if ((e = array_begin(&c, &count))) return e;
    *total = (int)count;
    for (uint32_t i = 0; i < count && !e; i++) {
        uint32_t fields;
        if ((e = object_begin(&c, &fields))) break;
        const uint8_t *idp = NULL; uint32_t idl = 0; double val = 1;
        for (uint32_t f = 0; f < fields && !e; f++) {
            const uint8_t *kn; uint32_t klen;
            if ((e = take_key(&c, &kn, &klen))) break;
            if (name_eq(kn, klen, "key"))        e = take_string(&c, &idp, &idl);
            else if (name_eq(kn, klen, "value")) e = read_number(&c, &val);
            else                                 e = skip_value(&c);
        }
        if (!e) e = dict_set(lenmap, (const char *)idp, (int)idl, val == 0 ? 1 : val);
    }
    return e;
}

/* Look up a docId's length (default 1). */
static double length_of(const dict *lenmap, const char *id, int idl) {
    int i = dict_find(lenmap, id, idl);
    return i >= 0 ? lenmap->vals[i] : 1;
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
    dict tf; dict_init(&tf); /* unique query terms (insertion order) */
    int e = tokenize_count(query, query_len, &tf);
    if (e) { dict_free(&tf); return e; }
    if (tf.n == 0) { dict_free(&tf); return emit_empty_array(out, out_len); }

    dict lenmap; dict_init(&lenmap);
    int total = 0;
    e = load_lengths(doc_lengths, &lenmap, &total);

    double *idf = calloc((size_t)tf.n, sizeof(double));
    dict scores; dict_init(&scores);
    if (!idf) e = BJ_ERR_OOM;

    /* idf per term */
    for (int t = 0; t < tf.n && !e; t++) {
        bpt_key kt = str_key(tf.keys[t], tf.klen[t]);
        int found; const uint8_t *vp; size_t vl;
        if ((e = bpt_search(index, &kt, &found, &vp, &vl))) break;
        if (found) {
            dict post; dict_init(&post);
            e = decode_obj(vp, vl, &post);
            if (!e && post.n > 0) idf[t] = log((double)total / (double)post.n);
            dict_free(&post);
        }
    }

    /* accumulate tf*idf per doc */
    for (int t = 0; t < tf.n && !e; t++) {
        bpt_key kt = str_key(tf.keys[t], tf.klen[t]);
        int found; const uint8_t *vp; size_t vl;
        if ((e = bpt_search(index, &kt, &found, &vp, &vl))) break;
        if (!found) continue;
        dict post; dict_init(&post);
        e = decode_obj(vp, vl, &post);
        for (int j = 0; j < post.n && !e; j++) {
            double dl = length_of(&lenmap, post.keys[j], post.klen[j]);
            double tfv = post.vals[j] / dl;
            int si = dict_find(&scores, post.keys[j], post.klen[j]);
            double prev = si >= 0 ? scores.vals[si] : 0;
            e = dict_set(&scores, post.keys[j], post.klen[j], prev + tfv * idf[t]);
        }
        dict_free(&post);
    }

    /* coverage boost */
    for (int s = 0; s < scores.n && !e; s++) {
        bpt_key kd = str_key(scores.keys[s], scores.klen[s]);
        int found; const uint8_t *vp; size_t vl;
        if ((e = bpt_search(doc_terms, &kd, &found, &vp, &vl))) break;
        dict dterms; dict_init(&dterms);
        if (found) e = decode_obj(vp, vl, &dterms);
        int matching = 0;
        for (int t = 0; t < tf.n; t++)
            if (dict_find(&dterms, tf.keys[t], tf.klen[t]) >= 0) matching++;
        double coverage = (double)matching / (double)tf.n;
        scores.vals[s] *= (1.0 + coverage);
        dict_free(&dterms);
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

    free(idf);
    dict_free(&scores);
    dict_free(&lenmap);
    dict_free(&tf);
    return e;
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
        bpt_key k0 = str_key(tf.keys[0], tf.klen[0]);
        int found; const uint8_t *vp; size_t vl;
        e = bpt_search(index, &k0, &found, &vp, &vl);
        if (!e && found) e = decode_obj(vp, vl, &cand);
    }

    /* Intersect with each remaining term. */
    for (int t = 1; t < tf.n && !e && cand.n > 0; t++) {
        bpt_key kt = str_key(tf.keys[t], tf.klen[t]);
        int found; const uint8_t *vp; size_t vl;
        if ((e = bpt_search(index, &kt, &found, &vp, &vl))) break;
        dict setT; dict_init(&setT);
        if (found) e = decode_obj(vp, vl, &setT);
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
