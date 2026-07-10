// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef DOLRECOMP_TYPES_H
#define DOLRECOMP_TYPES_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>

typedef uint8_t  u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
typedef int8_t   s8;
typedef int16_t  s16;
typedef int32_t  s32;
typedef int64_t  s64;
typedef float    f32;
typedef double   f64;

// sign-extend from N bits to s32
static inline s32 sign_extend(u32 value, int bits) {
    u32 mask = 1u << (bits - 1);
    return (s32)((value ^ mask) - mask);
}

// byte swap
static inline u16 bswap16(u16 v) {
    return __builtin_bswap16(v);
}

static inline u32 bswap32(u32 v) {
    return __builtin_bswap32(v);
}

// big-endian read/write
static inline u16 read_be16(const u8* p) {
    u16 v;
    memcpy(&v, p, 2);
    return __builtin_bswap16(v);
}

static inline u32 read_be32(const u8* p) {
    u32 v;
    memcpy(&v, p, 4);
    return __builtin_bswap32(v);
}

static inline u64 read_be64(const u8* p) {
    u64 v;
    memcpy(&v, p, 8);
    return __builtin_bswap64(v);
}

static inline void write_be16(u8* p, u16 v) {
    u16 swapped = __builtin_bswap16(v);
    memcpy(p, &swapped, 2);
}

static inline void write_be32(u8* p, u32 v) {
    u32 swapped = __builtin_bswap32(v);
    memcpy(p, &swapped, 4);
}

static inline void write_be64(u8* p, u64 v) {
    u64 swapped = __builtin_bswap64(v);
    memcpy(p, &swapped, 8);
}

#endif /* DOLRECOMP_TYPES_H */
