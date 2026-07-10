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

#define EMIT_CHUNK_INSTRUCTIONS 4096u
#define REL_AUTO_BASE 0x80500000u
#define REL_AUTO_ALIGN 0x10000u
#define DATABASE_DIR "database"
#define DATABASE_TITLES_FILE "titles.txt"
#define DATABASE_SETUP_FLAG ".setup_done.flag"
#define GAMETDB_TITLES_URL "https://www.gametdb.com/titles.txt?LANG=EN"
#define EXTERN_DIR "extern"
#define WIT_DIR "extern/wit"
#define WIT_BIN_DIR "extern/wit/bin"

#ifdef _WIN32
#define WIT_DOWNLOAD_URL "https://wit.wiimm.de/download/wit-v3.05a-r8638-cygwin64.zip"
#define WIT_ARCHIVE_NAME "wit-v3.05a-r8638-cygwin64.zip"
#define WIT_EXE_NAME "wit.exe"
#elif defined(__APPLE__)
#define WIT_DOWNLOAD_URL "https://wit.wiimm.de/download/wit-v3.05a-r8638-mac.tar.gz"
#define WIT_ARCHIVE_NAME "wit-v3.05a-r8638-mac.tar.gz"
#define WIT_EXE_NAME "wit"
#else
#define WIT_DOWNLOAD_URL "https://wit.wiimm.de/download/wit-v3.05a-r8638-x86_64.tar.gz"
#define WIT_ARCHIVE_NAME "wit-v3.05a-r8638-x86_64.tar.gz"
#define WIT_EXE_NAME "wit"
#endif

static void print_usage(const char* argv0) {
    (void)argv0;

    fprintf(stderr, "usage: dolrecomp.exe [-jN] [--cpu gekko|broadway|espresso] [--gamecube] <input> [wii-title-id] [output.c | output-dir]\n");
    fprintf(stderr, "\n");
    fprintf(stderr, "extract:  dolrecomp.exe extract game.iso output_folder\n");
    fprintf(stderr, "extract:  dolrecomp.exe extract game.wbfs output_folder\n");
    fprintf(stderr, "wii:      dolrecomp.exe <input.dol> SUKE01 build\n");
    fprintf(stderr, "gamecube: dolrecomp.exe --gamecube <input.dol> build\n");
    fprintf(stderr, "rel:      dolrecomp.exe <input.rel | rel_folder> SUKE01 build\n");
    fprintf(stderr, "wii u cpu: dolrecomp.exe --cpu espresso <input.rpx> build\n");
    fprintf(stderr, "with output.c: writes that split C set\n");
    fprintf(stderr, "with output-dir: writes output-dir/<wii-title-id>_generated/<wii-title-id>.c\n");
    fprintf(stderr, "with --gamecube or --cpu espresso output-dir: writes output-dir/generated/generated.c\n");
    fprintf(stderr, "-jN writes split C files with N jobs, like -j14\n");
    fprintf(stderr, "--rel-base optionally sets the first virtual load address used for REL codegen\n");
    fprintf(stderr, "--setup downloads database/titles.txt and can install wit tools\n");
    fprintf(stderr, "without output: writes generated code under the current directory\n");
}

static int write_setup_flag(const char* path) {
#ifdef _WIN32
    SetFileAttributesA(path, FILE_ATTRIBUTE_NORMAL);
#endif

    FILE* file = fopen(path, "w");
    if (!file) {
        fprintf(stderr, "error: can't write setup flag '%s'\n", path);
        return 0;
    }

    fprintf(file, "done\n");
    if (fclose(file) != 0) {
        fprintf(stderr, "error: can't finish setup flag '%s'\n", path);
        return 0;
    }

#ifdef _WIN32
    SetFileAttributesA(path, FILE_ATTRIBUTE_HIDDEN);
#endif
    return 1;
}

