// SPDX-License-Identifier: GPL-3.0-or-later
#include "core/cpu.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#if defined(__x86_64__) || defined(_M_X64)
#include <immintrin.h>
#endif

#if defined(_MSC_VER)
#include <intrin.h>
#else
#include <stdatomic.h>
#endif

typedef struct {
    s32 base;
    s32 dec;
} EstimateEntry;

/* Adapted from Dolphin Emulator's Common/FloatUtils.cpp.
 * Copyright 2018 Dolphin Emulator Project, GPL-2.0-or-later. */
static const EstimateEntry frsqrte_table[32] = {
    {0x1a7e800, -0x568}, {0x17cb800, -0x4f3}, {0x1552800, -0x48d}, {0x130c000, -0x435},
    {0x10f2000, -0x3e7}, {0x0eff000, -0x3a2}, {0x0d2e000, -0x365}, {0x0b7c000, -0x32e},
    {0x09e5000, -0x2fc}, {0x0867000, -0x2d0}, {0x06ff000, -0x2a8}, {0x05ab800, -0x283},
    {0x046a000, -0x261}, {0x0339800, -0x243}, {0x0218800, -0x226}, {0x0105800, -0x20b},
    {0x3ffa000, -0x7a4}, {0x3c29000, -0x700}, {0x38aa000, -0x670}, {0x3572000, -0x5f2},
    {0x3279000, -0x584}, {0x2fb7000, -0x524}, {0x2d26000, -0x4cc}, {0x2ac0000, -0x47e},
    {0x2881000, -0x43a}, {0x2665000, -0x3fa}, {0x2468000, -0x3c2}, {0x2287000, -0x38e},
    {0x20c1000, -0x35e}, {0x1f12000, -0x332}, {0x1d79000, -0x30a}, {0x1bf4000, -0x2e6},
};

static const EstimateEntry fres_table[32] = {
    {0x7ff800, 0x3e1}, {0x783800, 0x3a7}, {0x70ea00, 0x371}, {0x6a0800, 0x340},
    {0x638800, 0x313}, {0x5d6200, 0x2ea}, {0x579000, 0x2c4}, {0x520800, 0x2a0},
    {0x4cc800, 0x27f}, {0x47ca00, 0x261}, {0x430800, 0x245}, {0x3e8000, 0x22a},
    {0x3a2c00, 0x212}, {0x360800, 0x1fb}, {0x321400, 0x1e5}, {0x2e4a00, 0x1d1},
    {0x2aa800, 0x1be}, {0x272c00, 0x1ac}, {0x23d600, 0x19b}, {0x209e00, 0x18b},
    {0x1d8800, 0x17c}, {0x1a9000, 0x16e}, {0x17ae00, 0x15b}, {0x14f800, 0x15b},
    {0x124400, 0x143}, {0x0fbe00, 0x143}, {0x0d3800, 0x12d}, {0x0ade00, 0x12d},
    {0x088400, 0x11a}, {0x065000, 0x11a}, {0x041c00, 0x108}, {0x020c00, 0x106},
};

f64 ppc_approx_rsqrt(f64 value) {
    u64 bits = f64_bits(value);
    u64 mantissa = bits & 0x000FFFFFFFFFFFFFull;
    u64 sign = bits & 0x8000000000000000ull;
    s64 exponent = (s64)(bits & 0x7FF0000000000000ull);

    if (mantissa == 0 && exponent == 0)
        return f64_value(sign | 0x7FF0000000000000ull);
    if (exponent == (s64)0x7FF0000000000000ull) {
        if (mantissa == 0)
            return sign ? f64_value(0x7FF8000000000000ull) : 0.0;
        return f64_value(bits | 0x0008000000000000ull);
    }
    if (sign)
        return f64_value(0x7FF8000000000000ull);

    if (exponent == 0) {
        do {
            exponent -= (s64)0x0010000000000000ull;
            mantissa <<= 1;
        } while ((mantissa & 0x0010000000000000ull) == 0);
        mantissa &= 0x000FFFFFFFFFFFFFull;
        exponent += (s64)0x0010000000000000ull;
    }

    u64 exponent_lsb = (u64)exponent & 0x0010000000000000ull;
    exponent = ((s64)0x3FF0000000000000ull -
                (exponent - (s64)0x3FE0000000000000ull) / 2) &
               (s64)0x7FF0000000000000ull;
    u32 i = (u32)((exponent_lsb | mantissa) >> 37);
    const EstimateEntry* entry = &frsqrte_table[i / 2048u];
    bits = (u64)exponent |
           ((u64)(entry->base + entry->dec * (s32)(i % 2048u)) << 26);
    return f64_value(bits);
}

f64 ppc_approx_reciprocal(f64 value) {
    u64 bits = f64_bits(value);
    u64 mantissa = bits & 0x000FFFFFFFFFFFFFull;
    u64 sign = bits & 0x8000000000000000ull;
    u64 exponent = bits & 0x7FF0000000000000ull;

    if (mantissa == 0 && exponent == 0)
        return f64_value(sign | 0x7FF0000000000000ull);
    if (exponent == 0x7FF0000000000000ull) {
        if (mantissa == 0)
            return f64_value(sign);
        return f64_value(bits | 0x0008000000000000ull);
    }
    if (exponent < (895ull << 52))
        return f64_value(sign | 0x47EFFFFFE0000000ull);
    if (exponent >= (1149ull << 52))
        return f64_value(sign);

    exponent = 0x7FD0000000000000ull - exponent;
    u32 i = (u32)(mantissa >> 37);
    const EstimateEntry* entry = &fres_table[i / 1024u];
    bits = sign | exponent |
           ((u64)(entry->base - (entry->dec * (s32)(i % 1024u) + 1) / 2) << 29);
    return f64_value(bits);
}

