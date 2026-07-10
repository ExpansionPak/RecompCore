#ifndef STRIKERSRECOMP_HOST_HLE_DVD_H
#define STRIKERSRECOMP_HOST_HLE_DVD_H

#include "core/cpu.h"

void hle_DVDInit(CPUState* cpu);
void hle_DVDClose(CPUState* cpu);
void hle_DVDGetCommandBlockStatus(CPUState* cpu);
void hle_DVDGetDriveStatus(CPUState* cpu);
void hle_DVDConvertPathToEntrynum(CPUState* cpu);
void hle_DVDFastOpen(CPUState* cpu);
void hle_DVDReadAsyncPrio(CPUState* cpu);

#endif // STRIKERSRECOMP_HOST_HLE_DVD_H