static int download_titles_database(const char* output_path) {
    char temp_path[1100];
    if (snprintf(temp_path, sizeof(temp_path), "%s.tmp", output_path) >=
        (int)sizeof(temp_path)) {
        fprintf(stderr, "error: database path is too long\n");
        return 0;
    }

#ifdef _WIN32
    const char* command =
        "curl.exe -fsSL --max-time 60 \"" GAMETDB_TITLES_URL "\"";
#else
    const char* command =
        "curl -fsSL --max-time 60 '" GAMETDB_TITLES_URL "'";
#endif

    FILE* pipe = popen(command, "r");
    if (!pipe) {
        fprintf(stderr, "error: can't start curl\n");
        return 0;
    }

    FILE* out = fopen(temp_path, "wb");
    if (!out) {
        pclose(pipe);
        fprintf(stderr, "error: can't write '%s'\n", temp_path);
        return 0;
    }

    unsigned char buffer[8192];
    size_t total = 0;
    for (;;) {
        size_t got = fread(buffer, 1, sizeof(buffer), pipe);
        if (got > 0) {
            if (fwrite(buffer, 1, got, out) != got) {
                fclose(out);
                pclose(pipe);
                remove(temp_path);
                fprintf(stderr, "error: failed writing '%s'\n", temp_path);
                return 0;
            }
            total += got;
        }
        if (got < sizeof(buffer)) {
            if (feof(pipe))
                break;
            if (ferror(pipe)) {
                fclose(out);
                pclose(pipe);
                remove(temp_path);
                fprintf(stderr, "error: failed reading titles database\n");
                return 0;
            }
        }
    }

    if (fclose(out) != 0) {
        pclose(pipe);
        remove(temp_path);
        fprintf(stderr, "error: failed writing '%s'\n", temp_path);
        return 0;
    }

    int rc = pclose(pipe);
    if (rc != 0 || total == 0) {
        remove(temp_path);
        fprintf(stderr, "error: failed downloading GameTDB titles\n");
        return 0;
    }

    remove(output_path);
    if (rename(temp_path, output_path) != 0) {
        remove(temp_path);
        fprintf(stderr, "error: can't install '%s'\n", output_path);
        return 0;
    }

    return 1;
}

static int local_wit_path(char* out, size_t out_size) {
    char path[1024];

    if (join_path(path, sizeof(path), WIT_BIN_DIR, WIT_EXE_NAME) &&
        file_exists(path)) {
        return snprintf(out, out_size, "%s", path) > 0 &&
               strlen(out) < out_size;
    }

    if (join_path(path, sizeof(path), WIT_DIR, WIT_EXE_NAME) &&
        file_exists(path)) {
        return snprintf(out, out_size, "%s", path) > 0 &&
               strlen(out) < out_size;
    }

    return 0;
}

static int command_exists_quiet(const char* command_name) {
    char* quoted = shell_quote_arg(command_name);
    if (!quoted)
        return 0;

    char command[1200];
#ifdef _WIN32
    int written = snprintf(command, sizeof(command), "%s --version >NUL 2>NUL",
                           quoted);
#else
    int written = snprintf(command, sizeof(command), "%s --version >/dev/null 2>/dev/null",
                           quoted);
#endif
    free(quoted);
    return written > 0 && (size_t)written < sizeof(command) &&
           run_shell_command(command);
}

static int wit_tools_available(void) {
    char path[1024];
    if (local_wit_path(path, sizeof(path)))
        return 1;
    return command_exists_quiet("wit");
}

static int prompt_yes_no(const char* prompt) {
    char line[32];
    printf("%s", prompt);
    fflush(stdout);

    if (!fgets(line, sizeof(line), stdin))
        return 0;
    return line[0] == 'y' || line[0] == 'Y';
}

