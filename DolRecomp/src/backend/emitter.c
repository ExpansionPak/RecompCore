// SPDX-License-Identifier: GPL-3.0-or-later
#include "emitter_private.h"
#include <stdlib.h>
#include <string.h>

u32 cr_field_shift(u8 crf) {
    return 4u * (7u - (u32)crf);
}

u32 ppc_mask32(u8 mb, u8 me) {
    u32 mask = 0;
    u8 bit = mb;

    for (;;) {
        mask |= 0x80000000u >> bit;
        if (bit == me)
            break;
        bit = (u8)((bit + 1) & 31);
    }

    return mask;
}

void emit_set_cr0_from_gpr(FILE* out, u8 reg) {
    fprintf(out, "        u32 cr_bits = 0;\n");
    fprintf(out, "        s32 cr_value = (s32)ctx->gpr[%u];\n", reg);
    fprintf(out, "        if (cr_value < 0)  cr_bits |= 0x8u;\n");
    fprintf(out, "        if (cr_value > 0)  cr_bits |= 0x4u;\n");
    fprintf(out, "        if (cr_value == 0) cr_bits |= 0x2u;\n");
    fprintf(out, "        cr_bits |= (ctx->xer >> 31) & 1u;\n");
    fprintf(out, "        ctx->cr = (ctx->cr & 0x0FFFFFFFu) | (cr_bits << 28);\n");
}

void emit_set_cr1_from_fpscr(FILE* out) {
    fprintf(out, "        ctx->cr = (ctx->cr & 0xF0FFFFFFu) | ((ctx->fpscr >> 4) & 0x0F000000u);\n");
}

void emit_compare_s32(FILE* out, u8 crf, const char* lhs, const char* rhs) {
    u32 shift = cr_field_shift(crf);

    fprintf(out, "    {\n");
    fprintf(out, "        s32 val_a = (s32)(%s);\n", lhs);
    fprintf(out, "        s32 val_b = (s32)(%s);\n", rhs);
    fprintf(out, "        u32 cr_bits = 0;\n");
    fprintf(out, "        if (val_a < val_b)  cr_bits |= 0x8u;\n");
    fprintf(out, "        if (val_a > val_b)  cr_bits |= 0x4u;\n");
    fprintf(out, "        if (val_a == val_b) cr_bits |= 0x2u;\n");
    fprintf(out, "        cr_bits |= (ctx->xer >> 31) & 1u;\n");
    fprintf(out, "        ctx->cr = (ctx->cr & ~(0xFu << %u)) | (cr_bits << %u);\n",
            shift, shift);
    fprintf(out, "    }\n");
}

void emit_compare_u32(FILE* out, u8 crf, const char* lhs, const char* rhs) {
    u32 shift = cr_field_shift(crf);

    fprintf(out, "    {\n");
    fprintf(out, "        u32 val_a = (u32)(%s);\n", lhs);
    fprintf(out, "        u32 val_b = (u32)(%s);\n", rhs);
    fprintf(out, "        u32 cr_bits = 0;\n");
    fprintf(out, "        if (val_a < val_b)  cr_bits |= 0x8u;\n");
    fprintf(out, "        if (val_a > val_b)  cr_bits |= 0x4u;\n");
    fprintf(out, "        if (val_a == val_b) cr_bits |= 0x2u;\n");
    fprintf(out, "        cr_bits |= (ctx->xer >> 31) & 1u;\n");
    fprintf(out, "        ctx->cr = (ctx->cr & ~(0xFu << %u)) | (cr_bits << %u);\n",
            shift, shift);
    fprintf(out, "    }\n");
}

void emit_fcompare(FILE* out, const PPCInst* inst) {
    fprintf(out, "    ppc_fcmp(ctx, %uu, ctx->fpr[%u], ctx->fpr[%u], %s);\n",
            inst->crfD, inst->rA, inst->rB,
            inst->op == PPC_OP_FCMPO ? "true" : "false");
}

