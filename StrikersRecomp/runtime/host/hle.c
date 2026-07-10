// High-level emulation (HLE) dispatch.
//
// The recompiled game runs natively, but GameCube SDK functions that touch
// hardware (GX/VI/PAD/SI/AI/DSP/DI/EXI) or the OS kernel cannot. Those are
// intercepted at their guest addresses. `hle_install` resolves the generated
// SDK names once and builds an address-indexed table; `host_call` then performs
// one constant-time lookup per dispatched block.
//
// Functions in the table without a handler fall through to their recompiled
// implementation (true for pure-computation SDK routines that run fine
// natively); set STRIKERS_HLE_LOG=1 to trace which SDK functions the game
// calls and whether each is handled.
#include "hle.h"

#include "gxruntime/aram.h"
#include "gxruntime/memory_card.h"
#include "host/audio.h"
#include "gxruntime/dvd.h"
#include "gxruntime/platform.h"
#include "host/hle_abi.h"
#include "host/interrupt.h"
#include "host/mmio.h"
#include "host/sdk_map.h"
#include "host/hle_dvd.h"
#include "host/hle_card.h"
#include "host/hle_input.h"
#include "host/hle_physics.h"

#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

typedef void (*HleHandler)(CPUState* cpu);

typedef struct {
    const char* name;
    HleHandler  fn;
} HleEntry;

typedef struct {
    HleHandler intercept;
    HleHandler notify;
} HleDispatchEntry;

// Trace every intercepted SDK call (and DVD activity). Set by STRIKERS_HLE_LOG.
bool g_hle_log = false;
bool g_audio_log = false;
bool g_graphics_log = false;
static bool g_movie_log = false;
static bool g_movie_cadence_log = false;
bool g_card_log = false;
static bool g_auto_skip_card_prompt = false;
static bool g_state_log = false;
static u64 g_auto_input_last_pulse = ~(u64)0;
static u64 g_auto_input_once_frame = 0;
static u64 g_auto_input_once_sent = ~(u64)0;
static u64 g_auto_skip_card_last_pulse = ~(u64)0;
static unsigned g_movie_texobj_log_count = 0;
// Address handlers normally return to the intercepted function's caller.
// Synchronous host operations with a guest completion callback can instead
// tail-call that callback and let its normal `blr` return to the same caller.
static bool g_hle_tail_call = false;

#ifdef STRIKERSRECOMP_AURORA
#define HLE_DISPLAY_LIST_RETURN 0x7FFF0010u
#define HLE_GX_BEGIN_RETURN     0x7FFF0020u
#define GX_DIRTY_STATE_HELPER   0x8024DCA0u
#define GX_FLUSH_PRIM_HELPER    0x8024DDF0u
#define GX_DATA_SDA_OFFSET      (-20664)
#define GX_DIRTY_STATE_OFFSET   1452u

typedef struct {
    bool active;
    u8 stage;
    u32 caller;
    u32 address;
    u32 size;
} HleDisplayListCall;

static HleDisplayListCall g_display_list_call;

typedef struct {
    bool active;
    u8 stage;
    u8 primitive;
    u8 vtxfmt;
    u16 vertex_count;
    u32 caller;
} HleGxBegin;

static HleGxBegin g_gx_begin;
#endif

// CARD API completions are asynchronous on GameCube. Performing host storage
// synchronously is fine, but invoking a guest callback recursively from the
// intercepted API is not: Strikers checks the API's immediate return before
// the callback is allowed to advance its state machine. Queue completions and
// run them on the next dispatch with an interrupt-style saved register context.
#define HLE_CALLBACK_RETURN 0x7FFF0000u
#define HLE_CALLBACK_QUEUE_CAPACITY 32u

typedef struct {
    u32 address;
    s32 channel;
    s32 result;
} HlePendingCallback;

typedef struct {
    u32 gpr[32];
    f64 fpr[32];
    f64 ps1[32];
    u32 pc;
    u32 lr;
    u32 ctr;
    u32 cr;
    u32 xer;
    u32 fpscr;
    u32 msr;
    u32 srr0;
    u32 srr1;
    u32 dar;
    u32 dsisr;
    u32 ear;
    u32 hid2;
    u32 sr[16];
    u32 gqr[8];
    u32 exception;
    u32 program_exception;
    u32 reserve_addr;
    bool reserve_valid;
} HleSavedContext;

static HlePendingCallback g_callback_queue[HLE_CALLBACK_QUEUE_CAPACITY];
static u32 g_callback_read;
static u32 g_callback_count;
static bool g_callback_active;
static HleSavedContext g_callback_context;
DolMemoryCard* g_memory_card;

static void save_callback_context(HleSavedContext* saved,
                                  const CPUState* cpu) {
    memcpy(saved->gpr, cpu->gpr, sizeof saved->gpr);
    memcpy(saved->fpr, cpu->fpr, sizeof saved->fpr);
    memcpy(saved->ps1, cpu->ps1, sizeof saved->ps1);
    saved->pc = cpu->pc;
    saved->lr = cpu->lr;
    saved->ctr = cpu->ctr;
    saved->cr = cpu->cr;
    saved->xer = cpu->xer;
    saved->fpscr = cpu->fpscr;
    saved->msr = cpu->msr;
    saved->srr0 = cpu->srr0;
    saved->srr1 = cpu->srr1;
    saved->dar = cpu->dar;
    saved->dsisr = cpu->dsisr;
    saved->ear = cpu->ear;
    saved->hid2 = cpu->hid2;
    memcpy(saved->sr, cpu->sr, sizeof saved->sr);
    memcpy(saved->gqr, cpu->gqr, sizeof saved->gqr);
    saved->exception = cpu->exception;
    saved->program_exception = cpu->program_exception;
    saved->reserve_addr = cpu->reserve_addr;
    saved->reserve_valid = cpu->reserve_valid;
}

static void restore_callback_context(CPUState* cpu,
                                     const HleSavedContext* saved) {
    memcpy(cpu->gpr, saved->gpr, sizeof saved->gpr);
    memcpy(cpu->fpr, saved->fpr, sizeof saved->fpr);
    memcpy(cpu->ps1, saved->ps1, sizeof saved->ps1);
    cpu->pc = saved->pc;
    cpu->lr = saved->lr;
    cpu->ctr = saved->ctr;
    cpu->cr = saved->cr;
    cpu->xer = saved->xer;
    cpu->fpscr = saved->fpscr;
    cpu->msr = saved->msr;
    cpu->srr0 = saved->srr0;
    cpu->srr1 = saved->srr1;
    cpu->dar = saved->dar;
    cpu->dsisr = saved->dsisr;
    cpu->ear = saved->ear;
    cpu->hid2 = saved->hid2;
    memcpy(cpu->sr, saved->sr, sizeof saved->sr);
    memcpy(cpu->gqr, saved->gqr, sizeof saved->gqr);
    cpu->exception = saved->exception;
    cpu->program_exception = saved->program_exception;
    cpu->reserve_addr = saved->reserve_addr;
    cpu->reserve_valid = saved->reserve_valid;
}

bool queue_guest_callback(u32 address, s32 channel, s32 result) {
    if (address == 0)
        return true;
    if (g_callback_count == HLE_CALLBACK_QUEUE_CAPACITY) {
        fprintf(stderr, "[card] completion callback queue overflow\n");
        return false;
    }
    u32 index =
        (g_callback_read + g_callback_count) % HLE_CALLBACK_QUEUE_CAPACITY;
    g_callback_queue[index].address = address;
    g_callback_queue[index].channel = channel;
    g_callback_queue[index].result = result;
    g_callback_count++;
    if (g_card_log)
        fprintf(stderr,
                "[card] queued callback=0x%08X channel=%d result=%d depth=%u\n",
                address, channel, result, g_callback_count);
    return true;
}

bool hle_poll_callback(CPUState* cpu) {
    if (cpu == NULL || g_callback_active || g_callback_count == 0)
        return false;

    HlePendingCallback pending = g_callback_queue[g_callback_read];
    g_callback_read = (g_callback_read + 1u) % HLE_CALLBACK_QUEUE_CAPACITY;
    g_callback_count--;

    save_callback_context(&g_callback_context, cpu);
    g_callback_active = true;
    cpu->gpr[3] = (u32)pending.channel;
    cpu->gpr[4] = (u32)pending.result;
    cpu->pc = pending.address;
    cpu->lr = HLE_CALLBACK_RETURN;
    cpu->exception = 0;
    cpu->program_exception = 0;
    if (g_card_log)
        fprintf(stderr,
                "[card] dispatch callback=0x%08X channel=%d result=%d\n",
                pending.address, pending.channel, pending.result);
    return true;
}

// ---------------------------------------------------------------------------
// Guest memory helpers
// ---------------------------------------------------------------------------
char* hle_read_cstr(CPUState* c, u32 gaddr, char* buf, size_t cap) {
    size_t i = 0;
    if (gaddr != 0) {
        for (; i + 1 < cap; i++) {
            u8 ch = mem_read8(c, gaddr + (u32)i);
            if (ch == 0)
                break;
            buf[i] = (char)ch;
        }
    }
    buf[i] = '\0';
    return buf;
}

bool copy_guest_to_host(CPUState* cpu, u32 guest_address, void* output,
                               u32 length) {
    if (length == 0)
        return true;
    u32 available = 0;
    const void* direct =
        mmio_guest_pointer(cpu, guest_address, &available);
    if (direct != NULL && available >= length) {
        memcpy(output, direct, length);
        return true;
    }
    u8* bytes = (u8*)output;
    for (u32 i = 0; i < length; i++)
        bytes[i] = mem_read8(cpu, guest_address + i);
    return true;
}

bool copy_host_to_guest(CPUState* cpu, u32 guest_address,
                               const void* input, u32 length) {
    if (length == 0)
        return true;
    u32 available = 0;
    void* direct = mmio_guest_pointer(cpu, guest_address, &available);
    if (direct != NULL && available >= length) {
        memcpy(direct, input, length);
        return true;
    }
    const u8* bytes = (const u8*)input;
    for (u32 i = 0; i < length; i++)
        mem_write8(cpu, guest_address + i, bytes[i]);
    return true;
}

// ---------------------------------------------------------------------------
// OS handlers
// ---------------------------------------------------------------------------

// OSReport(const char* fmt, ...) — the game's own diagnostic logging. Aurora
// does not provide it, so reformat against the EABI varargs here. This is the
// most valuable early intercept: it surfaces what the boot code is doing.
static void hle_OSReport(CPUState* cpu) {
    char fmt[1024];
    hle_read_cstr(cpu, cpu->gpr[3], fmt, sizeof fmt);

    char out[2048];
    size_t oi = 0;
    unsigned iarg = 1;  // next integer vararg -> r4
    unsigned farg = 1;  // next float vararg   -> f2 (f1 unused for varargs)

    for (size_t i = 0; fmt[i] && oi + 1 < sizeof out;) {
        if (fmt[i] != '%') {
            out[oi++] = fmt[i++];
            continue;
        }
        // Rebuild a single conversion spec, dropping length modifiers so the
        // host snprintf matches the value type we pass (always 32-bit / double).
        char spec[32];
        size_t si = 0;
        spec[si++] = fmt[i++];
        while (fmt[i] && si + 2 < sizeof spec && strchr("-+ #0123456789.", fmt[i]))
            spec[si++] = fmt[i++];
        while (fmt[i] && strchr("lhLzjt", fmt[i]))
            i++;  // skip length modifier
        char conv = fmt[i] ? fmt[i++] : '\0';
        spec[si++] = conv;
        spec[si] = '\0';

        char tmp[576];
        tmp[0] = '\0';
        switch (conv) {
        case '%':
            tmp[0] = '%';
            tmp[1] = '\0';
            break;
        case 'd': case 'i':
            snprintf(tmp, sizeof tmp, spec, (int)hle_arg_u32(cpu, iarg++));
            break;
        case 'u': case 'x': case 'X': case 'o': case 'c':
            snprintf(tmp, sizeof tmp, spec, (unsigned)hle_arg_u32(cpu, iarg++));
            break;
        case 'p':
            snprintf(tmp, sizeof tmp, "0x%08X", hle_arg_u32(cpu, iarg++));
            break;
        case 's': {
            char s[512];
            hle_read_cstr(cpu, hle_arg_u32(cpu, iarg++), s, sizeof s);
            snprintf(tmp, sizeof tmp, spec, s);
            break;
        }
        case 'f': case 'F': case 'e': case 'E': case 'g': case 'G':
            snprintf(tmp, sizeof tmp, spec, hle_arg_f64(cpu, farg++));
            break;
        default:
            snprintf(tmp, sizeof tmp, "%s", spec);
            break;
        }
        for (size_t k = 0; tmp[k] && oi + 1 < sizeof out;)
            out[oi++] = tmp[k++];
    }
    out[oi] = '\0';
    fputs(out, stdout);
    fflush(stdout);
}

// A handler that does nothing and returns to the caller. Used to skip recompiled
// SDK routines whose only job is to drive hardware the host owns instead.
static void hle_noop(CPUState* cpu) { (void)cpu; }

// Skip a recompiled routine and report success (BOOL/int return of 1). For
// init functions whose hardware bring-up the host replaces but whose callers
// branch on a success return.
static void hle_return_true(CPUState* cpu) { hle_set_u32(cpu, 1); }
static void hle_return_zero(CPUState* cpu) { hle_set_u32(cpu, 0); }

