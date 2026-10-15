// SPDX-License-Identifier: GPL-3.0-or-later
#include "core/cpu.h"
#include <stdlib.h>

extern void ppc_set_mem_write_journal(PPCMemWriteJournal fn, void* user);

static unsigned queries, reads, writes, journals;
static void* pointer(CPUState* cpu, u32 ea, u32 size) {
    (void)cpu; (void)ea; (void)size;
    ++queries;
    return NULL;
}
static u64 read_external(CPUState* cpu, u32 ea, u8 size) {
    (void)cpu;
    if (ea != 0xCC000000u || size != 4)
        abort();
    ++reads;
    return 0x12345678u;
}
static void write_external(CPUState* cpu, u32 ea, u64 value, u8 size) {
    (void)cpu;
    if (ea != 0xCC000000u || size != 4 || value != 0x87654321u)
        abort();
    ++writes;
}
static void journal(u32 offset, u32 size, void* user) {
    (void)user;
    if (offset != 0xD0u || size != 4)
        abort();
    ++journals;
}
int main(void) {
    CPUState cpu = {0};
    cpu.ram = calloc(1, GC_MAIN_RAM_SIZE);
    if (!cpu.ram)
        return 1;
    cpu.ram_size = GC_MAIN_RAM_SIZE;
    cpu.external_pointer = pointer;
    cpu.external_read = read_external;
    cpu.external_write = write_external;
    ppc_set_mem_write_journal(journal, NULL);
    mem_write32(&cpu, 0xC00000D0u, 0xAABBCCDDu);
    int bad = mem_read32(&cpu, 0x800000D0u) != 0xAABBCCDDu;
    bad |= mem_read32(&cpu, 0xCC000000u) != 0x12345678u;
    mem_write32(&cpu, 0xCC000000u, 0x87654321u);
    bad |= g_dolrecomp_fm_min != (1ull << 32) || !g_dolrecomp_fm_queried;
    bad |= queries != 0 || reads != 1 || writes != 1 || journals != 1;
    ppc_set_mem_write_journal(NULL, NULL);
    free(cpu.ram);
    return bad;
}
