#include "disc_extract_internal.h"
#include "util.h"
#include "backend/emitter.h" // for read_be32/etc if needed (actually decode/endian helpers are in util.h or frontend)
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

#ifdef _WIN32
#include <direct.h>
#else
#include <sys/stat.h>
#include <unistd.h>
#endif

// Helpers for reading big-endian values (usually in util.h or inline here)
static u32 read_be32(const u8* data) {
    return ((u32)data[0] << 24) | ((u32)data[1] << 16) | ((u32)data[2] << 8) | data[3];
}

int ascii_ieq(const char* a, const char* b) {
    while (*a && *b) {
        char ca = *a;
        char cb = *b;
        if (ca >= 'A' && ca <= 'Z')
            ca = (char)(ca - 'A' + 'a');
        if (cb >= 'A' && cb <= 'Z')
            cb = (char)(cb - 'A' + 'a');
        if (ca != cb)
            return 0;
        a++;
        b++;
    }
    return *a == '\0' && *b == '\0';
}

static const char* path_extension(const char* path) {
    const char* slash = strrchr(path, '/');
    const char* backslash = strrchr(path, '\\');
    const char* base = path;
    const char* dot;
    if (slash && slash + 1 > base)
        base = slash + 1;
    if (backslash && backslash + 1 > base)
        base = backslash + 1;
    dot = strrchr(base, '.');
    return dot ? dot : "";
}

static int is_iso_path(const char* path) {
    return ascii_ieq(path_extension(path), ".iso");
}

static int is_wbfs_path(const char* path) {
    return ascii_ieq(path_extension(path), ".wbfs");
}

int is_supported_image_path(const char* path) {
    return is_iso_path(path) || is_wbfs_path(path);
}

static int seek_file_base(FILE* file, s64 offset, int origin) {
#ifdef _WIN32
    return _fseeki64(file, offset, origin) == 0;
#else
    return fseeko(file, (off_t)offset, origin) == 0;
#endif
}

static int seek_file(FILE* file, u64 offset) {
    return seek_file_base(file, (s64)offset, SEEK_SET);
}

static u64 tell_file(FILE* file) {
#ifdef _WIN32
    return (u64)_ftelli64(file);
#else
    return (u64)ftello(file);
#endif
}

static int get_file_size(FILE* file, u64* size) {
    if (!seek_file(file, 0))
        return 0;
    if (!seek_file_base(file, 0, SEEK_END))
        return 0;
    *size = tell_file(file);
    return seek_file(file, 0);
}

int read_at(RawReader* reader, u64 offset, void* out, size_t size) {
    if (offset > reader->size || size > reader->size - offset)
        return 0;
    if (!seek_file(reader->file, offset))
        return 0;
    return fread(out, 1, size, reader->file) == size;
}

int raw_reader_open(RawReader* reader, const char* path) {
    memset(reader, 0, sizeof(*reader));
    reader->file = fopen(path, "rb");
    if (!reader->file) {
        fprintf(stderr, "error: can't open '%s'\n", path);
        return 0;
    }
    if (!get_file_size(reader->file, &reader->size)) {
        fprintf(stderr, "error: can't get size for '%s'\n", path);
        fclose(reader->file);
        reader->file = NULL;
        return 0;
    }
    return 1;
}

void raw_reader_close(RawReader* reader) {
    if (reader->file)
        fclose(reader->file);
    memset(reader, 0, sizeof(*reader));
}

static void sanitize_component(char* out, size_t out_size, const char* in) {
    size_t w = 0;
    if (out_size == 0)
        return;

    if (strcmp(in, ".") == 0 || strcmp(in, "..") == 0 || in[0] == '\0') {
        snprintf(out, out_size, "_");
        return;
    }

    for (size_t i = 0; in[i] != '\0' && w + 1 < out_size; i++) {
        unsigned char c = (unsigned char)in[i];
        if (c < 0x20 || c == '/' || c == '\\' || c == ':' ||
            c == '*' || c == '?' || c == '"' || c == '<' ||
            c == '>' || c == '|') {
            out[w++] = '_';
        } else {
            out[w++] = (char)c;
        }
    }
    out[w] = '\0';
    if (out[0] == '\0')
        snprintf(out, out_size, "_");
}

static int write_range(RawReader* reader, u64 offset, u64 size,
                       const char* output_path) {
    u8 buffer[COPY_CHUNK];
    FILE* out = fopen(output_path, "wb");
    if (!out) {
        fprintf(stderr, "error: can't open '%s'\n", output_path);
        return 0;
    }

    u64 remaining = size;
    u64 cursor = offset;
    while (remaining != 0) {
        size_t chunk = remaining > sizeof(buffer) ? sizeof(buffer) : (size_t)remaining;
        if (!read_at(reader, cursor, buffer, chunk)) {
            fprintf(stderr, "error: failed reading image at 0x%llX\n",
                    (unsigned long long)cursor);
            fclose(out);
            return 0;
        }
        if (fwrite(buffer, 1, chunk, out) != chunk) {
            fprintf(stderr, "error: failed writing '%s'\n", output_path);
            fclose(out);
            return 0;
        }
        cursor += chunk;
        remaining -= chunk;
    }

    if (fclose(out) != 0) {
        fprintf(stderr, "error: failed closing '%s'\n", output_path);
        return 0;
    }
    return 1;
}