void emit_dform_ea(FILE* out, u8 ra, s16 simm, bool update) {
    if (ra == 0 && !update) {
        fprintf(out, "(u32)(s32)(%d)", (int)simm);
    } else {
        fprintf(out, "ctx->gpr[%u] + (u32)(s32)(%d)", ra, (int)simm);
    }
}

void emit_xform_ea(FILE* out, u8 ra, u8 rb, bool update) {
    if (ra == 0 && !update) {
        fprintf(out, "ctx->gpr[%u]", rb);
    } else {
        fprintf(out, "ctx->gpr[%u] + ctx->gpr[%u]", ra, rb);
    }
}

void emit_load(FILE* out, const PPCInst* inst, const char* read_expr,
                      bool update) {
    fprintf(out, "    {\n");
    fprintf(out, "        u32 ea = ");
    emit_dform_ea(out, inst->rA, inst->simm, update);
    fprintf(out, ";\n");
    fprintf(out, "        ctx->gpr[%u] = %s;\n", inst->rD, read_expr);
    if (update) {
        fprintf(out, "        ctx->gpr[%u] = ea;\n", inst->rA);
    }
    fprintf(out, "    }\n");
}

void emit_loadx(FILE* out, const PPCInst* inst, const char* read_expr,
                       bool update) {
    fprintf(out, "    {\n");
    fprintf(out, "        u32 ea = ");
    emit_xform_ea(out, inst->rA, inst->rB, update);
    fprintf(out, ";\n");
    fprintf(out, "        ctx->gpr[%u] = %s;\n", inst->rD, read_expr);
    if (update) {
        fprintf(out, "        ctx->gpr[%u] = ea;\n", inst->rA);
    }
    fprintf(out, "    }\n");
}

void emit_store(FILE* out, const PPCInst* inst, const char* write_func,
                       const char* cast_type, bool update) {
    fprintf(out, "    {\n");
    fprintf(out, "        u32 ea = ");
    emit_dform_ea(out, inst->rA, inst->simm, update);
    fprintf(out, ";\n");
    fprintf(out, "        %s(ctx, ea, (%s)ctx->gpr[%u]);\n",
            write_func, cast_type, inst->rS);
    if (update) {
        fprintf(out, "        ctx->gpr[%u] = ea;\n", inst->rA);
    }
    fprintf(out, "    }\n");
}

void emit_storex(FILE* out, const PPCInst* inst, const char* write_func,
                        const char* cast_type, bool update) {
    fprintf(out, "    {\n");
    fprintf(out, "        u32 ea = ");
    emit_xform_ea(out, inst->rA, inst->rB, update);
    fprintf(out, ";\n");
    fprintf(out, "        %s(ctx, ea, (%s)ctx->gpr[%u]);\n",
            write_func, cast_type, inst->rS);
    if (update) {
        fprintf(out, "        ctx->gpr[%u] = ea;\n", inst->rA);
    }
    fprintf(out, "    }\n");
}

void emit_fload(FILE* out, const PPCInst* inst, bool single,
                       bool update) {
    fprintf(out, "    {\n");
    fprintf(out, "        u32 ea = ");
    emit_dform_ea(out, inst->rA, inst->simm, update);
    fprintf(out, ";\n");
    fprintf(out, "        if (!%s(ctx, %uu, ea, 0x%08Xu)) return;\n",
            single ? "ppc_lfs_op" : "ppc_lfd_op", inst->rD, inst->address);
    if (update) {
        fprintf(out, "        ctx->gpr[%u] = ea;\n", inst->rA);
    }
    fprintf(out, "    }\n");
}

