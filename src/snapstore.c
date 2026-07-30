/*
 * snapstore.c — see snapstore.h for the protocol and the reason the
 * decisions live here while the file operations stay in the host.
 */
#include "snapstore.h"
#include "bjfile.h"
#include "bjcursor.h"

#include <stdlib.h>
#include <string.h>

/* Manifest record version. Bumping it makes every older manifest fail to
 * adopt, which the protocol already handles: a generation that does not
 * adopt is swept and the next snapshot writes a fresh one. That is why
 * this needs no migration story of its own -- snapshots are derived
 * state, regenerable from the log. */
#define SST_MANIFEST_VERSION 1

/* ---- generation bookkeeping ------------------------------------------- */

typedef struct {
    char *name;          /* the data file's name, owned */
    char *role;          /* its role, owned */
} sst_file;

typedef struct {
    uint64_t  gen;
    char     *manifest;  /* the manifest file's name, or NULL */
    sst_file *files;
    uint32_t  n, cap;
} sst_gen;

struct sst {
    char     *prefix;
    uint32_t  prefix_len;

    sst_gen  *gens;
    uint32_t  n_gens, cap_gens;

    uint32_t *candidates;      /* indices into gens, newest first */
    uint32_t  n_candidates;

    /* Between sst_try_manifest and sst_confirm: the candidate under
     * consideration, its record, and the files whose sizes decide it. */
    int       pending;
    uint32_t  pending_cand;
    dbuf      pending_manifest;
    char    **pending_names;
    uint64_t *pending_sizes;
    uint32_t  n_pending, cap_pending;

    uint64_t  next_gen;

    int       has_latest;
    uint64_t  latest_gen;
    dbuf      latest;          /* the adopted manifest record (no CRC tail) */
};

static void gen_clear(sst_gen *g) {
    for (uint32_t i = 0; i < g->n; i++) { free(g->files[i].name); free(g->files[i].role); }
    free(g->files);
    free(g->manifest);
    memset(g, 0, sizeof(*g));
}

static void pending_clear(sst *s) {
    for (uint32_t i = 0; i < s->n_pending; i++) free(s->pending_names[i]);
    free(s->pending_names);
    free(s->pending_sizes);
    s->pending_names = NULL;
    s->pending_sizes = NULL;
    s->n_pending = s->cap_pending = 0;
    s->pending = 0;
    s->pending_manifest.len = 0;
}

static void scan_clear(sst *s) {
    for (uint32_t i = 0; i < s->n_gens; i++) gen_clear(&s->gens[i]);
    free(s->gens);
    free(s->candidates);
    s->gens = NULL; s->n_gens = s->cap_gens = 0;
    s->candidates = NULL; s->n_candidates = 0;
    pending_clear(s);
}

sst *sst_new(const char *prefix, uint32_t prefix_len) {
    sst *s = (sst *)calloc(1, sizeof(sst));
    if (!s) return NULL;
    s->prefix = (char *)malloc(prefix_len + 1u);
    if (!s->prefix) { free(s); return NULL; }
    /* A prefix-less store is legal: sst_check_files needs no names, and
     * the standalone validator in structures-core.js creates one for
     * exactly that. memcpy from a NULL source is undefined even for zero
     * bytes, so guard rather than rely on it. */
    if (prefix_len) memcpy(s->prefix, prefix, prefix_len);
    s->prefix[prefix_len] = '\0';
    s->prefix_len = prefix_len;
    s->next_gen = 1;
    return s;
}

void sst_free(sst *s) {
    if (!s) return;
    scan_clear(s);
    dbuf_free(&s->pending_manifest);
    dbuf_free(&s->latest);
    free(s->prefix);
    free(s);
}

/* ---- names ------------------------------------------------------------ */

static int role_is_wellformed(const char *role, uint32_t len) {
    if (len == 0) return 0;
    for (uint32_t i = 0; i < len; i++) {
        char c = role[i];
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
            (c >= '0' && c <= '9') || c == '_' || c == '-') continue;
        return 0;
    }
    return 1;
}

/* Decimal, no leading zeros beyond "0" itself -- so one generation has
 * exactly one spelling and "snap-01-x.bj" is somebody else's file. */
static int put_u64(dbuf *out, uint64_t v) {
    char tmp[21];
    int n = 0;
    do { tmp[n++] = (char)('0' + (v % 10)); v /= 10; } while (v);
    char rev[21];
    for (int i = 0; i < n; i++) rev[i] = tmp[n - 1 - i];
    return dbuf_put(out, (const uint8_t *)rev, (size_t)n);
}

