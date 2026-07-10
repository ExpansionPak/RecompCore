// SPDX-License-Identifier: GPL-3.0-or-later
#include "loader.h"
#include "util.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    RELFile rel;
} RELBatchItem;

int rpx_embedded_data_word(u32 raw) {
    return raw == 0x00400000u || raw == 0x00600000u;
}

int word_bytes_are_text(u32 raw) {
    u8 bytes[4] = {
        (u8)(raw >> 24),
        (u8)(raw >> 16),
        (u8)(raw >> 8),
        (u8)raw,
    };

    int printable = 0;
    for (u32 i = 0; i < 4; i++) {
        if (bytes[i] >= 0x20 && bytes[i] <= 0x7Eu) {
            printable++;
        } else if (bytes[i] != 0) {
            return 0;
        }
    }

    return printable >= 3;
}

int dol_embedded_data_word(u32 raw) {
    if (raw == 0)
        return 1;
    if ((raw >> 26) == 0)
        return 1;
    if (word_bytes_are_text(raw))
        return 1;
    return 0;
}

int embedded_data_word(EmbeddedDataMode mode, u32 raw) {
    switch (mode) {
    case EMBEDDED_DATA_DOL:
        return dol_embedded_data_word(raw);
    case EMBEDDED_DATA_RPX:
        return rpx_embedded_data_word(raw);
    default:
        return 0;
    }
}

int emit_dol_split(const DOLFile* dol, const char* output_path,
                          DolRecompCPU cpu, u32 jobs, int local_chunks_dir) {
    LoadedCodeSection sections[DOL_NUM_TEXT];
    u32 section_count = 0;

    for (u32 i = 0; i < DOL_NUM_TEXT; i++) {
        if (dol->header.text_sizes[i] == 0)
            continue;

        const u8* data = dol_get_text_section(dol, (int)i);
        if (!data)
            continue;

        LoadedCodeSection* section = &sections[section_count++];
        section->label = "text";
        section->name = NULL;
        section->data = data;
        section->index = i;
        section->file_offset = dol->header.text_offsets[i];
        section->address = dol->header.text_addresses[i];
        section->size = dol->header.text_sizes[i];
        section->embedded_data_mode = EMBEDDED_DATA_DOL;
    }

    return emit_code_sections_split(sections, section_count, output_path, cpu,
                                    dol->header.entry_point, jobs,
                                    local_chunks_dir);
}

int emit_rpx_split(const RPXFile* rpx, const char* output_path,
                          DolRecompCPU cpu, u32 jobs, int local_chunks_dir) {
    LoadedCodeSection sections[RPX_MAX_CODE_SECTIONS];

    for (u32 i = 0; i < rpx->code_section_count; i++) {
        const RPXCodeSection* code = &rpx->code_sections[i];
        LoadedCodeSection* section = &sections[i];
        section->label = "rpx";
        section->name = code->name;
        section->data = code->data;
        section->index = i;
        section->file_offset = code->offset;
        section->address = code->address;
        section->size = code->size;
        section->embedded_data_mode = EMBEDDED_DATA_RPX;
    }

    return emit_code_sections_split(sections, rpx->code_section_count,
                                    output_path, cpu, 0, jobs,
                                    local_chunks_dir);
}

int emit_rel_split(const RELFile* rel, const char* output_path,
                          DolRecompCPU cpu, u32 jobs, int local_chunks_dir) {
    LoadedCodeSection* sections =
        (LoadedCodeSection*)calloc(rel->section_count, sizeof(LoadedCodeSection));
    if (!sections) {
        fprintf(stderr, "error: out of memory\n");
        return 0;
    }

    u32 section_count = 0;
    for (u32 i = 0; i < rel->section_count; i++) {
        const RELSection* rel_section = &rel->sections[i];
        if (!rel_section->executable || rel_section->size == 0 || !rel_section->data)
            continue;

        LoadedCodeSection* section = &sections[section_count++];
        section->label = "rel";
        section->name = NULL;
        section->data = rel_section->data;
        section->index = rel_section->index;
        section->file_offset = rel_section->offset;
        section->address = rel_section->address;
        section->size = rel_section->size;
        section->embedded_data_mode = EMBEDDED_DATA_DOL;
    }

    int ok = emit_code_sections_split(sections, section_count, output_path, cpu,
                                       rel->entry_point, jobs, local_chunks_dir);
    free(sections);
    return ok;
}