static int download_file(const char* url, const char* output_path) {
    char temp_path[1200];
    if (snprintf(temp_path, sizeof(temp_path), "%s.tmp", output_path) >=
        (int)sizeof(temp_path)) {
        fprintf(stderr, "error: download path is too long\n");
        return 0;
    }

    char* qurl = shell_quote_arg(url);
    char* qout = shell_quote_arg(temp_path);
    if (!qurl || !qout) {
        free(qurl);
        free(qout);
        fprintf(stderr, "error: out of memory\n");
        return 0;
    }

    char command[2600];
#ifdef _WIN32
    int written = snprintf(command, sizeof(command),
                           "curl.exe -fL --max-time 300 -o %s %s",
                           qout, qurl);
#else
    int written = snprintf(command, sizeof(command),
                           "curl -fL --max-time 300 -o %s %s",
                           qout, qurl);
#endif
    free(qurl);
    free(qout);

    if (written <= 0 || (size_t)written >= sizeof(command)) {
        fprintf(stderr, "error: download command is too long\n");
        return 0;
    }

    remove(temp_path);
    if (!run_shell_command(command)) {
        remove(temp_path);
        fprintf(stderr, "error: download failed\n");
        return 0;
    }

    remove(output_path);
    if (rename(temp_path, output_path) != 0) {
        remove(temp_path);
        fprintf(stderr, "error: can't install '%s'\n", output_path);
        return 0;
    }

    return 1;
}

#ifdef _WIN32
static void write_ps_quoted(FILE* file, const char* text) {
    fputc('\'', file);
    for (size_t i = 0; text[i] != '\0'; i++) {
        if (text[i] == '\'')
            fputs("''", file);
        else
            fputc(text[i], file);
    }
    fputc('\'', file);
}

static int write_wit_install_script(const char* script_path,
                                    const char* archive_path) {
    FILE* file = fopen(script_path, "w");
    if (!file) {
        fprintf(stderr, "error: can't write '%s'\n", script_path);
        return 0;
    }

    fprintf(file, "$ErrorActionPreference = 'Stop'\n");
    fprintf(file, "$archive = ");
    write_ps_quoted(file, archive_path);
    fprintf(file, "\n$root = ");
    write_ps_quoted(file, WIT_DIR);
    fprintf(file, "\n$unpack = Join-Path $root 'unpack'\n");
    fprintf(file, "$bin = ");
    write_ps_quoted(file, WIT_BIN_DIR);
    fprintf(file, "\nRemove-Item -LiteralPath $unpack -Recurse -Force -ErrorAction SilentlyContinue\n");
    fprintf(file, "Remove-Item -LiteralPath $bin -Recurse -Force -ErrorAction SilentlyContinue\n");
    fprintf(file, "New-Item -ItemType Directory -Force -Path $unpack | Out-Null\n");
    fprintf(file, "New-Item -ItemType Directory -Force -Path $bin | Out-Null\n");
    fprintf(file, "Expand-Archive -LiteralPath $archive -DestinationPath $unpack -Force\n");
    fprintf(file, "$wit = Get-ChildItem -LiteralPath $unpack -Recurse -Filter 'wit.exe' | Select-Object -First 1\n");
    fprintf(file, "if (-not $wit) { exit 2 }\n");
    fprintf(file, "Copy-Item -Path (Join-Path $wit.Directory.FullName '*') -Destination $bin -Recurse -Force\n");
    fprintf(file, "Remove-Item -LiteralPath $unpack -Recurse -Force -ErrorAction SilentlyContinue\n");

    if (fclose(file) != 0) {
        fprintf(stderr, "error: failed writing '%s'\n", script_path);
        return 0;
    }
    return 1;
}

