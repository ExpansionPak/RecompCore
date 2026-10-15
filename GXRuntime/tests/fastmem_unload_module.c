// SPDX-License-Identifier: GPL-3.0-or-later
#include "core/cpu.h"

static CPUState cpu;
static void* view;

static void* pointer(CPUState* state, u32 ea, u32 size) {
    (void)state;
    return ea == 0xFFFFF000u && size == 0 ? view : NULL;
}

__declspec(dllexport) int start_fastmem(void* base) {
    view = base;
    cpu.external_pointer = pointer;
    (void)mem_read32(&cpu, 0xCC000000u);
    return g_dolrecomp_fm_min == 0x06000000u;
}
