#ifndef DOLRECOMP_DISC_EXTRACT_INTERNAL_H
#define DOLRECOMP_DISC_EXTRACT_INTERNAL_H

#include "core/types.h"
#include <stdio.h>

#define GC_MAGIC  0xC2339F3Du
#define WII_MAGIC 0x5D1C9EA3u

#define DISC_HEADER_SIZE 0x440u
#define BI2_OFFSET       0x440u
#define BI2_SIZE         0x2000u
#define APPLOADER_OFFSET 0x2440u

#define MAX_PATH_BUF 4096
#define COPY_CHUNK   0x10000u

#ifdef _WIN32
#define WIT_EXE_NAME "wit.exe"
#else
#define WIT_EXE_NAME "wit"
#endif

typedef enum {
    EXTRACT_OK,
    EXTRACT_UNSUPPORTED,
    EXTRACT_FAILED,
} ExtractResult;

typedef struct {
    const char* input_path;
    const char* output_dir;
    const char* wit_path;
    int info_only;
    int native_only;
    int prefer_wit;
} Options;

typedef struct {
    FILE* file;
    u64 size;
} RawReader;

typedef struct {
    bool is_dir;
    u32 name_offset;
    u32 offset;
    u32 size;
    char name[256];
} FstEntry;

// Common Helpers
int is_supported_image_path(const char* path);
int raw_reader_open(RawReader* reader, const char* path);
void raw_reader_close(RawReader* reader);
int read_at(RawReader* reader, u64 offset, void* out, size_t size);
char* quote_arg(const char* arg);
void print_usage(void);

// Subsystem Entry Points
ExtractResult extract_gamecube_iso_native(const Options* opts);
int print_native_info(const char* path);
ExtractResult extract_with_wit(const Options* opts);

#endif /* DOLRECOMP_DISC_EXTRACT_INTERNAL_H */