/* FPSCR bit masks (IBM bits 0..31 mapped to LSB-relative masks). */
#define FPSCR_FX_BIT     0x80000000u
#define FPSCR_FEX_BIT    0x40000000u
#define FPSCR_VX_BIT     0x20000000u
#define FPSCR_OX_BIT     0x10000000u
#define FPSCR_UX_BIT     0x08000000u
#define FPSCR_ZX_BIT     0x04000000u
#define FPSCR_XX_BIT     0x02000000u
#define FPSCR_VXSNAN_BIT 0x01000000u
#define FPSCR_VXISI_BIT  0x00800000u
#define FPSCR_VXIDI_BIT  0x00400000u
#define FPSCR_VXZDZ_BIT  0x00200000u
#define FPSCR_VXIMZ_BIT  0x00100000u
#define FPSCR_VXVC_BIT   0x00080000u
#define FPSCR_FR_BIT     0x00040000u
#define FPSCR_FI_BIT     0x00020000u
#define FPSCR_C_BIT      0x00010000u
#define FPSCR_FPCC_MASK  0x0000F000u
#define FPSCR_VXSOFT_BIT 0x00000400u
#define FPSCR_VXSQRT_BIT 0x00000200u
#define FPSCR_VXCVI_BIT  0x00000100u
#define FPSCR_VE_BIT     0x00000080u
#define FPSCR_OE_BIT     0x00000040u
#define FPSCR_UE_BIT     0x00000020u
#define FPSCR_ZE_BIT     0x00000010u
#define FPSCR_XE_BIT     0x00000008u
#define FPSCR_NI_BIT     0x00000004u
#define FPSCR_RN_MASK    0x00000003u
#define FPSCR_VX_ANY_MASK (FPSCR_VXSNAN_BIT | FPSCR_VXISI_BIT | FPSCR_VXIDI_BIT | \
                           FPSCR_VXZDZ_BIT | FPSCR_VXIMZ_BIT | FPSCR_VXVC_BIT | \
                           FPSCR_VXSOFT_BIT | FPSCR_VXSQRT_BIT | FPSCR_VXCVI_BIT)
#define FPSCR_ANY_X_MASK (FPSCR_OX_BIT | FPSCR_UX_BIT | FPSCR_ZX_BIT | FPSCR_XX_BIT | \
                           FPSCR_VX_ANY_MASK)
#define PPC_F64_QNAN_BITS 0x7FF8000000000000ull

void ppc_fpscr_updated(CPUState* cpu) {
    const u32 any_e = 0x000000F8u;
    u32 fpscr = cpu->fpscr;
    fpscr = (fpscr & ~FPSCR_VX_BIT) | ((fpscr & FPSCR_VX_ANY_MASK) ? FPSCR_VX_BIT : 0u);
    fpscr = (fpscr & ~FPSCR_FEX_BIT) |
            ((((fpscr >> 22) & fpscr & any_e) != 0) ? FPSCR_FEX_BIT : 0u);
    cpu->fpscr = fpscr;
}

/* Arm the host FPU rounding + flush-to-zero mode from guest FPSCR RN/NI,
 * mirroring Dolphin's RoundingModeUpdated -> Common::FPU::SetSIMDMode: the
 * same FPCR/MXCSR bits Dolphin writes, so native code and Dolphin's
 * interpreter compute under an identical host FP environment. */
static void ppc_arm_host_fp_mode(CPUState* cpu) {
#if defined(__aarch64__)
    static const u64 rmode_table[4] = {
        0ull << 22, /* nearest */
        3ull << 22, /* toward zero */
        1ull << 22, /* +inf */
        2ull << 22, /* -inf */
    };
    const u64 FPCR_FZ = 1ull << 24;
    const u64 FPCR_AH = 1ull << 1;
    const u64 FPCR_FIZ = 1ull << 0;
    const u64 flush_mask = FPCR_FZ | FPCR_AH | FPCR_FIZ;
    const u64 rmode_mask = 3ull << 22;
    u64 fpcr;
    __asm__ __volatile__("mrs %0, fpcr" : "=r"(fpcr));
    fpcr &= ~(flush_mask | rmode_mask);
    fpcr |= rmode_table[cpu->fpscr & FPSCR_RN_MASK];
    if (cpu->fpscr & FPSCR_NI_BIT)
        fpcr |= FPCR_FZ | FPCR_AH;
    __asm__ __volatile__("msr fpcr, %0" : : "r"(fpcr));
#elif defined(__x86_64__) || defined(_M_X64)
    static const u32 rmode_table[4] = {
        0u << 13, /* nearest */
        3u << 13, /* toward zero */
        2u << 13, /* +inf */
        1u << 13, /* -inf */
    };
    u32 csr = _mm_getcsr();
    csr &= ~((3u << 13) | 0x8040u);
    csr |= rmode_table[cpu->fpscr & FPSCR_RN_MASK];
    if (cpu->fpscr & FPSCR_NI_BIT)
        csr |= 0x8040u; /* FTZ + DAZ */
    _mm_setcsr(csr);
#else
    (void)cpu;
#endif
}

void ppc_fpscr_control_updated(CPUState* cpu) {
    ppc_fpscr_updated(cpu);
    ppc_arm_host_fp_mode(cpu);
}

static void set_fp_exception(CPUState* cpu, u32 bit);

void ppc_mtfsb0_op(CPUState* cpu, u8 bit) {
    cpu->fpscr &= ~(0x80000000u >> bit);
    ppc_fpscr_control_updated(cpu);
}

void ppc_mtfsb1_op(CPUState* cpu, u8 bit) {
    u32 mask = 0x80000000u >> bit;
    if (mask & FPSCR_ANY_X_MASK)
        set_fp_exception(cpu, mask); /* exception bits ride the FX path */
    else
        cpu->fpscr |= mask;
    ppc_fpscr_control_updated(cpu);
}

static void set_fp_exception(CPUState* cpu, u32 bit) {
    if ((cpu->fpscr & bit) != bit)
        cpu->fpscr |= FPSCR_FX_BIT;
    cpu->fpscr |= bit;
    ppc_fpscr_updated(cpu);
}

static void clear_fifr(CPUState* cpu) {
    cpu->fpscr &= ~(FPSCR_FI_BIT | FPSCR_FR_BIT);
}

static bool is_snan(f64 value) {
    u64 bits = f64_bits(value);
    u64 fraction = bits & 0x000FFFFFFFFFFFFFull;
    return (bits & 0x7FF0000000000000ull) == 0x7FF0000000000000ull &&
           fraction != 0 && (fraction & 0x0008000000000000ull) == 0;
}

static u32 classify_f64(f64 value) {
    u64 bits = f64_bits(value);
    u64 sign = bits >> 63;
    u64 exponent = bits & 0x7FF0000000000000ull;
    u64 fraction = bits & 0x000FFFFFFFFFFFFFull;
    if (exponent == 0x7FF0000000000000ull)
        return fraction ? 0x11u : (sign ? 0x09u : 0x05u);
    if (exponent == 0)
        return fraction ? (sign ? 0x18u : 0x14u) : (sign ? 0x12u : 0x02u);
    return sign ? 0x08u : 0x04u;
}

static u32 classify_f32(f32 value) {
    u32 bits;
    memcpy(&bits, &value, sizeof(bits));
    u32 sign = bits >> 31;
    u32 exponent = bits & 0x7F800000u;
    u32 fraction = bits & 0x007FFFFFu;
    if (exponent == 0x7F800000u)
        return fraction ? 0x11u : (sign ? 0x09u : 0x05u);
    if (exponent == 0)
        return fraction ? (sign ? 0x18u : 0x14u) : (sign ? 0x12u : 0x02u);
    return sign ? 0x08u : 0x04u;
}

