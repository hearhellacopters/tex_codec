/*
 * astc.cpp - ASTC codec module (see codec_common.h for the contract).
 *
 * Decoder: full LDR-profile decode for all 14 2D block sizes, following the
 * Khronos ASTC specification (Khronos Data Format spec / OpenGL ES 3.2
 * Annex C): block-mode field decode, void-extent blocks, BISE integer
 * sequence encoding with exact trit/quint packing, colour endpoint modes
 * 0-15 including the HDR modes (2/3/7/11/14/15), exact colour and weight
 * unquantization, hash52-based partition selection, dual-plane support and
 * the integer bilinear weight-grid infill.  HDR blocks are decoded through
 * the logarithmic (LNS) interpolation path and reconstructed as fp16.
 *
 * Encoder: always-valid deterministic bitstream.  Uniform blocks become LDR
 * void-extent blocks; everything else uses a single partition, no dual
 * plane, CEM 12 (LDR RGBA direct) with a per-block-size weight grid /
 * quantization configuration chosen so all fields fit in 128 bits.
 */

#include "codec_common.h"

#include <math.h>

namespace texc {
namespace {

/* ------------------------------------------------------------ geometry -- */

static bool astc_dims(texc_format fmt, uint32_t &bw, uint32_t &bh) {
    switch (fmt) {
        case TEXC_FORMAT_ASTC_4x4:   bw = 4;  bh = 4;  return true;
        case TEXC_FORMAT_ASTC_5x4:   bw = 5;  bh = 4;  return true;
        case TEXC_FORMAT_ASTC_5x5:   bw = 5;  bh = 5;  return true;
        case TEXC_FORMAT_ASTC_6x5:   bw = 6;  bh = 5;  return true;
        case TEXC_FORMAT_ASTC_6x6:   bw = 6;  bh = 6;  return true;
        case TEXC_FORMAT_ASTC_8x5:   bw = 8;  bh = 5;  return true;
        case TEXC_FORMAT_ASTC_8x6:   bw = 8;  bh = 6;  return true;
        case TEXC_FORMAT_ASTC_8x8:   bw = 8;  bh = 8;  return true;
        case TEXC_FORMAT_ASTC_10x5:  bw = 10; bh = 5;  return true;
        case TEXC_FORMAT_ASTC_10x6:  bw = 10; bh = 6;  return true;
        case TEXC_FORMAT_ASTC_10x8:  bw = 10; bh = 8;  return true;
        case TEXC_FORMAT_ASTC_10x10: bw = 10; bh = 10; return true;
        case TEXC_FORMAT_ASTC_12x10: bw = 12; bh = 10; return true;
        case TEXC_FORMAT_ASTC_12x12: bw = 12; bh = 12; return true;
        default: return false;
    }
}

/* -------------------------------------------------------- 128-bit block -- */
/* Bit 0 = LSB of byte 0 (ASTC little-endian bit numbering).                */

struct Block128 {
    uint64_t w[2];