void emit_floadx(FILE* out, const PPCInst* inst, bool single,
                        bool update) {
    fprintf(out, "    {\n");
    fprintf(out, "        u32 ea = ");
    emit_xform_ea(out, inst->rA, inst->rB, update);
    fprintf(out, ";\n");
    fprintf(out, "        if (!%s(ctx, %uu, ea, 0x%08Xu)) return;\n",
            single ? "ppc_lfs_op" : "ppc_lfd_op", inst->rD, inst->address);
    if (update) {
        fprintf(out, "        ctx->gpr[%u] = ea;\n", inst->rA);
    }
    fprintf(out, "    }\n");
}

void emit_fstore(FILE* out, const PPCInst* inst, bool single,
                        bool update) {
    fprintf(out, "    {\n");
    fprintf(out, "        u32 ea = ");
    emit_dform_ea(out, inst->rA, inst->simm, update);
    fprintf(out, ";\n");
    fprintf(out, "        if (!%s(ctx, %uu, ea, 0x%08Xu)) return;\n",
            single ? "ppc_stfs_op" : "ppc_stfd_op", inst->rS, inst->address);
    if (update) {
        fprintf(out, "        ctx->gpr[%u] = ea;\n", inst->rA);
    }
    fprintf(out, "    }\n");
}

void emit_fstorex(FILE* out, const PPCInst* inst, bool single,
                         bool update) {
    fprintf(out, "    {\n");
    fprintf(out, "        u32 ea = ");
    emit_xform_ea(out, inst->rA, inst->rB, update);
    fprintf(out, ";\n");
    fprintf(out, "        if (!%s(ctx, %uu, ea, 0x%08Xu)) return;\n",
            single ? "ppc_stfs_op" : "ppc_stfd_op", inst->rS, inst->address);
    if (update) {
        fprintf(out, "        ctx->gpr[%u] = ea;\n", inst->rA);
    }
    fprintf(out, "    }\n");
}

void emit_psq_load(FILE* out, const PPCInst* inst, bool indexed,
                          bool update) {
    fprintf(out, "    {\n");
    fprintf(out, "        u32 ea = ");
    if (indexed) {
        emit_xform_ea(out, inst->rA, inst->rB, update);
    } else {
        emit_dform_ea(out, inst->rA, inst->simm, update);
    }
    fprintf(out, ";\n");
    fprintf(out, "        if (!ppc_psq_load(ctx, %uu, ea, %s, %uu, %s, 0x%08Xu)) return;\n",
            inst->rD, inst->w ? "true" : "false", inst->i,
            indexed ? "true" : "false", inst->address);
    if (update) {
        fprintf(out, "        ctx->gpr[%u] = ea;\n", inst->rA);
    }
    fprintf(out, "    }\n");
}

void emit_psq_store(FILE* out, const PPCInst* inst, bool indexed,
                           bool update) {
    fprintf(out, "    {\n");
    fprintf(out, "        u32 ea = ");
    if (indexed) {
        emit_xform_ea(out, inst->rA, inst->rB, update);
    } else {
        emit_dform_ea(out, inst->rA, inst->simm, update);
    }
    fprintf(out, ";\n");
    fprintf(out, "        if (!ppc_psq_store(ctx, %uu, ea, %s, %uu, %s, 0x%08Xu)) return;\n",
            inst->rS, inst->w ? "true" : "false", inst->i,
            indexed ? "true" : "false", inst->address);
    if (update) {
        fprintf(out, "        ctx->gpr[%u] = ea;\n", inst->rA);
    }
    fprintf(out, "    }\n");
}

void emit_dcbz(FILE* out, const PPCInst* inst) {
    fprintf(out, "    {\n");
    fprintf(out, "        u32 ea = ");
    emit_xform_ea(out, inst->rA, inst->rB, false);
    fprintf(out, ";\n");
    fprintf(out, "        ea &= ~31u;\n");
    fprintf(out, "        for (u32 i = 0; i < 32; i += 4) mem_write32(ctx, ea + i, 0);\n");
    fprintf(out, "    }\n");
}

