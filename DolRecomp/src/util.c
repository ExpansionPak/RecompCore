// SPDX-License-Identifier: GPL-3.0-or-later
#include "util.h"
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
#include <sys/stat.h>
#include <unistd.h>
#endif

int make_dir(const char* path) {
#ifdef _WIN32
    int rc = _mkdir(path);
#else
    int rc = mkdir(path, 0777);
#endif
    if (rc == 0 || errno == EEXIST)
        return 1;

    fprintf(stderr, "error: can't create directory '%s'\n", path);
    return 0;
}

const char* path_basename(const char* path) {
    const char* slash = strrchr(path, '/');
    const char* backslash = strrchr(path, '\\');
    const char* base = NULL;
    if (!slash) {
        base = backslash;
    } else if (!backslash) {
        base = slash;
    } else {
        base = slash > backslash ? slash : backslash;
    }
    return base ? base + 1 : path;
}

int path_dirname(const char* path, char* dir, size_t dir_size) {
    const char* slash = strrchr(path, '/');
    const char* backslash = strrchr(path, '\\');
    const char* last = NULL;

    if (!slash) {
        last = backslash;
    } else if (!backslash) {
        last = slash;
    } else {
        last = slash > backslash ? slash : backslash;
    }

    if (!last) {
        if (dir_size < 2)
            return 0;
        dir[0] = '.';
        dir[1] = '\0';
        return 1;
    }

    size_t len = (size_t)(last - path);
    if (len == 0)
        len = 1;
    if (len + 1 > dir_size)
        return 0;

    memcpy(dir, path, len);
    dir[len] = '\0';
    return 1;
}

int ascii_lower(int ch) {
    if (ch >= 'A' && ch <= 'Z')
        return ch + ('a' - 'A');
    return ch;
}

int ascii_upper(int ch) {
    if (ch >= 'a' && ch <= 'z')
        return ch - ('a' - 'A');
    return ch;
}

int ascii_case_equal(const char* a, const char* b) {
    while (*a && *b) {
        if (ascii_lower((unsigned char)*a) != ascii_lower((unsigned char)*b))
            return 0;
        a++;
        b++;
    }
    return *a == '\0' && *b == '\0';
}

int is_path_sep(char ch) {
    return ch == '/' || ch == '\\';
}

int has_c_extension(const char* path) {
    const char* base = path_basename(path);
    const char* dot = strrchr(base, '.');
    return dot && ascii_case_equal(dot, ".c");
}

int has_rpx_extension(const char* path) {
    const char* base = path_basename(path);
    const char* dot = strrchr(base, '.');
    return dot && ascii_case_equal(dot, ".rpx");
}

int has_rel_extension(const char* path) {
    const char* base = path_basename(path);
    const char* dot = strrchr(base, '.');
    return dot && ascii_case_equal(dot, ".rel");
}

int is_title_id(const char* text) {
    size_t len = strlen(text);
    if (len != 6)
        return 0;

    for (size_t i = 0; i < len; i++) {
        char ch = text[i];
        if (!((ch >= 'A' && ch <= 'Z') ||
              (ch >= 'a' && ch <= 'z') ||
              (ch >= '0' && ch <= '9'))) {
            return 0;
        }
    }

    return 1;
}

int is_title_id_length_valid(const char* text) {
    return strlen(text) == 6;
}

int parse_cpu_name(const char* text, DolRecompCPU* cpu) {
    if (ascii_case_equal(text, "gekko") || ascii_case_equal(text, "gamecube")) {
        *cpu = DOLRECOMP_CPU_GEKKO;
        return 1;
    }

    if (ascii_case_equal(text, "broadway") || ascii_case_equal(text, "wii")) {
        *cpu = DOLRECOMP_CPU_BROADWAY;
        return 1;
    }

    if (ascii_case_equal(text, "espresso") || ascii_case_equal(text, "wiiu") ||
        ascii_case_equal(text, "wii-u")) {
        *cpu = DOLRECOMP_CPU_ESPRESSO;
        return 1;
    }

    return 0;
}

const char* cpu_display_name(DolRecompCPU cpu) {
    switch (cpu) {
    case DOLRECOMP_CPU_BROADWAY:
        return "Broadway (Wii)";
    case DOLRECOMP_CPU_ESPRESSO:
        return "Espresso (Wii U)";
    case DOLRECOMP_CPU_GEKKO:
    default:
        return "Gekko (GameCube)";
    }
}

void copy_title_id(char* out, size_t out_size, const char* title_id) {
    size_t len = strlen(title_id);
    if (len >= out_size)
        len = out_size - 1;

    for (size_t i = 0; i < len; i++)
        out[i] = (char)ascii_upper((unsigned char)title_id[i]);
    out[len] = '\0';
}