    Block128() { w[0] = 0; w[1] = 0; }
    explicit Block128(const uint8_t *src) {
        w[0] = 0; w[1] = 0;
        for (int i = 0; i < 8; i++) {
            w[0] |= (uint64_t)src[i] << (8 * i);
            w[1] |= (uint64_t)src[8 + i] << (8 * i);
        }
    }
    void store(uint8_t *dst) const {
        for (int i = 0; i < 8; i++) {
            dst[i]     = (uint8_t)(w[0] >> (8 * i));
            dst[8 + i] = (uint8_t)(w[1] >> (8 * i));
        }
    }
    uint32_t bit(int pos) const {
        return (uint32_t)((w[pos >> 6] >> (pos & 63)) & 1u);
    }
    /* Read `count` (0..32) bits starting at `pos` (LSB-first). */
    uint32_t bits(int pos, int count) const {
        uint32_t r = 0;
        for (int i = 0; i < count; i++)
            r |= bit(pos + i) << i;
        return r;
    }
    void set_bit(int pos) {
        w[pos >> 6] |= (uint64_t)1 << (pos & 63);
    }
    void set_bits(int pos, int count, uint64_t val) {
        for (int i = 0; i < count; i++)
            if ((val >> i) & 1) set_bit(pos + i);
    }
};

static Block128 reverse128(const Block128 &b) {
    Block128 r;
    for (int i = 0; i < 128; i++)
        if (b.bit(i)) r.set_bit(127 - i);
    return r;
}

/* Forward bit stream over a Block128; bits at/after `len` read as zero
 * (the sequence-truncation rule of the spec's ISE). */
struct BitReader {
    const Block128 *b;
    int start, len, pos;
    BitReader(const Block128 &blk, int start_, int len_)
        : b(&blk), start(start_), len(len_), pos(0) {}
    uint32_t next(int n) {
        uint32_t r = 0;
        for (int i = 0; i < n; i++, pos++)
            if (pos < len) r |= b->bit(start + pos) << i;
        return r;
    }
};

struct BitWriter {
    Block128 *b;
    int pos;
    BitWriter(Block128 &blk, int start) : b(&blk), pos(start) {}
    void put(uint32_t v, int n) {
        for (int i = 0; i < n; i++, pos++)
            if ((v >> i) & 1) b->set_bit(pos);
    }
};

/* Replicate an n-bit value into `to` bits (spec bit replication). */
static uint32_t rep_bits(uint32_t v, int from, int to) {
    uint32_t dst = 0;
    for (int shift = to - from; shift > -from; shift -= from)
        dst |= shift >= 0 ? (v << shift) : (v >> -shift);
    return dst;
}

static int32_t sign_extend_i(int32_t v, int bits) {
    if (v & (1 << (bits - 1)))
        v |= ~((1 << bits) - 1);
    return v;
}

/* ------------------------------------------------ integer sequence (BISE) */

enum IseMode { ISE_BITS = 0, ISE_TRIT = 1, ISE_QUINT = 2 };

struct IseParams {
    IseMode mode;
    int bits;
};

static int ise_levels(const IseParams &p) {
    switch (p.mode) {
        case ISE_TRIT:  return 3 << p.bits;
        case ISE_QUINT: return 5 << p.bits;
        default:        return 1 << p.bits;
    }
}

static int ise_bit_count(const IseParams &p, int nvals) {
    switch (p.mode) {
        case ISE_TRIT:  return (nvals * 8 + 4) / 5 + nvals * p.bits;
        case ISE_QUINT: return (nvals * 7 + 2) / 3 + nvals * p.bits;
        default:        return nvals * p.bits;
    }
}

/* All quantization steps in descending range order (QUANT_256 .. QUANT_2).
 * l100 = round(100 * log2(levels)), used by the encoder's config search.   */
struct QuantDesc { IseMode mode; int bits; int levels; int l100; };
static const QuantDesc k_quant_desc[21] = {
    { ISE_BITS,  8, 256, 800 }, { ISE_TRIT,  6, 192, 758 },
    { ISE_QUINT, 5, 160, 732 }, { ISE_BITS,  7, 128, 700 },
    { ISE_TRIT,  5,  96, 658 }, { ISE_QUINT, 4,  80, 632 },
    { ISE_BITS,  6,  64, 600 }, { ISE_TRIT,  4,  48, 558 },
    { ISE_QUINT, 3,  40, 532 }, { ISE_BITS,  5,  32, 500 },
    { ISE_TRIT,  3,  24, 458 }, { ISE_QUINT, 2,  20, 432 },
    { ISE_BITS,  4,  16, 400 }, { ISE_TRIT,  2,  12, 358 },
    { ISE_QUINT, 1,  10, 332 }, { ISE_BITS,  3,   8, 300 },
    { ISE_TRIT,  1,   6, 258 }, { ISE_QUINT, 0,   5, 232 },
    { ISE_BITS,  2,   4, 200 }, { ISE_TRIT,  0,   3, 158 },
    { ISE_BITS,  1,   2, 100 }
};

/* Largest quantization whose ISE encoding of `nvals` values fits in
 * `avail` bits (identical to the spec's colour-quantization selection).   */
static IseParams max_quant_for_bits(int avail, int nvals) {
    for (int i = 0; i < 21; i++) {
        IseParams p = { k_quant_desc[i].mode, k_quant_desc[i].bits };
        if (ise_bit_count(p, nvals) <= avail)
            return p;
    }
    IseParams p = { ISE_BITS, 1 };
    return p;
}

/* Trit/quint block decode per the spec pseudocode (C.2.12), plus minimal
 * encodings for the packer (lowest T/Q for each trit/quint combination:
 * their truncated high bits are zero, as required for partial groups).    */
static void decode_trit_block(uint32_t T, uint8_t t[5]) {
    uint32_t C;
    if (((T >> 2) & 7) == 7) {
        C = (((T >> 5) & 7) << 2) | (T & 3);
        t[3] = 2; t[4] = 2;
    } else {
        C = T & 0x1F;
        if (((T >> 5) & 3) == 3) {
            t[4] = 2; t[3] = (uint8_t)((T >> 7) & 1);
        } else {
            t[4] = (uint8_t)((T >> 7) & 1); t[3] = (uint8_t)((T >> 5) & 3);
        }
    }
    if ((C & 3) == 3) {
        t[2] = 2;
        t[1] = (uint8_t)((C >> 4) & 1);
        t[0] = (uint8_t)((((C >> 3) & 1) << 1) | ((C >> 2) & 1 & (((C >> 3) & 1) ^ 1)));
    } else if (((C >> 2) & 3) == 3) {
        t[2] = 2; t[1] = 2;
        t[0] = (uint8_t)(C & 3);
    } else {
        t[2] = (uint8_t)((C >> 4) & 1);
        t[1] = (uint8_t)((C >> 2) & 3);
        t[0] = (uint8_t)((((C >> 1) & 1) << 1) | (C & 1 & (((C >> 1) & 1) ^ 1)));
    }
}

static void decode_quint_block(uint32_t Q, uint8_t q[3]) {
    if (((Q >> 1) & 3) == 3 && ((Q >> 5) & 3) == 0) {
        uint32_t q0 = Q & 1;
        q[2] = (uint8_t)((q0 << 2) |
                         ((((Q >> 4) & 1) & (q0 ^ 1)) << 1) |
                         (((Q >> 3) & 1) & (q0 ^ 1)));
        q[1] = 4; q[0] = 4;
    } else {
        uint32_t C;
        if (((Q >> 1) & 3) == 3) {
            q[2] = 4;
            C = (((Q >> 3) & 3) << 3) | (((~(Q >> 5)) & 3) << 1) | (Q & 1);
        } else {
            q[2] = (uint8_t)((Q >> 5) & 3);
            C = Q & 0x1F;
        }
        if ((C & 7) == 5) {
            q[1] = 4; q[0] = (uint8_t)((C >> 3) & 3);
        } else {
            q[1] = (uint8_t)((C >> 3) & 3); q[0] = (uint8_t)(C & 7);
        }
    }
}

struct IseTables {
    uint8_t trits[256][5];
    uint8_t quints[128][3];
    int16_t trit_enc[243];   /* key = t0 + 3*t1 + 9*t2 + 27*t3 + 81*t4 */
    int16_t quint_enc[125];  /* key = q0 + 5*q1 + 25*q2 */
    IseTables() {
        for (int i = 0; i < 243; i++) trit_enc[i] = -1;
        for (int i = 0; i < 125; i++) quint_enc[i] = -1;
        for (uint32_t T = 0; T < 256; T++) {
            decode_trit_block(T, trits[T]);
            int key = trits[T][0] + 3 * trits[T][1] + 9 * trits[T][2] +
                      27 * trits[T][3] + 81 * trits[T][4];
            if (trit_enc[key] < 0) trit_enc[key] = (int16_t)T;
        }
        for (uint32_t Q = 0; Q < 128; Q++) {
            decode_quint_block(Q, quints[Q]);
            int key = quints[Q][0] + 5 * quints[Q][1] + 25 * quints[Q][2];
            if (quint_enc[key] < 0) quint_enc[key] = (int16_t)Q;
        }
    }
};

static const IseTables &ise_tables() {
    static const IseTables t;
    return t;
}

struct IseVal {
    uint8_t m;   /* low bits                    */
    uint8_t tq;  /* trit or quint value         */
    uint8_t v;   /* full value (tq << bits) | m */
};

static void decode_ise(IseVal *dst, int nvals, BitReader &br, const IseParams &p) {
    const IseTables &tab = ise_tables();
    if (p.mode == ISE_TRIT) {
        for (int base = 0; base < nvals; base += 5) {
            uint32_t m[5], t01, t23, t4, t56, t7;
            m[0] = br.next(p.bits); t01 = br.next(2);
            m[1] = br.next(p.bits); t23 = br.next(2);
            m[2] = br.next(p.bits); t4  = br.next(1);
            m[3] = br.next(p.bits); t56 = br.next(2);
            m[4] = br.next(p.bits); t7  = br.next(1);
            uint32_t T = (t7 << 7) | (t56 << 5) | (t4 << 4) | (t23 << 2) | t01;
            int k = nvals - base < 5 ? nvals - base : 5;
            for (int i = 0; i < k; i++) {
                dst[base + i].m  = (uint8_t)m[i];
                dst[base + i].tq = tab.trits[T][i];
                dst[base + i].v  = (uint8_t)((tab.trits[T][i] << p.bits) + m[i]);
            }
        }
    } else if (p.mode == ISE_QUINT) {
        for (int base = 0; base < nvals; base += 3) {
            uint32_t m[3], q012, q34, q56;
            m[0] = br.next(p.bits); q012 = br.next(3);
            m[1] = br.next(p.bits); q34  = br.next(2);
            m[2] = br.next(p.bits); q56  = br.next(2);
            uint32_t Q = (q56 << 5) | (q34 << 3) | q012;
            int k = nvals - base < 3 ? nvals - base : 3;
            for (int i = 0; i < k; i++) {
                dst[base + i].m  = (uint8_t)m[i];
                dst[base + i].tq = tab.quints[Q][i];
                dst[base + i].v  = (uint8_t)((tab.quints[Q][i] << p.bits) + m[i]);
            }
        }
    } else {
        for (int i = 0; i < nvals; i++) {
            uint32_t m = br.next(p.bits);
            dst[i].m = (uint8_t)m;
            dst[i].tq = 0;
            dst[i].v = (uint8_t)m;
        }
    }
}

/* Exact inverse of decode_ise; writes only the bits that belong to the
 * sequence (partial trailing trit/quint groups are truncated per spec).   */
static void encode_ise(Block128 &blk, int start, const uint8_t *vals, int nvals,
                       const IseParams &p) {
    const IseTables &tab = ise_tables();
    BitWriter bw(blk, start);
    uint32_t mask = (uint32_t)((1u << p.bits) - 1);
    if (p.mode == ISE_TRIT) {
        for (int base = 0; base < nvals; base += 5) {
            int k = nvals - base < 5 ? nvals - base : 5;
            uint32_t m[5] = { 0, 0, 0, 0, 0 };
            int key = 0, pw = 1;
            for (int i = 0; i < 5; i++) {
                uint32_t v = i < k ? vals[base + i] : 0;
                m[i] = v & mask;
                key += (int)(v >> p.bits) * pw;
                pw *= 3;
            }
            uint32_t T = (uint32_t)tab.trit_enc[key];
            bw.put(m[0], p.bits); bw.put(T & 3, 2);
            if (k > 1) { bw.put(m[1], p.bits); bw.put((T >> 2) & 3, 2); }
            if (k > 2) { bw.put(m[2], p.bits); bw.put((T >> 4) & 1, 1); }
            if (k > 3) { bw.put(m[3], p.bits); bw.put((T >> 5) & 3, 2); }
            if (k > 4) { bw.put(m[4], p.bits); bw.put((T >> 7) & 1, 1); }
        }
    } else if (p.mode == ISE_QUINT) {
        for (int base = 0; base < nvals; base += 3) {
            int k = nvals - base < 3 ? nvals - base : 3;
            uint32_t m[3] = { 0, 0, 0 };
            int key = 0, pw = 1;
            for (int i = 0; i < 3; i++) {
                uint32_t v = i < k ? vals[base + i] : 0;
                m[i] = v & mask;
                key += (int)(v >> p.bits) * pw;
                pw *= 5;
            }
            uint32_t Q = (uint32_t)tab.quint_enc[key];
            bw.put(m[0], p.bits); bw.put(Q & 7, 3);
            if (k > 1) { bw.put(m[1], p.bits); bw.put((Q >> 3) & 3, 2); }
            if (k > 2) { bw.put(m[2], p.bits); bw.put((Q >> 5) & 3, 2); }
        }
    } else {
        for (int i = 0; i < nvals; i++)
            bw.put(vals[i], p.bits);
    }
}

/* --------------------------------------------------------- unquantization */

/* Weight unquantization to [0,64] (spec C.2.17). */
static uint32_t unquant_weight(uint32_t m, uint32_t tq, uint32_t v,
                               const IseParams &p) {
    uint32_t w;
    if (p.mode == ISE_BITS) {
        w = rep_bits(v, p.bits, 6);
    } else if (p.bits == 0) {
        static const uint32_t map0[3] = { 0, 32, 63 };
        static const uint32_t map1[5] = { 0, 16, 32, 47, 63 };
        w = p.mode == ISE_TRIT ? map0[tq] : map1[tq];
    } else {
        uint32_t a = m & 1, b = (m >> 1) & 1, c = (m >> 2) & 1;
        uint32_t A = a == 0 ? 0 : 0x7F;
        uint32_t B = 0, C = 0;
        if (p.mode == ISE_TRIT) {
            if (p.bits == 1)      { C = 50; B = 0; }
            else if (p.bits == 2) { C = 23; B = (b << 6) | (b << 2) | b; }
            else                  { C = 11; B = (c << 6) | (b << 5) | (c << 1) | b; }
        } else {
            if (p.bits == 1)      { C = 28; B = 0; }
            else                  { C = 13; B = (b << 6) | (b << 1); }
        }
        w = (((tq * C + B) ^ A) >> 2) | (A & 0x20);
    }
    if (w > 32) w += 1;
    return w;
}

/* Colour endpoint unquantization to [0,255] (spec C.2.13). */
static uint32_t unquant_color(uint32_t m, uint32_t tq, uint32_t v,
                              const IseParams &p) {
    if (p.mode == ISE_BITS)
        return rep_bits(v, p.bits, 8);
    int range_case = p.bits * 2 - (p.mode == ISE_TRIT ? 2 : 1);
    if (range_case < 0 || range_case > 10)
        return 0; /* not reachable for valid blocks */
    static const uint32_t Ca[11] = { 204, 113, 93, 54, 44, 26, 22, 13, 11, 6, 5 };
    uint32_t C = Ca[range_case];
    uint32_t a = m & 1, b = (m >> 1) & 1, c = (m >> 2) & 1;
    uint32_t d = (m >> 3) & 1, e = (m >> 4) & 1, f = (m >> 5) & 1;
    uint32_t A = a == 0 ? 0 : 0x1FF;
    uint32_t B = 0;
    switch (range_case) {
        case 0: case 1: B = 0; break;
        case 2:  B = (b << 8) | (b << 4) | (b << 2) | (b << 1); break;
        case 3:  B = (b << 8) | (b << 3) | (b << 2); break;
        case 4:  B = (c << 8) | (b << 7) | (c << 3) | (b << 2) | (c << 1) | b; break;
        case 5:  B = (c << 8) | (b << 7) | (c << 2) | (b << 1) | c; break;
        case 6:  B = (d << 8) | (c << 7) | (b << 6) | (d << 2) | (c << 1) | b; break;
        case 7:  B = (d << 8) | (c << 7) | (b << 6) | (d << 1) | c; break;
        case 8:  B = (e << 8) | (d << 7) | (c << 6) | (b << 5) | (e << 1) | d; break;
        case 9:  B = (e << 8) | (d << 7) | (c << 6) | (b << 5) | e; break;
        default: B = (f << 8) | (e << 7) | (d << 6) | (c << 5) | (b << 4) | f; break;
    }
    return (((tq * C + B) ^ A) >> 2) | (A & 0x80);
}

/* ---------------------------------------------------- block mode decode -- */

struct BlockMode {
    bool error;
    bool void_extent;
    bool dual;
    int gw, gh;          /* weight grid dimensions */
    IseParams wparams;   /* weight ISE parameters  */
};

static BlockMode decode_block_mode(uint32_t d) {
    BlockMode m;
    m.error = false; m.void_extent = false; m.dual = false;
    m.gw = 0; m.gh = 0;
    m.wparams.mode = ISE_BITS; m.wparams.bits = 0;

    if ((d & 0x1FF) == 0x1FC) { m.void_extent = true; return m; }
    if (((d & 3) == 0 && ((d >> 6) & 7) == 7) || (d & 0xF) == 0) {
        m.error = true;
        return m;
    }
    uint32_t r;
    if ((d & 3) == 0) {
        uint32_t r0 = (d >> 4) & 1, r1 = (d >> 2) & 1, r2 = (d >> 3) & 1;
        uint32_t i78 = (d >> 7) & 3;
        r = (r2 << 2) | (r1 << 1) | r0;
        if (i78 == 3) {
            bool i5 = ((d >> 5) & 1) != 0;
            m.gw = i5 ? 10 : 6;
            m.gh = i5 ? 6 : 10;
        } else {
            uint32_t a = (d >> 5) & 3;
            if (i78 == 0)      { m.gw = 12;                      m.gh = (int)a + 2; }
            else if (i78 == 1) { m.gw = (int)a + 2;              m.gh = 12; }
            else               { m.gw = (int)a + 6;              m.gh = (int)((d >> 9) & 3) + 6; }
        }
    } else {
        uint32_t r0 = (d >> 4) & 1, r1 = d & 1, r2 = (d >> 1) & 1;
        uint32_t i23 = (d >> 2) & 3, a = (d >> 5) & 3;
        r = (r2 << 2) | (r1 << 1) | r0;
        if (i23 == 3) {
            uint32_t b = (d >> 7) & 1;
            bool i8 = ((d >> 8) & 1) != 0;
            m.gw = i8 ? (int)b + 2 : (int)a + 2;
            m.gh = i8 ? (int)a + 2 : (int)b + 6;
        } else {
            uint32_t b = (d >> 7) & 3;
            if (i23 == 0)      { m.gw = (int)b + 4; m.gh = (int)a + 2; }
            else if (i23 == 1) { m.gw = (int)b + 8; m.gh = (int)a + 2; }
            else               { m.gw = (int)a + 2; m.gh = (int)b + 8; }
        }
    }
    bool zero_dh = (d & 3) == 0 && ((d >> 7) & 3) == 2;
    bool h  = zero_dh ? false : (((d >> 9) & 1) != 0);
    m.dual  = zero_dh ? false : (((d >> 10) & 1) != 0);
    static const IseParams lo[6] = {
        { ISE_BITS, 1 }, { ISE_TRIT, 0 }, { ISE_BITS, 2 },
        { ISE_QUINT, 0 }, { ISE_TRIT, 1 }, { ISE_BITS, 3 }
    };
    static const IseParams hi[6] = {
        { ISE_QUINT, 1 }, { ISE_TRIT, 2 }, { ISE_BITS, 4 },
        { ISE_QUINT, 2 }, { ISE_TRIT, 3 }, { ISE_BITS, 5 }
    };
    m.wparams = h ? hi[r - 2] : lo[r - 2];
    return m;
}

/* -------------------------------------------------- colour endpoints ---- */

struct Endpoints {
    int32_t e0[4], e1[4];  /* 8-bit LDR or 12-bit HDR components */
    bool rgb_hdr, a_hdr;
};

static int cem_value_count(uint32_t cem) {
    return ((int)(cem >> 2) + 1) * 2;
}

static void bit_transfer_signed(int32_t &a, int32_t &b) {
    b >>= 1;
    b |= a & 0x80;
    a >>= 1;
    a &= 0x3F;
    if (a & 0x20) a -= 0x40;
}

static int32_t clamp12(int32_t v) { return v < 0 ? 0 : (v > 0xFFF ? 0xFFF : v); }
static int32_t clamp255(int32_t v) { return v < 0 ? 0 : (v > 0xFF ? 0xFF : v); }

static void set4(int32_t *d, int32_t x, int32_t y, int32_t z, int32_t w) {
    d[0] = x; d[1] = y; d[2] = z; d[3] = w;
}
static void set4c(int32_t *d, int32_t x, int32_t y, int32_t z, int32_t w) {
    d[0] = clamp255(x); d[1] = clamp255(y); d[2] = clamp255(z); d[3] = clamp255(w);
}
/* blue-contraction, then clamp to 0..255 */
static void set4bc(int32_t *d, int32_t r, int32_t g, int32_t b, int32_t a) {
    d[0] = clamp255((r + b) >> 1); d[1] = clamp255((g + b) >> 1);
    d[2] = clamp255(b); d[3] = clamp255(a);
}

static void decode_hdr_mode7(Endpoints &ep, uint32_t v0, uint32_t v1,
                             uint32_t v2, uint32_t v3) {
    uint32_t m10 = ((v1 >> 7) & 1) | (((v2 >> 7) & 1) << 1);
    uint32_t m23 = (v0 >> 6) & 3;
    uint32_t majcomp = m10 != 3 ? m10 : (m23 != 3 ? m23 : 0);
    uint32_t mode    = m10 != 3 ? m23 : (m23 != 3 ? 4 : 5);
    int32_t R = (int32_t)(v0 & 0x3F);
    int32_t G = (int32_t)(v1 & 0x1F);
    int32_t B = (int32_t)(v2 & 0x1F);
    int32_t S = (int32_t)(v3 & 0x1F);
    int32_t x0 = (v1 >> 6) & 1, x1 = (v1 >> 5) & 1, x2 = (v2 >> 6) & 1;
    int32_t x3 = (v2 >> 5) & 1, x4 = (v3 >> 7) & 1, x5 = (v3 >> 6) & 1;
    int32_t x6 = (v3 >> 5) & 1;
    switch (mode) {
        case 0: R |= x0 << 9; R |= x1 << 8; R |= x2 << 7; R |= x3 << 10;
                R |= x4 << 6; S |= x5 << 6; S |= x6 << 5; break;
        case 1: R |= x0 << 8; G |= x1 << 5; R |= x2 << 7; B |= x3 << 5;
                R |= x4 << 6; R |= x5 << 10; R |= x6 << 9; break;
        case 2: R |= x0 << 9; R |= x1 << 8; R |= x2 << 7; R |= x3 << 6;
                S |= x4 << 7; S |= x5 << 6; S |= x6 << 5; break;
        case 3: R |= x0 << 8; G |= x1 << 5; R |= x2 << 7; B |= x3 << 5;
                R |= x4 << 6; S |= x5 << 6; S |= x6 << 5; break;
        case 4: G |= x0 << 6; G |= x1 << 5; B |= x2 << 6; B |= x3 << 5;
                R |= x4 << 6; R |= x5 << 7; S |= x6 << 5; break;
        default: G |= x0 << 6; G |= x1 << 5; B |= x2 << 6; B |= x3 << 5;
                R |= x4 << 6; S |= x5 << 6; S |= x6 << 5; break;
    }
    static const int shamt[6] = { 1, 1, 2, 3, 4, 5 };
    R <<= shamt[mode]; G <<= shamt[mode]; B <<= shamt[mode]; S <<= shamt[mode];
    if (mode != 5) { G = R - G; B = R - B; }
    if (majcomp == 1)      { int32_t t = R; R = G; G = t; }
    else if (majcomp == 2) { int32_t t = R; R = B; B = t; }
    set4(ep.e0, clamp12(R - S), clamp12(G - S), clamp12(B - S), 0x780);
    set4(ep.e1, clamp12(R),     clamp12(G),     clamp12(B),     0x780);
}

static void decode_hdr_mode11(Endpoints &ep, uint32_t v0, uint32_t v1,
                              uint32_t v2, uint32_t v3, uint32_t v4,
                              uint32_t v5) {
    uint32_t major = (((v5 >> 7) & 1) << 1) | ((v4 >> 7) & 1);
    if (major == 3) {
        set4(ep.e0, (int32_t)(v0 << 4), (int32_t)(v2 << 4),
             (int32_t)((v4 & 0x7F) << 5), 0x780);
        set4(ep.e1, (int32_t)(v1 << 4), (int32_t)(v3 << 4),
             (int32_t)((v5 & 0x7F) << 5), 0x780);
        return;
    }
    uint32_t mode = (((v3 >> 7) & 1) << 2) | (((v2 >> 7) & 1) << 1) | ((v1 >> 7) & 1);
    int32_t a  = (int32_t)((((v1 >> 6) & 1) << 8) | v0);
    int32_t c  = (int32_t)(v1 & 0x3F);
    int32_t b0 = (int32_t)(v2 & 0x3F);
    int32_t b1 = (int32_t)(v3 & 0x3F);
    int32_t d0 = (int32_t)(v4 & 0x1F);
    int32_t d1 = (int32_t)(v5 & 0x1F);
    int32_t x0 = (v2 >> 6) & 1, x1 = (v3 >> 6) & 1, x2 = (v4 >> 6) & 1;
    int32_t x3 = (v5 >> 6) & 1, x4 = (v4 >> 5) & 1, x5 = (v5 >> 5) & 1;
    switch (mode) {
        case 0: b0 |= x0 << 6; b1 |= x1 << 6; d0 |= x2 << 6; d1 |= x3 << 6;
                d0 |= x4 << 5; d1 |= x5 << 5; break;
        case 1: b0 |= x0 << 6; b1 |= x1 << 6; b0 |= x2 << 7; b1 |= x3 << 7;
                d0 |= x4 << 5; d1 |= x5 << 5; break;
        case 2: a |= x0 << 9;  c |= x1 << 6;  d0 |= x2 << 6; d1 |= x3 << 6;
                d0 |= x4 << 5; d1 |= x5 << 5; break;
        case 3: b0 |= x0 << 6; b1 |= x1 << 6; a |= x2 << 9;  c |= x3 << 6;
                d0 |= x4 << 5; d1 |= x5 << 5; break;
        case 4: b0 |= x0 << 6; b1 |= x1 << 6; b0 |= x2 << 7; b1 |= x3 << 7;
                a |= x4 << 9;  a |= x5 << 10; break;
        case 5: a |= x0 << 9;  a |= x1 << 10; c |= x2 << 7;  c |= x3 << 6;
                d0 |= x4 << 5; d1 |= x5 << 5; break;
        case 6: b0 |= x0 << 6; b1 |= x1 << 6; a |= x2 << 11; c |= x3 << 6;
                a |= x4 << 9;  a |= x5 << 10; break;
        default: a |= x0 << 9; a |= x1 << 10; a |= x2 << 11; c |= x3 << 6;
                d0 |= x4 << 5; d1 |= x5 << 5; break;
    }
    static const int ndb[8] = { 7, 6, 7, 6, 5, 6, 5, 6 };
    d0 = sign_extend_i(d0, ndb[mode]);
    d1 = sign_extend_i(d1, ndb[mode]);
    int sh = (int)((mode >> 1) ^ 3);
    a <<= sh; c <<= sh; b0 <<= sh; b1 <<= sh; d0 <<= sh; d1 <<= sh;
    set4(ep.e0, clamp12(a - c), clamp12(a - b0 - c - d0),
         clamp12(a - b1 - c - d1), 0x780);
    set4(ep.e1, clamp12(a), clamp12(a - b0), clamp12(a - b1), 0x780);
    if (major == 1) {
        int32_t t = ep.e0[0]; ep.e0[0] = ep.e0[1]; ep.e0[1] = t;
        t = ep.e1[0]; ep.e1[0] = ep.e1[1]; ep.e1[1] = t;
    } else if (major == 2) {
        int32_t t = ep.e0[0]; ep.e0[0] = ep.e0[2]; ep.e0[2] = t;
        t = ep.e1[0]; ep.e1[0] = ep.e1[2]; ep.e1[2] = t;
    }
}

static void decode_hdr_mode15(Endpoints &ep, uint32_t v0, uint32_t v1,
                              uint32_t v2, uint32_t v3, uint32_t v4,
                              uint32_t v5, uint32_t v6in, uint32_t v7in) {
    decode_hdr_mode11(ep, v0, v1, v2, v3, v4, v5);
    uint32_t mode = (((v7in >> 7) & 1) << 1) | ((v6in >> 7) & 1);
    int32_t v6 = (int32_t)(v6in & 0x7F);
    int32_t v7 = (int32_t)(v7in & 0x7F);
    if (mode == 3) {
        ep.e0[3] = v6 << 5;
        ep.e1[3] = v7 << 5;
    } else {
        v6 |= (v7 << (mode + 1)) & 0x780;
        v7 &= 0x3F >> mode;
        v7 ^= 0x20 >> mode;
        v7 -= 0x20 >> mode;
        v6 <<= 4 - (int)mode;
        v7 <<= 4 - (int)mode;
        v7 += v6;
        v7 = clamp12(v7);
        ep.e0[3] = v6;
        ep.e1[3] = v7;
    }
}

/* Decode one partition's endpoints from its unquantized values (0..255). */
static void decode_endpoints(Endpoints &ep, uint32_t cem, const uint32_t *v) {
    ep.rgb_hdr = false;
    ep.a_hdr = false;
    switch (cem) {
        case 0:  /* LDR luminance direct */
            set4(ep.e0, (int32_t)v[0], (int32_t)v[0], (int32_t)v[0], 0xFF);
            set4(ep.e1, (int32_t)v[1], (int32_t)v[1], (int32_t)v[1], 0xFF);
            break;
        case 1: { /* LDR luminance base+offset */
            int32_t L0 = (int32_t)((v[0] >> 2) | (v[1] & 0xC0));
            int32_t L1 = L0 + (int32_t)(v[1] & 0x3F);
            if (L1 > 0xFF) L1 = 0xFF;
            set4(ep.e0, L0, L0, L0, 0xFF);
            set4(ep.e1, L1, L1, L1, 0xFF);
            break;
        }
        case 2: { /* HDR luminance, large range */
            uint32_t y0, y1;
            if (v[1] >= v[0]) { y0 = v[0] << 4; y1 = v[1] << 4; }
            else              { y0 = (v[1] << 4) + 8; y1 = (v[0] << 4) - 8; }
            set4(ep.e0, (int32_t)y0, (int32_t)y0, (int32_t)y0, 0x780);
            set4(ep.e1, (int32_t)y1, (int32_t)y1, (int32_t)y1, 0x780);
            ep.rgb_hdr = ep.a_hdr = true;
            break;
        }
        case 3: { /* HDR luminance, small range */
            uint32_t y0, d;
            if (v[0] & 0x80) {
                y0 = (((v[1] >> 5) & 7) << 9) | ((v[0] & 0x7F) << 2);
                d  = (v[1] & 0x1F) << 2;
            } else {
                y0 = (((v[1] >> 4) & 0xF) << 8) | ((v[0] & 0x7F) << 1);
                d  = (v[1] & 0x0F) << 1;
            }
            uint32_t y1 = y0 + d;
            if (y1 > 0xFFF) y1 = 0xFFF;
            set4(ep.e0, (int32_t)y0, (int32_t)y0, (int32_t)y0, 0x780);
            set4(ep.e1, (int32_t)y1, (int32_t)y1, (int32_t)y1, 0x780);
            ep.rgb_hdr = ep.a_hdr = true;
            break;
        }
        case 4:  /* LDR luminance + alpha direct */
            set4(ep.e0, (int32_t)v[0], (int32_t)v[0], (int32_t)v[0], (int32_t)v[2]);
            set4(ep.e1, (int32_t)v[1], (int32_t)v[1], (int32_t)v[1], (int32_t)v[3]);
            break;
        case 5: { /* LDR luminance + alpha, base+offset */
            int32_t v0 = (int32_t)v[0], v1 = (int32_t)v[1];
            int32_t v2 = (int32_t)v[2], v3 = (int32_t)v[3];
            bit_transfer_signed(v1, v0);
            bit_transfer_signed(v3, v2);
            set4c(ep.e0, v0, v0, v0, v2);
            set4c(ep.e1, v0 + v1, v0 + v1, v0 + v1, v2 + v3);
            break;
        }
        case 6:  /* LDR RGB scale */
            set4(ep.e0, (int32_t)((v[0] * v[3]) >> 8), (int32_t)((v[1] * v[3]) >> 8),
                 (int32_t)((v[2] * v[3]) >> 8), 0xFF);
            set4(ep.e1, (int32_t)v[0], (int32_t)v[1], (int32_t)v[2], 0xFF);
            break;
        case 7:  /* HDR RGB, base+scale */
            decode_hdr_mode7(ep, v[0], v[1], v[2], v[3]);
            ep.rgb_hdr = ep.a_hdr = true;
            break;
        case 8:  /* LDR RGB direct */
            if (v[1] + v[3] + v[5] >= v[0] + v[2] + v[4]) {
                set4(ep.e0, (int32_t)v[0], (int32_t)v[2], (int32_t)v[4], 0xFF);
                set4(ep.e1, (int32_t)v[1], (int32_t)v[3], (int32_t)v[5], 0xFF);
            } else {
                set4bc(ep.e0, (int32_t)v[1], (int32_t)v[3], (int32_t)v[5], 0xFF);
                set4bc(ep.e1, (int32_t)v[0], (int32_t)v[2], (int32_t)v[4], 0xFF);
            }
            break;
        case 9: { /* LDR RGB base+offset */
            int32_t v0 = (int32_t)v[0], v1 = (int32_t)v[1];
            int32_t v2 = (int32_t)v[2], v3 = (int32_t)v[3];
            int32_t v4 = (int32_t)v[4], v5 = (int32_t)v[5];
            bit_transfer_signed(v1, v0);
            bit_transfer_signed(v3, v2);
            bit_transfer_signed(v5, v4);
            if (v1 + v3 + v5 >= 0) {
                set4c(ep.e0, v0, v2, v4, 0xFF);
                set4c(ep.e1, v0 + v1, v2 + v3, v4 + v5, 0xFF);
            } else {
                set4bc(ep.e0, v0 + v1, v2 + v3, v4 + v5, 0xFF);
                set4bc(ep.e1, v0, v2, v4, 0xFF);
            }
            break;
        }
        case 10: /* LDR RGB scale + alpha */
            set4(ep.e0, (int32_t)((v[0] * v[3]) >> 8), (int32_t)((v[1] * v[3]) >> 8),
                 (int32_t)((v[2] * v[3]) >> 8), (int32_t)v[4]);
            set4(ep.e1, (int32_t)v[0], (int32_t)v[1], (int32_t)v[2], (int32_t)v[5]);
            break;
        case 11: /* HDR RGB direct */
            decode_hdr_mode11(ep, v[0], v[1], v[2], v[3], v[4], v[5]);
            ep.rgb_hdr = ep.a_hdr = true;
            break;
        case 12: /* LDR RGBA direct */
            if (v[1] + v[3] + v[5] >= v[0] + v[2] + v[4]) {
                set4(ep.e0, (int32_t)v[0], (int32_t)v[2], (int32_t)v[4], (int32_t)v[6]);
                set4(ep.e1, (int32_t)v[1], (int32_t)v[3], (int32_t)v[5], (int32_t)v[7]);
            } else {
                set4bc(ep.e0, (int32_t)v[1], (int32_t)v[3], (int32_t)v[5], (int32_t)v[7]);
                set4bc(ep.e1, (int32_t)v[0], (int32_t)v[2], (int32_t)v[4], (int32_t)v[6]);
            }
            break;
        case 13: { /* LDR RGBA base+offset */
            int32_t v0 = (int32_t)v[0], v1 = (int32_t)v[1];
            int32_t v2 = (int32_t)v[2], v3 = (int32_t)v[3];
            int32_t v4 = (int32_t)v[4], v5 = (int32_t)v[5];
            int32_t v6 = (int32_t)v[6], v7 = (int32_t)v[7];
            bit_transfer_signed(v1, v0);
            bit_transfer_signed(v3, v2);
            bit_transfer_signed(v5, v4);
            bit_transfer_signed(v7, v6);
            if (v1 + v3 + v5 >= 0) {
                set4c(ep.e0, v0, v2, v4, v6);
                set4c(ep.e1, v0 + v1, v2 + v3, v4 + v5, v6 + v7);
            } else {
                set4bc(ep.e0, v0 + v1, v2 + v3, v4 + v5, v6 + v7);
                set4bc(ep.e1, v0, v2, v4, v6);
            }
            break;
        }
        case 14: /* HDR RGB + LDR alpha */
            decode_hdr_mode11(ep, v[0], v[1], v[2], v[3], v[4], v[5]);
            ep.e0[3] = (int32_t)v[6];
            ep.e1[3] = (int32_t)v[7];
            ep.rgb_hdr = true;
            ep.a_hdr = false;
            break;
        default: /* 15: HDR RGB + HDR alpha */
            decode_hdr_mode15(ep, v[0], v[1], v[2], v[3], v[4], v[5], v[6], v[7]);
            ep.rgb_hdr = ep.a_hdr = true;
            break;
    }
}

/* ------------------------------------------------------ partition select */

static uint32_t hash52(uint32_t p) {
    p ^= p >> 15;  p -= p << 17;  p += p << 7;   p += p << 4;
    p ^= p >> 5;   p += p << 16;  p ^= p >> 7;   p ^= p >> 3;
    p ^= p << 6;   p ^= p >> 17;
    return p;
}

static int select_partition(uint32_t seed_in, int x_in, int y_in,
                            int npart, bool small_block) {
    uint32_t x = (uint32_t)(small_block ? x_in << 1 : x_in);
    uint32_t y = (uint32_t)(small_block ? y_in << 1 : y_in);
    uint32_t z = 0;
    uint32_t seed = seed_in + 1024u * (uint32_t)(npart - 1);
    uint32_t rnum = hash52(seed);
    uint8_t s1  = (uint8_t)( rnum        & 0xF);
    uint8_t s2  = (uint8_t)((rnum >>  4) & 0xF);
    uint8_t s3  = (uint8_t)((rnum >>  8) & 0xF);
    uint8_t s4  = (uint8_t)((rnum >> 12) & 0xF);
    uint8_t s5  = (uint8_t)((rnum >> 16) & 0xF);
    uint8_t s6  = (uint8_t)((rnum >> 20) & 0xF);
    uint8_t s7  = (uint8_t)((rnum >> 24) & 0xF);
    uint8_t s8  = (uint8_t)((rnum >> 28) & 0xF);
    uint8_t s9  = (uint8_t)((rnum >> 18) & 0xF);
    uint8_t s10 = (uint8_t)((rnum >> 22) & 0xF);
    uint8_t s11 = (uint8_t)((rnum >> 26) & 0xF);
    uint8_t s12 = (uint8_t)(((rnum >> 30) | (rnum << 2)) & 0xF);
    s1  = (uint8_t)(s1  * s1);   s2  = (uint8_t)(s2  * s2);
    s3  = (uint8_t)(s3  * s3);   s4  = (uint8_t)(s4  * s4);
    s5  = (uint8_t)(s5  * s5);   s6  = (uint8_t)(s6  * s6);
    s7  = (uint8_t)(s7  * s7);   s8  = (uint8_t)(s8  * s8);
    s9  = (uint8_t)(s9  * s9);   s10 = (uint8_t)(s10 * s10);
    s11 = (uint8_t)(s11 * s11);  s12 = (uint8_t)(s12 * s12);
    int shA = (seed & 2) != 0 ? 4 : 5;
    int shB = npart == 3 ? 6 : 5;
    int sh1 = (seed & 1) != 0 ? shA : shB;
    int sh2 = (seed & 1) != 0 ? shB : shA;
    int sh3 = (seed & 0x10) != 0 ? sh1 : sh2;
    s1  = (uint8_t)(s1  >> sh1);  s2  = (uint8_t)(s2  >> sh2);
    s3  = (uint8_t)(s3  >> sh1);  s4  = (uint8_t)(s4  >> sh2);
    s5  = (uint8_t)(s5  >> sh1);  s6  = (uint8_t)(s6  >> sh2);
    s7  = (uint8_t)(s7  >> sh1);  s8  = (uint8_t)(s8  >> sh2);
    s9  = (uint8_t)(s9  >> sh3);  s10 = (uint8_t)(s10 >> sh3);
    s11 = (uint8_t)(s11 >> sh3);  s12 = (uint8_t)(s12 >> sh3);
    int a =              0x3F & (int)(s1 * x + s2 * y + s11 * z + (rnum >> 14));
    int b =              0x3F & (int)(s3 * x + s4 * y + s12 * z + (rnum >> 10));
    int c = npart >= 3 ? 0x3F & (int)(s5 * x + s6 * y + s9  * z + (rnum >>  6)) : 0;
    int d = npart >= 4 ? 0x3F & (int)(s7 * x + s8 * y + s10 * z + (rnum >>  2)) : 0;
    return a >= b && a >= c && a >= d ? 0
         : b >= c && b >= d           ? 1
         : c >= d                     ? 2
         :                              3;
}

/* ------------------------------------------------------- weight infill -- */
/* Integer bilinear infill of the decimated weight grid (spec C.2.18).     */

static uint32_t infill_weight(const uint32_t *uw, int nplanes, int plane,
                              int gw, int gh, int bw, int bh, int tx, int ty) {
    uint32_t ds = (uint32_t)((1024 + bw / 2) / (bw - 1));
    uint32_t dt = (uint32_t)((1024 + bh / 2) / (bh - 1));
    uint32_t gs = (ds * (uint32_t)tx * (uint32_t)(gw - 1) + 32) >> 6;
    uint32_t gt = (dt * (uint32_t)ty * (uint32_t)(gh - 1) + 32) >> 6;
    uint32_t js = gs >> 4, fs = gs & 0xF;
    uint32_t jt = gt >> 4, ft = gt & 0xF;
    uint32_t w11 = (fs * ft + 8) >> 4;
    uint32_t w10 = ft - w11;
    uint32_t w01 = fs - w11;
    uint32_t w00 = 16 - fs - ft + w11;
    uint32_t i00 = jt * (uint32_t)gw + js;
    uint32_t i01 = i00 + 1;
    uint32_t i10 = i00 + (uint32_t)gw;
    uint32_t i11 = i10 + 1;
    uint32_t n = (uint32_t)(gw * gh);
    uint32_t p00 = i00 < n ? uw[i00 * (uint32_t)nplanes + (uint32_t)plane] : 0;
    uint32_t p01 = i01 < n ? uw[i01 * (uint32_t)nplanes + (uint32_t)plane] : 0;
    uint32_t p10 = i10 < n ? uw[i10 * (uint32_t)nplanes + (uint32_t)plane] : 0;
    uint32_t p11 = i11 < n ? uw[i11 * (uint32_t)nplanes + (uint32_t)plane] : 0;
    return (p00 * w00 + p01 * w01 + p10 * w10 + p11 * w11 + 8) >> 4;
}

/* ------------------------------------------------------ block decoding -- */

enum TexelKind {
    KIND_UNORM16 = 0,  /* value is a 16-bit unorm             */
    KIND_FP16    = 1   /* value is raw IEEE half-float bits   */
};

struct DecodedBlock {
    uint16_t val[144][4];
    uint8_t  kind[144][4];
    bool     error;    /* error colour (opaque magenta) */
};

static void decode_block(const uint8_t *src, int bw, int bh, DecodedBlock &out) {
    out.error = false;
    Block128 blk(src);
    BlockMode bm = decode_block_mode(blk.bits(0, 11));
    if (bm.error) { out.error = true; return; }

    int ntex = bw * bh;

    if (bm.void_extent) {
        /* Extent coordinate validity is intentionally not enforced. */
        bool hdr = blk.bit(9) != 0;
        uint16_t c[4] = {
            (uint16_t)blk.bits(64, 16),  (uint16_t)blk.bits(80, 16),
            (uint16_t)blk.bits(96, 16), (uint16_t)blk.bits(112, 16)
        };
        for (int i = 0; i < ntex; i++)
            for (int ch = 0; ch < 4; ch++) {
                out.val[i][ch] = c[ch];
                out.kind[i][ch] = hdr ? (uint8_t)KIND_FP16 : (uint8_t)KIND_UNORM16;
            }
        return;
    }

    int nweights = bm.gw * bm.gh * (bm.dual ? 2 : 1);
    int wbits = ise_bit_count(bm.wparams, nweights);
    int nparts = (int)blk.bits(11, 2) + 1;
    if (nweights > 64 || wbits > 96 || wbits < 24 ||
        bm.gw > bw || bm.gh > bh || (nparts == 4 && bm.dual)) {
        out.error = true;
        return;
    }

    bool single_cem = nparts == 1 || blk.bits(23, 2) == 0;
    int config_bits = (nparts == 1 ? 17 : single_cem ? 29 : 25 + 3 * nparts) +
                      (bm.dual ? 2 : 0);
    int color_bits = 128 - wbits - config_bits;
    int extra_start = 127 - wbits - (single_cem ? -1
                                     : nparts == 4 ? 7
                                     : nparts == 3 ? 4
                                     : 1);

    /* Colour endpoint modes. */
    uint32_t cems[4] = { 0, 0, 0, 0 };
    if (nparts == 1) {
        cems[0] = blk.bits(13, 4);
    } else {
        uint32_t sel = blk.bits(23, 2);
        if (sel == 0) {
            uint32_t mode = blk.bits(25, 4);
            for (int i = 0; i < nparts; i++) cems[i] = mode;
        } else {
            for (int i = 0; i < nparts; i++) {
                uint32_t cls = sel - (blk.bit(25 + i) ? 0 : 1);
                int l0i = nparts + 2 * i;
                int l1i = nparts + 2 * i + 1;
                uint32_t l0 = blk.bit(l0i < 4 ? 25 + l0i : extra_start + l0i - 4);
                uint32_t l1 = blk.bit(l1i < 4 ? 25 + l1i : extra_start + l1i - 4);
                cems[i] = (cls << 2) | (l1 << 1) | l0;
            }
        }
    }

    int ncolor = 0;
    for (int i = 0; i < nparts; i++) ncolor += cem_value_count(cems[i]);
    if (ncolor > 18 || color_bits < (13 * ncolor + 4) / 5) {
        out.error = true;
        return;
    }

    /* Colour endpoint values. */
    IseParams cq = max_quant_for_bits(color_bits, ncolor);
    IseVal cvals[18];
    {
        BitReader cr(blk, nparts == 1 ? 17 : 29, ise_bit_count(cq, ncolor));
        decode_ise(cvals, ncolor, cr, cq);
    }
    uint32_t unq[18];
    for (int i = 0; i < ncolor; i++)
        unq[i] = unquant_color(cvals[i].m, cvals[i].tq, cvals[i].v, cq);

    Endpoints eps[4];
    {
        int idx = 0;
        for (int i = 0; i < nparts; i++) {
            decode_endpoints(eps[i], cems[i], &unq[idx]);
            idx += cem_value_count(cems[i]);
        }
    }

    /* Weights (stored bit-reversed from the top of the block). */
    Block128 rev = reverse128(blk);
    IseVal wvals[64];
    {
        BitReader wr(rev, 0, wbits);
        decode_ise(wvals, nweights, wr, bm.wparams);
    }
    uint32_t uw[64];
    for (int i = 0; i < nweights; i++)
        uw[i] = unquant_weight(wvals[i].m, wvals[i].tq, wvals[i].v, bm.wparams);

    int ccs = bm.dual ? (int)blk.bits(extra_start - 2, 2) : -1;
    uint32_t seed = nparts > 1 ? blk.bits(13, 10) : 0;
    bool small_block = ntex < 31;
    int nplanes = bm.dual ? 2 : 1;

    for (int ty = 0; ty < bh; ty++) {
        for (int tx = 0; tx < bw; tx++) {
            int ti = ty * bw + tx;
            int part = nparts == 1 ? 0
                     : select_partition(seed, tx, ty, nparts, small_block);
            const Endpoints &ep = eps[part];
            uint32_t w0 = infill_weight(uw, nplanes, 0, bm.gw, bm.gh, bw, bh, tx, ty);
            uint32_t w1 = bm.dual
                        ? infill_weight(uw, nplanes, 1, bm.gw, bm.gh, bw, bh, tx, ty)
                        : w0;
            for (int ch = 0; ch < 4; ch++) {
                bool hdr = ch == 3 ? ep.a_hdr : ep.rgb_hdr;
                uint32_t w = (ccs == ch) ? w1 : w0;
                if (!hdr) {
                    uint32_t c0 = ((uint32_t)ep.e0[ch] << 8) | (uint32_t)ep.e0[ch];
                    uint32_t c1 = ((uint32_t)ep.e1[ch] << 8) | (uint32_t)ep.e1[ch];
                    uint32_t c = (c0 * (64 - w) + c1 * w + 32) >> 6;
                    out.val[ti][ch] = (uint16_t)c;
                    out.kind[ti][ch] = (uint8_t)KIND_UNORM16;
                } else {
                    uint32_t c0 = (uint32_t)ep.e0[ch] << 4;
                    uint32_t c1 = (uint32_t)ep.e1[ch] << 4;
                    uint32_t c = (c0 * (64 - w) + c1 * w + 32) >> 6;
                    uint32_t e = (c >> 11) & 0x1F;
                    uint32_t mm = c & 0x7FF;
                    uint32_t mt = mm < 512 ? 3 * mm
                                : mm >= 1536 ? 5 * mm - 2048
                                : 4 * mm - 512;
                    uint32_t h = (e << 10) + (mt >> 3);
                    if (((h >> 10) & 0x1F) == 0x1F)
                        h = 0x7BFF; /* inf/nan -> largest normal half */
                    out.val[ti][ch] = (uint16_t)h;
                    out.kind[ti][ch] = (uint8_t)KIND_FP16;
                }
            }
        }
    }
}

/* -------------------------------------------------- output conversions -- */

static uint8_t unorm16_to_u8(uint16_t v) { return (uint8_t)(v >> 8); }

static uint8_t fp16_to_u8(uint16_t h) {
    float f = half_to_float(h);
    if (f != f) f = 1.0f;  /* NaN */
    f = clamp01(f);
    return (uint8_t)(f * 255.0f + 0.5f);
}

/* ------------------------------------------------------------ encoder --- */

struct EncConfig {
    bool valid;
    bool dual;            /* dual-plane configuration          */
    uint32_t block_mode;  /* 11-bit block mode field           */
    int gw, gh;           /* weight grid                       */
    int nvals;            /* gw*gh*(dual?2:1) weight values    */
    IseParams wq;         /* weight quantization               */
    IseParams cq;         /* colour endpoint quantization      */
    int wlevels, clevels;
    int wbits;            /* ISE bit count of the weight data  */
    uint8_t wunq[32];     /* raw weight index -> [0,64]        */
    uint8_t cunq[256];    /* raw colour index -> [0,255]       */
};

/* Deterministic per-block-size configuration: single partition, CEM 12,
 * optionally dual-plane.  Chooses the block mode maximizing a quality
 * heuristic subject to the 128-bit budget (11 block mode + 2 partition
 * count + 4 CEM (+2 CCS if dual) + weights + endpoints).                 */
static EncConfig make_config(int bw, int bh, bool want_dual) {
    EncConfig cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.valid = false;

    long best_score = -1;
    for (int pass = 0; pass < 2 && !cfg.valid; pass++) {
        for (uint32_t mode = 0; mode < 2048; mode++) {
            BlockMode bm = decode_block_mode(mode);
            if (bm.error || bm.void_extent || bm.dual != want_dual) continue;
            if (bm.gw > bw || bm.gh > bh) continue;
            int nw = bm.gw * bm.gh * (want_dual ? 2 : 1);
            if (nw > 64) continue;
            int wbits = ise_bit_count(bm.wparams, nw);
            if (wbits < 24 || wbits > 96) continue;
            int avail = 128 - 17 - (want_dual ? 2 : 0) - wbits;
            if (avail < (13 * 8 + 4) / 5) continue;   /* 8 endpoint values */
            IseParams cq = max_quant_for_bits(avail, 8);
            int wlev = ise_levels(bm.wparams);
            int clev = ise_levels(cq);
            if (clev < wlev) continue;
            if (pass == 0 && (clev < 16 || wlev < 8)) continue; /* quality floor */
            int wl100 = 0, cl100 = 0;
            for (int i = 0; i < 21; i++) {
                if (k_quant_desc[i].levels == wlev) wl100 = k_quant_desc[i].l100;
                if (k_quant_desc[i].levels == clev) cl100 = k_quant_desc[i].l100;
            }
            long score = 2L * nw * wl100 + 8L * cl100;
            if (score > best_score) {
                best_score = score;
                cfg.valid = true;
                cfg.dual = want_dual;
                cfg.block_mode = mode;
                cfg.gw = bm.gw; cfg.gh = bm.gh;
                cfg.nvals = nw;
                cfg.wq = bm.wparams;
                cfg.cq = cq;
                cfg.wlevels = wlev;
                cfg.clevels = clev;
                cfg.wbits = wbits;
            }
        }
    }
    if (!cfg.valid) return cfg;

    for (int r = 0; r < cfg.wlevels; r++) {
        uint32_t m = (uint32_t)r & ((1u << cfg.wq.bits) - 1);
        uint32_t tq = (uint32_t)r >> cfg.wq.bits;
        cfg.wunq[r] = (uint8_t)unquant_weight(m, tq, (uint32_t)r, cfg.wq);
    }
    for (int r = 0; r < cfg.clevels; r++) {
        uint32_t m = (uint32_t)r & ((1u << cfg.cq.bits) - 1);
        uint32_t tq = (uint32_t)r >> cfg.cq.bits;
        cfg.cunq[r] = (uint8_t)unquant_color(m, tq, (uint32_t)r, cfg.cq);
    }
    return cfg;
}

/* Nearest quantization; ties resolved toward the lower reconstruction so
 * the chosen reconstruction is monotone in the input (this guarantees the
 * CEM 12 decoder never takes the blue-contract branch for min/max pairs). */
static uint8_t quantize_to(const uint8_t *unq, int levels, int target) {
    int best = 0;
    int best_err = 0x7FFFFFFF, best_val = -1;
    for (int r = 0; r < levels; r++) {
        int err = (int)unq[r] - target;
        if (err < 0) err = -err;
        if (err < best_err || (err == best_err && (int)unq[r] < best_val)) {
            best_err = err;
            best_val = unq[r];
            best = r;
        }
    }
    return (uint8_t)best;
}

static void encode_void_extent_ldr(const uint8_t rgba[4], uint8_t *dst) {
    Block128 b;
    b.set_bits(0, 9, 0x1FC);          /* void-extent signature      */
    /* bit 9 = 0: LDR colours */
    b.set_bits(10, 2, 3);             /* reserved, must be 1        */
    b.set_bits(12, 52, ~(uint64_t)0); /* extents: all-ones = unused */
    for (int c = 0; c < 4; c++) {
        uint32_t v16 = ((uint32_t)rgba[c] << 8) | rgba[c];
        b.set_bits(64 + 16 * c, 16, v16);
    }
    b.store(dst);
}

/* PCA (power iteration) + per-channel least-squares line fit over the
 * selected channels.  Returns false if the fit is degenerate.             */
static bool fit_line(const uint8_t *px, int ntex, const uint8_t *chs, int nch,
                     int e0[4], int e1[4]) {
    double mu[4] = { 0, 0, 0, 0 };
    for (int i = 0; i < ntex; i++)
        for (int k = 0; k < nch; k++)
            mu[chs[k]] += px[4 * i + chs[k]];
    for (int k = 0; k < nch; k++)
        mu[chs[k]] /= (double)ntex;

    double axis[4] = { 0, 0, 0, 0 };
    {
        int mn[4] = { 255, 255, 255, 255 }, mx[4] = { 0, 0, 0, 0 };
        for (int i = 0; i < ntex; i++)
            for (int k = 0; k < nch; k++) {
                int c = chs[k], v = px[4 * i + c];
                if (v < mn[c]) mn[c] = v;
                if (v > mx[c]) mx[c] = v;
            }
        double n2 = 0.0;
        for (int k = 0; k < nch; k++) {
            int c = chs[k];
            axis[c] = (double)(mx[c] - mn[c]);
            n2 += axis[c] * axis[c];
        }
        if (n2 <= 0.0) return false;
        double n = sqrt(n2);
        for (int k = 0; k < nch; k++) axis[chs[k]] /= n;
    }
    for (int it = 0; it < 4; it++) {
        double nd[4] = { 0, 0, 0, 0 };
        for (int i = 0; i < ntex; i++) {
            double s = 0.0;
            for (int k = 0; k < nch; k++) {
                int c = chs[k];
                s += ((double)px[4 * i + c] - mu[c]) * axis[c];
            }
            for (int k = 0; k < nch; k++) {
                int c = chs[k];
                nd[c] += s * ((double)px[4 * i + c] - mu[c]);
            }
        }
        double n2 = 0.0;
        for (int k = 0; k < nch; k++) n2 += nd[chs[k]] * nd[chs[k]];
        if (n2 < 1e-12) break;
        double n = sqrt(n2);
        for (int k = 0; k < nch; k++) axis[chs[k]] = nd[chs[k]] / n;
    }

    double t[144];
    double tmin = 1e30, tmax = -1e30;
    for (int i = 0; i < ntex; i++) {
        double s = 0.0;
        for (int k = 0; k < nch; k++) {
            int c = chs[k];
            s += ((double)px[4 * i + c] - mu[c]) * axis[c];
        }
        t[i] = s;
        if (s < tmin) tmin = s;
        if (s > tmax) tmax = s;
    }
    if (tmax - tmin < 1e-9) return false;

    double S00 = 0, S01 = 0, S11 = 0, B0[4] = { 0, 0, 0, 0 }, B1[4] = { 0, 0, 0, 0 };
    for (int i = 0; i < ntex; i++) {
        double u = (t[i] - tmin) / (tmax - tmin);
        double w0 = 1.0 - u;
        S00 += w0 * w0; S01 += w0 * u; S11 += u * u;
        for (int k = 0; k < nch; k++) {
            int c = chs[k];
            B0[c] += w0 * px[4 * i + c];
            B1[c] += u * px[4 * i + c];
        }
    }
    double det = S00 * S11 - S01 * S01;
    if (fabs(det) < 1e-9) return false;
    for (int k = 0; k < nch; k++) {
        int c = chs[k];
        double a = (B0[c] * S11 - B1[c] * S01) / det;
        double b = (B1[c] * S00 - B0[c] * S01) / det;
        if (a < 0) a = 0; if (a > 255) a = 255;
        if (b < 0) b = 0; if (b > 255) b = 255;
        e0[c] = (int)(a + 0.5);
        e1[c] = (int)(b + 0.5);
    }
    return true;
}

/* Ideal per-texel weights in [0,64]: projection on the e0->e1 axis over
 * the selected channels.                                                  */
static void weight_targets(const uint8_t *px, int ntex, const uint8_t *chs,
                           int nch, const int e0[4], const int e1[4],
                           uint8_t *targets) {
    int64_t d[4] = { 0, 0, 0, 0 }, den = 0;
    for (int k = 0; k < nch; k++) {
        int c = chs[k];
        d[c] = e1[c] - e0[c];
        den += d[c] * d[c];
    }
    for (int i = 0; i < ntex; i++) {
        if (den == 0) { targets[i] = 0; continue; }
        int64_t num = 0;
        for (int k = 0; k < nch; k++) {
            int c = chs[k];
            num += ((int64_t)px[4 * i + c] - e0[c]) * d[c];
        }
        int w;
        if (num <= 0) w = 0;
        else if (num >= den) w = 64;
        else w = (int)((num * 64 + den / 2) / den);
        targets[i] = (uint8_t)w;
    }
}

/* Down-sample full-resolution weight targets to the grid with the transpose
 * of the decoder's bilinear infill, then quantize to raw weight indices.  */
static void grid_downsample(const uint8_t *targets, int bw, int bh,
                            const EncConfig &cfg, uint8_t *graw) {
    int gw = cfg.gw, gh = cfg.gh, ngrid = gw * gh;
    double acc[64], accw[64];
    for (int i = 0; i < ngrid; i++) { acc[i] = 0.0; accw[i] = 0.0; }
    uint32_t ds = (uint32_t)((1024 + bw / 2) / (bw - 1));
    uint32_t dt = (uint32_t)((1024 + bh / 2) / (bh - 1));
    for (int ty = 0; ty < bh; ty++)
        for (int tx = 0; tx < bw; tx++) {
            uint32_t gs = (ds * (uint32_t)tx * (uint32_t)(gw - 1) + 32) >> 6;
            uint32_t gt = (dt * (uint32_t)ty * (uint32_t)(gh - 1) + 32) >> 6;
            uint32_t js = gs >> 4, fs = gs & 0xF;
            uint32_t jt = gt >> 4, ft = gt & 0xF;
            uint32_t w11 = (fs * ft + 8) >> 4;
            uint32_t w10 = ft - w11;
            uint32_t w01 = fs - w11;
            uint32_t w00 = 16 - fs - ft + w11;
            uint32_t idx[4] = { jt * (uint32_t)gw + js, jt * (uint32_t)gw + js + 1,
                                (jt + 1) * (uint32_t)gw + js,
                                (jt + 1) * (uint32_t)gw + js + 1 };
            uint32_t twt[4] = { w00, w01, w10, w11 };
            for (int k = 0; k < 4; k++)
                if (twt[k] && idx[k] < (uint32_t)ngrid) {
                    acc[idx[k]]  += (double)twt[k] * targets[ty * bw + tx];
                    accw[idx[k]] += (double)twt[k];
                }
        }
    for (int gy = 0; gy < gh; gy++)
        for (int gx = 0; gx < gw; gx++) {
            int gi = gy * gw + gx;
            int target;
            if (accw[gi] > 0.0) {
                target = (int)(acc[gi] / accw[gi] + 0.5);
            } else {
                int tx = gw > 1 ? (gx * (bw - 1) + (gw - 1) / 2) / (gw - 1) : 0;
                int ty = gh > 1 ? (gy * (bh - 1) + (gh - 1) / 2) / (gh - 1) : 0;
                target = targets[ty * bw + tx];
            }
            if (target < 0) target = 0;
            if (target > 64) target = 64;
            int best = 0, best_err = 0x7FFFFFFF;
            for (int r = 0; r < cfg.wlevels; r++) {
                int err = (int)cfg.wunq[r] - target;
                if (err < 0) err = -err;
                if (err < best_err) { best_err = err; best = r; }
            }
            graw[gi] = (uint8_t)best;
        }
}

/* Quantize endpoints (CEM 12 order r0 r1 g0 g1 b0 b1 a0 a1) and swap the
 * pair order if needed so the decoder never takes the blue-contract branch
 * (comparison is on the unquantized RGB sums).  Returns true if swapped;
 * e0/e1 are swapped along with the raw values.                            */
static bool quantize_endpoints_cem12(const EncConfig &cfg, int e0[4], int e1[4],
                                     uint8_t craw[8]) {
    for (int c = 0; c < 4; c++) {
        craw[2 * c]     = quantize_to(cfg.cunq, cfg.clevels, e0[c]);
        craw[2 * c + 1] = quantize_to(cfg.cunq, cfg.clevels, e1[c]);
    }
    int s0 = cfg.cunq[craw[0]] + cfg.cunq[craw[2]] + cfg.cunq[craw[4]];
    int s1 = cfg.cunq[craw[1]] + cfg.cunq[craw[3]] + cfg.cunq[craw[5]];
    if (s1 < s0) {
        for (int c = 0; c < 4; c++) {
            uint8_t tr = craw[2 * c];
            craw[2 * c] = craw[2 * c + 1];
            craw[2 * c + 1] = tr;
            int te = e0[c]; e0[c] = e1[c]; e1[c] = te;
        }
        return true;
    }
    return false;
}

static void pack_block(const EncConfig &cfg, const uint8_t craw[8],
                       const uint8_t *wraw, int ccs, uint8_t *dst) {
    Block128 b;
    b.set_bits(0, 11, cfg.block_mode);
    /* bits 11..12: partition count - 1 = 0 */
    b.set_bits(13, 4, 12);            /* CEM 12: LDR RGBA direct */
    encode_ise(b, 17, craw, 8, cfg.cq);
    if (cfg.dual)
        b.set_bits(126 - cfg.wbits, 2, (uint32_t)ccs);
    Block128 wtmp;
    encode_ise(wtmp, 0, wraw, cfg.nvals, cfg.wq);
    for (int i = 0; i < cfg.wbits; i++)
        if (wtmp.bit(i)) b.set_bit(127 - i);
    b.store(dst);
}

static const uint8_t k_all4[4] = { 0, 1, 2, 3 };

static bool encode_block_single(const uint8_t *px, int bw, int bh,
                                const EncConfig &cfg, uint8_t *dst) {
    if (!cfg.valid) return false;
    int ntex = bw * bh;
    int e0[4], e1[4];
    if (!fit_line(px, ntex, k_all4, 4, e0, e1)) return false;
    uint8_t craw[8];
    quantize_endpoints_cem12(cfg, e0, e1, craw);
    uint8_t targets[144], wraw[64];
    weight_targets(px, ntex, k_all4, 4, e0, e1, targets);
    grid_downsample(targets, bw, bh, cfg, wraw);
    pack_block(cfg, craw, wraw, 0, dst);
    return true;
}

static bool encode_block_dual(const uint8_t *px, int bw, int bh,
                              const EncConfig &cfg, uint8_t *dst) {
    if (!cfg.valid) return false;
    int ntex = bw * bh;

    /* Pick the dual-plane channel: worst line-fit residual of a 4-channel
     * fit (that channel benefits most from its own weight ramp). */
    int ccs = 3;
    {
        int fe0[4], fe1[4];
        if (fit_line(px, ntex, k_all4, 4, fe0, fe1)) {
            uint8_t tg[144];
            weight_targets(px, ntex, k_all4, 4, fe0, fe1, tg);
            double res[4] = { 0, 0, 0, 0 };
            for (int i = 0; i < ntex; i++)
                for (int c = 0; c < 4; c++) {
                    double p = fe0[c] + (fe1[c] - fe0[c]) * (double)tg[i] / 64.0;
                    double dch = (double)px[4 * i + c] - p;
                    res[c] += dch * dch;
                }
            ccs = 0;
            for (int c = 1; c < 4; c++)
                if (res[c] > res[ccs]) ccs = c;
        }
    }

    uint8_t others[3];
    int no = 0;
    for (int c = 0; c < 4; c++)
        if (c != ccs) others[no++] = (uint8_t)c;

    int e0[4], e1[4];
    if (!fit_line(px, ntex, others, 3, e0, e1)) {
        /* remaining channels constant */
        for (int k = 0; k < 3; k++) {
            int c = others[k];
            e0[c] = px[c];
            e1[c] = px[c];
        }
    }
    int mn = 255, mx = 0;
    for (int i = 0; i < ntex; i++) {
        int v = px[4 * i + ccs];
        if (v < mn) mn = v;
        if (v > mx) mx = v;
    }
    e0[ccs] = mn;
    e1[ccs] = mx;

    uint8_t craw[8];
    quantize_endpoints_cem12(cfg, e0, e1, craw);

    uint8_t t0[144], t1[144];
    uint8_t chs1[1] = { (uint8_t)ccs };
    weight_targets(px, ntex, others, 3, e0, e1, t0);
    weight_targets(px, ntex, chs1, 1, e0, e1, t1);

    uint8_t g0[32], g1[32], wraw[64];
    grid_downsample(t0, bw, bh, cfg, g0);
    grid_downsample(t1, bw, bh, cfg, g1);
    int ngrid = cfg.gw * cfg.gh;
    for (int i = 0; i < ngrid; i++) {
        wraw[2 * i]     = g0[i];
        wraw[2 * i + 1] = g1[i];
    }
    pack_block(cfg, craw, wraw, ccs, dst);
    return true;
}

/* Decode-based SSE of a candidate block against the source pixels. */
static uint64_t block_sse(const uint8_t *px, const uint8_t *blk, int bw, int bh) {
    DecodedBlock db;
    decode_block(blk, bw, bh, db);
    if (db.error) return ~(uint64_t)0;
    uint64_t s = 0;
    int ntex = bw * bh;
    for (int i = 0; i < ntex; i++)
        for (int c = 0; c < 4; c++) {
            int v = db.kind[i][c] == KIND_UNORM16 ? (db.val[i][c] >> 8)
                                                  : fp16_to_u8(db.val[i][c]);
            int d = v - (int)px[4 * i + c];
            s += (uint64_t)((int64_t)d * d);
        }
    return s;
}

static void encode_block(const uint8_t *px, int bw, int bh,
                         const EncConfig &cfg_s, const EncConfig &cfg_d,
                         uint8_t *dst) {
    int ntex = bw * bh;

    /* Uniform block -> LDR void extent (near-exact roundtrip). */
    bool uniform = true;
    for (int i = 1; i < ntex && uniform; i++)
        uniform = memcmp(px, px + 4 * i, 4) == 0;
    if (!uniform) {
        uint8_t bs[16], bd[16];
        bool ok_s = encode_block_single(px, bw, bh, cfg_s, bs);
        bool ok_d = encode_block_dual(px, bw, bh, cfg_d, bd);
        uint64_t sse_s = ok_s ? block_sse(px, bs, bw, bh) : ~(uint64_t)0;
        uint64_t sse_d = ok_d ? block_sse(px, bd, bw, bh) : ~(uint64_t)0;
        if (ok_s && sse_s <= sse_d) { memcpy(dst, bs, 16); return; }
        if (ok_d)                   { memcpy(dst, bd, 16); return; }
        /* fall through to void extent of the average */
    }

    uint8_t avg[4];
    if (uniform) {
        memcpy(avg, px, 4);
    } else {
        for (int c = 0; c < 4; c++) {
            uint32_t s = 0;
            for (int i = 0; i < ntex; i++) s += px[4 * i + c];
            avg[c] = (uint8_t)((s + ntex / 2) / (uint32_t)ntex);
        }
    }
    encode_void_extent_ldr(avg, dst);
}

/* --------------------------------------------------------- public API --- */

} /* anonymous namespace */

int astc_decode(texc_format fmt, const uint8_t *src, size_t src_size,
                uint32_t width, uint32_t height, uint8_t *dst) {
    uint32_t bw, bh;
    if (!astc_dims(fmt, bw, bh)) return TEXC_ERR_INVALID_ARG;
    uint32_t nbx = (width + bw - 1) / bw;
    uint32_t nby = (height + bh - 1) / bh;
    if (src_size < (size_t)nbx * nby * 16) return TEXC_ERR_BUFFER_TOO_SMALL;

    DecodedBlock db;
    uint8_t blockpx[144 * 4];
    for (uint32_t by = 0; by < nby; by++) {
        for (uint32_t bx = 0; bx < nbx; bx++) {
            decode_block(src + ((size_t)by * nbx + bx) * 16, (int)bw, (int)bh, db);
            int ntex = (int)(bw * bh);
            if (db.error) {
                for (int i = 0; i < ntex; i++) {
                    blockpx[4 * i + 0] = 0xFF;
                    blockpx[4 * i + 1] = 0x00;
                    blockpx[4 * i + 2] = 0xFF;
                    blockpx[4 * i + 3] = 0xFF;
                }
            } else {
                for (int i = 0; i < ntex; i++)
                    for (int c = 0; c < 4; c++)
                        blockpx[4 * i + c] = db.kind[i][c] == KIND_UNORM16
                                           ? unorm16_to_u8(db.val[i][c])
                                           : fp16_to_u8(db.val[i][c]);
            }
            write_block_rgba8(dst, width, height, bx, by, bw, bh, blockpx);
        }
    }
    return TEXC_OK;
}

int astc_decode_f32(texc_format fmt, const uint8_t *src, size_t src_size,
                    uint32_t width, uint32_t height, float *dst) {
    uint32_t bw, bh;
    if (!astc_dims(fmt, bw, bh)) return TEXC_ERR_INVALID_ARG;
    uint32_t nbx = (width + bw - 1) / bw;
    uint32_t nby = (height + bh - 1) / bh;
    if (src_size < (size_t)nbx * nby * 16) return TEXC_ERR_BUFFER_TOO_SMALL;

    DecodedBlock db;
    float blockpx[144 * 4];
    for (uint32_t by = 0; by < nby; by++) {
        for (uint32_t bx = 0; bx < nbx; bx++) {
            decode_block(src + ((size_t)by * nbx + bx) * 16, (int)bw, (int)bh, db);
            int ntex = (int)(bw * bh);
            if (db.error) {
                for (int i = 0; i < ntex; i++) {
                    blockpx[4 * i + 0] = 1.0f;
                    blockpx[4 * i + 1] = 0.0f;
                    blockpx[4 * i + 2] = 1.0f;
                    blockpx[4 * i + 3] = 1.0f;
                }
            } else {
                for (int i = 0; i < ntex; i++)
                    for (int c = 0; c < 4; c++)
                        blockpx[4 * i + c] = db.kind[i][c] == KIND_UNORM16
                                           ? (float)db.val[i][c] / 65535.0f
                                           : half_to_float(db.val[i][c]);
            }
            write_block_rgba32f(dst, width, height, bx, by, bw, bh, blockpx);
        }
    }
    return TEXC_OK;
}

int astc_encode(texc_format fmt, const uint8_t *src,
                uint32_t width, uint32_t height, uint8_t *dst,
                const texc_encode_options *opts) {
    (void)opts;                     /* no ASTC-specific options yet */
    uint32_t bw, bh;
    if (!astc_dims(fmt, bw, bh)) return TEXC_ERR_INVALID_ARG;
    uint32_t nbx = (width + bw - 1) / bw;
    uint32_t nby = (height + bh - 1) / bh;

    EncConfig cfg_s = make_config((int)bw, (int)bh, false);
    EncConfig cfg_d = make_config((int)bw, (int)bh, true);
    uint8_t blockpx[144 * 4];
    for (uint32_t by = 0; by < nby; by++) {
        for (uint32_t bx = 0; bx < nbx; bx++) {
            read_block_rgba8(src, width, height, bx, by, bw, bh, blockpx);
            encode_block(blockpx, (int)bw, (int)bh, cfg_s, cfg_d,
                         dst + ((size_t)by * nbx + bx) * 16);
        }
    }
    return TEXC_OK;
}

} /* namespace texc */