static u32 lc_transaction_count(u32 bytes) {
    const u32 blocks = (bytes + 31u) / 32u;
    return (blocks + 127u) / 128u;
}

static void hle_LCStoreBlocks(CPUState* cpu) {
    const u32 dest = hle_arg_u32(cpu, 0);
    const u32 src = hle_arg_u32(cpu, 1);
    const u32 blocks = hle_arg_u32(cpu, 2) ? hle_arg_u32(cpu, 2) : 128u;
    mmio_guest_copy(cpu, dest, src, blocks * 32u);
}

static void hle_LCStoreData(CPUState* cpu) {
    const u32 dest = hle_arg_u32(cpu, 0);
    const u32 src = hle_arg_u32(cpu, 1);
    const u32 bytes = hle_arg_u32(cpu, 2);
    mmio_guest_copy(cpu, dest, src, bytes);
    hle_set_u32(cpu, lc_transaction_count(bytes));
}

// Desktop hosts have no GameCube chassis reset switch. Controller-based
// Start+L+R soft reset remains implemented by the game's own input path.
static void hle_OSGetResetButtonState(CPUState* cpu) {
    hle_set_u32(cpu, 0u);
}

static void hle_salInitDsp(CPUState* cpu) {
    audio_skip_dsp_init(cpu);
    hle_set_u32(cpu, 1);
}

// ---------------------------------------------------------------------------
// ARAM (AR) handlers
//
// The real ARAM setup (__OSInitAudioSystem -> ARInit) does a DSP/ARAM hardware
// handshake we skip, so the AR* SDK functions are served here against the host
// ARAM buffer (aram.c). Without these, ARGetBaseAddress returns uninitialized
// garbage and the game's ARAM heap walks an invalid free list forever.
// ---------------------------------------------------------------------------
static void hle_ARInit(CPUState* cpu) { (void)cpu; }              // no-op
static void hle_ARGetBaseAddress(CPUState* cpu) { hle_set_u32(cpu, ARAM_BASE); }
static void hle_ARGetSize(CPUState* cpu) { hle_set_u32(cpu, ARAM_SIZE); }
static void hle_ARGetDMAStatus(CPUState* cpu) { hle_set_u32(cpu, 0); }  // idle

// ARStartDMA(u32 type, u32 mainmem, u32 aram, u32 length). type 0 = MRAM->ARAM,
// 1 = ARAM->MRAM. Performed synchronously against the host ARAM buffer.
static void hle_ARStartDMA(CPUState* cpu) {
    u32 type = hle_arg_u32(cpu, 0);
    u32 mainmem = hle_arg_u32(cpu, 1);
    u32 aram = hle_arg_u32(cpu, 2);
    u32 length = hle_arg_u32(cpu, 3);
    if (type == 0)
        aram_dma_to_aram(cpu->ram, mainmem, aram, length);
    else
        aram_dma_to_ram(cpu->ram, mainmem, aram, length);
}

// MusyX aramUploadData normally enqueues ARQ DMA and receives an interrupt
// later. Our ARAM copy is synchronous, so replace the whole queue operation.
// If the caller supplied a completion callback, tail-call it with the original
// user value; its return address is already the caller of aramUploadData.
static void hle_aramUploadData(CPUState* cpu) {
    u32 mainmem = hle_arg_u32(cpu, 0);
    u32 aram = hle_arg_u32(cpu, 1);
    u32 length = hle_arg_u32(cpu, 2);
    u32 callback = hle_arg_u32(cpu, 4);
    u32 user = hle_arg_u32(cpu, 5);
    aram_dma_to_aram(cpu->ram, mainmem, aram, length);
    if (callback) {
        cpu->gpr[3] = user;
        cpu->pc = callback;
        g_hle_tail_call = true;
    }
}

// Some MusyX builds inline aramUploadData into aramInit, leaving ARQPostRequest
// as the actual hardware boundary. Complete the transfer immediately and
// tail-call ARQ's completion routine with the request pointer. The caller
// increments its queue count after ARQPostRequest returns; MusyX's callback
// decrements it first, so the original bookkeeping still settles at zero.
static void hle_ARQPostRequest(CPUState* cpu) {
    u32 request = hle_arg_u32(cpu, 0);
    u32 type = hle_arg_u32(cpu, 2);
    u32 source = hle_arg_u32(cpu, 4);
    u32 destination = hle_arg_u32(cpu, 5);
    u32 length = hle_arg_u32(cpu, 6);
    u32 callback = hle_arg_u32(cpu, 7);
    if (type == 0)
        aram_dma_to_aram(cpu->ram, source, destination, length);
    else
        aram_dma_to_ram(cpu->ram, destination, source, length);
    if (callback) {
        cpu->gpr[3] = request;
        cpu->pc = callback;
        g_hle_tail_call = true;
    }
}



static u32 guest_hash_bytes(const void* data, u32 bytes) {
    const u8* p = (const u8*)data;
    u32 hash = 2166136261u;
    for (u32 i = 0; i < bytes; i++)
        hash = (hash ^ p[i]) * 16777619u;
    return hash ? hash : 1u;
}

static u32 guest_hash(CPUState* cpu, u32 address, u32 bytes,
                      u32* hashed_bytes) {
    u32 available = 0;
    const void* data = mmio_guest_pointer(cpu, address, &available);
    if (data == NULL || bytes == 0 || available == 0) {
        if (hashed_bytes != NULL)
            *hashed_bytes = 0;
        return 0;
    }
    if (bytes > available)
        bytes = available;
    if (hashed_bytes != NULL)
        *hashed_bytes = bytes;
    return guest_hash_bytes(data, bytes);
}

#define THP_SIMPLE_CONTROL 0x8032F000u



#ifdef STRIKERSRECOMP_AURORA
// ---------------------------------------------------------------------------
// Aurora graphics metadata
//
// Raw writes to the GameCube write-gather pipe are forwarded by mmio.c into
// Aurora's GX FIFO. Most GX state therefore needs no per-function marshalling.
// Pointer-bearing commands are the exception: the guest encodes 32-bit
// GameCube physical addresses, while Aurora needs native host pointers. Track
// the guest objects selected by GX and emit Aurora metadata immediately before
// each draw, after the guest's raw texture/state commands are already queued.
// ---------------------------------------------------------------------------
#define GX_MAX_TEXTURES 8u
#define GX_MAX_TLUTS    20u

typedef struct {
    bool valid;
    u32 object;
    u32 guest_data;
    u32 width;
    u32 height;
    u32 format;
    u32 tlut;
    u32 version;
    u32 emitted_version;
    u32 data_bytes;
    u32 data_hash;
    u8 flags;
} GxTextureBinding;

typedef struct {
    bool valid;
    u32 object;
    u32 guest_data;
    u32 format;
    u32 version;
    u32 emitted_version;
    u32 data_bytes;
    u32 data_hash;
    u16 entries;
} GxTlutBinding;

static GxTextureBinding g_textures[GX_MAX_TEXTURES];
static GxTlutBinding g_tluts[GX_MAX_TLUTS];
static u32 g_array_address[32];
static u8 g_array_stride[32];
static unsigned g_gx_begin_count;
static unsigned g_gx_array_count;
// Per-frame accounting of indexed-array uploads. Aurora uploads each distinct
// indexed array's *whole advertised size* into a fixed, non-growable 8 MB
// per-frame storage buffer (aurora/lib/gfx StorageBufferSize); if the sum of
// all arrays bound in one frame exceeds that, the non-owned ByteBuffer aborts
// (ByteBuffer::resize, m_owned==false). We watch the running total so a future
// heavy scene warns loudly instead of crashing silently.
static u64 g_frame_array_bytes;
static unsigned g_frame_array_calls;
static u64 g_frame_array_bytes_peak;
static bool g_array_budget_warned;
// Aurora's per-frame storage buffer is 8 MB; warn once past 75% of it.
enum { AURORA_STORAGE_BUDGET = 8u * 1024u * 1024u };
enum { ARRAY_UPLOAD_WARN_BYTES = (AURORA_STORAGE_BUDGET * 3u) / 4u };
// GXSetArray carries no vertex count (smstrikers-decomp NL/glx/glxSend.cpp:1708
// passes only base+stride). With the local Aurora span patch live, push_gx_draw
// uploads only each draw's REAL indexed span min(array.size,(maxidx+1)*stride),
// so we advertise the FULL resolvable range and let the span patch bound the
// actual upload. No vertex cap is needed, and a cap can ONLY truncate geometry:
// the old flat 2048 cap truncated gameplay player/pitch meshes (which index
// >2048 verts per base) and collapsed the whole 3D scene to the clear color.
// g_max_array_verts is therefore an OPTIONAL DEBUG CEILING ONLY; 0 (the default)
// means uncapped. Set STRIKERS_MAX_ARRAY_VERTS only to deliberately truncate.
static u32 g_max_array_verts = 0u;
static unsigned g_vi_wait_count;
static unsigned g_movie_texture_load_count;
static u64 g_movie_cadence_present_count;
static u64 g_movie_cadence_texframe_changes;
static double g_movie_cadence_start_time;
static s32 g_movie_cadence_last_texframe;
static bool g_movie_cadence_started;

static void* graphics_guest_pointer(CPUState* cpu, u32 address, u32* available) {
    return mmio_guest_pointer(cpu, address, available);
}

static u32 gx_texture_base_bytes(u32 width, u32 height, u32 format) {
    u32 shift_x = 2;
    u32 shift_y = 2;
    switch (format) {
    case 0x00u: // GX_TF_I4
    case 0x08u: // GX_TF_C4
    case 0x0Eu: // GX_TF_CMPR
    case 0x20u: // GX_CTF_R4
    case 0x30u: // GX_CTF_Z4
        shift_x = 3;
        shift_y = 3;
        break;
    case 0x01u: // GX_TF_I8
    case 0x02u: // GX_TF_IA4
    case 0x09u: // GX_TF_C8
    case 0x11u: // GX_TF_Z8
    case 0x22u: // GX_CTF_RA4
    case 0x27u: // GX_CTF_A8
    case 0x28u: // GX_CTF_R8
    case 0x29u: // GX_CTF_G8
    case 0x2Au: // GX_CTF_B8
    case 0x39u: // GX_CTF_Z8M
    case 0x3Au: // GX_CTF_Z8L
        shift_x = 3;
        shift_y = 2;
        break;
    default:
        shift_x = 2;
        shift_y = 2;
        break;
    }

    const u32 tile_x = (width + (1u << shift_x) - 1u) >> shift_x;
    const u32 tile_y = (height + (1u << shift_y) - 1u) >> shift_y;
    const u32 block_bits = (format == 0x06u || format == 0x16u) ? 64u : 32u;
    return block_bits * tile_x * tile_y;
}

static u32 gx_hash_token_mix(u32 hash, u32 value) {
    for (u32 i = 0; i < 4u; i++) {
        hash = (hash ^ (u8)(value >> (i * 8u))) * 16777619u;
    }
    return hash ? hash : 1u;
}

static u32 gx_texture_resource_version(u32 width, u32 height, u32 format,
                                       u32 tlut, u8 flags,
                                       u32 data_bytes, u32 data_hash) {
    u32 hash = 2166136261u;
    hash = gx_hash_token_mix(hash, 0x54455831u);
    hash = gx_hash_token_mix(hash, width);
    hash = gx_hash_token_mix(hash, height);
    hash = gx_hash_token_mix(hash, format);
    hash = gx_hash_token_mix(hash, tlut);
    hash = gx_hash_token_mix(hash, flags);
    hash = gx_hash_token_mix(hash, data_bytes);
    return gx_hash_token_mix(hash, data_hash);
}

static u32 gx_tlut_resource_version(u32 format, u16 entries, u32 data_bytes,
                                    u32 data_hash) {
    u32 hash = 2166136261u;
    hash = gx_hash_token_mix(hash, 0x544C5431u);
    hash = gx_hash_token_mix(hash, format);
    hash = gx_hash_token_mix(hash, entries);
    hash = gx_hash_token_mix(hash, data_bytes);
    return gx_hash_token_mix(hash, data_hash);
}

static bool thp_movie_plane_generation(CPUState* cpu, u32 guest_data,
                                       u32 width, u32 height, u32 format,
                                       u32* data_bytes, u32* data_hash,
                                       u32* cache_object) {
    if (format != 1u)
        return false;

    const u32 base = THP_SIMPLE_CONTROL;
    const u32 y = mem_read32(cpu, base + 0x15Cu);
    const u32 u = mem_read32(cpu, base + 0x160u);
    const u32 v = mem_read32(cpu, base + 0x164u);
    bool matches_plane = false;
    if (guest_data == y && width == 512u && height == 416u)
        matches_plane = true;
    else if ((guest_data == u || guest_data == v) &&
             width == 256u && height == 208u)
        matches_plane = true;
    if (!matches_plane)
        return false;

    const s32 tex_frame = (s32)mem_read32(cpu, base + 0x168u);
    if (tex_frame < 0)
        return false;

    const u32 bytes = gx_texture_base_bytes(width, height, format);
    u32 hash = 2166136261u;
    hash = gx_hash_token_mix(hash, 0x54485031u);
    hash = gx_hash_token_mix(hash, guest_data);
    hash = gx_hash_token_mix(hash, (u32)tex_frame);
    hash = gx_hash_token_mix(hash, width);
    hash = gx_hash_token_mix(hash, height);
    hash = gx_hash_token_mix(hash, format);
    if (data_bytes != NULL)
        *data_bytes = bytes;
    if (data_hash != NULL)
        *data_hash = hash;
    if (cache_object != NULL)
        *cache_object = guest_data;
    return true;
}