void sleep_ms(u32 milliseconds) {
#ifdef _WIN32
    Sleep(milliseconds);
#else
    usleep((useconds_t)milliseconds * 1000u);
#endif
}

const char* skip_spaces(const char* text) {
    while (*text == ' ' || *text == '\t')
        text++;
    return text;
}

void copy_trimmed(char* out, size_t out_size, const char* start, const char* end) {
    while (end > start &&
           (end[-1] == '\r' || end[-1] == '\n' ||
            end[-1] == ' ' || end[-1] == '\t')) {
        end--;
    }

    size_t len = (size_t)(end - start);
    if (len >= out_size)
        len = out_size - 1;
    memcpy(out, start, len);
    out[len] = '\0';
}

int make_dir_tree(const char* path) {
    char dir[1200];
    if (path[0] == '\0')
        return 1;
    if (file_exists(path))
        return path_is_directory(path);

    if (!path_dirname(path, dir, sizeof(dir)))
        return 0;
    if (strcmp(dir, ".") != 0 && strcmp(dir, "/") != 0 &&
        !make_dir_tree(dir)) {
        return 0;
    }

    return make_dir(path);
}

int join_path(char* out, size_t out_size, const char* dir, const char* name) {
    size_t dir_len = strlen(dir);
    size_t name_len = strlen(name);

    if (dir_len == 0) {
        if (name_len + 1 > out_size)
            return 0;
        memcpy(out, name, name_len + 1);
        return 1;
    }

    int need_sep = !is_path_sep(dir[dir_len - 1]) && !is_path_sep(name[0]);
    size_t required = dir_len + name_len + (need_sep ? 1 : 0) + 1;
    if (required > out_size)
        return 0;

    memcpy(out, dir, dir_len);
    if (need_sep) {
#ifdef _WIN32
        out[dir_len] = '\\';
#else
        out[dir_len] = '/';
#endif
        memcpy(out + dir_len + 1, name, name_len + 1);
    } else {
        memcpy(out + dir_len, name, name_len + 1);
    }

    return 1;
}

int file_exists(const char* path) {
#ifdef _WIN32
    DWORD attr = GetFileAttributesA(path);
    return attr != INVALID_FILE_ATTRIBUTES;
#else
    struct stat st;
    return stat(path, &st) == 0;
#endif
}

int path_is_directory(const char* path) {
#ifdef _WIN32
    DWORD attr = GetFileAttributesA(path);
    if (attr == INVALID_FILE_ATTRIBUTES)
        return 0;
    return (attr & FILE_ATTRIBUTE_DIRECTORY) != 0;
#else
    struct stat st;
    if (stat(path, &st) != 0)
        return 0;
    return S_ISDIR(st.st_mode);
#endif
}

void path_list_free(PathList* list) {
    for (u32 i = 0; i < list->count; i++)
        free(list->paths[i]);
    free(list->paths);
    list->paths = NULL;
    list->count = 0;
    list->capacity = 0;
}

char* copy_string_alloc(const char* text) {
    size_t len = strlen(text);
    char* copy = (char*)malloc(len + 1);
    if (!copy)
        return NULL;
    memcpy(copy, text, len + 1);
    return copy;
}

int path_list_add(PathList* list, const char* path) {
    if (list->count == list->capacity) {
        u32 new_capacity = list->capacity ? list->capacity * 2u : 32u;
        char** new_paths =
            (char**)realloc(list->paths, new_capacity * sizeof(*new_paths));
        if (!new_paths) {
            fprintf(stderr, "error: out of memory\n");
            return 0;
        }
        list->paths = new_paths;
        list->capacity = new_capacity;
    }

    list->paths[list->count] = copy_string_alloc(path);
    if (!list->paths[list->count]) {
        fprintf(stderr, "error: out of memory\n");
        return 0;
    }
    list->count++;
    return 1;
}

