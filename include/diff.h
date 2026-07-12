/*
 * diff.h — C port of the `diff` (jsdiff) 8.0.3 functions used by
 * src/textlog.js: createPatch, applyPatch and structuredPatch.
 *
 * The goal is byte-for-byte compatibility with jsdiff so that patches produced
 * here are identical to those produced by the JS library (and vice versa),
 * letting the C/WASM TextLog and the JS TextLog interoperate on disk.
 *
 * The three entry points below mirror how textlog.js calls jsdiff:
 *   - diff_create_patch  == createPatch(fileName, a, b)         (INCLUDE_HEADERS,
 *                                                                context = 4)
 *   - diff_apply_patch   == applyPatch(source, patch)           (fuzzFactor 0)
 *   - diff_get_diff      == the getDiff() formatting in textlog.js, built on
 *                           structuredPatch(`version F`, `version T`, a, b, '', '')
 *
 * Each writes a freshly malloc'd buffer through *out / *outlen; the caller owns
 * it and must free() it. Return value is 0 on success or a negative DIFF_ERR_*.
 *
 * Scope note: applyPatch is faithful to jsdiff for fuzzFactor 0 (the only mode
 * textlog uses), including the exact "\ No newline at end of file" handling,
 * best-fit hunk scanning and automatic CRLF/LF conversion. fuzzFactor > 0
 * (fuzzy context matching) is not needed by textlog and is not implemented.
 */
#ifndef DIFF_H
#define DIFF_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define DIFF_OK         0
#define DIFF_ERR_OOM  (-1)
#define DIFF_ERR_PARSE (-2)

/* createPatch(fileName, a, b) -> unified diff string. */
int diff_create_patch(const char *file_name,
                      const uint8_t *a, size_t alen,
                      const uint8_t *b, size_t blen,
                      uint8_t **out, size_t *outlen);

/* textlog.js getDiff(fromVersion, toVersion) formatting over structuredPatch. */
int diff_get_diff(long from_version, long to_version,
                  const uint8_t *a, size_t alen,
                  const uint8_t *b, size_t blen,
                  uint8_t **out, size_t *outlen);

/*
 * applyPatch(source, patch) with fuzzFactor 0. On success *applied is 1 and
 * out/outlen hold the patched text; if the patch does not fit (jsdiff would
 * return false) *applied is 0 and *out is NULL.
 */
int diff_apply_patch(const uint8_t *source, size_t srclen,
                     const uint8_t *patch, size_t patchlen,
                     uint8_t **out, size_t *outlen, int *applied);

/*
 * Binary copy/insert delta (private format; not jsdiff-compatible). Produces a
 * compact byte-level delta that rebuilds `target` from `source` using COPY
 * (offset,len) runs from source and INSERT (literal bytes) runs, greedily
 * matched via a hash index of source K-grams. Typically several times smaller
 * than a unified-diff patch of the same edit — no context lines, hunk headers,
 * or per-line prefixes — and faster to apply (memcpy runs, no line scanning).
 * This is the format TextLog stores for DIFF entries; getDiff still renders a
 * human-readable unified diff by re-diffing the reconstructed texts.
 */
int diff_create_delta(const uint8_t *source, size_t srclen,
                      const uint8_t *target, size_t targetlen,
                      uint8_t **out, size_t *outlen);

/*
 * Apply a diff_create_delta delta to `source`. On success *applied is 1 and
 * out/outlen hold the rebuilt target; on a malformed or out-of-bounds delta
 * *applied is 0 and *out is NULL (mirroring diff_apply_patch's "doesn't fit").
 */
int diff_apply_delta(const uint8_t *source, size_t srclen,
                     const uint8_t *delta, size_t deltalen,
                     uint8_t **out, size_t *outlen, int *applied);

#ifdef __cplusplus
}
#endif

#endif /* DIFF_H */