static void set_fprf(CPUState* cpu, u32 value) {
    cpu->fpscr = (cpu->fpscr & ~(0x1Fu << 12)) | ((value & 0x1Fu) << 12);
}

bool ppc_fres(CPUState* cpu, f64 value, f64* result) {
    if (value == 0.0) {
        set_fp_exception(cpu, 0x04000000u);
        clear_fifr(cpu);
        if (cpu->fpscr & 0x10u)
            return false;
    } else if (is_snan(value)) {
        set_fp_exception(cpu, 0x01000000u);
        clear_fifr(cpu);
        if (cpu->fpscr & 0x80u)
            return false;
    } else if (isnan(value) || isinf(value)) {
        clear_fifr(cpu);
    }

    *result = ppc_approx_reciprocal(value);
    set_fprf(cpu, classify_f32((f32)*result));
    return true;
}

bool ppc_frsqrte(CPUState* cpu, f64 value, f64* result) {
    if (value < 0.0) {
        set_fp_exception(cpu, 0x00000200u);
        clear_fifr(cpu);
        if (cpu->fpscr & 0x80u)
            return false;
    } else if (value == 0.0) {
        set_fp_exception(cpu, 0x04000000u);
        clear_fifr(cpu);
        if (cpu->fpscr & 0x10u)
            return false;
    } else if (is_snan(value)) {
        set_fp_exception(cpu, 0x01000000u);
        clear_fifr(cpu);
        if (cpu->fpscr & 0x80u)
            return false;
    } else if (isnan(value) || isinf(value)) {
        clear_fifr(cpu);
    }

    *result = ppc_approx_rsqrt(value);
    set_fprf(cpu, classify_f64(*result));
    return true;
}

void ppc_ps_res(CPUState* cpu, f64 a, f64 b, f64* result_a, f64* result_b) {
    if (a == 0.0 || b == 0.0) {
        set_fp_exception(cpu, 0x04000000u);
        clear_fifr(cpu);
    }
    if (is_snan(a) || is_snan(b))
        set_fp_exception(cpu, 0x01000000u);
    if (isnan(a) || isinf(a) || isnan(b) || isinf(b))
        clear_fifr(cpu);
    *result_a = ppc_approx_reciprocal(a);
    *result_b = ppc_approx_reciprocal(b);
    set_fprf(cpu, classify_f32((f32)*result_a));
}

void ppc_ps_rsqrte(CPUState* cpu, f64 a, f64 b, f64* result_a, f64* result_b) {
    if (a == 0.0 || b == 0.0) {
        set_fp_exception(cpu, 0x04000000u);
        clear_fifr(cpu);
    }
    if (a < 0.0 || b < 0.0) {
        set_fp_exception(cpu, 0x00000200u);
        clear_fifr(cpu);
    }
    if (is_snan(a) || is_snan(b))
        set_fp_exception(cpu, 0x01000000u);
    if (isnan(a) || isinf(a) || isnan(b) || isinf(b))
        clear_fifr(cpu);
    *result_a = ppc_approx_rsqrt(a);
    *result_b = ppc_approx_rsqrt(b);
    set_fprf(cpu, classify_f32((f32)*result_a));
}

static unsigned leading_zeroes_u64(u64 value) {
    unsigned count = 0;
    while ((value & 0x8000000000000000ull) == 0) {
        value <<= 1;
        count++;
    }
    return count;
}

static f64 force_25_bit(f64 value) {
    u64 bits = f64_bits(value);
    u64 fraction = bits & 0x000FFFFFFFFFFFFFull;
    u64 keep_mask = 0xFFFFFFFFF8000000ull;
    u64 round = 0x0000000008000000ull;

    if ((bits & 0x7FF0000000000000ull) == 0 && fraction != 0) {
        unsigned shift = leading_zeroes_u64(fraction) - 11;
        if (shift < 28) {
            keep_mask = ~((1ull << (27 - shift)) - 1);
            round >>= shift;
        } else {
            keep_mask = ~0ull;
            round = 0;
        }
    }

    bits = (bits & keep_mask) + (bits & round);
    return f64_value(bits);
}

bool ppc_fma(CPUState* cpu, f64 a, f64 c, f64 b, bool single,
             bool subtract, bool negative, f64* output) {
    f64 addend = subtract ? -b : b;
    f64 result;

    if (!single) {
        result = fma(a, c, addend);
    } else {
        f64 rounded_c = force_25_bit(c);
        result = fma(a, rounded_c, addend);
        u64 bits = f64_bits(result);
        if ((bits & 0x000000001FFFFFFFull) == 0x0000000010000000ull) {
            f64 a_prime = addend - result;
            f64 b_prime = result + a_prime;
            f64 error = fma(a, rounded_c, a_prime) + (addend - b_prime);
            if (error != 0.0) {
                if ((error > 0.0) == (result > 0.0)) bits++;
                else bits--;
                result = f64_value(bits);
            }
        }
        result = (f64)(f32)result;
    }

    if (isnan(result)) {
        u32 invalid = 0;
        if (is_snan(a) || is_snan(b) || is_snan(c))
            invalid |= 0x01000000u;

        clear_fifr(cpu);
        if (isnan(a)) {
            result = f64_value(f64_bits(a) | 0x0008000000000000ull);
        } else if (isnan(b)) {
            result = f64_value(f64_bits(b) | 0x0008000000000000ull);
        } else if (isnan(c)) {
            result = f64_value(f64_bits(c) | 0x0008000000000000ull);
        } else {
            bool invalid_multiply = (a == 0.0 && isinf(c)) ||
                                    (isinf(a) && c == 0.0);
            invalid |= invalid_multiply ? 0x00100000u : 0x00800000u;
            result = f64_value(0x7FF8000000000000ull);
        }

        if (invalid) {
            set_fp_exception(cpu, invalid);
            if (cpu->fpscr & 0x80u)
                return false;
        }
    } else if (isinf(a) || isinf(b) || isinf(c)) {
        clear_fifr(cpu);
    }

    if (negative && !isnan(result))
        result = -result;
    set_fprf(cpu, single ? classify_f32((f32)result) : classify_f64(result));
    *output = result;
    return true;
}

