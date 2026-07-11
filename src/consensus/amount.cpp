// Copyright (c) 2011-2021 The Freicoin Developers
//
// This program is free software: you can redistribute it and/or modify it under
// The terms of version 3 of the GNU Affero General Public License as published
// by the Free Software Foundation.
//
// This program is distributed in the hope that it will be useful, but WITHOUT
// ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
// FOR A PARTICULAR PURPOSE.  See the GNU Affero General Public License for more
// details.
//
// You should have received a copy of the GNU Affero General Public License
// along with this program.  If not, see <https://www.gnu.org/licenses/>.

#include "consensus/amount.h"

#include <tinyformat.h>

#include <algorithm>
#include <array>

/** Only set to true when running the regtest chain with the
 ** '-notimeadjust' option set, making TimeAdjustValueForward() and
 ** TimeAdjustValueBackward() return their inputs unmodified. This
 ** enables running bitcoin regression tests unmodified. */
bool disable_time_adjust = DEFAULT_DISABLE_TIME_ADJUST;

CAmount TimeAdjustValueForward(const CAmount& initial_value, uint32_t distance)
{
    /* If we're in bitcoin unit test compatibility mode, return our
     * input unmodified, with no demurrage adjustment. */
    if (disable_time_adjust)
        return initial_value;

    /* We accept a signed initial_value as input, but perform
     * demurrage calculations on that value's absolute magnitude. */
    const int sign = (initial_value > 0) - (initial_value < 0);
    const uint64_t value = std::abs(initial_value);

    /* The demurrage rate for an offset of 0 blocks, which is 1.0
     * exactly, has no representation in 0.64 fixed point. */
    if (distance == 0)
        return initial_value;
    /* A distance of 2^26 blocks and beyond are sufficient to decay
     * even MAX_MONEY to zero. */
    if (distance >= ((uint32_t)1<<26))
        return 0;

    /* This array of pre-generated constants is an exponentiation
     * ladder of properly calculated 64-bit fixed point demurrage
     * rates for power-of-2 block intervals. Calculating the actual
     * demurrage rate for the passed in distance is a matter of
     * performing fixed point multiplication of the factors
     * corresponding to the powers of 2 (set bits) which make up
     * distance.
     *
     * Our lookup table does not go beyond 26 entries because a
     * distance of 1<<26 (the would-be 27th entry) would cause even
     * MAX_MONEY (2^53 - 1) to decay to zero. If we are given a
     * distance value greater than or equal to (1<<26), we simply
     * return 0LL. */
    static std::array<uint32_t, 2*26> k32 = {
        0xfffff000, 0x00000000, /* 2^0 = 1 */
        0xffffe000, 0x01000000, /* 2^1 = 2 */
        0xffffc000, 0x05ffffc0, /* 2^2 = 4 */
        0xffff8000, 0x1bfffc80, /* 2^3 = 8 */
        0xffff0000, 0x77ffdd00, /* ... */
        0xfffe0001, 0xeffeca00,
        0xfffc0007, 0xdff5d409,
        0xfff8001f, 0xbfaca8a2,
        0xfff0007f, 0x7d5d5a6a,
        0xffe001fe, 0xeacb48a8,
        0xffc007fd, 0x55dfda2a,
        0xff801ff6, 0xad5499cd,
        0xff007fcd, 0x67f98aad,
        0xfe01fe9b, 0x74f0943e,
        0xfc07f540, 0x767d2a82,
        0xf81fab16, 0x3dc15990,
        0xf07d5f65, 0xf9604ac9,
        0xe1eb5045, 0x80b6ebf7,
        0xc75f7b66, 0xa5075def,
        0x9b459576, 0x663bbb3e,
        0x5e2d55e7, 0x48e27ab4,
        0x22a5531d, 0x29a95916,
        0x04b054d7, 0xfda49c4d,
        0x0015fc1b, 0x85085be9,
        0x000001e3, 0x54ca043c,
        0x00000000, 0x00039089
    };

    /* Overflow sensitive fixed point multiply-and-accumulate methods
     * which are used both for the exponentiation to calcuate the
     * demurrage rate, and the final multiply at the end. */
    uint64_t sum=0, overflow=0;
    auto shift32 = [&]() {
        sum = (overflow << 32) + (sum >> 32);
        overflow = 0;
    };
    auto term = [&](uint64_t val) {
        overflow += (sum + val) < sum;
        sum += val;
    };

    /* We calculate the first 64 fractional bits of the aggregate
     * demurrage rate over distance blocks by raising the per-block
     * rate of (1 - 2^-20) to the distance'th power. To perform this
     * computation efficiently we perform N multiplications out of a
     * pre-computed exponentiation ladder, where N is the number of
     * set bits in the binary representation of distance. */

    /* At the end of this calculation w will contain the first 64
     * fractional bits of the demurrage rate as a pair of 32-bit
     * unsigned words, the most significant word first. Its initial
     * value is multiplicative identity, 1.0, for which the fractional
     * bits are zero. */
    std::array<uint32_t, 2> w = { 0, 0 };

    /* The first multiplication has the accumulator set to 1.0, which
     * is the only time it has a value >= 1. Since we don't store the
     * non-fractional bits, we need to special-case the first
     * multiplication. */
    bool first = true;

    for (int bit = 0; distance; distance >>= 1, ++bit) {
        if (distance & 1) {
            /* The first time through the accumulator has the value
             * 1.0.  Multiplication by 1.0 is easy--just copy the
             * value from the table. */
            if (first) {
                first = false;
                w[0] = k32[2*bit];
                w[1] = k32[2*bit+1];
                continue;
            }

            /* Zero-extend the accumulator state and ladder entry as
             * we will be doing our multiplications in 64-bit to
             * capture to full range. */
            const uint64_t w0 = w[0];
            const uint64_t w1 = w[1];
            const uint64_t k0 = k32[2*bit];
            const uint64_t k1 = k32[2*bit+1];

            /* Carry out the multiplication, term-by-term. Terms whose
             * contribution to the final result are entirely wiped
             * away by truncation are not included. */

            overflow = 0;
            sum= k1 * w0;
            term(k0 * w1);
            shift32();
            term(k0 * w0);
            w[1] = static_cast<uint32_t>(sum);
            shift32();

            w[0] = static_cast<uint32_t>(sum);

            /* Debugging check: under no circumstances should it ever
             * be the case that the high-order bits of sum (including
             * detected overflow) are non-zero. That would indicate
             * that the multiplication resulted in a value of 1.0 or
             * greater, which shouldn't be possile. */
            // shift32();
            // assert(sum == 0);
        }
    }

    /* We now perform an approximately similar multiplication of the
     * final calculated demurrage factor by the passed in value. */
    const uint64_t v0 = value >> 32;
    const uint64_t v1 = static_cast<uint32_t>(value);

    overflow = 0;
    sum = (w[1] * v1) >> 32;
    term(w[1] * v0);
    term(w[0] * v1);
    shift32();
    term(w[0] * v0);

    /* Debugging check: having the overflow bit set at this point
     * would indicate that the demurrage calculation has resulted in
     * an amount that is greater than std::numeric_limits<int64_t>::
     * max(), which should never be possible as the demurrage factor
     * should always be a fractional number less than one. */
    // assert(overflow == 0);

    /* Debugging check: the semantics of time-adjustment are that it
     * never returns a value with magnitude outside the range of [0,
     * MAX_MONEY]. But the only way this could happen is if we are
     * passed a value greater than MAX_MONEY, which is outside the
     * domain of this function. So we don't need to check for this in
     * production */
    // assert(result <= MAX_MONEY);

    return sign * CAmount(static_cast<int64_t>(sum));
}

