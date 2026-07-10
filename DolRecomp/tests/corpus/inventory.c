/*
 * Inventory the decoded opcode/forms in real DOL text sections and compare
 * them with the semantic probe corpus.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "frontend/decoder.h"
#include "frontend/dol.h"
#include "opcode_samples.h"
#include "probe.h"
#include "probes.inc"

typedef struct {
    u64 count;
    u32 first_raw;
    u32 first_address;
    bool forms[256];
    bool probed;
    bool ordinary_matrix;
    bool dedicated_required;
} OpcodeStat;

typedef struct {
    u64 words;
    u64 known;
    u64 embedded;
    u64 unknown;
} Totals;

static int word_bytes_are_text(u32 raw) {
    u8 bytes[4] = {(u8)(raw >> 24), (u8)(raw >> 16), (u8)(raw >> 8), (u8)raw};
    int printable = 0;
    for (unsigned i = 0; i < 4; i++) {
        if (bytes[i] >= 0x20 && bytes[i] <= 0x7E)
            printable++;
        else if (bytes[i] != 0)
            return 0;
    }
    return printable >= 3;
}

static int embedded_data_word(u32 raw) {
    return raw == 0 || (raw >> 26) == 0 || word_bytes_are_text(raw);
}

static unsigned form_id(const PPCInst* inst) {
    return (inst->rc ? 1u : 0u) | (inst->oe ? 2u : 0u) |
           (inst->aa ? 4u : 0u) | (inst->lk ? 8u : 0u) |
           ((unsigned)inst->w << 4) | ((unsigned)inst->i << 5);
}

static unsigned form_count(const OpcodeStat* stat) {
    unsigned count = 0;
    for (unsigned i = 0; i < 256; i++)
        count += stat->forms[i] ? 1u : 0u;
    return count;
}

static bool requires_dedicated(PPCOpcode op) {
    switch (op) {
    case PPC_OP_B: case PPC_OP_BC: case PPC_OP_BCLR: case PPC_OP_BCCTR:
    case PPC_OP_SC: case PPC_OP_RFI: case PPC_OP_TWI: case PPC_OP_TW:
    case PPC_OP_MFMSR: case PPC_OP_MTMSR:
    case PPC_OP_MFSR: case PPC_OP_MFSRIN:
    case PPC_OP_MTSR: case PPC_OP_MTSRIN:
    case PPC_OP_MFTB:
    case PPC_OP_DCBST: case PPC_OP_DCBF: case PPC_OP_DCBTST:
    case PPC_OP_DCBT: case PPC_OP_DCBI: case PPC_OP_ICBI:
    case PPC_OP_DCBZ_L: case PPC_OP_TLBIE: case PPC_OP_TLBSYNC:
    case PPC_OP_ECIWX: case PPC_OP_ECOWX:
        return true;
    default:
        return false;
    }
}

static int scan_dol(const char* path, OpcodeStat* stats, Totals* totals) {
    DOLFile dol;
    if (!dol_load(&dol, path))
        return 0;
    for (int section = 0; section < DOL_NUM_TEXT; section++) {
        const u8* data = dol_get_text_section(&dol, section);
        const u32 size = dol.header.text_sizes[section];
        const u32 base = dol.header.text_addresses[section];
        if (!data)
            continue;
        for (u32 offset = 0; offset < size; offset += 4) {
            const u32 raw = read_be32(data + offset);
            const u32 address = base + offset;
            const PPCInst inst = ppc_decode(raw, address);
            totals->words++;
            if (inst.op == PPC_OP_UNKNOWN) {
                if (embedded_data_word(raw))
                    totals->embedded++;
                else
                    totals->unknown++;
                continue;
            }
            totals->known++;
            OpcodeStat* stat = &stats[inst.op];
            if (stat->count == 0) {
                stat->first_raw = raw;
                stat->first_address = address;
            }
            stat->count++;
            stat->forms[form_id(&inst)] = true;
        }
    }
    dol_free(&dol);
    return 1;
}

static void mark_probe_coverage(OpcodeStat* stats) {
    for (unsigned i = 0; i < probe_count; i++) {
        const Probe* p = &probes[i];
        const unsigned count = p->raw_words ? p->raw_count : 1u;
        for (unsigned j = 0; j < count; j++) {
            const u32 raw = p->raw_words ? p->raw_words[j] : p->raw;
            const PPCInst inst = ppc_decode(raw, p->address + j * 4u);
            if (inst.op != PPC_OP_UNKNOWN)
                stats[inst.op].probed = true;
        }
    }
    for (unsigned i = 0; i < opcode_raw_count; i++) {
        const PPCInst inst = ppc_decode(opcode_raws[i], 0x80006000u);
        if (inst.op == PPC_OP_UNKNOWN)
            continue;
        if (requires_dedicated(inst.op))
            stats[inst.op].dedicated_required = true;
        else
            stats[inst.op].ordinary_matrix = true;
    }
}

int main(int argc, char** argv) {
    if (argc < 4 || strcmp(argv[1], "--json") != 0) {
        fprintf(stderr, "usage: %s --json OUTPUT DOL...\n", argv[0]);
        return 2;
    }

    OpcodeStat stats[PPC_OP_COUNT];
    Totals totals = {0};
    memset(stats, 0, sizeof(stats));
    mark_probe_coverage(stats);
    for (int i = 3; i < argc; i++) {
        if (!scan_dol(argv[i], stats, &totals))
            return 2;
    }

    unsigned used = 0;
    unsigned covered = 0;
    unsigned missing = 0;
    unsigned ordinary = 0;
    unsigned dedicated = 0;
    for (int op = 1; op < PPC_OP_COUNT; op++) {
        if (!stats[op].count)
            continue;
        used++;
        if (stats[op].probed)
            covered++;
        if (stats[op].ordinary_matrix)
            ordinary++;
        if (stats[op].dedicated_required)
            dedicated++;
        if (!stats[op].ordinary_matrix && !stats[op].dedicated_required)
            missing++;
    }

    FILE* out = fopen(argv[2], "w");
    if (!out) {
        perror(argv[2]);
        return 2;
    }
    fprintf(out,
            "{\n  \"dol_count\": %d,\n  \"words\": %llu,\n"
            "  \"known_words\": %llu,\n  \"embedded_words\": %llu,\n"
            "  \"unknown_words\": %llu,\n  \"used_opcodes\": %u,\n"
            "  \"curated_probed_opcodes\": %u,\n"
            "  \"ordinary_matrix_opcodes\": %u,\n"
            "  \"dedicated_required_opcodes\": %u,\n"
            "  \"unclassified_opcodes\": %u,\n"
            "  \"opcodes\": [\n",
            argc - 3, (unsigned long long)totals.words,
            (unsigned long long)totals.known,
            (unsigned long long)totals.embedded,
            (unsigned long long)totals.unknown, used, covered, ordinary,
            dedicated, missing);
    bool first = true;
    for (int op = 1; op < PPC_OP_COUNT; op++) {
        const OpcodeStat* stat = &stats[op];
        if (!stat->count)
            continue;
        fprintf(out,
                "%s    {\"name\": \"%s\", \"count\": %llu, \"forms\": %u, "
                "\"curated_probed\": %s, \"ordinary_matrix\": %s, "
                "\"dedicated_required\": %s, \"first_raw\": \"%08X\", "
                "\"first_address\": \"%08X\"}",
                first ? "" : ",\n", ppc_op_name((PPCOpcode)op),
                (unsigned long long)stat->count, form_count(stat),
                stat->probed ? "true" : "false",
                stat->ordinary_matrix ? "true" : "false",
                stat->dedicated_required ? "true" : "false", stat->first_raw,
                stat->first_address);
        first = false;
    }
    fprintf(out, "\n  ]\n}\n");
    fclose(out);

    printf("real-DOL corpus: %llu words, %u decoded opcodes, %u curated, "
           "%u ordinary-matrix, %u dedicated, %u unclassified, %llu unknown\n",
           (unsigned long long)totals.words, used, covered, ordinary, dedicated,
           missing,
           (unsigned long long)totals.unknown);
    return 0;
}
