#include "decoder_private.h"

bool decode_primary_4(PPCInst* inst, u32 raw, u32 address) {
    (void)address;
    u32 xo = PPC_XO(raw);
    switch (xo) {
    case 0:   decode_fcmp(inst, PPC_OP_PS_CMPU0, raw); return true;
    case 18:  decode_a_frt_fra_frb_frc(inst, PPC_OP_PS_DIV, raw); return true;
    case 24:
        if (PPC_RA(raw) == 0) { decode_x_frt_frb(inst, PPC_OP_PS_RES, raw); return true; }
        break;
    case 26:
        if (PPC_RA(raw) == 0) { decode_x_frt_frb(inst, PPC_OP_PS_RSQRTE, raw); return true; }
        break;
    case 20:  decode_a_frt_fra_frb_frc(inst, PPC_OP_PS_SUB, raw); return true;
    case 21:  decode_a_frt_fra_frb_frc(inst, PPC_OP_PS_ADD, raw); return true;
    case 32:  decode_fcmp(inst, PPC_OP_PS_CMPO0, raw); return true;
    case 40:  decode_x_frt_frb(inst, PPC_OP_PS_NEG, raw); return true;
    case 64:  decode_fcmp(inst, PPC_OP_PS_CMPU1, raw); return true;
    case 72:  decode_x_frt_frb(inst, PPC_OP_PS_MR, raw); return true;
    case 96:  decode_fcmp(inst, PPC_OP_PS_CMPO1, raw); return true;
    case 136: decode_x_frt_frb(inst, PPC_OP_PS_NABS, raw); return true;
    case 264: decode_x_frt_frb(inst, PPC_OP_PS_ABS, raw); return true;
    case 528: decode_a_frt_fra_frb_frc(inst, PPC_OP_PS_MERGE00, raw); return true;
    case 560: decode_a_frt_fra_frb_frc(inst, PPC_OP_PS_MERGE01, raw); return true;
    case 592: decode_a_frt_fra_frb_frc(inst, PPC_OP_PS_MERGE10, raw); return true;
    case 624: decode_a_frt_fra_frb_frc(inst, PPC_OP_PS_MERGE11, raw); return true;
    case 1014:
        if (PPC_RD(raw) == 0 && !PPC_RC(raw)) {
            inst->op = PPC_OP_DCBZ_L;
            inst->rA = PPC_RA(raw);
            inst->rB = PPC_RB(raw);
            return true;
        }
        break;
    default: {
        u32 psxo = xo & 0x3Fu;
        switch (psxo) {
        case 6:  decode_psq_x_rt_ra_rb_norc(inst, PPC_OP_PSQ_LX, raw); return true;
        case 7:  decode_psq_x_rs_ra_rb_norc(inst, PPC_OP_PSQ_STX, raw); return true;
        case 38: decode_psq_x_rt_ra_rb_update(inst, PPC_OP_PSQ_LUX, raw); return true;
        case 39: decode_psq_x_rs_ra_rb_update(inst, PPC_OP_PSQ_STUX, raw); return true;
        default: {
            switch (PPC_A_XO(raw)) {
            case 10: decode_a_frt_fra_frb_frc(inst, PPC_OP_PS_SUM0, raw); return true;
            case 11: decode_a_frt_fra_frb_frc(inst, PPC_OP_PS_SUM1, raw); return true;
            case 12: decode_a_frt_fra_frb_frc(inst, PPC_OP_PS_MULS0, raw); return true;
            case 13: decode_a_frt_fra_frb_frc(inst, PPC_OP_PS_MULS1, raw); return true;
            case 14: decode_a_frt_fra_frb_frc(inst, PPC_OP_PS_MADDS0, raw); return true;
            case 15: decode_a_frt_fra_frb_frc(inst, PPC_OP_PS_MADDS1, raw); return true;
            case 23: decode_a_frt_fra_frb_frc(inst, PPC_OP_PS_SEL, raw); return true;
            case 25: decode_a_frt_fra_frb_frc(inst, PPC_OP_PS_MUL, raw); return true;
            case 28: decode_a_frt_fra_frb_frc(inst, PPC_OP_PS_MSUB, raw); return true;
            case 29: decode_a_frt_fra_frb_frc(inst, PPC_OP_PS_MADD, raw); return true;
            case 30: decode_a_frt_fra_frb_frc(inst, PPC_OP_PS_NMSUB, raw); return true;
            case 31: decode_a_frt_fra_frb_frc(inst, PPC_OP_PS_NMADD, raw); return true;
            default: break;
            }
            break;
        }
        }
        break;
    }
    }
    return false;
}

