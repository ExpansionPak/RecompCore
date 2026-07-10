// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef DOLRECOMP_ANALYSIS_H
#define DOLRECOMP_ANALYSIS_H

#include "core/types.h"
#include "frontend/decoder.h"
#include "loader.h"

#define SMC_DISPLAY_RANGE_LIMIT 8

typedef struct {
    int known;
    u32 value;
} KnownReg;

typedef struct {
    u32 start;
    u32 end;
} SMCRange;

typedef struct {
    int possible;
    int allocation_failed;
    u32 range_count;
    u32 range_capacity;
    SMCRange* ranges;
} SMCAnalysis;

void smc_analysis_free(SMCAnalysis* smc);
void smc_note(SMCAnalysis* smc, u32 addr);
int write_smc_report(const SMCAnalysis* smc, const char* path);

int code_range_overlaps(const LoadedCodeSection* sections, u32 section_count, u32 address, u32 size);
int known_dform_ea(const KnownReg regs[32], const PPCInst* inst, u32* ea);
int known_indexed_ea(const KnownReg regs[32], const PPCInst* inst, u32* ea);
int store_size_for_inst(const PPCInst* inst, u32* size);
int inst_has_dform_ea(PPCOpcode op);
int inst_has_indexed_ea(PPCOpcode op);
int smc_inst_targets_code(const LoadedCodeSection* sections, u32 section_count, const PPCInst* inst, const KnownReg regs[32]);

void clear_reg(KnownReg regs[32], u8 reg);
void set_reg(KnownReg regs[32], u8 reg, u32 value);
void update_known_regs(KnownReg regs[32], const PPCInst* inst);
void analyze_smc_section(const LoadedCodeSection* sections, u32 section_count, const PPCInst* insts, u32 count, SMCAnalysis* smc);

#endif /* DOLRECOMP_ANALYSIS_H */