static int put_cstr(dbuf *out, const char *s) {
    return dbuf_put(out, (const uint8_t *)s, strlen(s));
}

int sst_manifest_name(const sst *s, uint64_t gen, dbuf *out) {
    int e = dbuf_put(out, (const uint8_t *)s->prefix, s->prefix_len);
    if (!e) e = put_cstr(out, "-");
    if (!e) e = put_u64(out, gen);
    if (!e) e = put_cstr(out, ".manifest.bj");
    return e;
}

int sst_data_name(const sst *s, uint64_t gen, const char *role, uint32_t role_len, dbuf *out) {
    if (!role_is_wellformed(role, role_len)) return SST_ERR_ROLE;
    int e = dbuf_put(out, (const uint8_t *)s->prefix, s->prefix_len);
    if (!e) e = put_cstr(out, "-");
    if (!e) e = put_u64(out, gen);
    if (!e) e = put_cstr(out, "-");
    if (!e) e = dbuf_put(out, (const uint8_t *)role, role_len);
    if (!e) e = put_cstr(out, ".bj");
    return e;
}

int sst_log_name(const sst *s, uint64_t gen, dbuf *out) {
    int e = dbuf_put(out, (const uint8_t *)s->prefix, s->prefix_len);
    if (!e) e = put_cstr(out, "-log-");
    if (!e) e = put_u64(out, gen);
    if (!e) e = put_cstr(out, ".bj");
    return e;
}

/* ---- name parsing ------------------------------------------------------ */

/*
 * The inverse of the three name builders. Written by hand rather than
 * with the regex engine, exactly as db_names.c's sweep matcher is: the
 * grammar is three fixed shapes, and a regex would be a dependency and a
 * cache and a second description of something already described above.
 */
typedef enum { SST_NAME_OTHER, SST_NAME_MANIFEST, SST_NAME_DATA, SST_NAME_LOG } sst_kind;

static int parse_u64(const char *p, uint32_t len, uint64_t *out) {
    if (len == 0 || len > 20) return 0;
    if (len > 1 && p[0] == '0') return 0;          /* one spelling per number */
    uint64_t v = 0;
    for (uint32_t i = 0; i < len; i++) {
        if (p[i] < '0' || p[i] > '9') return 0;
        if (v > (UINT64_MAX - (uint64_t)(p[i] - '0')) / 10u) return 0;
        v = v * 10u + (uint64_t)(p[i] - '0');
    }
    *out = v;
    return 1;
}

static int ends_with(const char *p, uint32_t len, const char *suffix, uint32_t *body_len) {
    uint32_t sl = (uint32_t)strlen(suffix);
    if (len < sl || memcmp(p + len - sl, suffix, sl) != 0) return 0;
    *body_len = len - sl;
    return 1;
}

static sst_kind classify(const sst *s, const char *name, uint32_t len,
                         uint64_t *gen, const char **role, uint32_t *role_len) {
    /* Every name this store owns begins "<prefix>-". */
    if (len <= s->prefix_len + 1u) return SST_NAME_OTHER;
    if (memcmp(name, s->prefix, s->prefix_len) != 0 || name[s->prefix_len] != '-')
        return SST_NAME_OTHER;
    const char *p = name + s->prefix_len + 1;
    uint32_t plen = len - s->prefix_len - 1;

    uint32_t body;
    if (ends_with(p, plen, ".manifest.bj", &body)) {
        if (parse_u64(p, body, gen)) return SST_NAME_MANIFEST;
        return SST_NAME_OTHER;
    }
    if (!ends_with(p, plen, ".bj", &body)) return SST_NAME_OTHER;

    /* "log-<gen>" or "<gen>-<role>". */
    if (body > 4 && memcmp(p, "log-", 4) == 0) {
        if (parse_u64(p + 4, body - 4, gen)) return SST_NAME_LOG;
        return SST_NAME_OTHER;
    }
    for (uint32_t i = 0; i < body; i++) {
        if (p[i] != '-') continue;
        if (!parse_u64(p, i, gen)) return SST_NAME_OTHER;
        *role = p + i + 1;
        *role_len = body - i - 1;
        if (!role_is_wellformed(*role, *role_len)) return SST_NAME_OTHER;
        return SST_NAME_DATA;
    }
    return SST_NAME_OTHER;
}

/* ---- scan -------------------------------------------------------------- */

