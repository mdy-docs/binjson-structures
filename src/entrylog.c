/*
 * entrylog.c — persistent replicated-command log (Raft log / WAL). See
 * entrylog.h.
 *
 * The log is file-resident and append-only, built on bjfile exactly like
 * textlog.c: entry records and a fixed-size metadata record travel in the
 * binjson wire format, every commit ends with metadata preceded by a CRC
 * trailer (bjfile_append_protected), and open runs one bjfile_scan_commits
 * pass that verifies each protected commit, indexes entry offsets, and
 * recovers a torn tail by truncation.
 *
 * Records:
 *   entry     { index, term, type, payload }        (payload is BINARY)
 *   metadata  { baseIndex, baseTerm, lastIndex, lastTerm,
 *               currentTerm, votedFor, commitIndex }  (all INT: fixed size)
 *
 * Suffix truncation is logical: elog_truncate_from commits a metadata record
 * whose lastIndex moved back, and a later append writes the replacement
 * entry at a new offset. The open-time scan replays this in file order — an
 * entry whose index is not last+1 supersedes (drops) everything from that
 * index up — so the in-memory offset index always reflects the live log and
 * dead bytes are never read again. ents[i] is entry base_index + 1 + i.
 *
 * Durability: elog_append only buffers in the bjfile pending-write buffer;
 * elog_sync ends the batch with metadata + trailer and pushes it to the host
 * in one write. Hard-state changes commit immediately (Raft requires them
 * durable before any RPC response). Mutating operations either commit fully
 * or roll the in-memory state back to the last committed metadata (`cm`).
 */
#include "entrylog.h"
#include "bjfile.h"
#include "bjcursor.h"
#include "dbuf.h"

#include <stdlib.h>
#include <string.h>

/* On-wire size of a metadata record: OBJECT header (1 type + 4 size + 4
 * count) plus 7 fields, each 4 (key length) + key bytes + 9 (INT). The key
 * lengths sum to 64, so 9 + 7 * 13 + 64. Fixed by construction (all fields
 * are always written, all as INT). */
#define EL_META_SIZE 164

/* ---- Record parsing -------------------------------------------------- */

typedef struct {
    int is_entry;                    /* has "payload"      */
    int is_metadata;                 /* has "currentTerm"  */
    uint64_t index, term;
    int etype;
    const uint8_t *payload; uint32_t payload_len;   /* into image */
    uint64_t base_index, base_term;
    uint64_t last_index, last_term;
    uint64_t current_term, voted_for, commit_index;
} erec;

static int take_binary(cur *c, const uint8_t **p, uint32_t *len) {
    uint8_t t;
    if (take_type(c, &t)) return BJ_ERR_EOF;
    if (t != BJ_TYPE_BINARY) return BJ_ERR_UNKNOWN_TYPE;
    if (take_u32(c, len)) return BJ_ERR_EOF;
    if (cur_need(c, *len)) return BJ_ERR_EOF;
    *p = c->d + c->pos; c->pos += *len;
    return BJ_OK;
}

/* Parse the record bytes (rec, len) into *r. The payload span points into
 * `rec` and is only valid while those bytes are. */
static int parse_record(const uint8_t *rec, size_t len, erec *r) {
    memset(r, 0, sizeof(*r));
    cur c = { rec, len, 0 };
    uint32_t count;
    int e = object_begin(&c, &count);
    if (e) return e;
    for (uint32_t i = 0; i < count; i++) {
        const uint8_t *kn; uint32_t klen;
        if ((e = take_key(&c, &kn, &klen))) return e;
        if (name_eq(kn, klen, "index")) {
            if ((e = read_u64(&c, &r->index))) return e;
        } else if (name_eq(kn, klen, "term")) {
            if ((e = read_u64(&c, &r->term))) return e;
        } else if (name_eq(kn, klen, "type")) {
            if ((e = read_int31(&c, &r->etype))) return e;
        } else if (name_eq(kn, klen, "payload")) {
            if ((e = take_binary(&c, &r->payload, &r->payload_len))) return e;
            r->is_entry = 1;
        } else if (name_eq(kn, klen, "baseIndex")) {
            if ((e = read_u64(&c, &r->base_index))) return e;
        } else if (name_eq(kn, klen, "baseTerm")) {
            if ((e = read_u64(&c, &r->base_term))) return e;
        } else if (name_eq(kn, klen, "lastIndex")) {
            if ((e = read_u64(&c, &r->last_index))) return e;
        } else if (name_eq(kn, klen, "lastTerm")) {
            if ((e = read_u64(&c, &r->last_term))) return e;
        } else if (name_eq(kn, klen, "currentTerm")) {
            if ((e = read_u64(&c, &r->current_term))) return e;
            r->is_metadata = 1;
        } else if (name_eq(kn, klen, "votedFor")) {
            if ((e = read_u64(&c, &r->voted_for))) return e;
        } else if (name_eq(kn, klen, "commitIndex")) {
            if ((e = read_u64(&c, &r->commit_index))) return e;
        } else {
            if ((e = skip_value(&c))) return e;
        }
    }
    return BJ_OK;
}

