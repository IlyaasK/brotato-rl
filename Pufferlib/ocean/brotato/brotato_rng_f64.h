/* Binary64 arithmetic for Godot's global rand_range on devices without FP64.
 * Source initialization bounds can be binary64 literals, not merely floats.
 * Values are normalized 53-bit significands; every operation rounds to nearest,
 * ties to even, like the engine's separate double operations (no FMA).
 * Game ranges stay finite and normal in binary64; bit conversion also handles
 * subnormals. Final binary32 conversion preserves subnormals.
 * Included by brotato_rng.h after its RNG/CLZ definitions. */
#pragma once

typedef struct { bt_u64 sig; int exp; unsigned int sign; } BtRngF64;

BT_HD static BtRngF64 bt_rf64_from_bits(bt_u64 bits) {
    BtRngF64 r;
    r.sign = (unsigned int)(bits >> 63);
    unsigned int e = (unsigned int)((bits >> 52) & 2047u);
    r.sig = bits & (((bt_u64)1 << 52) - 1);
    r.exp = 0;
    if (e) {
        r.sig |= (bt_u64)1 << 52;
        r.exp = (int)e - 1023 - 52;
    } else if (r.sig) {
        r.exp = -1074;
        while (!(r.sig & ((bt_u64)1 << 52))) { r.sig <<= 1; r.exp--; }
    }
    return r;
}

BT_HD static bt_u64 bt_rf64_shr_jam(bt_u64 v, int shift) {
    if (shift <= 0) return v;
    if (shift >= 64) return v ? (bt_u64)1 : (bt_u64)0;
    return (v >> shift) | ((v << (64 - shift)) != 0 ? (bt_u64)1 : (bt_u64)0);
}

BT_HD static bt_u64 bt_rf64_round_shr(bt_u64 v, int shift) {
    if (shift <= 0) return v << (-shift);
    if (shift >= 64) return 0;
    bt_u64 q = v >> shift;
    bt_u64 midpoint = (bt_u64)1 << (shift - 1);
    bt_u64 rem = v & (((bt_u64)1 << shift) - 1);
    return q + ((rem > midpoint || (rem == midpoint && (q & 1))) ? 1 : 0);
}

BT_HD static bt_u64 bt_rf64_to_bits(BtRngF64 r) {
    bt_u64 sign = (bt_u64)r.sign << 63;
    if (!r.sig) return sign;
    int e = r.exp + 52 + 1023;
    if (e <= 0) return sign | bt_rf64_round_shr(r.sig, 1 - e);
    if (e >= 2047) return sign | ((bt_u64)2047 << 52);
    return sign | ((bt_u64)e << 52) | (r.sig & (((bt_u64)1 << 52) - 1));
}

/* Integer-typed GDScript members truncate AFTER the source addition. */
BT_HD static BtRngF64 bt_rf64_trunc(BtRngF64 r) {
    if (!r.sig) { r.sign = 0; return r; }
    if (r.exp >= 0) return r;
    int shift = -r.exp;
    if (shift >= 53) {
        r.sig = 0; r.exp = 0; r.sign = 0;
    } else {
        r.sig = (r.sig >> shift) << shift;
    }
    return r;
}

BT_HD static BtRngF64 bt_rf64_from_float(float f) {
    union { float f; unsigned int u; } bits;
    bits.f = f;
    BtRngF64 r;
    r.sign = bits.u >> 31;
    unsigned int e = (bits.u >> 23) & 255u;
    unsigned int m = bits.u & 0x7fffffu;
    r.sig = 0; r.exp = 0;
    if (!e && !m) return r;
    if (e) {
        r.sig = (bt_u64)(m | 0x800000u) << 29;
        r.exp = (int)e - 127 - 52;
    } else {
        r.exp = -126 - 52;
        while (!(m & 0x800000u)) { m <<= 1; r.exp--; }
        r.sig = (bt_u64)m << 29;
    }
    return r;
}