/* ------------------------------------------------------------ selftest -- */

#ifdef TEXC_SELFTEST

#include <cmath>
#include <cstdio>

namespace texc {
namespace {

static int g_fails = 0;

static void check(bool ok, const char *what) {
    if (!ok) {
        printf("FAIL: %s\n", what);
        g_fails++;
    }
}

static uint32_t g_rng = 0x12345678u;
static uint32_t rnd() {
    g_rng = g_rng * 1664525u + 1013904223u;
    return g_rng >> 8;
}

/* Spot-check the trit/quint decode against known spec values. */
static void test_ise_tables() {
    const IseTables &t = ise_tables();
    static const struct { int T; uint8_t v[5]; } tk[] = {
        { 0x0F, { 2, 0, 2, 0, 0 } }, { 28, { 0, 0, 0, 2, 2 } },
        { 93,  { 1, 2, 0, 2, 2 } }, { 255, { 2, 1, 2, 2, 2 } },
        { 12,  { 0, 2, 2, 0, 0 } }, { 3,   { 0, 0, 2, 0, 0 } },
    };
    for (size_t i = 0; i < sizeof(tk) / sizeof(tk[0]); i++)
        for (int j = 0; j < 5; j++)
            check(t.trits[tk[i].T][j] == tk[i].v[j], "trit table KAT");
    static const struct { int Q; uint8_t v[3]; } qk[] = {
        { 5, { 0, 4, 0 } }, { 38, { 4, 0, 4 } }, { 127, { 1, 3, 4 } },
        { 87, { 3, 2, 4 } }, { 17, { 1, 2, 0 } }, { 7, { 4, 4, 4 } },
    };
    for (size_t i = 0; i < sizeof(qk) / sizeof(qk[0]); i++)
        for (int j = 0; j < 3; j++)
            check(t.quints[qk[i].Q][j] == qk[i].v[j], "quint table KAT");
}

/* Spot-check unquantization value sets against the spec. */
static void test_unquant_tables() {
    /* QUANT_6 weights -> {0,12,25,39,52,64} */
    IseParams w6 = { ISE_TRIT, 1 };
    int seen[65] = { 0 };
    for (uint32_t v = 0; v < 6; v++)
        seen[unquant_weight(v & 1, v >> 1, v, w6)] = 1;
    check(seen[0] && seen[12] && seen[25] && seen[39] && seen[52] && seen[64],
          "QUANT_6 weight values");
    /* QUANT_5 weights -> {0,16,32,48,64} */
    IseParams w5 = { ISE_QUINT, 0 };
    int seen5[65] = { 0 };
    for (uint32_t v = 0; v < 5; v++)
        seen5[unquant_weight(0, v, v, w5)] = 1;
    check(seen5[0] && seen5[16] && seen5[32] && seen5[48] && seen5[64],
          "QUANT_5 weight values");
    /* QUANT_6 colours -> {0,51,102,153,204,255} */
    IseParams c6 = { ISE_TRIT, 1 };
    int seenc[256] = { 0 };
    for (uint32_t v = 0; v < 6; v++)
        seenc[unquant_color(v & 1, v >> 1, v, c6)] = 1;
    check(seenc[0] && seenc[51] && seenc[102] && seenc[153] && seenc[204] &&
          seenc[255], "QUANT_6 colour values");
}

static void test_ise_roundtrip() {
    for (int qi = 0; qi < 21; qi++) {
        IseParams p = { k_quant_desc[qi].mode, k_quant_desc[qi].bits };
        int levels = k_quant_desc[qi].levels;
        for (int n = 1; n <= 64; n = (n < 20 ? n + 1 : n + 11)) {
            int nbits = ise_bit_count(p, n);
            if (5 + nbits > 128) continue;
            uint8_t vals[64];
            for (int i = 0; i < n; i++)
                vals[i] = (uint8_t)(rnd() % (uint32_t)levels);
            Block128 b;
            encode_ise(b, 5, vals, n, p);
            /* no stray bits outside the sequence */
            bool clean = true;
            for (int i = 0; i < 128; i++)
                if ((i < 5 || i >= 5 + nbits) && b.bit(i)) clean = false;
            check(clean, "ISE writer bit range");
            IseVal out[64];
            BitReader br(b, 5, nbits);
            decode_ise(out, n, br, p);
            bool ok = true;
            for (int i = 0; i < n; i++)
                if (out[i].v != vals[i]) ok = false;
            check(ok, "ISE roundtrip");
        }
    }
}

struct FmtInfo { texc_format f; const char *name; };
static const FmtInfo k_fmts[14] = {
    { TEXC_FORMAT_ASTC_4x4, "4x4" },     { TEXC_FORMAT_ASTC_5x4, "5x4" },
    { TEXC_FORMAT_ASTC_5x5, "5x5" },     { TEXC_FORMAT_ASTC_6x5, "6x5" },
    { TEXC_FORMAT_ASTC_6x6, "6x6" },     { TEXC_FORMAT_ASTC_8x5, "8x5" },
    { TEXC_FORMAT_ASTC_8x6, "8x6" },     { TEXC_FORMAT_ASTC_8x8, "8x8" },
    { TEXC_FORMAT_ASTC_10x5, "10x5" },   { TEXC_FORMAT_ASTC_10x6, "10x6" },
    { TEXC_FORMAT_ASTC_10x8, "10x8" },   { TEXC_FORMAT_ASTC_10x10, "10x10" },
    { TEXC_FORMAT_ASTC_12x10, "12x10" }, { TEXC_FORMAT_ASTC_12x12, "12x12" },
};

static void make_gradient(uint8_t *img, int w, int h) {
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            uint8_t *p = img + 4 * (y * w + x);
            p[0] = (uint8_t)(x * 255 / (w - 1));
            p[1] = (uint8_t)(y * 255 / (h - 1));
            p[2] = (uint8_t)((x + y) * 255 / (w + h - 2));
            p[3] = (uint8_t)(255 - x * 127 / (w - 1));
        }
}

static double psnr8(const uint8_t *a, const uint8_t *b, size_t n) {
    double mse = 0.0;
    for (size_t i = 0; i < n; i++) {
        double d = (double)a[i] - (double)b[i];
        mse += d * d;
    }
    mse /= (double)n;
    if (mse <= 0.0) return 99.0;
    return 10.0 * log10(255.0 * 255.0 / mse);
}

static uint8_t g_img[64 * 64 * 4], g_dec[64 * 64 * 4], g_enc[16384];
static float g_decf[64 * 64 * 4];

static void test_roundtrip_psnr() {
    static const int sizes[2][2] = { { 32, 32 }, { 37, 23 } };
    printf("Gradient roundtrip PSNR (dB):\n");
    for (int fi = 0; fi < 14; fi++) {
        uint32_t bw, bh;
        astc_dims(k_fmts[fi].f, bw, bh);
        printf("  ASTC %-6s", k_fmts[fi].name);
        for (int si = 0; si < 2; si++) {
            int w = sizes[si][0], h = sizes[si][1];
            uint32_t nbx = ((uint32_t)w + bw - 1) / bw;
            uint32_t nby = ((uint32_t)h + bh - 1) / bh;
            make_gradient(g_img, w, h);
            check(astc_encode(k_fmts[fi].f, g_img, (uint32_t)w, (uint32_t)h,
                              g_enc, nullptr) == TEXC_OK, "encode rc");
            check(astc_decode(k_fmts[fi].f, g_enc, (size_t)nbx * nby * 16,
                              (uint32_t)w, (uint32_t)h, g_dec) == TEXC_OK,
                  "decode rc");
            double p = psnr8(g_img, g_dec, (size_t)w * h * 4);
            printf("  %2dx%-2d: %6.2f", w, h, p);
            check(p >= 25.0, "gradient PSNR >= 25 dB");
            /* f32 path agrees with the 8-bit path (LDR unorm16 >> 8). */
            check(astc_decode_f32(k_fmts[fi].f, g_enc, (size_t)nbx * nby * 16,
                                  (uint32_t)w, (uint32_t)h, g_decf) == TEXC_OK,
                  "decode_f32 rc");
            bool fok = true;
            for (int i = 0; i < w * h * 4; i++) {
                float fv = g_decf[i];
                if (fv < 0.0f || fv > 1.0f) fok = false;
                int b8 = (int)(fv * 65535.0f + 0.5f) >> 8;
                if (b8 != g_dec[i]) fok = false;
            }
            check(fok, "f32/8-bit consistency");
        }
        printf("\n");
    }
}

static void test_uniform() {
    static const uint8_t colors[4][4] = {
        { 0, 0, 0, 0 }, { 255, 255, 255, 255 },
        { 12, 34, 56, 78 }, { 200, 1, 2, 255 },
    };
    static const int sizes[2][2] = { { 32, 32 }, { 37, 23 } };
    for (int fi = 0; fi < 14; fi++) {
        uint32_t bw, bh;
        astc_dims(k_fmts[fi].f, bw, bh);
        for (int si = 0; si < 2; si++) {
            int w = sizes[si][0], h = sizes[si][1];
            uint32_t nbx = ((uint32_t)w + bw - 1) / bw;
            uint32_t nby = ((uint32_t)h + bh - 1) / bh;
            for (int ci = 0; ci < 4; ci++) {
                for (int i = 0; i < w * h; i++)
                    memcpy(g_img + 4 * i, colors[ci], 4);
                astc_encode(k_fmts[fi].f, g_img, (uint32_t)w, (uint32_t)h, g_enc,
                            nullptr);
                astc_decode(k_fmts[fi].f, g_enc, (size_t)nbx * nby * 16,
                            (uint32_t)w, (uint32_t)h, g_dec);
                int maxerr = 0;
                for (int i = 0; i < w * h * 4; i++) {
                    int e = (int)g_img[i] - (int)g_dec[i];
                    if (e < 0) e = -e;
                    if (e > maxerr) maxerr = e;
                }
                check(maxerr <= 1, "uniform roundtrip max err <= 1");
            }
        }
    }
}

static void test_void_extent_kat() {
    /* LDR void-extent block, hand-crafted. */
    Block128 b;
    b.set_bits(0, 9, 0x1FC);
    b.set_bits(10, 2, 3);
    b.set_bits(12, 52, ~(uint64_t)0);
    b.set_bits(64, 16, 0x3C00);
    b.set_bits(80, 16, 0x0000);
    b.set_bits(96, 16, 0xFFFF);
    b.set_bits(112, 16, 0x8000);
    uint8_t blk[16];
    b.store(blk);
    uint8_t out[4 * 4 * 4];
    check(astc_decode(TEXC_FORMAT_ASTC_4x4, blk, 16, 4, 4, out) == TEXC_OK,
          "void extent decode rc");
    bool ok = true;
    for (int i = 0; i < 16; i++)
        ok = ok && out[4 * i + 0] == 0x3C && out[4 * i + 1] == 0x00 &&
             out[4 * i + 2] == 0xFF && out[4 * i + 3] == 0x80;
    check(ok, "LDR void extent 8-bit KAT");
    float outf[4 * 4 * 4];
    astc_decode_f32(TEXC_FORMAT_ASTC_4x4, blk, 16, 4, 4, outf);
    ok = true;
    for (int i = 0; i < 16; i++) {
        ok = ok && fabs(outf[4 * i + 0] - 0x3C00 / 65535.0) < 1e-6;
        ok = ok && outf[4 * i + 1] == 0.0f;
        ok = ok && outf[4 * i + 2] == 1.0f;
        ok = ok && fabs(outf[4 * i + 3] - 0x8000 / 65535.0) < 1e-6;
    }
    check(ok, "LDR void extent f32 KAT");

    /* HDR void-extent block: fp16 colours (1.0, 2.0, 0.5, 1.0). */
    Block128 hb;
    hb.set_bits(0, 9, 0x1FC);
    hb.set_bit(9);
    hb.set_bits(10, 2, 3);
    hb.set_bits(12, 52, ~(uint64_t)0);
    hb.set_bits(64, 16, 0x3C00);
    hb.set_bits(80, 16, 0x4000);
    hb.set_bits(96, 16, 0x3800);
    hb.set_bits(112, 16, 0x3C00);
    hb.store(blk);
    astc_decode_f32(TEXC_FORMAT_ASTC_4x4, blk, 16, 4, 4, outf);
    ok = true;
    for (int i = 0; i < 16; i++)
        ok = ok && outf[4 * i + 0] == 1.0f && outf[4 * i + 1] == 2.0f &&
             outf[4 * i + 2] == 0.5f && outf[4 * i + 3] == 1.0f;
    check(ok, "HDR void extent f32 KAT");
    astc_decode(TEXC_FORMAT_ASTC_4x4, blk, 16, 4, 4, out);
    ok = true;
    for (int i = 0; i < 16; i++)
        ok = ok && out[4 * i + 0] == 255 && out[4 * i + 1] == 255 &&
             out[4 * i + 2] == 128 && out[4 * i + 3] == 255;
    check(ok, "HDR void extent 8-bit clamp KAT");

    /* Reserved block mode -> error colour (opaque magenta). */
    uint8_t zero[16];
    memset(zero, 0, 16);
    astc_decode(TEXC_FORMAT_ASTC_4x4, zero, 16, 4, 4, out);
    ok = true;
    for (int i = 0; i < 16; i++)
        ok = ok && out[4 * i + 0] == 255 && out[4 * i + 1] == 0 &&
             out[4 * i + 2] == 255 && out[4 * i + 3] == 255;
    check(ok, "reserved mode -> magenta");
}

/* Encode one 4x4 grey ramp block with the encoder (CEM 12) and verify every
 * texel against an independent computation of the spec's decode formula. */
static void test_cem12_manual() {
    EncConfig cfg = make_config(4, 4, false);
    check(cfg.valid, "4x4 config valid");
    check(cfg.gw == 4, "4x4 config uses full-width weight grid");
    uint8_t img[4 * 4 * 4];
    for (int y = 0; y < 4; y++)
        for (int x = 0; x < 4; x++) {
            uint8_t v = (uint8_t)(x * 85);
            uint8_t *p = img + 4 * (y * 4 + x);
            p[0] = v; p[1] = v; p[2] = v; p[3] = 255;
        }
    uint8_t blk[16];
    check(encode_block_single(img, 4, 4, cfg, blk), "single-plane encode ok");
    uint8_t out[4 * 4 * 4];
    astc_decode(TEXC_FORMAT_ASTC_4x4, blk, 16, 4, 4, out);

    /* endpoints 0 / 255 must quantize exactly at any colour quant level */
    check(cfg.cunq[quantize_to(cfg.cunq, cfg.clevels, 0)] == 0,
          "endpoint 0 exact");
    check(cfg.cunq[quantize_to(cfg.cunq, cfg.clevels, 255)] == 255,
          "endpoint 255 exact");

    for (int x = 0; x < 4; x++) {
        /* ideal projection weight for the grey ramp */
        int v = x * 85;
        int64_t num = 3LL * 255 * v, den = 3LL * 255 * 255;
        int target = (int)((num * 64 + den / 2) / den);
        int best = 0, best_err = 0x7FFFFFFF;
        for (int r = 0; r < cfg.wlevels; r++) {
            int err = (int)cfg.wunq[r] - target;
            if (err < 0) err = -err;
            if (err < best_err) { best_err = err; best = r; }
        }
        uint32_t w = cfg.wunq[best];
        uint32_t c = (0u * (64 - w) + 65535u * w + 32) >> 6;
        uint8_t expect = (uint8_t)(c >> 8);
        for (int y = 0; y < 4; y++) {
            const uint8_t *p = out + 4 * (y * 4 + x);
            check(p[0] == expect && p[1] == expect && p[2] == expect,
                  "CEM12 manual texel (rgb)");
            check(p[3] == 255, "CEM12 manual texel (alpha)");
        }
    }
}

/* Hand-packed CEM 2 (HDR luminance) block: exercises the LNS/HDR path. */
static void test_cem2_hdr_manual() {
    uint32_t mode = 0;
    bool found = false;
    for (uint32_t m = 0; m < 2048 && !found; m++) {
        BlockMode bm = decode_block_mode(m);
        if (!bm.error && !bm.void_extent && !bm.dual && bm.gw == 4 &&
            bm.gh == 4 && bm.wparams.mode == ISE_TRIT && bm.wparams.bits == 1) {
            mode = m;
            found = true;
        }
    }
    check(found, "CEM2 block mode found");
    if (!found) return;
    Block128 b;
    b.set_bits(0, 11, mode);
    b.set_bits(13, 4, 2);            /* CEM 2 */
    uint8_t craw[2] = { 0x40, 0x80 };
    IseParams cq = { ISE_BITS, 8 };  /* = max quant for 69 bits, 2 values */
    encode_ise(b, 17, craw, 2, cq);
    /* weights: all raw 0 -> all bits zero, nothing to write */
    uint8_t blk[16];
    b.store(blk);
    float outf[4 * 4 * 4];
    astc_decode_f32(TEXC_FORMAT_ASTC_4x4, blk, 16, 4, 4, outf);
    /* w=0 everywhere -> c = (0x40 << 4) << 4 = 0x4000; LNS: E=8, M=0 ->
     * half = 0x2000 = 2^-7 */
    bool ok = true;
    for (int i = 0; i < 16; i++) {
        ok = ok && outf[4 * i + 0] == 0.0078125f;
        ok = ok && outf[4 * i + 1] == 0.0078125f;
        ok = ok && outf[4 * i + 2] == 0.0078125f;
        ok = ok && outf[4 * i + 3] == 1.0f;
    }
    check(ok, "CEM2 HDR luminance f32 KAT");
    uint8_t out[4 * 4 * 4];
    astc_decode(TEXC_FORMAT_ASTC_4x4, blk, 16, 4, 4, out);
    ok = true;
    for (int i = 0; i < 16; i++)
        ok = ok && out[4 * i + 0] == 2 && out[4 * i + 1] == 2 &&
             out[4 * i + 2] == 2 && out[4 * i + 3] == 255;
    check(ok, "CEM2 HDR luminance 8-bit KAT");
}

/* Weight infill: an 8x8 block whose weight grid is smaller than the block
 * must still decode into plausible, monotone values. */
static void test_infill() {
    EncConfig cfg = make_config(8, 8, false);
    check(cfg.valid, "8x8 config valid");
    check(cfg.gw < 8 || cfg.gh < 8, "8x8 config exercises infill");
    uint8_t img[8 * 8 * 4];
    for (int y = 0; y < 8; y++)
        for (int x = 0; x < 8; x++) {
            uint8_t *p = img + 4 * (y * 8 + x);
            p[0] = (uint8_t)(x * 255 / 7);
            p[1] = (uint8_t)(y * 255 / 7);
            p[2] = 128;
            p[3] = 255;
        }
    uint8_t blk[16], out[8 * 8 * 4];
    astc_encode(TEXC_FORMAT_ASTC_8x8, img, 8, 8, blk, nullptr);
    astc_decode(TEXC_FORMAT_ASTC_8x8, blk, 16, 8, 8, out);
    bool mono = true, plausible = true;
    for (int y = 0; y < 8; y++)
        for (int x = 0; x < 8; x++) {
            const uint8_t *p = out + 4 * (y * 8 + x);
            if (x > 0 && (int)p[0] < (int)out[4 * (y * 8 + x - 1)] - 4)
                mono = false;
            if (y > 0 && (int)p[1] < (int)out[4 * ((y - 1) * 8 + x) + 1] - 4)
                mono = false;
            int e2 = (int)p[2] - 128;
            if (e2 < -24 || e2 > 24) plausible = false;
            if (p[3] != 255) plausible = false;
        }
    check(mono, "infill gradient monotonicity");
    check(plausible, "infill plausible values");
}

} /* anonymous namespace */