static double host_time_seconds(void) {
    struct timespec ts;
    timespec_get(&ts, TIME_UTC);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1000000000.0;
}

static u32 thp_audio_valid_samples(CPUState* cpu, u32* output_valid) {
    const u32 base = THP_SIMPLE_CONTROL;
    u32 total = 0;
    for (u32 i = 0; i < 6u; i++)
        total += mem_read32(cpu, base + 0x16Cu + i * 12u + 8u);

    if (output_valid != NULL) {
        const u32 output_index = mem_read32(cpu, base + 0x1B8u);
        *output_valid = output_index < 6u
            ? mem_read32(cpu, base + 0x16Cu + output_index * 12u + 8u)
            : 0u;
    }
    return total;
}

static void movie_cadence_present(CPUState* cpu) {
    if (!g_movie_cadence_log)
        return;

    const u32 base = THP_SIMPLE_CONTROL;
    if (mem_read8(cpu, base + 0x6Cu) == 0u) {
        g_movie_cadence_started = false;
        return;
    }

    const s32 tex_frame = (s32)mem_read32(cpu, base + 0x168u);
    const double now = host_time_seconds();
    if (!g_movie_cadence_started) {
        g_movie_cadence_started = true;
        g_movie_cadence_start_time = now;
        g_movie_cadence_present_count = 0;
        g_movie_cadence_texframe_changes = 0;
        g_movie_cadence_last_texframe = tex_frame;
    }

    g_movie_cadence_present_count++;
    if (tex_frame != g_movie_cadence_last_texframe) {
        g_movie_cadence_texframe_changes++;
        g_movie_cadence_last_texframe = tex_frame;
    }

    const double elapsed = now - g_movie_cadence_start_time;
    if (elapsed >= 1.0) {
        u32 audio_output_valid = 0;
        const u32 audio_valid = thp_audio_valid_samples(cpu, &audio_output_valid);
        fprintf(stderr,
                "[movie-cadence] seconds=%.3f presents=%llu "
                "texframe_changes=%llu last_texframe=%d audio_dec=%u "
                "audio_out=%u audio_valid=%u audio_out_valid=%u\n",
                elapsed,
                (unsigned long long)g_movie_cadence_present_count,
                (unsigned long long)g_movie_cadence_texframe_changes,
                g_movie_cadence_last_texframe,
                mem_read32(cpu, base + 0x1B4u),
                mem_read32(cpu, base + 0x1B8u),
                audio_valid,
                audio_output_valid);
        g_movie_cadence_start_time = now;
        g_movie_cadence_present_count = 0;
        g_movie_cadence_texframe_changes = 0;
    }
}

static void guest_write_f32(CPUState* cpu, u32 address, f32 value) {
    u32 bits;
    memcpy(&bits, &value, sizeof bits);
    mem_write32(cpu, address, bits);
}

// PSMTXConcat is pure SDK math, but its paired-single implementation is used
// on virtually every rendered packet. Keep a portable scalar implementation at
// the HLE boundary so host floating-point behavior cannot corrupt transforms.
static void hle_PSMTXConcat(CPUState* cpu) {
    const u32 a_address = hle_arg_u32(cpu, 0);
    const u32 b_address = hle_arg_u32(cpu, 1);
    const u32 out_address = hle_arg_u32(cpu, 2);
    f32 a[12], b[12], out[12];

    for (u32 i = 0; i < 12; i++) {
        a[i] = guest_read_f32(cpu, a_address + i * 4u);
        b[i] = guest_read_f32(cpu, b_address + i * 4u);
    }
    for (u32 row = 0; row < 3; row++) {
        for (u32 col = 0; col < 3; col++) {
            out[row * 4u + col] =
                (f32)((f32)(a[row * 4u] * b[col]) +
                      (f32)(a[row * 4u + 1u] * b[4u + col]) +
                      (f32)(a[row * 4u + 2u] * b[8u + col]));
        }
        out[row * 4u + 3u] =
            (f32)(a[row * 4u + 3u] +
                  (f32)(a[row * 4u] * b[3u]) +
                  (f32)(a[row * 4u + 1u] * b[7u]) +
                  (f32)(a[row * 4u + 2u] * b[11u]));
    }
    for (u32 i = 0; i < 12; i++)
        guest_write_f32(cpu, out_address + i * 4u, out[i]);
}

// GXSetArray(GXAttr attr, void* base, u8 stride). Replace the hardware CP-base
// write with Aurora's extended command carrying a real host pointer.
static void hle_GXSetArray(CPUState* cpu) {
    u32 attr = hle_arg_u32(cpu, 0);
    u32 address = hle_arg_u32(cpu, 1);
    u8 stride = (u8)hle_arg_u32(cpu, 2);
    u32 available = 0;
    void* data = graphics_guest_pointer(cpu, address, &available);
    // GXSetArray carries no length on real hardware, but Aurora's FIFO path
    // uploads the whole advertised array into a fixed per-frame storage buffer
    // (8 MB) every time the array is used, once per distinct base. The game
    // re-bases the position/color/texcoord arrays per object and indexes locally
    // from each base, so a single frame can bind dozens of arrays; advertising
    // the 16-bit index ceiling, or even stride*8192, summed past 8 MB once enough
    // character models were on screen (e.g. both team captains on the VS screen)
    // and aborted Aurora's non-owned storage ByteBuffer. Bound the advertised
    // range to a realistic per-mesh vertex count: small enough to keep the total
    // far under 8 MB, large enough to cover every index a real draw references.
    // Advertise the full resolvable range; Aurora's span patch uploads only the
    // real per-draw indexed span, so this never inflates storage and never
    // truncates geometry. An explicit STRIKERS_MAX_ARRAY_VERTS (g_max_array_verts
    // != 0) applies a debug ceiling only.
    u32 size = available;
    if (g_max_array_verts != 0u) {
        u64 indexed_span = (u64)(stride ? stride : 1u) * g_max_array_verts;
        if (indexed_span < (u64)size)
            size = (u32)indexed_span;
    }
    if (attr < 32u) {
        g_array_address[attr] = address;
        g_array_stride[attr] = stride;
    }
    if (g_graphics_log && (++g_gx_array_count <= 24 || size > 500000u))
        fprintf(stderr, "[gfx] array attr=%u guest=0x%08X host=%p size=%u stride=%u available=%u\n",
                attr, address, data, size, stride, available);
    g_frame_array_bytes += size;
    g_frame_array_calls++;
    // The advertised-byte budget warn modelled the OLD whole-array upload; with
    // the span patch the real upload is per-draw span-bounded, so this only makes
    // sense (and only risks false positives) when an explicit cap is set.
    if (g_max_array_verts != 0u &&
        g_frame_array_bytes > ARRAY_UPLOAD_WARN_BYTES && !g_array_budget_warned) {
        g_array_budget_warned = true;
        fprintf(stderr,
                "[gfx] WARNING: indexed-array uploads this frame reached %llu bytes, "
                "nearing Aurora's %u-byte per-frame storage limit. Lower "
                "STRIKERS_MAX_ARRAY_VERTS (currently %u) if this scene aborts.\n",
                (unsigned long long)g_frame_array_bytes,
                (unsigned)AURORA_STORAGE_BUDGET, g_max_array_verts);
    }
    dol_platform_set_array_guest(attr, address, data, size, stride);
}

static void emit_gx_resource_metadata(CPUState* cpu);

static void finish_GXCallDisplayList(CPUState* cpu) {
    u32 available = 0;
    void* data = graphics_guest_pointer(
        cpu, g_display_list_call.address, &available);
    if (data == NULL || g_display_list_call.size > available) {
        fprintf(stderr,
                "[gx] GXCallDisplayList unresolved address=0x%08X size=%u "
                "available=%u\n",
                g_display_list_call.address, g_display_list_call.size,
                available);
    } else {
        emit_gx_resource_metadata(cpu);
        dol_platform_call_display_list(data, g_display_list_call.size);
    }
    cpu->lr = g_display_list_call.caller;
    cpu->pc = g_display_list_call.caller;
    g_display_list_call.active = false;
}

static void continue_GXCallDisplayList(CPUState* cpu) {
    const u32 gx_data =
        mem_read32(cpu, cpu->gpr[2] + (u32)(s32)GX_DATA_SDA_OFFSET);
    if (g_display_list_call.stage == 0u &&
        mem_read32(cpu, gx_data + GX_DIRTY_STATE_OFFSET) != 0u) {
        g_display_list_call.stage = 1u;
        cpu->lr = HLE_DISPLAY_LIST_RETURN;
        cpu->pc = GX_DIRTY_STATE_HELPER;
        return;
    }
    if (g_display_list_call.stage <= 1u &&
        mem_read32(cpu, gx_data) != 0u) {
        g_display_list_call.stage = 2u;
        cpu->lr = HLE_DISPLAY_LIST_RETURN;
        cpu->pc = GX_FLUSH_PRIM_HELPER;
        return;
    }
    finish_GXCallDisplayList(cpu);
}

static void hle_GXCallDisplayList(CPUState* cpu) {
    if (g_display_list_call.active) {
        fprintf(stderr, "[gx] nested GXCallDisplayList host continuation\n");
        return;
    }
    g_display_list_call.active = true;
    g_display_list_call.stage = 0u;
    g_display_list_call.caller = cpu->lr;
    g_display_list_call.address = hle_arg_u32(cpu, 0);
    g_display_list_call.size = hle_arg_u32(cpu, 1);
    g_hle_tail_call = true;
    continue_GXCallDisplayList(cpu);
}

static void notify_GXBegin(CPUState* cpu);

static void finish_GXBegin(CPUState* cpu) {
    emit_gx_resource_metadata(cpu);
    dol_platform_mark_gx_begin();
    mem_write8(cpu, 0xCC008000u,
               (u8)(g_gx_begin.primitive | g_gx_begin.vtxfmt));
    mem_write16(cpu, 0xCC008000u, g_gx_begin.vertex_count);
    cpu->lr = g_gx_begin.caller;
    cpu->pc = g_gx_begin.caller;
    g_gx_begin.active = false;
}

static void continue_GXBegin(CPUState* cpu) {
    const u32 gx_data =
        mem_read32(cpu, cpu->gpr[2] + (u32)(s32)GX_DATA_SDA_OFFSET);
    if (g_gx_begin.stage == 0u &&
        mem_read32(cpu, gx_data + GX_DIRTY_STATE_OFFSET) != 0u) {
        g_gx_begin.stage = 1u;
        cpu->lr = HLE_GX_BEGIN_RETURN;
        cpu->pc = GX_DIRTY_STATE_HELPER;
        return;
    }
    if (g_gx_begin.stage <= 1u && mem_read32(cpu, gx_data) != 0u) {
        g_gx_begin.stage = 2u;
        cpu->lr = HLE_GX_BEGIN_RETURN;
        cpu->pc = GX_FLUSH_PRIM_HELPER;
        return;
    }
    finish_GXBegin(cpu);
}

static void hle_GXBegin(CPUState* cpu) {
    if (g_gx_begin.active) {
        fprintf(stderr, "[gx] nested GXBegin host continuation\n");
        cpu->pc = cpu->lr;
        return;
    }
    notify_GXBegin(cpu);
    g_gx_begin.active = true;
    g_gx_begin.stage = 0u;
    g_gx_begin.primitive = (u8)hle_arg_u32(cpu, 0);
    g_gx_begin.vtxfmt = (u8)hle_arg_u32(cpu, 1);
    g_gx_begin.vertex_count = (u16)hle_arg_u32(cpu, 2);
    g_gx_begin.caller = cpu->lr;
    g_hle_tail_call = true;
    continue_GXBegin(cpu);
}