BT_HD static BtRngF64 bt_rf64_add(BtRngF64 a, BtRngF64 b) {
    if (!a.sig && !b.sig) { a.sign &= b.sign; return a; }
    if (!a.sig) return b;
    if (!b.sig) return a;
    if (a.exp < b.exp) { BtRngF64 t = a; a = b; b = t; }
    bt_u64 x = a.sig << 3;
    bt_u64 y = bt_rf64_shr_jam(b.sig << 3, a.exp - b.exp);
    BtRngF64 r;
    r.exp = a.exp; r.sign = a.sign;
    if (a.sign == b.sign) x += y;
    else if (x >= y) x -= y;
    else { x = y - x; r.sign = b.sign; }
    if (!x) { r.sig = 0; r.exp = 0; r.sign = 0; return r; }
    if (x & ((bt_u64)1 << 56)) { x = bt_rf64_shr_jam(x, 1); r.exp++; }
    while (!(x & ((bt_u64)1 << 55))) { x <<= 1; r.exp--; }
    r.sig = bt_rf64_round_shr(x, 3);
    if (r.sig == ((bt_u64)1 << 53)) { r.sig >>= 1; r.exp++; }
    return r;
}

BT_HD static BtRngF64 bt_rf64_mul(BtRngF64 a, BtRngF64 b) {
    BtRngF64 r;
    r.sign = a.sign ^ b.sign; r.sig = 0; r.exp = 0;
    if (!a.sig || !b.sig) return r;
    bt_u64 a0 = (unsigned int)a.sig, a1 = a.sig >> 32;
    bt_u64 b0 = (unsigned int)b.sig, b1 = b.sig >> 32;
    bt_u64 p00 = a0 * b0, p01 = a0 * b1, p10 = a1 * b0;
    bt_u64 lo = p00, hi = a1 * b1;
    bt_u64 old = lo;
    lo += p01 << 32;
    hi += (p01 >> 32) + (lo < old ? 1 : 0);
    old = lo;
    lo += p10 << 32;
    hi += (p10 >> 32) + (lo < old ? 1 : 0);
    int shift = (hi & ((bt_u64)1 << 41)) ? 53 : 52;
    bt_u64 q = (lo >> shift) | (hi << (64 - shift));
    bt_u64 rem = lo & (((bt_u64)1 << shift) - 1);
    bt_u64 midpoint = (bt_u64)1 << (shift - 1);
    r.sig = q + ((rem > midpoint || (rem == midpoint && (q & 1))) ? 1 : 0);
    r.exp = a.exp + b.exp + shift;
    if (r.sig == ((bt_u64)1 << 53)) { r.sig >>= 1; r.exp++; }
    return r;
}

/* Round-to-nearest-even binary64 division of two normalized significands.
 * The quotient significand is produced by a 53-step restoring long division of
 * a.sig * 2^52 by b.sig (both operands are in [2^52, 2^53), so the unrounded
 * quotient lands in [2^51, 2^53) and needs at most one left shift to normalize),
 * then rounded from the exact remainder.  `b` zero is unreachable on the game
 * paths (all divisors are literal constants such as 100.0) and yields +-inf. */
BT_HD static BtRngF64 bt_rf64_div(BtRngF64 a, BtRngF64 b) {
    BtRngF64 r;
    r.sign = a.sign ^ b.sign; r.sig = 0; r.exp = 0;
    if (!b.sig) {                       /* x/0 -> +-inf, 0/0 -> NaN (both
                                         * unreachable in-game: every divisor is a
                                         * literal constant) */
        r.sig = !a.sig ? ((bt_u64)1 << 51) : ((bt_u64)1 << 52);
        r.exp = 972;                    /* e = 972+52+1023 = 2047 */
        return r;
    }
    if (!a.sig) { return r; }           /* 0/x -> +-0 with the xor sign */
    bt_u64 rem = a.sig, q = 0;
    if (rem >= b.sig) { rem -= b.sig; q = 1; }
    for (int i = 51; i >= 0; i--) {
        rem <<= 1;
        q <<= 1;
        if (rem >= b.sig) { rem -= b.sig; q |= 1; }
    }
    bt_u64 twice = rem << 1;
    if (twice > b.sig || (twice == b.sig && (q & 1))) q++;
    int exp = a.exp - b.exp - 52;
    if (q == ((bt_u64)1 << 53)) { q >>= 1; exp++; }
    while (q && !(q & ((bt_u64)1 << 52))) { q <<= 1; exp--; }
    r.sig = q; r.exp = exp;
    return r;
}

