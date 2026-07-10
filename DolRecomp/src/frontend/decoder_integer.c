#include "decoder_private.h"

bool decode_primary_31_integer(PPCInst* inst, u32 raw, u32 address) {
    (void)address;
    u32 xo = PPC_XO(raw);
    u32 xo9 = xo & 0x1FFu;
    bool oe = ((raw >> 10) & 1u) != 0;

    switch (xo9) {
    case 8:   decode_xo_rt_ra_rb(inst, oe ? PPC_OP_SUBFCO : PPC_OP_SUBFC, raw); return true;
    case 10:  decode_xo_rt_ra_rb(inst, oe ? PPC_OP_ADDCO : PPC_OP_ADDC, raw); return true;
    case 40:  decode_xo_rt_ra_rb(inst, oe ? PPC_OP_SUBFO : PPC_OP_SUBF, raw); return true;
    case 104:
        if (PPC_RB(raw) == 0) { decode_xo_rt_ra(inst, oe ? PPC_OP_NEGO : PPC_OP_NEG, raw); return true; }
        break;
    case 136: decode_xo_rt_ra_rb(inst, oe ? PPC_OP_SUBFEO : PPC_OP_SUBFE, raw); return true;
    case 138: decode_xo_rt_ra_rb(inst, oe ? PPC_OP_ADDEO : PPC_OP_ADDE, raw); return true;
    case 200:
        if (PPC_RB(raw) == 0) { decode_xo_rt_ra(inst, oe ? PPC_OP_SUBFZEO : PPC_OP_SUBFZE, raw); return true; }
        break;
    case 202:
        if (PPC_RB(raw) == 0) { decode_xo_rt_ra(inst, oe ? PPC_OP_ADDZEO : PPC_OP_ADDZE, raw); return true; }
        break;
    case 232:
        if (PPC_RB(raw) == 0) { decode_xo_rt_ra(inst, oe ? PPC_OP_SUBFMEO : PPC_OP_SUBFME, raw); return true; }
        break;
    case 234:
        if (PPC_RB(raw) == 0) { decode_xo_rt_ra(inst, oe ? PPC_OP_ADDMEO : PPC_OP_ADDME, raw); return true; }
        break;
    case 235: decode_xo_rt_ra_rb(inst, oe ? PPC_OP_MULLWO : PPC_OP_MULLW, raw); return true;
    case 266: decode_xo_rt_ra_rb(inst, oe ? PPC_OP_ADDO : PPC_OP_ADD, raw); return true;
    case 459: decode_xo_rt_ra_rb(inst, oe ? PPC_OP_DIVWUO : PPC_OP_DIVWU, raw); return true;
    case 491: decode_xo_rt_ra_rb(inst, oe ? PPC_OP_DIVWO : PPC_OP_DIVW, raw); return true;
    default: break;
    }

    switch (xo) {
    case 0: // cmp/cmpw
        if ((raw >> 21) & 1u) break;
        inst->op   = PPC_OP_CMP;
        inst->crfD = (raw >> 23) & 0x7;
        inst->l    = (raw >> 21) & 0x1;
        inst->rA   = PPC_RA(raw);
        inst->rB   = PPC_RB(raw);
        return true;
    case 4:
        if (!PPC_RC(raw)) {
            inst->op = PPC_OP_TW;
            inst->to = PPC_RD(raw);
            inst->rA = PPC_RA(raw);
            inst->rB = PPC_RB(raw);
            return true;
        }
        break;
    case 11:  decode_x_rt_ra_rb(inst, PPC_OP_MULHWU, raw); return true;
    case 19:
        if (PPC_RA(raw) == 0 && PPC_RB(raw) == 0 && !PPC_RC(raw)) {
            inst->op = PPC_OP_MFCR;
            inst->rD = PPC_RD(raw);
            return true;
        }
        break;
    case 20:
        if (!PPC_RC(raw)) { decode_x_rt_ra_rb(inst, PPC_OP_LWARX, raw); return true; }
        break;
    case 23:  decode_x_rt_ra_rb_norc(inst, PPC_OP_LWZX, raw); return true;
    case 24:  decode_x_rs_ra_rb(inst, PPC_OP_SLW, raw); return true;
    case 26:  decode_x_rs_ra_rb(inst, PPC_OP_CNTLZW, raw); return true;
    case 28:  decode_x_rs_ra_rb(inst, PPC_OP_AND, raw); return true;
    case 32: // cmpl/cmplw
        if ((raw >> 21) & 1u) break;
        inst->op   = PPC_OP_CMPL;
        inst->crfD = (raw >> 23) & 0x7;
        inst->l    = (raw >> 21) & 0x1;
        inst->rA   = PPC_RA(raw);
        inst->rB   = PPC_RB(raw);
        return true;
    case 55:  decode_x_rt_ra_rb_update(inst, PPC_OP_LWZUX, raw); return true;
    case 60:  decode_x_rs_ra_rb(inst, PPC_OP_ANDC, raw); return true;
    case 75:  decode_x_rt_ra_rb(inst, PPC_OP_MULHW, raw); return true;
    case 87:  decode_x_rt_ra_rb_norc(inst, PPC_OP_LBZX, raw); return true;
    case 119: decode_x_rt_ra_rb_update(inst, PPC_OP_LBZUX, raw); return true;
    case 124: decode_x_rs_ra_rb(inst, PPC_OP_NOR, raw); return true;
    case 151: decode_x_rs_ra_rb_norc(inst, PPC_OP_STWX, raw); return true;
    case 183: decode_x_rs_ra_rb_update(inst, PPC_OP_STWUX, raw); return true;
    case 215: decode_x_rs_ra_rb_norc(inst, PPC_OP_STBX, raw); return true;
    case 247: decode_x_rs_ra_rb_update(inst, PPC_OP_STBUX, raw); return true;
    case 279: decode_x_rt_ra_rb_norc(inst, PPC_OP_LHZX, raw); return true;
    case 284: decode_x_rs_ra_rb(inst, PPC_OP_EQV, raw); return true;
    case 311: decode_x_rt_ra_rb_update(inst, PPC_OP_LHZUX, raw); return true;
    case 316: decode_x_rs_ra_rb(inst, PPC_OP_XOR, raw); return true;
    case 343: decode_x_rt_ra_rb_norc(inst, PPC_OP_LHAX, raw); return true;
    case 375: decode_x_rt_ra_rb_update(inst, PPC_OP_LHAUX, raw); return true;
    case 407: decode_x_rs_ra_rb_norc(inst, PPC_OP_STHX, raw); return true;
    case 412: decode_x_rs_ra_rb(inst, PPC_OP_ORC, raw); return true;
    case 439: decode_x_rs_ra_rb_update(inst, PPC_OP_STHUX, raw); return true;
    case 444: decode_x_rs_ra_rb(inst, PPC_OP_OR, raw); return true;
    case 476: decode_x_rs_ra_rb(inst, PPC_OP_NAND, raw); return true;
    case 512:
        if ((raw & ((3u << 21) | (0x1Fu << 16) | (0x1Fu << 11) | 1u)) == 0) {
            inst->op = PPC_OP_MCRXR;
            inst->crfD = (raw >> 23) & 7u;
            return true;
        }
        break;
    case 533:
        if (!PPC_RC(raw) && PPC_RD(raw) != PPC_RA(raw) && PPC_RD(raw) != PPC_RB(raw)) {
            decode_x_rt_ra_rb(inst, PPC_OP_LSWX, raw);
            return true;
        }
        break;
    case 534: decode_x_rt_ra_rb_norc(inst, PPC_OP_LWBRX, raw); return true;
    case 535: decode_x_rt_ra_rb_norc(inst, PPC_OP_LFSX, raw); return true;
    case 536: decode_x_rs_ra_rb(inst, PPC_OP_SRW, raw); return true;
    case 567: decode_x_frt_ra_rb_update(inst, PPC_OP_LFSUX, raw); return true;
    case 597:
        if (!PPC_RC(raw) &&
            !reg_in_wrapped_range(PPC_RD(raw), string_register_count(PPC_RB(raw)), PPC_RA(raw))) {
            inst->op = PPC_OP_LSWI;
            inst->rD = PPC_RD(raw);
            inst->rA = PPC_RA(raw);
            inst->nb = PPC_RB(raw);
            return true;
        }
        break;
    case 599: decode_x_rt_ra_rb_norc(inst, PPC_OP_LFDX, raw); return true;
    case 631: decode_x_frt_ra_rb_update(inst, PPC_OP_LFDUX, raw); return true;
    case 661:
        if (!PPC_RC(raw)) { decode_x_rs_ra_rb(inst, PPC_OP_STSWX, raw); return true; }
        break;
    case 662: decode_x_rs_ra_rb_norc(inst, PPC_OP_STWBRX, raw); return true;
    case 663: decode_x_rs_ra_rb_norc(inst, PPC_OP_STFSX, raw); return true;
    case 695: decode_x_rs_ra_rb_update(inst, PPC_OP_STFSUX, raw); return true;
    case 727: decode_x_rs_ra_rb_norc(inst, PPC_OP_STFDX, raw); return true;
    case 759: decode_x_rs_ra_rb_update(inst, PPC_OP_STFDUX, raw); return true;
    case 725:
        if (!PPC_RC(raw)) {
            inst->op = PPC_OP_STSWI;
            inst->rS = PPC_RS(raw);
            inst->rA = PPC_RA(raw);
            inst->nb = PPC_RB(raw);
            return true;
        }
        break;
    case 790: decode_x_rt_ra_rb_norc(inst, PPC_OP_LHBRX, raw); return true;
    case 792: decode_x_rs_ra_rb(inst, PPC_OP_SRAW, raw); return true;
    case 824:
        inst->op = PPC_OP_SRAWI;
        inst->rS = PPC_RS(raw);
        inst->rA = PPC_RA(raw);
        inst->sh = PPC_SH(raw);
        inst->rc = PPC_RC(raw);
        return true;
    case 918: decode_x_rs_ra_rb_norc(inst, PPC_OP_STHBRX, raw); return true;
    case 922: decode_x_rs_ra_rb(inst, PPC_OP_EXTSH, raw); return true;
    case 954: decode_x_rs_ra_rb(inst, PPC_OP_EXTSB, raw); return true;
    case 150:
        if (PPC_RC(raw)) {
            decode_x_rs_ra_rb(inst, PPC_OP_STWCX, raw);
            inst->rc = true;
            return true;
        }
        break;
    default:
        break;
    }
    return false;
}
