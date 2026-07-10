// SPDX-License-Identifier: GPL-3.0-or-later
#include "emitter_private.h"
#include <stdio.h>

static void emit_cr_logical(FILE* out, const PPCInst* inst, const char* expr) {
    fprintf(out, "    {\n");
    fprintf(out, "        u32 a = (ctx->cr >> (31u - %uu)) & 1u;\n", inst->rA);
    fprintf(out, "        u32 b = (ctx->cr >> (31u - %uu)) & 1u;\n", inst->rB);
    fprintf(out, "        u32 mask = 0x80000000u >> %u;\n", inst->rD);
    fprintf(out, "        u32 value = (%s) & 1u;\n", expr);
    fprintf(out, "        ctx->cr = (ctx->cr & ~mask) | (value ? mask : 0u);\n");
    fprintf(out, "    }\n");
}

bool emit_system_instruction(FILE* out, const PPCInst* inst, u32 func_start, u32 func_end) {
    (void)func_start;
    (void)func_end;

    switch (inst->op) {
    case PPC_OP_TWI:
        fprintf(out, "    if (ppc_trap_condition(%uu, ctx->gpr[%u], (u32)(s32)%d)) {\n",
                inst->to, inst->rA, (int)inst->simm);
        fprintf(out, "        ppc_program_exception(ctx, PPC_PROGRAM_TRAP, 0x%08Xu);\n", inst->address);
        fprintf(out, "        return;\n");
        fprintf(out, "    }\n");
        return true;

    case PPC_OP_TW:
        fprintf(out, "    if (ppc_trap_condition(%uu, ctx->gpr[%u], ctx->gpr[%u])) {\n",
                inst->to, inst->rA, inst->rB);
        fprintf(out, "        ppc_program_exception(ctx, PPC_PROGRAM_TRAP, 0x%08Xu);\n", inst->address);
        fprintf(out, "        return;\n");
        fprintf(out, "    }\n");
        return true;

    case PPC_OP_DCBZ:
        emit_dcbz(out, inst);
        return true;

    case PPC_OP_DCBZ_L:
        fprintf(out, "    {\n");
        fprintf(out, "        u32 ea = ");
        emit_xform_ea(out, inst->rA, inst->rB, false);
        fprintf(out, ";\n");
        fprintf(out, "        ppc_dcbz_l(ctx, ea, 0x%08Xu);\n", inst->address);
        fprintf(out, "        if (ctx->exception) return;\n");
        fprintf(out, "    }\n");
        return true;

    case PPC_OP_DCBTST:
    case PPC_OP_DCBT:
        /* Pure prefetch hints: no architectural effect. */
        fprintf(out, "    (void)ctx;\n");
        return true;

    case PPC_OP_DCBST:
    case PPC_OP_DCBF:
    case PPC_OP_DCBI:
    case PPC_OP_ICBI:
        /* Cache maintenance is environment state (self-modifying code
         * detection, icache coherency): defer to the runtime's instruction
         * fallback so the host can invalidate any recompiled coverage. */
        fprintf(out, "    ppc_fallback_instruction(ctx, 0x%08Xu, 0x%08Xu);\n",
                inst->raw, inst->address);
        fprintf(out, "    return;\n");
        return true;

    case PPC_OP_CRAND:  emit_cr_logical(out, inst, "a & b"); break;
    case PPC_OP_CRANDC: emit_cr_logical(out, inst, "a & ~b"); break;
    case PPC_OP_CREQV:  emit_cr_logical(out, inst, "~(a ^ b)"); break;
    case PPC_OP_CRNAND: emit_cr_logical(out, inst, "~(a & b)"); break;
    case PPC_OP_CRNOR:  emit_cr_logical(out, inst, "~(a | b)"); break;
    case PPC_OP_CROR:   emit_cr_logical(out, inst, "a | b"); break;
    case PPC_OP_CRORC:  emit_cr_logical(out, inst, "a | ~b"); break;
    case PPC_OP_CRXOR:  emit_cr_logical(out, inst, "a ^ b"); break;

    case PPC_OP_MCRF: {
        u32 dst_shift = cr_field_shift(inst->crfD);
        u32 src_shift = cr_field_shift(inst->crfS);
        fprintf(out, "    {\n");
        fprintf(out, "        u32 bits = (ctx->cr >> %u) & 0xFu;\n", src_shift);
        fprintf(out, "        ctx->cr = (ctx->cr & ~(0xFu << %u)) | (bits << %u);\n",
                dst_shift, dst_shift);
        fprintf(out, "    }\n");
        return true;
    }

    case PPC_OP_MCRXR: {
        u32 dst_shift = cr_field_shift(inst->crfD);
        fprintf(out, "    {\n");
        fprintf(out, "        u32 bits = (ctx->xer >> 28) & 0xFu;\n");
        fprintf(out, "        ctx->cr = (ctx->cr & ~(0xFu << %u)) | (bits << %u);\n",
                dst_shift, dst_shift);
        fprintf(out, "        ctx->xer &= ~0xE0000000u;\n");
        fprintf(out, "    }\n");
        return true;
    }

    case PPC_OP_MFCR:
        fprintf(out, "    ctx->gpr[%u] = ctx->cr;\n", inst->rD);
        return true;

    case PPC_OP_MTCRF: {
        u32 mask = 0;
        for (u32 crf = 0; crf < 8; crf++) {
            if (inst->crm & (0x80u >> crf))
                mask |= 0xFu << cr_field_shift((u8)crf);
        }
        if (mask) {
            fprintf(out, "    ctx->cr = (ctx->cr & ~0x%08Xu) | (ctx->gpr[%u] & 0x%08Xu);\n",
                    mask, inst->rS, mask);
        } else {
            fprintf(out, "    // mtcrf mask selects no CR fields\n");
        }
        return true;
    }

    case PPC_OP_MFMSR:
        fprintf(out, "    ctx->gpr[%u] = ctx->msr;\n", inst->rD);
        return true;

    case PPC_OP_MTMSR:
        fprintf(out, "    ctx->msr = ctx->gpr[%u];\n", inst->rS);
        return true;

    case PPC_OP_MFSR:
        fprintf(out, "    ctx->gpr[%u] = ctx->sr[%u];\n", inst->rD, inst->sr);
        return true;

    case PPC_OP_MFSRIN:
        fprintf(out, "    ctx->gpr[%u] = ctx->sr[(ctx->gpr[%u] >> 28) & 0xFu];\n",
                inst->rD, inst->rB);
        return true;

    case PPC_OP_MTSR:
        fprintf(out, "    ctx->sr[%u] = ctx->gpr[%u];\n", inst->sr, inst->rS);
        return true;

    case PPC_OP_MTSRIN:
        fprintf(out, "    ctx->sr[(ctx->gpr[%u] >> 28) & 0xFu] = ctx->gpr[%u];\n",
                inst->rB, inst->rS);
        return true;

    case PPC_OP_MFTB:
        fprintf(out, "    ctx->gpr[%u] = ppc_mftb(ctx, %uu, 0x%08Xu);\n",
                inst->rD, inst->spr, inst->address);
        fprintf(out, "    if (ctx->exception) return;\n");
        return true;

    case PPC_OP_MFSPR:
        switch (inst->spr) {
        case 1: fprintf(out, "    ctx->gpr[%u] = ctx->xer;\n", inst->rD); break;
        case 8: fprintf(out, "    ctx->gpr[%u] = ctx->lr;\n", inst->rD); break;
        case 9: fprintf(out, "    ctx->gpr[%u] = ctx->ctr;\n", inst->rD); break;
        case 26: fprintf(out, "    ctx->gpr[%u] = ctx->srr0;\n", inst->rD); break;
        case 27: fprintf(out, "    ctx->gpr[%u] = ctx->srr1;\n", inst->rD); break;
        case 268:
        case 269:
            fprintf(out, "    ctx->gpr[%u] = ppc_mftb(ctx, %uu, 0x%08Xu);\n",
                    inst->rD, inst->spr, inst->address);
            fprintf(out, "    if (ctx->exception) return;\n");
            break;
        case 912: fprintf(out, "    ctx->gpr[%u] = ctx->gqr[0];\n", inst->rD); break;
        case 913: fprintf(out, "    ctx->gpr[%u] = ctx->gqr[1];\n", inst->rD); break;
        case 914: fprintf(out, "    ctx->gpr[%u] = ctx->gqr[2];\n", inst->rD); break;
        case 915: fprintf(out, "    ctx->gpr[%u] = ctx->gqr[3];\n", inst->rD); break;
        case 916: fprintf(out, "    ctx->gpr[%u] = ctx->gqr[4];\n", inst->rD); break;
        case 917: fprintf(out, "    ctx->gpr[%u] = ctx->gqr[5];\n", inst->rD); break;
        case 918: fprintf(out, "    ctx->gpr[%u] = ctx->gqr[6];\n", inst->rD); break;
        case 919: fprintf(out, "    ctx->gpr[%u] = ctx->gqr[7];\n", inst->rD); break;
        case 282: fprintf(out, "    ctx->gpr[%u] = ctx->ear;\n", inst->rD); break;
        case 920: fprintf(out, "    ctx->gpr[%u] = ctx->hid2;\n", inst->rD); break;
        default:
            /* SPRs without a CPUState model (HID0, WPAR, DMAU/DMAL, DEC,
             * PMCs, ...) are environment state: defer to the runtime's
             * instruction fallback so it can supply the live value. */
            fprintf(out, "    // environment mfspr %u\n", inst->spr);
            fprintf(out, "    ppc_fallback_instruction(ctx, 0x%08Xu, 0x%08Xu);\n",
                    inst->raw, inst->address);
            fprintf(out, "    return;\n");
            break;
        }
        return true;

    case PPC_OP_MTSPR:
        switch (inst->spr) {
        case 1: fprintf(out, "    ctx->xer = ctx->gpr[%u];\n", inst->rS); break;
        case 8: fprintf(out, "    ctx->lr = ctx->gpr[%u];\n", inst->rS); break;
        case 9: fprintf(out, "    ctx->ctr = ctx->gpr[%u];\n", inst->rS); break;
        case 26: fprintf(out, "    ctx->srr0 = ctx->gpr[%u];\n", inst->rS); break;
        case 27: fprintf(out, "    ctx->srr1 = ctx->gpr[%u];\n", inst->rS); break;
        case 282: fprintf(out, "    ctx->ear = ctx->gpr[%u];\n", inst->rS); break;
        case 912: fprintf(out, "    ctx->gqr[0] = ctx->gpr[%u];\n", inst->rS); break;
        case 913: fprintf(out, "    ctx->gqr[1] = ctx->gpr[%u];\n", inst->rS); break;
        case 914: fprintf(out, "    ctx->gqr[2] = ctx->gpr[%u];\n", inst->rS); break;
        case 915: fprintf(out, "    ctx->gqr[3] = ctx->gpr[%u];\n", inst->rS); break;
        case 916: fprintf(out, "    ctx->gqr[4] = ctx->gpr[%u];\n", inst->rS); break;
        case 917: fprintf(out, "    ctx->gqr[5] = ctx->gpr[%u];\n", inst->rS); break;
        case 918: fprintf(out, "    ctx->gqr[6] = ctx->gpr[%u];\n", inst->rS); break;
        case 919: fprintf(out, "    ctx->gqr[7] = ctx->gpr[%u];\n", inst->rS); break;
        case 920: fprintf(out, "    ctx->hid2 = ctx->gpr[%u];\n", inst->rS); break;
        default:
            /* SPRs without a CPUState model carry environment side effects
             * (DMAU/DMAL locked-cache DMA, DEC, HID0 cache control, WPAR):
             * defer to the runtime's instruction fallback. */
            fprintf(out, "    // environment mtspr %u\n", inst->spr);
            fprintf(out, "    ppc_fallback_instruction(ctx, 0x%08Xu, 0x%08Xu);\n",
                    inst->raw, inst->address);
            fprintf(out, "    return;\n");
            break;
        }
        return true;

    case PPC_OP_TLBIE:
        fprintf(out, "    ppc_tlbie(ctx, ctx->gpr[%u], 0x%08Xu);\n", inst->rB, inst->address);
        fprintf(out, "    if (ctx->exception) return;\n");
        return true;

    case PPC_OP_SYNC:
    case PPC_OP_EIEIO:
    case PPC_OP_ISYNC:
    case PPC_OP_TLBSYNC:
        fprintf(out, "    ppc_memory_fence();\n");
        return true;

    case PPC_OP_ECIWX:
        fprintf(out, "    {\n");
        fprintf(out, "        u32 ea = ");
        emit_xform_ea(out, inst->rA, inst->rB, false);
        fprintf(out, ";\n");
        fprintf(out, "        u32 value = ppc_eciwx(ctx, ea, 0x%08Xu);\n", inst->address);
        fprintf(out, "        if (ctx->exception) return;\n");
        fprintf(out, "        ctx->gpr[%u] = value;\n", inst->rD);
        fprintf(out, "    }\n");
        return true;

    case PPC_OP_ECOWX:
        fprintf(out, "    {\n");
        fprintf(out, "        u32 ea = ");
        emit_xform_ea(out, inst->rA, inst->rB, false);
        fprintf(out, ";\n");
        fprintf(out, "        ppc_ecowx(ctx, ea, ctx->gpr[%u], 0x%08Xu);\n",
                inst->rS, inst->address);
        fprintf(out, "        if (ctx->exception) return;\n");
        fprintf(out, "    }\n");
        return true;

    default:
        return false;
    }
    return true;
}
