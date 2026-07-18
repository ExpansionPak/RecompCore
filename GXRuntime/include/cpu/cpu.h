#ifndef GXRUNTIME_CPU_COMPAT_H
#define GXRUNTIME_CPU_COMPAT_H

#include "core/cpu.h"

static inline u32 ppc_mfspr(CPUState* cpu, u16 spr, u32 cia)
{
    switch (spr)
    {
    case 1: return cpu->xer;
    case 8: return cpu->lr;
    case 9: return cpu->ctr;
    case 18: return cpu->dsisr;
    case 19: return cpu->dar;
    case 26: return cpu->srr0;
    case 27: return cpu->srr1;
    case 282: return cpu->ear;
    case 912:
    case 913:
    case 914:
    case 915:
    case 916:
    case 917:
    case 918:
    case 919: return cpu->gqr[spr - 912];
    case 920: return cpu->hid2;
    default: break;
    }

    const u32 saved = cpu->gpr[0];
    const u32 encoded_spr = (((u32)spr & 31u) << 5) | ((u32)spr >> 5);
    ppc_fallback_instruction(cpu, 0x7C0002A6u | (encoded_spr << 11), cia);
    const u32 value = cpu->gpr[0];
    cpu->gpr[0] = saved;
    return value;
}

static inline void ppc_mtspr(CPUState* cpu, u16 spr, u32 value, u32 cia)
{
    switch (spr)
    {
    case 1: cpu->xer = value; return;
    case 8: cpu->lr = value; return;
    case 9: cpu->ctr = value; return;
    case 18: cpu->dsisr = value; return;
    case 19: cpu->dar = value; return;
    case 26: cpu->srr0 = value; return;
    case 27: cpu->srr1 = value; return;
    case 282: cpu->ear = value; return;
    case 284: cpu->timebase = (cpu->timebase & 0xFFFFFFFF00000000ull) | value; return;
    case 285: cpu->timebase = ((u64)value << 32) | (cpu->timebase & 0xFFFFFFFFull); return;
    case 912:
    case 913:
    case 914:
    case 915:
    case 916:
    case 917:
    case 918:
    case 919: cpu->gqr[spr - 912] = value; return;
    case 920: cpu->hid2 = value; return;
    default: break;
    }

    const u32 saved = cpu->gpr[0];
    const u32 encoded_spr = (((u32)spr & 31u) << 5) | ((u32)spr >> 5);
    cpu->gpr[0] = value;
    ppc_fallback_instruction(cpu, 0x7C0003A6u | (encoded_spr << 11), cia);
    cpu->gpr[0] = saved;
}

#endif