static sst_gen *gen_at(sst *s, uint64_t gen) {
    for (uint32_t i = 0; i < s->n_gens; i++) if (s->gens[i].gen == gen) return &s->gens[i];
    if (s->n_gens == s->cap_gens) {
        uint32_t nc = s->cap_gens ? s->cap_gens * 2 : 4;
        sst_gen *ng = (sst_gen *)realloc(s->gens, (size_t)nc * sizeof(*ng));
        if (!ng) return NULL;
        s->gens = ng;
        s->cap_gens = nc;
    }
    sst_gen *g = &s->gens[s->n_gens++];
    memset(g, 0, sizeof(*g));
    g->gen = gen;
    return g;
}

static char *dup_n(const char *p, uint32_t n) {
    char *d = (char *)malloc(n + 1u);
    if (!d) return NULL;
    memcpy(d, p, n);
    d[n] = '\0';
    return d;
}

static int gen_add_file(sst_gen *g, const char *name, uint32_t name_len,
                        const char *role, uint32_t role_len) {
    if (g->n == g->cap) {
        uint32_t nc = g->cap ? g->cap * 2 : 8;
        sst_file *nf = (sst_file *)realloc(g->files, (size_t)nc * sizeof(*nf));
        if (!nf) return BJ_ERR_OOM;
        g->files = nf;
        g->cap = nc;
    }
    sst_file *f = &g->files[g->n];
    f->name = dup_n(name, name_len);
    f->role = dup_n(role, role_len);
    if (!f->name || !f->role) { free(f->name); free(f->role); return BJ_ERR_OOM; }
    g->n++;
    return BJ_OK;
}

int sst_scan(sst *s, const uint8_t *listing, uint32_t listing_len) {
    scan_clear(s);
    s->next_gen = 1;

    uint64_t max_gen = 0;
    uint32_t at = 0;
    while (at < listing_len) {
        uint32_t end = at;
        while (end < listing_len && listing[end] != '\0') end++;
        const char *name = (const char *)listing + at;
        uint32_t name_len = end - at;
        /* A trailing NUL yields a final empty name; skip it rather than
         * make the caller choose between separator and terminator. */
        if (name_len == 0) { at = end + 1; continue; }

        uint64_t gen = 0;
        const char *role = NULL; uint32_t role_len = 0;
        switch (classify(s, name, name_len, &gen, &role, &role_len)) {
            case SST_NAME_MANIFEST: {
                sst_gen *g = gen_at(s, gen);
                if (!g) return BJ_ERR_OOM;
                free(g->manifest);
                g->manifest = dup_n(name, name_len);
                if (!g->manifest) return BJ_ERR_OOM;
                if (gen > max_gen) max_gen = gen;
                break;
            }
            case SST_NAME_DATA: {
                sst_gen *g = gen_at(s, gen);
                if (!g) return BJ_ERR_OOM;
                int e = gen_add_file(g, name, name_len, role, role_len);
                if (e) return e;
                if (gen > max_gen) max_gen = gen;
                break;
            }
            case SST_NAME_LOG:
                /* Separate lifecycle (sst_log_candidates), but its
                 * generation still counts: a paired log proves a
                 * generation number was used, and reusing it would put a
                 * fresh snapshot behind a stale log. */
                if (gen > max_gen) max_gen = gen;
                break;
            case SST_NAME_OTHER:
                break;
        }
        at = end + 1;
    }
    s->next_gen = max_gen + 1;

    /* Candidates: generations with a manifest, newest first. Insertion
     * sort over what is nearly always one or two entries. */
    s->candidates = (uint32_t *)malloc((s->n_gens ? s->n_gens : 1) * sizeof(uint32_t));
    if (!s->candidates) return BJ_ERR_OOM;
    for (uint32_t i = 0; i < s->n_gens; i++) {
        if (!s->gens[i].manifest) continue;
        uint32_t at2 = s->n_candidates;
        while (at2 > 0 && s->gens[s->candidates[at2 - 1]].gen < s->gens[i].gen) {
            s->candidates[at2] = s->candidates[at2 - 1];
            at2--;
        }
        s->candidates[at2] = i;
        s->n_candidates++;
    }
    return BJ_OK;
}

uint32_t sst_candidate_count(const sst *s) { return s->n_candidates; }

const char *sst_candidate_manifest(const sst *s, uint32_t i, uint32_t *len) {
    if (i >= s->n_candidates) { *len = 0; return NULL; }
    const char *n = s->gens[s->candidates[i]].manifest;
    *len = (uint32_t)strlen(n);
    return n;
}

/* ---- manifest parsing -------------------------------------------------- */

