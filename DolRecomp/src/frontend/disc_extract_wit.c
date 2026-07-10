#include "disc_extract_internal.h"
#include "util.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int file_exists(const char* path) {
    FILE* file = fopen(path, "rb");
    if (!file)
        return 0;
    fclose(file);
    return 1;
}

static int local_wit_path(char* out, size_t out_size) {
    char bin_dir[MAX_PATH_BUF];

    if (join_path(bin_dir, sizeof(bin_dir), "extern/wit", "bin") &&
        join_path(out, out_size, bin_dir, WIT_EXE_NAME) &&
        file_exists(out)) {
        return 1;
    }

    return join_path(out, out_size, "extern/wit", WIT_EXE_NAME) &&
           file_exists(out);
}

char* quote_arg(const char* arg) {
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

static int command_exists(const char* exe) {
    char* qexe = quote_arg(exe);
    if (!qexe)
        return 0;

    char cmd[MAX_PATH_BUF + 128];
#ifdef _WIN32
    snprintf(cmd, sizeof(cmd), "%s --version >NUL 2>NUL", qexe);
#else
    snprintf(cmd, sizeof(cmd), "%s --version >/dev/null 2>/dev/null", qexe);
#endif
    free(qexe);
    return system(cmd) == 0;
}

ExtractResult extract_with_wit(const Options* opts) {
    char local_wit[MAX_PATH_BUF];
    const char* wit = opts->wit_path;
    int should_check_command = 1;

    if (!wit && local_wit_path(local_wit, sizeof(local_wit))) {
        wit = local_wit;
        should_check_command = 0;
    }
    if (!wit)
        wit = "wit";

    if (should_check_command && !command_exists(wit)) {
        fprintf(stderr, "wit bridge: '%s' was not found\n", wit);
        fprintf(stderr, "run dolrecomp.exe --setup or pass --wit <path>\n");
        return EXTRACT_UNSUPPORTED;
    }

    if (!make_dir_tree(opts->output_dir))
        return EXTRACT_FAILED;

    char* qwit = quote_arg(wit);
    char* qin = quote_arg(opts->input_path);
    char* qout = quote_arg(opts->output_dir);
    if (!qwit || !qin || !qout) {
        free(qwit);
        free(qin);
        free(qout);
        fprintf(stderr, "error: out of memory\n");
        return EXTRACT_FAILED;
    }

    size_t cmd_size = strlen(qwit) + strlen(qin) + strlen(qout) + 64;
    char* cmd = (char*)malloc(cmd_size);
    if (!cmd) {
        free(qwit);
        free(qin);
        free(qout);
        fprintf(stderr, "error: out of memory\n");
        return EXTRACT_FAILED;
    }

#ifdef _WIN32
    snprintf(cmd, cmd_size, "\"%s EXTRACT -o %s %s\"", qwit, qin, qout);
#else
    snprintf(cmd, cmd_size, "%s EXTRACT -o %s %s", qwit, qin, qout);
#endif
    printf("using wit bridge\n");
    int rc = system(cmd);

    free(cmd);
    free(qwit);
    free(qin);
    free(qout);

    if (rc != 0) {
        fprintf(stderr, "wit extraction failed\n");
        return EXTRACT_FAILED;
    }
    return EXTRACT_OK;
}