/* ====================================================================
 * Instruction-shaped FP unit, mirrored bit-exactly from Dolphin's
 * interpreter (Interpreter_FloatingPoint.cpp, Interpreter_Paired.cpp,
 * Interpreter_FPUtils.h). Dolphin is the lockstep oracle: every rule
 * here (NaN propagation order a->b->c, Force25Bit frC rounding, the
 * single-precision FMA round-once tie fix, Fill vs SetPS0 lane policy,
 * FPRF/FI/FR updates, VE/ZE write gating) matches its source.
 * ==================================================================== */

typedef struct {
    f64 value;
    u32 exception; /* last FPSCR exception set by the NI_* helper */
} FPRes;

static f64 make_quiet(f64 value) {
    return f64_value(f64_bits(value) | 0x0008000000000000ull);
}

/* ForceSingle/ForceDouble: on x86-64/arm64 Dolphin arms hardware
 * flush-to-zero when FPSCR.NI is set (cpu_info.bFlushToZero == true), so
 * the only software part left is the pre-rounding subnormal-single flush
 * quirk. ppc_arm_host_fp_mode keeps the hardware side in step. */
static f32 force_single(const CPUState* cpu, f64 value) {
    if (cpu->fpscr & FPSCR_NI_BIT) {
        u64 no_sign = f64_bits(value) & 0x7FFFFFFFFFFFFFFFull;
        if (no_sign < 0x3810000000000000ull) {
            u32 flushed = (u32)((f64_bits(value) & 0x8000000000000000ull) >> 32);
            return f32_value(flushed);
        }
    }
    return (f32)value;
}

static f64 force_double(const CPUState* cpu, f64 d) {
    (void)cpu;
    return d;
}

/* Dolphin Force25Bit: round frC's mantissa to 25 bits (ties up), with the
 * subnormal arithmetic-shift normalization. */
static f64 force_25bit_c(f64 d) {
    u64 integral = f64_bits(d);
    u64 exponent = integral & 0x7FF0000000000000ull;
    u64 fraction = integral & 0x000FFFFFFFFFFFFFull;

    if (exponent == 0 && fraction != 0) {
        s64 keep_mask = (s64)0xFFFFFFFFF8000000ll;
        u64 round = 0x8000000u;
        unsigned shift = leading_zeroes_u64(fraction) - 11u;
        keep_mask >>= shift;
        round >>= shift;
        integral = (integral & (u64)keep_mask) + (integral & round);
    } else {
        integral = (integral & 0xFFFFFFFFF8000000ull) + (integral & 0x8000000ull);
    }
    return f64_value(integral);
}

static FPRes ni_add(CPUState* cpu, f64 a, f64 b) {
    FPRes result = {a + b, 0};

    if (isnan(result.value)) {
        if (is_snan(a) || is_snan(b)) {
            result.exception = FPSCR_VXSNAN_BIT;
            set_fp_exception(cpu, FPSCR_VXSNAN_BIT);
        }
        clear_fifr(cpu);
        if (isnan(a)) { result.value = make_quiet(a); return result; }
        if (isnan(b)) { result.value = make_quiet(b); return result; }
        result.exception = FPSCR_VXISI_BIT;
        set_fp_exception(cpu, FPSCR_VXISI_BIT);
        result.value = f64_value(PPC_F64_QNAN_BITS);
        return result;
    }

    if (isinf(a) || isinf(b))
        clear_fifr(cpu);
    return result;
}

static FPRes ni_sub(CPUState* cpu, f64 a, f64 b) {
    FPRes result = {a - b, 0};

    if (isnan(result.value)) {
        if (is_snan(a) || is_snan(b)) {
            result.exception = FPSCR_VXSNAN_BIT;
            set_fp_exception(cpu, FPSCR_VXSNAN_BIT);
        }
        clear_fifr(cpu);
        if (isnan(a)) { result.value = make_quiet(a); return result; }
        if (isnan(b)) { result.value = make_quiet(b); return result; }
        result.exception = FPSCR_VXISI_BIT;
        set_fp_exception(cpu, FPSCR_VXISI_BIT);
        result.value = f64_value(PPC_F64_QNAN_BITS);
        return result;
    }

    if (isinf(a) || isinf(b))
        clear_fifr(cpu);
    return result;
}

static FPRes ni_mul(CPUState* cpu, f64 a, f64 b) {
    FPRes result = {a * b, 0};

    if (isnan(result.value)) {
        if (is_snan(a) || is_snan(b)) {
            result.exception = FPSCR_VXSNAN_BIT;
            set_fp_exception(cpu, FPSCR_VXSNAN_BIT);
        }
        clear_fifr(cpu);
        if (isnan(a)) { result.value = make_quiet(a); return result; }
        if (isnan(b)) { result.value = make_quiet(b); return result; }
        result.exception = FPSCR_VXIMZ_BIT;
        set_fp_exception(cpu, FPSCR_VXIMZ_BIT);
        result.value = f64_value(PPC_F64_QNAN_BITS);
        return result;
    }

    return result;
}

static FPRes ni_div(CPUState* cpu, f64 a, f64 b) {
    FPRes result = {a / b, 0};

    if (isinf(result.value)) {
        if (b == 0.0) {
            result.exception = FPSCR_ZX_BIT;
            set_fp_exception(cpu, FPSCR_ZX_BIT);
            return result;
        }
    } else if (isnan(result.value)) {
        if (is_snan(a) || is_snan(b)) {
            result.exception = FPSCR_VXSNAN_BIT;
            set_fp_exception(cpu, FPSCR_VXSNAN_BIT);
        }
        clear_fifr(cpu);
        if (isnan(a)) { result.value = make_quiet(a); return result; }
        if (isnan(b)) { result.value = make_quiet(b); return result; }
        if (b == 0.0) {
            result.exception = FPSCR_VXZDZ_BIT;
            set_fp_exception(cpu, FPSCR_VXZDZ_BIT);
        } else if (isinf(a) && isinf(b)) {
            result.exception = FPSCR_VXIDI_BIT;
            set_fp_exception(cpu, FPSCR_VXIDI_BIT);
        }
        result.value = f64_value(PPC_F64_QNAN_BITS);
        return result;
    }

    return result;
}

/* NI_madd_msub: (a * c) + b with NaN priority a, b, c. The single-precision
 * form rounds frC to 25 bits, computes a 64-bit fused FMA, and repairs
 * round-to-nearest even-ties with Moller's 2Sum error term so the final
 * 32-bit rounding happens exactly once (Dolphin's algorithm verbatim). */
