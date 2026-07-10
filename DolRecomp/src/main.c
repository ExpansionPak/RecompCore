#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <direct.h>
#include <windows.h>
#else
#include <dirent.h>
#include <pthread.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

#ifdef _WIN32
#define popen _popen
#define pclose _pclose
#endif

#include "core/types.h"
#include "frontend/dol.h"
#include "frontend/rel.h"
#include "frontend/rpx.h"
#include "frontend/disc_extract.h"
#include "frontend/decoder.h"
#include "backend/emitter.h"
#include "util.h"
#include "loader.h"
#include "analysis.h"
#include "cli.h"
#include "setup.h"
#include "jobs.h"

#define EMIT_CHUNK_INSTRUCTIONS 4096u
#define REL_AUTO_BASE 0x80500000u
#define REL_AUTO_ALIGN 0x10000u

static int build_named_output_path(const char* output_root, const char* title_id,
                                   char* output_path, size_t output_path_size) {
    char folder_path[1100];

    if (!make_dir_tree(output_root))
        return 0;

    char folder_name[128];
    if (snprintf(folder_name, sizeof(folder_name), "%s_generated", title_id) >=
        (int)sizeof(folder_name)) {
        fprintf(stderr, "error: output path is too long\n");
        return 0;
    }

    if (!join_path(folder_path, sizeof(folder_path), output_root, folder_name)) {
        fprintf(stderr, "error: output path is too long\n");
        return 0;
    }

    if (!make_dir_tree(folder_path))
        return 0;

    char file_name[128];
    if (snprintf(file_name, sizeof(file_name), "%s.c", title_id) >=
        (int)sizeof(file_name)) {
        fprintf(stderr, "error: output path is too long\n");
        return 0;
    }

    if (!join_path(output_path, output_path_size, folder_path, file_name)) {
        fprintf(stderr, "error: output path is too long\n");
        return 0;
    }

    return 1;
}

static int build_gamecube_output_path(const char* output_root, char* output_path,
                                      size_t output_path_size) {
    char folder_path[1100];

    if (!make_dir_tree(output_root))
        return 0;

    if (!join_path(folder_path, sizeof(folder_path), output_root, "generated")) {
        fprintf(stderr, "error: output path is too long\n");
        return 0;
    }

    if (!make_dir_tree(folder_path))
        return 0;

    if (!join_path(output_path, output_path_size, folder_path, "generated.c")) {
        fprintf(stderr, "error: output path is too long\n");
        return 0;
    }

    return 1;
}



static int split_include_name(const char* stem, char* include_name, size_t include_size) {
    const char* base = path_basename(stem);
    int written = snprintf(include_name, include_size, "%s.h", base);
    return written > 0 && (size_t)written < include_size;
}

static void emit_chunk_prototype(FILE* out, u32 func_addr) {
    fprintf(out, "void func_%08X(CPUState* ctx);\n", func_addr);
}

typedef struct {
    u32 start;
    u32 end;
} FunctionRange;

typedef struct {
    FunctionRange* ranges;
    u32 count;
    u32 capacity;
} FunctionList;

static void function_list_free(FunctionList* list) {
    free(list->ranges);
    list->ranges = NULL;
    list->count = 0;
    list->capacity = 0;
}

static int function_list_add(FunctionList* list, u32 start, u32 end) {
    if (list->count == list->capacity) {
        u32 new_capacity = list->capacity ? list->capacity * 2u : 64u;
        FunctionRange* new_ranges =
            (FunctionRange*)realloc(list->ranges, new_capacity * sizeof(*new_ranges));
        if (!new_ranges) {
            fprintf(stderr, "error: out of memory\n");
            return 0;
        }
        list->ranges = new_ranges;
        list->capacity = new_capacity;
    }

    list->ranges[list->count].start = start;
    list->ranges[list->count].end = end;
    list->count++;
    return 1;
}

