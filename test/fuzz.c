/*
 * fuzz.c — hostile-file fuzz harness for the C data structures (not part of
 * the WASM build; see test/fuzz.sh).
 *
 * Builds a small valid file for each structure through its public API over a
 * memory-backed bj_io, then repeatedly mutates a copy (bit flips, byte
 * writes, splices, truncations) and runs the full open + operate + compact
 * surface over the corrupted image. Every return code is ignored — the
 * structures are free to fail — but they must not crash, hang, or trip
 * ASan/UBSan. A SIGALRM watchdog converts hangs into failures.
 *
 * Usage:            ./bjfuzz [iterations] [seed]        (default 20000, 1)
 * Reproduce a case: ./bjfuzz 1 <seed-printed-on-failure>
 * libFuzzer build:  cc -DBJFUZZ_LIBFUZZER -fsanitize=fuzzer,address ...
 *   (input = 1 selector byte + image bytes)
 */
#include "binjson.h"
#include "bjio.h"
#include "dbuf.h"
#include "bplustree.h"
#include "rtree.h"
#include "textlog.h"
#include "textindex.h"
#include "entrylog.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>

/* ---- Memory-backed bj_io over a dbuf --------------------------------- */

static uint64_t mem_size(void *ctx) { return ((dbuf *)ctx)->len; }
static int64_t mem_read(void *ctx, uint64_t off, uint8_t *buf, uint32_t len) {
    dbuf *d = (dbuf *)ctx;
    if (off >= d->len) return 0;
    uint64_t n = d->len - off;
    if (n > len) n = len;
    memcpy(buf, d->data + off, (size_t)n);
    return (int64_t)n;
}
static int32_t mem_write(void *ctx, uint64_t off, const uint8_t *buf, uint32_t len) {
    dbuf *d = (dbuf *)ctx;
    if (off > d->len) return -1;   /* appends only, no holes */
    if (off + len > d->len) {
        if (dbuf_ensure(d, (size_t)(off + len) - d->len)) return -1;
        d->len = (size_t)(off + len);
    }
    if (len) memcpy(d->data + off, buf, len);
    return 0;
}
static int32_t mem_trunc(void *ctx, uint64_t len) {
    dbuf *d = (dbuf *)ctx;
    if (len > d->len) return -1;
    d->len = (size_t)len;
    return 0;
}
static bj_io mem_io(dbuf *d) {
    bj_io io = { d, mem_size, mem_read, mem_write, mem_trunc };
    return io;
}

/* ---- PRNG (xorshift64*) ---------------------------------------------- */

static uint64_t rng_state;
static uint64_t rnd(void) {
    uint64_t x = rng_state;
    x ^= x >> 12; x ^= x << 25; x ^= x >> 27;
    rng_state = x;
    return x * 0x2545F4914F6CDD1DULL;
}

/* ---- Seed images ------------------------------------------------------ */

static bpt_key num_key(double v) {
    bpt_key k; k.is_string = 0; k.num = v; k.str = NULL; k.str_len = 0;
    return k;
}

static int seed_bpt(dbuf *img) {
    bj_io io = mem_io(img);
    bpt *t = bpt_create(&io, 4);
    if (!t) return -1;
    bj_builder *b = bj_builder_new();
    if (!b) { bpt_free(t); return -1; }
    uint8_t big[400];
    memset(big, 'v', sizeof(big));
    for (int i = 0; i < 40; i++) {
        bpt_key k = num_key(i);
        int e;
        /* Stage an applied index partway so the image holds commits with
         * both metadata sizes (legacy and appliedIndex-extended). */
        if (i == 20) bpt_set_applied_index(t, 1234);
        if (i % 7 == 0) {
            /* Over BPT_OOL_THRESHOLD: exercises out-of-line value storage
             * (and, once mutated, corrupt OOL markers/targets). */
            e = bpt_add(t, &k, big, (uint32_t)sizeof(big));
        } else {
            bj_builder_reset(b);
            bj_put_int(b, i * 10);
            size_t n; const uint8_t *d = bj_builder_data(b, &n);
            e = (!d) ? BJ_ERR_STATE : bpt_add(t, &k, d, (uint32_t)n);
        }
        if (e) break;
    }
    bj_builder_free(b);
    bpt_free(t);
    return 0;
}