static int extract_wit_archive(const char* archive_path) {
    char script_path[1200];
    if (join_path(script_path, sizeof(script_path), WIT_DIR, "install_wit.ps1") == 0) {
        fprintf(stderr, "error: setup path is too long\n");
        return 0;
    }

    if (!write_wit_install_script(script_path, archive_path))
        return 0;

    char* qscript = shell_quote_arg(script_path);
    if (!qscript) {
        fprintf(stderr, "error: out of memory\n");
        return 0;
    }

    char command[1800];
    int written = snprintf(command, sizeof(command),
                           "powershell.exe -NoProfile -ExecutionPolicy Bypass -File %s",
                           qscript);
    free(qscript);

    if (written <= 0 || (size_t)written >= sizeof(command)) {
        fprintf(stderr, "error: extract command is too long\n");
        return 0;
    }

    if (!run_shell_command(command)) {
        fprintf(stderr, "error: failed extracting wit tools\n");
        return 0;
    }

    remove(script_path);
    return 1;
}
#else
static int extract_wit_archive(const char* archive_path) {
    char* qarchive = shell_quote_arg(archive_path);
    char* qroot = shell_quote_arg(WIT_DIR);
    char* qbin = shell_quote_arg(WIT_BIN_DIR);
    if (!qarchive || !qroot || !qbin) {
        free(qarchive);
        free(qroot);
        free(qbin);
        fprintf(stderr, "error: out of memory\n");
        return 0;
    }

    char command[3000];
    int written = snprintf(command, sizeof(command),
        "rm -rf %s/unpack %s && mkdir -p %s/unpack %s && "
        "tar -xzf %s -C %s/unpack && "
        "found=$(find %s/unpack -type f -name wit | head -n 1) && "
        "test -n \"$found\" && cp -R \"$(dirname \"$found\")\"/. %s && "
        "chmod +x %s/wit && rm -rf %s/unpack",
        qroot, qbin, qroot, qbin, qarchive, qroot, qroot, qbin, qbin, qroot);

    free(qarchive);
    free(qroot);
    free(qbin);

    if (written <= 0 || (size_t)written >= sizeof(command)) {
        fprintf(stderr, "error: extract command is too long\n");
        return 0;
    }

    if (!run_shell_command(command)) {
        fprintf(stderr, "error: failed extracting wit tools\n");
        return 0;
    }
    return 1;
}
#endif

static int install_wit_tools(void) {
    char archive_path[1200];

    if (!make_dir_tree(EXTERN_DIR) || !make_dir_tree(WIT_DIR))
        return 0;

    if (!join_path(archive_path, sizeof(archive_path), WIT_DIR,
                   WIT_ARCHIVE_NAME)) {
        fprintf(stderr, "error: wit path is too long\n");
        return 0;
    }

    printf("downloading Wiimms ISO Tools...\n");
    if (!download_file(WIT_DOWNLOAD_URL, archive_path))
        return 0;

    printf("installing Wiimms ISO Tools...\n");
    if (!extract_wit_archive(archive_path))
        return 0;

    char wit_path[1024];
    if (!local_wit_path(wit_path, sizeof(wit_path))) {
        fprintf(stderr, "error: wit install did not produce %s\n", WIT_EXE_NAME);
        return 0;
    }

    printf("  wit:    %s\n", wit_path);
    return 1;
}

static int setup_wit_tools(void) {
    if (wit_tools_available()) {
        printf("wit tools found.\n");
        return 1;
    }

    if (!prompt_yes_no("wit tools missing, download? [Y/N] ")) {
        printf("skipping wit tools.\n");
        return 1;
    }

    return install_wit_tools();
}

static int run_setup(void) {
    char titles_path[1024];
    char flag_path[1024];

    if (!make_dir_tree(DATABASE_DIR))
        return 0;
    if (!database_titles_path(titles_path, sizeof(titles_path)) ||
        !database_path(flag_path, sizeof(flag_path), DATABASE_SETUP_FLAG)) {
        fprintf(stderr, "error: database path is too long\n");
        return 0;
    }

    printf("setting up database...\n");
    printf("downloading GameTDB titles.txt...\n");
    if (!download_titles_database(titles_path))
        return 0;

    if (!write_setup_flag(flag_path))
        return 0;

    if (!setup_wit_tools())
        return 0;

    printf("done!\n");
    printf("  titles: %s\n", titles_path);
    printf("  flag:   %s\n", flag_path);
    return 1;
}

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