/* One {role, name, size, crc} entry. Spans point into the record. */
typedef struct {
    const uint8_t *role; uint32_t role_len;
    const uint8_t *name; uint32_t name_len;
    uint64_t size;
    uint32_t crc;
} mf_file;

static int read_string_field(const uint8_t *v, size_t vlen, const uint8_t **p, uint32_t *len) {
    if (vlen < 5 || v[0] != BJ_TYPE_STRING) return SST_ERR_MANIFEST;
    uint32_t n = rdu32(v + 1);
    if ((size_t)n + 5 != vlen) return SST_ERR_MANIFEST;
    *p = v + 5;
    *len = n;
    return BJ_OK;
}

static int read_u64_field(const uint8_t *v, size_t vlen, uint64_t *out) {
    cur c = { v, vlen, 0 };
    double d;
    if (read_number(&c, &d) != BJ_OK) return SST_ERR_MANIFEST;
    if (d < 0 || d > 9007199254740992.0) return SST_ERR_MANIFEST;
    *out = (uint64_t)d;
    return BJ_OK;
}

/* Any failure -- malformed buffer, missing key -- is SST_ERR_MANIFEST, so
 * a bad manifest always reads as "this generation did not commit" and the
 * caller moves on to the next candidate instead of aborting the open. */
static int field(const uint8_t *obj, size_t obj_len, const char *key,
                 const uint8_t **v, size_t *vlen) {
    int found = 0;
    int e = obj_get_field(obj, obj_len, (const uint8_t *)key, (uint32_t)strlen(key),
                          v, vlen, &found);
    if (e || !found) return SST_ERR_MANIFEST;
    return BJ_OK;
}

/*
 * Walk a manifest's `files` array, handing each entry to `visit`. Shared
 * by adoption, sweeping and validation so there is one reading of the
 * record -- three readers is how a checksum rule drifts.
 */
typedef int (*mf_visit)(void *ctx, const mf_file *f);

static int files_each(const uint8_t *fv, size_t fvlen, mf_visit visit, void *ctx) {
    int e = BJ_OK;
    cur c = { fv, fvlen, 0 };
    uint32_t count;
    if (array_begin(&c, &count) != BJ_OK) return SST_ERR_MANIFEST;
    for (uint32_t i = 0; i < count; i++) {
        size_t start = c.pos;
        if (skip_value(&c) != BJ_OK) return SST_ERR_MANIFEST;
        const uint8_t *entry = c.d + start;
        size_t entry_len = c.pos - start;

        mf_file f;
        memset(&f, 0, sizeof(f));
        const uint8_t *v; size_t vlen;
        if ((e = field(entry, entry_len, "role", &v, &vlen))) return e;
        if ((e = read_string_field(v, vlen, &f.role, &f.role_len))) return e;
        /* Whether the role is USABLE is not decided here: reading a
         * manifest with an impossible role means "this generation did
         * not commit" (fall back to the next candidate), while writing
         * one means the caller made a mistake (SST_ERR_ROLE). The two
         * visitors say which. */
        /* `name` is optional: a manifest that travelled over the wire
         * from a leader describes roles, not the receiver's filenames. */
        if (field(entry, entry_len, "name", &v, &vlen) == BJ_OK) {
            if ((e = read_string_field(v, vlen, &f.name, &f.name_len))) return e;
        }
        if ((e = field(entry, entry_len, "size", &v, &vlen))) return e;
        if ((e = read_u64_field(v, vlen, &f.size))) return e;
        uint64_t crc;
        if ((e = field(entry, entry_len, "crc", &v, &vlen))) return e;
        if ((e = read_u64_field(v, vlen, &crc))) return e;
        if (crc > 0xffffffffu) return SST_ERR_MANIFEST;
        f.crc = (uint32_t)crc;

        if ((e = visit(ctx, &f))) return e;
    }
    return BJ_OK;
}

static int mf_each_file(const uint8_t *manifest, uint32_t manifest_len,
                        mf_visit visit, void *ctx) {
    const uint8_t *fv; size_t fvlen;
    int e = field(manifest, manifest_len, "files", &fv, &fvlen);
    if (e) return e;
    return files_each(fv, fvlen, visit, ctx);
}

/* A manifest record is binjson followed by a little-endian CRC-32 of
 * exactly those bytes. Validity IS the commit, so this must be strict:
 * an accepted torn manifest adopts a half-written generation. */