static int seed_rtree(dbuf *img) {
    bj_io io = mem_io(img);
    rtree *t = rtree_create(&io, 4);
    if (!t) return -1;
    for (int i = 0; i < 40; i++) {
        uint8_t oid[12];
        memset(oid, i, sizeof(oid));
        if (i == 20) rtree_set_applied_index(t, 77);   /* mixed metadata sizes */
        if (rtree_insert(t, (i * 37) % 170 - 85.0, (i * 73) % 350 - 175.0, oid)) break;
    }
    rtree_free(t);
    return 0;
}

static int seed_textlog(dbuf *img) {
    bj_io io = mem_io(img);
    textlog *t = textlog_create(&io, 3);
    if (!t) return -1;
    char text[128];
    for (int i = 0; i < 8; i++) {
        if (i == 4) textlog_set_applied_index(t, 55);   /* mixed metadata sizes */
        int n = snprintf(text, sizeof(text),
                         "line one v%d\nline two\nline %d\n", i, i * i);
        uint64_t v;
        if (textlog_add_version(t, (const uint8_t *)text, (uint32_t)n,
                                1700000000000LL + i, &v)) break;
    }
    textlog_free(t);
    return 0;
}

static int seed_entrylog(dbuf *img) {
    bj_io io = mem_io(img);
    elog *t = elog_create(&io);
    if (!t) return -1;
    /* A realistic history: entries across rising terms, a commit-index
     * update, a truncation, and re-appended replacements — so mutated
     * images hit the metadata, entry, and truncation-replay paths. */
    elog_set_hard_state(t, 2, 7);
    char text[64];
    for (int i = 0; i < 10; i++) {
        int n = snprintf(text, sizeof(text), "command %d payload\n", i);
        uint64_t idx;
        if (elog_append(t, i < 5 ? 1 : 2, EL_NORMAL,
                        (const uint8_t *)text, (uint32_t)n, &idx)) break;
    }
    elog_set_commit_index(t, 4);
    elog_sync(t);
    elog_truncate_from(t, 8);
    elog_append(t, 2, EL_CONFIG, (const uint8_t *)"cfg", 3, NULL);
    elog_sync(t);
    elog_free(t);
    return 0;
}

/* ---- Exercise a (possibly corrupt) image ----------------------------- */

static void ex_bpt(dbuf *img) {
    bj_io io = mem_io(img);
    bpt *t = bpt_open(&io);
    if (!t) return;
    int found; const uint8_t *p; size_t n; int h;
    bpt_key k = num_key((double)(rnd() % 64));
    bpt_search(t, &k, &found, &p, &n);
    bpt_entries(t, &p, &n);
    bpt_key lo = num_key(3), hi = num_key(33);
    bpt_range(t, &lo, &hi, &p, &n);
    bpt_height(t, &h);
    bpt_cursor *c = bpt_cursor_open(t, NULL, NULL);
    if (c) {
        bpt_key ck; const uint8_t *v; size_t vl;
        /* Corrupt DAGs can make traversals large; bound the walk. */
        for (int g = 0; g < 100000 && bpt_cursor_next(c, &ck, &v, &vl) == 1; g++) {}
        bpt_cursor_close(c);
    }
    bpt_set_applied_index(t, bpt_applied_index(t) + 1);
    uint8_t val[9] = { 0 };
    bj_builder *b = bj_builder_new();
    if (b) {
        bj_put_int(b, 7);
        size_t vn; const uint8_t *vd = bj_builder_data(b, &vn);
        if (vd && vn <= sizeof(val)) {
            bpt_add(t, &k, vd, (uint32_t)vn);
            bpt_delete(t, &k);
        }
        bj_builder_free(b);
    }
    /* A fresh out-of-line insert/search/delete against the (possibly
     * corruption-recovered) live tree, independent of the seeded big value. */
    {
        uint8_t big2[350];
        memset(big2, 'w', sizeof(big2));
        bpt_key bk = num_key(1000.0 + (double)(rnd() % 16));
        bpt_add(t, &bk, big2, (uint32_t)sizeof(big2));
        bpt_search(t, &bk, &found, &p, &n);
        bpt_delete(t, &bk);
    }
    /* Churn: a run of deletes drives leaf/internal underflow and the
     * merge/redistribute rebalancing, including root collapses. */
    for (int g = 0; g < 24; g++) {
        bpt_key dk = num_key((double)(rnd() % 48));
        bpt_delete(t, &dk);
    }
    /* The full-tree invariant walk must never crash or hang, whatever the
     * mutations did (errors are the expected outcome on corrupt images). */
    bpt_verify(t);
    const uint8_t *bp; size_t bl;
    if (bpt_boundaries(t, &bp, &bl) == 0) {
        bpt *snap = bpt_snapshot(t);
        if (snap) {
            bpt_entries(snap, &p, &n);
            bpt_free(snap);
        }
    }
    bpt *at = bpt_open_at(&io, rnd() % ((uint64_t)img->len + 1));
    if (at) { bpt_search(at, &k, &found, &p, &n); bpt_free(at); }
    dbuf dst; memset(&dst, 0, sizeof(dst));
    bj_io dio = mem_io(&dst);
    bpt_compact(t, &dio);
    dbuf_free(&dst);
    bpt_free(t);
}

