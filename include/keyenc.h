/*
 * keyenc.h — order-preserving byte encoding for B+ tree composite /
 * secondary-index keys.
 *
 * See bplustree.h's key-convention note for the rationale: the tree is
 * unique-key by design, so a secondary index encodes the indexed value(s)
 * followed by the primary key into one composite byte string, and all
 * entries sharing a value form a contiguous range retrieved with a single
 * range/cursor scan.
 *
 * This is the ONE implementation of that convention. It used to exist
 * twice -- once here in C (as nisaba-db's wasm/src/db_keyenc.c) and once
 * in JavaScript as this package's own orderedKey/compositeKey/
 * compositeUpperBound -- which meant two encoders that had to agree
 * byte-for-byte forever, with nothing enforcing it. The JS versions are
 * now thin marshalling over these functions (keyenc_wasm.c,
 * wasm/structures-core.js).
 *
 * Wire shape of one encoded key: call qk_put_value (or the typed
 * qk_put_number/_string/_date it dispatches to) once per indexed field in
 * index order, then exactly one qk_put_id:
 *   - number:    0x00 + 8-byte sign-normalized big-endian IEEE-754 double
 *   - string:    0x01 + UTF-8 bytes + a 0x00 terminator (so a string part
 *                must not itself contain U+0000)
 *   - id suffix: 0x02 + the 12 raw ObjectId bytes verbatim
 *   - date:      0x03 + 8-byte sign-normalized big-endian int64 millis
 *                since epoch (the same sign-bit flip as number, applied to
 *                a plain signed integer rather than an IEEE-754 pattern)
 *
 * Every tag byte is < 0xff and every part is self-delimiting, so parts
 * concatenate unambiguously and qk_put_upper_bound's single 0xff sentinel
 * is guaranteed to sort after every real key sharing the same value
 * prefix -- regardless of what follows, and in particular regardless of
 * the arbitrary bytes of an id suffix. That last point is why the id
 * suffix is tagged at all: untagged raw id bytes would occasionally
 * corrupt a range-scan upper bound, since an ObjectId's first byte can
 * itself be 0xff.
 *
 * Only number (INT and FLOAT are one domain, matching how JS treats every
 * number uniformly), string and date have an encoding here. Any other
 * value type is BJ_ERR_STATE; nisaba-db's db.c translates that to its own
 * DC_ERR_UNINDEXABLE_VALUE at the call site. Widening the domain (OID, a
 * full BSON-like total order across types) is future work.
 */
#ifndef KEYENC_H
#define KEYENC_H

#include <stdint.h>
#include <stddef.h>

#include "binjson.h"
#include "dbuf.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Typed parts. Each appends one complete, self-delimiting part to `out`.
 *
 * qk_put_number: BJ_ERR_STATE for NaN, which has no ordering. -0 is
 *   normalized to +0 so the two encode identically (they compare equal).
 * qk_put_string: BJ_ERR_STATE if the bytes contain U+0000, which is
 *   reserved as the terminator.
 */
int qk_put_number(dbuf *out, double v);
int qk_put_string(dbuf *out, const uint8_t *utf8, uint32_t len);
int qk_put_date(dbuf *out, int64_t millis);

/*
 * Append the encoding of one binjson-encoded scalar value -- `value`/
 * `value_len` spans exactly one type byte plus payload, e.g. as produced
 * by bjcursor.h's skip_value. Dispatches to the typed functions above on
 * the binjson type byte; BJ_ERR_STATE for any other type.
 */
int qk_put_value(dbuf *out, const uint8_t *value, size_t value_len);

/* Append the id-suffix encoding of a 12-byte ObjectId -- always the last
 * part of a composite key, and the tree's row reference. */
int qk_put_id(dbuf *out, const uint8_t id[12]);

/*
 * Append the exclusive upper-bound sentinel (0xff) for range-scanning
 * every composite key that shares the value parts already written to
 * `out`. Call this on a copy of the value-parts-only prefix, i.e. before
 * any qk_put_id, not on a buffer that already carries an id suffix.
 */
int qk_put_upper_bound(dbuf *out);

#ifdef __cplusplus
}
#endif

#endif /* KEYENC_H */