static void emit_dispatch_helpers(FILE* out, const FunctionList* funcs, u32 entry_point) {
    fprintf(out, "\n#define DOLRECOMP_ENTRY_POINT 0x%08Xu\n", entry_point);
    fprintf(out, "\ntypedef void (*DolRecompFunction)(CPUState* ctx);\n");
    fprintf(out, "\nstatic inline int dolrecomp_call(CPUState* ctx, u32 address) {\n");
    fprintf(out, "    if (ppc_host_call(ctx, address)) return 1;\n");

    bool optimized = false;
    if (funcs->count >= 3) {
        u32 stride = funcs->ranges[2].start - funcs->ranges[1].start;
        if (stride > 0) {
            bool matches = true;
            for (u32 i = 1; i < funcs->count; i++) {
                if (funcs->ranges[i].start != funcs->ranges[1].start + (i - 1) * stride) {
                    matches = false;
                    break;
                }
            }
            for (u32 i = 1; i < funcs->count - 1; i++) {
                if (funcs->ranges[i].end != funcs->ranges[i + 1].start) {
                    matches = false;
                    break;
                }
            }
            if (matches) {
                optimized = true;
                u32 first_start = funcs->ranges[0].start;
                u32 first_end = funcs->ranges[0].end;
                u32 regular_start = funcs->ranges[1].start;
                u32 regular_end = funcs->ranges[funcs->count - 1].end;

                fprintf(out, "    // DolRecomp constant-time chunk dispatch.\n");
                fprintf(out, "    if (address >= 0x%08Xu && address < 0x%08Xu && ((address - 0x%08Xu) & 3u) == 0u) {\n",
                        first_start, first_end, first_start);
                fprintf(out, "        func_%08X(ctx);\n", first_start);
                fprintf(out, "        return 1;\n");
                fprintf(out, "    }\n");
                fprintf(out, "    if (address >= 0x%08Xu && address < 0x%08Xu && ((address - 0x%08Xu) & 3u) == 0u) {\n",
                        regular_start, regular_end, regular_start);
                fprintf(out, "        static const DolRecompFunction functions[] = {\n");
                for (u32 i = 1; i < funcs->count; i++) {
                    fprintf(out, "            func_%08X%s\n", funcs->ranges[i].start, (i == funcs->count - 1) ? "" : ",");
                }
                fprintf(out, "        };\n");
                fprintf(out, "        const u32 index = (address - 0x%08Xu) / 0x%Xu;\n", regular_start, stride);
                fprintf(out, "        functions[index](ctx);\n");
                fprintf(out, "        return 1;\n");
                fprintf(out, "    }\n");
            }
        }
    }

    if (!optimized) {
        for (u32 i = 0; i < funcs->count; i++) {
            fprintf(out,
                    "    if (address >= 0x%08Xu && address < 0x%08Xu && "
                    "((address - 0x%08Xu) & 3u) == 0u) { func_%08X(ctx); return 1; }\n",
                    funcs->ranges[i].start, funcs->ranges[i].end,
                    funcs->ranges[i].start, funcs->ranges[i].start);
        }
    }

    fprintf(out, "    return 0;\n");
    fprintf(out, "}\n");
    fprintf(out, "\nstatic inline int dolrecomp_run_blocks(CPUState* ctx, u32 max_blocks) {\n");
    fprintf(out, "    u32 blocks = 0;\n");
    fprintf(out, "    while (max_blocks == 0u || blocks < max_blocks) {\n");
    fprintf(out, "        if (!dolrecomp_call(ctx, ctx->pc)) return 0;\n");
    fprintf(out, "        if (ctx->exception) return 0;\n");
    fprintf(out, "        blocks++;\n");
    fprintf(out, "    }\n");
    fprintf(out, "    return 1;\n");
    fprintf(out, "}\n");
}