static void record_GXLoadTexObj(CPUState* cpu, u32 obj, u32 slot) {
    static unsigned texture_trace_count = 0;
    static bool texture_watch_initialized = false;
    static u32 texture_watch_address = 0;
    static unsigned texture_watch_begin = UINT_MAX;
    if (slot >= GX_MAX_TEXTURES)
        return;
    u32 image0 = mem_read32(cpu, obj + 8u);
    u32 image3 = mem_read32(cpu, obj + 12u);
    u32 guest_data = GC_RAM_BASE | ((image3 & 0x001FFFFFu) << 5);
    u32 width = (image0 & 0x3FFu) + 1u;
    u32 height = ((image0 >> 10) & 0x3FFu) + 1u;
    u32 format = mem_read32(cpu, obj + 20u);
    u32 tlut = mem_read32(cpu, obj + 24u);
    u8 flags = mem_read8(cpu, obj + 31u);
    u32 data_bytes = gx_texture_base_bytes(width, height, format);
    u32 data_hash = 0;
    u32 cache_object = obj;
    const bool movie_generation =
        thp_movie_plane_generation(cpu, guest_data, width, height, format,
                                   &data_bytes, &data_hash, &cache_object);
    if (!movie_generation)
        data_hash = guest_hash(cpu, guest_data, data_bytes, &data_bytes);
    const u32 version =
        gx_texture_resource_version(width, height, format, tlut, flags,
                                    data_bytes, data_hash);
    if (!movie_generation)
        cache_object = version;
    if (!texture_watch_initialized) {
        const char* watch = getenv("STRIKERS_TEXTURE_WATCH");
        texture_watch_address =
            watch != NULL ? (u32)strtoul(watch, NULL, 0) : 0u;
        texture_watch_initialized = true;
    }
    if (guest_data == texture_watch_address &&
        texture_watch_begin != g_gx_begin_count) {
        texture_watch_begin = g_gx_begin_count;
        fprintf(stderr,
                "[texture-watch] begin=%u slot=%u obj=%08X data=%08X "
                "%ux%u fmt=%u flags=%02X hash=%08X id=%08X version=%08X\n",
                g_gx_begin_count, slot, obj, guest_data, width, height, format,
                flags, data_hash, cache_object, version);
    }
    if (g_mash_route_complete && getenv("STRIKERS_TEXTURE_CACHE_LOG") != NULL &&
        texture_trace_count < 256u) {
        ++texture_trace_count;
        fprintf(stderr,
                "[texture-cache] load=%u begin=%u slot=%u obj=%08X data=%08X "
                "%ux%u fmt=%u tlut=%u flags=%02X bytes=%u hash=%08X "
                "id=%08X version=%08X movie=%u\n",
                texture_trace_count, g_gx_begin_count, slot, obj, guest_data,
                width, height, format, tlut, flags, data_bytes, data_hash,
                cache_object, version, movie_generation ? 1u : 0u);
    }
    GxTextureBinding* binding = &g_textures[slot];
    bool changed = !binding->valid ||
                   binding->object != cache_object ||
                   binding->version != version;
    binding->valid = true;
    binding->object = cache_object;
    binding->guest_data = guest_data;
    binding->width = width;
    binding->height = height;
    binding->format = format;
    binding->tlut = tlut;
    binding->flags = flags;
    binding->data_bytes = data_bytes;
    binding->data_hash = data_hash;
    binding->version = version;
    // The retail BP texture-load sequence clears Aurora's recomp metadata.
    // Re-emit the stable identity after every guest bind, even when the bytes
    // are unchanged, so the renderer can hit its converted-texture cache.
    binding->emitted_version = 0;
    if (g_movie_log && format == 1u) {
        unsigned seq = ++g_movie_texobj_log_count;
        bool should_log = changed || seq <= 96u || (seq % 60u) == 0u;
        if (should_log) {
        fprintf(stderr,
                "[movie] GXLoadTexObj slot=%u obj=0x%08X data=0x%08X "
                "%ux%u fmt=%u bytes=%u hash=%08X changed=%u ver=%u seq=%u "
                "gen=%u cache=0x%08X lr=0x%08X\n",
                slot, obj, guest_data, width, height, format, data_bytes,
                data_hash, changed ? 1u : 0u, binding->version, seq,
                movie_generation ? 1u : 0u, cache_object, cpu->lr);
        }
    }
}

#endif

#ifdef STRIKERSRECOMP_AURORA

static bool guest_backchain_contains(CPUState* cpu, u32 start, u32 end) {
    if (cpu->lr >= start && cpu->lr < end)
        return true;
    u32 sp = cpu->gpr[1];
    const u32 ram_end = GC_RAM_BASE + cpu->ram_size;
    for (unsigned frame = 0; frame < 24u; ++frame) {
        if (sp < GC_RAM_BASE || sp + 8u > ram_end)
            break;
        const u32 caller = mem_read32(cpu, sp);
        if (caller <= sp || caller < GC_RAM_BASE || caller + 8u > ram_end)
            break;
        const u32 ret = mem_read32(cpu, caller + 4u);
        if (ret >= start && ret < end)
            return true;
        sp = caller;
    }
    return false;
}



// GXLoadPosMtxImm(MtxPtr mtx, u32 pnIdx): every 3D actor/camera modelview is
// loaded here. The frame-end gx_modelview snapshot only catches the LAST matrix
// (a 2D/overlay layer), so to tell whether the recomp ever applies the real 3D
// camera tilt we sample the matrices loaded mid-frame. To avoid flooding we log
// only matrices with a meaningful rotation (|row1.z| or |row2.y| > 0.1, the
// camera-tilt elements) plus a per-frame count of how many loads carried tilt.
static unsigned g_posmtx_loads_this_frame = 0;
static unsigned g_posmtx_tilted_this_frame = 0;
static unsigned g_posmtx_logged = 0;
static void notify_GXLoadPosMtxImm(CPUState* cpu) {
    static int matrix_log_enabled = -1;
    if (matrix_log_enabled < 0)
        matrix_log_enabled = getenv("STRIKERS_MATRIX_LOG") != NULL ? 1 : 0;
    const bool ball_load =
        ball_state_log_enabled() &&
        guest_backchain_contains(cpu, 0x8011DF10u, 0x8011E38Cu);
    if (!matrix_log_enabled && !ball_load)
        return;
    const u32 mtx_addr = hle_arg_u32(cpu, 0);
    const u32 pn_idx = hle_arg_u32(cpu, 1);
    u8 raw[48];
    copy_guest_to_host(cpu, mtx_addr, raw, sizeof raw);
    float m[12];
    for (int i = 0; i < 12; i++) {
        u32 b = ((u32)raw[i * 4] << 24) | ((u32)raw[i * 4 + 1] << 16) |
                ((u32)raw[i * 4 + 2] << 8) | (u32)raw[i * 4 + 3];
        memcpy(&m[i], &b, sizeof(float));
    }
    if (ball_load) {
        static unsigned ball_matrix_count = 0;
        ++ball_matrix_count;
        if (ball_matrix_count <= 40u || (ball_matrix_count % 60u) == 0u) {
            fprintf(stderr,
                    "[ball-posmtx] load=%u guest-frame=%llu begin=%u "
                    "addr=0x%08X pn=%u lr=0x%08X\n"
                    "  % .7g % .7g % .7g % .7g\n"
                    "  % .7g % .7g % .7g % .7g\n"
                    "  % .7g % .7g % .7g % .7g\n",
                    ball_matrix_count,
                    (unsigned long long)(cpu->timebase / 675000ull),
                    g_gx_begin_count, mtx_addr, pn_idx, cpu->lr,
                    (double)m[0], (double)m[1], (double)m[2], (double)m[3],
                    (double)m[4], (double)m[5], (double)m[6], (double)m[7],
                    (double)m[8], (double)m[9], (double)m[10], (double)m[11]);
        }
    }
    if (!matrix_log_enabled)
        return;
    ++g_posmtx_loads_this_frame;
    // Row1.z = m[6], Row2.y = m[9] carry the camera pitch/tilt rotation.
    const float tilt = (m[6] < 0 ? -m[6] : m[6]) + (m[9] < 0 ? -m[9] : m[9]);
    if (tilt > 0.1f) {
        ++g_posmtx_tilted_this_frame;
        if (g_posmtx_logged < 60u) {
            ++g_posmtx_logged;
            fprintf(stderr,
                    "[posmtx] id=%u TILTED:\n  % .5g % .5g % .5g % .5g\n"
                    "  % .5g % .5g % .5g % .5g\n  % .5g % .5g % .5g % .5g\n",
                    pn_idx, (double)m[0], (double)m[1], (double)m[2], (double)m[3],
                    (double)m[4], (double)m[5], (double)m[6], (double)m[7],
                    (double)m[8], (double)m[9], (double)m[10], (double)m[11]);
        }
    }
}

static void notify_GXLoadTexObj(CPUState* cpu) {
    record_GXLoadTexObj(cpu, hle_arg_u32(cpu, 0), hle_arg_u32(cpu, 1));
}

static void notify_GXLoadTlut(CPUState* cpu) {
    u32 slot = hle_arg_u32(cpu, 1);
    if (slot >= GX_MAX_TLUTS)
        return;
    u32 obj = hle_arg_u32(cpu, 0);
    u32 load_tlut0 = mem_read32(cpu, obj + 4u);
    u32 guest_data = GC_RAM_BASE | ((load_tlut0 & 0x001FFFFFu) << 5);
    u32 format = (mem_read32(cpu, obj) >> 10) & 3u;
    u16 entries = mem_read16(cpu, obj + 8u);
    u32 data_bytes = (u32)entries * 2u;
    u32 data_hash = guest_hash(cpu, guest_data, data_bytes, &data_bytes);
    const u32 version =
        gx_tlut_resource_version(format, entries, data_bytes, data_hash);
    GxTlutBinding* binding = &g_tluts[slot];
    binding->valid = true;
    binding->object = version;
    binding->guest_data = guest_data;
    binding->format = format;
    binding->entries = entries;
    binding->data_bytes = data_bytes;
    binding->data_hash = data_hash;
    binding->version = version;
    binding->emitted_version = 0;
}

static void emit_gx_resource_metadata(CPUState* cpu) {
    for (u32 slot = 0; slot < GX_MAX_TLUTS; slot++) {
        GxTlutBinding* binding = &g_tluts[slot];
        if (!binding->valid)
            continue;
        if (binding->emitted_version == binding->version)
            continue;
        void* data = graphics_guest_pointer(cpu, binding->guest_data, NULL);
        dol_platform_load_tlut_guest((u8)slot, binding->guest_data, data,
                                     binding->format, binding->entries,
                                     binding->object, binding->version);
        binding->emitted_version = binding->version;
    }

    for (u32 slot = 0; slot < GX_MAX_TEXTURES; slot++) {
        GxTextureBinding* binding = &g_textures[slot];
        if (!binding->valid)
            continue;
        if (binding->emitted_version == binding->version)
            continue;
        void* data = graphics_guest_pointer(cpu, binding->guest_data, NULL);
        if (g_graphics_log && g_gx_begin_count == 1 && data != NULL) {
            u64 pixels = (u64)binding->width * binding->height;
            u32 bytes = (binding->format == 0u || binding->format == 8u ||
                         binding->format == 14u)
                            ? (u32)((pixels + 1u) / 2u)
                            : (binding->format == 1u || binding->format == 2u ||
                               binding->format == 9u)
                                  ? (u32)pixels
                                  : (binding->format == 6u) ? (u32)(pixels * 4u)
                                                            : (u32)(pixels * 2u);
            const u8* source = (const u8*)data;
            u32 nonzero = 0;
            u32 hash = 2166136261u;
            for (u32 i = 0; i < bytes; i++) {
                nonzero += source[i] != 0;
                hash = (hash ^ source[i]) * 16777619u;
            }
            fprintf(stderr, "[gfx] tex-data slot=%u bytes=%u nonzero=%u hash=%08X\n",
                    slot, bytes, nonzero, hash);
        }
        if (g_graphics_log && g_gx_begin_count < 12)
            fprintf(stderr,
                    "[gfx] tex slot=%u obj=0x%08X data=0x%08X host=%p "
                    "%ux%u fmt=%u tlut=%u flags=0x%02X ver=%u\n",
                    slot, binding->object, binding->guest_data, data,
                    binding->width, binding->height, binding->format,
                    binding->tlut, binding->flags, binding->version);
        if (g_movie_log && binding->format == 1u) {
            const unsigned movie_tex_seq = ++g_movie_texture_load_count;
            if (movie_tex_seq < 96u || (movie_tex_seq % 60u) == 0u) {
                fprintf(stderr,
                        "[movie] draw-tex slot=%u data=0x%08X %ux%u fmt=I8 "
                        "bytes=%u hash=%08X ver=%u begin=%u seq=%u\n",
                        slot, binding->guest_data, binding->width, binding->height,
                        binding->data_bytes, binding->data_hash, binding->version,
                        g_gx_begin_count, movie_tex_seq);
            }
        }
        dol_platform_load_texture_guest(
            (u8)slot, binding->guest_data, data, binding->width,
            binding->height, binding->format, binding->tlut,
            (binding->flags & 1u) != 0, binding->object, binding->version);
        binding->emitted_version = binding->version;
    }
}

