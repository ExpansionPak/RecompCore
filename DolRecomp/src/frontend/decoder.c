#include "decoder_private.h"
#include <stdio.h>

bool decode_primary_31(PPCInst* inst, u32 raw, u32 address) {
    if (decode_primary_31_system(inst, raw, address))
        return true;
    return decode_primary_31_integer(inst, raw, address);
}

PPCInst ppc_decode(u32 raw, u32 address) {
    PPCInst inst;
    memset(&inst, 0, sizeof(inst));
    inst.raw     = raw;
    inst.address = address;

    switch (PPC_PRIMARY(raw)) {
    case 3:
        inst.op = PPC_OP_TWI;
        inst.to = PPC_RD(raw);
        inst.rA = PPC_RA(raw);
        inst.simm = PPC_SIMM(raw);
        break;

    case 4:
        if (!decode_primary_4(&inst, raw, address))
            inst.op = PPC_OP_UNKNOWN;
        break;

    case 7: // mulli
        decode_d_rt_ra(&inst, PPC_OP_MULLI, raw);
        break;

    case 8: // subfic
        decode_d_rt_ra(&inst, PPC_OP_SUBFIC, raw);
        break;

    case 10: // cmpli/cmplwi
        if ((raw >> 21) & 1u) {
            inst.op = PPC_OP_UNKNOWN;
            break;
        }
        inst.op   = PPC_OP_CMPLI;
        inst.crfD = (raw >> 23) & 0x7;
        inst.l    = (raw >> 21) & 0x1;
        inst.rA   = PPC_RA(raw);
        inst.uimm = PPC_UIMM(raw);
        break;

    case 11: // cmpi/cmpwi
        if ((raw >> 21) & 1u) {
            inst.op = PPC_OP_UNKNOWN;
            break;
        }
        inst.op   = PPC_OP_CMPI;
        inst.crfD = (raw >> 23) & 0x7;
        inst.l    = (raw >> 21) & 0x1;
        inst.rA   = PPC_RA(raw);
        inst.simm = PPC_SIMM(raw);
        break;

    case 12: // addic
        decode_d_rt_ra(&inst, PPC_OP_ADDIC, raw);
        break;

    case 13: // addic.
        decode_d_rt_ra(&inst, PPC_OP_ADDIC_DOT, raw);
        inst.rc = true;
        break;

    case 14: // addi, with rA=0 using literal zero
        decode_d_rt_ra(&inst, PPC_OP_ADDI, raw);
        break;

    case 15: // addis, with rA=0 using literal zero
        decode_d_rt_ra(&inst, PPC_OP_ADDIS, raw);
        break;

    case 16: { // bc/bcl/bca/bcla
        inst.op = PPC_OP_BC;
        inst.bo = PPC_BO(raw);
        inst.bi = PPC_BI(raw);
        inst.aa = (raw >> 1) & 1;
        inst.lk = raw & 1;

        s32 displacement = sign_extend(raw & 0x0000FFFC, 16);
        inst.branch_target = inst.aa
            ? (u32)displacement
            : address + (u32)displacement;
        break;
    }

    case 17:
        inst.op = raw == 0x44000002u ? PPC_OP_SC : PPC_OP_UNKNOWN;
        break;

    case 18: { // b/bl/ba/bla
        inst.op = PPC_OP_B;
        inst.aa = (raw >> 1) & 1;
        inst.lk = raw & 1;

        s32 displacement = sign_extend(raw & 0x03FFFFFC, 26);
        inst.branch_target = inst.aa
            ? (u32)displacement
            : address + (u32)displacement;
        break;
    }

    case 19:
        if (!decode_primary_19(&inst, raw, address))
            inst.op = PPC_OP_UNKNOWN;
        break;

    case 20: // rlwimi
        inst.op = PPC_OP_RLWIMI;
        inst.rS = PPC_RS(raw);
        inst.rA = PPC_RA(raw);
        inst.sh = PPC_SH(raw);
        inst.mb = PPC_MB(raw);
        inst.me = PPC_ME(raw);
        inst.rc = PPC_RC(raw);
        break;

    case 21: // rlwinm
        inst.op = PPC_OP_RLWINM;
        inst.rS = PPC_RS(raw);
        inst.rA = PPC_RA(raw);
        inst.sh = PPC_SH(raw);
        inst.mb = PPC_MB(raw);
        inst.me = PPC_ME(raw);
        inst.rc = PPC_RC(raw);
        break;

    case 23: // rlwnm
        inst.op = PPC_OP_RLWNM;
        inst.rS = PPC_RS(raw);
        inst.rA = PPC_RA(raw);
        inst.rB = PPC_RB(raw);
        inst.mb = PPC_MB(raw);
        inst.me = PPC_ME(raw);
        inst.rc = PPC_RC(raw);
        break;

    case 24: // ori, with ori r0,r0,0 serving as nop
        inst.op   = PPC_OP_ORI;
        inst.rS   = PPC_RS(raw);
        inst.rA   = PPC_RA(raw);
        inst.uimm = PPC_UIMM(raw);
        break;

    case 25: // oris
        inst.op   = PPC_OP_ORIS;
        inst.rS   = PPC_RS(raw);
        inst.rA   = PPC_RA(raw);
        inst.uimm = PPC_UIMM(raw);
        break;

    case 26: // xori
        inst.op   = PPC_OP_XORI;
        inst.rS   = PPC_RS(raw);
        inst.rA   = PPC_RA(raw);
        inst.uimm = PPC_UIMM(raw);
        break;

    case 27: // xoris
        inst.op   = PPC_OP_XORIS;
        inst.rS   = PPC_RS(raw);
        inst.rA   = PPC_RA(raw);
        inst.uimm = PPC_UIMM(raw);
        break;

    case 28: // andi.
        inst.op   = PPC_OP_ANDI;
        inst.rS   = PPC_RS(raw);
        inst.rA   = PPC_RA(raw);
        inst.uimm = PPC_UIMM(raw);
        inst.rc   = true;
        break;

    case 29: // andis.
        inst.op   = PPC_OP_ANDIS;
        inst.rS   = PPC_RS(raw);
        inst.rA   = PPC_RA(raw);
        inst.uimm = PPC_UIMM(raw);
        inst.rc   = true;
        break;

    case 31:
        if (!decode_primary_31(&inst, raw, address))
            inst.op = PPC_OP_UNKNOWN;
        break;

    case 32: decode_d_rt_ra(&inst, PPC_OP_LWZ, raw); break;
    case 33: decode_d_rt_ra_update(&inst, PPC_OP_LWZU, raw); break;
    case 34: decode_d_rt_ra(&inst, PPC_OP_LBZ, raw); break;
    case 35: decode_d_rt_ra_update(&inst, PPC_OP_LBZU, raw); break;
    case 36: decode_d_rs_ra(&inst, PPC_OP_STW, raw); break;
    case 37: decode_d_rs_ra_update(&inst, PPC_OP_STWU, raw); break;
    case 38: decode_d_rs_ra(&inst, PPC_OP_STB, raw); break;
    case 39: decode_d_rs_ra_update(&inst, PPC_OP_STBU, raw); break;
    case 40: decode_d_rt_ra(&inst, PPC_OP_LHZ, raw); break;
    case 41: decode_d_rt_ra_update(&inst, PPC_OP_LHZU, raw); break;
    case 42: decode_d_rt_ra(&inst, PPC_OP_LHA, raw); break;
    case 43: decode_d_rt_ra_update(&inst, PPC_OP_LHAU, raw); break;
    case 44: decode_d_rs_ra(&inst, PPC_OP_STH, raw); break;
    case 45: decode_d_rs_ra_update(&inst, PPC_OP_STHU, raw); break;
    case 46: decode_d_rt_ra(&inst, PPC_OP_LMW, raw); break;
    case 47: decode_d_rs_ra(&inst, PPC_OP_STMW, raw); break;
    case 48: decode_d_rt_ra(&inst, PPC_OP_LFS, raw); break;
    case 49: decode_d_frt_ra_update(&inst, PPC_OP_LFSU, raw); break;
    case 50: decode_d_rt_ra(&inst, PPC_OP_LFD, raw); break;
    case 51: decode_d_frt_ra_update(&inst, PPC_OP_LFDU, raw); break;
    case 52: decode_d_rs_ra(&inst, PPC_OP_STFS, raw); break;
    case 53: decode_d_rs_ra_update(&inst, PPC_OP_STFSU, raw); break;
    case 54: decode_d_rs_ra(&inst, PPC_OP_STFD, raw); break;
    case 55: decode_d_rs_ra_update(&inst, PPC_OP_STFDU, raw); break;
    case 56: decode_psq_d_rt_ra(&inst, PPC_OP_PSQ_L, raw); break;
    case 57: decode_psq_d_rt_ra_update(&inst, PPC_OP_PSQ_LU, raw); break;
    case 60: decode_psq_d_rs_ra(&inst, PPC_OP_PSQ_ST, raw); break;
    case 61: decode_psq_d_rs_ra_update(&inst, PPC_OP_PSQ_STU, raw); break;

    case 59:
        if (!decode_primary_59(&inst, raw, address))
            inst.op = PPC_OP_UNKNOWN;
        break;

    case 63:
        if (!decode_primary_63(&inst, raw, address))
            inst.op = PPC_OP_UNKNOWN;
        break;

    default:
        inst.op = PPC_OP_UNKNOWN;
        break;
    }

    return inst;
}