static int manifest_body(const uint8_t *bytes, uint32_t len,
                         const uint8_t **body, uint32_t *body_len) {
    if (len < 5) return SST_ERR_MANIFEST;
    uint32_t n = len - 4;
    uint32_t want = rdu32(bytes + n);
    if (bjfile_crc32(0, bytes, n) != want) return SST_ERR_MANIFEST;

    uint64_t version;
    const uint8_t *v; size_t vlen;
    if (field(bytes, n, "snapshot", &v, &vlen)) return SST_ERR_MANIFEST;
    if (read_u64_field(v, vlen, &version)) return SST_ERR_MANIFEST;
    if (version != SST_MANIFEST_VERSION) return SST_ERR_MANIFEST;
    if (field(bytes, n, "lastIncludedIndex", &v, &vlen)) return SST_ERR_MANIFEST;
    if (field(bytes, n, "lastIncludedTerm", &v, &vlen)) return SST_ERR_MANIFEST;

    *body = bytes;
    *body_len = n;
    return BJ_OK;
}

/* ---- adoption ---------------------------------------------------------- */

typedef struct { const sst_gen *g; sst *s; uint64_t gen; } adopt_ctx;

static int pending_add(sst *s, const char *name, size_t name_len, uint64_t want_size) {
    if (s->n_pending == s->cap_pending) {
        uint32_t nc = s->cap_pending ? s->cap_pending * 2 : 8;
        char **nn = (char **)realloc(s->pending_names, (size_t)nc * sizeof(*nn));
        if (!nn) return BJ_ERR_OOM;
        s->pending_names = nn;
        uint64_t *ns = (uint64_t *)realloc(s->pending_sizes, (size_t)nc * sizeof(*ns));
        if (!ns) return BJ_ERR_OOM;
        s->pending_sizes = ns;
        s->cap_pending = nc;
    }
    char *copy = dup_n(name, (uint32_t)name_len);
    if (!copy) return BJ_ERR_OOM;
    s->pending_names[s->n_pending] = copy;
    s->pending_sizes[s->n_pending] = want_size;
    s->n_pending++;
    return BJ_OK;
}

/*
 * Beat one's per-file check: the file must be present in this generation
 * under the name this store would give that role. Checking the name as
 * well as the role stops a manifest from pointing at a file belonging to
 * some other generation. The recorded size goes on the pending list for
 * beat two rather than being checked here -- nothing has been opened yet.
 */
static int manifest_check(void *vctx, const mf_file *f) {
    adopt_ctx *ctx = (adopt_ctx *)vctx;
    dbuf want = {0};
    int e = sst_data_name(ctx->s, ctx->gen, (const char *)f->role, f->role_len, &want);
    /* A role that cannot be a filename names a file that cannot exist,
     * so this manifest describes no generation on disk -- reported as a
     * failed manifest, which the host's loop falls back from, rather
     * than as an error that would abort the open entirely. */
    if (e) { dbuf_free(&want); return SST_ERR_MANIFEST; }

    int present = 0;
    for (uint32_t i = 0; i < ctx->g->n; i++) {
        const sst_file *have = &ctx->g->files[i];
        if (strlen(have->name) != want.len) continue;
        if (memcmp(have->name, want.data, want.len) != 0) continue;
        /* A manifest may carry filenames (a locally written one does) or
         * only roles (one that travelled from a leader does). When it
         * carries them they must be this generation's. */
        if (f->name_len && (f->name_len != want.len ||
                            memcmp(f->name, want.data, want.len) != 0)) break;
        present = 1;
        break;
    }
    if (!present) { dbuf_free(&want); return SST_ERR_MANIFEST; }

    e = pending_add(ctx->s, (const char *)want.data, want.len, f->size);
    dbuf_free(&want);
    return e;
}

int sst_try_manifest(sst *s, uint32_t i, const uint8_t *bytes, uint32_t len) {
    if (i >= s->n_candidates) return BJ_ERR_RANGE;
    pending_clear(s);
    const sst_gen *g = &s->gens[s->candidates[i]];

    const uint8_t *body; uint32_t body_len;
    int e = manifest_body(bytes, len, &body, &body_len);
    if (e) return e;

    adopt_ctx ctx = { g, s, g->gen };
    e = mf_each_file(body, body_len, manifest_check, &ctx);
    if (e) { pending_clear(s); return e; }

    s->pending_manifest.len = 0;
    e = dbuf_put(&s->pending_manifest, body, body_len);
    if (e) { pending_clear(s); return e; }
    s->pending_cand = i;
    s->pending = 1;
    return BJ_OK;
}

uint32_t sst_pending_count(const sst *s) { return s->pending ? s->n_pending : 0; }

