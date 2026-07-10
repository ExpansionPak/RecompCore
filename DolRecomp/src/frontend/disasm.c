// SPDX-License-Identifier: GPL-3.0-or-later
#include "decoder.h"
#include <stdio.h>
#include <string.h>

static const char* spr_name(u16 spr) {
    switch (spr) {
    case 1: return "xer";
    case 8: return "lr";
    case 9: return "ctr";
    case 26: return "srr0";
    case 27: return "srr1";
    case 282: return "ear";
    case 912: return "gqr0";
    case 913: return "gqr1";
    case 914: return "gqr2";
    case 915: return "gqr3";
    case 916: return "gqr4";
    case 917: return "gqr5";
    case 918: return "gqr6";
    case 919: return "gqr7";
    case 920: return "hid2";
    default: return NULL;
    }
}

static const char* dot(const PPCInst* inst) {
    return inst->rc ? "." : "";
}

char* ppc_disasm(char* buf, size_t buf_size, const PPCInst* inst) {
    switch (inst->op) {
    case PPC_OP_MULLI:
        snprintf(buf, buf_size, "mulli   r%u, r%u, %d",
                 inst->rD, inst->rA, (int)inst->simm);
        break;

    case PPC_OP_SUBFIC:
        snprintf(buf, buf_size, "subfic  r%u, r%u, %d",
                 inst->rD, inst->rA, (int)inst->simm);
        break;

    case PPC_OP_ADDI:
        if (inst->rA == 0) {
            snprintf(buf, buf_size, "li      r%u, %d",
                     inst->rD, (int)inst->simm);
        } else {
            snprintf(buf, buf_size, "addi    r%u, r%u, %d",
                     inst->rD, inst->rA, (int)inst->simm);
        }
        break;

    case PPC_OP_ADDIC:
        snprintf(buf, buf_size, "addic   r%u, r%u, %d",
                 inst->rD, inst->rA, (int)inst->simm);
        break;

    case PPC_OP_ADDIC_DOT:
        snprintf(buf, buf_size, "addic.  r%u, r%u, %d",
                 inst->rD, inst->rA, (int)inst->simm);
        break;

    case PPC_OP_ADDIS:
        if (inst->rA == 0) {
            snprintf(buf, buf_size, "lis     r%u, %d",
                     inst->rD, (int)inst->simm);
        } else {
            snprintf(buf, buf_size, "addis   r%u, r%u, %d",
                     inst->rD, inst->rA, (int)inst->simm);
        }
        break;

    case PPC_OP_CMPI:
        if (inst->crfD == 0) {
            snprintf(buf, buf_size, "cmpwi   r%u, %d",
                     inst->rA, (int)inst->simm);
        } else {
            snprintf(buf, buf_size, "cmpwi   cr%u, r%u, %d",
                     inst->crfD, inst->rA, (int)inst->simm);
        }
        break;

    case PPC_OP_CMPLI:
        if (inst->crfD == 0) {
            snprintf(buf, buf_size, "cmplwi  r%u, 0x%04X",
                     inst->rA, inst->uimm);
        } else {
            snprintf(buf, buf_size, "cmplwi  cr%u, r%u, 0x%04X",
                     inst->crfD, inst->rA, inst->uimm);
        }
        break;

    case PPC_OP_TWI:
        snprintf(buf, buf_size, "twi     %u, r%u, %d",
                 inst->to, inst->rA, (int)inst->simm);
        break;

    case PPC_OP_CMP:
        if (inst->crfD == 0) {
            snprintf(buf, buf_size, "cmpw    r%u, r%u",
                     inst->rA, inst->rB);
        } else {
            snprintf(buf, buf_size, "cmpw    cr%u, r%u, r%u",
                     inst->crfD, inst->rA, inst->rB);
        }
        break;

    case PPC_OP_CMPL:
        if (inst->crfD == 0) {
            snprintf(buf, buf_size, "cmplw   r%u, r%u",
                     inst->rA, inst->rB);
        } else {
            snprintf(buf, buf_size, "cmplw   cr%u, r%u, r%u",
                     inst->crfD, inst->rA, inst->rB);
        }
        break;

    case PPC_OP_TW:
        snprintf(buf, buf_size, "tw      %u, r%u, r%u",
                 inst->to, inst->rA, inst->rB);
        break;

    case PPC_OP_ORI:
        if (inst->rS == 0 && inst->rA == 0 && inst->uimm == 0) {
            snprintf(buf, buf_size, "nop");
        } else {
            snprintf(buf, buf_size, "ori     r%u, r%u, 0x%04X",
                     inst->rA, inst->rS, inst->uimm);
        }
        break;

    case PPC_OP_ORIS:
        snprintf(buf, buf_size, "oris    r%u, r%u, 0x%04X",
                 inst->rA, inst->rS, inst->uimm);
        break;

    case PPC_OP_XORI:
        snprintf(buf, buf_size, "xori    r%u, r%u, 0x%04X",
                 inst->rA, inst->rS, inst->uimm);
        break;

    case PPC_OP_XORIS:
        snprintf(buf, buf_size, "xoris   r%u, r%u, 0x%04X",
                 inst->rA, inst->rS, inst->uimm);
        break;

    case PPC_OP_ANDI:
        snprintf(buf, buf_size, "andi.   r%u, r%u, 0x%04X",
                 inst->rA, inst->rS, inst->uimm);
        break;

    case PPC_OP_ANDIS:
        snprintf(buf, buf_size, "andis.  r%u, r%u, 0x%04X",
                 inst->rA, inst->rS, inst->uimm);
        break;

    case PPC_OP_AND:
    case PPC_OP_ANDC:
    case PPC_OP_OR:
    case PPC_OP_ORC:
    case PPC_OP_XOR:
    case PPC_OP_NAND:
    case PPC_OP_NOR:
    case PPC_OP_EQV:
    case PPC_OP_SLW:
    case PPC_OP_SRW:
    case PPC_OP_SRAW:
        snprintf(buf, buf_size, "%s%s   r%u, r%u, r%u",
                 ppc_op_name(inst->op), dot(inst), inst->rA, inst->rS, inst->rB);
        break;

    case PPC_OP_CNTLZW:
    case PPC_OP_EXTSB:
    case PPC_OP_EXTSH:
        snprintf(buf, buf_size, "%s%s r%u, r%u",
                 ppc_op_name(inst->op), dot(inst), inst->rA, inst->rS);
        break;

    case PPC_OP_ADD:
    case PPC_OP_ADDO:
    case PPC_OP_ADDC:
    case PPC_OP_ADDCO:
    case PPC_OP_ADDE:
    case PPC_OP_ADDEO:
    case PPC_OP_SUBF:
    case PPC_OP_SUBFO:
    case PPC_OP_SUBFC:
    case PPC_OP_SUBFCO:
    case PPC_OP_SUBFE:
    case PPC_OP_SUBFEO:
    case PPC_OP_MULLW:
    case PPC_OP_MULLWO:
    case PPC_OP_MULHW:
    case PPC_OP_MULHWU:
    case PPC_OP_DIVW:
    case PPC_OP_DIVWO:
    case PPC_OP_DIVWU:
    case PPC_OP_DIVWUO:
        snprintf(buf, buf_size, "%s%s   r%u, r%u, r%u",
                 ppc_op_name(inst->op), dot(inst), inst->rD, inst->rA, inst->rB);
        break;

    case PPC_OP_ADDZE:
    case PPC_OP_ADDZEO:
    case PPC_OP_ADDME:
    case PPC_OP_ADDMEO:
    case PPC_OP_SUBFZE:
    case PPC_OP_SUBFZEO:
    case PPC_OP_SUBFME:
    case PPC_OP_SUBFMEO:
    case PPC_OP_NEG:
    case PPC_OP_NEGO:
        snprintf(buf, buf_size, "%s%s  r%u, r%u",
                 ppc_op_name(inst->op), dot(inst), inst->rD, inst->rA);
        break;

    case PPC_OP_SRAWI:
        snprintf(buf, buf_size, "srawi%s r%u, r%u, %u",
                 dot(inst), inst->rA, inst->rS, inst->sh);
        break;

    case PPC_OP_RLWINM:
        snprintf(buf, buf_size, "rlwinm%s r%u, r%u, %u, %u, %u",
                 dot(inst), inst->rA, inst->rS, inst->sh, inst->mb, inst->me);
        break;

    case PPC_OP_RLWNM:
        snprintf(buf, buf_size, "rlwnm%s r%u, r%u, r%u, %u, %u",
                 dot(inst), inst->rA, inst->rS, inst->rB, inst->mb, inst->me);
        break;

    case PPC_OP_RLWIMI:
        snprintf(buf, buf_size, "rlwimi%s r%u, r%u, %u, %u, %u",
                 dot(inst), inst->rA, inst->rS, inst->sh, inst->mb, inst->me);
        break;

    case PPC_OP_LWZ:
    case PPC_OP_LWZU:
    case PPC_OP_LBZ:
    case PPC_OP_LBZU:
    case PPC_OP_LHZ:
    case PPC_OP_LHZU:
    case PPC_OP_LHA:
    case PPC_OP_LHAU:
    case PPC_OP_LMW:
        snprintf(buf, buf_size, "%s     r%u, %d(r%u)",
                 ppc_op_name(inst->op), inst->rD, (int)inst->simm, inst->rA);
        break;

    case PPC_OP_LFS:
    case PPC_OP_LFSU:
    case PPC_OP_LFD:
    case PPC_OP_LFDU:
        snprintf(buf, buf_size, "%s     f%u, %d(r%u)",
                 ppc_op_name(inst->op), inst->rD, (int)inst->simm, inst->rA);
        break;

    case PPC_OP_STW:
    case PPC_OP_STWU:
    case PPC_OP_STB:
    case PPC_OP_STBU:
    case PPC_OP_STH:
    case PPC_OP_STHU:
    case PPC_OP_STMW:
        snprintf(buf, buf_size, "%s     r%u, %d(r%u)",
                 ppc_op_name(inst->op), inst->rS, (int)inst->simm, inst->rA);
        break;

    case PPC_OP_STFS:
    case PPC_OP_STFSU:
    case PPC_OP_STFD:
    case PPC_OP_STFDU:
        snprintf(buf, buf_size, "%s     f%u, %d(r%u)",
                 ppc_op_name(inst->op), inst->rS, (int)inst->simm, inst->rA);
        break;

    case PPC_OP_LWZX:
    case PPC_OP_LWZUX:
    case PPC_OP_LBZX:
    case PPC_OP_LBZUX:
    case PPC_OP_LHZX:
    case PPC_OP_LHZUX:
    case PPC_OP_LHAX:
    case PPC_OP_LHAUX:
    case PPC_OP_LWBRX:
    case PPC_OP_LHBRX:
    case PPC_OP_LSWX:
    case PPC_OP_LWARX:
        snprintf(buf, buf_size, "%s    r%u, r%u, r%u",
                 ppc_op_name(inst->op), inst->rD, inst->rA, inst->rB);
        break;

    case PPC_OP_LFSX:
    case PPC_OP_LFSUX:
    case PPC_OP_LFDX:
    case PPC_OP_LFDUX:
        snprintf(buf, buf_size, "%s    f%u, r%u, r%u",
                 ppc_op_name(inst->op), inst->rD, inst->rA, inst->rB);
        break;

    case PPC_OP_STWX:
    case PPC_OP_STWUX:
    case PPC_OP_STBX:
    case PPC_OP_STBUX:
    case PPC_OP_STHX:
    case PPC_OP_STHUX:
    case PPC_OP_STWBRX:
    case PPC_OP_STHBRX:
    case PPC_OP_STSWX:
    case PPC_OP_STWCX:
        snprintf(buf, buf_size, "%s    r%u, r%u, r%u",
                 ppc_op_name(inst->op), inst->rS, inst->rA, inst->rB);
        break;

    case PPC_OP_STFSX:
    case PPC_OP_STFSUX:
    case PPC_OP_STFDX:
    case PPC_OP_STFDUX:
    case PPC_OP_STFIWX:
        snprintf(buf, buf_size, "%s    f%u, r%u, r%u",
                 ppc_op_name(inst->op), inst->rS, inst->rA, inst->rB);
        break;

    case PPC_OP_FADDS:
    case PPC_OP_FSUBS:
    case PPC_OP_FDIVS:
    case PPC_OP_FADD:
    case PPC_OP_FSUB:
    case PPC_OP_FDIV:
        snprintf(buf, buf_size, "%s%s   f%u, f%u, f%u",
                 ppc_op_name(inst->op), dot(inst), inst->rD, inst->rA, inst->rB);
        break;

    case PPC_OP_FMADD:
    case PPC_OP_FMSUB:
    case PPC_OP_FNMADD:
    case PPC_OP_FNMSUB:
    case PPC_OP_FMADDS:
    case PPC_OP_FMSUBS:
    case PPC_OP_FNMADDS:
    case PPC_OP_FNMSUBS:
        snprintf(buf, buf_size, "%s%s f%u, f%u, f%u, f%u",
                 ppc_op_name(inst->op), dot(inst), inst->rD, inst->rA,
                 inst->rC, inst->rB);
        break;

    case PPC_OP_FMULS:
    case PPC_OP_FMUL:
        snprintf(buf, buf_size, "%s%s   f%u, f%u, f%u",
                 ppc_op_name(inst->op), dot(inst), inst->rD, inst->rA, inst->rC);
        break;

    case PPC_OP_FSEL:
        snprintf(buf, buf_size, "fsel%s   f%u, f%u, f%u, f%u",
                 dot(inst), inst->rD, inst->rA, inst->rC, inst->rB);
        break;

    case PPC_OP_FMR:
    case PPC_OP_FNEG:
    case PPC_OP_FABS:
    case PPC_OP_FNABS:
    case PPC_OP_FRSP:
    case PPC_OP_FRES:
    case PPC_OP_FRSQRTE:
    case PPC_OP_FCTIW:
    case PPC_OP_FCTIWZ:
        snprintf(buf, buf_size, "%s%s    f%u, f%u",
                 ppc_op_name(inst->op), dot(inst), inst->rD, inst->rB);
        break;

    case PPC_OP_FCMPU:
    case PPC_OP_FCMPO:
        snprintf(buf, buf_size, "%s   cr%u, f%u, f%u",
                 ppc_op_name(inst->op), inst->crfD, inst->rA, inst->rB);
        break;

    case PPC_OP_MTFSB0:
    case PPC_OP_MTFSB1:
        snprintf(buf, buf_size, "%s%s  %u",
                 ppc_op_name(inst->op), dot(inst), inst->rD);
        break;

    case PPC_OP_MFFS:
        snprintf(buf, buf_size, "mffs%s   f%u", dot(inst), inst->rD);
        break;

    case PPC_OP_MCRFS:
        snprintf(buf, buf_size, "mcrfs   cr%u, cr%u", inst->crfD, inst->crfS);
        break;

    case PPC_OP_MTFSFI:
        snprintf(buf, buf_size, "mtfsfi%s %u, %u", dot(inst), inst->crfD, inst->imm);
        break;

    case PPC_OP_MTFSF:
        snprintf(buf, buf_size, "mtfsf%s  0x%02X, f%u", dot(inst), inst->fm, inst->rB);
        break;

    case PPC_OP_PSQ_L:
    case PPC_OP_PSQ_LU:
        snprintf(buf, buf_size, "%s   f%u, %d(r%u), %u, %u",
                 ppc_op_name(inst->op), inst->rD, (int)inst->simm,
                 inst->rA, inst->w, inst->i);
        break;

    case PPC_OP_PSQ_ST:
    case PPC_OP_PSQ_STU:
        snprintf(buf, buf_size, "%s   f%u, %d(r%u), %u, %u",
                 ppc_op_name(inst->op), inst->rS, (int)inst->simm,
                 inst->rA, inst->w, inst->i);
        break;

    case PPC_OP_PSQ_LX:
    case PPC_OP_PSQ_LUX:
        snprintf(buf, buf_size, "%s   f%u, r%u, r%u, %u, %u",
                 ppc_op_name(inst->op), inst->rD, inst->rA, inst->rB,
                 inst->w, inst->i);
        break;

    case PPC_OP_PSQ_STX:
    case PPC_OP_PSQ_STUX:
        snprintf(buf, buf_size, "%s   f%u, r%u, r%u, %u, %u",
                 ppc_op_name(inst->op), inst->rS, inst->rA, inst->rB,
                 inst->w, inst->i);
        break;

    case PPC_OP_PS_ADD:
    case PPC_OP_PS_SUB:
    case PPC_OP_PS_DIV:
        snprintf(buf, buf_size, "%s%s  f%u, f%u, f%u",
                 ppc_op_name(inst->op), dot(inst), inst->rD, inst->rA, inst->rB);
        break;

    case PPC_OP_PS_RES:
    case PPC_OP_PS_RSQRTE:
        snprintf(buf, buf_size, "%s%s f%u, f%u",
                 ppc_op_name(inst->op), dot(inst), inst->rD, inst->rB);
        break;

    case PPC_OP_LSWI:
        snprintf(buf, buf_size, "lswi    r%u, r%u, %u", inst->rD, inst->rA,
                 inst->nb ? inst->nb : 32u);
        break;

    case PPC_OP_STSWI:
        snprintf(buf, buf_size, "stswi   r%u, r%u, %u", inst->rS, inst->rA,
                 inst->nb ? inst->nb : 32u);
        break;

    case PPC_OP_SYNC:
    case PPC_OP_EIEIO:
    case PPC_OP_ISYNC:
    case PPC_OP_SC:
    case PPC_OP_RFI:
    case PPC_OP_TLBSYNC:
        snprintf(buf, buf_size, "%s", ppc_op_name(inst->op));
        break;

    case PPC_OP_PS_MUL:
    case PPC_OP_PS_MULS0:
    case PPC_OP_PS_MULS1:
        snprintf(buf, buf_size, "%s%s  f%u, f%u, f%u",
                 ppc_op_name(inst->op), dot(inst), inst->rD, inst->rA, inst->rC);
        break;

    case PPC_OP_PS_MADD:
    case PPC_OP_PS_MSUB:
    case PPC_OP_PS_NMADD:
    case PPC_OP_PS_NMSUB:
    case PPC_OP_PS_SUM0:
    case PPC_OP_PS_SUM1:
    case PPC_OP_PS_MADDS0:
    case PPC_OP_PS_MADDS1:
    case PPC_OP_PS_SEL:
        snprintf(buf, buf_size, "%s%s  f%u, f%u, f%u, f%u",
                 ppc_op_name(inst->op), dot(inst), inst->rD, inst->rA,
                 inst->rC, inst->rB);
        break;

    case PPC_OP_PS_NEG:
    case PPC_OP_PS_ABS:
    case PPC_OP_PS_NABS:
    case PPC_OP_PS_MR:
        snprintf(buf, buf_size, "%s%s  f%u, f%u",
                 ppc_op_name(inst->op), dot(inst), inst->rD, inst->rB);
        break;

    case PPC_OP_PS_MERGE00:
    case PPC_OP_PS_MERGE01:
    case PPC_OP_PS_MERGE10:
    case PPC_OP_PS_MERGE11:
        snprintf(buf, buf_size, "%s%s  f%u, f%u, f%u",
                 ppc_op_name(inst->op), dot(inst), inst->rD, inst->rA, inst->rB);
        break;

    case PPC_OP_PS_CMPU0:
    case PPC_OP_PS_CMPO0:
    case PPC_OP_PS_CMPU1:
    case PPC_OP_PS_CMPO1:
        snprintf(buf, buf_size, "%s cr%u, f%u, f%u",
                 ppc_op_name(inst->op), inst->crfD, inst->rA, inst->rB);
        break;

    case PPC_OP_DCBZ:
    case PPC_OP_DCBZ_L:
    case PPC_OP_DCBST:
    case PPC_OP_DCBF:
    case PPC_OP_DCBTST:
    case PPC_OP_DCBT:
    case PPC_OP_DCBI:
    case PPC_OP_ICBI:
        if (inst->rA == 0) {
            snprintf(buf, buf_size, "%s    0, r%u", ppc_op_name(inst->op), inst->rB);
        } else {
            snprintf(buf, buf_size, "%s    r%u, r%u", ppc_op_name(inst->op), inst->rA, inst->rB);
        }
        break;

    case PPC_OP_TLBIE:
        snprintf(buf, buf_size, "tlbie   r%u", inst->rB);
        break;

    case PPC_OP_ECIWX:
        snprintf(buf, buf_size, "eciwx   r%u, r%u, r%u", inst->rD, inst->rA, inst->rB);
        break;

    case PPC_OP_ECOWX:
        snprintf(buf, buf_size, "ecowx   r%u, r%u, r%u", inst->rS, inst->rA, inst->rB);
        break;

    case PPC_OP_MCRXR:
        snprintf(buf, buf_size, "mcrxr   cr%u", inst->crfD);
        break;

    case PPC_OP_MFMSR:
        snprintf(buf, buf_size, "mfmsr   r%u", inst->rD);
        break;

    case PPC_OP_MTMSR:
        snprintf(buf, buf_size, "mtmsr   r%u", inst->rS);
        break;

    case PPC_OP_MFSR:
        snprintf(buf, buf_size, "mfsr    r%u, %u", inst->rD, inst->sr);
        break;

    case PPC_OP_MFSRIN:
        snprintf(buf, buf_size, "mfsrin  r%u, r%u", inst->rD, inst->rB);
        break;

    case PPC_OP_MTSR:
        snprintf(buf, buf_size, "mtsr    %u, r%u", inst->sr, inst->rS);
        break;

    case PPC_OP_MTSRIN:
        snprintf(buf, buf_size, "mtsrin  r%u, r%u", inst->rS, inst->rB);
        break;

    case PPC_OP_B: {
        const char* mnemonic = "b";
        if (inst->lk && inst->aa)       mnemonic = "bla";
        else if (inst->lk)              mnemonic = "bl";
        else if (inst->aa)              mnemonic = "ba";

        snprintf(buf, buf_size, "%-7s 0x%08X", mnemonic, inst->branch_target);
        break;
    }

    case PPC_OP_BC: {
        const char* suffix = "";
        if (inst->lk && inst->aa)       suffix = "la";
        else if (inst->lk)              suffix = "l";
        else if (inst->aa)              suffix = "a";

        snprintf(buf, buf_size, "bc%s    %u, %u, 0x%08X",
                 suffix, inst->bo, inst->bi, inst->branch_target);
        break;
    }

    case PPC_OP_BCLR:
        if (inst->bo == 20 && inst->bi == 0) {
            snprintf(buf, buf_size, "%s", inst->lk ? "blrl" : "blr");
        } else {
            snprintf(buf, buf_size, "bclr%s  %u, %u",
                     inst->lk ? "l" : "", inst->bo, inst->bi);
        }
        break;

    case PPC_OP_BCCTR:
        if (inst->bo == 20 && inst->bi == 0) {
            snprintf(buf, buf_size, "%s", inst->lk ? "bctrl" : "bctr");
        } else {
            snprintf(buf, buf_size, "bcctr%s %u, %u",
                     inst->lk ? "l" : "", inst->bo, inst->bi);
        }
        break;

    case PPC_OP_CRAND:
    case PPC_OP_CRANDC:
    case PPC_OP_CREQV:
    case PPC_OP_CRNAND:
    case PPC_OP_CRNOR:
    case PPC_OP_CROR:
    case PPC_OP_CRORC:
    case PPC_OP_CRXOR:
        snprintf(buf, buf_size, "%-7s %u, %u, %u",
                 ppc_op_name(inst->op), inst->rD, inst->rA, inst->rB);
        break;

    case PPC_OP_MCRF:
        snprintf(buf, buf_size, "mcrf    cr%u, cr%u", inst->crfD, inst->crfS);
        break;

    case PPC_OP_MFCR:
        snprintf(buf, buf_size, "mfcr    r%u", inst->rD);
        break;

    case PPC_OP_MTCRF:
        if (inst->crm == 0xFF)
            snprintf(buf, buf_size, "mtcr    r%u", inst->rS);
        else
            snprintf(buf, buf_size, "mtcrf   0x%02X, r%u", inst->crm, inst->rS);
        break;

    case PPC_OP_MFSPR: {
        const char* name = spr_name(inst->spr);
        if (name)
            snprintf(buf, buf_size, "mf%s    r%u", name, inst->rD);
        else
            snprintf(buf, buf_size, "mfspr   r%u, %u", inst->rD, inst->spr);
        break;
    }

    case PPC_OP_MFTB:
        if (inst->spr == 268)
            snprintf(buf, buf_size, "mftb    r%u", inst->rD);
        else if (inst->spr == 269)
            snprintf(buf, buf_size, "mftbu   r%u", inst->rD);
        else
            snprintf(buf, buf_size, "mftb    r%u, %u", inst->rD, inst->spr);
        break;

    case PPC_OP_MTSPR: {
        const char* name = spr_name(inst->spr);
        if (name)
            snprintf(buf, buf_size, "mt%s    r%u", name, inst->rS);
        else
            snprintf(buf, buf_size, "mtspr   %u, r%u", inst->spr, inst->rS);
        break;
    }

    default:
        snprintf(buf, buf_size, ".long   0x%08X", inst->raw);
        break;
    }

    return buf;
}