bool decode_primary_59(PPCInst* inst, u32 raw, u32 address) {
    (void)address;
    switch (PPC_A_XO(raw)) {
    case 18: decode_a_frt_fra_frb_frc(inst, PPC_OP_FDIVS, raw); return true;
    case 20: decode_a_frt_fra_frb_frc(inst, PPC_OP_FSUBS, raw); return true;
    case 21: decode_a_frt_fra_frb_frc(inst, PPC_OP_FADDS, raw); return true;
    case 24:
        if (PPC_RA(raw) == 0 && PPC_RC_REG(raw) == 0) {
            decode_x_frt_frb(inst, PPC_OP_FRES, raw);
            return true;
        }
        break;
    case 25: decode_a_frt_fra_frb_frc(inst, PPC_OP_FMULS, raw); return true;
    case 28: decode_a_frt_fra_frb_frc(inst, PPC_OP_FMSUBS, raw); return true;
    case 29: decode_a_frt_fra_frb_frc(inst, PPC_OP_FMADDS, raw); return true;
    case 30: decode_a_frt_fra_frb_frc(inst, PPC_OP_FNMSUBS, raw); return true;
    case 31: decode_a_frt_fra_frb_frc(inst, PPC_OP_FNMADDS, raw); return true;
    default: break;
    }
    return false;
}

bool decode_primary_63(PPCInst* inst, u32 raw, u32 address) {
    (void)address;
    switch (PPC_XO(raw)) {
    case 0:   decode_fcmp(inst, PPC_OP_FCMPU, raw); return true;
    case 12:  decode_x_frt_frb(inst, PPC_OP_FRSP, raw); return true;
    case 14:
        if (PPC_RA(raw) == 0) { decode_x_frt_frb(inst, PPC_OP_FCTIW, raw); return true; }
        break;
    case 15:
        if (PPC_RA(raw) == 0) { decode_x_frt_frb(inst, PPC_OP_FCTIWZ, raw); return true; }
        break;
    case 32:  decode_fcmp(inst, PPC_OP_FCMPO, raw); return true;
    case 38:  decode_mtfsb(inst, PPC_OP_MTFSB1, raw); return true;
    case 40:  decode_x_frt_frb(inst, PPC_OP_FNEG, raw); return true;
    case 70:  decode_mtfsb(inst, PPC_OP_MTFSB0, raw); return true;
    case 72:  decode_x_frt_frb(inst, PPC_OP_FMR, raw); return true;
    case 136: decode_x_frt_frb(inst, PPC_OP_FNABS, raw); return true;
    case 264: decode_x_frt_frb(inst, PPC_OP_FABS, raw); return true;
    case 64:
        if (raw & ((3u << 21) | (0x7Fu << 11) | 1u)) break;
        inst->op = PPC_OP_MCRFS;
        inst->crfD = (raw >> 23) & 7u;
        inst->crfS = (raw >> 18) & 7u;
        return true;
    case 134:
        if (raw & ((0x7Fu << 16) | (1u << 11))) break;
        inst->op = PPC_OP_MTFSFI;
        inst->crfD = (raw >> 23) & 7u;
        inst->imm = (raw >> 12) & 0xFu;
        inst->rc = PPC_RC(raw);
        return true;
    case 583:
        if (PPC_RA(raw) != 0 || PPC_RB(raw) != 0) break;
        inst->op = PPC_OP_MFFS;
        inst->rD = PPC_RD(raw);
        inst->rc = PPC_RC(raw);
        return true;
    case 711:
        if (raw & ((1u << 25) | (1u << 16))) break;
        inst->op = PPC_OP_MTFSF;
        inst->fm = (raw >> 17) & 0xFFu;
        inst->rB = PPC_RB(raw);
        inst->rc = PPC_RC(raw);
        return true;
    default:
        switch (PPC_A_XO(raw)) {
        case 18: decode_a_frt_fra_frb_frc(inst, PPC_OP_FDIV, raw); return true;
        case 20: decode_a_frt_fra_frb_frc(inst, PPC_OP_FSUB, raw); return true;
        case 21: decode_a_frt_fra_frb_frc(inst, PPC_OP_FADD, raw); return true;
        case 23: decode_a_frt_fra_frb_frc(inst, PPC_OP_FSEL, raw); return true;
        case 25: decode_a_frt_fra_frb_frc(inst, PPC_OP_FMUL, raw); return true;
        case 26:
            if (PPC_RA(raw) == 0 && PPC_RC_REG(raw) == 0) {
                decode_x_frt_frb(inst, PPC_OP_FRSQRTE, raw);
                return true;
            }
            break;
        case 28: decode_a_frt_fra_frb_frc(inst, PPC_OP_FMSUB, raw); return true;
        case 29: decode_a_frt_fra_frb_frc(inst, PPC_OP_FMADD, raw); return true;
        case 30: decode_a_frt_fra_frb_frc(inst, PPC_OP_FNMSUB, raw); return true;
        case 31: decode_a_frt_fra_frb_frc(inst, PPC_OP_FNMADD, raw); return true;
        default: break;
        }
        break;
    }
    return false;
}