/* nVersion=3-lite: per-asset demurrage at rate 2^-k per block. Structurally identical to
 * TimeAdjustValueForward, except the exponentiation ladder of (1 - 2^-k)^(2^bit) is generated
 * on the fly (the host currency's k32 table is only for k=20). The ladder MUST be generated
 * with >=96 fractional guard bits: naive 64-bit squaring drifts a few ULPs and does not match
 * the canonical k=20 table. Proof + golden vectors: research/nversion3/. */
CAmount TimeAdjustValueForwardK(const CAmount& initial_value, uint32_t distance, unsigned k)
{
    if (disable_time_adjust)
        return initial_value;
    if (k == 20)
        return TimeAdjustValueForward(initial_value, distance);   /* host currency: canonical path */
    if (distance == 0)
        return initial_value;
    if (distance >= ((uint32_t)1 << 26))
        return 0;

    const int sign = (initial_value > 0) - (initial_value < 0);
    const uint64_t value = std::abs(initial_value);

    /* Build the 64-bit-fraction ladder for shift k, squaring the base (1 - 2^-k) with 96
     * guard bits. The exact (a*a) >> 96 for a < 2^96 is done by 64-bit split to avoid the
     * 192-bit intermediate overflowing __int128. */
    typedef unsigned __int128 u128;
    std::array<uint32_t, 2*26> lk;
    {
        const int P = 96;
        u128 c = ((u128)1 << P) - ((u128)1 << (P - (int)k));
        for (int bit = 0; bit < 26; ++bit) {
            uint64_t e = (uint64_t)(c >> (P - 64));
            lk[2*bit]   = (uint32_t)(e >> 32);
            lk[2*bit+1] = (uint32_t)e;
            uint64_t aH = (uint64_t)(c >> 64), aL = (uint64_t)c;
            u128 X = (u128)2 * aH * aL;                 /* < 2^97 */
            u128 Y = (u128)aL * aL;                     /* < 2^128 */
            c = ((u128)aH * aH << 32) + (X >> 32) + (((( X & 0xffffffffULL) << 64) + Y) >> 96);
        }
    }

    /* Identical accumulation + final multiply to TimeAdjustValueForward, using lk. */
    uint64_t sum = 0, overflow = 0;
    auto shift32 = [&]() { sum = (overflow << 32) + (sum >> 32); overflow = 0; };
    auto term = [&](uint64_t val) { overflow += (sum + val) < sum; sum += val; };

    std::array<uint32_t, 2> w = { 0, 0 };
    bool first = true;
    for (int bit = 0; distance; distance >>= 1, ++bit) {
        if (distance & 1) {
            if (first) { first = false; w[0] = lk[2*bit]; w[1] = lk[2*bit+1]; continue; }
            const uint64_t w0 = w[0], w1 = w[1], k0 = lk[2*bit], k1 = lk[2*bit+1];
            overflow = 0; sum = k1 * w0; term(k0 * w1); shift32(); term(k0 * w0);
            w[1] = static_cast<uint32_t>(sum); shift32(); w[0] = static_cast<uint32_t>(sum);
        }
    }

    const uint64_t v0 = value >> 32, v1 = static_cast<uint32_t>(value);
    overflow = 0; sum = (w[1] * v1) >> 32; term(w[1] * v0); term(w[0] * v1); shift32(); term(w[0] * v0);
    return sign * CAmount(static_cast<int64_t>(sum));
}