static void notify_GXBegin(CPUState* cpu) {
    ++g_gx_begin_count;
    if (ball_state_log_enabled() &&
        guest_backchain_contains(cpu, 0x8011DF10u, 0x8011E38Cu)) {
        fprintf(stderr,
                "[ball-draw] guest-frame=%llu begin=%u prim=%u fmt=%u "
                "count=%u lr=0x%08X\n",
                (unsigned long long)(cpu->timebase / 675000ull),
                g_gx_begin_count, hle_arg_u32(cpu, 0), hle_arg_u32(cpu, 1),
                hle_arg_u32(cpu, 2), cpu->lr);
    }
    if (g_graphics_log && g_gx_begin_count <= 12)
        fprintf(stderr,
                "[gfx] begin #%u prim=%u fmt=%u count=%u lr=0x%08X\n",
                g_gx_begin_count, hle_arg_u32(cpu, 0), hle_arg_u32(cpu, 1),
                hle_arg_u32(cpu, 2), cpu->lr);
    if (g_graphics_log && g_gx_begin_count == 1) {
        u32 packet = cpu->gpr[30];
        fprintf(stderr,
                "[gfx] packet=0x%08X index=0x%08X vertices=%u prim=%u "
                "streams=%u stream-list=0x%08X\n",
                packet, mem_read32(cpu, packet + 4u), mem_read16(cpu, packet + 8u),
                mem_read8(cpu, packet + 0x0Au), mem_read8(cpu, packet + 0x0Bu),
                mem_read32(cpu, packet + 0x0Cu));
        fprintf(stderr,
                "[gfx] packet-state texture=%08X%08X material=0x%08X "
                "program=0x%08X raster=0x%08X matrix=0x%08X "
                "textures=%08X,%08X,%08X,%08X,%08X,%08X "
                "texconfig=%u user-key=0x%08X material-set=0x%08X\n",
                mem_read32(cpu, packet + 0x10u),
                mem_read32(cpu, packet + 0x14u),
                mem_read32(cpu, packet + 0x18u),
                mem_read32(cpu, packet + 0x1Cu),
                mem_read32(cpu, packet + 0x20u),
                mem_read32(cpu, packet + 0x24u),
                mem_read32(cpu, packet + 0x28u),
                mem_read32(cpu, packet + 0x2Cu),
                mem_read32(cpu, packet + 0x30u),
                mem_read32(cpu, packet + 0x34u),
                mem_read32(cpu, packet + 0x38u),
                mem_read32(cpu, packet + 0x3Cu),
                mem_read8(cpu, packet + 0x40u),
                mem_read32(cpu, packet + 0x42u),
                mem_read32(cpu, packet + 0x46u));
        fprintf(stderr,
                "[gfx] state view=%u program=0x%08X texconfig=0x%08X "
                "viewport=%08X,%08X,%08X,%08X\n",
                mem_read32(cpu, 0x80374010u), mem_read32(cpu, 0x80372FA4u),
                mem_read32(cpu, 0x80374024u), mem_read32(cpu, 0x8032C238u),
                mem_read32(cpu, 0x8032C23Cu), mem_read32(cpu, 0x8032C240u),
                mem_read32(cpu, 0x8032C244u));
    }
}

static void notify_GXCopyTex(CPUState* cpu) {
    u32 address = hle_arg_u32(cpu, 0);
    void* data = graphics_guest_pointer(cpu, address, NULL);
    dol_platform_set_copy_destination_guest(address, data);
}

// GXCopyDisp(void* dest, GXBool clear) copies the EFB to the external frame
// buffer -- this is the game's real frame boundary (one per rendered frame).
// Drive Aurora's present here so each game frame is drained, rendered, and
// reset on its own. Presenting on VIWaitForRetrace instead let the front-end's
// load/transition loop (which renders many frames between retraces) pile every
// frame's geometry into a single Aurora frame packet, overflowing its fixed
// per-frame vertex/index staging buffers (ByteBuffer::resize abort). Aurora
// presents the EFB directly, so the stubbed display copy is harmless; we only
// need the boundary signal.
// Dump the game's GL/glx transform globals (G4QE01 addresses) as big-endian
// floats so the recomp's per-frame matrices can be diffed against Dolphin's
// View->Memory read of the same addresses. Gated by STRIKERS_MATRIX_LOG; rate
// limited so a long run does not flood. The conclusion fork: matching guest
// matrices + missing geometry -> bug is downstream (XF emission/Aurora); a
// degenerate/diverging matrix -> upstream recomp matrix math.
static void matrix_log_floats(CPUState* cpu, const char* name, u32 addr,
                              u32 count) {
    u8 raw[64];
    if (count * 4u > sizeof(raw))
        count = (u32)(sizeof(raw) / 4u);
    copy_guest_to_host(cpu, addr, raw, count * 4u);
    fprintf(stderr, "[matrix] %s @0x%08X:", name, addr);
    for (u32 i = 0; i < count; i++) {
        u32 bits = ((u32)raw[i * 4 + 0] << 24) | ((u32)raw[i * 4 + 1] << 16) |
                   ((u32)raw[i * 4 + 2] << 8) | (u32)raw[i * 4 + 3];
        float f;
        memcpy(&f, &bits, sizeof(f));
        if ((i & 3u) == 0u)
            fprintf(stderr, "\n  ");
        fprintf(stderr, " % .6g", (double)f);
    }
    fprintf(stderr, "\n");
}

static void matrix_log_frame(CPUState* cpu) {
    static int s_enabled = -1;
    static unsigned s_frame = 0;
    if (s_enabled < 0)
        s_enabled = getenv("STRIKERS_MATRIX_LOG") != NULL ? 1 : 0;
    if (!s_enabled)
        return;
    ++s_frame;
    if (s_frame > 20u && (s_frame % 60u) != 0u)
        return;
    fprintf(stderr, "[matrix] ---- frame %u ----\n", s_frame);
    matrix_log_floats(cpu, "gx_proj     ", 0x8032C090u, 16u); // projection 4x4
    matrix_log_floats(cpu, "gx_modelview", 0x8032C0D0u, 12u); // camera x object
    matrix_log_floats(cpu, "gx_mview    ", 0x8032C060u, 12u); // view (camera)
    // How many of this frame's GXLoadPosMtxImm loads carried a real 3D rotation.
    // Resolves whether the recomp ever applies the camera tilt (H2) or never
    // does (H1) — independent of the last-set frame-end gx_modelview.
    fprintf(stderr, "[matrix] posmtx loads=%u tilted=%u\n",
            g_posmtx_loads_this_frame, g_posmtx_tilted_this_frame);
    g_posmtx_loads_this_frame = 0;
    g_posmtx_tilted_this_frame = 0;
}

static void notify_GXCopyDisp(CPUState* cpu) {
    // TEMP DIAG (G013 cutscene): observe the guest transition-state flags each
    // frame so a live goal celebration can discriminate "guest chose not to
    // render the world" from "host skipped the draws". Gated by env.
    {
        static int s_cutscene_diag = -1;
        if (s_cutscene_diag < 0)
            s_cutscene_diag = getenv("STRIKERS_CUTSCENE_DIAG") != NULL ? 1 : 0;
        if (s_cutscene_diag) {
            static u64 s_diag_frame = 0;
            u8 render_world = mem_read8(cpu, 0x8037273Cu); // g_bRenderWorld
            fprintf(stderr, "[cutscene] frame=%llu g_bRenderWorld=%u\n",
                    (unsigned long long)++s_diag_frame, render_world);
        }
    }
    matrix_log_frame(cpu);
    if (g_frame_array_bytes > g_frame_array_bytes_peak)
        g_frame_array_bytes_peak = g_frame_array_bytes;
    if (g_graphics_log && g_frame_array_calls)
        fprintf(stderr,
                "[gfx] frame array uploads: calls=%u bytes=%llu (%.2f MB) peak=%.2f MB cap=8MB\n",
                g_frame_array_calls,
                (unsigned long long)g_frame_array_bytes,
                (double)g_frame_array_bytes / (1024.0 * 1024.0),
                (double)g_frame_array_bytes_peak / (1024.0 * 1024.0));
    g_frame_array_bytes = 0;
    g_frame_array_calls = 0;
    movie_cadence_present(cpu);
    dol_platform_present();
}

static void notify_VIConfigure(CPUState* cpu) {
    u32 mode = hle_arg_u32(cpu, 0);
    if (!mode)
        return;
    if (g_graphics_log)
        fprintf(stderr,
                "[gfx] VIConfigure mode=0x%08X tv=%u fb=%ux%u xfb=%u vi=%ux%u\n",
                mode, mem_read32(cpu, mode), mem_read16(cpu, mode + 4u),
                mem_read16(cpu, mode + 6u), mem_read16(cpu, mode + 8u),
                mem_read16(cpu, mode + 0x0Eu), mem_read16(cpu, mode + 0x10u));
    dol_platform_configure_vi(
        mem_read32(cpu, mode),
        mem_read16(cpu, mode + 4u),
        mem_read16(cpu, mode + 6u),
        mem_read16(cpu, mode + 8u),
        mem_read16(cpu, mode + 0x0Eu),
        mem_read16(cpu, mode + 0x10u));
}

static void notify_VIWaitForRetrace(CPUState* cpu) {
    ++g_vi_wait_count;
    if (g_graphics_log && (g_vi_wait_count <= 12u || g_vi_wait_count % 60u == 0u)) {
        u32 manager = mem_read32(cpu, 0x803742A4u);
        u32 active = manager ? mem_read32(cpu, manager + 0xA04u) : 0u;
        fprintf(stderr, "[gfx] retrace=%u async-manager=0x%08X active=0x%08X",
                g_vi_wait_count, manager, active);
        if (active) {
            u32 entry = mem_read32(cpu, active);
            fprintf(stderr,
                    " entry=0x%08X file=0x%08X callback=0x%08X buffer=0x%08X "
                    "size=%u position=%u phase=%u remaining=%u",
                    entry, mem_read32(cpu, entry + 8u), mem_read32(cpu, entry + 0x0Cu),
                    mem_read32(cpu, entry + 0x10u), mem_read32(cpu, entry + 0x14u),
                    mem_read32(cpu, entry + 0x18u), mem_read32(cpu, entry + 0x20u),
                    mem_read32(cpu, entry + 0x24u));
        }
        fputc('\n', stderr);
    }
    // The frame is presented from GXCopyDisp (the real frame boundary); the VI
    // retrace only paces the game (handled by the retrace interrupt).
}

static void hle_PADInit(CPUState* cpu) {
    hle_set_u32(cpu, dol_platform_pad_init() ? 1u : 0u);
}

static void hle_PADRead(CPUState* cpu) {
    u32 out = hle_arg_u32(cpu, 0);
    DolPadState state[4];
    memset(state, 0, sizeof state);
    u32 motor_mask = dol_platform_pad_read(state);
    if (g_auto_input) {
        const u64 frame = cpu->timebase / 675000ull;
        if (frame >= 135u && ((frame - 135u) % 90u) < 12u) {
            state[0].button |= 0x0100u;
            state[0].analog_a = 0xFFu;
            if (g_auto_input_last_pulse != frame) {
                fprintf(stderr, "[input] scripted A at frame %llu\n",
                        (unsigned long long)frame);
                g_auto_input_last_pulse = frame;
            }
        }
        state[0].error = 0;
    }
    if (g_auto_input_once_frame != 0) {
        const u64 frame = cpu->timebase / 675000ull;
        if (frame >= g_auto_input_once_frame &&
            frame < g_auto_input_once_frame + 8u &&
            g_auto_input_once_sent != g_auto_input_once_frame) {
            state[0].button |= 0x0100u;
            state[0].analog_a = 0xFFu;
            state[0].error = 0;
            fprintf(stderr, "[input] one-shot A at frame %llu\n",
                    (unsigned long long)frame);
            g_auto_input_once_sent = g_auto_input_once_frame;
        }
    }
    if (g_auto_skip_card_prompt) {
        const u64 frame = cpu->timebase / 675000ull;
        u16 scripted = 0;
        const char* action = NULL;
        if (frame >= 360u && frame < 372u) {
            scripted = 0x0004u; // PAD_BUTTON_DOWN
            action = "Down";
        } else if (frame >= 420u && frame < 432u) {
            scripted = 0x0100u; // PAD_BUTTON_A
            action = "A";
        }
        if (scripted != 0) {
            state[0].button |= scripted;
            if (scripted == 0x0004u)
                state[0].stick_y = -80;
            if (scripted == 0x0100u)
                state[0].analog_a = 0xFFu;
            state[0].error = 0;
            if (g_auto_skip_card_last_pulse != frame) {
                fprintf(stderr, "[input] card-prompt %s at frame %llu\n",
                        action, (unsigned long long)frame);
                g_auto_skip_card_last_pulse = frame;
            }
        }
    }
    if (input_script_apply(cpu, &state[0].button, &state[0].stick_x,
                           &state[0].stick_y, &state[0].analog_a))
        state[0].error = 0;
    mash_to_gameplay_apply(cpu, &state[0].button, &state[0].stick_x,
                           &state[0].analog_a);
    if (g_mash_to_gameplay)
        state[0].error = 0;
    for (u32 i = 0; i < 4; i++) {
        u32 p = out + i * 12u;
        mem_write16(cpu, p, state[i].button);
        mem_write8(cpu, p + 2u, (u8)state[i].stick_x);
        mem_write8(cpu, p + 3u, (u8)state[i].stick_y);
        mem_write8(cpu, p + 4u, (u8)state[i].substick_x);
        mem_write8(cpu, p + 5u, (u8)state[i].substick_y);
        mem_write8(cpu, p + 6u, state[i].trigger_left);
        mem_write8(cpu, p + 7u, state[i].trigger_right);
        mem_write8(cpu, p + 8u, state[i].analog_a);
        mem_write8(cpu, p + 9u, state[i].analog_b);
        mem_write8(cpu, p + 10u, (u8)state[i].error);
        mem_write8(cpu, p + 11u, 0);
    }
    hle_set_u32(cpu, motor_mask);
}

static void hle_PADReset(CPUState* cpu) {
    bool reset = dol_platform_pad_reset(hle_arg_u32(cpu, 0));
    hle_set_u32(cpu, (reset || g_mash_to_gameplay) ? 1u : 0u);
}

static void hle_PADRecalibrate(CPUState* cpu) {
    bool recalibrated = dol_platform_pad_recalibrate(hle_arg_u32(cpu, 0));
    hle_set_u32(cpu, (recalibrated || g_mash_to_gameplay) ? 1u : 0u);
}

static void hle_PADControlMotor(CPUState* cpu) {
    dol_platform_pad_control_motor(hle_arg_u32(cpu, 0), hle_arg_u32(cpu, 1));
}

