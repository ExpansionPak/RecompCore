// SPDX-License-Identifier: GPL-3.0-or-later
#include "emitter_private.h"
#include <stdio.h>

static void emit_dynamic_branch(FILE* out, const PPCInst* inst, const char* target_expr) {
    if (inst->lk) {
        fprintf(out, "            ctx->lr = 0x%08Xu;\n", inst->address + 4);
    }
    fprintf(out, "            ctx->pc = %s;\n", target_expr);
    fprintf(out, "            return;\n");
}

bool emit_branch_instruction(FILE* out, const PPCInst* inst, u32 func_start, u32 func_end) {
    switch (inst->op) {
    case PPC_OP_B:
        fprintf(out, "    {\n");
        emit_direct_branch(out, inst,
                           branch_target_is_local(func_start, func_end, inst->branch_target));
        fprintf(out, "    }\n");
        return true;

    case PPC_OP_BC:
        fprintf(out, "    {\n");
        emit_branch_condition(out, inst->bo, inst->bi);
        fprintf(out, "        if (ctr_ok && cr_ok) {\n");
        emit_direct_branch(out, inst,
                           branch_target_is_local(func_start, func_end, inst->branch_target));
        fprintf(out, "        }\n");
        fprintf(out, "    }\n");
        return true;

    case PPC_OP_BCLR:
        fprintf(out, "    {\n");
        emit_branch_condition(out, inst->bo, inst->bi);
        fprintf(out, "        if (ctr_ok && cr_ok) {\n");
        emit_dynamic_branch(out, inst, "ctx->lr & ~3u");
        fprintf(out, "        }\n");
        fprintf(out, "    }\n");
        return true;

    case PPC_OP_BCCTR:
        fprintf(out, "    {\n");
        emit_branch_condition(out, inst->bo, inst->bi);
        fprintf(out, "        if (ctr_ok && cr_ok) {\n");
        emit_dynamic_branch(out, inst, "ctx->ctr & ~3u");
        fprintf(out, "        }\n");
        fprintf(out, "    }\n");
        return true;

    case PPC_OP_SC:
        fprintf(out, "    ppc_system_call_exception(ctx, 0x%08Xu);\n", inst->address);
        fprintf(out, "    return;\n");
        return true;

    case PPC_OP_RFI:
        fprintf(out, "    ppc_rfi(ctx, 0x%08Xu);\n", inst->address);
        fprintf(out, "    return;\n");
        return true;

    default:
        return false;
    }
}