/* ---- Log state ------------------------------------------------------- */

/* Index of one live entry: where its record lives and its term (kept in
 * memory so elog_term_at — the hot AppendEntries consistency check — never
 * reads the file). Position i holds entry base_index + 1 + i. */
typedef struct {
    uint64_t off;
    uint64_t term;
} el_ent;

/* The mutable metadata fields as one value, so the last committed state can
 * be snapshotted and restored wholesale on a failed commit. */
typedef struct {
    uint64_t last_index, last_term;
    uint64_t current_term, voted_for;
    uint64_t commit_index;
    int      n_ents;
} el_meta;

struct elog {
    bjfile      f;                /* backing file                          */
    dbuf        out;              /* last read output                      */
    bj_builder *bld;              /* reused for entry/metadata encoding    */
    uint64_t    base_index;       /* tile base: log owns (base, last]      */
    uint64_t    base_term;        /* term of entry base_index              */
    el_meta     m;                /* live state (may lead the file)        */
    el_meta     cm;               /* state as of the last committed sync   */
    el_ent     *ents; int cap_ents;   /* live entries; count is m.n_ents   */
};

static int ents_reserve(elog *t, int need) {
    if (need <= t->cap_ents) return BJ_OK;
    int nc = t->cap_ents ? t->cap_ents * 2 : 16;
    while (nc < need) nc *= 2;
    el_ent *ne = (el_ent *)realloc(t->ents, (size_t)nc * sizeof(el_ent));
    if (!ne) return BJ_ERR_OOM;
    t->ents = ne; t->cap_ents = nc;
    return BJ_OK;
}

static int set_out(elog *t, const uint8_t *b, size_t n) {
    t->out.len = 0;
    return dbuf_put(&t->out, b, n);
}

/* ---- Encoding -------------------------------------------------------- */

static int encode_entry(elog *t, uint64_t index, uint64_t term, int type,
                        const uint8_t *payload, uint32_t payload_len,
                        uint64_t *off) {
    bj_builder *b = t->bld;
    bj_builder_reset(b);
    bj_begin_object(b);
    bj_put_key(b, (const uint8_t *)"index", 5);   bj_put_int(b, (int64_t)index);
    bj_put_key(b, (const uint8_t *)"term", 4);    bj_put_int(b, (int64_t)term);
    bj_put_key(b, (const uint8_t *)"type", 4);    bj_put_int(b, type);
    bj_put_key(b, (const uint8_t *)"payload", 7); bj_put_binary(b, payload, payload_len);
    bj_end_object(b);
    int e = bj_builder_error(b);
    if (e) return e;
    size_t len;
    const uint8_t *d = bj_builder_data(b, &len);
    if (!d) return BJ_ERR_STATE;
    return bjfile_append(&t->f, d, len, off);
}

/* Encode a metadata record for the given state into `b`. Every field is
 * written every time, all as INT, keeping the record EL_META_SIZE bytes. */
static int encode_metadata(bj_builder *b, uint64_t base_index, uint64_t base_term,
                           const el_meta *m) {
    bj_builder_reset(b);
    bj_begin_object(b);
    bj_put_key(b, (const uint8_t *)"baseIndex", 9);    bj_put_int(b, (int64_t)base_index);
    bj_put_key(b, (const uint8_t *)"baseTerm", 8);     bj_put_int(b, (int64_t)base_term);
    bj_put_key(b, (const uint8_t *)"lastIndex", 9);    bj_put_int(b, (int64_t)m->last_index);
    bj_put_key(b, (const uint8_t *)"lastTerm", 8);     bj_put_int(b, (int64_t)m->last_term);
    bj_put_key(b, (const uint8_t *)"currentTerm", 11); bj_put_int(b, (int64_t)m->current_term);
    bj_put_key(b, (const uint8_t *)"votedFor", 8);     bj_put_int(b, (int64_t)m->voted_for);
    bj_put_key(b, (const uint8_t *)"commitIndex", 11); bj_put_int(b, (int64_t)m->commit_index);
    bj_end_object(b);
    return bj_builder_error(b);
}