static void ex_rtree(dbuf *img) {
    bj_io io = mem_io(img);
    rtree *t = rtree_open(&io);
    if (!t) return;
    const uint8_t *p; size_t n;
    rtree_search_bbox(t, -90, 90, -180, 180, &p, &n);
    rtree_search_radius(t, 10, 10, 500, &p, &n);
    rtree_cursor *c = rtree_cursor_open(t, -90, 90, -180, 180);
    if (c) {
        double la, ln;
        uint8_t eo[12];
        for (int g = 0; g < 100000 && rtree_cursor_next(c, &la, &ln, eo) == 1; g++) {}
        rtree_cursor_close(c);
    }
    rtree_nearest(t, 10, 20, 8, &p, &n);
    uint8_t oid[12];
    memset(oid, (int)(rnd() % 64), sizeof(oid));
    rtree_insert(t, 1.5, 2.5, oid);
    int removed;
    rtree_remove(t, oid, &removed);
    rtree_remove_at(t, 1.5, 2.5, oid, &removed);
    dbuf dst; memset(&dst, 0, sizeof(dst));
    bj_io dio = mem_io(&dst);
    rtree_compact(t, &dio);
    dbuf_free(&dst);
    rtree_free(t);
}

static void ex_textlog(dbuf *img) {
    bj_io io = mem_io(img);
    textlog *t = textlog_open(&io);
    if (!t) return;
    const uint8_t *p; size_t n;
    uint64_t v = textlog_version(t);
    if (v > 16) v = 16;   /* bound reconstruction work on hostile metadata */
    for (uint64_t i = 1; i <= v; i++) {
        textlog_get_version(t, i, &p, &n);
        textlog_get_version_hash(t, i, &p, &n);
    }
    if (v >= 2) textlog_get_diff(t, 1, v, &p, &n);
    textlog_add_version(t, (const uint8_t *)"mutated\n", 8, 1700000000999LL, &v);
    textlog_free(t);
}

static void ex_entrylog(dbuf *img) {
    bj_io io = mem_io(img);
    elog *t = elog_open(&io);
    if (!t) return;
    const uint8_t *p; size_t n;
    uint64_t base = elog_base_index(t), last = elog_last_index(t);
    if (last - base > 32) last = base + 32;   /* bound hostile metadata */
    uint64_t term; int type, count;
    for (uint64_t i = base + 1; i <= last; i++) {
        elog_get(t, i, &term, &type, &p, &n);
        elog_term_at(t, i, &term);
    }
    elog_get_batch(t, base + 1, 4096, &count, &p, &n);
    elog_verify(t);
    elog_set_commit_index(t, base + rnd() % (last - base + 1));
    elog_truncate_from(t, base + 1 + rnd() % (last - base + 1));
    elog_set_hard_state(t, elog_current_term(t) + 1, 3);
    elog_append(t, elog_current_term(t), EL_NOOP,
                (const uint8_t *)"mutated", 7, NULL);
    elog_sync(t);
    dbuf dst; memset(&dst, 0, sizeof(dst));
    bj_io dio = mem_io(&dst);
    elog_compact(t, &dio, elog_base_index(t), elog_base_term(t));
    dbuf_free(&dst);
    elog_free(t);
}

/* The text index runs its own decoders (postings, term maps, lengths) over
 * blobs stored in B+ trees; pointing all three roles at one fuzzed tree
 * exercises them against arbitrary bytes. */
static void ex_textindex(dbuf *img) {
    bj_io io = mem_io(img);
    bpt *t = bpt_open(&io);
    if (!t) return;
    uint8_t *out = NULL; size_t out_len = 0;
    if (tix_query(t, t, t, "hello world seven", 17, &out, &out_len) == 0) free(out);
    out = NULL;
    if (tix_query_all(t, t, t, "hello world", 11, &out, &out_len) == 0) free(out);
    tix_add(t, t, t, NULL, "doc-1", 5, "some text to index", 18);
    int removed;
    tix_remove(t, t, t, NULL, "doc-1", 5, &removed);
    bpt_free(t);
}