void emit_branch_condition(FILE* out, u8 bo, u8 bi) {
    bool ctr_ignored = (bo & 0x04) != 0;
    bool cond_ignored = (bo & 0x10) != 0;

    if (!ctr_ignored) {
        fprintf(out, "        ctx->ctr--;\n");
        fprintf(out, "        bool ctr_ok = (((ctx->ctr != 0) ? 1u : 0u) ^ %uu) != 0;\n",
                (bo >> 1) & 1u);
    } else {
        fprintf(out, "        bool ctr_ok = true;\n");
    }

    if (!cond_ignored) {
        u32 mask = 0x80000000u >> bi;
        fprintf(out, "        bool cr_ok = (((ctx->cr & 0x%08Xu) != 0) == %s);\n",
                mask, ((bo >> 3) & 1u) ? "true" : "false");
    } else {
        fprintf(out, "        bool cr_ok = true;\n");
    }
}

bool branch_target_is_local(u32 func_start, u32 func_end, u32 target) {
    return target >= func_start && target < func_end && ((target - func_start) & 3u) == 0;
}

void emit_direct_branch(FILE* out, const PPCInst* inst, bool local_target) {
    bool local_backward = local_target && inst->branch_target <= inst->address;

    if (inst->lk) {
        fprintf(out, "            ctx->lr = 0x%08Xu;\n", inst->address + 4);
        fprintf(out, "            ctx->pc = 0x%08Xu;\n", inst->branch_target);
        fprintf(out, "            return;\n");
        return;
    }
    if (local_backward) {
        fprintf(out, "            ctx->pc = 0x%08Xu;\n", inst->branch_target);
        fprintf(out, "            return;\n");
    } else if (local_target) {
        fprintf(out, "            goto label_%08X;\n", inst->branch_target);
    } else {
        fprintf(out, "            ctx->pc = 0x%08Xu;\n", inst->branch_target);
        fprintf(out, "            return;\n");
    }
}

static const char* emit_cpu_macro(DolRecompCPU cpu) {
    switch (cpu) {
    case DOLRECOMP_CPU_BROADWAY:
        return "BROADWAY";
    case DOLRECOMP_CPU_ESPRESSO:
        return "ESPRESSO";
    case DOLRECOMP_CPU_GEKKO:
    default:
        return "GEKKO";
    }
}

static const char* emit_cpu_label(DolRecompCPU cpu) {
    switch (cpu) {
    case DOLRECOMP_CPU_BROADWAY:
        return "broadway";
    case DOLRECOMP_CPU_ESPRESSO:
        return "espresso";
    case DOLRECOMP_CPU_GEKKO:
    default:
        return "gekko";
    }
}

void emit_header_for_cpu(FILE* out, DolRecompCPU cpu) {
    fprintf(out,
        "// DolRecomp output\n"
        "// cpu: %s\n"
        "\n"
        "#define DOLRECOMP_CPU_%s 1\n"
        "#define DOLRECOMP_CPU_NAME \"%s\"\n"
        "\n"
        "#include <string.h>\n"
        "#include <math.h>\n"
        "#include \"core/cpu.h\"\n"
        "\n"
        "static inline u32 dolrecomp_rotl32(u32 value, u32 sh) {\n"
        "    sh &= 31u;\n"
        "    return sh ? ((value << sh) | (value >> (32u - sh))) : value;\n"
        "}\n"
        "\n"
        "static inline f32 dolrecomp_f32_from_bits(u32 bits) {\n"
        "    f32 value;\n"
        "    memcpy(&value, &bits, sizeof(value));\n"
        "    return value;\n"
        "}\n"
        "\n"
        "static inline u32 dolrecomp_f32_to_bits(f32 value) {\n"
        "    u32 bits;\n"
        "    memcpy(&bits, &value, sizeof(bits));\n"
        "    return bits;\n"
        "}\n"
        "\n"
        "static inline f64 dolrecomp_f64_from_bits(u64 bits) {\n"
        "    f64 value;\n"
        "    memcpy(&value, &bits, sizeof(value));\n"
        "    return value;\n"
        "}\n"
        "\n"
        "static inline u64 dolrecomp_f64_to_bits(f64 value) {\n"
        "    u64 bits;\n"
        "    memcpy(&bits, &value, sizeof(bits));\n"
        "    return bits;\n"
        "}\n"
        "\n"
        "static inline f64 dolrecomp_ps_round(f64 value) {\n"
        "    return (f64)(f32)value;\n"
        "}\n"
        "\n"
        "static inline f64 dolrecomp_ps_from_bits(u32 bits) {\n"
        "    return (f64)dolrecomp_f32_from_bits(bits);\n"
        "}\n"
        "\n"
        "static inline u32 dolrecomp_ps_to_bits(f64 value) {\n"
        "    return dolrecomp_f32_to_bits((f32)value);\n"
        "}\n"
        "\n"
        ,
        emit_cpu_label(cpu),
        emit_cpu_macro(cpu),
        emit_cpu_label(cpu));
}