/* Append metadata (CRC-trailer-protected, ending the commit) to `f`. */
static int append_metadata(bjfile *f, bj_builder *b, uint64_t base_index,
                           uint64_t base_term, const el_meta *m) {
    int e = encode_metadata(b, base_index, base_term, m);
    if (e) return e;
    size_t len;
    const uint8_t *d = bj_builder_data(b, &len);
    if (!d) return BJ_ERR_STATE;
    return bjfile_append_protected(f, d, len);
}

/*
 * End the current batch: metadata + trailer, one host write. On failure the
 * pending bytes are dropped and the live state rolls back to the committed
 * one, so the memory picture always matches the file.
 */
static int commit_state(elog *t) {
    int e = append_metadata(&t->f, t->bld, t->base_index, t->base_term, &t->m);
    if (!e) e = bjfile_commit(&t->f);
    if (e) {
        bjfile_discard(&t->f);
        t->m = t->cm;
        return e;
    }
    t->cm = t->m;
    return BJ_OK;
}

/* ---- Lifecycle ------------------------------------------------------- */

elog *elog_create_at(const bj_io *io, uint64_t base_index, uint64_t base_term) {
    elog *t = (elog *)calloc(1, sizeof(elog));
    if (!t) return NULL;
    t->bld = bj_builder_new();
    if (!t->bld) { free(t); return NULL; }
    bjfile_init(&t->f, io);
    t->base_index = base_index;
    t->base_term = base_term;
    /* A fresh tile owns (base, ...]: empty, with last == base. Entries at or
     * below the base are committed by definition (they live in the snapshot
     * this tile continues from), so the commit index starts there. Hard
     * state starts at the snapshot's term with no vote; the host restores
     * the real values with elog_set_hard_state before serving RPCs. */
    t->m.last_index = base_index;
    t->m.last_term = base_term;
    t->m.current_term = base_term;
    t->m.voted_for = EL_VOTED_NONE;
    t->m.commit_index = base_index;
    t->m.n_ents = 0;
    t->cm = t->m;
    if (bjfile_append_header(&t->f, t->bld, "entrylog") ||
        append_metadata(&t->f, t->bld, base_index, base_term, &t->m) ||
        bjfile_commit(&t->f)) {
        elog_free(t);
        return NULL;
    }
    return t;
}

elog *elog_create(const bj_io *io) {
    return elog_create_at(io, 0, 0);
}

/*
 * Commit-scan state. Entries are collected raw, in file order, with their
 * indexes; the live entry table is derived afterwards, once the last good
 * commit (and thus the authoritative metadata) is known — deriving during
 * the scan would bake in effects of a torn tail commit that recovery then
 * rejects. The last two metadata candidates are kept: at most one metadata
 * record can sit in a rejected tail commit, so the one ending exactly at
 * the last good offset is always among them.
 */
typedef struct { uint64_t off, index, term; } el_raw;

typedef struct {
    el_raw  *raw; int n_raw, cap_raw;
    erec     md[2];
    uint64_t md_end[2];
    int      n_md;
} el_scan;

static int el_scan_cb(void *ctx, uint64_t off, const uint8_t *rec,
                      size_t rec_len, int *is_commit_end) {
    el_scan *s = (el_scan *)ctx;
    erec r;
    if (parse_record(rec, rec_len, &r)) return BJ_OK;  /* not an object: skip */
    if (r.is_entry) {
        if (s->n_raw == s->cap_raw) {
            int nc = s->cap_raw ? s->cap_raw * 2 : 16;
            el_raw *nr = (el_raw *)realloc(s->raw, (size_t)nc * sizeof(el_raw));
            if (!nr) return BJ_ERR_OOM;
            s->raw = nr; s->cap_raw = nc;
        }
        el_raw e = { off, r.index, r.term };
        s->raw[s->n_raw++] = e;
    }
    if (r.is_metadata) {
        s->md[s->n_md & 1] = r;
        s->md_end[s->n_md & 1] = off + rec_len;
        s->n_md++;
        *is_commit_end = 1;
    }
    return BJ_OK;
}

