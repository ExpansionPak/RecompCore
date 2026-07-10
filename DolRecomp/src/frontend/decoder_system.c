#include "decoder_private.h"

bool decode_primary_19(PPCInst* inst, u32 raw, u32 address) {
    (void)address;
    u32 xo = PPC_XO(raw);
    if (xo == 0) {
        if (raw & ((3u << 21) | (0x7Fu << 11) | 1u)) {
            inst->op = PPC_OP_UNKNOWN;
        } else {
            inst->op   = PPC_OP_MCRF;
            inst->crfD = (raw >> 23) & 0x7;
            inst->crfS = (raw >> 18) & 0x7;
        }
        return true;
    } else if (raw == 0x4C000064u) {
        inst->op = PPC_OP_RFI;
        return true;
    } else if (xo == 16) {
        inst->op = PPC_OP_BCLR;
        inst->bo = PPC_BO(raw);
        inst->bi = PPC_BI(raw);
        inst->lk = raw & 1;
        return true;
    } else if (raw == 0x4C00012Cu) {
        inst->op = PPC_OP_ISYNC;
        return true;
    } else if (xo == 33) {
        decode_cr_logical(inst, PPC_OP_CRNOR, raw);
        return true;
    } else if (xo == 129) {
        decode_cr_logical(inst, PPC_OP_CRANDC, raw);
        return true;
    } else if (xo == 193) {
        decode_cr_logical(inst, PPC_OP_CRXOR, raw);
        return true;
    } else if (xo == 225) {
        decode_cr_logical(inst, PPC_OP_CRNAND, raw);
        return true;
    } else if (xo == 257) {
        decode_cr_logical(inst, PPC_OP_CRAND, raw);
        return true;
    } else if (xo == 289) {
        decode_cr_logical(inst, PPC_OP_CREQV, raw);
        return true;
    } else if (xo == 417) {
        decode_cr_logical(inst, PPC_OP_CRORC, raw);
        return true;
    } else if (xo == 449) {
        decode_cr_logical(inst, PPC_OP_CROR, raw);
        return true;
    } else if (xo == 528) {
        if ((PPC_BO(raw) & 4u) == 0) {
            inst->op = PPC_OP_UNKNOWN;
        } else {
            inst->op = PPC_OP_BCCTR;
            inst->bo = PPC_BO(raw);
            inst->bi = PPC_BI(raw);
            inst->lk = raw & 1;
        }
        return true;
    }
    return false;
}