static void hle_PADSetSpec(CPUState* cpu) {
    dol_platform_pad_set_spec(hle_arg_u32(cpu, 0));
}
#else
// Deterministic controller pulses for headless integration tests. This stays
// fully opt-in so normal headless runs still report no connected controllers.
static bool headless_scripted_pad(CPUState* cpu,
                                  u16* buttons,
                                  s8* stick_x,
                                  s8* stick_y,
                                  u8* analog_a) {
    bool connected = false;
    const u64 frame = cpu->timebase / 675000ull;
    if (g_auto_input) {
        const bool pressed = frame >= 135u && ((frame - 135u) % 90u) < 12u;
        if (pressed) {
            *buttons |= 0x0100u;
            *analog_a = 0xFFu;
            connected = true;
            if (g_auto_input_last_pulse != frame) {
                fprintf(stderr, "[input] scripted A at frame %llu\n",
                        (unsigned long long)frame);
                g_auto_input_last_pulse = frame;
            }
        }
    }
    if (input_script_apply(cpu, buttons, stick_x, stick_y, analog_a))
        connected = true;
    mash_to_gameplay_apply(cpu, buttons, stick_x, analog_a);
    if (g_mash_to_gameplay)
        connected = true;
    return connected;
}

static void hle_PADInit(CPUState* cpu) {
    hle_set_u32(cpu, 1u);
}

static void hle_PADRead(CPUState* cpu) {
    const u32 out = hle_arg_u32(cpu, 0);
    u16 buttons = 0;
    s8 stick_x = 0;
    s8 stick_y = 0;
    u8 analog_a = 0;
    const bool connected =
        headless_scripted_pad(cpu, &buttons, &stick_x, &stick_y, &analog_a);
    for (u32 i = 0; i < 4u; i++) {
        const u32 p = out + i * 12u;
        mem_write16(cpu, p, i == 0u ? buttons : 0u);
        mem_write8(cpu, p + 2u, (u8)(i == 0u ? stick_x : 0));
        mem_write8(cpu, p + 3u, (u8)(i == 0u ? stick_y : 0));
        for (u32 byte = 4u; byte < 8u; byte++)
            mem_write8(cpu, p + byte, 0u);
        mem_write8(cpu, p + 8u, i == 0u ? analog_a : 0u);
        mem_write8(cpu, p + 9u, 0u);
        mem_write8(cpu, p + 10u, (u8)((i == 0u && connected) ? 0 : -1));
        mem_write8(cpu, p + 11u, 0u);
    }
    hle_set_u32(cpu, 0u);
}

static void hle_PADReset(CPUState* cpu) {
    hle_set_u32(cpu, (g_auto_input || g_mash_to_gameplay ||
                      g_input_script_count != 0u) ? 1u : 0u);
}

static void hle_PADRecalibrate(CPUState* cpu) {
    hle_set_u32(cpu, (g_auto_input || g_mash_to_gameplay ||
                      g_input_script_count != 0u) ? 1u : 0u);
}

static void hle_PADControlMotor(CPUState* cpu) {
    (void)cpu;
}

static void hle_PADSetSpec(CPUState* cpu) {
    (void)cpu;
}
#endif

// ---------------------------------------------------------------------------
// Handler registry
// ---------------------------------------------------------------------------
// Aurora-implemented SDK functions, matched by name against the generated table.
static const HleEntry kHandlers[] = {
    { "OSReport",          hle_OSReport },
    { "ARInit",            hle_ARInit },
    { "ARGetBaseAddress",  hle_ARGetBaseAddress },
    { "ARGetSize",         hle_ARGetSize },
    { "ARGetDMAStatus",    hle_ARGetDMAStatus },
    { "ARStartDMA",        hle_ARStartDMA },
    { "CARDInit",          hle_CARDInit },
    { "CARDProbe",         hle_CARDProbe },
    { "CARDProbeEx",       hle_CARDProbeEx },
    { "CARDGetResultCode", hle_CARDGetResultCode },
    { "CARDGetFastMode",   hle_CARDGetFastMode },
    { "CARDGetXferredBytes", hle_CARDGetXferredBytes },
    { "CARDMountAsync",    hle_CARDMountAsync },
    { "CARDCheckExAsync",  hle_CARDCheckExAsync },
    { "CARDCheckAsync",    hle_CARDCheckAsync },
    { "CARDFreeBlocks",    hle_CARDFreeBlocks },
    { "CARDOpen",          hle_CARDOpen },
    { "CARDCreateAsync",   hle_CARDCreateAsync },
    { "CARDDeleteAsync",   hle_CARDDeleteAsync },
    { "CARDGetStatus",     hle_CARDGetStatus },
    { "CARDSetStatusAsync", hle_CARDSetStatusAsync },
    { "CARDGetSerialNo",   hle_CARDGetSerialNo },
    { "CARDUnmount",       hle_CARDUnmount },
    { "CARDClose",         hle_CARDClose },
    { "CARDReadAsync",     hle_CARDReadAsync },
    { "CARDWriteAsync",    hle_CARDWriteAsync },
};

// Functions intercepted by address rather than by Aurora name. These are host-
// owned hardware/OS boundaries below (or beside) the public SDK API: either a
// helper that busy-waits on unmodeled hardware (skipped) or a device the host
// implements itself (DVD, served from the ISO). Addresses are from the decomp
// symbol map (G4QE01).
typedef struct {
    u32         address;
    HleHandler  fn;
    const char* name;
} HleAddrEntry;

static const HleAddrEntry kAddrHandlers[] = {
    // DSP boot handshake: polls a DSP mailbox bit that never changes without a
    // modeled DSP. Audio is routed through Aurora, so skip the hardware init.
    { 0x80254718u, hle_noop,                     "__OSInitAudioSystem" },
    // AI sample-rate-converter calibration: times transitions of the AI sample
    // counter (__AIRegs[2]), which never ticks without modeled AI hardware.
    // Aurora owns audio, so skip the calibration (its result feeds only AI regs).
    { 0x8023B610u, hle_noop,                     "__AI_SRC_INIT" },
    // MusyX's CPU-side sequencer and voice engine run unchanged. Only skip the
    // physical DSP task boot; audio.c executes the resulting AX command lists.
    { 0x80282B98u, hle_salInitDsp,                "salInitDsp" },
    { 0x80281BCCu, hle_aramUploadData,            "aramUploadData" },
    { 0x8023D638u, hle_ARQPostRequest,            "ARQPostRequest" },
    { 0x802442C4u, hle_return_zero,                "DSPCheckMailToDSP" },
    // DVD: host serves the disc file system from the ISO (see DVD handlers).
    { 0x8024608Cu, hle_DVDInit,                  "DVDInit" },
    { 0x80245C0Cu, hle_DVDConvertPathToEntrynum, "DVDConvertPathToEntrynum" },
    { 0x80245F00u, hle_DVDFastOpen,              "DVDFastOpen" },
    { 0x80245F74u, hle_DVDClose,                 "DVDClose" },
    { 0x80245F98u, hle_DVDReadAsyncPrio,         "DVDReadAsyncPrio" },
    { 0x80248118u, hle_DVDGetCommandBlockStatus, "DVDGetCommandBlockStatus" },
    { 0x80248164u, hle_DVDGetDriveStatus,        "DVDGetDriveStatus" },
    // CARDFormatAsync is present in G4QE01 but absent from Aurora's SDK header
    // intersection, so it is registered by its exact game address.
    { 0x802429FCu, hle_CARDFormatAsync,           "CARDFormatAsync" },
    { 0x80257CC0u, hle_OSGetResetButtonState,    "OSGetResetButtonState" },
    // Locked-cache DMA helpers. THP video decode writes into the 0xE0000000
    // scratch area and uses these to move rows into MEM1 texture planes.
    { 0x80254C54u, hle_LCStoreBlocks,             "LCStoreBlocks" },
    { 0x80254C78u, hle_LCStoreData,               "LCStoreData" },
    { 0x80254D24u, hle_noop,                      "LCQueueWait" },
#ifdef STRIKERSRECOMP_AURORA
    { 0x80252680u, hle_PSMTXConcat,                "PSMTXConcat" },
    { 0x8024D240u, hle_GXSetArray,                "GXSetArray" },
    { 0x8024DD20u, hle_GXBegin,                   "GXBegin" },
    { 0x80251510u, hle_GXCallDisplayList,          "GXCallDisplayList" },
#endif
    { 0x8025AC10u, hle_PADReset,                  "PADReset" },
    { 0x8025AD20u, hle_PADRecalibrate,            "PADRecalibrate" },
    { 0x8025AE34u, hle_PADInit,                   "PADInit" },
    { 0x8025AF84u, hle_PADRead,                   "PADRead" },
    { 0x8025B284u, hle_PADControlMotor,           "PADControlMotor" },
    { 0x8025B33Cu, hle_PADSetSpec,                "PADSetSpec" },
};

// Functions to trace (log name + first args) without intercepting: the
// recompiled body still runs. A debugging aid for bringing up the OS scheduler,
// VI, and interrupt paths. Enabled with STRIKERS_HLE_LOG=1.
typedef struct {
    u32         address;
    const char* name;
} HleTraceEntry;

static const HleTraceEntry kTrace[] = {
    { 0x8023BA70u, "ARInit" },
    { 0x8023BB40u, "ARGetBaseAddress" },
    { 0x8023BB48u, "ARGetSize" },
    { 0x8025DA80u, "VIInit" },
    { 0x8025DF30u, "VIWaitForRetrace" },
    { 0x8025E3F8u, "VIConfigure" },
    { 0x8025ED30u, "VISetNextFrameBuffer" },
    { 0x80255360u, "OSLoadContext" },
    { 0x80259370u, "__OSReschedule" },
    { 0x80259598u, "OSResumeThread" },
    { 0x80256B74u, "__OSInterruptInit" },
    { 0x80259990u, "OSSleepThread" },
    { 0x80259820u, "OSSuspendThread" },
    { 0x8024D7ACu, "GXSetDrawDone" },
    { 0x8024DBACu, "GXFinishInterruptHandler" },
};

static const char* trace_name(u32 address) {
    for (size_t i = 0; i < sizeof kTrace / sizeof kTrace[0]; i++)
        if (kTrace[i].address == address)
            return kTrace[i].name;
    return NULL;
}

// Notify hooks run *alongside* the recompiled function (it still executes), to
// observe a call and drive host state -- unlike kAddrHandlers, which replace it.

// FinishQueue in GXMisc.c for G4QE01. A thread sleeps here (in GXWaitDrawDone)
// only while a draw-done token is in flight (DrawDone == 0), so the wait itself
// is the faithful trigger for the Pixel Engine FINISH interrupt. By the time
// execution reaches this OSSleepThread call external interrupts are disabled, so
// committing here enqueues the waiter before interrupt_poll can deliver FINISH,
// matching a GPU completing submitted work. This covers both the standalone
// GXSetDrawDone+GXWaitDrawDone pair and the inlined GXDrawDone() (which never
// calls the standalone GXSetDrawDone -- the loading-screen GXDrawDone() used to
// hang here forever because nothing armed the finish).
#define GX_FINISH_QUEUE 0x803746DCu

static void notify_OSSleepThread(CPUState* cpu) {
    if (hle_arg_u32(cpu, 0) == GX_FINISH_QUEUE)
        interrupt_commit_pe_finish();
}

static void notify_DSPSendMailToDSP(CPUState* cpu) {
    audio_dsp_mail(cpu, hle_arg_u32(cpu, 0));
}

static void notify_audio_api(CPUState* cpu) {
    if (g_audio_log)
        fprintf(stderr, "[audio] API 0x%08X r3=0x%08X r4=0x%08X r5=0x%08X\n",
                cpu->pc, cpu->gpr[3], cpu->gpr[4], cpu->gpr[5]);
}

static void notify_OSResetSystem(CPUState* cpu) {
    fprintf(stderr,
            "[reset] OSResetSystem mode=%u code=0x%08X force-menu=%u lr=0x%08X\n",
            cpu->gpr[3], cpu->gpr[4], cpu->gpr[5], cpu->lr);
}

static void notify_OSLoadContext(CPUState* cpu) {
    interrupt_restore_fpu_context(cpu, cpu->gpr[3]);
}

static void notify_SetNextState(CPUState* cpu) {
    if (g_mash_to_gameplay && g_mash_side_assigned &&
        !g_mash_route_complete) {
        if (g_mash_prematch && cpu->gpr[3] == 0x00000002u) {
            g_mash_route_complete = true;
            fprintf(stderr,
                    "[input] gameplay reached; controller is now neutral\n");
        } else if (!g_mash_prematch && cpu->gpr[3] == 0x00000100u) {
            g_mash_prematch = true;
            fprintf(stderr,
                    "[input] prematch reached; mashing A only\n");
        }
    }
    if (!g_state_log)
        return;
    const u32 manager = mem_read32(cpu, 0x803742B8u);
    fprintf(stderr,
            "[state] SetNextState next=0x%08X manager=0x%08X "
            "current=0x%08X pending=0x%08X lr=0x%08X\n",
            cpu->gpr[3], manager,
            manager ? mem_read32(cpu, manager + 8u) : 0u,
            manager ? mem_read32(cpu, manager + 0x0Cu) : 0u, cpu->lr);
}

static void notify_TaskManagerStartup(CPUState* cpu) {
    if (g_state_log)
        fprintf(stderr,
                "[state] TaskManager::Startup initial=0x%08X old=0x%08X "
                "lr=0x%08X timebase=%llu\n",
                cpu->gpr[3], mem_read32(cpu, 0x803742B8u), cpu->lr,
                (unsigned long long)cpu->timebase);
}