void emit_header(FILE* out) {
    emit_header_for_cpu(out, DOLRECOMP_CPU_GEKKO);
}

void emit_footer(FILE* out) {
    fprintf(out, "\n// end\n");
}

static void emit_instruction_with_range(FILE* out, const PPCInst* inst,
                                        u32 func_start, u32 func_end) {
    char disasm[64];
    ppc_disasm(disasm, sizeof(disasm), inst);
    fprintf(out, "    // %08X: %s\n", inst->address, disasm);

    if (inst->embedded_data) {
        fprintf(out, "    // embedded data\n\n");
        return;
    }

    if (ppc_op_uses_fpu(inst->op)) {
        fprintf(out, "    if (!ppc_fp_available(ctx, 0x%08Xu)) return;\n", inst->address);
    }

    if (emit_integer_instruction(out, inst, func_start, func_end)) return;
    if (emit_float_instruction(out, inst, func_start, func_end)) return;
    if (emit_branch_instruction(out, inst, func_start, func_end)) return;
    if (emit_system_instruction(out, inst, func_start, func_end)) return;

    fprintf(out, "    // unknown instruction fallback\n");
    fprintf(out, "    ppc_fallback_instruction(ctx, 0x%08Xu, 0x%08Xu);\n",
            inst->raw, inst->address);
    fprintf(out, "    return;\n");
}

void emit_instruction(FILE* out, const PPCInst* inst) {
    emit_instruction_with_range(out, inst, 0, (u32)-1);
}

static bool mfspr_is_modeled(u16 spr) {
    switch (spr) {
    case 1: case 8: case 9: case 26: case 27:
    case 268: case 269: case 282:
    case 912: case 913: case 914: case 915:
    case 916: case 917: case 918: case 919: case 920:
        return true;
    default:
        return false;
    }
}

static bool mtspr_is_modeled(u16 spr) {
    switch (spr) {
    case 1: case 8: case 9: case 26: case 27: case 282:
    case 912: case 913: case 914: case 915:
    case 916: case 917: case 918: case 919: case 920:
        return true;
    default:
        return false;
    }
}

static bool inst_routes_to_fallback(const PPCInst* inst) {
    switch (inst->op) {
    case PPC_OP_DCBST:
    case PPC_OP_DCBF:
    case PPC_OP_DCBI:
    case PPC_OP_ICBI:
    case PPC_OP_UNKNOWN:
        return true;
    case PPC_OP_MFSPR:
        return !mfspr_is_modeled(inst->spr);
    case PPC_OP_MTSPR:
        return !mtspr_is_modeled(inst->spr);
    default:
        return false;
    }
}

static bool inst_ends_block(const PPCInst* inst) {
    switch (inst->op) {
    case PPC_OP_B:
    case PPC_OP_BC:
    case PPC_OP_BCLR:
    case PPC_OP_BCCTR:
    case PPC_OP_SC:
    case PPC_OP_RFI:
        return true;
    default:
        return inst_routes_to_fallback(inst);
    }
}

