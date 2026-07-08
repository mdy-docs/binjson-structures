/*
 * stemmer.h — C port of the `stemmer` npm package (v2.0.1), i.e. the Porter
 * stemming algorithm as implemented by Titus Wormer.
 *
 * This is a byte-for-byte port of that specific implementation (not Martin
 * Porter's canonical C), so results match the JS package exactly for ASCII
 * word input - see the conformance harness. The measure (m) and the "v" and "o"
 * consonant-vowel conditions are computed directly rather than via regexes.
 *
 * Input is treated as ASCII: bytes A-Z are lowercased; other bytes are passed
 * through and classified as consonants (matching how the JS regex character
 * classes treat non-vowel characters). Non-ASCII / Unicode casing is out of
 * scope, matching how textindex tokenizes (lowercase ASCII word tokens).
 */
#ifndef STEMMER_H
#define STEMMER_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Write the Porter stem of the `len`-byte word at `word` into `out`, which must
 * have capacity >= len + 2 (the stem is never longer than the input, plus a NUL
 * terminator). Returns the stem length (not counting the NUL).
 */
int stemmer_stem(const char *word, int len, char *out);

#ifdef __cplusplus
}
#endif

#endif /* STEMMER_H */