int emit_code_sections_split(const LoadedCodeSection* sections,
                                    u32 section_count,
                                    const char* output_path,
                                    DolRecompCPU cpu, u32 entry_point, u32 jobs,
                                    int local_chunks_dir) {
    char stem[1024];
    char header_path[1100];
    char chunks_dir[1100];
    char chunks_label[512];
    char include_name[512];

    if (!make_output_stem(output_path, stem, sizeof(stem)))
        return 0;
    if (!split_include_name(stem, include_name, sizeof(include_name))) {
        fprintf(stderr, "error: output include name is too long\n");
        return 0;
    }

    if (snprintf(header_path, sizeof(header_path), "%s.h", stem) >= (int)sizeof(header_path)) {
        fprintf(stderr, "error: output path is too long\n");
        return 0;
    }

    if (local_chunks_dir) {
        char output_dir[1024];
        if (!path_dirname(output_path, output_dir, sizeof(output_dir)) ||
            !join_path(chunks_dir, sizeof(chunks_dir), output_dir, "chunks")) {
            fprintf(stderr, "error: output path is too long\n");
            return 0;
        }
        snprintf(chunks_label, sizeof(chunks_label), "chunks");
    } else {
        if (snprintf(chunks_dir, sizeof(chunks_dir), "%s_chunks", stem) >= (int)sizeof(chunks_dir)) {
            fprintf(stderr, "error: output path is too long\n");
            return 0;
        }
        snprintf(chunks_label, sizeof(chunks_label), "%s", path_basename(chunks_dir));
    }

    if (!make_dir_tree(chunks_dir))
        return 0;

    FILE* manifest = fopen(output_path, "w");
    if (!manifest) {
        fprintf(stderr, "error: can't open output '%s'\n", output_path);
        return 0;
    }

    FILE* header = fopen(header_path, "w");
    if (!header) {
        fprintf(stderr, "error: can't open output '%s'\n", header_path);
        fclose(manifest);
        return 0;
    }

    fprintf(manifest, "// DolRecomp split output\n");
    fprintf(manifest, "#include \"%s\"\n\n", include_name);
    fprintf(manifest, "// Build these C files too:\n");

    emit_header_for_cpu(header, cpu);
    fprintf(header, "\n// Function entry points\n");

    u32 file_count = 0;
    FunctionList funcs = {0};
    SMCAnalysis smc = {0};

    for (u32 s = 0; s < section_count; s++) {
        const LoadedCodeSection* section = &sections[s];
        if (section->size == 0 || !section->data) continue;

        const u8* section_data = section->data;
        u32 base_addr = section->address;
        u32 section_sz = section->size;
        u32 num_insts = section_sz / 4;

        if (section->name && section->name[0] != '\0') {
            printf("decoding %s[%u] %s: %u instructions at 0x%08X\n",
                   section->label, section->index, section->name, num_insts,
                   base_addr);
        } else {
            printf("decoding %s[%u]: %u instructions at 0x%08X\n",
                   section->label, section->index, num_insts, base_addr);
        }

        PPCInst* insts = (PPCInst*)malloc(num_insts * sizeof(PPCInst));
        if (!insts) {
            fprintf(stderr, "error: out of memory\n");
            smc_analysis_free(&smc);
            function_list_free(&funcs);
            fclose(header);
            fclose(manifest);
            return 0;
        }

        u32 decoded = 0, embedded = 0, unknown = 0;
        for (u32 i = 0; i < num_insts; i++) {
            u32 raw = read_be32(section_data + i * 4);
            u32 addr = base_addr + i * 4;
            insts[i] = ppc_decode(raw, addr);
            if (insts[i].op == PPC_OP_UNKNOWN &&
                embedded_data_word(section->embedded_data_mode, raw)) {
                insts[i].embedded_data = true;
            }
            decoded++;
            if (insts[i].embedded_data) {
                embedded++;
            } else if (insts[i].op == PPC_OP_UNKNOWN) {
                unknown++;
            }
        }

        if (embedded != 0) {
            printf("  %u decoded, %u known, %u embedded data, %u unknown\n",
                   decoded, decoded - embedded - unknown, embedded, unknown);
        } else {
            printf("  %u decoded, %u known, %u unknown\n",
                   decoded, decoded - unknown, unknown);
        }

        if (section->embedded_data_mode == EMBEDDED_DATA_DOL) {
            analyze_smc_section(sections, section_count, insts, num_insts, &smc);
            if (smc.allocation_failed) {
                fprintf(stderr, "error: out of memory\n");
                smc_analysis_free(&smc);
                function_list_free(&funcs);
                free(insts);
                fclose(header);
                fclose(manifest);
                return 0;
            }
        }

        u32 section_job_count =
            (num_insts + EMIT_CHUNK_INSTRUCTIONS - 1u) / EMIT_CHUNK_INSTRUCTIONS;
        ChunkJob* chunk_jobs = (ChunkJob*)calloc(section_job_count, sizeof(ChunkJob));
        if (!chunk_jobs) {
            fprintf(stderr, "error: out of memory\n");
            smc_analysis_free(&smc);
            function_list_free(&funcs);
            free(insts);
            fclose(header);
            fclose(manifest);
            return 0;
        }

        for (u32 start = 0; start < num_insts; start += EMIT_CHUNK_INSTRUCTIONS) {
            u32 chunk_count = num_insts - start;
            u32 func_addr = base_addr + start * 4u;
            char chunk_name[128];
            u32 job_index = start / EMIT_CHUNK_INSTRUCTIONS;

            if (chunk_count > EMIT_CHUNK_INSTRUCTIONS)
                chunk_count = EMIT_CHUNK_INSTRUCTIONS;

            if (snprintf(chunk_name, sizeof(chunk_name),
                         "chunk_%04u_%s%u_%08X.c", file_count,
                         section->label, section->index, func_addr) >=
                (int)sizeof(chunk_name)) {
                fprintf(stderr, "error: chunk name is too long\n");
                smc_analysis_free(&smc);
                function_list_free(&funcs);
                free(chunk_jobs);
                free(insts);
                fclose(header);
                fclose(manifest);
                return 0;
            }

            ChunkJob* job = &chunk_jobs[job_index];
            job->insts = insts + start;
            job->count = chunk_count;
            job->func_addr = func_addr;

            if (!join_path(job->path, sizeof(job->path), chunks_dir, chunk_name)) {
                fprintf(stderr, "error: chunk path is too long\n");
                smc_analysis_free(&smc);
                function_list_free(&funcs);
                free(chunk_jobs);
                free(insts);
                fclose(header);
                fclose(manifest);
                return 0;
            }

            if (snprintf(job->include_name, sizeof(job->include_name), "%s",
                         include_name) >= (int)sizeof(job->include_name)) {
                fprintf(stderr, "error: output include name is too long\n");
                smc_analysis_free(&smc);
                function_list_free(&funcs);
                free(chunk_jobs);
                free(insts);
                fclose(header);
                fclose(manifest);
                return 0;
            }

            emit_chunk_prototype(header, func_addr);
            if (!function_list_add(&funcs, func_addr, func_addr + chunk_count * 4u)) {
                smc_analysis_free(&smc);
                function_list_free(&funcs);
                free(chunk_jobs);
                free(insts);
                fclose(header);
                fclose(manifest);
                return 0;
            }
            fprintf(manifest, "// %s/%s\n", chunks_label, chunk_name);
            file_count++;
        }

        u32 active_jobs = effective_chunk_jobs(section_job_count, jobs);
        printf("  writing %u chunks with %u job%s\n",
               section_job_count, active_jobs, active_jobs == 1 ? "" : "s");
        if (!run_chunk_jobs(chunk_jobs, section_job_count, jobs)) {
            smc_analysis_free(&smc);
            function_list_free(&funcs);
            free(chunk_jobs);
            free(insts);
            fclose(header);
            fclose(manifest);
            return 0;
        }

        free(chunk_jobs);
        free(insts);
    }

    if (smc.possible) {
        u32 display_count = smc.range_count;
        char smc_report_path[1100];
        if (display_count > SMC_DISPLAY_RANGE_LIMIT)
            display_count = SMC_DISPLAY_RANGE_LIMIT;

        printf("warning: this DOL may patch executable memory at runtime. generated code many need additional patches\n");
        printf("  possible patching instructions:\n");
        for (u32 i = 0; i < display_count; i++) {
            printf("    0x%08X-0x%08X\n", smc.ranges[i].start, smc.ranges[i].end);
        }

        if (smc.range_count > SMC_DISPLAY_RANGE_LIMIT) {
            if (snprintf(smc_report_path, sizeof(smc_report_path), "%s_smc.txt", stem) >=
                (int)sizeof(smc_report_path)) {
                fprintf(stderr, "error: SMC report path is too long\n");
                smc_analysis_free(&smc);
                function_list_free(&funcs);
                fclose(header);
                fclose(manifest);
                return 0;
            }
            if (!write_smc_report(&smc, smc_report_path)) {
                smc_analysis_free(&smc);
                function_list_free(&funcs);
                fclose(header);
                fclose(manifest);
                return 0;
            }
            printf("    ...\n");
            printf("  full list: %s\n", smc_report_path);
        }
    }

    emit_dispatch_helpers(header, &funcs, entry_point);
    emit_footer(header);
    smc_analysis_free(&smc);
    function_list_free(&funcs);
    fprintf(manifest, "\n// %u C files\n", file_count);

    fclose(header);
    fclose(manifest);

    printf("done!\n");
    printf("  header: %s\n", header_path);
    printf("  chunks: %s (%u files)\n", chunks_dir, file_count);
    return 1;
}