static void fe_route_pad_state(CPUState* cpu,
                               u16* buttons,
                               s8* stick_x,
                               s8* stick_y,
                               s8* err,
                               u16* just_pressed) {
    *buttons = 0;
    *stick_x = 0;
    *stick_y = 0;
    *err = -1;
    *just_pressed = 0;
    const u32 current = mem_read32(cpu, 0x80372FF0u); // PadStatus::s_Current
    if (current >= GC_RAM_BASE && current + 12u <= GC_RAM_BASE + cpu->ram_size) {
        *buttons = mem_read16(cpu, current);
        *stick_x = (s8)mem_read8(cpu, current + 2u);
        *stick_y = (s8)mem_read8(cpu, current + 3u);
        *err = (s8)mem_read8(cpu, current + 10u);
    }
    const u32 categories = mem_read32(cpu, 0x80372FF8u); // platpad.cpp padStatus
    if (categories >= GC_RAM_BASE &&
        categories + 0x382u <= GC_RAM_BASE + cpu->ram_size) {
        *just_pressed = mem_read16(cpu, categories + 0x380u);
    }
}

static bool fe_route_should_log(CPUState* cpu,
                                u16 buttons,
                                u16 just_pressed,
                                u64* last_frame) {
    const u64 frame = cpu->timebase / 675000ull;
    if (*last_frame == frame)
        return false;
    if (buttons != 0u || just_pressed != 0u || (frame % 120u) == 0u) {
        *last_frame = frame;
        return true;
    }
    return false;
}

static void notify_SHMainMenuUpdate(CPUState* cpu) {
    if (!g_state_log)
        return;
    static u64 s_last_frame = ~(u64)0;
    static s32 s_last_index = -999;
    const u32 self = cpu->gpr[3];
    const s32 index = (s32)mem_read32(cpu, self + 0x228u);
    const s32 count = (s32)mem_read32(cpu, self + 0x22Cu);
    const u32 flags = mem_read32(cpu, self + 0x234u);
    u16 buttons = 0;
    u16 just_pressed = 0;
    s8 stick_x = 0;
    s8 stick_y = 0;
    s8 err = -1;
    fe_route_pad_state(cpu, &buttons, &stick_x, &stick_y, &err, &just_pressed);
    const bool changed = index != s_last_index;
    if (changed || fe_route_should_log(cpu, buttons, just_pressed, &s_last_frame)) {
        fprintf(stderr,
                "[fe-route] main-menu frame=%llu self=0x%08X index=%d/%d "
                "flags=0x%08X pad=%04X jp=%04X stick=%d,%d err=%d%s\n",
                (unsigned long long)(cpu->timebase / 675000ull), self, index,
                count, flags, buttons, just_pressed, stick_x, stick_y, err,
                changed ? " index-change" : "");
        s_last_index = index;
    }
}

static void notify_IChooseSideCheckControllers(CPUState* cpu) {
    const u32 self = cpu->gpr[3];
    if (g_mash_to_gameplay && (s32)mem_read32(cpu, self) >= 0)
        g_mash_side_assigned = true;
    if (!g_state_log)
        return;
    static u64 s_last_frame = ~(u64)0;
    static s32 s_last_sides[4] = { -999, -999, -999, -999 };
    static u8 s_last_ready = 0xFFu;
    s32 sides[4];
    u8 ready = 0;
    for (u32 i = 0; i < 4u; ++i) {
        sides[i] = (s32)mem_read32(cpu, self + i * 4u);
        ready |= (mem_read8(cpu, self + 0x10u + i) != 0u) ? (u8)(1u << i) : 0u;
    }
    const s32 disabled_side = (s32)cpu->gpr[4];
    const s32 context = (s32)mem_read32(cpu, self + 0x9Cu);
    u16 buttons = 0;
    u16 just_pressed = 0;
    s8 stick_x = 0;
    s8 stick_y = 0;
    s8 err = -1;
    fe_route_pad_state(cpu, &buttons, &stick_x, &stick_y, &err, &just_pressed);
    bool changed = ready != s_last_ready;
    for (u32 i = 0; i < 4u; ++i)
        changed |= sides[i] != s_last_sides[i];
    if (changed || fe_route_should_log(cpu, buttons, just_pressed, &s_last_frame)) {
        fprintf(stderr,
                "[fe-route] choose-side frame=%llu self=0x%08X disabled=%d "
                "context=%d sides=%d,%d,%d,%d ready=0x%X pad=%04X jp=%04X "
                "stick=%d,%d err=%d%s\n",
                (unsigned long long)(cpu->timebase / 675000ull), self,
                disabled_side, context, sides[0], sides[1], sides[2], sides[3],
                ready, buttons, just_pressed, stick_x, stick_y, err,
                changed ? " side-change" : "");
        for (u32 i = 0; i < 4u; ++i)
            s_last_sides[i] = sides[i];
        s_last_ready = ready;
    }
}

static void notify_IChooseSidePositionController(CPUState* cpu) {
    if (!g_state_log)
        return;
    const u32 self = cpu->gpr[3];
    const s32 pad = (s32)cpu->gpr[4];
    if (pad < 0 || pad >= 4)
        return;
    const s32 side = (s32)mem_read32(cpu, self + (u32)pad * 4u);
    fprintf(stderr,
            "[fe-route] position-controller frame=%llu self=0x%08X pad=%d "
            "side=%d tween=%u vis=%u\n",
            (unsigned long long)(cpu->timebase / 675000ull), self, pad, side,
            cpu->gpr[5] != 0u, cpu->gpr[6] != 0u);
}

static void notify_MovieStart(CPUState* cpu) {
    if (!g_movie_log)
        return;
    char filename[256];
    hle_read_cstr(cpu, hle_arg_u32(cpu, 0), filename, sizeof filename);
    fprintf(stderr, "[movie] MovieStart file='%s' sound=%u loop=%u lr=0x%08X\n",
            filename, hle_arg_u32(cpu, 1), hle_arg_u32(cpu, 2), cpu->lr);
}

static void notify_MovieStop(CPUState* cpu) {
    if (g_movie_log)
        fprintf(stderr, "[movie] MovieStop lr=0x%08X\n", cpu->lr);
}

static void notify_MoviePlay(CPUState* cpu) {
    static unsigned count = 0;
    if (!g_movie_log)
        return;
    ++count;
    if (count <= 120u || (count % 60u) == 0u)
        fprintf(stderr, "[movie] MoviePlay #%u lr=0x%08X\n", count, cpu->lr);
}

static void notify_THPSimpleOpen(CPUState* cpu) {
    if (!g_movie_log)
        return;
    char filename[256];
    hle_read_cstr(cpu, hle_arg_u32(cpu, 0), filename, sizeof filename);
    fprintf(stderr, "[movie] THPSimpleOpen file='%s' lr=0x%08X\n",
            filename, cpu->lr);
}

static void notify_THPSimpleSetBuffer(CPUState* cpu) {
    if (g_movie_log)
        fprintf(stderr, "[movie] THPSimpleSetBuffer buffer=0x%08X lr=0x%08X\n",
                hle_arg_u32(cpu, 0), cpu->lr);
}

static void notify_THPSimplePreLoad(CPUState* cpu) {
    if (g_movie_log)
        fprintf(stderr, "[movie] THPSimplePreLoad loop=%u lr=0x%08X\n",
                hle_arg_u32(cpu, 0), cpu->lr);
}

static void notify_THPSimpleDecode(CPUState* cpu) {
    static unsigned count = 0;
    if (!g_movie_log)
        return;
    ++count;
    if (count <= 120u || (count % 60u) == 0u)
        fprintf(stderr, "[movie] THPSimpleDecode #%u audio-track=%d lr=0x%08X\n",
                count, (s32)hle_arg_u32(cpu, 0), cpu->lr);
}

static void notify_THPVideoDecode(CPUState* cpu) {
    static unsigned count = 0;
    if (!g_movie_log)
        return;
    ++count;
    if (count <= 120u || (count % 60u) == 0u)
        fprintf(stderr,
                "[movie] THPVideoDecode #%u frame=0x%08X y=0x%08X "
                "u=0x%08X v=0x%08X work=0x%08X lr=0x%08X\n",
                count, hle_arg_u32(cpu, 0), hle_arg_u32(cpu, 1),
                hle_arg_u32(cpu, 2), hle_arg_u32(cpu, 3),
                hle_arg_u32(cpu, 4), cpu->lr);
}

static u32 movie_plane_hash(CPUState* cpu, u32 guest_data, u32 bytes) {
    u32 hash_bytes = bytes;
    return guest_hash(cpu, guest_data, hash_bytes, &hash_bytes);
}

static void log_thp_simple_control(CPUState* cpu, const char* tag,
                                   unsigned count) {
    const u32 base = THP_SIMPLE_CONTROL;
    const u32 y = mem_read32(cpu, base + 0x15Cu);
    const u32 u = mem_read32(cpu, base + 0x160u);
    const u32 v = mem_read32(cpu, base + 0x164u);
    const u32 next_decode = mem_read32(cpu, base + 0x7Cu);
    const u32 read_idx = mem_read32(cpu, base + 0x80u);
    const u32 rb_index = next_decode < 16u ? next_decode : 0u;
    const u32 rb = base + 0x9Cu + rb_index * 12u;
    const u32 rb_ptr = mem_read32(cpu, rb + 0u);
    const u32 rb_frame = mem_read32(cpu, rb + 4u);
    const u32 rb_valid = mem_read32(cpu, rb + 8u);
    fprintf(stderr,
            "[movie] %s #%u ret=%d hid2=0x%08X open=%u playing=%u "
            "audio=%u loop=%u audioExist=%u nextDecode=%u readIdx=%u "
            "nextSize=%u frameCount=%u texFrame=%d rb[%u]={ptr=0x%08X "
            "frame=%u valid=%u} y=0x%08X:%08X u=0x%08X:%08X "
            "v=0x%08X:%08X\n",
            tag, count, (s32)cpu->gpr[3], cpu->hid2,
            mem_read32(cpu, base + 0x68u),
            mem_read8(cpu, base + 0x6Cu), mem_read8(cpu, base + 0x6Du),
            mem_read8(cpu, base + 0x6Eu), mem_read8(cpu, base + 0x6Fu),
            next_decode, read_idx, mem_read32(cpu, base + 0x84u),
            mem_read32(cpu, base + 0x88u),
            (s32)mem_read32(cpu, base + 0x168u), rb_index, rb_ptr, rb_frame,
            rb_valid, y, movie_plane_hash(cpu, y, 512u * 416u),
            u, movie_plane_hash(cpu, u, 256u * 208u),
            v, movie_plane_hash(cpu, v, 256u * 208u));
}

static void notify_THPVideoDecodeReturn(CPUState* cpu) {
    static unsigned count = 0;
    if (!g_movie_log)
        return;
    ++count;
    if ((s32)cpu->gpr[3] != 0 || count <= 120u || (count % 60u) == 0u)
        log_thp_simple_control(cpu, "THPVideoDecodeReturn", count);
}

static void notify_THPSimpleDecodeReturn(CPUState* cpu) {
    static unsigned count = 0;
    if (!g_movie_log)
        return;
    ++count;
    const s32 ret = (s32)cpu->gpr[3];
    const bool unusual = ret != 0 && ret != 3;
    if (unusual || count <= 120u || (count % 60u) == 0u)
        log_thp_simple_control(cpu, "THPSimpleDecodeReturn", count);
}

static void notify_LCEnable(CPUState* cpu) {
    if (g_movie_log)
        fprintf(stderr, "[movie] LCEnable entry hid2=0x%08X lr=0x%08X\n",
                cpu->hid2, cpu->lr);
}

static void notify_LCDisable(CPUState* cpu) {
    if (g_movie_log)
        fprintf(stderr, "[movie] LCDisable entry hid2=0x%08X lr=0x%08X\n",
                cpu->hid2, cpu->lr);
}

static const HleAddrEntry kPhysicsNotify[] = {
    { 0x80009E00u, notify_cBallUpdateOrientation, "cBall::UpdateOrientation" },
    { 0x8000B828u, notify_cBallPostPhysicsUpdate, "cBall::PostPhysicsUpdate" },
    { 0x80132B10u, notify_PhysicsUpdate,       "PhysicsUpdate" },
    { 0x801343C8u, notify_PhysicsAIBallPostUpdate, "PhysicsAIBall::PostUpdate" },
    { 0x8020199Cu, notify_PhysicsWorldUpdate,  "PhysicsWorld::Update" },
    { 0x80201A8Cu, notify_PhysicsWorldPreUpdate, "PhysicsWorld::PreUpdate" },
    { 0x80220894u, notify_dWorldQuickStep,   "dWorldQuickStep" },
    { 0x802214E8u, notify_dBodySetForce,     "dBodySetForce" },
    { 0x80221530u, notify_dBodyAddForce,     "dBodyAddForce" },
    { 0x8022160Cu, notify_dBodySetAngularVel, "dBodySetAngularVel" },
    { 0x8022161Cu, notify_dBodySetLinearVel, "dBodySetLinearVel" },
    { 0x8022162Cu, notify_dBodySetRotation,  "dBodySetRotation" },
    { 0x802216B4u, notify_dBodySetPosition,  "dBodySetPosition" },
    { 0x80222DC0u, notify_SorLcp,             "SOR_LCP" },
    { 0x80222994u, notify_SorLcpReturn,       "SOR_LCP return" },
    { 0x80223F30u, notify_dxStepBody,        "dxStepBody" },
#ifdef STRIKERSRECOMP_AURORA
    { 0x8011DF10u, notify_DrawableBallRender, "DrawableBall::Render" },
#endif
};