const char *sst_pending_name(const sst *s, uint32_t i, uint32_t *len) {
    if (!s->pending || i >= s->n_pending) { *len = 0; return NULL; }
    *len = (uint32_t)strlen(s->pending_names[i]);
    return s->pending_names[i];
}

int sst_confirm(sst *s, const double *sizes, uint32_t n) {
    if (!s->pending) return BJ_ERR_STATE;
    if (n != s->n_pending) { pending_clear(s); return SST_ERR_MANIFEST; }
    for (uint32_t i = 0; i < n; i++) {
        double d = sizes[i];
        if (d < 0 || (uint64_t)d != s->pending_sizes[i]) { pending_clear(s); return SST_ERR_MANIFEST; }
    }
    s->latest.len = 0;
    int e = dbuf_put(&s->latest, s->pending_manifest.data, s->pending_manifest.len);
    if (e) { pending_clear(s); return e; }
    s->latest_gen = s->gens[s->candidates[s->pending_cand]].gen;
    s->has_latest = 1;
    pending_clear(s);
    return BJ_OK;
}

int sst_has_latest(const sst *s) { return s->has_latest; }
uint64_t sst_latest_gen(const sst *s) { return s->latest_gen; }
uint64_t sst_next_gen(const sst *s) { return s->next_gen; }

int sst_latest(const sst *s, dbuf *out, int *has) {
    *has = s->has_latest;
    if (!s->has_latest) return BJ_OK;

    /* The stored record plus the generation number, which lives in the
     * filenames rather than inside the manifest -- so a generation's
     * files can be identified without opening one. */
    cur c = { s->latest.data, s->latest.len, 0 };
    uint32_t count;
    if (object_begin(&c, &count) != BJ_OK) return SST_ERR_MANIFEST;

    bj_builder *b = bj_builder_new();
    if (!b) return BJ_ERR_OOM;
    int e = bj_begin_object(b);
    if (!e) e = bj_put_key(b, (const uint8_t *)"gen", 3);
    if (!e) e = bj_put_int(b, (int64_t)s->latest_gen);
    for (uint32_t i = 0; !e && i < count; i++) {
        const uint8_t *kp; uint32_t klen;
        if ((e = take_key(&c, &kp, &klen))) break;
        size_t start = c.pos;
        if ((e = skip_value(&c))) break;
        if (klen == 8 && memcmp(kp, "snapshot", 8) == 0) continue;  /* wire detail */
        e = bj_put_key(b, kp, klen);
        if (!e) e = bj_put_raw(b, c.d + start, (uint32_t)(c.pos - start));
    }
    if (!e) e = bj_end_object(b);
    if (!e) {
        size_t n; const uint8_t *p = bj_builder_data(b, &n);
        if (!p) e = bj_builder_error(b) ? bj_builder_error(b) : BJ_ERR_STATE;
        else e = dbuf_put(out, p, n);
    }
    bj_builder_free(b);
    return e;
}

int sst_sweep_plan(const sst *s, dbuf *out) {
    for (uint32_t i = 0; i < s->n_gens; i++) {
        const sst_gen *g = &s->gens[i];
        if (s->has_latest && g->gen == s->latest_gen) continue;
        int e = BJ_OK;
        if (g->manifest) {
            e = put_cstr(out, g->manifest);
            if (!e) e = dbuf_put(out, (const uint8_t *)"", 1);
        }
        for (uint32_t k = 0; !e && k < g->n; k++) {
            e = put_cstr(out, g->files[k].name);
            if (!e) e = dbuf_put(out, (const uint8_t *)"", 1);
        }
        if (e) return e;
    }
    return BJ_OK;
}

/* ---- commit ------------------------------------------------------------ */

/* Roles must be usable as filenames and unique within a generation --
 * checked while encoding, so a bad one fails the commit rather than
 * producing a generation that silently never adopts. */
typedef struct { const uint8_t *seen[64]; uint32_t seen_len[64]; uint32_t n; } role_set;

static int role_set_add(role_set *rs, const uint8_t *role, uint32_t len) {
    if (!role_is_wellformed((const char *)role, len)) return SST_ERR_ROLE;
    for (uint32_t i = 0; i < rs->n && i < 64; i++) {
        if (rs->seen_len[i] == len && memcmp(rs->seen[i], role, len) == 0) return SST_ERR_ROLE;
    }
    if (rs->n < 64) { rs->seen[rs->n] = role; rs->seen_len[rs->n] = len; }
    rs->n++;
    return BJ_OK;
}

static int encode_check(void *ctx, const mf_file *f) {
    return role_set_add((role_set *)ctx, f->role, f->role_len);
}

