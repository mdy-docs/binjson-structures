/*
 * keyenc.c — see keyenc.h.
 *
 * Moved here from nisaba-db's wasm/src/db_keyenc.c, where it had already
 * grown a hard dependency on this package's bjcursor.h and none at all on
 * anything in nisaba-db. Living next to bplustree.c -- whose header
 * documents the composite-key convention this implements -- also lets
 * this package's own JS wrappers stop carrying a second copy of the
 * encoding.
 *
 * The split between the typed qk_put_number/_string/_date and the
 * binjson-dispatching qk_put_value is what lets both callers share one
 * implementation: the document layer arrives with pre-encoded binjson
 * bytes and uses qk_put_value, while the JS wrapper arrives with a plain
 * JS number or string and uses the typed entry points directly, with no
 * pointless encode-then-decode round trip through binjson.
 */
#include "keyenc.h"
#include "bjcursor.h"

#include <math.h>
#include <string.h>

int qk_put_number(dbuf *out, double v) {
    if (isnan(v)) return BJ_ERR_STATE;   /* NaN has no ordering */
    if (v == 0) v = 0;                   /* normalize -0 to +0 so they encode equal */

    uint64_t bits;
    memcpy(&bits, &v, 8);
    uint8_t enc[9];
    enc[0] = 0x00;
    for (int i = 0; i < 8; i++) enc[1 + i] = (uint8_t)(bits >> (8 * (7 - i)));
    /* Total-order transform: flip the sign bit for positives, all bits for
     * negatives, so unsigned byte order matches numeric order. */
    if (enc[1] & 0x80) { for (int i = 1; i < 9; i++) enc[i] ^= 0xff; }
    else enc[1] ^= 0x80;
    return dbuf_put(out, enc, 9);
}

int qk_put_string(dbuf *out, const uint8_t *utf8, uint32_t len) {
    for (uint32_t i = 0; i < len; i++) {
        if (utf8[i] == 0) return BJ_ERR_STATE;   /* reserved as the terminator */
    }
    uint8_t tag = 0x01;
    int e = dbuf_put(out, &tag, 1);
    if (e) return e;
    e = dbuf_put(out, utf8, len);
    if (e) return e;
    uint8_t term = 0x00;
    return dbuf_put(out, &term, 1);
}

int qk_put_date(dbuf *out, int64_t millis) {
    uint64_t bits = (uint64_t)millis;
    uint8_t enc[9];
    enc[0] = 0x03;
    for (int i = 0; i < 8; i++) enc[1 + i] = (uint8_t)(bits >> (8 * (7 - i)));
    /* The same signed total-order transform as qk_put_number, applied to a
     * plain two's-complement int64 rather than an IEEE-754 bit pattern:
     * flipping just the sign bit turns two's-complement ordering into
     * unsigned byte-order comparison. */
    enc[1] ^= 0x80;
    return dbuf_put(out, enc, 9);
}

int qk_put_value(dbuf *out, const uint8_t *value, size_t value_len) {
    if (value_len < 1) return BJ_ERR_EOF;
    uint8_t type = value[0];

    if (type == BJ_TYPE_INT || type == BJ_TYPE_FLOAT) {
        cur c = { value, value_len, 0 };
        double d;
        int e = read_number(&c, &d);
        if (e) return e;
        return qk_put_number(out, d);
    }

    if (type == BJ_TYPE_DATE) {
        if (value_len != 9) return BJ_ERR_STATE;
        /* Raw LE bytes are the int64 millis-since-epoch, per bj_put_date. */
        return qk_put_date(out, (int64_t)rdu64(value + 1));
    }

    if (type == BJ_TYPE_STRING) {
        cur c = { value, value_len, 0 };
        const uint8_t *sp; uint32_t slen;
        int e = take_string(&c, &sp, &slen);
        if (e) return e;
        return qk_put_string(out, sp, slen);
    }

    return BJ_ERR_STATE;   /* unsupported type for an ordered key part */
}

int qk_put_id(dbuf *out, const uint8_t id[12]) {
    uint8_t tag = 0x02;
    int e = dbuf_put(out, &tag, 1);
    if (e) return e;
    return dbuf_put(out, id, 12);
}

int qk_put_upper_bound(dbuf *out) {
    uint8_t sentinel = 0xff;
    return dbuf_put(out, &sentinel, 1);
}