static const char* opcode_names[PPC_OP_COUNT] = {
    [PPC_OP_UNKNOWN] = "???",
    [PPC_OP_MULLI]   = "mulli",
    [PPC_OP_SUBFIC]  = "subfic",
    [PPC_OP_ADDI]    = "addi",
    [PPC_OP_ADDIC]   = "addic",
    [PPC_OP_ADDIC_DOT] = "addic.",
    [PPC_OP_ADDIS]   = "addis",
    [PPC_OP_CMPI]    = "cmpi",
    [PPC_OP_CMPLI]   = "cmpli",
    [PPC_OP_TWI]     = "twi",
    [PPC_OP_ORI]     = "ori",
    [PPC_OP_ORIS]    = "oris",
    [PPC_OP_XORI]    = "xori",
    [PPC_OP_XORIS]   = "xoris",
    [PPC_OP_ANDI]    = "andi.",
    [PPC_OP_ANDIS]   = "andis.",
    [PPC_OP_LWZ]     = "lwz",
    [PPC_OP_LWZU]    = "lwzu",
    [PPC_OP_LBZ]     = "lbz",
    [PPC_OP_LBZU]    = "lbzu",
    [PPC_OP_STW]     = "stw",
    [PPC_OP_STWU]    = "stwu",
    [PPC_OP_STB]     = "stb",
    [PPC_OP_STBU]    = "stbu",
    [PPC_OP_LHZ]     = "lhz",
    [PPC_OP_LHZU]    = "lhzu",
    [PPC_OP_LHA]     = "lha",
    [PPC_OP_LHAU]    = "lhau",
    [PPC_OP_STH]     = "sth",
    [PPC_OP_STHU]    = "sthu",
    [PPC_OP_LMW]     = "lmw",
    [PPC_OP_STMW]    = "stmw",
    [PPC_OP_B]       = "b",
    [PPC_OP_BC]      = "bc",
    [PPC_OP_BCLR]    = "bclr",
    [PPC_OP_BCCTR]   = "bcctr",
    [PPC_OP_SC]      = "sc",
    [PPC_OP_RFI]     = "rfi",
    [PPC_OP_CRAND]   = "crand",
    [PPC_OP_CRANDC]  = "crandc",
    [PPC_OP_CREQV]   = "creqv",
    [PPC_OP_CRNAND]  = "crnand",
    [PPC_OP_CRNOR]   = "crnor",
    [PPC_OP_CROR]    = "cror",
    [PPC_OP_CRORC]   = "crorc",
    [PPC_OP_CRXOR]   = "crxor",
    [PPC_OP_MCRF]    = "mcrf",
    [PPC_OP_MFCR]    = "mfcr",
    [PPC_OP_MTCRF]   = "mtcrf",
    [PPC_OP_MFSPR]   = "mfspr",
    [PPC_OP_MTSPR]   = "mtspr",
    [PPC_OP_MFTB]    = "mftb",
    [PPC_OP_CMP]     = "cmp",
    [PPC_OP_CMPL]    = "cmpl",
    [PPC_OP_TW]      = "tw",
    [PPC_OP_ADD]     = "add",
    [PPC_OP_ADDO]    = "addo",
    [PPC_OP_ADDC]    = "addc",
    [PPC_OP_ADDCO]   = "addco",
    [PPC_OP_ADDE]    = "adde",
    [PPC_OP_ADDEO]   = "addeo",
    [PPC_OP_ADDME]   = "addme",
    [PPC_OP_ADDMEO]  = "addmeo",
    [PPC_OP_ADDZE]   = "addze",
    [PPC_OP_ADDZEO]  = "addzeo",
    [PPC_OP_SUBF]    = "subf",
    [PPC_OP_SUBFO]   = "subfo",
    [PPC_OP_SUBFC]   = "subfc",
    [PPC_OP_SUBFCO]  = "subfco",
    [PPC_OP_SUBFE]   = "subfe",
    [PPC_OP_SUBFEO]  = "subfeo",
    [PPC_OP_SUBFME]  = "subfme",
    [PPC_OP_SUBFMEO] = "subfmeo",
    [PPC_OP_SUBFZE]  = "subfze",
    [PPC_OP_SUBFZEO] = "subfzeo",
    [PPC_OP_NEG]     = "neg",
    [PPC_OP_NEGO]    = "nego",
    [PPC_OP_AND]     = "and",
    [PPC_OP_ANDC]    = "andc",
    [PPC_OP_OR]      = "or",
    [PPC_OP_ORC]     = "orc",
    [PPC_OP_XOR]     = "xor",
    [PPC_OP_NAND]    = "nand",
    [PPC_OP_NOR]     = "nor",
    [PPC_OP_EQV]     = "eqv",
    [PPC_OP_CNTLZW]  = "cntlzw",
    [PPC_OP_EXTSB]   = "extsb",
    [PPC_OP_EXTSH]   = "extsh",
    [PPC_OP_SLW]     = "slw",
    [PPC_OP_SRW]     = "srw",
    [PPC_OP_SRAW]    = "sraw",
    [PPC_OP_SRAWI]   = "srawi",
    [PPC_OP_RLWINM]  = "rlwinm",
    [PPC_OP_RLWNM]   = "rlwnm",
    [PPC_OP_RLWIMI]  = "rlwimi",
    [PPC_OP_LWZX]    = "lwzx",
    [PPC_OP_LWZUX]   = "lwzux",
    [PPC_OP_LBZX]    = "lbzx",
    [PPC_OP_LBZUX]   = "lbzux",
    [PPC_OP_LHZX]    = "lhzx",
    [PPC_OP_LHZUX]   = "lhzux",
    [PPC_OP_LHAX]    = "lhax",
    [PPC_OP_LHAUX]   = "lhaux",
    [PPC_OP_LWBRX]   = "lwbrx",
    [PPC_OP_LHBRX]   = "lhbrx",
    [PPC_OP_STWX]    = "stwx",
    [PPC_OP_STWUX]   = "stwux",
    [PPC_OP_STBX]    = "stbx",
    [PPC_OP_STBUX]   = "stbux",
    [PPC_OP_STHX]    = "sthx",
    [PPC_OP_STHUX]   = "sthux",
    [PPC_OP_STWBRX]  = "stwbrx",
    [PPC_OP_STHBRX]  = "sthbrx",
    [PPC_OP_LSWI]    = "lswi",
    [PPC_OP_LSWX]    = "lswx",
    [PPC_OP_STSWI]   = "stswi",
    [PPC_OP_STSWX]   = "stswx",
    [PPC_OP_LWARX]   = "lwarx",
    [PPC_OP_STWCX]   = "stwcx.",
    [PPC_OP_STFIWX]  = "stfiwx",
    [PPC_OP_LFS]     = "lfs",
    [PPC_OP_LFSU]    = "lfsu",
    [PPC_OP_LFD]     = "lfd",
    [PPC_OP_LFDU]    = "lfdu",
    [PPC_OP_STFS]    = "stfs",
    [PPC_OP_STFSU]   = "stfsux", // wait, STFSU or STFSUX? it's stfsu
    [PPC_OP_STFD]    = "stfd",
    [PPC_OP_STFDU]   = "stfdu",
    [PPC_OP_LFSX]    = "lfsx",
    [PPC_OP_LFSUX]   = "lfsux",
    [PPC_OP_LFDX]    = "lfdx",
    [PPC_OP_LFDUX]   = "lfdux",
    [PPC_OP_STFSX]   = "stfsx",
    [PPC_OP_STFSUX]  = "stfsux",
    [PPC_OP_STFDX]   = "stfdx",
    [PPC_OP_STFDUX]  = "stfdux",
    [PPC_OP_FADDS]   = "fadds",
    [PPC_OP_FSUBS]   = "fsubs",
    [PPC_OP_FMULS]   = "fmuls",
    [PPC_OP_FDIVS]   = "fdivs",
    [PPC_OP_FRES]    = "fres",
    [PPC_OP_FMADDS]  = "fmadds",
    [PPC_OP_FMSUBS]  = "fmsubs",
    [PPC_OP_FNMADDS] = "fnmadds",
    [PPC_OP_FNMSUBS] = "fnmsubs",
    [PPC_OP_FADD]    = "fadd",
    [PPC_OP_FSUB]    = "fsub",
    [PPC_OP_FMUL]    = "fmul",
    [PPC_OP_FDIV]    = "fdiv",
    [PPC_OP_FRSQRTE] = "frsqrte",
    [PPC_OP_FMADD]   = "fmadd",
    [PPC_OP_FMSUB]   = "fmsub",
    [PPC_OP_FNMADD]  = "fnmadd",
    [PPC_OP_FNMSUB]  = "fnmsub",
    [PPC_OP_FCTIW]   = "fctiw",
    [PPC_OP_FCTIWZ]  = "fctiwz",
    [PPC_OP_FMR]     = "fmr",
    [PPC_OP_FNEG]    = "fneg",
    [PPC_OP_FABS]    = "fabs",
    [PPC_OP_FNABS]   = "fnabs",
    [PPC_OP_FRSP]    = "frsp",
    [PPC_OP_FSEL]    = "fsel",
    [PPC_OP_FCMPU]   = "fcmpu",
    [PPC_OP_FCMPO]   = "fcmpo",
    [PPC_OP_MTFSB0]  = "mtfsb0",
    [PPC_OP_MTFSB1]  = "mtfsb1",
    [PPC_OP_MCRFS]   = "mcrfs",
    [PPC_OP_MFFS]    = "mffs",
    [PPC_OP_MTFSF]   = "mtfsf",
    [PPC_OP_MTFSFI]  = "mtfsfi",
    [PPC_OP_PSQ_L]   = "psq_l",
    [PPC_OP_PSQ_LU]  = "psq_lu",
    [PPC_OP_PSQ_ST]  = "psq_st",
    [PPC_OP_PSQ_STU] = "psq_stu",
    [PPC_OP_PSQ_LX]  = "psq_lx",
    [PPC_OP_PSQ_LUX] = "psq_lux",
    [PPC_OP_PSQ_STX] = "psq_stx",
    [PPC_OP_PSQ_STUX] = "psq_stux",
    [PPC_OP_PS_ADD]  = "ps_add",
    [PPC_OP_PS_SUB]  = "ps_sub",
    [PPC_OP_PS_MUL]  = "ps_mul",
    [PPC_OP_PS_DIV]  = "ps_div",
    [PPC_OP_PS_RES]  = "ps_res",
    [PPC_OP_PS_RSQRTE] = "ps_rsqrte",
    [PPC_OP_PS_MADD] = "ps_madd",
    [PPC_OP_PS_MSUB] = "ps_msub",
    [PPC_OP_PS_NMADD] = "ps_nmadd",
    [PPC_OP_PS_NMSUB] = "ps_nmsub",
    [PPC_OP_PS_NEG]  = "ps_neg",
    [PPC_OP_PS_ABS]  = "ps_abs",
    [PPC_OP_PS_NABS] = "ps_nabs",
    [PPC_OP_PS_MR]   = "ps_mr",
    [PPC_OP_PS_SUM0] = "ps_sum0",
    [PPC_OP_PS_SUM1] = "ps_sum1",
    [PPC_OP_PS_MULS0] = "ps_muls0",
    [PPC_OP_PS_MULS1] = "ps_muls1",
    [PPC_OP_PS_MADDS0] = "ps_madds0",
    [PPC_OP_PS_MADDS1] = "ps_madds1",
    [PPC_OP_PS_MERGE00] = "ps_merge00",
    [PPC_OP_PS_MERGE01] = "ps_merge01",
    [PPC_OP_PS_MERGE10] = "ps_merge10",
    [PPC_OP_PS_MERGE11] = "ps_merge11",
    [PPC_OP_PS_CMPU0] = "ps_cmpu0",
    [PPC_OP_PS_CMPO0] = "ps_cmpo0",
    [PPC_OP_PS_CMPU1] = "ps_cmpu1",
    [PPC_OP_PS_CMPO1] = "ps_cmpo1",
    [PPC_OP_PS_SEL]  = "ps_sel",
    [PPC_OP_MULLW]   = "mullw",
    [PPC_OP_MULLWO]  = "mullwo",
    [PPC_OP_MULHW]   = "mulhw",
    [PPC_OP_MULHWU]  = "mulhwu",
    [PPC_OP_DIVW]    = "divw",
    [PPC_OP_DIVWO]   = "divwo",
    [PPC_OP_DIVWU]   = "divwu",
    [PPC_OP_DIVWUO]  = "divwuo",
    [PPC_OP_DCBZ]    = "dcbz",
    [PPC_OP_DCBZ_L]  = "dcbz_l",
    [PPC_OP_DCBST]   = "dcbst",
    [PPC_OP_DCBF]    = "dcbf",
    [PPC_OP_DCBTST]  = "dcbtst",
    [PPC_OP_DCBT]    = "dcbt",
    [PPC_OP_DCBI]    = "dcbi",
    [PPC_OP_ICBI]    = "icbi",
    [PPC_OP_SYNC]    = "sync",
    [PPC_OP_EIEIO]   = "eieio",
    [PPC_OP_ISYNC]   = "isync",
    [PPC_OP_MCRXR]   = "mcrxr",
    [PPC_OP_MFMSR]   = "mfmsr",
    [PPC_OP_MTMSR]   = "mtmsr",
    [PPC_OP_MFSR]    = "mfsr",
    [PPC_OP_MFSRIN]  = "mfsrin",
    [PPC_OP_MTSR]    = "mtsr",
    [PPC_OP_MTSRIN]  = "mtsrin",
    [PPC_OP_TLBIE]   = "tlbie",
    [PPC_OP_TLBSYNC] = "tlbsync",
    [PPC_OP_ECIWX]   = "eciwx",
    [PPC_OP_ECOWX]   = "ecowx",
};

const char* ppc_op_name(PPCOpcode op) {
    if (op >= PPC_OP_COUNT) return "???";
    return opcode_names[op];
}
