// SPDX-License-Identifier: GPL-3.0-or-later
#include "emitter_private.h"
#include <stdio.h>

bool emit_float_instruction(FILE* out, const PPCInst* inst, u32 func_start, u32 func_end) {
    (void)func_start;
    (void)func_end;

    if (!ppc_op_uses_fpu(inst->op)) {
        return false;
    }

    switch (inst->op) {
    case PPC_OP_LFS:   emit_fload(out, inst, true,  false); break;
    case PPC_OP_LFSU:  emit_fload(out, inst, true,  true); break;
    case PPC_OP_LFD:   emit_fload(out, inst, false, false); break;
    case PPC_OP_LFDU:  emit_fload(out, inst, false, true); break;

    case PPC_OP_LFSX:  emit_floadx(out, inst, true,  false); break;
    case PPC_OP_LFSUX: emit_floadx(out, inst, true,  true); break;
    case PPC_OP_LFDX:  emit_floadx(out, inst, false, false); break;
    case PPC_OP_LFDUX: emit_floadx(out, inst, false, true); break;

    case PPC_OP_PSQ_L:   emit_psq_load(out, inst, false, false); break;
    case PPC_OP_PSQ_LU:  emit_psq_load(out, inst, false, true); break;
    case PPC_OP_PSQ_LX:  emit_psq_load(out, inst, true,  false); break;
    case PPC_OP_PSQ_LUX: emit_psq_load(out, inst, true,  true); break;

    case PPC_OP_STFS:   emit_fstore(out, inst, true,  false); break;
    case PPC_OP_STFSU:  emit_fstore(out, inst, true,  true); break;
    case PPC_OP_STFD:   emit_fstore(out, inst, false, false); break;
    case PPC_OP_STFDU:  emit_fstore(out, inst, false, true); break;

    case PPC_OP_STFSX:  emit_fstorex(out, inst, true,  false); break;
    case PPC_OP_STFSUX: emit_fstorex(out, inst, true,  true); break;
    case PPC_OP_STFDX:  emit_fstorex(out, inst, false, false); break;
    case PPC_OP_STFDUX: emit_fstorex(out, inst, false, true); break;

    case PPC_OP_PSQ_ST:   emit_psq_store(out, inst, false, false); break;
    case PPC_OP_PSQ_STU:  emit_psq_store(out, inst, false, true); break;
    case PPC_OP_PSQ_STX:  emit_psq_store(out, inst, true,  false); break;
    case PPC_OP_PSQ_STUX: emit_psq_store(out, inst, true,  true); break;

    case PPC_OP_STFIWX:
        fprintf(out, "    {\n        u32 ea = ");
        emit_xform_ea(out, inst->rA, inst->rB, false);
        fprintf(out, ";\n        mem_write32(ctx, ea, (u32)dolrecomp_f64_to_bits(ctx->fpr[%u]));\n    }\n", inst->rS);
        break;

    case PPC_OP_FADDS:
        fprintf(out, "    ppc_fadds(ctx, %uu, %uu, %uu);\n", inst->rD, inst->rA, inst->rB);
        if (inst->rc) emit_set_cr1_from_fpscr(out);
        break;

    case PPC_OP_FSUBS:
        fprintf(out, "    ppc_fsubs(ctx, %uu, %uu, %uu);\n", inst->rD, inst->rA, inst->rB);
        if (inst->rc) emit_set_cr1_from_fpscr(out);
        break;

    case PPC_OP_FMULS:
        fprintf(out, "    ppc_fmuls(ctx, %uu, %uu, %uu);\n", inst->rD, inst->rA, inst->rC);
        if (inst->rc) emit_set_cr1_from_fpscr(out);
        break;

    case PPC_OP_FDIVS:
        fprintf(out, "    ppc_fdivs(ctx, %uu, %uu, %uu);\n", inst->rD, inst->rA, inst->rB);
        if (inst->rc) emit_set_cr1_from_fpscr(out);
        break;

    case PPC_OP_FRES:
        fprintf(out, "    ppc_fres_op(ctx, %uu, %uu);\n", inst->rD, inst->rB);
        if (inst->rc) emit_set_cr1_from_fpscr(out);
        break;

    case PPC_OP_FMADDS:
    case PPC_OP_FMSUBS:
    case PPC_OP_FNMADDS:
    case PPC_OP_FNMSUBS: {
        const bool sub = inst->op == PPC_OP_FMSUBS || inst->op == PPC_OP_FNMSUBS;
        const bool neg = inst->op == PPC_OP_FNMADDS || inst->op == PPC_OP_FNMSUBS;
        fprintf(out, "    ppc_fmadd_op(ctx, %uu, %uu, %uu, %uu, true, %s, %s);\n",
                inst->rD, inst->rA, inst->rC, inst->rB,
                sub ? "true" : "false", neg ? "true" : "false");
        if (inst->rc) emit_set_cr1_from_fpscr(out);
        break;
    }

    case PPC_OP_FADD:
        fprintf(out, "    ppc_fadd(ctx, %uu, %uu, %uu);\n", inst->rD, inst->rA, inst->rB);
        if (inst->rc) emit_set_cr1_from_fpscr(out);
        break;

    case PPC_OP_FSUB:
        fprintf(out, "    ppc_fsub(ctx, %uu, %uu, %uu);\n", inst->rD, inst->rA, inst->rB);
        if (inst->rc) emit_set_cr1_from_fpscr(out);
        break;

    case PPC_OP_FMUL:
        fprintf(out, "    ppc_fmul(ctx, %uu, %uu, %uu);\n", inst->rD, inst->rA, inst->rC);
        if (inst->rc) emit_set_cr1_from_fpscr(out);
        break;

    case PPC_OP_FDIV:
        fprintf(out, "    ppc_fdiv(ctx, %uu, %uu, %uu);\n", inst->rD, inst->rA, inst->rB);
        if (inst->rc) emit_set_cr1_from_fpscr(out);
        break;

    case PPC_OP_FRSQRTE:
        fprintf(out, "    ppc_frsqrte_op(ctx, %uu, %uu);\n", inst->rD, inst->rB);
        if (inst->rc) emit_set_cr1_from_fpscr(out);
        break;

    case PPC_OP_FMADD:
    case PPC_OP_FMSUB:
    case PPC_OP_FNMADD:
    case PPC_OP_FNMSUB: {
        const bool sub = inst->op == PPC_OP_FMSUB || inst->op == PPC_OP_FNMSUB;
        const bool neg = inst->op == PPC_OP_FNMADD || inst->op == PPC_OP_FNMSUB;
        fprintf(out, "    ppc_fmadd_op(ctx, %uu, %uu, %uu, %uu, false, %s, %s);\n",
                inst->rD, inst->rA, inst->rC, inst->rB,
                sub ? "true" : "false", neg ? "true" : "false");
        if (inst->rc) emit_set_cr1_from_fpscr(out);
        break;
    }

    case PPC_OP_FCTIW:
    case PPC_OP_FCTIWZ:
        fprintf(out, "    ppc_fctiw_op(ctx, %uu, %uu, %s);\n",
                inst->rD, inst->rB, inst->op == PPC_OP_FCTIWZ ? "true" : "false");
        if (inst->rc) emit_set_cr1_from_fpscr(out);
        break;

    case PPC_OP_FMR:
        /* Binary move: PS0 only, FPSCR untouched (Dolphin fmrx). */
        fprintf(out, "    ctx->fpr[%u] = ctx->fpr[%u];\n", inst->rD, inst->rB);
        if (inst->rc) emit_set_cr1_from_fpscr(out);
        break;

    case PPC_OP_FNEG:
        fprintf(out, "    ctx->fpr[%u] = dolrecomp_f64_from_bits(dolrecomp_f64_to_bits(ctx->fpr[%u]) ^ 0x8000000000000000ull);\n",
                inst->rD, inst->rB);
        if (inst->rc) emit_set_cr1_from_fpscr(out);
        break;

    case PPC_OP_FABS:
        fprintf(out, "    ctx->fpr[%u] = dolrecomp_f64_from_bits(dolrecomp_f64_to_bits(ctx->fpr[%u]) & 0x7FFFFFFFFFFFFFFFull);\n",
                inst->rD, inst->rB);
        if (inst->rc) emit_set_cr1_from_fpscr(out);
        break;

    case PPC_OP_FNABS:
        fprintf(out, "    ctx->fpr[%u] = dolrecomp_f64_from_bits(dolrecomp_f64_to_bits(ctx->fpr[%u]) | 0x8000000000000000ull);\n",
                inst->rD, inst->rB);
        if (inst->rc) emit_set_cr1_from_fpscr(out);
        break;

    case PPC_OP_FRSP:
        fprintf(out, "    ppc_frsp(ctx, %uu, %uu);\n", inst->rD, inst->rB);
        if (inst->rc) emit_set_cr1_from_fpscr(out);
        break;

    case PPC_OP_FSEL:
        fprintf(out, "    {\n");
        fprintf(out, "        ctx->fpr[%u] = (ctx->fpr[%u] >= -0.0) ? ctx->fpr[%u] : ctx->fpr[%u];\n",
                inst->rD, inst->rA, inst->rC, inst->rB);
        if (inst->rc) {
            emit_set_cr1_from_fpscr(out);
        }
        fprintf(out, "    }\n");
        break;

    case PPC_OP_MTFSB0:
    case PPC_OP_MTFSB1:
        /* Dolphin mtfsb0x/mtfsb1x: raw bit clear / FX-raising bit set, then
         * FPSCRUpdated (summary recompute + host rounding re-arm). */
        fprintf(out, "    ppc_mtfsb%c_op(ctx, %uu);\n",
                inst->op == PPC_OP_MTFSB0 ? '0' : '1', inst->rD);
        if (inst->rc) {
            emit_set_cr1_from_fpscr(out);
        }
        break;

    case PPC_OP_MFFS:
        fprintf(out, "    ctx->fpr[%u] = dolrecomp_f64_from_bits(0xFFF8000000000000ull | ctx->fpscr);\n", inst->rD);
        if (inst->rc) emit_set_cr1_from_fpscr(out);
        break;

    case PPC_OP_MCRFS: {
        u32 shift = cr_field_shift(inst->crfS);
        u32 dst_shift = cr_field_shift(inst->crfD);
        fprintf(out, "    {\n");
        fprintf(out, "        u32 field = (ctx->fpscr >> %u) & 0xFu;\n", shift);
        /* Clear any read exception bits: FX | ANY_X (Dolphin mcrfs). */
        fprintf(out, "        ctx->fpscr &= ~((0xFu << %u) & 0x9FF80700u);\n", shift);
        fprintf(out, "        ppc_fpscr_control_updated(ctx);\n");
        fprintf(out, "        ctx->cr = (ctx->cr & ~(0xFu << %u)) | (field << %u);\n", dst_shift, dst_shift);
        fprintf(out, "    }\n");
        break;
    }

    case PPC_OP_MTFSFI: {
        u32 shift = cr_field_shift(inst->crfD);
        fprintf(out, "    ctx->fpscr = (ctx->fpscr & ~(0xFu << %u)) | (0x%Xu << %u);\n",
                shift, inst->imm, shift);
        fprintf(out, "    ppc_fpscr_control_updated(ctx);\n");
        if (inst->rc) emit_set_cr1_from_fpscr(out);
        break;
    }

    case PPC_OP_MTFSF:
        fprintf(out, "    {\n");
        fprintf(out, "        u32 mask = 0;\n");
        fprintf(out, "        for (u32 i = 0; i < 8; i++) if (0x%02Xu & (1u << i)) mask |= 0xFu << (i * 4);\n", inst->fm);
        fprintf(out, "        u32 source = (u32)dolrecomp_f64_to_bits(ctx->fpr[%u]);\n", inst->rB);
        fprintf(out, "        ctx->fpscr = (ctx->fpscr & ~mask) | (source & mask);\n");
        fprintf(out, "        ppc_fpscr_control_updated(ctx);\n");
        if (inst->rc) emit_set_cr1_from_fpscr(out);
        fprintf(out, "    }\n");
        break;

    case PPC_OP_PS_ADD:
        fprintf(out, "    ppc_ps_add_op(ctx, %uu, %uu, %uu);\n", inst->rD, inst->rA, inst->rB);
        if (inst->rc) emit_set_cr1_from_fpscr(out);
        break;

    case PPC_OP_PS_SUB:
        fprintf(out, "    ppc_ps_sub_op(ctx, %uu, %uu, %uu);\n", inst->rD, inst->rA, inst->rB);
        if (inst->rc) emit_set_cr1_from_fpscr(out);
        break;

    case PPC_OP_PS_MUL:
        fprintf(out, "    ppc_ps_mul_op(ctx, %uu, %uu, %uu);\n", inst->rD, inst->rA, inst->rC);
        if (inst->rc) emit_set_cr1_from_fpscr(out);
        break;

    case PPC_OP_PS_DIV:
        fprintf(out, "    ppc_ps_div_op(ctx, %uu, %uu, %uu);\n", inst->rD, inst->rA, inst->rB);
        if (inst->rc) emit_set_cr1_from_fpscr(out);
        break;

    case PPC_OP_PS_RES:
        fprintf(out, "    ppc_ps_res_op(ctx, %uu, %uu);\n", inst->rD, inst->rB);
        if (inst->rc) emit_set_cr1_from_fpscr(out);
        break;

    case PPC_OP_PS_RSQRTE:
        fprintf(out, "    ppc_ps_rsqrte_op(ctx, %uu, %uu);\n", inst->rD, inst->rB);
        if (inst->rc) emit_set_cr1_from_fpscr(out);
        break;

    case PPC_OP_PS_MADD:
    case PPC_OP_PS_MSUB:
    case PPC_OP_PS_NMADD:
    case PPC_OP_PS_NMSUB: {
        const bool sub = inst->op == PPC_OP_PS_MSUB || inst->op == PPC_OP_PS_NMSUB;
        const bool neg = inst->op == PPC_OP_PS_NMADD || inst->op == PPC_OP_PS_NMSUB;
        fprintf(out, "    ppc_ps_madd_op(ctx, %uu, %uu, %uu, %uu, %s, %s);\n",
                inst->rD, inst->rA, inst->rC, inst->rB,
                sub ? "true" : "false", neg ? "true" : "false");
        if (inst->rc) emit_set_cr1_from_fpscr(out);
        break;
    }

    case PPC_OP_PS_NEG:
        fprintf(out, "    ctx->fpr[%u] = dolrecomp_f64_from_bits(dolrecomp_f64_to_bits(ctx->fpr[%u]) ^ 0x8000000000000000ull);\n",
                inst->rD, inst->rB);
        fprintf(out, "    ctx->ps1[%u] = dolrecomp_f64_from_bits(dolrecomp_f64_to_bits(ctx->ps1[%u]) ^ 0x8000000000000000ull);\n",
                inst->rD, inst->rB);
        if (inst->rc) emit_set_cr1_from_fpscr(out);
        break;

    case PPC_OP_PS_ABS:
        fprintf(out, "    ctx->fpr[%u] = dolrecomp_f64_from_bits(dolrecomp_f64_to_bits(ctx->fpr[%u]) & 0x7FFFFFFFFFFFFFFFull);\n",
                inst->rD, inst->rB);
        fprintf(out, "    ctx->ps1[%u] = dolrecomp_f64_from_bits(dolrecomp_f64_to_bits(ctx->ps1[%u]) & 0x7FFFFFFFFFFFFFFFull);\n",
                inst->rD, inst->rB);
        if (inst->rc) emit_set_cr1_from_fpscr(out);
        break;

    case PPC_OP_PS_NABS:
        fprintf(out, "    ctx->fpr[%u] = dolrecomp_f64_from_bits(dolrecomp_f64_to_bits(ctx->fpr[%u]) | 0x8000000000000000ull);\n",
                inst->rD, inst->rB);
        fprintf(out, "    ctx->ps1[%u] = dolrecomp_f64_from_bits(dolrecomp_f64_to_bits(ctx->ps1[%u]) | 0x8000000000000000ull);\n",
                inst->rD, inst->rB);
        if (inst->rc) emit_set_cr1_from_fpscr(out);
        break;

    case PPC_OP_PS_MR:
        fprintf(out, "    ctx->fpr[%u] = ctx->fpr[%u];\n", inst->rD, inst->rB);
        fprintf(out, "    ctx->ps1[%u] = ctx->ps1[%u];\n", inst->rD, inst->rB);
        if (inst->rc) emit_set_cr1_from_fpscr(out);
        break;

    case PPC_OP_PS_SUM0:
        fprintf(out, "    ppc_ps_sum0(ctx, %uu, %uu, %uu, %uu);\n",
                inst->rD, inst->rA, inst->rC, inst->rB);
        if (inst->rc) emit_set_cr1_from_fpscr(out);
        break;

    case PPC_OP_PS_SUM1:
        fprintf(out, "    ppc_ps_sum1(ctx, %uu, %uu, %uu, %uu);\n",
                inst->rD, inst->rA, inst->rC, inst->rB);
        if (inst->rc) emit_set_cr1_from_fpscr(out);
        break;

    case PPC_OP_PS_MULS0:
        fprintf(out, "    ppc_ps_muls0(ctx, %uu, %uu, %uu);\n", inst->rD, inst->rA, inst->rC);
        if (inst->rc) emit_set_cr1_from_fpscr(out);
        break;

    case PPC_OP_PS_MULS1:
        fprintf(out, "    ppc_ps_muls1(ctx, %uu, %uu, %uu);\n", inst->rD, inst->rA, inst->rC);
        if (inst->rc) emit_set_cr1_from_fpscr(out);
        break;

    case PPC_OP_PS_MADDS0:
        fprintf(out, "    ppc_ps_madds0(ctx, %uu, %uu, %uu, %uu);\n",
                inst->rD, inst->rA, inst->rC, inst->rB);
        if (inst->rc) emit_set_cr1_from_fpscr(out);
        break;

    case PPC_OP_PS_MADDS1:
        fprintf(out, "    ppc_ps_madds1(ctx, %uu, %uu, %uu, %uu);\n",
                inst->rD, inst->rA, inst->rC, inst->rB);
        if (inst->rc) emit_set_cr1_from_fpscr(out);
        break;

    case PPC_OP_PS_MERGE00:
        fprintf(out, "    { f64 t0 = ctx->fpr[%u]; f64 t1 = ctx->fpr[%u]; ctx->fpr[%u] = t0; ctx->ps1[%u] = t1; }\n",
                inst->rA, inst->rB, inst->rD, inst->rD);
        if (inst->rc) emit_set_cr1_from_fpscr(out);
        break;

    case PPC_OP_PS_MERGE01:
        fprintf(out, "    { f64 t0 = ctx->fpr[%u]; f64 t1 = ctx->ps1[%u]; ctx->fpr[%u] = t0; ctx->ps1[%u] = t1; }\n",
                inst->rA, inst->rB, inst->rD, inst->rD);
        if (inst->rc) emit_set_cr1_from_fpscr(out);
        break;

    case PPC_OP_PS_MERGE10:
        fprintf(out, "    { f64 t0 = ctx->ps1[%u]; f64 t1 = ctx->fpr[%u]; ctx->fpr[%u] = t0; ctx->ps1[%u] = t1; }\n",
                inst->rA, inst->rB, inst->rD, inst->rD);
        if (inst->rc) emit_set_cr1_from_fpscr(out);
        break;

    case PPC_OP_PS_MERGE11:
        fprintf(out, "    { f64 t0 = ctx->ps1[%u]; f64 t1 = ctx->ps1[%u]; ctx->fpr[%u] = t0; ctx->ps1[%u] = t1; }\n",
                inst->rA, inst->rB, inst->rD, inst->rD);
        if (inst->rc) emit_set_cr1_from_fpscr(out);
        break;

    case PPC_OP_PS_CMPU0:
    case PPC_OP_PS_CMPO0:
    case PPC_OP_PS_CMPU1:
    case PPC_OP_PS_CMPO1: {
        const bool lane1 =
            inst->op == PPC_OP_PS_CMPU1 || inst->op == PPC_OP_PS_CMPO1;
        const bool ordered =
            inst->op == PPC_OP_PS_CMPO0 || inst->op == PPC_OP_PS_CMPO1;
        fprintf(out, "    ppc_fcmp(ctx, %uu, ctx->%s[%u], ctx->%s[%u], %s);\n",
                inst->crfD, lane1 ? "ps1" : "fpr", inst->rA,
                lane1 ? "ps1" : "fpr", inst->rB, ordered ? "true" : "false");
        break;
    }

    case PPC_OP_PS_SEL:
        fprintf(out, "    {\n");
        fprintf(out, "        f64 t0 = (ctx->fpr[%u] >= -0.0) ? ctx->fpr[%u] : ctx->fpr[%u];\n",
                inst->rA, inst->rC, inst->rB);
        fprintf(out, "        f64 t1 = (ctx->ps1[%u] >= -0.0) ? ctx->ps1[%u] : ctx->ps1[%u];\n",
                inst->rA, inst->rC, inst->rB);
        fprintf(out, "        ctx->fpr[%u] = t0;\n", inst->rD);
        fprintf(out, "        ctx->ps1[%u] = t1;\n", inst->rD);
        if (inst->rc) emit_set_cr1_from_fpscr(out);
        fprintf(out, "    }\n");
        break;

    case PPC_OP_FCMPU:
    case PPC_OP_FCMPO:
        emit_fcompare(out, inst);
        break;

    default:
        return false;
    }

    return true;
}
