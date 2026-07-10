/*
 * Convert the emitted-differential probe table into devkitPPC oracle cases.
 * Privileged or non-returning instructions are kept out of the ordinary
 * trampoline and reported explicitly for dedicated exception/control tests.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "core/types.h"
#include "frontend/decoder.h"
#include "opcode_samples.h"
#include "probe.h"
#include "probes.inc"

static u64 single_to_double_bits(u64 raw) {
    u32 bits = (u32)raw;
    float single;
    double value;
    u64 result;
    memcpy(&single, &bits, sizeof(single));
    value = single;
    memcpy(&result, &value, sizeof(result));
    return result;
}

static const char* unsafe_reason(PPCOpcode op) {
    switch (op) {
    case PPC_OP_BCLR:
    case PPC_OP_BCCTR:
        return "branch needs a bounded control-flow fixture";
    case PPC_OP_SC:
        return "system-call exception needs dedicated vector";
    case PPC_OP_RFI:
        return "exception return changes machine context";
    case PPC_OP_TWI:
    case PPC_OP_TW:
        return "trap outcome needs dedicated program-exception vector";
    case PPC_OP_MTMSR:
    case PPC_OP_MTSR:
    case PPC_OP_MTSRIN:
    case PPC_OP_TLBIE:
    case PPC_OP_TLBSYNC:
        return "privileged translation/MSR mutation needs isolated context";
    case PPC_OP_MFMSR:
    case PPC_OP_MFSR:
    case PPC_OP_MFSRIN:
        return "host machine state is not a portable seeded input";
    case PPC_OP_MFTB:
        return "time-base result is intentionally nondeterministic";
    case PPC_OP_DCBST:
    case PPC_OP_DCBF:
    case PPC_OP_DCBTST:
    case PPC_OP_DCBT:
    case PPC_OP_DCBI:
    case PPC_OP_ICBI:
    case PPC_OP_DCBZ_L:
        return "cache operation needs a cache-observable fixture";
    case PPC_OP_ECIWX:
    case PPC_OP_ECOWX:
        return "external-control access needs dedicated hardware setup";
    default:
        return NULL;
    }
}

static const char* auto_unsafe_reason(PPCOpcode op) {
    if (op == PPC_OP_B || op == PPC_OP_BC)
        return "branch needs a bounded control-flow fixture";
    return unsafe_reason(op);
}

static bool is_memory_op(PPCOpcode op) {
    switch (op) {
    case PPC_OP_LWZ: case PPC_OP_LWZU: case PPC_OP_LBZ: case PPC_OP_LBZU:
    case PPC_OP_STW: case PPC_OP_STWU: case PPC_OP_STB: case PPC_OP_STBU:
    case PPC_OP_LHZ: case PPC_OP_LHZU: case PPC_OP_LHA: case PPC_OP_LHAU:
    case PPC_OP_STH: case PPC_OP_STHU: case PPC_OP_LMW: case PPC_OP_STMW:
    case PPC_OP_LWZX: case PPC_OP_LWZUX: case PPC_OP_LBZX: case PPC_OP_LBZUX:
    case PPC_OP_LHZX: case PPC_OP_LHZUX: case PPC_OP_LHAX: case PPC_OP_LHAUX:
    case PPC_OP_LWBRX: case PPC_OP_LHBRX:
    case PPC_OP_STWX: case PPC_OP_STWUX: case PPC_OP_STBX: case PPC_OP_STBUX:
    case PPC_OP_STHX: case PPC_OP_STHUX:
    case PPC_OP_STWBRX: case PPC_OP_STHBRX:
    case PPC_OP_LSWI: case PPC_OP_LSWX: case PPC_OP_STSWI: case PPC_OP_STSWX:
    case PPC_OP_LWARX: case PPC_OP_STWCX: case PPC_OP_STFIWX:
    case PPC_OP_LFS: case PPC_OP_LFSU: case PPC_OP_LFD: case PPC_OP_LFDU:
    case PPC_OP_STFS: case PPC_OP_STFSU: case PPC_OP_STFD: case PPC_OP_STFDU:
    case PPC_OP_LFSX: case PPC_OP_LFSUX: case PPC_OP_LFDX: case PPC_OP_LFDUX:
    case PPC_OP_STFSX: case PPC_OP_STFSUX: case PPC_OP_STFDX: case PPC_OP_STFDUX:
    case PPC_OP_PSQ_L: case PPC_OP_PSQ_LU: case PPC_OP_PSQ_ST: case PPC_OP_PSQ_STU:
    case PPC_OP_PSQ_LX: case PPC_OP_PSQ_LUX:
    case PPC_OP_PSQ_STX: case PPC_OP_PSQ_STUX:
    case PPC_OP_DCBZ:
        return true;
    default:
        return false;
    }
}

static bool is_indexed_memory_op(PPCOpcode op) {
    switch (op) {
    case PPC_OP_LWZX: case PPC_OP_LWZUX: case PPC_OP_LBZX: case PPC_OP_LBZUX:
    case PPC_OP_LHZX: case PPC_OP_LHZUX: case PPC_OP_LHAX: case PPC_OP_LHAUX:
    case PPC_OP_LWBRX: case PPC_OP_LHBRX:
    case PPC_OP_STWX: case PPC_OP_STWUX: case PPC_OP_STBX: case PPC_OP_STBUX:
    case PPC_OP_STHX: case PPC_OP_STHUX:
    case PPC_OP_STWBRX: case PPC_OP_STHBRX:
    case PPC_OP_LSWX: case PPC_OP_STSWX:
    case PPC_OP_LWARX: case PPC_OP_STWCX: case PPC_OP_STFIWX:
    case PPC_OP_LFSX: case PPC_OP_LFSUX: case PPC_OP_LFDX: case PPC_OP_LFDUX:
    case PPC_OP_STFSX: case PPC_OP_STFSUX: case PPC_OP_STFDX: case PPC_OP_STFDUX:
    case PPC_OP_PSQ_LX: case PPC_OP_PSQ_LUX:
    case PPC_OP_PSQ_STX: case PPC_OP_PSQ_STUX:
    case PPC_OP_DCBZ:
        return true;
    default:
        return false;
    }
}

static const char* probe_unsafe_reason(const Probe* probe) {
    const unsigned count = probe->raw_words ? probe->raw_count : 1u;
    for (unsigned i = 0; i < count; i++) {
        const u32 raw = probe->raw_words ? probe->raw_words[i] : probe->raw;
        const PPCInst inst = ppc_decode(raw, probe->address + i * 4u);
        const char* reason = unsafe_reason(inst.op);
        if (reason)
            return reason;
        if (inst.op == PPC_OP_UNKNOWN)
            return "decoder rejected instruction";
        if (inst.op == PPC_OP_B || inst.op == PPC_OP_BC) {
            const u32 end = probe->address + count * 4u;
            if (inst.branch_target < probe->address || inst.branch_target > end)
                return "direct branch leaves probe sequence";
        }
    }
    return NULL;
}

static bool fuzzable_probe(const Probe* probe) {
    return !probe_unsafe_reason(probe) && probe->raw_words && probe->raw_count > 1;
}

static void emit_u32_array(FILE* out, const char* field, const u32* values,
                           unsigned count) {
    fprintf(out, "            .%s = {", field);
    for (unsigned i = 0; i < count; i++) {
        if (values[i])
            fprintf(out, " [%u] = 0x%08Xu,", i, values[i]);
    }
    fprintf(out, " },\n");
}

static void emit_u64_array(FILE* out, const char* field, const u64* values,
                           unsigned count, bool singles) {
    fprintf(out, "            .%s = {", field);
    for (unsigned i = 0; i < count; i++) {
        u64 value = singles ? single_to_double_bits(values[i]) : values[i];
        if (value)
            fprintf(out, " [%u] = 0x%016llXull,", i,
                    (unsigned long long)value);
    }
    fprintf(out, " },\n");
}

static u64 double_to_bits(double value) {
    u64 bits;
    memcpy(&bits, &value, sizeof(bits));
    return bits;
}

static void make_auto_input(const PPCInst* inst, u32 gpr[32], u64 fpr[32],
                            u64 ps1[32], u8 mem[256], u32* guest_mem_base,
                            u32* mem_len, u32* xer) {
    for (unsigned i = 0; i < 32; i++) {
        gpr[i] = 0x01020304u * (i + 1u);
        fpr[i] = double_to_bits(1.0 + (double)i * 0.25);
        ps1[i] = double_to_bits(2.0 + (double)i * 0.25);
    }
    for (unsigned i = 0; i < 256; i += 8) {
        mem[i + 0] = 0x3F; mem[i + 1] = 0x80;
        mem[i + 2] = 0x00; mem[i + 3] = 0x00;
        mem[i + 4] = 0x40; mem[i + 5] = 0x00;
        mem[i + 6] = 0x00; mem[i + 7] = 0x00;
    }
    *guest_mem_base = 0;
    *mem_len = 0;
    *xer = 0;
    if (!is_memory_op(inst->op))
        return;

    const u32 base = 0x80010000u;
    const u32 target = base + 0x80u;
    *guest_mem_base = base;
    *mem_len = 256;
    if (is_indexed_memory_op(inst->op)) {
        if (inst->rA)
            gpr[inst->rA] = target;
        gpr[inst->rB] = 0;
    } else if (inst->rA) {
        gpr[inst->rA] = target - (u32)(s32)inst->simm;
    }
    if (inst->op == PPC_OP_LSWX || inst->op == PPC_OP_STSWX)
        *xer = 8u;
}

int main(int argc, char** argv) {
    if (argc != 3) {
        fprintf(stderr, "usage: %s OUTPUT_INC COVERAGE_JSON\n", argv[0]);
        return 2;
    }
    FILE* out = fopen(argv[1], "w");
    FILE* report = fopen(argv[2], "w");
    if (!out || !report) {
        perror("open output");
        if (out) fclose(out);
        if (report) fclose(report);
        return 2;
    }

    bool probed_ops[PPC_OP_COUNT];
    memset(probed_ops, 0, sizeof(probed_ops));
    for (unsigned i = 0; i < probe_count; i++) {
        const Probe* p = &probes[i];
        const unsigned count = p->raw_words ? p->raw_count : 1u;
        for (unsigned j = 0; j < count; j++) {
            const u32 raw = p->raw_words ? p->raw_words[j] : p->raw;
            const PPCInst inst = ppc_decode(raw, p->address + j * 4u);
            if (inst.op != PPC_OP_UNKNOWN)
                probed_ops[inst.op] = true;
        }
    }

    unsigned accepted = 0;
    unsigned skipped = 0;
    for (unsigned i = 0; i < probe_count; i++) {
        if (probe_unsafe_reason(&probes[i]))
            skipped++;
        else
            accepted++;
    }
    unsigned auto_count = 0;
    unsigned dedicated_opcode_count = 0;
    for (unsigned i = 0; i < opcode_raw_count; i++) {
        const PPCInst inst = ppc_decode(opcode_raws[i], 0x80006000u);
        if (inst.op == PPC_OP_UNKNOWN || probed_ops[inst.op])
            continue;
        if (auto_unsafe_reason(inst.op))
            dedicated_opcode_count++;
        else
            auto_count++;
    }
    unsigned fuzz_count = 0;
    for (unsigned i = 0; i < probe_count; i++) {
        if (fuzzable_probe(&probes[i]))
            fuzz_count += 2;
    }

    fprintf(out, "/* Generated from tests/diff/probes.inc. Do not edit. */\n");
    unsigned output_index = 0;
    for (unsigned i = 0; i < probe_count; i++) {
        const Probe* p = &probes[i];
        if (probe_unsafe_reason(p))
            continue;
        const unsigned count = p->raw_words ? p->raw_count : 1u;
        fprintf(out, "static const u32 generated_words_%u[] = {", output_index);
        for (unsigned j = 0; j < count; j++) {
            const u32 raw = p->raw_words ? p->raw_words[j] : p->raw;
            fprintf(out, " 0x%08Xu,", raw);
        }
        fprintf(out, " };\n");
        output_index++;
    }
    for (unsigned i = 0; i < opcode_raw_count; i++) {
        const PPCInst inst = ppc_decode(opcode_raws[i], 0x80006000u);
        if (inst.op == PPC_OP_UNKNOWN || probed_ops[inst.op] ||
            auto_unsafe_reason(inst.op))
            continue;
        fprintf(out, "static const u32 generated_words_%u[] = { 0x%08Xu };\n",
                output_index, opcode_raws[i]);
        output_index++;
    }
    for (unsigned i = 0; i < probe_count; i++) {
        const Probe* p = &probes[i];
        if (!fuzzable_probe(p))
            continue;
        for (unsigned round = 0; round < 2; round++) {
            fprintf(out, "static const u32 generated_words_%u[] = {", output_index);
            for (unsigned j = 0; j < p->raw_count; j++)
                fprintf(out, " 0x%08Xu,", p->raw_words[j]);
            fprintf(out, " };\n");
            output_index++;
        }
    }

    fprintf(out, "\nstatic const OracleCase generated_cases[] = {\n");
    output_index = 0;
    for (unsigned i = 0; i < probe_count; i++) {
        const Probe* p = &probes[i];
        if (probe_unsafe_reason(p))
            continue;
        const unsigned count = p->raw_words ? p->raw_count : 1u;
        fprintf(out, "    {\n");
        fprintf(out, "        .name = \"%s\", .words = generated_words_%u, "
                     ".word_count = %uu,\n", p->name, output_index, count);
        fprintf(out, "        .guest_mem_base = 0x%08Xu, .mem_len = %uu,\n",
                p->mem_base, p->mem_len);
        if (p->mem_len) {
            fprintf(out, "        .mem = {");
            for (u32 j = 0; j < p->mem_len; j++)
                fprintf(out, " 0x%02Xu,", p->mem_in[j]);
            fprintf(out, " },\n");
        }
        fprintf(out, "        .in = {\n");
        emit_u32_array(out, "gpr", p->gpr, 32);
        emit_u64_array(out, "fpr", p->fpr, 32, p->fp_single_inputs);
        emit_u64_array(out, "ps1", p->ps1, 32, p->fp_single_inputs);
        fprintf(out, "            .cr = 0x%08Xu, .xer = 0x%08Xu, "
                     ".lr = 0x%08Xu, .ctr = 0x%08Xu,\n",
                p->cr, p->xer, p->lr, p->ctr);
        fprintf(out, "            .fpscr = 0x%016llXull,\n",
                (unsigned long long)p->fpscr);
        fprintf(out, "        },\n");
        fprintf(out, "    },\n");
        output_index++;
    }
    for (unsigned i = 0; i < opcode_raw_count; i++) {
        const PPCInst inst = ppc_decode(opcode_raws[i], 0x80006000u);
        if (inst.op == PPC_OP_UNKNOWN || probed_ops[inst.op] ||
            auto_unsafe_reason(inst.op))
            continue;
        u32 gpr[32];
        u64 fpr[32];
        u64 ps1[32];
        u8 mem[256];
        u32 guest_mem_base;
        u32 mem_len;
        u32 xer;
        make_auto_input(&inst, gpr, fpr, ps1, mem, &guest_mem_base, &mem_len, &xer);

        fprintf(out, "    {\n");
        fprintf(out, "        .name = \"auto.%s\", .words = generated_words_%u, "
                     ".word_count = 1u,\n", ppc_op_name(inst.op), output_index);
        fprintf(out, "        .guest_mem_base = 0x%08Xu, .mem_len = %uu,\n",
                guest_mem_base, mem_len);
        if (mem_len) {
            fprintf(out, "        .mem = {");
            for (u32 j = 0; j < mem_len; j++)
                fprintf(out, " 0x%02Xu,", mem[j]);
            fprintf(out, " },\n");
        }
        fprintf(out, "        .in = {\n");
        emit_u32_array(out, "gpr", gpr, 32);
        emit_u64_array(out, "fpr", fpr, 32, false);
        emit_u64_array(out, "ps1", ps1, 32, false);
        fprintf(out, "            .cr = 0x12345678u, .xer = 0x%08Xu,\n", xer);
        fprintf(out, "        },\n");
        fprintf(out, "    },\n");
        output_index++;
    }
    for (unsigned i = 0; i < probe_count; i++) {
        const Probe* p = &probes[i];
        if (!fuzzable_probe(p))
            continue;
        for (unsigned round = 0; round < 2; round++) {
            fprintf(out, "    {\n");
            fprintf(out, "        .name = \"fuzz.%u.%s\", .words = generated_words_%u, "
                         ".word_count = %uu,\n", round, p->name, output_index,
                         p->raw_count);
            fprintf(out, "        .guest_mem_base = 0x%08Xu, .mem_len = %uu,\n",
                    p->mem_base, p->mem_len);
            if (p->mem_len) {
                fprintf(out, "        .mem = {");
                for (u32 j = 0; j < p->mem_len; j++)
                    fprintf(out, " 0x%02Xu,", p->mem_in[j]);
                fprintf(out, " },\n");
            }
            fprintf(out, "        .in = {\n");
            emit_u32_array(out, "gpr", p->gpr, 32);
            emit_u64_array(out, "fpr", p->fpr, 32, p->fp_single_inputs);
            emit_u64_array(out, "ps1", p->ps1, 32, p->fp_single_inputs);
            fprintf(out, "            .cr = 0x%08Xu, .xer = 0x%08Xu, "
                         ".lr = 0x%08Xu, .ctr = 0x%08Xu,\n",
                    p->cr ^ (0x11111111u * (round + 1u)),
                    p->xer ^ (0x20000000u * (round & 1u)), p->lr, p->ctr);
            fprintf(out, "            .fpscr = 0x%016llXull,\n",
                    (unsigned long long)(p->fpscr ^ (0x000000000000F000ull << round)));
            fprintf(out, "        },\n");
            fprintf(out, "    },\n");
            output_index++;
        }
    }
    fprintf(out, "};\n");
    fprintf(out, "static const unsigned generated_case_count = %uu;\n",
            accepted + auto_count + fuzz_count);

    fprintf(report, "{\n  \"source_probes\": %u,\n"
                    "  \"accepted_curated\": %u,\n"
                    "  \"accepted_auto_opcodes\": %u,\n"
                    "  \"propagation_fuzz\": %u,\n"
                    "  \"total_ordinary\": %u,\n"
                    "  \"skipped_curated\": [\n",
            probe_count, accepted, auto_count, fuzz_count,
            accepted + auto_count + fuzz_count);
    bool first = true;
    for (unsigned i = 0; i < probe_count; i++) {
        const char* reason = probe_unsafe_reason(&probes[i]);
        if (!reason)
            continue;
        fprintf(report, "%s    {\"name\": \"%s\", \"reason\": \"%s\"}",
                first ? "" : ",\n", probes[i].name, reason);
        first = false;
    }
    fprintf(report, "\n  ],\n  \"dedicated_opcodes\": [\n");
    first = true;
    for (unsigned i = 0; i < opcode_raw_count; i++) {
        const PPCInst inst = ppc_decode(opcode_raws[i], 0x80006000u);
        if (inst.op == PPC_OP_UNKNOWN || probed_ops[inst.op] ||
            !auto_unsafe_reason(inst.op))
            continue;
        fprintf(report, "%s    {\"name\": \"%s\", \"reason\": \"%s\"}",
                first ? "" : ",\n", ppc_op_name(inst.op),
                auto_unsafe_reason(inst.op));
        first = false;
    }
    fprintf(report, "\n  ]\n}\n");

    fclose(out);
    fclose(report);
    fprintf(stderr, "generated %u ordinary oracle cases (%u curated, %u opcode "
                    "fill); %u curated and %u opcode families require dedicated "
                    "harnesses; %u propagation fuzz cases\n",
            accepted + auto_count + fuzz_count, accepted, auto_count,
            skipped, dedicated_opcode_count, fuzz_count);
    return 0;
}