int collect_rel_paths(const char* root, PathList* list) {
#ifdef _WIN32
    char pattern[1200];
    if (!join_path(pattern, sizeof(pattern), root, "*")) {
        fprintf(stderr, "error: path is too long\n");
        return 0;
    }

    WIN32_FIND_DATAA data;
    HANDLE find = FindFirstFileA(pattern, &data);
    if (find == INVALID_HANDLE_VALUE) {
        fprintf(stderr, "error: can't read directory '%s'\n", root);
        return 0;
    }

    do {
        if (strcmp(data.cFileName, ".") == 0 ||
            strcmp(data.cFileName, "..") == 0) {
            continue;
        }

        char child[1200];
        if (!join_path(child, sizeof(child), root, data.cFileName)) {
            FindClose(find);
            fprintf(stderr, "error: path is too long\n");
            return 0;
        }

        if ((data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
            if (!collect_rel_paths(child, list)) {
                FindClose(find);
                return 0;
            }
        } else if (has_rel_extension(child)) {
            if (!path_list_add(list, child)) {
                FindClose(find);
                return 0;
            }
        }
    } while (FindNextFileA(find, &data));

    FindClose(find);
    return 1;
#else
    DIR* dir = opendir(root);
    if (!dir) {
        fprintf(stderr, "error: can't read directory '%s'\n", root);
        return 0;
    }

    struct dirent* entry;
    while ((entry = readdir(dir)) != NULL) {
        if (strcmp(entry->d_name, ".") == 0 ||
            strcmp(entry->d_name, "..") == 0) {
            continue;
        }

        char child[1200];
        if (!join_path(child, sizeof(child), root, entry->d_name)) {
            closedir(dir);
            fprintf(stderr, "error: path is too long\n");
            return 0;
        }

        if (path_is_directory(child)) {
            if (!collect_rel_paths(child, list)) {
                closedir(dir);
                return 0;
            }
        } else if (has_rel_extension(child)) {
            if (!path_list_add(list, child)) {
                closedir(dir);
                return 0;
            }
        }
    }

    closedir(dir);
    return 1;
#endif
}

static int compare_paths_for_sort(const void* a, const void* b) {
    const char* const* pa = (const char* const*)a;
    const char* const* pb = (const char* const*)b;
    return strcmp(*pa, *pb);
}

void path_list_sort(PathList* list) {
    if (list->count > 1)
        qsort(list->paths, list->count, sizeof(list->paths[0]),
              compare_paths_for_sort);
}

char* shell_quote_arg(const char* arg) {
    size_t len = strlen(arg);
    size_t cap = len * 4 + 3;
    char* out = (char*)malloc(cap);
    if (!out)
        return NULL;

#ifdef _WIN32
    size_t w = 0;
    out[w++] = '"';
    for (size_t i = 0; i < len; i++) {
        if (arg[i] == '"')
            out[w++] = '\\';
        out[w++] = arg[i];
    }
    out[w++] = '"';
    out[w] = '\0';
#else
    size_t w = 0;
    out[w++] = '\'';
    for (size_t i = 0; i < len; i++) {
        if (arg[i] == '\'') {
            memcpy(out + w, "'\\''", 4);
            w += 4;
        } else {
            out[w++] = arg[i];
        }
    }
    out[w++] = '\'';
    out[w] = '\0';
#endif

    return out;
}

int run_shell_command(const char* command) {
    return system(command) == 0;
}

int build_generated_folder_path(const char* output_root,
                                       const char* title_id,
                                       int titleless_mode,
                                       char* folder_path,
                                       size_t folder_path_size) {
    if (!make_dir_tree(output_root))
        return 0;

    if (titleless_mode) {
        if (!join_path(folder_path, folder_path_size, output_root, "generated")) {
            fprintf(stderr, "error: output path is too long\n");
            return 0;
        }
    } else {
        char folder_name[128];
        if (snprintf(folder_name, sizeof(folder_name), "%s_generated", title_id) >=
            (int)sizeof(folder_name)) {
            fprintf(stderr, "error: output path is too long\n");
            return 0;
        }
        if (!join_path(folder_path, folder_path_size, output_root, folder_name)) {
            fprintf(stderr, "error: output path is too long\n");
            return 0;
        }
    }

    return make_dir_tree(folder_path);
}

void safe_module_name(const char* path, char* out, size_t out_size) {
    const char* base = path_basename(path);
    size_t len = strlen(base);
    if (len >= 4 && ascii_case_equal(base + len - 4, ".rel"))
        len -= 4;

    size_t w = 0;
    for (size_t i = 0; i < len && w + 1 < out_size; i++) {
        char ch = base[i];
        if ((ch >= 'A' && ch <= 'Z') ||
            (ch >= 'a' && ch <= 'z') ||
            (ch >= '0' && ch <= '9') ||
            ch == '_' || ch == '-') {
            out[w++] = ch;
        } else {
            out[w++] = '_';
        }
    }

    if (w == 0 && out_size > 1)
        out[w++] = 'm';
    out[w] = '\0';
}

int build_rel_output_path(const char* generated_root,
                                 const char* rel_path,
                                 u32 module_id,
                                 char* output_path,
                                 size_t output_path_size) {
    char rels_dir[1200];
    char module_name[320];
    char module_dir_name[400];
    char module_dir[1200];

    if (!join_path(rels_dir, sizeof(rels_dir), generated_root, "rels")) {
        fprintf(stderr, "error: output path is too long\n");
        return 0;
    }
    if (!make_dir_tree(rels_dir))
        return 0;

    safe_module_name(rel_path, module_name, sizeof(module_name));
    if (snprintf(module_dir_name, sizeof(module_dir_name), "%s_%u",
                 module_name, module_id) >= (int)sizeof(module_dir_name)) {
        fprintf(stderr, "error: output path is too long\n");
        return 0;
    }

    if (!join_path(module_dir, sizeof(module_dir), rels_dir, module_dir_name)) {
        fprintf(stderr, "error: output path is too long\n");
        return 0;
    }
    if (!make_dir_tree(module_dir))
        return 0;

    if (!join_path(output_path, output_path_size, module_dir, "generated.c")) {
        fprintf(stderr, "error: output path is too long\n");
        return 0;
    }

    return 1;
}

int make_output_stem(const char* output_path, char* stem, size_t stem_size) {
    size_t len = strlen(output_path);
    if (len + 1 > stem_size) {
        fprintf(stderr, "error: output path is too long\n");
        return 0;
    }
    memcpy(stem, output_path, len + 1);
    char* dot = strrchr(stem, '.');
    if (dot && ascii_case_equal(dot, ".c"))
        *dot = '\0';
    return 1;
}

#define DATABASE_DIR "database"
#define DATABASE_TITLES_FILE "titles.txt"
#define GAMETDB_TITLES_URL "https://www.gametdb.com/titles.txt?LANG=EN"

int database_path(char* out, size_t out_size, const char* name) {
    return join_path(out, out_size, DATABASE_DIR, name);
}

int database_titles_path(char* out, size_t out_size) {
    return database_path(out, out_size, DATABASE_TITLES_FILE);
}

int database_titles_available(void) {
    char path[1200];
    return database_titles_path(path, sizeof(path)) && file_exists(path);
}

void print_database_missing_notice(void) {
    fprintf(stderr,
            "database/titles.txt is missing; using GameCube mode. Run dolrecomp --setup to download it.\n");
}

static int parse_gametdb_title_line(const char* line, const char* title_id,
                                    char* out, size_t out_size) {
    const char* eq = strchr(line, '=');
    if (!eq)
        return 0;

    const char* id_start = skip_spaces(line);
    const char* id_end = eq;
    while (id_end > id_start && (id_end[-1] == ' ' || id_end[-1] == '\t'))
        id_end--;

    if ((size_t)(id_end - id_start) != 6)
        return 0;

    for (size_t i = 0; i < 6; i++) {
        if (ascii_upper((unsigned char)id_start[i]) !=
            ascii_upper((unsigned char)title_id[i])) {
            return 0;
        }
    }

    const char* title_start = skip_spaces(eq + 1);
    const char* title_end = title_start + strlen(title_start);
    copy_trimmed(out, out_size, title_start, title_end);
    return out[0] != '\0';
}

static int resolve_game_name_from_stream(FILE* stream, const char* title_id,
                                         char* out, size_t out_size) {
    char line[1024];
    while (fgets(line, sizeof(line), stream)) {
        if (parse_gametdb_title_line(line, title_id, out, out_size))
            return 1;
    }
    return 0;
}

static int resolve_game_name_from_file(const char* path, const char* title_id,
                                       char* out, size_t out_size) {
    FILE* file = fopen(path, "r");
    if (!file)
        return 0;

    int ok = resolve_game_name_from_stream(file, title_id, out, out_size);
    fclose(file);
    return ok;
}

int resolve_game_name(const char* title_id, char* out, size_t out_size) {
    const char* env_path = getenv("DOLRECOMP_GAMETDB_TITLES");
    if (env_path && env_path[0] != '\0' &&
        resolve_game_name_from_file(env_path, title_id, out, out_size)) {
        return 1;
    }

    char path[1200];
    if (!database_titles_path(path, sizeof(path)))
        return 0;

    return resolve_game_name_from_file(path, title_id, out, out_size);
}

void describe_game(char* out, size_t out_size, const char* title_id,
                          const char* filename) {
    char name[400];
    if (resolve_game_name(title_id, name, sizeof(name))) {
        snprintf(out, out_size, "%s (%s)", name, title_id);
    } else if (filename) {
        snprintf(out, out_size, "%s", path_basename(filename));
    } else {
        snprintf(out, out_size, "%s", title_id);
    }
}