static FPRes ni_madd_msub(CPUState* cpu, f64 a, f64 c, f64 b, bool sub, bool single) {
    FPRes result = {0.0, 0};

    if (!single) {
        result.value = fma(a, c, sub ? -b : b);
    } else {
        f64 c_round = force_25bit_c(c);
        f64 b_sign = sub ? -b : b;
        result.value = fma(a, c_round, b_sign);

        u64 result_bits = f64_bits(result.value);
        const u64 D_MASK = 0x000000001FFFFFFFull;
        const u64 EVEN_TIE = 0x0000000010000000ull;
        if ((result_bits & D_MASK) == EVEN_TIE) {
            f64 a_prime = b_sign - result.value;
            f64 b_prime = result.value + a_prime;
            f64 delta_a = fma(a, c_round, a_prime);
            f64 delta_b = b_sign - b_prime;
            f64 error = delta_a + delta_b;
            if (error != 0.0) {
                if ((error > 0.0) == (result.value > 0.0))
                    result.value = f64_value(result_bits + 1);
                else
                    result.value = f64_value(result_bits - 1);
            }
        }
    }

    if (isnan(result.value)) {
        if (is_snan(a) || is_snan(b) || is_snan(c)) {
            result.exception = FPSCR_VXSNAN_BIT;
            set_fp_exception(cpu, FPSCR_VXSNAN_BIT);
        }
        clear_fifr(cpu);
        if (isnan(a)) { result.value = make_quiet(a); return result; }
        if (isnan(b)) { result.value = make_quiet(b); return result; }
        if (isnan(c)) { result.value = make_quiet(c); return result; }
        result.exception = isnan(a * c) ? FPSCR_VXIMZ_BIT : FPSCR_VXISI_BIT;
        set_fp_exception(cpu, result.exception);
        result.value = f64_value(PPC_F64_QNAN_BITS);
        return result;
    }

    if (isinf(a) || isinf(b) || isinf(c))
        clear_fifr(cpu);
    return result;
}

static bool fp_invalid_gated(const CPUState* cpu, const FPRes* res) {
    return (cpu->fpscr & FPSCR_VE_BIT) != 0 && (res->exception & FPSCR_VX_ANY_MASK) != 0;
}

/* Write-back helpers: single results fill both PS lanes and classify as
 * float; double results write PS0 only and classify as double. */
static void fp_write_single(CPUState* cpu, u8 d, f32 rounded) {
    cpu->fpr[d] = (f64)rounded;
    cpu->ps1[d] = (f64)rounded;
    set_fprf(cpu, classify_f32(rounded));
}

static void fp_write_double(CPUState* cpu, u8 d, f64 value) {
    cpu->fpr[d] = value;
    set_fprf(cpu, classify_f64(value));
}

void ppc_fadds(CPUState* cpu, u8 d, u8 a, u8 b) {
    FPRes sum = ni_add(cpu, cpu->fpr[a], cpu->fpr[b]);
    if (!fp_invalid_gated(cpu, &sum))
        fp_write_single(cpu, d, force_single(cpu, sum.value));
}

void ppc_fsubs(CPUState* cpu, u8 d, u8 a, u8 b) {
    FPRes diff = ni_sub(cpu, cpu->fpr[a], cpu->fpr[b]);
    if (!fp_invalid_gated(cpu, &diff))
        fp_write_single(cpu, d, force_single(cpu, diff.value));
}

void ppc_fmuls(CPUState* cpu, u8 d, u8 a, u8 c) {
    f64 c_value = force_25bit_c(cpu->fpr[c]);
    FPRes product = ni_mul(cpu, cpu->fpr[a], c_value);
    if (!fp_invalid_gated(cpu, &product)) {
        fp_write_single(cpu, d, force_single(cpu, product.value));
        cpu->fpscr &= ~(FPSCR_FI_BIT | FPSCR_FR_BIT);
    }
}

void ppc_fdivs(CPUState* cpu, u8 d, u8 a, u8 b) {
    FPRes quotient = ni_div(cpu, cpu->fpr[a], cpu->fpr[b]);
    bool not_divide_by_zero =
        (cpu->fpscr & FPSCR_ZE_BIT) == 0 || quotient.exception != FPSCR_ZX_BIT;
    if (not_divide_by_zero && !fp_invalid_gated(cpu, &quotient))
        fp_write_single(cpu, d, force_single(cpu, quotient.value));
}

void ppc_fadd(CPUState* cpu, u8 d, u8 a, u8 b) {
    FPRes sum = ni_add(cpu, cpu->fpr[a], cpu->fpr[b]);
    if (!fp_invalid_gated(cpu, &sum))
        fp_write_double(cpu, d, force_double(cpu, sum.value));
}

void ppc_fsub(CPUState* cpu, u8 d, u8 a, u8 b) {
    FPRes diff = ni_sub(cpu, cpu->fpr[a], cpu->fpr[b]);
    if (!fp_invalid_gated(cpu, &diff))
        fp_write_double(cpu, d, force_double(cpu, diff.value));
}

void ppc_fmul(CPUState* cpu, u8 d, u8 a, u8 c) {
    FPRes product = ni_mul(cpu, cpu->fpr[a], cpu->fpr[c]);
    if (!fp_invalid_gated(cpu, &product)) {
        fp_write_double(cpu, d, force_double(cpu, product.value));
        cpu->fpscr &= ~(FPSCR_FI_BIT | FPSCR_FR_BIT);
    }
}

void ppc_fdiv(CPUState* cpu, u8 d, u8 a, u8 b) {
    FPRes quotient = ni_div(cpu, cpu->fpr[a], cpu->fpr[b]);
    bool not_divide_by_zero =
        (cpu->fpscr & FPSCR_ZE_BIT) == 0 || quotient.exception != FPSCR_ZX_BIT;
    if (not_divide_by_zero && !fp_invalid_gated(cpu, &quotient))
        fp_write_double(cpu, d, force_double(cpu, quotient.value));
}

void ppc_fmadd_op(CPUState* cpu, u8 d, u8 a, u8 c, u8 b,
                  bool single, bool subtract, bool negative) {
    FPRes product = ni_madd_msub(cpu, cpu->fpr[a], cpu->fpr[c], cpu->fpr[b],
                                 subtract, single);
    if (fp_invalid_gated(cpu, &product))
        return;

    if (single) {
        f32 tmp = force_single(cpu, product.value);
        f32 result = (negative && !isnan(tmp)) ? -tmp : tmp;
        cpu->fpr[d] = (f64)result;
        cpu->ps1[d] = (f64)result;
        if (!subtract && !negative) { /* fmaddsx quirk: FI tracks rounding */
            cpu->fpscr = (cpu->fpscr & ~(FPSCR_FI_BIT | FPSCR_FR_BIT)) |
                         ((product.value != (f64)tmp) ? FPSCR_FI_BIT : 0u);
        }
        set_fprf(cpu, classify_f32(result));
    } else {
        f64 tmp = force_double(cpu, product.value);
        f64 result = (negative && !isnan(tmp)) ? -tmp : tmp;
        cpu->fpr[d] = result;
        set_fprf(cpu, classify_f64(result));
    }
}