typedef struct {
    const char* input_path;
    const char* title_id_arg;
    const char* output_arg;
    DolRecompCPU cpu;
    u32 jobs;
    u32 rel_base;
    int gamecube_mode;
    int cpu_explicit;
    int rel_base_set;
    int setup_mode;
    int show_help;
} CliOptions;

static int parse_job_count(const char* text, u32* jobs) {
    char* end = NULL;
    errno = 0;
    unsigned long value = strtoul(text, &end, 10);
    if (errno != 0 || !end || *end != '\0' || value == 0 || value > 256) {
        fprintf(stderr, "error: job count must be 1..256\n");
        return 0;
    }

    *jobs = (u32)value;
    return 1;
}

static int parse_u32_arg(const char* text, const char* name, u32* value_out) {
    char* end = NULL;
    errno = 0;
    unsigned long value = strtoul(text, &end, 0);
    if (errno != 0 || !end || *end != '\0' || value > 0xFFFFFFFFul) {
        fprintf(stderr, "error: %s must be a 32-bit address\n", name);
        return 0;
    }

    *value_out = (u32)value;
    return 1;
}

static int parse_cli(int argc, char** argv, CliOptions* opts) {
    const char* positional[3];
    int positional_count = 0;

    memset(opts, 0, sizeof(*opts));
    opts->cpu = DOLRECOMP_CPU_GEKKO;
    opts->jobs = 1;

    for (int i = 1; i < argc; i++) {
        const char* arg = argv[i];

        if (strcmp(arg, "--help") == 0 || strcmp(arg, "-h") == 0) {
            print_usage(argv[0]);
            opts->show_help = 1;
            return 1;
        }

        if (strcmp(arg, "--setup") == 0) {
            opts->setup_mode = 1;
            continue;
        }

        if (strcmp(arg, "--gamecube") == 0 || strcmp(arg, "-gc") == 0) {
            opts->gamecube_mode = 1;
            continue;
        }

        if (strcmp(arg, "--cpu") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "error: --cpu needs gekko, broadway, or espresso\n");
                return 0;
            }
            if (!parse_cpu_name(argv[++i], &opts->cpu)) {
                fprintf(stderr, "error: unknown cpu '%s'\n", argv[i]);
                return 0;
            }
            opts->cpu_explicit = 1;
            continue;
        }

        if (strncmp(arg, "--cpu=", 6) == 0) {
            if (!parse_cpu_name(arg + 6, &opts->cpu)) {
                fprintf(stderr, "error: unknown cpu '%s'\n", arg + 6);
                return 0;
            }
            opts->cpu_explicit = 1;
            continue;
        }

        if (strcmp(arg, "--gekko") == 0) {
            opts->cpu = DOLRECOMP_CPU_GEKKO;
            opts->cpu_explicit = 1;
            continue;
        }

        if (strcmp(arg, "--broadway") == 0) {
            opts->cpu = DOLRECOMP_CPU_BROADWAY;
            opts->cpu_explicit = 1;
            continue;
        }

        if (strcmp(arg, "--espresso") == 0 || strcmp(arg, "--wiiu-cpu") == 0) {
            opts->cpu = DOLRECOMP_CPU_ESPRESSO;
            opts->cpu_explicit = 1;
            continue;
        }

        if (strcmp(arg, "-j") == 0 || strcmp(arg, "--jobs") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "error: -j needs a number\n");
                return 0;
            }
            if (!parse_job_count(argv[++i], &opts->jobs))
                return 0;
            continue;
        }

        if (strncmp(arg, "-j", 2) == 0 && arg[2] != '\0') {
            if (!parse_job_count(arg + 2, &opts->jobs))
                return 0;
            continue;
        }

        if (strncmp(arg, "--jobs=", 7) == 0) {
            if (!parse_job_count(arg + 7, &opts->jobs))
                return 0;
            continue;
        }

        if (strcmp(arg, "--rel-base") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "error: --rel-base needs an address\n");
                return 0;
            }
            if (!parse_u32_arg(argv[++i], "--rel-base", &opts->rel_base))
                return 0;
            opts->rel_base_set = 1;
            continue;
        }

        if (strncmp(arg, "--rel-base=", 11) == 0) {
            if (!parse_u32_arg(arg + 11, "--rel-base", &opts->rel_base))
                return 0;
            opts->rel_base_set = 1;
            continue;
        }

        if (arg[0] == '-' && arg[1] != '\0') {
            fprintf(stderr, "error: unknown option '%s'\n", arg);
            return 0;
        }

        if (positional_count >= 3) {
            print_usage(argv[0]);
            return 0;
        }
        positional[positional_count++] = arg;
    }

    if (positional_count == 0) {
        if (opts->setup_mode)
            return 1;
        print_usage(argv[0]);
        return 0;
    }

    if (opts->setup_mode) {
        print_usage(argv[0]);
        return 0;
    }

    if (opts->gamecube_mode && opts->cpu == DOLRECOMP_CPU_ESPRESSO) {
        fprintf(stderr, "error: --gamecube cannot be used with espresso\n");
        return 0;
    }

    opts->input_path = positional[0];

    if (opts->gamecube_mode || opts->cpu == DOLRECOMP_CPU_ESPRESSO) {
        opts->title_id_arg = "generated";
        opts->output_arg = positional_count > 1 ? positional[1] : NULL;
        if (positional_count > 2) {
            print_usage(argv[0]);
            return 0;
        }
    } else {
        opts->title_id_arg = positional_count > 1 ? positional[1] : NULL;
        opts->output_arg = positional_count > 2 ? positional[2] : NULL;
    }

    return 1;
}

