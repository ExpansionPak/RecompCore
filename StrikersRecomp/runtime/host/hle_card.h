#ifndef STRIKERSRECOMP_HOST_HLE_CARD_H
#define STRIKERSRECOMP_HOST_HLE_CARD_H

#include "core/cpu.h"

void hle_CARDInit(CPUState* cpu);
void hle_CARDProbe(CPUState* cpu);
void hle_CARDProbeEx(CPUState* cpu);
void hle_CARDGetResultCode(CPUState* cpu);
void hle_CARDGetFastMode(CPUState* cpu);
void hle_CARDGetXferredBytes(CPUState* cpu);
void hle_CARDUnmount(CPUState* cpu);
void hle_CARDClose(CPUState* cpu);
void hle_CARDMountAsync(CPUState* cpu);
void hle_CARDCheckAsync(CPUState* cpu);
void hle_CARDCheckExAsync(CPUState* cpu);
void hle_CARDFreeBlocks(CPUState* cpu);
void hle_CARDOpen(CPUState* cpu);
void hle_CARDCreateAsync(CPUState* cpu);
void hle_CARDDeleteAsync(CPUState* cpu);
void hle_CARDReadAsync(CPUState* cpu);
void hle_CARDWriteAsync(CPUState* cpu);
void hle_CARDGetStatus(CPUState* cpu);
void hle_CARDSetStatusAsync(CPUState* cpu);
void hle_CARDGetSerialNo(CPUState* cpu);
void hle_CARDFormatAsync(CPUState* cpu);

#endif // STRIKERSRECOMP_HOST_HLE_CARD_H