/*
 * Open: verify the file identifies as an entry log (when it carries a
 * header), scan it once — verifying every protected commit's CRC and
 * collecting entry records — then rebuild the live entry table by replaying
 * the collected entries in file order against the adopted metadata: an
 * entry whose index is not last+1 supersedes everything from that index up
 * (the on-disk shape a logical truncation plus re-append leaves behind).
 * A torn tail is truncated back to the last good commit.
 */
elog *elog_open(const bj_io *io) {
    elog *t = (elog *)calloc(1, sizeof(elog));
    if (!t) return NULL;
    t->bld = bj_builder_new();
    if (!t->bld) { free(t); return NULL; }
    bjfile_init(&t->f, io);

    if (bjfile_check_header(&t->f, "entrylog") < 0) { elog_free(t); return NULL; }

    el_scan s;
    memset(&s, 0, sizeof(s));
    uint64_t good = 0, flen = bjfile_len(&t->f);
    if (bjfile_scan_commits(&t->f, el_scan_cb, &s, &good)) goto fail;

    {
        /* Adopt the metadata record that ends the last good commit. */
        erec *adopt = NULL;
        for (int i = 0; i < 2 && i < s.n_md; i++)
            if (s.md_end[i] == good) adopt = &s.md[i];
        if (!adopt ||
            adopt->last_index < adopt->base_index ||
            adopt->last_term < adopt->base_term ||
            adopt->current_term < adopt->last_term ||
            adopt->commit_index > adopt->last_index ||
            adopt->last_index - adopt->base_index > 0x7fffffff)
            goto fail;

        t->base_index = adopt->base_index;
        t->base_term = adopt->base_term;
        t->m.last_index = adopt->last_index;
        t->m.last_term = adopt->last_term;
        t->m.current_term = adopt->current_term;
        t->m.voted_for = adopt->voted_for;
        t->m.commit_index = adopt->commit_index;

        /* Replay the raw entries (good commits only) into the live table. */
        int n = 0;
        for (int i = 0; i < s.n_raw; i++) {
            el_raw *r = &s.raw[i];
            if (r->off >= good) continue;              /* rejected tail commit */
            if (r->index <= t->base_index) goto fail;  /* below the tile base  */
            uint64_t pos = r->index - t->base_index - 1;
            if (pos > (uint64_t)n) goto fail;          /* gap in the sequence  */
            n = (int)pos;                              /* supersede from here  */
            if (ents_reserve(t, n + 1)) goto fail;
            el_ent ent = { r->off, r->term };
            t->ents[n++] = ent;
        }

        /* The metadata bounds are authoritative: entries above lastIndex are
         * dead (a truncation not followed by re-appends); fewer entries than
         * the bounds promise is a damaged file. The surviving tail entry's
         * term must corroborate lastTerm. */
        int want = (int)(t->m.last_index - t->base_index);
        if (n < want) goto fail;
        t->m.n_ents = want;
        uint64_t tail_term = want ? t->ents[want - 1].term : t->base_term;
        if (tail_term != t->m.last_term) goto fail;
    }

    t->cm = t->m;
    if (good < flen && bjfile_set_len(&t->f, good)) goto fail;
    free(s.raw);
    return t;

fail:
    free(s.raw);
    elog_free(t);
    return NULL;
}

void elog_free(elog *t) {
    if (!t) return;
    bj_builder_free(t->bld);
    bjfile_dispose(&t->f);
    free(t->out.data);
    free(t->ents);
    free(t);
}

/* ---- Accessors ------------------------------------------------------- */

uint64_t elog_base_index(const elog *t)   { return t->base_index; }
uint64_t elog_base_term(const elog *t)    { return t->base_term; }
uint64_t elog_last_index(const elog *t)   { return t->m.last_index; }
uint64_t elog_last_term(const elog *t)    { return t->m.last_term; }
uint64_t elog_current_term(const elog *t) { return t->m.current_term; }
uint64_t elog_voted_for(const elog *t)    { return t->m.voted_for; }
uint64_t elog_commit_index(const elog *t) { return t->m.commit_index; }
uint64_t elog_file_len(const elog *t)     { return bjfile_len(&t->f); }

const uint8_t *elog_out(const elog *t, size_t *len) {
    if (len) *len = t->out.len;
    return t->out.data;
}

