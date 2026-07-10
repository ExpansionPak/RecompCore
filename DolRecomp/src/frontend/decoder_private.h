#ifndef DOLRECOMP_DECODER_PRIVATE_H
#define DOLRECOMP_DECODER_PRIVATE_H

#include "decoder.h"
#include "core/types.h"
#include <string.h>

// Field extraction from a host-endian 32-bit instruction word.
#define PPC_PRIMARY(raw)   ((raw) >> 26)
#define PPC_RD(raw)        (((raw) >> 21) & 0x1F)
#define PPC_RS(raw)        (((raw) >> 21) & 0x1F)
#define PPC_RA(raw)        (((raw) >> 16) & 0x1F)
#define PPC_RB(raw)        (((raw) >> 11) & 0x1F)
#define PPC_RC_REG(raw)    (((raw) >> 6) & 0x1F)
#define PPC_BO(raw)        (((raw) >> 21) & 0x1F)
#define PPC_BI(raw)        (((raw) >> 16) & 0x1F)
#define PPC_XO(raw)        (((raw) >> 1) & 0x3FF)
#define PPC_A_XO(raw)      (((raw) >> 1) & 0x1F)
#define PPC_SPR(raw)       ((((raw) >> 16) & 0x1F) | (((raw) >> 6) & 0x3E0))
#define PPC_CRM(raw)       (((raw) >> 12) & 0xFF)
#define PPC_MB(raw)        (((raw) >> 6) & 0x1F)
#define PPC_ME(raw)        (((raw) >> 1) & 0x1F)
#define PPC_SH(raw)        (((raw) >> 11) & 0x1F)
#define PPC_RC(raw)        (((raw) & 1) != 0)
#define PPC_SIMM(raw)      ((s16)((raw) & 0xFFFF))
#define PPC_UIMM(raw)      ((u16)((raw) & 0xFFFF))

static inline void decode_d_rt_ra(PPCInst* inst, PPCOpcode op, u32 raw) {
    inst->op = op;
    inst->rD = PPC_RD(raw);
    inst->rA = PPC_RA(raw);
    inst->simm = PPC_SIMM(raw);
}

static inline void decode_d_rt_ra_update(PPCInst* inst, PPCOpcode op, u32 raw) {
    if (PPC_RA(raw) == 0 || PPC_RA(raw) == PPC_RD(raw)) {
        inst->op = PPC_OP_UNKNOWN;
        return;
    }
    decode_d_rt_ra(inst, op, raw);
}

static inline void decode_d_frt_ra_update(PPCInst* inst, PPCOpcode op, u32 raw) {
    if (PPC_RA(raw) == 0) {
        inst->op = PPC_OP_UNKNOWN;
        return;
    }
    decode_d_rt_ra(inst, op, raw);
}

static inline void decode_d_rs_ra(PPCInst* inst, PPCOpcode op, u32 raw) {
    inst->op = op;
    inst->rS = PPC_RS(raw);
    inst->rA = PPC_RA(raw);
    inst->simm = PPC_SIMM(raw);
}

static inline void decode_d_rs_ra_update(PPCInst* inst, PPCOpcode op, u32 raw) {
    if (PPC_RA(raw) == 0) {
        inst->op = PPC_OP_UNKNOWN;
        return;
    }
    decode_d_rs_ra(inst, op, raw);
}

static inline void decode_x_rt_ra_rb(PPCInst* inst, PPCOpcode op, u32 raw) {
    inst->op = op;
    inst->rD = PPC_RD(raw);
    inst->rA = PPC_RA(raw);
    inst->rB = PPC_RB(raw);
    inst->rc = PPC_RC(raw);
}

static inline void decode_x_rt_ra_rb_norc(PPCInst* inst, PPCOpcode op, u32 raw) {
    if (PPC_RC(raw)) {
        inst->op = PPC_OP_UNKNOWN;
        return;
    }
    decode_x_rt_ra_rb(inst, op, raw);
}

static inline void decode_x_rt_ra_rb_update(PPCInst* inst, PPCOpcode op, u32 raw) {
    if (PPC_RC(raw) || PPC_RA(raw) == 0 || PPC_RA(raw) == PPC_RD(raw)) {
        inst->op = PPC_OP_UNKNOWN;
        return;
    }
    decode_x_rt_ra_rb(inst, op, raw);
}