typedef struct {
    const PPCInst* insts;
    u32 count;
    u32 func_addr;
    char path[1200];
    char include_name[512];
} ChunkJob;

typedef struct {
    const ChunkJob* jobs;
    u32 job_count;
    u32 next_job;
    int failed;
#ifdef _WIN32
    CRITICAL_SECTION lock;
#else
    pthread_mutex_t lock;
#endif
} WorkerQueue;

static int emit_chunk_file(const ChunkJob* job) {
    FILE* chunk = fopen(job->path, "w");
    if (!chunk) {
        fprintf(stderr, "error: can't open output '%s'\n", job->path);
        return 0;
    }

    fprintf(chunk, "// DolRecomp output\n");
    fprintf(chunk, "#include \"../%s\"\n\n", job->include_name);
    emit_function(chunk, job->insts, job->count, job->func_addr);

    if (fclose(chunk) != 0) {
        fprintf(stderr, "error: failed writing '%s'\n", job->path);
        return 0;
    }

    return 1;
}

static int queue_init(WorkerQueue* queue, const ChunkJob* jobs, u32 job_count) {
    queue->jobs = jobs;
    queue->job_count = job_count;
    queue->next_job = 0;
    queue->failed = 0;
#ifdef _WIN32
    InitializeCriticalSection(&queue->lock);
    return 1;
#else
    return pthread_mutex_init(&queue->lock, NULL) == 0;
#endif
}

static void queue_destroy(WorkerQueue* queue) {
#ifdef _WIN32
    DeleteCriticalSection(&queue->lock);
#else
    pthread_mutex_destroy(&queue->lock);
#endif
}

static void queue_lock(WorkerQueue* queue) {
#ifdef _WIN32
    EnterCriticalSection(&queue->lock);
#else
    pthread_mutex_lock(&queue->lock);
#endif
}

