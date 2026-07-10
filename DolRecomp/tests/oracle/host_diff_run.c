#include <stdio.h>
#include <string.h>

#include "core/cpu.h"
#include "host_diff.h"
#include "host_fixture.inc"

extern void (*const host_oracle_funcs[])(CPUState*);

typedef struct {
    const char* name;
    const char* field;
    unsigned index;
} KnownMismatch;

static KnownMismatch known_mismatches[] = {
    /* The DolRecomp FP/paired-single/load-
     * store unit is now emitted as calls to a shared helper set that mirrors
     * Dolphin's interpreter bit-exactly (scalar/paired single Fill of both PS
     * lanes, Force25Bit frC rounding + single-precision round-once FMA tie
     * fix, FPRF/FI/FR/exception-summary FPSCR updates, lfs/stfs ConvertTo*
     * bit repack, and Helper_StoreString word-RMW for stswi/stswx). All 93
     * previously-catalogued scalar-single/frsp/FPSCR mismatches are retired
     * as genuine emitter fixes. New CONFIRMED EMITTER defects go here with
     * the golden (real-Gekko) value and an upstream-fix note. */

    /* TEST-HARNESS ARTIFACTS (NOT emitter defects). The auto stswi/stswx
     * cases use EA = gpr[13] (= 0x80065EE0, an oracle-window pointer). The
     * host harness runs the emitted C with normalize_pointer() remapping
     * that pointer to 0x80000080 so the EA lands in the 512-byte test RAM
     * window -- but stswi r12,r13,17 also STORES gpr[13]'s value as its 2nd
     * word, so the emitter faithfully stores the remapped 0x80000080 while
     * the golden capture stored the original 0x80065EE0. ppc_stsw is proven
     * byte-exact to Dolphin's Helper_StoreString on the un-remapped inputs
     * (idx132..135 -> 80 06 5E E0). Proper fix: regenerate the capture so
     * store-string data registers don't alias a remapped EA pointer (e.g.
     * EA in a register outside [rS, rS+ceil(NB/4))), or teach the harness
     * to rewrite matching output bytes. Until then these 6 bytes cannot
     * reproduce and are catalogued as artifacts. */
    {"auto.stswi", "mem", 133},
    {"auto.stswi", "mem", 134},
    {"auto.stswi", "mem", 135},
    {"auto.stswx", "mem", 133},
    {"auto.stswx", "mem", 134},
    {"auto.stswx", "mem", 135},
};
static int known_mismatch_seen[
    sizeof(known_mismatches) / sizeof(known_mismatches[0])];

static u32 normalize_pointer(u32 value, u32 oracle_base) {
    if (oracle_base && value >= oracle_base &&
        value - oracle_base < HOST_ORACLE_MEM_SIZE) {
        return GC_RAM_BASE + value - oracle_base;
    }
    return value;
}

static int known_mismatch(const char* name, const char* field, unsigned index) {
    const char* base_name = name;
    if (!strncmp(name, "fuzz.", 5)) {
        const char* after_round = strchr(name + 5, '.');
        if (after_round && after_round[1])
            base_name = after_round + 1;
    }
    const unsigned count =
        (unsigned)(sizeof(known_mismatches) / sizeof(known_mismatches[0]));
    for (unsigned i = 0; i < count; i++) {
        KnownMismatch* known = &known_mismatches[i];
        if (!strcmp(base_name, known->name) && !strcmp(field, known->field) &&
            index == known->index) {
            known_mismatch_seen[i] = 1;
            return 1;
        }
    }
    return 0;
}

static void report(const HostOracleCase* c, const char* field, unsigned index,
                   u64 got, u64 want, int* failures, int* known) {
    if (got == want)
        return;
    if (known_mismatch(c->name, field, index)) {
        (*known)++;
        printf("XFAIL %-12s %-6s[%u] got=%016llX want=%016llX\n",
               c->name, field, index, (unsigned long long)got,
               (unsigned long long)want);
    } else {
        (*failures)++;
        printf("FAIL  %-12s %-6s[%u] got=%016llX want=%016llX\n",
               c->name, field, index, (unsigned long long)got,
               (unsigned long long)want);
    }
}