static inline void decode_x_frt_ra_rb_update(PPCInst* inst, PPCOpcode op, u32 raw) {
    if (PPC_RC(raw) || PPC_RA(raw) == 0) {
        inst->op = PPC_OP_UNKNOWN;
        return;
    }
    decode_x_rt_ra_rb(inst, op, raw);
}

static inline void decode_xo_rt_ra_rb(PPCInst* inst, PPCOpcode op, u32 raw) {
    decode_x_rt_ra_rb(inst, op, raw);
    inst->oe = ((raw >> 10) & 1u) != 0;
}

static inline void decode_x_rs_ra_rb(PPCInst* inst, PPCOpcode op, u32 raw) {
    inst->op = op;
    inst->rS = PPC_RS(raw);
    inst->rA = PPC_RA(raw);
    inst->rB = PPC_RB(raw);
    inst->rc = PPC_RC(raw);
}

static inline void decode_x_rs_ra_rb_norc(PPCInst* inst, PPCOpcode op, u32 raw) {
    if (PPC_RC(raw)) {
        inst->op = PPC_OP_UNKNOWN;
        return;
    }
    decode_x_rs_ra_rb(inst, op, raw);
}

static inline void decode_x_rs_ra_rb_update(PPCInst* inst, PPCOpcode op, u32 raw) {
    if (PPC_RC(raw) || PPC_RA(raw) == 0) {
        inst->op = PPC_OP_UNKNOWN;
        return;
    }
    decode_x_rs_ra_rb(inst, op, raw);
}

static inline void decode_a_frt_fra_frb_frc(PPCInst* inst, PPCOpcode op, u32 raw) {
    inst->op = op;
    inst->rD = PPC_RD(raw);
    inst->rA = PPC_RA(raw);
    inst->rB = PPC_RB(raw);
    inst->rC = PPC_RC_REG(raw);
    inst->rc = PPC_RC(raw);
}

static inline void decode_x_frt_frb(PPCInst* inst, PPCOpcode op, u32 raw) {
    if (PPC_RA(raw) != 0) {
        inst->op = PPC_OP_UNKNOWN;
        return;
    }
    inst->op = op;
    inst->rD = PPC_RD(raw);
    inst->rB = PPC_RB(raw);
    inst->rc = PPC_RC(raw);
}

static inline void decode_fcmp(PPCInst* inst, PPCOpcode op, u32 raw) {
    if (raw & ((3u << 21) | 1u)) {
        inst->op = PPC_OP_UNKNOWN;
        return;
    }
    inst->op = op;
    inst->crfD = (raw >> 23) & 0x7;
    inst->rA = PPC_RA(raw);
    inst->rB = PPC_RB(raw);
}

static inline void decode_cr_logical(PPCInst* inst, PPCOpcode op, u32 raw) {
    if (PPC_RC(raw)) {
        inst->op = PPC_OP_UNKNOWN;
        return;
    }
    inst->op = op;
    inst->rD = PPC_RD(raw);
    inst->rA = PPC_RA(raw);
    inst->rB = PPC_RB(raw);
}

static inline void decode_mtfsb(PPCInst* inst, PPCOpcode op, u32 raw) {
    if (PPC_RA(raw) != 0 || PPC_RB(raw) != 0) {
        inst->op = PPC_OP_UNKNOWN;
        return;
    }
    inst->op = op;
    inst->rD = PPC_RD(raw);
    inst->rc = PPC_RC(raw);
}

static inline void decode_xo_rt_ra(PPCInst* inst, PPCOpcode op, u32 raw) {
    inst->op = op;
    inst->rD = PPC_RD(raw);
    inst->rA = PPC_RA(raw);
    inst->rc = PPC_RC(raw);
    inst->oe = ((raw >> 10) & 1u) != 0;
}

static inline void decode_psq_d_rt_ra(PPCInst* inst, PPCOpcode op, u32 raw) {
    inst->op = op;
    inst->rD = PPC_RD(raw);
    inst->rA = PPC_RA(raw);
    inst->w = (raw >> 15) & 1u;
    inst->i = (raw >> 12) & 7u;
    inst->simm = (s16)sign_extend(raw & 0x0FFFu, 12);
}

