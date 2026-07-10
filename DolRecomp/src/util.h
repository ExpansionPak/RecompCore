// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef DOLRECOMP_UTIL_H
#define DOLRECOMP_UTIL_H

#include "core/types.h"
#include "backend/emitter.h"
#include <stdio.h>
#include <stddef.h>

typedef struct {
    char** paths;
    u32 count;
    u32 capacity;
} PathList;

int make_dir(const char* path);
const char* path_basename(const char* path);
int path_dirname(const char* path, char* dir, size_t dir_size);
int ascii_lower(int ch);
int ascii_upper(int ch);
int ascii_case_equal(const char* a, const char* b);
int is_path_sep(char ch);
int has_c_extension(const char* path);
int has_rpx_extension(const char* path);
int has_rel_extension(const char* path);
int is_title_id(const char* text);
int is_title_id_length_valid(const char* text);
int parse_cpu_name(const char* text, DolRecompCPU* cpu);
const char* cpu_display_name(DolRecompCPU cpu);
void copy_title_id(char* out, size_t out_size, const char* title_id);
void sleep_ms(u32 milliseconds);
const char* skip_spaces(const char* text);
void copy_trimmed(char* out, size_t out_size, const char* start, const char* end);
int make_dir_tree(const char* path);
int join_path(char* out, size_t out_size, const char* dir, const char* name);
int file_exists(const char* path);
int path_is_directory(const char* path);
void path_list_free(PathList* list);
char* copy_string_alloc(const char* text);
int path_list_add(PathList* list, const char* path);
int collect_rel_paths(const char* root, PathList* list);
void path_list_sort(PathList* list);
char* shell_quote_arg(const char* arg);
int run_shell_command(const char* command);
int build_generated_folder_path(const char* output_root, const char* title_id, int titleless_mode, char* folder_path, size_t folder_path_size);
void safe_module_name(const char* path, char* out, size_t out_size);
int build_rel_output_path(const char* generated_root, const char* rel_path, u32 module_id, char* output_path, size_t output_path_size);
int make_output_stem(const char* output_path, char* stem, size_t stem_size);
int database_path(char* out, size_t out_size, const char* name);
int database_titles_path(char* out, size_t out_size);
int database_titles_available(void);
void print_database_missing_notice(void);
int resolve_game_name(const char* title_id, char* out, size_t out_size);
void describe_game(char* out, size_t out_size, const char* title_id, const char* filename);

#endif /* DOLRECOMP_UTIL_H */