/* a * b rounded to binary64 (the shared shape of every float*float product the
 * source evaluates in double, e.g. `stat * (growth / 100.0)`). */
BT_HD static BtRngF64 bt_rf64_mul_ff(float a, float b) {
    return bt_rf64_mul(bt_rf64_from_float(a), bt_rf64_from_float(b));
}

/* (double)a / (double)b, rounded to binary64: the GDScript `x / y` shape for
 * float-valued operands with a binary64 divisor (x / 100.0). */
BT_HD static BtRngF64 bt_rf64_div_ff(float a, float b) {
    return bt_rf64_div(bt_rf64_from_float(a), bt_rf64_from_float(b));
}

/* 1.0 + pct/100.0 evaluated in binary64 (the source's gain multiplier). */
BT_HD static BtRngF64 bt_rf64_one_plus_pct(float pct) {
    BtRngF64 one; one.sig = (bt_u64)1 << 52; one.exp = -52; one.sign = 0;
    return bt_rf64_add(one, bt_rf64_div_ff(pct, 100.0f));
}

BT_HD static BtRngF64 bt_rf64_randd(BT_DEVICE BtRng* rng) {
    BtRngF64 r; r.sig = 0; r.exp = 0; r.sign = 0;
    unsigned int proto = bt_rng_next(rng);
    if (!proto) return r;
    unsigned int high = bt_rng_next(rng);
    unsigned int low = bt_rng_next(rng);
    bt_u64 raw = (((bt_u64)high << 32) | (bt_u64)low) | (bt_u64)0x8000000000000001;
    r.sig = bt_rf64_round_shr(raw, 11);
    r.exp = 11 - 64 - (int)bt_rng_clz32(proto);
    if (r.sig == ((bt_u64)1 << 53)) { r.sig >>= 1; r.exp++; }
    return r;
}

BT_HD static BtRngF64 bt_rf64_range_bounds(BT_DEVICE BtRng* rng, BtRngF64 a, BtRngF64 b) {
    BtRngF64 negative_a = a;
    negative_a.sign ^= 1u;
    BtRngF64 width = bt_rf64_add(b, negative_a);
    BtRngF64 sampled = bt_rf64_randd(rng);
    return bt_rf64_add(bt_rf64_mul(sampled, width), a);
}

BT_HD static BtRngF64 bt_rf64_range(BT_DEVICE BtRng* rng, float from, float to) {
    return bt_rf64_range_bounds(rng, bt_rf64_from_float(from), bt_rf64_from_float(to));
}

BT_HD static float bt_rf64_to_float(BtRngF64 r) {
    union { float f; unsigned int u; } bits;
    bits.u = r.sign << 31;
    if (!r.sig) return bits.f;
    int e = r.exp + 52;
    bt_u64 q;
    if (e < -126) {
        q = bt_rf64_round_shr(r.sig, -r.exp - 149);
        bits.u |= (unsigned int)q;
    } else {
        q = bt_rf64_round_shr(r.sig, 29);
        if (q == ((bt_u64)1 << 24)) { q >>= 1; e++; }
        if (e > 127) bits.u |= 0x7f800000u;
        else bits.u |= ((unsigned int)(e + 127) << 23) | ((unsigned int)q & 0x7fffffu);
    }
    return bits.f;
}

/* Core integer consumers have representable signed-32-bit results. Truncate the
 * double before binary32 conversion, as GDScript's `as int` does. */
BT_HD static int bt_rf64_to_int(BtRngF64 r) {
    bt_u64 q = r.sig;
    if (r.exp < 0) q = -r.exp >= 64 ? 0 : q >> (-r.exp);
    else q <<= r.exp;
    if (r.sign && q == (bt_u64)0x80000000) return -2147483647 - 1;
    return r.sign ? -(int)q : (int)q;
}