int main(int argc, char** argv) {
    if (argc > 1 && strcmp(argv[1], "extract") == 0)
        return disc_extract_main(argc - 1, argv + 1);

    CliOptions opts;
    if (!parse_cli(argc, argv, &opts))
        return 1;
    if (opts.show_help)
        return 0;
    if (opts.setup_mode)
        return run_setup() ? 0 : 1;

    const char* input_path  = opts.input_path;
    const char* title_id_arg = opts.title_id_arg;
    const char* output_arg = opts.output_arg;
    char title_id[64];
    char game_name[256];
    char named_output_path[1200];
    const char* output_path = NULL;
    int local_chunks_dir = 0;
    int effective_gamecube_mode = opts.gamecube_mode;
    DolRecompCPU effective_cpu = opts.cpu;
    int titleless_mode = effective_gamecube_mode || effective_cpu == DOLRECOMP_CPU_ESPRESSO;
    int espresso_rpx_mode = effective_cpu == DOLRECOMP_CPU_ESPRESSO;
    int input_is_directory = path_is_directory(input_path);
    int rel_mode = has_rel_extension(input_path) || input_is_directory;
    u32 rel_start_base = opts.rel_base_set ? opts.rel_base : REL_AUTO_BASE;

    title_id[0] = '\0';
    game_name[0] = '\0';

    if (has_rpx_extension(input_path) && effective_cpu != DOLRECOMP_CPU_ESPRESSO) {
        fprintf(stderr, "error: .rpx input requires --cpu espresso\n");
        return 1;
    }
    if (rel_mode && effective_cpu == DOLRECOMP_CPU_ESPRESSO) {
        fprintf(stderr, "error: .rel input cannot use espresso mode\n");
        return 1;
    }
    if (!titleless_mode && !database_titles_available()) {
        print_database_missing_notice();
        effective_gamecube_mode = 1;
        titleless_mode = 1;
        if (!output_arg && title_id_arg && !is_title_id_length_valid(title_id_arg))
            output_arg = title_id_arg;
        title_id_arg = NULL;
    }

    if (!titleless_mode) {
        if (!title_id_arg) {
            fprintf(stderr, "title id missing! specify gamecube mode if working with a gamecube dol with --gamecube\n");
            return 1;
        }
        if (!is_title_id_length_valid(title_id_arg)) {
            fprintf(stderr, "title id length is invalid\n");
            return 1;
        }
        if (!is_title_id(title_id_arg)) {
            fprintf(stderr, "error: title id must contain only letters/numbers, like SUKE01\n");
            return 1;
        }
    }

    if (!opts.cpu_explicit)
        effective_cpu = effective_gamecube_mode ? DOLRECOMP_CPU_GEKKO : DOLRECOMP_CPU_BROADWAY;
    espresso_rpx_mode = effective_cpu == DOLRECOMP_CPU_ESPRESSO;

    if (effective_gamecube_mode) {
        snprintf(game_name, sizeof(game_name), rel_mode ? "GameCube REL" : "GameCube DOL");
    } else if (effective_cpu == DOLRECOMP_CPU_ESPRESSO) {
        snprintf(game_name, sizeof(game_name), "Wii U executable");
    } else {
        copy_title_id(title_id, sizeof(title_id), title_id_arg);
        describe_game(game_name, sizeof(game_name), title_id, 0);
    }

    if (espresso_rpx_mode) {
        if (!has_rpx_extension(input_path)) {
            fprintf(stderr, "error: espresso mode expects an .rpx input\n");
            return 1;
        }

        RPXFile rpx;
        if (!rpx_load(&rpx, input_path))
            return 1;

        rpx_print_info(&rpx, game_name);
        printf("cpu: %s\n", cpu_display_name(effective_cpu));

        if (!output_arg) {
            printf("\ngenerating code...\n");
            output_arg = ".";
        }

        if (has_c_extension(output_arg)) {
            output_path = output_arg;
        } else {
            if (!build_gamecube_output_path(output_arg, named_output_path,
                                            sizeof(named_output_path))) {
                rpx_free(&rpx);
                return 1;
            }
            output_path = named_output_path;
            local_chunks_dir = 1;
        }

        printf("\nwriting output to: %s\n", output_path);
        if (!emit_rpx_split(&rpx, output_path, effective_cpu, opts.jobs,
                            local_chunks_dir)) {
            rpx_free(&rpx);
            return 1;
        }

        rpx_free(&rpx);
        return 0;
    }

    if (input_is_directory) {
        if (has_c_extension(output_arg ? output_arg : "")) {
            fprintf(stderr, "error: REL directory output must be a directory\n");
            return 1;
        }
        if (!output_arg) {
            printf("\ngenerating code...\n");
            output_arg = ".";
        }

        printf("REL base start: 0x%08X\n", rel_start_base);
        if (!emit_rel_directory(input_path, output_arg, title_id,
                                titleless_mode, effective_cpu, opts.jobs,
                                rel_start_base)) {
            return 1;
        }
        return 0;
    }

    if (rel_mode) {
        RELFile rel;
        if (!rel_load(&rel, input_path, rel_start_base))
            return 1;

        rel_print_info(&rel, game_name);
        printf("cpu: %s\n", cpu_display_name(effective_cpu));

        if (!output_arg) {
            printf("\ngenerating code...\n");
            output_arg = ".";
        }

        if (has_c_extension(output_arg)) {
            output_path = output_arg;
        } else if (titleless_mode) {
            if (!build_gamecube_output_path(output_arg, named_output_path,
                                            sizeof(named_output_path))) {
                rel_free(&rel);
                return 1;
            }
            output_path = named_output_path;
            local_chunks_dir = 1;
        } else {
            if (!build_named_output_path(output_arg, title_id,
                                         named_output_path, sizeof(named_output_path))) {
                rel_free(&rel);
                return 1;
            }
            output_path = named_output_path;
            local_chunks_dir = 1;
        }

        printf("\nwriting output to: %s\n", output_path);
        if (!emit_rel_split(&rel, output_path, effective_cpu, opts.jobs,
                            local_chunks_dir)) {
            rel_free(&rel);
            return 1;
        }

        rel_free(&rel);
        return 0;
    }

    DOLFile dol;
    if (!dol_load(&dol, input_path))
        return 1;
    dol_print_info(&dol, game_name);
    printf("cpu: %s\n", cpu_display_name(effective_cpu));

    if (!output_arg) {
        printf("\ngenerating code...\n");
        output_arg = ".";
    }

    if (has_c_extension(output_arg)) {
        output_path = output_arg;
    } else if (titleless_mode) {
        if (!build_gamecube_output_path(output_arg, named_output_path,
                                        sizeof(named_output_path))) {
            dol_free(&dol);
            return 1;
        }
        output_path = named_output_path;
        local_chunks_dir = 1;
    } else {
        if (!build_named_output_path(output_arg, title_id,
                                     named_output_path, sizeof(named_output_path))) {
            dol_free(&dol);
            return 1;
        }
        output_path = named_output_path;
        local_chunks_dir = 1;
    }

    printf("\nwriting output to: %s\n", output_path);
    if (!emit_dol_split(&dol, output_path, effective_cpu, opts.jobs, local_chunks_dir)) {
        dol_free(&dol);
        return 1;
    }

    dol_free(&dol);
    return 0;
}