// System sub-opcodes under primary opcode 31
bool decode_primary_31_system(PPCInst* inst, u32 raw, u32 address) {
    (void)address;
    u32 xo = PPC_XO(raw);
    switch (xo) {
    case 54:
        if (PPC_RD(raw) == 0 && !PPC_RC(raw)) {
            inst->op = PPC_OP_DCBST;
            inst->rA = PPC_RA(raw);
            inst->rB = PPC_RB(raw);
            return true;
        }
        break;
    case 83:
        if (PPC_RA(raw) == 0 && PPC_RB(raw) == 0 && !PPC_RC(raw)) {
            inst->op = PPC_OP_MFMSR;
            inst->rD = PPC_RD(raw);
            return true;
        }
        break;
    case 86:
        if (PPC_RD(raw) == 0 && !PPC_RC(raw)) {
            inst->op = PPC_OP_DCBF;
            inst->rA = PPC_RA(raw);
            inst->rB = PPC_RB(raw);
            return true;
        }
        break;
    case 144:
        if ((raw & ((1u << 20) | 1u)) == 0) {
            inst->op  = PPC_OP_MTCRF;
            inst->rS  = PPC_RS(raw);
            inst->crm = PPC_CRM(raw);
            return true;
        }
        break;
    case 146:
        if (PPC_RA(raw) == 0 && PPC_RB(raw) == 0 && !PPC_RC(raw)) {
            inst->op = PPC_OP_MTMSR;
            inst->rS = PPC_RS(raw);
            return true;
        }
        break;
    case 210:
        if (((raw >> 20) & 1u) == 0 && PPC_RB(raw) == 0 && !PPC_RC(raw)) {
            inst->op = PPC_OP_MTSR;
            inst->rS = PPC_RS(raw);
            inst->sr = PPC_RA(raw) & 0xFu;
            return true;
        }
        break;
    case 242:
        if (PPC_RA(raw) == 0 && !PPC_RC(raw)) {
            inst->op = PPC_OP_MTSRIN;
            inst->rS = PPC_RS(raw);
            inst->rB = PPC_RB(raw);
            return true;
        }
        break;
    case 246:
        if (PPC_RD(raw) == 0 && !PPC_RC(raw)) {
            inst->op = PPC_OP_DCBTST;
            inst->rA = PPC_RA(raw);
            inst->rB = PPC_RB(raw);
            return true;
        }
        break;
    case 278:
        if (PPC_RD(raw) == 0 && !PPC_RC(raw)) {
            inst->op = PPC_OP_DCBT;
            inst->rA = PPC_RA(raw);
            inst->rB = PPC_RB(raw);
            return true;
        }
        break;
    case 306:
        if (PPC_RD(raw) == 0 && PPC_RA(raw) == 0 && !PPC_RC(raw)) {
            inst->op = PPC_OP_TLBIE;
            inst->rB = PPC_RB(raw);
            return true;
        }
        break;
    case 310:
        if (!PPC_RC(raw)) {
            decode_x_rt_ra_rb(inst, PPC_OP_ECIWX, raw);
            return true;
        }
        break;
    case 339:
        if (!PPC_RC(raw)) {
            inst->op  = PPC_OP_MFSPR;
            inst->rD  = PPC_RD(raw);
            inst->spr = PPC_SPR(raw);
            return true;
        }
        break;
    case 371:
        if (!PPC_RC(raw)) {
            inst->op = PPC_OP_MFTB;
            inst->rD = PPC_RD(raw);
            inst->spr = PPC_SPR(raw);
            return true;
        }
        break;
    case 438:
        if (!PPC_RC(raw)) {
            decode_x_rs_ra_rb(inst, PPC_OP_ECOWX, raw);
            return true;
        }
        break;
    case 470:
        if (PPC_RD(raw) == 0 && !PPC_RC(raw)) {
            inst->op = PPC_OP_DCBI;
            inst->rA = PPC_RA(raw);
            inst->rB = PPC_RB(raw);
            return true;
        }
        break;
    case 467:
        if (!PPC_RC(raw)) {
            inst->op  = PPC_OP_MTSPR;
            inst->rS  = PPC_RS(raw);
            inst->spr = PPC_SPR(raw);
            return true;
        }
        break;
    case 595:
        if (((raw >> 20) & 1u) == 0 && PPC_RB(raw) == 0 && !PPC_RC(raw)) {
            inst->op = PPC_OP_MFSR;
            inst->rD = PPC_RD(raw);
            inst->sr = PPC_RA(raw) & 0xFu;
            return true;
        }
        break;
    case 598:
        if (raw == 0x7C0004ACu) {
            inst->op = PPC_OP_SYNC;
            return true;
        }
        break;
    case 659:
        if (PPC_RA(raw) == 0 && !PPC_RC(raw)) {
            inst->op = PPC_OP_MFSRIN;
            inst->rD = PPC_RD(raw);
            inst->rB = PPC_RB(raw);
            return true;
        }
        break;
    case 854:
        if (raw == 0x7C0006ACu) {
            inst->op = PPC_OP_EIEIO;
            return true;
        }
        break;
    case 566:
        if (raw == 0x7C00046Cu) {
            inst->op = PPC_OP_TLBSYNC;
            return true;
        }
        break;
    case 982:
        if (PPC_RD(raw) == 0 && !PPC_RC(raw)) {
            inst->op = PPC_OP_ICBI;
            inst->rA = PPC_RA(raw);
            inst->rB = PPC_RB(raw);
            return true;
        }
        break;
    case 983:
        if (!PPC_RC(raw)) {
            decode_x_rs_ra_rb(inst, PPC_OP_STFIWX, raw);
            return true;
        }
        break;
    case 1014:
        if (PPC_RD(raw) == 0 && !PPC_RC(raw)) {
            inst->op = PPC_OP_DCBZ;
            inst->rA = PPC_RA(raw);
            inst->rB = PPC_RB(raw);
            return true;
        }
        break;
    default:
        break;
    }
    return false;
}