int sst_manifest_encode(uint64_t last_included_index, uint64_t last_included_term,
                        const uint8_t *config, uint32_t config_len,
                        const uint8_t *files, uint32_t files_len,
                        dbuf *out) {
    /* Roles first, over the caller's own array -- these are the bytes
     * that get embedded verbatim, and a mistake here must be reported as
     * the mistake it is rather than surfacing at the next open as a
     * generation that mysteriously never adopts. */
    role_set rs;
    memset(&rs, 0, sizeof(rs));
    int e = files_each(files, files_len, encode_check, &rs);
    if (e) return e;

    bj_builder *b = bj_builder_new();
    if (!b) return BJ_ERR_OOM;
    e = bj_begin_object(b);
    if (!e) e = bj_put_key(b, (const uint8_t *)"snapshot", 8);
    if (!e) e = bj_put_int(b, SST_MANIFEST_VERSION);
    if (!e) e = bj_put_key(b, (const uint8_t *)"lastIncludedIndex", 17);
    if (!e) e = bj_put_int(b, (int64_t)last_included_index);
    if (!e) e = bj_put_key(b, (const uint8_t *)"lastIncludedTerm", 16);
    if (!e) e = bj_put_int(b, (int64_t)last_included_term);
    if (!e) e = bj_put_key(b, (const uint8_t *)"config", 6);
    if (!e) e = config_len ? bj_put_raw(b, config, config_len) : bj_put_null(b);
    if (!e) e = bj_put_key(b, (const uint8_t *)"files", 5);
    if (!e) e = bj_put_raw(b, files, files_len);
    if (!e) e = bj_end_object(b);

    size_t n = 0; const uint8_t *p = NULL;
    if (!e) {
        p = bj_builder_data(b, &n);
        if (!p) e = bj_builder_error(b) ? bj_builder_error(b) : BJ_ERR_STATE;
    }
    if (!e) e = dbuf_put(out, p, n);
    if (!e) {
        uint32_t crc = bjfile_crc32(0, p, n);
        uint8_t tail[4] = { (uint8_t)(crc & 0xff), (uint8_t)((crc >> 8) & 0xff),
                            (uint8_t)((crc >> 16) & 0xff), (uint8_t)((crc >> 24) & 0xff) };
        e = dbuf_put(out, tail, 4);
    }
    bj_builder_free(b);
    return e;
}

int sst_adopt_committed(sst *s, uint64_t gen, const uint8_t *manifest, uint32_t len,
                        dbuf *sweep) {
    const uint8_t *body; uint32_t body_len;
    int e = manifest_body(manifest, len, &body, &body_len);
    if (e) return e;

    /* The generation being replaced, if any -- its files are what the
     * caller deletes. Read BEFORE latest is overwritten. */
    if (s->has_latest && s->latest_gen != gen) {
        dbuf name = {0};
        e = sst_manifest_name(s, s->latest_gen, &name);
        if (!e) e = dbuf_put(sweep, name.data, name.len);
        if (!e) e = dbuf_put(sweep, (const uint8_t *)"", 1);
        dbuf_free(&name);

        uint64_t prev_gen = s->latest_gen;
        /* Names come from the store's own convention, not the manifest's
         * `name` field, so a manifest received from a leader (which
         * carries no names) sweeps correctly too. */
        const uint8_t *fv; size_t fvlen;
        if (!e && field(s->latest.data, s->latest.len, "files", &fv, &fvlen) == BJ_OK) {
            cur c = { fv, fvlen, 0 };
            uint32_t count;
            if (array_begin(&c, &count) == BJ_OK) {
                for (uint32_t i = 0; !e && i < count; i++) {
                    size_t start = c.pos;
                    if (skip_value(&c) != BJ_OK) break;
                    const uint8_t *v; size_t vlen;
                    if (field(c.d + start, c.pos - start, "role", &v, &vlen)) continue;
                    const uint8_t *rp; uint32_t rlen;
                    if (read_string_field(v, vlen, &rp, &rlen)) continue;
                    dbuf dn = {0};
                    e = sst_data_name(s, prev_gen, (const char *)rp, rlen, &dn);
                    if (!e) e = dbuf_put(sweep, dn.data, dn.len);
                    if (!e) e = dbuf_put(sweep, (const uint8_t *)"", 1);
                    dbuf_free(&dn);
                }
            }
        }
        if (e) return e;
    }

    s->latest.len = 0;
    e = dbuf_put(&s->latest, body, body_len);
    if (e) return e;
    s->latest_gen = gen;
    s->has_latest = 1;
    if (gen >= s->next_gen) s->next_gen = gen + 1;
    return BJ_OK;
}