int astc_selftest() {
    test_ise_tables();
    test_unquant_tables();
    test_ise_roundtrip();
    test_void_extent_kat();
    test_cem12_manual();
    test_cem2_hdr_manual();
    test_infill();
    test_uniform();
    test_roundtrip_psnr();
    {
        /* report chosen encoder configs */
        printf("Encoder configs (grid, weight levels, colour levels):\n");
        for (int fi = 0; fi < 14; fi++) {
            uint32_t bw, bh;
            astc_dims(k_fmts[fi].f, bw, bh);
            EncConfig cs = make_config((int)bw, (int)bh, false);
            EncConfig cd = make_config((int)bw, (int)bh, true);
            printf("  ASTC %-6s single: grid %dx%d wq %d cq %d wbits %d"
                   " | dual: grid %dx%d wq %d cq %d wbits %d\n",
                   k_fmts[fi].name, cs.gw, cs.gh, cs.wlevels, cs.clevels,
                   cs.wbits, cd.gw, cd.gh, cd.wlevels, cd.clevels, cd.wbits);
        }
    }
    if (g_fails == 0)
        printf("ASTC selftest: all tests passed\n");
    else
        printf("ASTC selftest: %d FAILURES\n", g_fails);
    return g_fails;
}

} /* namespace texc */

int main() {
    return texc::astc_selftest() == 0 ? 0 : 1;
}

#endif /* TEXC_SELFTEST */