static inline void decode_psq_d_rt_ra_update(PPCInst* inst, PPCOpcode op, u32 raw) {
    if (PPC_RA(raw) == 0) {
        inst->op = PPC_OP_UNKNOWN;
        return;
    }
    decode_psq_d_rt_ra(inst, op, raw);
}

static inline void decode_psq_d_rs_ra(PPCInst* inst, PPCOpcode op, u32 raw) {
    inst->op = op;
    inst->rS = PPC_RS(raw);
    inst->rA = PPC_RA(raw);
    inst->w = (raw >> 15) & 1u;
    inst->i = (raw >> 12) & 7u;
    inst->simm = (s16)sign_extend(raw & 0x0FFFu, 12);
}

static inline void decode_psq_d_rs_ra_update(PPCInst* inst, PPCOpcode op, u32 raw) {
    if (PPC_RA(raw) == 0) {
        inst->op = PPC_OP_UNKNOWN;
        return;
    }
    decode_psq_d_rs_ra(inst, op, raw);
}

static inline void decode_psq_x_rt_ra_rb(PPCInst* inst, PPCOpcode op, u32 raw) {
    inst->op = op;
    inst->rD = PPC_RD(raw);
    inst->rA = PPC_RA(raw);
    inst->rB = PPC_RB(raw);
    inst->w = (raw >> 10) & 1u;
    inst->i = (raw >> 7) & 7u;
}

static inline void decode_psq_x_rt_ra_rb_norc(PPCInst* inst, PPCOpcode op, u32 raw) {
    if (PPC_RC(raw)) {
        inst->op = PPC_OP_UNKNOWN;
        return;
    }
    decode_psq_x_rt_ra_rb(inst, op, raw);
}

static inline void decode_psq_x_rt_ra_rb_update(PPCInst* inst, PPCOpcode op, u32 raw) {
    if (PPC_RC(raw) || PPC_RA(raw) == 0) {
        inst->op = PPC_OP_UNKNOWN;
        return;
    }
    decode_psq_x_rt_ra_rb(inst, op, raw);
}

static inline void decode_psq_x_rs_ra_rb(PPCInst* inst, PPCOpcode op, u32 raw) {
    inst->op = op;
    inst->rS = PPC_RS(raw);
    inst->rA = PPC_RA(raw);
    inst->rB = PPC_RB(raw);
    inst->w = (raw >> 10) & 1u;
    inst->i = (raw >> 7) & 7u;
}

static inline void decode_psq_x_rs_ra_rb_norc(PPCInst* inst, PPCOpcode op, u32 raw) {
    if (PPC_RC(raw)) {
        inst->op = PPC_OP_UNKNOWN;
        return;
    }
    decode_psq_x_rs_ra_rb(inst, op, raw);
}

static inline void decode_psq_x_rs_ra_rb_update(PPCInst* inst, PPCOpcode op, u32 raw) {
    if (PPC_RC(raw) || PPC_RA(raw) == 0) {
        inst->op = PPC_OP_UNKNOWN;
        return;
    }
    decode_psq_x_rs_ra_rb(inst, op, raw);
}

static inline bool reg_in_wrapped_range(u8 start, u32 count, u8 reg) {
    for (u32 i = 0; i < count; i++) {
        if (((u32)start + i) % 32u == reg)
            return true;
    }
    return false;
}

static inline u32 string_register_count(u8 byte_count) {
    u32 count = byte_count ? byte_count : 32u;
    return (count + 3u) / 4u;
}

// Split decoder functions
bool decode_primary_4(PPCInst* inst, u32 raw, u32 address);
bool decode_primary_19(PPCInst* inst, u32 raw, u32 address);
bool decode_primary_31(PPCInst* inst, u32 raw, u32 address);
bool decode_primary_31_system(PPCInst* inst, u32 raw, u32 address);
bool decode_primary_31_integer(PPCInst* inst, u32 raw, u32 address);
bool decode_primary_59(PPCInst* inst, u32 raw, u32 address);
bool decode_primary_63(PPCInst* inst, u32 raw, u32 address);

#endif /* DOLRECOMP_DECODER_PRIVATE_H */