static const HleAddrEntry kNotify[] = {
    { 0x80259990u, notify_OSSleepThread,  "OSSleepThread" },
    { 0x802442FCu, notify_DSPSendMailToDSP, "DSPSendMailToDSP" },
    { 0x80268A08u, notify_audio_api,       "sndFXStartParaInfo" },
    { 0x8026C518u, notify_audio_api,       "sndStreamActivate" },
    { 0x80277274u, notify_audio_api,       "sndPushGroup" },
    { 0x80277BA4u, notify_audio_api,       "sndSeqPlayEx" },
    { 0x80257994u, notify_OSResetSystem,    "OSResetSystem" },
    { 0x801D2908u, notify_SetNextState,     "nlTaskManager::SetNextState" },
    { 0x801D2B28u, notify_TaskManagerStartup, "nlTaskManager::Startup" },
    { 0x800A9A5Cu, notify_SHMainMenuUpdate, "SHMainMenu::Update" },
    { 0x800C3C64u, notify_IChooseSideCheckControllers, "IChooseSide::CheckControllers" },
    { 0x800C36E8u, notify_IChooseSidePositionController, "IChooseSide::PositionController" },
    { 0x801CB7F8u, notify_MoviePlay,        "MoviePlay" },
    { 0x801CB8E0u, notify_MovieStop,        "MovieStop" },
    { 0x801CBA48u, notify_MovieStart,       "MovieStart" },
    { 0x801CC5E8u, notify_THPSimpleDecode,  "THPSimpleDecode" },
    { 0x801CB868u, notify_THPSimpleDecodeReturn, "THPSimpleDecode return" },
    { 0x801CC6C4u, notify_THPVideoDecodeReturn, "THPVideoDecode return" },
    { 0x801CC7F0u, notify_THPVideoDecodeReturn, "THPVideoDecode return" },
    { 0x801CCB00u, notify_THPSimplePreLoad, "THPSimplePreLoad" },
    { 0x801CCDD0u, notify_THPSimpleSetBuffer, "THPSimpleSetBuffer" },
    { 0x801CD2C8u, notify_THPSimpleOpen,    "THPSimpleOpen" },
    { 0x802857ACu, notify_THPVideoDecode,   "THPVideoDecode" },
    { 0x80254BF4u, notify_LCEnable,          "LCEnable" },
    { 0x80254C2Cu, notify_LCDisable,         "LCDisable" },
    { 0x80255360u, notify_OSLoadContext,     "OSLoadContext" },
#ifdef STRIKERSRECOMP_AURORA
    { 0x8024E974u, notify_GXCopyDisp,      "GXCopyDisp" },
    { 0x8024EADCu, notify_GXCopyTex,       "GXCopyTex" },
    { 0x8024F89Cu, notify_GXLoadTexObj,    "GXLoadTexObj" },
    { 0x8024F928u, notify_GXLoadTlut,      "GXLoadTlut" },
    { 0x8025186Cu, notify_GXLoadPosMtxImm, "GXLoadPosMtxImm" },
    { 0x8025DF30u, notify_VIWaitForRetrace,"VIWaitForRetrace" },
    { 0x8025E3F8u, notify_VIConfigure,     "VIConfigure" },
#endif
};

// ---------------------------------------------------------------------------
// Dispatch
// ---------------------------------------------------------------------------
// Lowest address holding loaded code. A call below this is a branch through an
// uninitialized/optional function pointer (e.g. a NULL callback hook).
#define HLE_CODE_BASE 0x80003000u
#define HLE_CODE_LIMIT 0x8028D260u
#define HLE_DISPATCH_COUNT ((HLE_CODE_LIMIT - HLE_CODE_BASE) / 4u)

static HleDispatchEntry g_hle_dispatch[HLE_DISPATCH_COUNT];

static HleDispatchEntry* hle_dispatch_entry(u32 address) {
    if (address < HLE_CODE_BASE || address >= HLE_CODE_LIMIT ||
        (address & 3u) != 0u)
        return NULL;
    return &g_hle_dispatch[(address - HLE_CODE_BASE) / 4u];
}

static void initialize_hle_dispatch(void) {
    memset(g_hle_dispatch, 0, sizeof g_hle_dispatch);

    for (size_t i = 0; i < sizeof kHandlers / sizeof kHandlers[0]; i++) {
        HleDispatchEntry* entry =
            hle_dispatch_entry(sdk_symbol_address(kHandlers[i].name));
        if (entry != NULL)
            entry->intercept = kHandlers[i].fn;
    }
    for (size_t i = 0; i < sizeof kAddrHandlers / sizeof kAddrHandlers[0]; i++) {
        HleDispatchEntry* entry =
            hle_dispatch_entry(kAddrHandlers[i].address);
        if (entry != NULL)
            entry->intercept = kAddrHandlers[i].fn;
    }
    for (size_t i = 0; i < sizeof kNotify / sizeof kNotify[0]; i++) {
        HleDispatchEntry* entry = hle_dispatch_entry(kNotify[i].address);
        if (entry != NULL)
            entry->notify = kNotify[i].fn;
    }
    if (ball_state_log_enabled()) {
        for (size_t i = 0;
             i < sizeof kPhysicsNotify / sizeof kPhysicsNotify[0]; ++i) {
            HleDispatchEntry* entry =
                hle_dispatch_entry(kPhysicsNotify[i].address);
            if (entry != NULL)
                entry->notify = kPhysicsNotify[i].fn;
        }
    }
}

static bool hle_dispatch(CPUState* cpu, u32 address) {
    if (address == HLE_CALLBACK_RETURN && g_callback_active) {
        restore_callback_context(cpu, &g_callback_context);
        g_callback_active = false;
        return true;
    }
#ifdef STRIKERSRECOMP_AURORA
    if (address == HLE_DISPLAY_LIST_RETURN && g_display_list_call.active) {
        continue_GXCallDisplayList(cpu);
        return true;
    }
    if (address == HLE_GX_BEGIN_RETURN && g_gx_begin.active) {
        continue_GXBegin(cpu);
        return true;
    }
#endif

    // Call through a NULL (or near-NULL) function pointer. GameCube SDK code
    // calls optional callback hooks (thread-switch callbacks, etc.) that default
    // to NULL; the recompiled `blrl` already saved the return address in lr, so
    // model the absent callback as a no-op and return to the caller instead of
    // halting. Loud + counted so genuine wild branches stay visible.
    if (address < HLE_CODE_BASE) {
        static unsigned long null_calls = 0;
        if (cpu->lr >= HLE_CODE_BASE) {
            if (g_hle_log || null_calls < 8)
                fprintf(stderr,
                        "[hle] null-pointer call to 0x%08X from lr=0x%08X "
                        "(treating as no-op callback, #%lu)\n",
                        address, cpu->lr, ++null_calls);
            else
                ++null_calls;
            hle_return(cpu);
            return true;
        }
        return false;  // lr also bad -> let the runtime stop and report
    }

    HleDispatchEntry* entry = hle_dispatch_entry(address);
    if (entry == NULL)
        return false;

    if (entry->intercept != NULL) {
        if (g_hle_log) {
            const char* name = sdk_symbol_name(address);
            if (name == NULL) {
                for (size_t i = 0;
                     i < sizeof kAddrHandlers / sizeof kAddrHandlers[0]; i++)
                    if (kAddrHandlers[i].address == address) {
                        name = kAddrHandlers[i].name;
                        break;
                    }
            }
            fprintf(stderr, "[hle] %-28s 0x%08X -> host\n",
                    name != NULL ? name : "(unnamed)", address);
        }
        g_hle_tail_call = false;
        entry->intercept(cpu);
        if (!g_hle_tail_call)
            hle_return(cpu);
        return true;
    }

    // Observe-only hooks: drive host state, then fall through so the recompiled
    // function still runs.
    if (entry->notify != NULL)
        entry->notify(cpu);

    if (g_hle_log) {
        const char* tn = trace_name(address);
        if (tn != NULL)
            fprintf(stderr, "[trace] %-22s lr=0x%08X r3=0x%08X r4=0x%08X r5=0x%08X\n",
                    tn, cpu->lr, cpu->gpr[3], cpu->gpr[4], cpu->gpr[5]);
        const char* name = sdk_symbol_name(address);
        if (name != NULL)
            fprintf(stderr, "[hle] %-28s 0x%08X (recompiled)\n", name,
                    address);
    }
    return false;
}

bool hle_card_open(const char* path) {
    hle_card_close();
    DolMemoryCardConfig config;
    memset(&config, 0, sizeof config);
    config.path = path;
    config.size_mbits = 4;
    config.encoding = 0;
    memcpy(config.game_code, "G4QE", 4);
    memcpy(config.company, "01", 2);
    g_memory_card = dol_card_open(&config);
    if (g_memory_card == NULL)
        return false;

    u16 size_mbits = 0;
    u32 sector_size = 0;
    if (dol_card_probe(g_memory_card, &size_mbits, &sector_size) !=
        DOL_CARD_RESULT_READY) {
        hle_card_close();
        return false;
    }
    fprintf(stderr,
            "[card] slot A: %s (%u Mbit, %u-byte sectors, serial %016llX)\n",
            path != NULL && path[0] != '\0' ? path : "(memory)",
            size_mbits, sector_size,
            (unsigned long long)dol_card_serial(g_memory_card));
    return true;
}

void hle_card_close(void) {
    dol_card_close(g_memory_card);
    g_memory_card = NULL;
    g_callback_read = 0;
    g_callback_count = 0;
    g_callback_active = false;
    memset(&g_callback_context, 0, sizeof g_callback_context);
}

void hle_install(CPUState* cpu) {
    g_hle_log = getenv("STRIKERS_HLE_LOG") != NULL;
    g_audio_log = getenv("STRIKERS_AUDIO_LOG") != NULL;
    g_graphics_log = getenv("STRIKERS_GFX_LOG") != NULL;
    g_movie_log = getenv("STRIKERS_MOVIE_LOG") != NULL;
    g_movie_cadence_log = getenv("STRIKERS_MOVIE_CADENCE_LOG") != NULL;
    g_card_log = getenv("STRIKERS_CARD_LOG") != NULL;
    g_auto_input = getenv("STRIKERS_AUTO_INPUT") != NULL;
    g_mash_to_gameplay = getenv("STRIKERS_MASH_TO_GAMEPLAY") != NULL;
    g_mash_side_assigned = false;
    g_mash_prematch = false;
    g_mash_route_complete = false;
    hle_physics_init();
    g_auto_skip_card_prompt = getenv("STRIKERS_AUTO_SKIP_CARD_PROMPT") != NULL;
    g_state_log = getenv("STRIKERS_STATE_LOG") != NULL;
    if (g_mash_to_gameplay) {
        fprintf(stderr,
                "[input] mashing A + Left + Start (8 frames held, 8 released)\n");
    }
#ifdef STRIKERSRECOMP_AURORA
    {
        const char* mav = getenv("STRIKERS_MAX_ARRAY_VERTS");
        if (mav) {
            unsigned long v = strtoul(mav, NULL, 0);
            if (v >= 16u && v <= 65536u)
                g_max_array_verts = (u32)v;
        }
    }
#endif
    g_auto_input_last_pulse = ~(u64)0;
    g_auto_skip_card_last_pulse = ~(u64)0;
    g_auto_input_once_sent = ~(u64)0;
    g_auto_input_once_frame = 0;
    g_callback_read = 0;
    g_callback_count = 0;
    g_callback_active = false;
    memset(&g_callback_context, 0, sizeof g_callback_context);
#ifdef STRIKERSRECOMP_AURORA
    memset(&g_display_list_call, 0, sizeof g_display_list_call);
    memset(&g_gx_begin, 0, sizeof g_gx_begin);
#endif
    initialize_hle_dispatch();
    const char* once_frame = getenv("STRIKERS_AUTO_INPUT_ONCE");
    if (once_frame != NULL && once_frame[0] != '\0')
        g_auto_input_once_frame = strtoull(once_frame, NULL, 0);
    parse_input_script(getenv("STRIKERS_INPUT_SCRIPT"));
#ifdef STRIKERSRECOMP_AURORA
    memset(g_textures, 0, sizeof g_textures);
    memset(g_tluts, 0, sizeof g_tluts);
    memset(g_array_address, 0, sizeof g_array_address);
    memset(g_array_stride, 0, sizeof g_array_stride);
    g_gx_begin_count = 0;
    g_gx_array_count = 0;
    g_frame_array_bytes = 0;
    g_frame_array_calls = 0;
    g_frame_array_bytes_peak = 0;
    g_array_budget_warned = false;
    g_vi_wait_count = 0;
    g_movie_texture_load_count = 0;
    g_movie_cadence_present_count = 0;
    g_movie_cadence_texframe_changes = 0;
    g_movie_cadence_start_time = 0.0;
    g_movie_cadence_last_texframe = -1;
    g_movie_cadence_started = false;
#endif
    cpu->host_call = hle_dispatch;
}
