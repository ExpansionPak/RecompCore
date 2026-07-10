#include "disc_extract_internal.h"
#include <stdio.h>
#include <string.h>

void print_usage(void) {
    printf("usage: dolrecomp.exe extract [options] <image.iso|image.wbfs> <output-dir>\n");
    printf("       dolrecomp.exe extract --info <image.iso|image.wbfs>\n");
    printf("\n");
    printf("options:\n");
    printf("  --info             print basic image info\n");
    printf("  --native-only      only use the built-in extractor\n");
    printf("  --prefer-wit       use Wiimms ISO Tool before native extraction\n");
    printf("  --wit <path>       path to wit or wit.exe\n");
    printf("  --help, -h         show this help\n");
    printf("\n");
    printf("supported inputs: .iso, .wbfs\n");
    printf("native support: GameCube ISO filesystem extraction\n");
    printf("wit bridge: Wii ISO and WBFS extraction\n");
}

static int parse_options(int argc, char** argv, Options* opts) {
    memset(opts, 0, sizeof(*opts));

    const char* positional[2];
    int positional_count = 0;

    for (int i = 1; i < argc; i++) {
        const char* arg = argv[i];
        if (strcmp(arg, "--help") == 0 || strcmp(arg, "-h") == 0) {
            print_usage();
            return 0;
        } else if (strcmp(arg, "--info") == 0) {
            opts->info_only = 1;
        } else if (strcmp(arg, "--native-only") == 0 || strcmp(arg, "--no-wit") == 0) {
            opts->native_only = 1;
        } else if (strcmp(arg, "--prefer-wit") == 0) {
            opts->prefer_wit = 1;
        } else if (strcmp(arg, "--wit") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "error: --wit needs a path\n");
                return -1;
            }
            opts->wit_path = argv[++i];
        } else if (strncmp(arg, "--wit=", 6) == 0) {
            opts->wit_path = arg + 6;
        } else if (arg[0] == '-' && arg[1] != '\0') {
            fprintf(stderr, "error: unknown option '%s'\n", arg);
            return -1;
        } else {
            if (positional_count >= 2) {
                print_usage();
                return -1;
            }
            positional[positional_count++] = arg;
        }
    }

    if (opts->info_only) {
        if (positional_count != 1) {
            print_usage();
            return -1;
        }
        opts->input_path = positional[0];
        return 1;
    }

    if (positional_count != 2) {
        print_usage();
        return -1;
    }

    opts->input_path = positional[0];
    opts->output_dir = positional[1];
    return 1;
}

int disc_extract_main(int argc, char** argv) {
    Options opts;
    int parsed = parse_options(argc, argv, &opts);
    if (parsed <= 0)
        return parsed == 0 ? 0 : 1;

    if (!is_supported_image_path(opts.input_path)) {
        fprintf(stderr, "unsupported format: only .iso and .wbfs are supported\n");
        return 1;
    }

    if (opts.info_only)
        return print_native_info(opts.input_path) ? 0 : 1;

    ExtractResult result = EXTRACT_UNSUPPORTED;
    if (opts.prefer_wit && !opts.native_only)
        result = extract_with_wit(&opts);

    if (result == EXTRACT_UNSUPPORTED)
        result = extract_gamecube_iso_native(&opts);

    if (result == EXTRACT_UNSUPPORTED && !opts.native_only)
        result = extract_with_wit(&opts);

    return result == EXTRACT_OK ? 0 : 1;
}