/* nVersion=3-lite: INTEREST (a growing bond) at rate (1 + 2^-k) per block, the mirror image
 * of per-asset demurrage. The factor is computed in 64.64 fixed point by square-and-multiply
 * with truncation after every multiply — the EXACT operation sequence of the reference model
 * (freicoin-wallet core/assets.mjs interestPV), so the two agree bit-for-bit. The factor is
 * unbounded but amounts are not: the present value SATURATES at MAX_MONEY, with the running
 * product and the squared base capped the moment their integer part reaches 2^53−1. Products
 * of two 117-bit fixed-point numbers are formed from 64-bit halves so no intermediate
 * overflows the 128-bit accumulator. */
CAmount TimeAdjustValueForwardInterestK(const CAmount& initial_value, uint32_t distance, unsigned k)
{
    if (disable_time_adjust)
        return initial_value;
    if (distance == 0 || initial_value == 0)
        return initial_value;

    const int sign = (initial_value > 0) - (initial_value < 0);
    const uint64_t value = std::abs(initial_value);

    typedef unsigned __int128 u128;
    const int P = 64;
    const u128 CAP = (u128)MAX_MONEY << P;   /* factor cap: integer part = MAX_MONEY */

    /* (a*b) >> 64 for a,b < 2^117, saturated at CAP. Split into 64-bit halves:
     * (a*b)>>64 = (aH*bH)<<64 + aH*bL + aL*bH + ((aL*bL)>>64); each addend < 2^117,
     * so the sum < 2^119 fits u128. If aH*bH alone reaches 2^53 the result is >= CAP. */
    auto mulshift = [&](u128 a, u128 b) -> u128 {
        const uint64_t aH = (uint64_t)(a >> P), aL = (uint64_t)a;
        const uint64_t bH = (uint64_t)(b >> P), bL = (uint64_t)b;
        const u128 hh = (u128)aH * bH;
        if (hh >> 53) return CAP;
        const u128 r = (hh << P) + (u128)aH * bL + (u128)aL * bH + (((u128)aL * bL) >> P);
        return r >= CAP ? CAP : r;
    };

    u128 acc = (u128)1 << P;
    u128 base = ((u128)1 << P) + ((u128)1 << (P - (int)k));
    uint32_t e = distance;
    while (e > 0) {
        if (e & 1) {
            acc = mulshift(acc, base);
            if (acc >= CAP) return sign * MAX_MONEY;
        }
        e >>= 1;
        if (e > 0) base = mulshift(base, base);   /* saturates at CAP internally */
    }

    /* pv = (value * acc) >> 64 with value < 2^53: value*accH < 2^106, fits exactly. */
    const u128 pv = (u128)value * (uint64_t)(acc >> P) + (((u128)value * (uint64_t)acc) >> P);
    return sign * (pv > (u128)MAX_MONEY ? MAX_MONEY : (CAmount)(int64_t)pv);
}