static void rel_batch_free(RELBatchItem* items, u32 count) {
    if (!items)
        return;
    for (u32 i = 0; i < count; i++)
        rel_free(&items[i].rel);
    free(items);
}

static u32 align_up_cli(u32 value, u32 alignment, int* ok) {
    u64 result = ((u64)value + alignment - 1u) / alignment * alignment;
    if (result > 0xFFFFFFFFu) {
        *ok = 0;
        return 0;
    }
    return (u32)result;
}

#define REL_AUTO_ALIGN 0x10000u
static int next_rel_base(const RELFile* rel, u32* cursor) {
    u32 end;
    int ok = 1;
    if (rel->file_size > 0xFFFFFFFFu - rel->base_address ||
        rel->bss_size > 0xFFFFFFFFu - rel->base_address - rel->file_size) {
        fprintf(stderr, "error: REL auto address range overflow\n");
        return 0;
    }

    end = rel->base_address + rel->file_size + rel->bss_size;
    *cursor = align_up_cli(end, REL_AUTO_ALIGN, &ok);
    if (!ok) {
        fprintf(stderr, "error: REL auto address range overflow\n");
        return 0;
    }
    return 1;
}

static int check_duplicate_rel_module(const RELBatchItem* items, u32 count,
                                      u32 module_id) {
    for (u32 i = 0; i < count; i++) {
        if (items[i].rel.module_id == module_id) {
            fprintf(stderr, "error: duplicate REL module id %u\n", module_id);
            return 0;
        }
    }
    return 1;
}

int emit_rel_directory(const char* input_dir, const char* output_root,
                              const char* title_id, int titleless_mode,
                              DolRecompCPU cpu, u32 jobs, u32 start_base) {
    PathList paths = {0};
    RELBatchItem* items = NULL;
    RELModuleMapEntry* map_entries = NULL;
    char generated_root[1200];
    int ok = 0;
    u32 cursor = start_base;

    if (!collect_rel_paths(input_dir, &paths))
        goto done;
    path_list_sort(&paths);

    if (paths.count == 0) {
        fprintf(stderr, "error: no .rel files found in '%s'\n", input_dir);
        goto done;
    }

    items = (RELBatchItem*)calloc(paths.count, sizeof(*items));
    map_entries = (RELModuleMapEntry*)calloc(paths.count, sizeof(*map_entries));
    if (!items || !map_entries) {
        fprintf(stderr, "error: out of memory\n");
        goto done;
    }

    printf("found %u REL module%s\n", paths.count, paths.count == 1 ? "" : "s");
    for (u32 i = 0; i < paths.count; i++) {
        if (!rel_load_image(&items[i].rel, paths.paths[i], cursor))
            goto done;
        if (!check_duplicate_rel_module(items, i, items[i].rel.module_id))
            goto done;

        map_entries[i].module_id = items[i].rel.module_id;
        map_entries[i].rel = &items[i].rel;

        printf("  module %u: %s -> base 0x%08X\n",
               items[i].rel.module_id, paths.paths[i], items[i].rel.base_address);
        if (!next_rel_base(&items[i].rel, &cursor))
            goto done;
    }

    RELModuleMap map = { map_entries, paths.count };
    for (u32 i = 0; i < paths.count; i++) {
        if (!rel_apply_relocations(&items[i].rel, &map))
            goto done;
    }

    if (!build_generated_folder_path(output_root, title_id, titleless_mode,
                                     generated_root, sizeof(generated_root))) {
        goto done;
    }

    for (u32 i = 0; i < paths.count; i++) {
        char rel_output_path[1200];
        printf("\nREL %u/%u: %s\n", i + 1, paths.count, paths.paths[i]);
        rel_print_info(&items[i].rel, NULL);
        if (!build_rel_output_path(generated_root, paths.paths[i],
                                   items[i].rel.module_id,
                                   rel_output_path, sizeof(rel_output_path))) {
            goto done;
        }
        printf("\nwriting output to: %s\n", rel_output_path);
        if (!emit_rel_split(&items[i].rel, rel_output_path, cpu, jobs, 1))
            goto done;
    }

    ok = 1;

done:
    free(map_entries);
    rel_batch_free(items, paths.count);
    path_list_free(&paths);
    return ok;
}