static int write_named_range(RawReader* reader, const char* dir,
                             const char* name, u64 offset, u64 size) {
    char path[MAX_PATH_BUF];
    if (!join_path(path, sizeof(path), dir, name)) {
        fprintf(stderr, "error: output path is too long\n");
        return 0;
    }
    return write_range(reader, offset, size, path);
}

static u32 dol_file_size_from_header(const u8* dol_header) {
    u32 max_end = 0x100u;

    for (u32 i = 0; i < 7; i++) {
        u32 off = read_be32(dol_header + i * 4);
        u32 size = read_be32(dol_header + 0x90 + i * 4);
        if (size != 0 && off + size > max_end)
            max_end = off + size;
    }

    for (u32 i = 0; i < 11; i++) {
        u32 off = read_be32(dol_header + 0x1c + i * 4);
        u32 size = read_be32(dol_header + 0xac + i * 4);
        if (size != 0 && off + size > max_end)
            max_end = off + size;
    }

    return max_end;
}

static int find_string_end(const char* base, size_t size, u32 offset) {
    if (offset >= size)
        return -1;
    for (size_t i = offset; i < size; i++) {
        if (base[i] == '\0')
            return (int)i;
    }
    return -1;
}

static int parse_fst_entries(const u8* fst, u32 fst_size,
                             FstEntry** out_entries, u32* out_count) {
    if (fst_size < 12) {
        fprintf(stderr, "error: FST is too small\n");
        return 0;
    }

    u32 root = read_be32(fst);
    u32 count = read_be32(fst + 8);
    if ((root >> 24) != 1 || count == 0 || count > fst_size / 12) {
        fprintf(stderr, "error: invalid FST root\n");
        return 0;
    }

    const char* names = (const char*)fst + count * 12u;
    size_t names_size = fst_size - count * 12u;
    FstEntry* entries = (FstEntry*)calloc(count, sizeof(FstEntry));
    if (!entries) {
        fprintf(stderr, "error: out of memory\n");
        return 0;
    }

    for (u32 i = 0; i < count; i++) {
        const u8* raw = fst + i * 12u;
        u32 type_name = read_be32(raw);
        entries[i].is_dir = (type_name >> 24) != 0;
        entries[i].name_offset = type_name & 0x00FFFFFFu;
        entries[i].offset = read_be32(raw + 4);
        entries[i].size = read_be32(raw + 8);

        int end = find_string_end(names, names_size, entries[i].name_offset);
        if (end < 0) {
            fprintf(stderr, "error: invalid FST name offset %u\n",
                    entries[i].name_offset);
            free(entries);
            return 0;
        }

        sanitize_component(entries[i].name, sizeof(entries[i].name),
                           names + entries[i].name_offset);
    }

    *out_entries = entries;
    *out_count = count;
    return 1;
}

static int extract_fst_dir(RawReader* reader, const FstEntry* entries,
                           u32 count, u32 dir_index, const char* dir_path,
                           u32* next_index) {
    u32 end = entries[dir_index].size;
    if (end > count || end <= dir_index) {
        fprintf(stderr, "error: invalid FST directory range\n");
        return 0;
    }

    u32 i = dir_index + 1;
    while (i < end) {
        char child_path[MAX_PATH_BUF];
        if (!join_path(child_path, sizeof(child_path), dir_path, entries[i].name)) {
            fprintf(stderr, "error: output path is too long\n");
            return 0;
        }

        if (entries[i].is_dir) {
            if (!make_dir_tree(child_path))
                return 0;
            u32 after_dir = i + 1;
            if (!extract_fst_dir(reader, entries, count, i, child_path, &after_dir))
                return 0;
            i = after_dir;
        } else {
            if (!write_range(reader, entries[i].offset, entries[i].size, child_path))
                return 0;
            i++;
        }
    }

    *next_index = end;
    return 1;
}