/* ---- validation -------------------------------------------------------- */

typedef struct {
    const uint8_t *actual; uint32_t actual_len;
    const uint8_t **bad_role; uint32_t *bad_role_len;
} check_ctx;

static int check_one(void *vctx, const mf_file *want) {
    check_ctx *ctx = (check_ctx *)vctx;
    cur c = { ctx->actual, ctx->actual_len, 0 };
    uint32_t count;
    if (array_begin(&c, &count) != BJ_OK) return SST_ERR_MANIFEST;

    for (uint32_t i = 0; i < count; i++) {
        size_t start = c.pos;
        if (skip_value(&c) != BJ_OK) return SST_ERR_MANIFEST;
        const uint8_t *entry = c.d + start;
        size_t entry_len = c.pos - start;

        const uint8_t *v; size_t vlen;
        if (field(entry, entry_len, "role", &v, &vlen)) return SST_ERR_MANIFEST;
        const uint8_t *rp; uint32_t rlen;
        if (read_string_field(v, vlen, &rp, &rlen)) return SST_ERR_MANIFEST;
        if (rlen != want->role_len || memcmp(rp, want->role, rlen) != 0) continue;

        uint64_t size, crc;
        if (field(entry, entry_len, "size", &v, &vlen)) return SST_ERR_MANIFEST;
        if (read_u64_field(v, vlen, &size)) return SST_ERR_MANIFEST;
        if (field(entry, entry_len, "crc", &v, &vlen)) return SST_ERR_MANIFEST;
        if (read_u64_field(v, vlen, &crc)) return SST_ERR_MANIFEST;

        if (size != want->size || (uint32_t)crc != want->crc) break;
        return BJ_OK;
    }
    /* Absent counts as mismatched: an install that never received a file
     * has not received the snapshot. */
    *ctx->bad_role = want->role;
    *ctx->bad_role_len = want->role_len;
    return SST_ERR_CHECKSUM;
}

int sst_check_files(const uint8_t *manifest, uint32_t manifest_len,
                    const uint8_t *actual, uint32_t actual_len,
                    const uint8_t **bad_role, uint32_t *bad_role_len) {
    *bad_role = NULL;
    *bad_role_len = 0;
    check_ctx ctx = { actual, actual_len, bad_role, bad_role_len };
    return mf_each_file(manifest, manifest_len, check_one, &ctx);
}

/* ---- paired entry logs -------------------------------------------------- */

/* Walk `listing`, collect this store's log files, emit newest first. */
static int log_walk(const sst *s, const uint8_t *listing, uint32_t listing_len,
                    const char *skip, uint32_t skip_len, dbuf *out) {
    /* At most a handful ever exist (pruneLogs runs after every snapshot),
     * so a selection pass beats allocating a sort buffer. */
    uint64_t emitted_above = UINT64_MAX;
    int first = 1;
    for (;;) {
        uint64_t best = 0; int have_best = 0;
        const char *best_name = NULL; uint32_t best_len = 0;

        uint32_t at = 0;
        while (at < listing_len) {
            uint32_t end = at;
            while (end < listing_len && listing[end] != '\0') end++;
            const char *name = (const char *)listing + at;
            uint32_t name_len = end - at;
            at = end + 1;
            if (name_len == 0) continue;

            uint64_t gen = 0; const char *role; uint32_t role_len;
            if (classify(s, name, name_len, &gen, &role, &role_len) != SST_NAME_LOG) continue;
            if (!first && gen >= emitted_above) continue;
            if (skip_len && name_len == skip_len && memcmp(name, skip, skip_len) == 0) continue;
            if (!have_best || gen > best) { best = gen; have_best = 1; best_name = name; best_len = name_len; }
        }
        if (!have_best) return BJ_OK;

        int e = dbuf_put(out, (const uint8_t *)best_name, best_len);
        if (!e) e = dbuf_put(out, (const uint8_t *)"", 1);
        if (e) return e;
        emitted_above = best;
        first = 0;
    }
}

int sst_log_candidates(const sst *s, const uint8_t *listing, uint32_t listing_len,
                       dbuf *out) {
    return log_walk(s, listing, listing_len, NULL, 0, out);
}

int sst_prune_logs_plan(const sst *s, const uint8_t *listing, uint32_t listing_len,
                        const char *keep, uint32_t keep_len, dbuf *out) {
    return log_walk(s, listing, listing_len, keep, keep_len, out);
}