int main(void) {
    CPUState cpu;
    if (!cpu_init(&cpu))
        return 2;

    int failures = 0;
    int known = 0;
    int xpass = 0;
    for (unsigned i = 0; i < host_oracle_case_count; i++) {
        const HostOracleCase* c = &host_oracle_cases[i];
        cpu_reset(&cpu);
        memcpy(cpu.ram, c->in_mem, HOST_ORACLE_MEM_SIZE);
        for (unsigned r = 0; r < 32; r++) {
            cpu.gpr[r] = normalize_pointer(c->in_gpr[r], c->oracle_mem_base);
            memcpy(&cpu.fpr[r], &c->in_fpr[r], sizeof(u64));
            memcpy(&cpu.ps1[r], &c->in_ps1[r], sizeof(u64));
        }
        cpu.cr = c->in_cr;
        cpu.xer = c->in_xer;
        cpu.lr = c->in_lr;
        cpu.ctr = c->in_ctr;
        cpu.msr = c->in_msr;
        cpu.fpscr = c->in_fpscr;
        cpu.srr0 = c->in_srr0;
        cpu.srr1 = c->in_srr1;
        cpu.dar = 0;
        cpu.dsisr = 0;
        cpu.ear = 0;
        cpu.hid2 = c->in_hid2;
        memcpy(cpu.sr, c->in_sr, sizeof(cpu.sr));
        memcpy(cpu.gqr, c->in_gqr, sizeof(cpu.gqr));
        cpu.pc = c->address;

        host_oracle_funcs[i](&cpu);

        for (unsigned r = 0; r < 32; r++) {
            u32 want_gpr = normalize_pointer(c->out_gpr[r], c->oracle_mem_base);
            report(c, "gpr", r, cpu.gpr[r], want_gpr, &failures, &known);
            u64 got;
            memcpy(&got, &cpu.fpr[r], sizeof(got));
            report(c, "fpr", r, got, c->out_fpr[r], &failures, &known);
            memcpy(&got, &cpu.ps1[r], sizeof(got));
            report(c, "ps1", r, got, c->out_ps1[r], &failures, &known);
        }
        report(c, "cr", 0, cpu.cr, c->out_cr, &failures, &known);
        report(c, "xer", 0, cpu.xer, c->out_xer, &failures, &known);
        report(c, "lr", 0, cpu.lr, c->out_lr, &failures, &known);
        report(c, "ctr", 0, cpu.ctr, c->out_ctr, &failures, &known);
        report(c, "msr", 0, cpu.msr, c->out_msr, &failures, &known);
        report(c, "fpscr", 0, cpu.fpscr, c->out_fpscr, &failures, &known);
        report(c, "srr0", 0, cpu.srr0, c->out_srr0, &failures, &known);
        report(c, "srr1", 0, cpu.srr1, c->out_srr1, &failures, &known);
        report(c, "dar", 0, cpu.dar, c->out_dar, &failures, &known);
        report(c, "dsisr", 0, cpu.dsisr, c->out_dsisr, &failures, &known);
        report(c, "ear", 0, cpu.ear, c->out_ear, &failures, &known);
        report(c, "hid2", 0, cpu.hid2, c->out_hid2, &failures, &known);
        for (unsigned r = 0; r < 16; r++)
            report(c, "sr", r, cpu.sr[r], c->out_sr[r], &failures, &known);
        for (unsigned r = 0; r < 8; r++)
            report(c, "gqr", r, cpu.gqr[r], c->out_gqr[r], &failures, &known);
        report(c, "exc", 0, cpu.exception, c->out_exception, &failures, &known);
        report(c, "pc", 0, cpu.pc, c->address + c->raw_count * 4u,
               &failures, &known);
        for (unsigned b = 0; b < HOST_ORACLE_MEM_SIZE; b++)
            report(c, "mem", b, cpu.ram[b], c->out_mem[b], &failures, &known);
    }

    const unsigned known_count =
        (unsigned)(sizeof(known_mismatches) / sizeof(known_mismatches[0]));
    for (unsigned i = 0; i < known_count; i++) {
        const KnownMismatch* expected = &known_mismatches[i];
        if (!known_mismatch_seen[i]) {
            xpass++;
            printf("XPASS %-12s %-6s[%u] catalogued mismatch disappeared\n",
                   expected->name, expected->field, expected->index);
        }
    }

    printf("full-state oracle differential: %u cases, %d XFAIL, "
           "%d unexpected, %d XPASS\n",
           host_oracle_case_count, known, failures, xpass);
    cpu_free(&cpu);
    return (failures || xpass) ? 1 : 0;
}