int elog_term_at(const elog *t, uint64_t index, uint64_t *out_term) {
    if (index < t->base_index || index > t->m.last_index) return BJ_ERR_RANGE;
    *out_term = (index == t->base_index)
        ? t->base_term
        : t->ents[index - t->base_index - 1].term;
    return BJ_OK;
}

/* ---- Appending ------------------------------------------------------- */

int elog_append(elog *t, uint64_t term, int type,
                const uint8_t *payload, uint32_t payload_len,
                uint64_t *out_index) {
    if (type < 0) return BJ_ERR_RANGE;
    /* Terms never move backwards, and no entry may carry a term the durable
     * hard state has not reached (the host must elog_set_hard_state first —
     * a leader acking with an unpersisted term is the classic Raft bug). */
    if (term < t->m.last_term || term > t->m.current_term) return BJ_ERR_STATE;
    if (t->m.last_index - t->base_index >= 0x7fffffff) return BJ_ERR_RANGE;
    int e = ents_reserve(t, t->m.n_ents + 1);
    if (e) return e;
    uint64_t index = t->m.last_index + 1;
    uint64_t off = 0;
    e = encode_entry(t, index, term, type, payload, payload_len, &off);
    if (e) { bjfile_discard(&t->f); t->m = t->cm; return e; }
    el_ent ent = { off, term };
    t->ents[t->m.n_ents++] = ent;
    t->m.last_index = index;
    t->m.last_term = term;
    if (out_index) *out_index = index;
    return BJ_OK;
}

int elog_sync(elog *t) {
    return commit_state(t);
}

int elog_set_hard_state(elog *t, uint64_t term, uint64_t voted_for) {
    if (term < t->m.current_term) return BJ_ERR_STATE;
    if (term == t->m.current_term) {
        if (voted_for != EL_VOTED_NONE &&
            t->m.voted_for != EL_VOTED_NONE && voted_for != t->m.voted_for)
            return BJ_ERR_STATE;   /* one vote per term */
        /* An existing vote cannot be retracted within its term: NONE here
         * means "no new vote to record", not "clear the vote". */
        if (voted_for != EL_VOTED_NONE) t->m.voted_for = voted_for;
    } else {
        t->m.current_term = term;
        t->m.voted_for = voted_for;   /* a new term resets any previous vote */
    }
    return commit_state(t);
}

int elog_set_commit_index(elog *t, uint64_t index) {
    if (index < t->base_index || index > t->m.last_index) return BJ_ERR_RANGE;
    if (index < t->m.commit_index) return BJ_ERR_STATE;  /* never decreases */
    t->m.commit_index = index;   /* staged; rides along with the next sync */
    return BJ_OK;
}

/* ---- Reading --------------------------------------------------------- */

/* Read and parse the live entry holding `index`; validates the stored index
 * matches (an offset-table/record mismatch is corruption, not a range bug). */
static int read_entry(elog *t, uint64_t index, erec *r) {
    const uint8_t *rec; size_t rec_len;
    int e = bjfile_read_record(&t->f, t->ents[index - t->base_index - 1].off,
                               &rec, &rec_len);
    if (e) return e;
    e = parse_record(rec, rec_len, r);
    if (e) return e;
    if (!r->is_entry || r->index != index) return BJ_ERR_VERIFY;
    return BJ_OK;
}

int elog_get(elog *t, uint64_t index, uint64_t *term, int *type,
             const uint8_t **out_ptr, size_t *out_len) {
    if (index <= t->base_index || index > t->m.last_index) return BJ_ERR_RANGE;
    erec r;
    int e = read_entry(t, index, &r);
    if (e) return e;
    e = set_out(t, r.payload, r.payload_len);
    if (e) return e;
    if (term) *term = r.term;
    if (type) *type = r.etype;
    *out_ptr = t->out.data; *out_len = t->out.len;
    return BJ_OK;
}

