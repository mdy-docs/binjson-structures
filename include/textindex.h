/*
 * textindex.h - C port of the full-text index in src/textindex.js.
 *
 * The index is built on three B+ trees (see bplustree.h), exactly like the
 * reference:
 *   - index:           stem      -> postings object { docId: termFreq }
 *   - documentTerms:   docId     -> terms object    { stem: termFreq }
 *   - documentLengths: docId     -> total term count (int)
 * The trees (and their append-only file images) are owned by the host, which
 * passes the three handles into every operation; this file implements the
 * indexing/query logic - tokenization, stop-word filtering, Porter stemming
 * (stemmer.h) and TF-IDF relevance scoring - and reads/writes the tree values
 * as binjson blobs (binjson.h).
 *
 * All keys are strings (docIds and stems), matching how textindex.js is used.
 * Query outputs are returned as freshly malloc'd binjson buffers (caller frees):
 *   - tix_query      -> ARRAY of { id: string, score: number }, sorted by score
 *   - tix_query_all  -> ARRAY of string ids (requireAll intersection)
 *
 * getTermCount / getDocumentCount are just the index / documentTerms sizes
 * (bpt_size), so the host reads those directly.
 *
 * All operations return BJ_OK (0) or a negative BJ_ERR_* code from binjson.h.
 */
#ifndef TEXTINDEX_H
#define TEXTINDEX_H

#include <stdint.h>
#include <stddef.h>

#include "binjson.h"
#include "bplustree.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Index `text` under `doc_id` (both UTF-8). Updates all three trees. */
int tix_add(bpt *index, bpt *doc_terms, bpt *doc_lengths,
            const char *doc_id, int doc_id_len,
            const char *text, int text_len);

/* Remove `doc_id` from the index. Writes 1/0 through *removed. */
int tix_remove(bpt *index, bpt *doc_terms, bpt *doc_lengths,
               const char *doc_id, int doc_id_len, int *removed);

/* Delete every entry from all three trees. */
int tix_clear(bpt *index, bpt *doc_terms, bpt *doc_lengths);

/* Relevance query: ARRAY of { id, score } sorted by descending score. */
int tix_query(bpt *index, bpt *doc_terms, bpt *doc_lengths,
              const char *query, int query_len,
              uint8_t **out, size_t *out_len);

/* requireAll query: ARRAY of string ids present for every query term. */
int tix_query_all(bpt *index, bpt *doc_terms, bpt *doc_lengths,
                  const char *query, int query_len,
                  uint8_t **out, size_t *out_len);

#ifdef __cplusplus
}
#endif

#endif /* TEXTINDEX_H */