static void queue_unlock(WorkerQueue* queue) {
#ifdef _WIN32
    LeaveCriticalSection(&queue->lock);
#else
    pthread_mutex_unlock(&queue->lock);
#endif
}

static int queue_take_job(WorkerQueue* queue, const ChunkJob** job) {
    int ok = 0;

    queue_lock(queue);
    if (!queue->failed && queue->next_job < queue->job_count) {
        *job = &queue->jobs[queue->next_job++];
        ok = 1;
    }
    queue_unlock(queue);

    return ok;
}

static void queue_mark_failed(WorkerQueue* queue) {
    queue_lock(queue);
    queue->failed = 1;
    queue_unlock(queue);
}

#ifdef _WIN32
static DWORD WINAPI chunk_worker_main(LPVOID arg) {
#else
static void* chunk_worker_main(void* arg) {
#endif
    WorkerQueue* queue = (WorkerQueue*)arg;
    const ChunkJob* job = NULL;

    while (queue_take_job(queue, &job)) {
        if (!emit_chunk_file(job)) {
            queue_mark_failed(queue);
            break;
        }
    }

#ifdef _WIN32
    return 0;
#else
    return NULL;
#endif
}

static int run_chunk_jobs(const ChunkJob* jobs, u32 job_count, u32 requested_jobs) {
    if (job_count == 0)
        return 1;

    if (requested_jobs == 0)
        requested_jobs = 1;
    if (requested_jobs > job_count)
        requested_jobs = job_count;
#ifdef _WIN32
    if (requested_jobs > 64)
        requested_jobs = 64;
#endif

    if (requested_jobs == 1) {
        for (u32 i = 0; i < job_count; i++) {
            if (!emit_chunk_file(&jobs[i]))
                return 0;
        }
        return 1;
    }

    WorkerQueue queue;
    if (!queue_init(&queue, jobs, job_count)) {
        fprintf(stderr, "error: can't start worker queue\n");
        return 0;
    }

#ifdef _WIN32
    HANDLE* handles = (HANDLE*)calloc(requested_jobs, sizeof(HANDLE));
    if (!handles) {
        queue_destroy(&queue);
        fprintf(stderr, "error: out of memory\n");
        return 0;
    }

    u32 created = 0;
    for (; created < requested_jobs; created++) {
        handles[created] = CreateThread(NULL, 0, chunk_worker_main, &queue, 0, NULL);
        if (!handles[created]) {
            queue_mark_failed(&queue);
            fprintf(stderr, "error: can't start worker thread\n");
            break;
        }
    }

    if (created > 0)
        WaitForMultipleObjects(created, handles, TRUE, INFINITE);

    for (u32 i = 0; i < created; i++)
        CloseHandle(handles[i]);
    free(handles);
#else
    pthread_t* threads = (pthread_t*)calloc(requested_jobs, sizeof(pthread_t));
    if (!threads) {
        queue_destroy(&queue);
        fprintf(stderr, "error: out of memory\n");
        return 0;
    }

    u32 created = 0;
    for (; created < requested_jobs; created++) {
        if (pthread_create(&threads[created], NULL, chunk_worker_main, &queue) != 0) {
            queue_mark_failed(&queue);
            fprintf(stderr, "error: can't start worker thread\n");
            break;
        }
    }

    for (u32 i = 0; i < created; i++)
        pthread_join(threads[i], NULL);
    free(threads);
#endif

    int ok = !queue.failed;
    queue_destroy(&queue);
    return ok;
}

static u32 effective_chunk_jobs(u32 job_count, u32 requested_jobs) {
    if (job_count == 0)
        return 0;
    if (requested_jobs == 0)
        requested_jobs = 1;
    if (requested_jobs > job_count)
        requested_jobs = job_count;
#ifdef _WIN32
    if (requested_jobs > 64)
        requested_jobs = 64;
#endif
    return requested_jobs;
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