void ppc_frsp(CPUState* cpu, u8 d, u8 b) {
    f64 value = cpu->fpr[b];
    f32 rounded = force_single(cpu, value);

    if (isnan(value)) {
        bool snan = is_snan(value);
        if (snan)
            set_fp_exception(cpu, FPSCR_VXSNAN_BIT);
        if (!snan || (cpu->fpscr & FPSCR_VE_BIT) == 0) {
            cpu->fpr[d] = (f64)rounded;
            cpu->ps1[d] = (f64)rounded;
            set_fprf(cpu, classify_f32(rounded));
        }
        clear_fifr(cpu);
    } else {
        if (value != (f64)rounded) {
            set_fp_exception(cpu, FPSCR_XX_BIT);
            cpu->fpscr |= FPSCR_FI_BIT;
        } else {
            cpu->fpscr &= ~FPSCR_FI_BIT;
        }
        cpu->fpscr = (cpu->fpscr & ~FPSCR_FR_BIT) |
                     ((fabs((f64)rounded) > fabs(value)) ? FPSCR_FR_BIT : 0u);
        set_fprf(cpu, classify_f32(rounded));
        cpu->fpr[d] = (f64)rounded;
        cpu->ps1[d] = (f64)rounded;
    }
}

void ppc_fres_op(CPUState* cpu, u8 d, u8 b) {
    f64 value = cpu->fpr[b];

    if (value == 0.0) {
        set_fp_exception(cpu, FPSCR_ZX_BIT);
        clear_fifr(cpu);
        if (cpu->fpscr & FPSCR_ZE_BIT)
            return;
    } else if (is_snan(value)) {
        set_fp_exception(cpu, FPSCR_VXSNAN_BIT);
        clear_fifr(cpu);
        if (cpu->fpscr & FPSCR_VE_BIT)
            return;
    } else if (isnan(value) || isinf(value)) {
        clear_fifr(cpu);
    }

    f64 result = ppc_approx_reciprocal(value);
    cpu->fpr[d] = result;
    cpu->ps1[d] = result;
    set_fprf(cpu, classify_f32((f32)result));
}

void ppc_frsqrte_op(CPUState* cpu, u8 d, u8 b) {
    f64 value = cpu->fpr[b];

    if (value < 0.0) {
        set_fp_exception(cpu, FPSCR_VXSQRT_BIT);
        clear_fifr(cpu);
        if (cpu->fpscr & FPSCR_VE_BIT)
            return;
    } else if (value == 0.0) {
        set_fp_exception(cpu, FPSCR_ZX_BIT);
        clear_fifr(cpu);
        if (cpu->fpscr & FPSCR_ZE_BIT)
            return;
    } else if (is_snan(value)) {
        set_fp_exception(cpu, FPSCR_VXSNAN_BIT);
        clear_fifr(cpu);
        if (cpu->fpscr & FPSCR_VE_BIT)
            return;
    } else if (isnan(value) || isinf(value)) {
        clear_fifr(cpu);
    }

    f64 result = ppc_approx_rsqrt(value);
    cpu->fpr[d] = result;
    set_fprf(cpu, classify_f64(result));
}

void ppc_fctiw_op(CPUState* cpu, u8 d, u8 b, bool toward_zero) {
    u64 result;
    if (ppc_fctiw(cpu, cpu->fpr[b], toward_zero, &result))
        cpu->fpr[d] = f64_value(result);
}

void ppc_fcmp(CPUState* cpu, u8 crfd, f64 a, f64 b, bool ordered) {
    u32 compare;

    if (isnan(a) || isnan(b)) {
        compare = 0x1u; /* FU */
        if (is_snan(a) || is_snan(b)) {
            set_fp_exception(cpu, FPSCR_VXSNAN_BIT);
            if (ordered && (cpu->fpscr & FPSCR_VE_BIT) == 0)
                set_fp_exception(cpu, FPSCR_VXVC_BIT);
        } else if (ordered) {
            set_fp_exception(cpu, FPSCR_VXVC_BIT);
        }
    } else if (a < b) {
        compare = 0x8u; /* FL */
    } else if (a > b) {
        compare = 0x4u; /* FG */
    } else {
        compare = 0x2u; /* FE */
    }

    /* Mirror Dolphin's FPCC quirk (Interpreter_FloatingPoint Helper_FloatCompare):
     *   fpscr.FPRF = (fpscr.FPRF & ~FPCC_MASK) | compare_value;
     * FPRF reads back as the 5-bit FIELD value (0..31), but FPCC_MASK is the
     * Hex-space constant (0xF << 12); (field_value & ~0xF000) is a no-op, so the
     * intended "clear FPCC" never happens and Dolphin OR-accumulates the compare
     * result into the existing FPCC. Arithmetic ops assign fpscr.FPRF directly
     * (ClassifyFloat/Double) and DO replace it, so only fcmp accumulates. We are
     * differential-exact against Dolphin's interpreter, so replicate the quirk. */
    cpu->fpscr |= (compare << 12);
    u32 shift = 4u * (7u - crfd);
    cpu->cr = (cpu->cr & ~(0xFu << shift)) | (compare << shift);
}

/* ---- paired singles ---- */

static void ps_write_both(CPUState* cpu, u8 d, f32 ps0, f32 ps1) {
    cpu->fpr[d] = (f64)ps0;
    cpu->ps1[d] = (f64)ps1;
}

void ppc_ps_add_op(CPUState* cpu, u8 d, u8 a, u8 b) {
    f32 ps0 = force_single(cpu, ni_add(cpu, cpu->fpr[a], cpu->fpr[b]).value);
    f32 ps1 = force_single(cpu, ni_add(cpu, cpu->ps1[a], cpu->ps1[b]).value);
    ps_write_both(cpu, d, ps0, ps1);
    set_fprf(cpu, classify_f32(ps0));
}

void ppc_ps_sub_op(CPUState* cpu, u8 d, u8 a, u8 b) {
    f32 ps0 = force_single(cpu, ni_sub(cpu, cpu->fpr[a], cpu->fpr[b]).value);
    f32 ps1 = force_single(cpu, ni_sub(cpu, cpu->ps1[a], cpu->ps1[b]).value);
    ps_write_both(cpu, d, ps0, ps1);
    set_fprf(cpu, classify_f32(ps0));
}