ExtractResult extract_gamecube_iso_native(const Options* opts) {
    RawReader reader;
    u8 header[0x500];
    if (!raw_reader_open(&reader, opts->input_path))
        return EXTRACT_FAILED;

    if (!read_at(&reader, 0, header, sizeof(header))) {
        raw_reader_close(&reader);
        return EXTRACT_UNSUPPORTED;
    }

    u32 wii_magic = read_be32(header + 0x18);
    u32 gc_magic = read_be32(header + 0x1c);
    if (gc_magic != GC_MAGIC) {
        raw_reader_close(&reader);
        if (wii_magic == WII_MAGIC)
            fprintf(stderr, "native extractor: Wii ISO needs the wit bridge\n");
        return EXTRACT_UNSUPPORTED;
    }

    u32 dol_offset = read_be32(header + 0x420);
    u32 fst_offset = read_be32(header + 0x424);
    u32 fst_size = read_be32(header + 0x428);
    if (dol_offset == 0 || fst_offset == 0 || fst_size == 0 ||
        (u64)fst_offset + fst_size > reader.size) {
        fprintf(stderr, "error: invalid GameCube disc header\n");
        raw_reader_close(&reader);
        return EXTRACT_FAILED;
    }

    printf("format: GameCube ISO\n");
    printf("game id: %.6s\n", header);

    if (!make_dir_tree(opts->output_dir)) {
        raw_reader_close(&reader);
        return EXTRACT_FAILED;
    }

    char sys_dir[MAX_PATH_BUF];
    char files_dir[MAX_PATH_BUF];
    if (!join_path(sys_dir, sizeof(sys_dir), opts->output_dir, "sys") ||
        !join_path(files_dir, sizeof(files_dir), opts->output_dir, "files")) {
        fprintf(stderr, "error: output path is too long\n");
        raw_reader_close(&reader);
        return EXTRACT_FAILED;
    }
    if (!make_dir_tree(sys_dir) || !make_dir_tree(files_dir)) {
        raw_reader_close(&reader);
        return EXTRACT_FAILED;
    }

    u8 dol_header[0x100];
    if (!read_at(&reader, dol_offset, dol_header, sizeof(dol_header))) {
        fprintf(stderr, "error: failed reading main.dol header\n");
        raw_reader_close(&reader);
        return EXTRACT_FAILED;
    }
    u32 dol_size = dol_file_size_from_header(dol_header);

    u32 apploader_size = 0x20u;
    u8 app_header[0x20];
    if (read_at(&reader, APPLOADER_OFFSET, app_header, sizeof(app_header))) {
        apploader_size += read_be32(app_header + 0x14);
        apploader_size += read_be32(app_header + 0x18);
    }

    if (!write_named_range(&reader, sys_dir, "boot.bin", 0, DISC_HEADER_SIZE) ||
        !write_named_range(&reader, sys_dir, "bi2.bin", BI2_OFFSET, BI2_SIZE) ||
        !write_named_range(&reader, sys_dir, "apploader.img",
                           APPLOADER_OFFSET, apploader_size) ||
        !write_named_range(&reader, sys_dir, "main.dol", dol_offset, dol_size) ||
        !write_named_range(&reader, sys_dir, "fst.bin", fst_offset, fst_size)) {
        raw_reader_close(&reader);
        return EXTRACT_FAILED;
    }

    u8* fst = (u8*)malloc(fst_size);
    if (!fst) {
        fprintf(stderr, "error: out of memory\n");
        raw_reader_close(&reader);
        return EXTRACT_FAILED;
    }
    if (!read_at(&reader, fst_offset, fst, fst_size)) {
        fprintf(stderr, "error: failed reading FST\n");
        free(fst);
        raw_reader_close(&reader);
        return EXTRACT_FAILED;
    }

    FstEntry* entries = NULL;
    u32 count = 0;
    if (!parse_fst_entries(fst, fst_size, &entries, &count)) {
        free(fst);
        raw_reader_close(&reader);
        return EXTRACT_FAILED;
    }

    u32 next = 0;
    int ok = extract_fst_dir(&reader, entries, count, 0, files_dir, &next);
    free(entries);
    free(fst);
    raw_reader_close(&reader);

    if (!ok)
        return EXTRACT_FAILED;

    printf("extracted to: %s\n", opts->output_dir);
    return EXTRACT_OK;
}

int print_native_info(const char* path) {
    RawReader reader;
    u8 header[0x500];
    if (!raw_reader_open(&reader, path))
        return 0;
    if (!read_at(&reader, 0, header, sizeof(header))) {
        fprintf(stderr, "error: failed reading image header\n");
        raw_reader_close(&reader);
        return 0;
    }

    if (memcmp(header, "WBFS", 4) == 0) {
        printf("format: WBFS container\n");
        printf("native extraction: use wit bridge\n");
        raw_reader_close(&reader);
        return 1;
    }

    u32 wii_magic = read_be32(header + 0x18);
    u32 gc_magic = read_be32(header + 0x1c);
    if (gc_magic == GC_MAGIC || wii_magic == WII_MAGIC) {
        u32 dol_offset = read_be32(header + 0x420);
        u32 fst_offset = read_be32(header + 0x424);
        u32 fst_size = read_be32(header + 0x428);
        printf("format: %s\n", gc_magic == GC_MAGIC ? "GameCube ISO" : "Wii ISO");
        printf("game id: %.6s\n", header);
        printf("main.dol: 0x%08X\n", dol_offset);
        printf("fst:      0x%08X (0x%X bytes)\n", fst_offset, fst_size);
        if (wii_magic == WII_MAGIC)
            printf("native extraction: use wit bridge for Wii partitions\n");
        raw_reader_close(&reader);
        return 1;
    }

    printf("format: unknown ISO/WBFS contents\n");
    raw_reader_close(&reader);
    return 1;
}