int elog_get_batch(elog *t, uint64_t from_index, size_t max_bytes, int *count,
                   const uint8_t **out_ptr, size_t *out_len) {
    if (from_index <= t->base_index) return BJ_ERR_RANGE;
    bj_builder *b = t->bld;
    bj_builder_reset(b);
    bj_begin_array(b);
    int n = 0;
    size_t gathered = 0;
    for (uint64_t i = from_index; i <= t->m.last_index; i++) {
        erec r;
        int e = read_entry(t, i, &r);
        if (e) return e;
        bj_begin_object(b);
        bj_put_key(b, (const uint8_t *)"index", 5);   bj_put_int(b, (int64_t)r.index);
        bj_put_key(b, (const uint8_t *)"term", 4);    bj_put_int(b, (int64_t)r.term);
        bj_put_key(b, (const uint8_t *)"type", 4);    bj_put_int(b, r.etype);
        bj_put_key(b, (const uint8_t *)"payload", 7); bj_put_binary(b, r.payload, r.payload_len);
        bj_end_object(b);
        n++;
        gathered += r.payload_len;
        if (gathered >= max_bytes) break;
    }
    bj_end_array(b);
    int e = bj_builder_error(b);
    if (e) return e;
    size_t len;
    const uint8_t *d = bj_builder_data(b, &len);
    if (!d) return BJ_ERR_STATE;
    e = set_out(t, d, len);
    if (e) return e;
    if (count) *count = n;
    *out_ptr = t->out.data; *out_len = t->out.len;
    return BJ_OK;
}

/* ---- Truncation & compaction ----------------------------------------- */

int elog_truncate_from(elog *t, uint64_t index) {
    if (index <= t->base_index || index > t->m.last_index + 1) return BJ_ERR_RANGE;
    if (t->f.wb_len) return BJ_ERR_STATE;   /* sync or lose buffered appends first */
    if (index == t->m.last_index + 1) return BJ_OK;   /* nothing to cut */
    if (index <= t->m.commit_index) return BJ_ERR_STATE;   /* committed entries never conflict */
    t->m.last_index = index - 1;
    t->m.last_term = (index - 1 == t->base_index)
        ? t->base_term
        : t->ents[index - t->base_index - 2].term;
    t->m.n_ents = (int)(index - 1 - t->base_index);
    return commit_state(t);
}

int elog_compact(elog *t, const bj_io *dst,
                 uint64_t new_base_index, uint64_t new_base_term) {
    if (new_base_index < t->base_index || new_base_index > t->m.last_index)
        return BJ_ERR_RANGE;
    if (t->f.wb_len) return BJ_ERR_STATE;
    uint64_t have_term;
    int e = elog_term_at(t, new_base_index, &have_term);
    if (e) return e;
    /* The claimed snapshot boundary must be this log's own entry — a wrong
     * term here would poison every future AppendEntries consistency check. */
    if (have_term != new_base_term) return BJ_ERR_STATE;

    bjfile df;
    e = bjfile_init(&df, dst);
    if (e) return e;
    df.autoflush = 1u << 18;   /* stream to the host in ~256 KB chunks */

    e = bjfile_append_header(&df, t->bld, "entrylog");
    for (uint64_t i = new_base_index + 1; i <= t->m.last_index && !e; i++) {
        const uint8_t *rec; size_t rec_len;
        /* Entry records carry no file offsets, so the bytes copy verbatim. */
        e = bjfile_read_record(&t->f, t->ents[i - t->base_index - 1].off,
                               &rec, &rec_len);
        if (!e) e = bjfile_append(&df, rec, rec_len, NULL);
    }
    if (!e) {
        el_meta m = t->m;
        m.n_ents = (int)(t->m.last_index - new_base_index);
        if (m.commit_index < new_base_index) m.commit_index = new_base_index;
        e = append_metadata(&df, t->bld, new_base_index, new_base_term, &m);
    }
    if (!e) e = bjfile_commit(&df);
    bjfile_dispose(&df);
    return e;
}

/* ---- Integrity ------------------------------------------------------- */

int elog_verify(elog *t) {
    uint64_t prev_term = t->base_term;
    for (int i = 0; i < t->m.n_ents; i++) {
        uint64_t index = t->base_index + 1 + (uint64_t)i;
        erec r;
        int e = read_entry(t, index, &r);   /* checks the stored index too */
        if (e) return e;
        if (r.term < prev_term) return BJ_ERR_VERIFY;   /* terms never fall */
        if (r.term != t->ents[i].term) return BJ_ERR_VERIFY;
        prev_term = r.term;
    }
    if (t->m.last_term != prev_term) return BJ_ERR_VERIFY;
    if (t->m.current_term < t->m.last_term) return BJ_ERR_VERIFY;
    if (t->m.commit_index > t->m.last_index) return BJ_ERR_VERIFY;
    if (t->m.last_index - t->base_index != (uint64_t)t->m.n_ents) return BJ_ERR_VERIFY;
    return BJ_OK;
}
