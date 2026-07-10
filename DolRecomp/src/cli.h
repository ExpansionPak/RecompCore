#ifndef DOLRECOMP_CLI_H
#define DOLRECOMP_CLI_H

#include "core/types.h"
#include "backend/emitter.h"

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

void print_usage(const char* argv0);
int parse_cli(int argc, char** argv, CliOptions* opts);

#endif /* DOLRECOMP_CLI_H */
