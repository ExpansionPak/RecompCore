// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef DOLRECOMP_LOADER_H
#define DOLRECOMP_LOADER_H

#include "core/types.h"
#include "backend/emitter.h"
#include "frontend/dol.h"
#include "frontend/rel.h"
#include "frontend/rpx.h"

typedef enum {
    EMBEDDED_DATA_NONE = 0,
    EMBEDDED_DATA_DOL,
    EMBEDDED_DATA_RPX
} EmbeddedDataMode;

typedef struct {
    const char* label;
    const char* name;
    const u8* data;
    u32 index;
    u32 file_offset;
    u32 address;
    u32 size;
    EmbeddedDataMode embedded_data_mode;
} LoadedCodeSection;

int embedded_data_word(EmbeddedDataMode mode, u32 raw);

int emit_code_sections_split(const LoadedCodeSection* sections,
                             u32 section_count, const char* output_path,
                             DolRecompCPU cpu, u32 entry_point, u32 jobs,
                             int local_chunks_dir);

int emit_dol_split(const DOLFile* dol, const char* output_path, DolRecompCPU cpu, u32 jobs, int local_chunks_dir);
int emit_rpx_split(const RPXFile* rpx, const char* output_path, DolRecompCPU cpu, u32 jobs, int local_chunks_dir);
int emit_rel_split(const RELFile* rel, const char* output_path, DolRecompCPU cpu, u32 jobs, int local_chunks_dir);
int emit_rel_directory(const char* input_dir, const char* output_root, const char* title_id, int titleless_mode, DolRecompCPU cpu, u32 jobs, u32 start_base);

#endif /* DOLRECOMP_LOADER_H */