void ppc_ps_mul_op(CPUState* cpu, u8 d, u8 a, u8 c) {
    f64 c0 = force_25bit_c(cpu->fpr[c]);
    f64 c1 = force_25bit_c(cpu->ps1[c]);
    f32 ps0 = force_single(cpu, ni_mul(cpu, cpu->fpr[a], c0).value);
    f32 ps1 = force_single(cpu, ni_mul(cpu, cpu->ps1[a], c1).value);
    ps_write_both(cpu, d, ps0, ps1);
    set_fprf(cpu, classify_f32(ps0));
}

void ppc_ps_div_op(CPUState* cpu, u8 d, u8 a, u8 b) {
    f32 ps0 = force_single(cpu, ni_div(cpu, cpu->fpr[a], cpu->fpr[b]).value);
    f32 ps1 = force_single(cpu, ni_div(cpu, cpu->ps1[a], cpu->ps1[b]).value);
    ps_write_both(cpu, d, ps0, ps1);
    set_fprf(cpu, classify_f32(ps0));
}

void ppc_ps_madd_op(CPUState* cpu, u8 d, u8 a, u8 c, u8 b,
                    bool subtract, bool negative) {
    f32 tmp0 = force_single(
        cpu, ni_madd_msub(cpu, cpu->fpr[a], cpu->fpr[c], cpu->fpr[b], subtract, true).value);
    f32 tmp1 = force_single(
        cpu, ni_madd_msub(cpu, cpu->ps1[a], cpu->ps1[c], cpu->ps1[b], subtract, true).value);
    f32 ps0 = (negative && !isnan(tmp0)) ? -tmp0 : tmp0;
    f32 ps1 = (negative && !isnan(tmp1)) ? -tmp1 : tmp1;
    ps_write_both(cpu, d, ps0, ps1);
    set_fprf(cpu, classify_f32(ps0));
}

void ppc_ps_madds0(CPUState* cpu, u8 d, u8 a, u8 c, u8 b) {
    f32 ps0 = force_single(
        cpu, ni_madd_msub(cpu, cpu->fpr[a], cpu->fpr[c], cpu->fpr[b], false, true).value);
    f32 ps1 = force_single(
        cpu, ni_madd_msub(cpu, cpu->ps1[a], cpu->fpr[c], cpu->ps1[b], false, true).value);
    ps_write_both(cpu, d, ps0, ps1);
    set_fprf(cpu, classify_f32(ps0));
}

void ppc_ps_madds1(CPUState* cpu, u8 d, u8 a, u8 c, u8 b) {
    f32 ps0 = force_single(
        cpu, ni_madd_msub(cpu, cpu->fpr[a], cpu->ps1[c], cpu->fpr[b], false, true).value);
    f32 ps1 = force_single(
        cpu, ni_madd_msub(cpu, cpu->ps1[a], cpu->ps1[c], cpu->ps1[b], false, true).value);
    ps_write_both(cpu, d, ps0, ps1);
    set_fprf(cpu, classify_f32(ps0));
}

void ppc_ps_sum0(CPUState* cpu, u8 d, u8 a, u8 c, u8 b) {
    f32 ps0 = force_single(cpu, ni_add(cpu, cpu->fpr[a], cpu->ps1[b]).value);
    f32 ps1 = force_single(cpu, cpu->ps1[c]);
    ps_write_both(cpu, d, ps0, ps1);
    set_fprf(cpu, classify_f32(ps0));
}

void ppc_ps_sum1(CPUState* cpu, u8 d, u8 a, u8 c, u8 b) {
    f32 ps0 = force_single(cpu, cpu->fpr[c]);
    f32 ps1 = force_single(cpu, ni_add(cpu, cpu->fpr[a], cpu->ps1[b]).value);
    ps_write_both(cpu, d, ps0, ps1);
    set_fprf(cpu, classify_f32(ps1));
}

void ppc_ps_muls0(CPUState* cpu, u8 d, u8 a, u8 c) {
    f64 c0 = force_25bit_c(cpu->fpr[c]);
    f32 ps0 = force_single(cpu, ni_mul(cpu, cpu->fpr[a], c0).value);
    f32 ps1 = force_single(cpu, ni_mul(cpu, cpu->ps1[a], c0).value);
    ps_write_both(cpu, d, ps0, ps1);
    set_fprf(cpu, classify_f32(ps0));
}

void ppc_ps_muls1(CPUState* cpu, u8 d, u8 a, u8 c) {
    f64 c1 = force_25bit_c(cpu->ps1[c]);
    f32 ps0 = force_single(cpu, ni_mul(cpu, cpu->fpr[a], c1).value);
    f32 ps1 = force_single(cpu, ni_mul(cpu, cpu->ps1[a], c1).value);
    ps_write_both(cpu, d, ps0, ps1);
    set_fprf(cpu, classify_f32(ps0));
}

void ppc_ps_res_op(CPUState* cpu, u8 d, u8 b) {
    f64 a = cpu->fpr[b];
    f64 b1 = cpu->ps1[b];

    if (a == 0.0 || b1 == 0.0) {
        set_fp_exception(cpu, FPSCR_ZX_BIT);
        clear_fifr(cpu);
    }
    if (isnan(a) || isinf(a) || isnan(b1) || isinf(b1))
        clear_fifr(cpu);
    if (is_snan(a) || is_snan(b1))
        set_fp_exception(cpu, FPSCR_VXSNAN_BIT);

    f64 ps0 = ppc_approx_reciprocal(a);
    f64 ps1 = ppc_approx_reciprocal(b1);
    cpu->fpr[d] = ps0;
    cpu->ps1[d] = ps1;
    set_fprf(cpu, classify_f32((f32)ps0));
}

void ppc_ps_rsqrte_op(CPUState* cpu, u8 d, u8 b) {
    f64 ps0_in = cpu->fpr[b];
    f64 ps1_in = cpu->ps1[b];

    if (ps0_in == 0.0 || ps1_in == 0.0) {
        set_fp_exception(cpu, FPSCR_ZX_BIT);
        clear_fifr(cpu);
    }
    if (ps0_in < 0.0 || ps1_in < 0.0) {
        set_fp_exception(cpu, FPSCR_VXSQRT_BIT);
        clear_fifr(cpu);
    }
    if (isnan(ps0_in) || isinf(ps0_in) || isnan(ps1_in) || isinf(ps1_in))
        clear_fifr(cpu);
    if (is_snan(ps0_in) || is_snan(ps1_in))
        set_fp_exception(cpu, FPSCR_VXSNAN_BIT);

    f32 dst_ps0 = force_single(cpu, ppc_approx_rsqrt(ps0_in));
    f32 dst_ps1 = force_single(cpu, ppc_approx_rsqrt(ps1_in));
    ps_write_both(cpu, d, dst_ps0, dst_ps1);
    set_fprf(cpu, classify_f32(dst_ps0));
}