static u32 inst_cycle_cost(const PPCInst* inst) {
    if (inst->embedded_data || inst_routes_to_fallback(inst))
        return 0;

    switch (inst->op) {
    case PPC_OP_MULLI:
        return 3;
    case PPC_OP_SC:
        return 2;
    case PPC_OP_RFI:
        return 2;
    case PPC_OP_TW:
        return 2;
    case PPC_OP_LMW:
    case PPC_OP_STMW:
        return 11;
    case PPC_OP_MULLW:
    case PPC_OP_MULLWO:
    case PPC_OP_MULHW:
    case PPC_OP_MULHWU:
        return 5;
    case PPC_OP_DIVW:
    case PPC_OP_DIVWO:
    case PPC_OP_DIVWU:
    case PPC_OP_DIVWUO:
        return 40;
    case PPC_OP_DCBZ:
        return 5;
    case PPC_OP_DCBTST:
    case PPC_OP_DCBT:
        return 2;
    case PPC_OP_MFSR:
    case PPC_OP_MFSRIN:
        return 3;
    case PPC_OP_MTSPR:
        return 2;
    case PPC_OP_SYNC:
        return 3;
    case PPC_OP_MTFSB0:
    case PPC_OP_MTFSB1:
    case PPC_OP_MTFSF:
    case PPC_OP_MTFSFI:
        return 3;
    case PPC_OP_FDIVS:
        return 17;
    case PPC_OP_FDIV:
        return 31;
    case PPC_OP_PS_DIV:
        return 17;
    case PPC_OP_PS_RSQRTE:
        return 2;
    default:
        return 1;
    }
}

void emit_function(FILE* out, const PPCInst* insts, u32 count, u32 func_addr) {
    u32 i;
    u32 func_end = func_addr + count * 4u;

    u8* leader = (u8*)calloc(count ? count : 1u, sizeof(u8));
    u32* block_cost = (u32*)calloc(count ? count : 1u, sizeof(u32));

    if (count)
        leader[0] = 1;
    for (i = 0; i < count; i++) {
        const PPCInst* inst = &insts[i];
        if (inst->embedded_data)
            continue;
        if (inst_ends_block(inst) && i + 1u < count)
            leader[i + 1u] = 1;
        if ((inst->op == PPC_OP_B || inst->op == PPC_OP_BC) &&
            branch_target_is_local(func_addr, func_end, inst->branch_target)) {
            leader[(inst->branch_target - func_addr) / 4u] = 1;
        }
    }
    for (i = 0; i < count; i++) {
        u32 j;
        if (!leader[i])
            continue;
        for (j = i;;) {
            block_cost[i] += inst_cycle_cost(&insts[j]);
            if (inst_ends_block(&insts[j]))
                break;
            j++;
            if (j >= count || leader[j])
                break;
        }
    }

    fprintf(out, "void func_%08X(CPUState* ctx) {\n", func_addr);
    fprintf(out, "    switch (ctx->pc) {\n");
    for (i = 0; i < count; i++) {
        fprintf(out, "    case 0x%08Xu: goto label_%08X;\n",
                insts[i].address, insts[i].address);
    }
    fprintf(out, "    default: return;\n");
    fprintf(out, "    }\n");

    for (i = 0; i < count; i++) {
        fprintf(out, "label_%08X:\n", insts[i].address);
        fprintf(out, "    ctx->pc = 0x%08Xu;\n", insts[i].address);
        if (leader[i] && block_cost[i] != 0)
            fprintf(out, "    ctx->downcount -= %u;\n", block_cost[i]);
        emit_instruction_with_range(out, &insts[i], func_addr, func_end);
    }

    free(leader);
    free(block_cost);

    fprintf(out, "    ctx->pc = 0x%08Xu;\n", func_end);
    fprintf(out, "}\n\n");
}