/* ---- Mutation engine --------------------------------------------------- */

static void mutate(dbuf *d) {
    if (d->len == 0) return;
    int nmut = 1 + (int)(rnd() % 8);
    for (int i = 0; i < nmut; i++) {
        size_t at = (size_t)(rnd() % d->len);
        switch (rnd() % 4) {
        case 0: d->data[at] ^= (uint8_t)(1u << (rnd() % 8)); break;   /* bit flip */
        case 1: d->data[at] = (uint8_t)rnd(); break;                  /* byte set */
        case 2:                                                       /* splice */
            if (d->len > 8) {
                size_t from = (size_t)(rnd() % (d->len - 8));
                size_t len = 1 + (size_t)(rnd() % 8);
                if (at + len > d->len) len = d->len - at;
                memmove(d->data + at, d->data + from, len);
            }
            break;
        case 3: d->len = at + 1; return;                              /* truncate */
        }
    }
}

static void run_one(int which, dbuf *img) {
    switch (which) {
    case 0: ex_bpt(img); break;
    case 1: ex_rtree(img); break;
    case 2: ex_textlog(img); break;
    case 3: ex_entrylog(img); break;
    default: ex_textindex(img); break;
    }
}

/* Seed image for a structure selector (textindex fuzzes a bpt image). */
static int seed_for(int which) {
    return which == 4 ? 0 : which;
}

#ifdef BJFUZZ_LIBFUZZER

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    if (size < 2) return 0;
    dbuf img; memset(&img, 0, sizeof(img));
    if (dbuf_put(&img, data + 1, size - 1)) return 0;
    run_one(data[0] % 5, &img);
    dbuf_free(&img);
    return 0;
}

#else /* self-contained mutational fuzzer */

static uint64_t cur_iter, cur_seed;
static int cur_which;

static void on_alarm(int sig) {
    (void)sig;
    fprintf(stderr, "\nHANG: structure %d, iteration %llu, seed %llu\n",
            cur_which, (unsigned long long)cur_iter, (unsigned long long)cur_seed);
    _exit(3);
}

int main(int argc, char **argv) {
    uint64_t iters = argc > 1 ? strtoull(argv[1], NULL, 10) : 20000;
    uint64_t seed0 = argc > 2 ? strtoull(argv[2], NULL, 10) : 1;

    dbuf seeds[4];
    memset(seeds, 0, sizeof(seeds));
    if (seed_bpt(&seeds[0]) || seed_rtree(&seeds[1]) || seed_textlog(&seeds[2]) ||
        seed_entrylog(&seeds[3])) {
        fprintf(stderr, "seed construction failed\n");
        return 2;
    }
    signal(SIGALRM, on_alarm);

    /* Sanity: every structure must handle its own unmutated seed. */
    for (int w = 0; w < 5; w++) {
        dbuf img; memset(&img, 0, sizeof(img));
        if (dbuf_put(&img, seeds[seed_for(w)].data, seeds[seed_for(w)].len)) return 2;
        alarm(20);
        run_one(w, &img);
        alarm(0);
        dbuf_free(&img);
    }

    for (uint64_t i = 0; i < iters; i++) {
        cur_iter = i;
        cur_seed = seed0 + i;
        rng_state = cur_seed * 0x9E3779B97F4A7C15ULL + 1;
        cur_which = (int)(rnd() % 5);
        const dbuf *src = &seeds[seed_for(cur_which)];

        dbuf img; memset(&img, 0, sizeof(img));
        if (dbuf_put(&img, src->data, src->len)) return 2;
        mutate(&img);
        alarm(20);
        run_one(cur_which, &img);
        alarm(0);
        dbuf_free(&img);

        if ((i + 1) % 10000 == 0)
            fprintf(stderr, "  %llu iterations\n", (unsigned long long)(i + 1));
    }

    for (int s = 0; s < 4; s++) dbuf_free(&seeds[s]);
    fprintf(stderr, "OK: %llu iterations, seeds %llu..%llu\n",
            (unsigned long long)iters, (unsigned long long)seed0,
            (unsigned long long)(seed0 + iters - 1));
    return 0;
}

#endif /* BJFUZZ_LIBFUZZER */