/* ---- FP loads/stores (Dolphin alignment + conversion semantics) ---- */

bool ppc_lfs_op(CPUState* cpu, u8 d, u32 ea, u32 cia) {
    if ((ea & 3u) != 0) {
        ppc_alignment_exception(cpu, ea, cia);
        return false;
    }
    u64 value = convert_to_double(mem_read32(cpu, ea));
    cpu->fpr[d] = f64_value(value);
    cpu->ps1[d] = f64_value(value);
    return true;
}

bool ppc_lfd_op(CPUState* cpu, u8 d, u32 ea, u32 cia) {
    if ((ea & 3u) != 0) {
        ppc_alignment_exception(cpu, ea, cia);
        return false;
    }
    cpu->fpr[d] = f64_value(mem_read64(cpu, ea));
    return true;
}

bool ppc_stfs_op(CPUState* cpu, u8 s, u32 ea, u32 cia) {
    if ((ea & 3u) != 0) {
        ppc_alignment_exception(cpu, ea, cia);
        return false;
    }
    mem_write32(cpu, ea, convert_to_single(f64_bits(cpu->fpr[s])));
    return true;
}

bool ppc_stfd_op(CPUState* cpu, u8 s, u32 ea, u32 cia) {
    if ((ea & 3u) != 0) {
        ppc_alignment_exception(cpu, ea, cia);
        return false;
    }
    mem_write64(cpu, ea, f64_bits(cpu->fpr[s]));
    return true;
}

bool ppc_lwarx_op(CPUState* cpu, u8 d, u32 ea, u32 cia) {
    if ((ea & 3u) != 0) {
        ppc_alignment_exception(cpu, ea, cia);
        return false;
    }
    cpu->gpr[d] = mem_read32(cpu, ea);
    cpu->reserve_valid = true;
    cpu->reserve_addr = ea;
    return true;
}

void ppc_stwcx_op(CPUState* cpu, u8 s, u32 ea, u32 cia) {
    if ((ea & 3u) != 0) {
        ppc_alignment_exception(cpu, ea, cia);
        return;
    }
    u32 so_bit = (cpu->xer >> 31) & 1u; /* XER.SO -> CR0[SO] */
    if (cpu->reserve_valid && ea == cpu->reserve_addr) {
        mem_write32(cpu, ea, cpu->gpr[s]);
        cpu->reserve_valid = false;
        cpu->cr = (cpu->cr & 0x0FFFFFFFu) | ((2u | so_bit) << 28);
        return;
    }
    cpu->cr = (cpu->cr & 0x0FFFFFFFu) | (so_bit << 28);
}

void ppc_stsw(CPUState* cpu, u32 ea, u32 n, u8 r, u32 cia) {
    /* Dolphin Helper_StoreString: only aligned 32-bit writes, reading and
     * merging the partial head/tail words so surrounding bytes are
     * preserved exactly. LE mode raises an alignment exception (Gekko is BE
     * in practice, so this never fires for Strikers). */
    if (cpu->msr & PPC_MSR_LE) {
        ppc_alignment_exception(cpu, ea, cia);
        return;
    }
    if (n == 0)
        return;

    u32 misalignment_bytes = ea & 3u;
    u32 misalignment_bits = misalignment_bytes * 8u;
    u32 current_address = ea & ~3u;
    u64 current_value = 0;

    if (misalignment_bytes != 0) {
        current_value = mem_read32(cpu, current_address);
        current_value <<= misalignment_bits;
        current_value &= 0xFFFFFFFF00000000ull;
        n += misalignment_bytes;
    }

    while (n >= 4) {
        current_value |= cpu->gpr[r];
        mem_write32(cpu, current_address, (u32)(current_value >> misalignment_bits));
        current_value <<= 32;
        current_address += 4;
        n -= 4;
        r = (u8)((r + 1) & 0x1Fu);
    }

    if (n != 0) {
        if (n > misalignment_bytes) {
            current_value |= cpu->gpr[r];
            current_value <<= (n - misalignment_bytes) * 8u;
        } else {
            current_value >>= (misalignment_bytes - n) * 8u;
        }
        current_value &= 0xFFFFFFFF00000000ull;
        current_value |= ((u64)mem_read32(cpu, current_address) << (n * 8u)) & 0xFFFFFFFFull;
        mem_write32(cpu, current_address, (u32)(current_value >> (n * 8u)));
    }
}

void ppc_memory_fence(void) {
#if defined(_MSC_VER)
    _ReadWriteBarrier();
#if defined(_M_IX86) || defined(_M_X64)
    _mm_mfence();
#endif
    _ReadWriteBarrier();
#else
    atomic_thread_fence(memory_order_seq_cst);
#endif
}

static f64 round_nearest_even(f64 value) {
    f64 lo = floor(value);
    f64 fraction = value - lo;
    if (fraction < 0.5)
        return lo;
    if (fraction > 0.5)
        return lo + 1.0;
    return fmod(lo, 2.0) == 0.0 ? lo : lo + 1.0;
}

bool ppc_fctiw(CPUState* cpu, f64 value, bool toward_zero, u64* output) {
    f64 rounded;
    switch (toward_zero ? 1u : (cpu->fpscr & 3u)) {
    case 1: rounded = trunc(value); break;
    case 2: rounded = ceil(value); break;
    case 3: rounded = floor(value); break;
    default: rounded = round_nearest_even(value); break;
    }

    u32 result;
    bool invalid = false;
    if (isnan(value)) {
        if (is_snan(value))
            set_fp_exception(cpu, 0x01000000u);
        result = 0x80000000u;
        invalid = true;
    } else if (rounded >= 2147483648.0) {
        result = 0x7FFFFFFFu;
        invalid = true;
    } else if (rounded < -2147483648.0) {
        result = 0x80000000u;
        invalid = true;
    } else {
        result = (u32)(s32)rounded;
    }

    clear_fifr(cpu);
    if (invalid) {
        set_fp_exception(cpu, FPSCR_VXCVI_BIT);
    } else if (rounded != value) {
        set_fp_exception(cpu, FPSCR_XX_BIT);
        cpu->fpscr |= FPSCR_FI_BIT;
        if (fabs(rounded) > fabs(value))
            cpu->fpscr |= FPSCR_FR_BIT;
    }

    if (invalid && (cpu->fpscr & 0x80u))
        return false;

    *output = 0xFFF8000000000000ull | result |
              ((result == 0 && signbit(value)) ? 0x100000000ull : 0ull);
    return true;
}