CAmount TimeAdjustValueReverse(const CAmount& initial_value, uint32_t distance)
{
    /* If we're in bitcoin unit test compatibility mode, return our
     * input unmodified, with no demurrage adjustment. */
    if (disable_time_adjust)
        return initial_value;

    /* We accept a signed initial_value as input, but perform
     * demurrage calculations on that value's absolute magnitude. */
    const int sign = (initial_value > 0) - (initial_value < 0);
    const uint64_t value = std::abs(initial_value);

    /* Later on we might return +/- MAX_MONEY in cases of overflow.
     * The one instance in which this is incorrect behavior is when
     * the input value is zero, so we must handle that as a special
     * case first. */
    if (value == 0)
        return 0;

    /* A distance of 2^26 blocks and beyond are sufficient to decay
     * even MAX_MONEY to zero going forward, which in reverse implies
     * a single kria would exceed MAX_MONEY. */
    const CAmount kOverflow(sign * MAX_MONEY);
    if (distance >= ((uint32_t)1<<26))
        return kOverflow;

    /* These arrays of pre-generated constants are an exponentiation
     * ladder of properly calculated 64.64-bit fixed point inverse
     * demurrage factors for power-of-2 block intervals. Calculating
     * the the aggregate inverse demurrage factor for the given
     * distance is a matter of performing fixed point multiplication
     * of the factors corresponding to the powers of 2 (set bits) in
     * the binary representation of distance.
     *
     * Our lookup table does not go beyond 26 entries because a
     * distance of 2^26 blocks (the would-be 27th entry) would cause
     * any input value (except zero) to overflow. */
    static std::array<uint32_t, 4*26> k32 = {
        0x00000000, 0x00000001, 0x00001000, 0x01000010, /* -2^0 = -1 */
        0x00000000, 0x00000001, 0x00002000, 0x03000040, /* -2^1 = -2 */
        0x00000000, 0x00000001, 0x00004000, 0x0a000140, /* -2^2 = -4 */
        0x00000000, 0x00000001, 0x00008000, 0x24000780, /* -2^3 = -8 */
        0x00000000, 0x00000001, 0x00010000, 0x88003300, /* ... */
        0x00000000, 0x00000001, 0x00020002, 0x10017600,
        0x00000000, 0x00000001, 0x00040008, 0x200b2c0b,
        0x00000000, 0x00000001, 0x00080020, 0x405758b2,
        0x00000000, 0x00000001, 0x00100080, 0x82b2baeb,
        0x00000000, 0x00000001, 0x00200201, 0x15760cb0,
        0x00000000, 0x00000001, 0x00400802, 0xab357b3b,
        0x00000000, 0x00000001, 0x00802009, 0x5800bbef,
        0x00000000, 0x00000001, 0x01008032, 0xbd5bcef3,
        0x00000000, 0x00000001, 0x02020166, 0x20651cee,
        0x00000000, 0x00000001, 0x04080ad5, 0xdee644e3,
        0x00000000, 0x00000001, 0x08205643, 0x1a97126a,
        0x00000000, 0x00000001, 0x1082b600, 0x14af6333,
        0x00000000, 0x00000001, 0x2216057d, 0x856dd258,
        0x00000000, 0x00000001, 0x48b5e655, 0x53fde431,
        0x00000000, 0x00000001, 0xa6129f7a, 0x2b20cd20,
        0x00000000, 0x00000002, 0xb7e16721, 0x96b730c5,
        0x00000000, 0x00000007, 0x6399a46e, 0xd2eda481,
        0x00000000, 0x00000036, 0x99272f73, 0x36391a9f,
        0x00000000, 0x00000ba4, 0xf827e152, 0x14cd8421,
        0x00000000, 0x008797a2, 0x510309b9, 0xc64e0d7e,
        0x000047d1, 0x470253b0, 0x78e38992, 0x14983b4b };

    /* Overflow sensitive fixed point multiply-and-accumulate methods
     * which are used both for the exponentiation to calcuate the
     * aggregate demurrage rate, and the final multiply at the end. */
    uint64_t sum=0, overflow=0;
    auto shift32 = [&]() {
        sum = (overflow << 32) + (sum >> 32);
        overflow = 0;
    };
    auto term = [&](uint64_t val) {
        overflow += (sum + val) < sum;
        sum += val;
    };

    /* We calculate the aggregate inverse demurrage factor for
     * distance by raising the per-block rate of 1/(1 - 2^-20) to the
     * distance'th power. To perform this computation efficiently we
     * perform N multiplications of a pre-computed exponentiation
     * ladder, where N is the number of set bits in the binary
     * representation of distance. */

    /* At the end of this calculation the 64.64-bit fixed-point number
     * w will contain a representation of the demurrage or inverse
     * demurrage rate as a quad of 32-bit unsigned words, the most
     * significant word first. Its initial value is multiplicative
     * identity, 1.0, for which the fractional bits are zero. */
    std::array<uint32_t, 4> w = { 0, 1, 0, 0 };

    /* The first multiplication has the accumulator set to 1.0. So as
     * an optimization we don't need to perform a term-by-term
     * multiplication, but can instead just copy the factor into the
     * accumulator. */
    bool first = true;

    for (int bit = 0; distance; distance >>= 1, ++bit) {
        if (distance & 1) {
            /* w contains the multiplicative identity, so the first
             * time a bit is set we simply copy the relevant factor
             * into the accumulator. */
            if (first) {
                first = false;
                w[0] = k32[4*bit];
                w[1] = k32[4*bit+1];
                w[2] = k32[4*bit+2];
                w[3] = k32[4*bit+3];
                continue;
            }

            /* Zero-extend the accumulator state and ladder entry as
             * we will be doing our multiplications in 64-bit to
             * capture full range and detect overflow. */
            const uint64_t w0 = w[0];
            const uint64_t w1 = w[1];
            const uint64_t w2 = w[2];
            const uint64_t w3 = w[3];
            const uint64_t k0 = k32[4*bit];
            const uint64_t k1 = k32[4*bit+1];
            const uint64_t k2 = k32[4*bit+2];
            const uint64_t k3 = k32[4*bit+3];

            /* Carry out the multiplication, term-by-term. Terms which
             * have no consistently detectible contribution to the
             * final result due to truncation are not included. */

            overflow = 0;
            sum= k3 * w2;
            term(k2 * w3);
            shift32();

            term(k3 * w1);
            term(k2 * w2);
            term(k1 * w3);
            w[3] = static_cast<uint32_t>(sum);
            shift32();

            if (bit == 25) {
                term(k3 * w0);
                term(k0 * w3);
            }
            term(k2 * w1);
            term(k1 * w2);
            w[2] = static_cast<uint32_t>(sum);
            shift32();

            if (bit == 25) {
                term(k2 * w0);
                term(k0 * w2);
            }
            term(k1 * w1);
            w[1] = static_cast<uint32_t>(sum);
            shift32();

            if (bit == 25) {
                term(k1 * w0);
                term(k0 * w1);
            }
            w[0] = static_cast<uint32_t>(sum);

            /* The above calculation can only possibly overflow on the
             * very last run through the loop. If there was overflow
             * the output would necessarily exceed MAX_MONEY and be
             * clamped so there is no need to proceed further. */
            if (bit == 25) {
                shift32();
                if (sum || w0) {
                    return kOverflow;
                }
            }
        }
    }

    /* Now we multiply the original value by the inverse demurrage
     * factor, in much the same way the fixed point calculations were
     * performed above, but with fewer terms since value has no
     * fractional component. */
    const uint64_t v0 = value >> 32;
    const uint64_t v1 = static_cast<uint32_t>(value);

    overflow = 0;
    sum = (v1 * w[3]) >> 32;

    term(v1 * w[2]);
    term(v0 * w[3]);
    shift32();

    term(v1 * w[1]);
    term(v0 * w[2]);
    const uint64_t r1 = static_cast<uint32_t>(sum);
    shift32();

    term(v1 * w[0]);
    term(v0 * w[1]);
    const uint64_t r0 = static_cast<uint32_t>(sum);
    shift32();

    /* The final term represents bits 65-128. If this term is
     * non-zero, or if shift32 left any residual value or overflow, we
     * know we have exceeded our range. */
    if (sum || (v0 && w[0])) {
        return kOverflow;
    }

    /* Finally we return our calculated result, clamped to never be
     * more than MAX_MONEY. */
    CAmount result((static_cast<int64_t>(r0) << 32) | static_cast<int64_t>(r1));
    if (result > MAX_MONEY) {
        return kOverflow;
    }

    return sign * result;
}

CAmount GetTimeAdjustedValue(const CAmount& initial_value, int relative_depth)
{
    if (relative_depth < 0)
        return TimeAdjustValueReverse(initial_value, std::abs(relative_depth));
    else
        return TimeAdjustValueForward(initial_value, relative_depth);
}

static const int32_t SCRIP_EPOCH = 5040000L;

CAmount FreicoinToScrip(const CAmount& freicoin, uint32_t height)
{
    return GetTimeAdjustedValue(freicoin, SCRIP_EPOCH - static_cast<int32_t>(height));
}

CAmount ScripToFreicoin(const CAmount& epoch_value, uint32_t height)
{
    return GetTimeAdjustedValue(epoch_value, static_cast<int32_t>(height) - SCRIP_EPOCH);
}

// End of File
