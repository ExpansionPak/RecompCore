// SPDX-License-Identifier: GPL-3.0-or-later
#include "dolruntime/aurora_backend.h"
#include "dolruntime/gx_recomp.h"
#include "dolruntime/platform.h"

#if DOLRUNTIME_HAS_AURORA_RECOMP
#include "dolruntime/aurora_recomp/retail_gx_frontend.hpp"
#include "dolruntime/aurora_recomp/trace.hpp"
#include "dolruntime/gxcore/gxcore.hpp"
#endif

#include <aurora/aurora.h>
#include <aurora/event.h>
#include <aurora/gfx.h>
#include <dolphin/gx.h>
#include <dolphin/gx/GXAurora.h>
#include <dolphin/gx/GXCommandList.h>
#include <dolphin/pad.h>
#include <dolphin/vi.h>
#include <SDL3/SDL_audio.h>
#include <SDL3/SDL_init.h>
#include <SDL3/SDL_scancode.h>
#include <SDL3/SDL_timer.h>

#include <gx/fifo.hpp>
#include <gx/gx.hpp>
#include <gx/recomp.hpp>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <unordered_map>

#if DOLRUNTIME_HAS_AURORA_RECOMP
// gxcore substrate submission lives in the Aurora fork (lib/gfx/gxcore_draw.cpp),
// linked into this binary via aurora::gx. Forward-declare the three entry points
// the live cutover uses so the backend needn't reach the fork-internal header
// (that path is reserved for the dolgx_replay tool target).
namespace aurora::gfx::gxcore {
bool submit_draw_plan(const dolruntime::gxcore::DrawPlan& plan);
void copy_efb_to_texture(const dolruntime::gxcore::EfbCopyCommand& cmd);
void reset_texture_cache();
} // namespace aurora::gfx::gxcore
#endif

namespace {
bool g_initialized = false;
bool g_frame_open = false;
bool g_should_quit = false;
bool g_graphics_log = false;
bool g_force_untextured = false;
bool g_gx_core_enabled = true; // gxcore is the default renderer; DOL_GX_CORE=0 opts out
bool g_draw_opcode_pending = false;
bool g_audio_queue_log = false;
bool g_frame_pacing_log = false;
unsigned long long g_present_count = 0;
unsigned long long g_fifo_bytes = 0;
unsigned long long g_audio_push_count = 0;
unsigned long long g_audio_throttle_count = 0;
unsigned long long g_audio_low_log_push = 0;
u32 g_audio_sample_rate = 32000;
SDL_AudioStream* g_audio_stream = nullptr;
bool g_audio_playing = false;
int g_audio_prebuffer_ms = 40;
int g_audio_max_queue_ms = 250;
DolPlatformGuestAddressResolverFn g_guest_address_resolver = nullptr;
void* g_guest_address_resolver_user = nullptr;

struct PendingTextureMetadata {
    bool valid = false;
    const void* data = nullptr;
    u32 width = 0;
    u32 height = 0;
    u32 format = 0;
    u32 tlut = 0;
    bool mipmap = false;
    u32 object_id = 0;
    u32 data_version = 0;
};

struct PendingTlutMetadata {
    bool valid = false;
    const void* data = nullptr;
    u32 format = 0;
    u16 entries = 0;
    u32 object_id = 0;
    u32 data_version = 0;
};

std::array<PendingTextureMetadata, 8> g_pending_textures{};
std::array<PendingTlutMetadata, 20> g_pending_tluts{};
#if DOLRUNTIME_HAS_AURORA_RECOMP
// Shadow validation now runs through the consuming sink: it reconstructs the
// renderer-facing draw/resource/state model a real Aurora sink would translate
// into draw calls, while still enforcing the packet-shape invariants the old
// validating sink checked. The live Aurora path stays authoritative; this only
// proves the consumer survives real FIFO traffic ahead of renderer cutover.
dolruntime::aurora_recomp::RetailGxFrontend g_shadow_frontend;
dolruntime::aurora_recomp::ConsumingAuroraRenderSink g_shadow_packet_sink;
bool g_shadow_frontend_enabled = false;
bool g_shadow_frontend_failed = false;
// 63/Mfin live cutover: when DOL_GX_CORE is set, the same shadow frontend that
// proved gameplay decode parity (S7) drives a GxCoreSink whose DrawPlans submit
// through the Dolphin-ported gxcore modules onto the Aurora substrate, and the
// live Aurora gx layer is bypassed entirely (no double-draw). This is the
// program's evidence-gated cutover — the live path stays compiled until the old
// gx-semantics layer is deleted, but at runtime only one path renders.
dolruntime::gxcore::GxCoreSink g_core_sink;
unsigned long long g_core_submitted = 0;
unsigned long long g_core_rejected = 0;
void core_plan_observer(const dolruntime::gxcore::DrawPlan& plan, void*) {
    if (!plan.ok)
        return; // skip reasons are tallied in the sink gap counters
    if (aurora::gfx::gxcore::submit_draw_plan(plan))
        ++g_core_submitted;
    else
        ++g_core_rejected;
}
void core_copy_observer(const dolruntime::gxcore::EfbCopyCommand& cmd, void*) {
    aurora::gfx::gxcore::copy_efb_to_texture(cmd);
}
// Per-frame draw-count diff (frontend shadow decode vs live Aurora push_gx_draw).
// Aurora's drawCallCount resets each frame and increments once per push_gx_draw;
// the shadow frontend emits one Draw packet per primitive from the SAME FIFO
// bytes. So per frame, frontend_draw_delta should equal Aurora drawCallCount if
// the consumer decodes the same draw stream Aurora renders. A standing mismatch
// means cutover is unsafe; a clean match means the remaining cutover work is the
// rendering translation, not the draw decode.
unsigned long long g_shadow_last_draw_total = 0;
unsigned long long g_shadow_last_vertex_total = 0;
unsigned long long g_shadow_draw_mismatch_frames = 0;
// Final pre-cutover gate: the consumer computes the byte extents an issued draw
// would submit (raw verts/topology indices/storage) via the cutover input
// builders; we diff this frame's delta against Aurora's native frame extents
// (lastVertSize/lastIndexSize/lastStorageSize). Raw vertex bytes are
// merge-invariant (only 4-byte push alignment differs), so they are hard-gated
// with an alignment band; topology/storage carry Aurora merge/cache caveats and
// are logged for observation only.
unsigned long long g_shadow_last_rawvert_total = 0;
unsigned long long g_shadow_last_topoidx_total = 0;
unsigned long long g_shadow_last_storage_total = 0;
unsigned long long g_shadow_vert_extent_mismatch_frames = 0;
unsigned long long g_shadow_last_zero_draw_total = 0;
// AuroraStats are published by the render worker one present late
// (aurora/lib/gfx/common.cpp end_frame -> render_worker::enqueue_end_frame
// copies the frame's counters into g_stats on the worker), so the sample read
// at present N describes frame N-1. Hold one frame of frontend counters and
// diff each stats sample against the PREVIOUS frame. Steady-state scenes (FE
// menus) are shift-invariant, which is why this only surfaced in gameplay
//. Zero-vertex retail draws count in Aurora but are frontend no-ops,
// so the draw gate is draws + zdraws == stats.
bool g_shadow_prev_frame_valid = false;
unsigned long long g_shadow_prev_frame_index = 0;
unsigned long long g_shadow_prev_draws = 0;
unsigned long long g_shadow_prev_zero_draws = 0;
unsigned long long g_shadow_prev_verts = 0;
unsigned long long g_shadow_prev_rawvert = 0;
unsigned long long g_shadow_prev_topoidx = 0;
unsigned long long g_shadow_prev_storage = 0;
bool g_shadow_transform_log_enabled = false;
// DOL_AURORA_RECOMP_DRAW_LIGHT_LOG: append the draw's XF lighting state
// (channel regs + loaded lights) to the transform log lines. Implies the
// transform observer; obeys the same FRAME/DRAW/LIMIT selectors.
// DOL_AURORA_RECOMP_DRAW_LIGHT_LIT_ONLY additionally drops draws whose
// channel controls do not light (they don't consume the LIMIT budget).
bool g_shadow_light_log_enabled = false;
bool g_shadow_light_log_lit_only = false;
bool g_shadow_transform_log_sequence_enabled = false;
unsigned long long g_shadow_transform_log_frame = 0;
unsigned long long g_shadow_transform_log_min_frame = 0;
long g_shadow_transform_log_draw = -1;
unsigned long long g_shadow_transform_log_sequence = 0;
unsigned long g_shadow_transform_log_limit = 1;
unsigned long g_shadow_transform_log_count = 0;
std::size_t g_shadow_transform_next_draw_index = 0;
#endif

void install_platform_ops();

int audio_ms_env(const char* name, int fallback, int min_value, int max_value) {
    const char* text = std::getenv(name);
    if (text == nullptr || text[0] == '\0')
        return fallback;

    char* end = nullptr;
    long value = std::strtol(text, &end, 10);
    if (end == text)
        return fallback;
    if (value < min_value)
        value = min_value;
    if (value > max_value)
        value = max_value;
    return static_cast<int>(value);
}

unsigned long long ull_env(const char* name, unsigned long long fallback) {
    const char* text = std::getenv(name);
    if (text == nullptr || text[0] == '\0')
        return fallback;
    char* end = nullptr;
    const unsigned long long value = std::strtoull(text, &end, 10);
    return end == text ? fallback : value;
}

long long_env(const char* name, long fallback) {
    const char* text = std::getenv(name);
    if (text == nullptr || text[0] == '\0')
        return fallback;
    char* end = nullptr;
    const long value = std::strtol(text, &end, 10);
    return end == text ? fallback : value;
}

void log_callback(AuroraLogLevel level, const char* module, const char* message,
                  unsigned int) {
    const char* level_name = "info";
    switch (level) {
    case LOG_DEBUG:   level_name = "debug"; break;
    case LOG_INFO:    level_name = "info"; break;
    case LOG_WARNING: level_name = "warning"; break;
    case LOG_ERROR:   level_name = "error"; break;
    case LOG_FATAL:   level_name = "fatal"; break;
    }
    std::fprintf(level >= LOG_ERROR ? stderr : stdout, "[aurora:%s:%s] %s\n",
                 level_name, module ? module : "core", message ? message : "");
    if (level == LOG_FATAL)
        std::abort();
}

void poll_events() {
    const AuroraEvent* event = aurora_update();
    while (event != nullptr && event->type != AURORA_NONE) {
        if (event->type == AURORA_EXIT)
            g_should_quit = true;
        ++event;
    }
}

void write_aurora_command(u16 command) {
    GXParam1u8(GX_AURORA);
    GXParam1u16(command);
}

void flush_pending_resource_metadata() {
    for (u8 slot = 0; slot < g_pending_tluts.size(); ++slot) {
        auto& pending = g_pending_tluts[slot];
        if (!pending.valid)
            continue;
        write_aurora_command(GX_AURORA_LOAD_TLUT);
        GXParam1u8(slot);
        GXCmd1u64(reinterpret_cast<u64>(pending.data));
        GXParam1u32(pending.format);
        GXParam1u16(pending.entries);
        GXParam1u32(pending.object_id);
        GXParam1u32(pending.data_version);
        pending.valid = false;
    }

    for (u8 slot = 0; slot < g_pending_textures.size(); ++slot) {
        auto& pending = g_pending_textures[slot];
        if (!pending.valid)
            continue;
        write_aurora_command(GX_AURORA_LOAD_TEXOBJ);
        GXParam1u8(slot);
        GXCmd1u64(reinterpret_cast<u64>(pending.data));
        GXParam1u32(pending.width);
        GXParam1u32(pending.height);
        GXParam1u32(pending.format);
        GXParam1u32(pending.tlut);
        GXParam1u8(pending.mipmap ? 1 : 0);
        GXParam1u32(pending.object_id);
        GXParam1u32(pending.data_version);
        pending.valid = false;
    }
}

bool gx_attr_to_cp_array(u32 attr, u8* out) {
    if (out == nullptr)
        return false;
    GXAttr cp_attr = static_cast<GXAttr>(attr);
    if (cp_attr == GX_VA_NBT)
        cp_attr = GX_VA_NRM;
    if (cp_attr < GX_VA_POS)
        return false;
    const u32 cp_index = static_cast<u32>(cp_attr) - static_cast<u32>(GX_VA_POS);
    if (cp_index >= DOL_GX_RECOMP_CP_ARRAY_COUNT)
        return false;
    *out = static_cast<u8>(cp_index);
    return true;
}

DolGuestAddressSpace map_address_space(
    aurora::gx::recomp::AddressSpace space) {
    switch (space) {
    case aurora::gx::recomp::AddressSpace::Virtual:
        return DOL_GUEST_ADDRESS_VIRTUAL;
    case aurora::gx::recomp::AddressSpace::Physical:
        return DOL_GUEST_ADDRESS_PHYSICAL;
    case aurora::gx::recomp::AddressSpace::Auto:
    default:
        return DOL_GUEST_ADDRESS_AUTO;
    }
}

DolGuestResourceKind map_resource_kind(aurora::gx::recomp::ResourceKind kind) {
    switch (kind) {
    case aurora::gx::recomp::ResourceKind::Fifo:
        return DOL_GUEST_RESOURCE_FIFO;
    case aurora::gx::recomp::ResourceKind::DisplayList:
        return DOL_GUEST_RESOURCE_DISPLAY_LIST;
    case aurora::gx::recomp::ResourceKind::VertexArray:
        return DOL_GUEST_RESOURCE_VERTEX_ARRAY;
    case aurora::gx::recomp::ResourceKind::Texture:
        return DOL_GUEST_RESOURCE_TEXTURE;
    case aurora::gx::recomp::ResourceKind::Tlut:
        return DOL_GUEST_RESOURCE_TLUT;
    case aurora::gx::recomp::ResourceKind::CopyDestination:
        return DOL_GUEST_RESOURCE_COPY_DESTINATION;
    case aurora::gx::recomp::ResourceKind::Generic:
    default:
        return DOL_GUEST_RESOURCE_GENERIC;
    }
}

bool aurora_guest_address_resolver_bridge(
    void*, std::uint32_t address, std::uint32_t size,
    aurora::gx::recomp::AddressSpace space,
    aurora::gx::recomp::ResourceKind resource, const void** data,
    std::uint32_t* available) {
    if (g_guest_address_resolver == nullptr)
        return false;
    return g_guest_address_resolver(
        g_guest_address_resolver_user, address, size, map_address_space(space),
        map_resource_kind(resource), data, available);
}

#if DOLRUNTIME_HAS_AURORA_RECOMP
void shadow_frontend_fail_metadata(const char* reason, u32 attr,
                                   u32 guest_address, u32 value) {
    g_shadow_frontend_failed = true;
    std::fprintf(stderr,
                 "[aurora-recomp] shadow RetailGxFrontend rejected metadata; "
                 "reason=%s attr=%u guest=0x%08X value=%u; "
                 "live Aurora path remains active\n",
                 reason != nullptr ? reason : "unknown", attr, guest_address,
                 value);
}

// --- .dolt trace recorder -------------------------------
// Env-gated raw-hardware trace of the retail GX stream for deterministic
// replay: DOL_AURORA_RECOMP_TRACE_OUT=<path> arms it,
// DOL_AURORA_RECOMP_TRACE_FRAMES=A:B bounds it to a 1-based frame window
// (frame N = everything after present N-1 up to and including present N).
// Recording forces the shadow frontend on so resolver traffic (MEM_UPDATE)
// and SET_ARRAY bridges fire.
bool g_trace_armed = false;
std::string g_trace_path;
unsigned long long g_trace_first_frame = 1ull;
unsigned long long g_trace_last_frame = ~0ull;
// Nonzero while inside present: final-draw assembly resolves guest memory at
// present time, and those records belong to the frame that just ended.
unsigned long long g_trace_present_scope_frame = 0ull;
bool g_trace_frame_begun = false;
unsigned long long g_trace_frames_recorded = 0ull;
dolruntime::aurora_recomp::trace::TraceWriter g_trace_writer;
// (guest_addr,size) -> content FNV across the whole recording window;
// suppresses re-recording unchanged memory (menu scenes re-resolve the same
// textures every frame — per-frame dedup made 61-frame traces ~57MB, cross-
// frame ~few MB). Replay-correct because Mode A replays every record from
// the start and its shadow MEM1 persists across frames; changed content
// hashes differently and is re-recorded in order.
std::unordered_map<u64, u64> g_trace_mem_dedup;

unsigned long long trace_current_frame() {
    return g_trace_present_scope_frame != 0ull ? g_trace_present_scope_frame
                                               : g_present_count + 1ull;
}

bool trace_open_now() {
    if (g_trace_writer.is_open())
        return true;
    dolruntime::aurora_recomp::trace::TraceHeader header{};
    header.mem1_size = 0x01800000u;
    // Best-effort game id from the disc header the apploader leaves at
    // 0x80000000; the file opens lazily at the first in-window record because
    // the guest resolver is installed after backend init.
    if (g_guest_address_resolver != nullptr) {
        const void* data = nullptr;
        u32 available = 0;
        if (g_guest_address_resolver(g_guest_address_resolver_user, 0x80000000u,
                                     sizeof header.game_id,
                                     DOL_GUEST_ADDRESS_VIRTUAL,
                                     DOL_GUEST_RESOURCE_GENERIC, &data,
                                     &available) &&
            data != nullptr && available >= sizeof header.game_id) {
            std::memcpy(header.game_id, data, sizeof header.game_id);
        }
    }
    if (!g_trace_writer.open(g_trace_path.c_str(), header)) {
        std::fprintf(stderr, "[trace] failed to open %s; recording disabled\n",
                     g_trace_path.c_str());
        g_trace_armed = false;
        return false;
    }
    return true;
}

// True when a record should be written now; opens the file and emits the
// frame's FRAME_BEGIN first so it precedes every record of the frame.
bool trace_should_record() {
    if (!g_trace_armed)
        return false;
    const unsigned long long frame = trace_current_frame();
    if (frame < g_trace_first_frame || frame > g_trace_last_frame)
        return false;
    if (!trace_open_now())
        return false;
    if (!g_trace_frame_begun) {
        g_trace_writer.frame_begin(static_cast<u32>(frame));
        g_trace_frame_begun = true;
        ++g_trace_frames_recorded;
    }
    return true;
}

void trace_record_mem_update(u32 address, u32 size, const void* data) {
    if (size == 0 || data == nullptr || !trace_should_record())
        return;
    const u64 key = (static_cast<u64>(address) << 32) | size;
    u64 hash = 1469598103934665603ull;
    const auto* bytes = static_cast<const unsigned char*>(data);
    for (u32 i = 0; i < size; ++i) {
        hash ^= bytes[i];
        hash *= 1099511628211ull;
    }
    const auto it = g_trace_mem_dedup.find(key);
    if (it != g_trace_mem_dedup.end() && it->second == hash)
        return;
    g_trace_mem_dedup[key] = hash;
    g_trace_writer.mem_update(address, data, size);
}

void trace_close_and_log() {
    if (!g_trace_writer.is_open()) {
        g_trace_armed = false;
        return;
    }
    const unsigned long long records = g_trace_writer.records_written();
    const unsigned long long bytes = g_trace_writer.bytes_written();
    const bool ok = g_trace_writer.close();
    g_trace_armed = false;
    std::fprintf(stderr,
                 "[trace] wrote %s frames=%llu records=%llu bytes=%llu%s\n",
                 g_trace_path.c_str(), g_trace_frames_recorded, records, bytes,
                 ok ? "" : " (WRITE ERRORS)");
}

void trace_on_present() {
    if (g_trace_armed && trace_should_record()) {
        const AuroraStats* stats = aurora_get_stats();
        dolruntime::aurora_recomp::trace::PresentStats ps{};
        ps.frame_index = static_cast<u32>(trace_current_frame());
        ps.queued_pipelines = stats->queuedPipelines;
        ps.created_pipelines = stats->createdPipelines;
        ps.draw_call_count = stats->drawCallCount;
        ps.merged_draw_call_count = stats->mergedDrawCallCount;
        ps.last_vert_size = stats->lastVertSize;
        ps.last_uniform_size = stats->lastUniformSize;
        ps.last_index_size = stats->lastIndexSize;
        ps.last_storage_size = stats->lastStorageSize;
        ps.last_texture_upload_size = stats->lastTextureUploadSize;
        g_trace_writer.present_stats(ps);
    }
    if (g_trace_armed && g_present_count >= g_trace_last_frame)
        trace_close_and_log();
    g_trace_present_scope_frame = 0ull;
    g_trace_frame_begun = false;
}
// ---------------------------------------------------------------------------

void shadow_frontend_set_array(u32 attr, u32 guest_address, u8 stride) {
    if (!g_shadow_frontend_enabled || g_shadow_frontend_failed)
        return;
    u8 cp_attr = 0;
    if (!gx_attr_to_cp_array(attr, &cp_attr)) {
        shadow_frontend_fail_metadata("unsupported GX array attribute", attr,
                                      guest_address, stride);
        return;
    }
    const u32 physical = dol_gx_recomp_guest_to_physical(guest_address);
    // Record the CP-shaped values (array index + physical address): exactly
    // what replay feeds back into set_cp_array, no SDK enum dependency.
    if (trace_should_record())
        g_trace_writer.set_array(cp_attr, physical, stride);
    if (!g_shadow_frontend.set_cp_array(cp_attr, physical, stride)) {
        shadow_frontend_fail_metadata("failed to mirror CP array state", attr,
                                      guest_address, stride);
    }
}

bool frontend_guest_address_resolver_bridge(
    void*, u32 address, u32 size, DolGuestAddressSpace space,
    DolGuestResourceKind resource, DolGuestResolvedRange* out) {
    if (g_guest_address_resolver == nullptr || out == nullptr)
        return false;
    const void* data = nullptr;
    u32 available = 0;
    if (!g_guest_address_resolver(g_guest_address_resolver_user, address, size,
                                  space, resource, &data, &available) ||
        data == nullptr || available < size)
        return false;
    *out = {
        .data = const_cast<void*>(data),
        .address = address,
        .size = size,
        .available = available,
        .space = space,
        .resource = resource,
    };
    // Every guest byte the frontend/sink consumes flows through this resolve;
    // recording here makes replay correct by construction (the record lands
    // before the FIFO record whose replay-side processing re-asks for it).
    trace_record_mem_update(address, size, data);
    return true;
}

// S7 DL mirror: Strikers HLE-replaces GXCallDisplayList, so DL bytes reach
// live Aurora through aurora_backend_call_display_list without ever passing
// the WGPIPE seam the shadow frontend taps. Forward the SAME host-resolved
// bytes through write_display_list (S3: byte-path-identical to the in-stream
// 0x40 opcode) so gameplay display lists are decoded by the shadow too.
void shadow_frontend_call_display_list(const void* data, u32 size) {
    if (!g_shadow_frontend_enabled || g_shadow_frontend_failed)
        return;
    const std::span<const std::uint8_t> bytes(
        static_cast<const std::uint8_t*>(data), size);
    if (g_gx_core_enabled) {
        if (!g_shadow_frontend.write_display_list(bytes, &g_core_sink)) {
            g_shadow_frontend_failed = true;
            std::fprintf(stderr,
                         "[gx-core] frontend rejected HLE display list "
                         "(%u bytes): %s (opcode=0x%02X)\n",
                         size,
                         g_shadow_frontend.last_error() != nullptr
                             ? g_shadow_frontend.last_error()
                             : "none",
                         static_cast<unsigned>(
                             g_shadow_frontend.last_error_opcode()));
        }
        return;
    }
    if (!g_shadow_frontend.write_display_list(bytes, &g_shadow_packet_sink)) {
        g_shadow_frontend_failed = true;
        std::fprintf(stderr,
                     "[aurora-recomp] shadow RetailGxFrontend rejected HLE "
                     "display list (%u bytes): parse_error=%s opcode=0x%02X "
                     "offset=%llu a=%u b=%u c=%u d=%u; live Aurora path "
                     "remains active\n",
                     size,
                     g_shadow_frontend.last_error() != nullptr
                         ? g_shadow_frontend.last_error()
                         : "none",
                     static_cast<unsigned>(
                         g_shadow_frontend.last_error_opcode()),
                     static_cast<unsigned long long>(
                         g_shadow_frontend.last_error_offset()),
                     g_shadow_frontend.last_error_a(),
                     g_shadow_frontend.last_error_b(),
                     g_shadow_frontend.last_error_c(),
                     g_shadow_frontend.last_error_d());
    }
}

void shadow_frontend_write(u64 value, u8 size) {
    if (!g_shadow_frontend_enabled || g_shadow_frontend_failed)
        return;
    std::uint8_t bytes[8] = {};
    switch (size) {
    case 1:
        bytes[0] = static_cast<std::uint8_t>(value);
        break;
    case 2:
        bytes[0] = static_cast<std::uint8_t>(value >> 8u);
        bytes[1] = static_cast<std::uint8_t>(value);
        break;
    case 4:
        bytes[0] = static_cast<std::uint8_t>(value >> 24u);
        bytes[1] = static_cast<std::uint8_t>(value >> 16u);
        bytes[2] = static_cast<std::uint8_t>(value >> 8u);
        bytes[3] = static_cast<std::uint8_t>(value);
        break;
    case 8:
        for (unsigned i = 0; i < 8; ++i)
            bytes[i] =
                static_cast<std::uint8_t>(value >> ((7u - i) * 8u));
        break;
    default:
        return;
    }
    const std::span<const std::uint8_t> fragment(bytes, size);
    if (g_gx_core_enabled) {
        if (!g_shadow_frontend.write_fifo(fragment) ||
            !g_shadow_frontend.flush(&g_core_sink)) {
            g_shadow_frontend_failed = true;
            std::fprintf(stderr,
                         "[gx-core] frontend rejected FIFO after %llu byte(s): "
                         "%s (opcode=0x%02X offset=%llu); consumer=%s\n",
                         g_fifo_bytes,
                         g_shadow_frontend.last_error() != nullptr
                             ? g_shadow_frontend.last_error()
                             : "none",
                         static_cast<unsigned>(
                             g_shadow_frontend.last_error_opcode()),
                         static_cast<unsigned long long>(
                             g_shadow_frontend.last_error_offset()),
                         g_core_sink.failure_reason() != nullptr
                             ? g_core_sink.failure_reason()
                             : "none");
        }
        return;
    }
    if (!g_shadow_frontend.write_fifo(fragment) ||
        !g_shadow_frontend.flush(&g_shadow_packet_sink)) {
        g_shadow_frontend_failed = true;
        const auto& failed = g_shadow_packet_sink.failed_packet();
        std::fprintf(stderr,
                     "[aurora-recomp] shadow RetailGxFrontend rejected FIFO "
                     "after %llu byte(s), packets=%llu "
                     "(stream=%llu state=%llu resource=%llu draw=%llu); "
                     "write_size=%u write_value=0x%016llX; "
                     "parse_error=%s parse_opcode=0x%02X "
                     "parse_offset=%llu pending=%llu "
                     "detail=(%u,%u,%u,%u); "
                     "reason=%s seq=%u event=%s(%u) "
                     "a=%u b=%u c=%u d=%u; "
                     "live Aurora path remains active\n",
                     g_fifo_bytes, g_shadow_packet_sink.packets(),
                     g_shadow_packet_sink.stream_packets(),
                     g_shadow_packet_sink.state_packets(),
                     g_shadow_packet_sink.resource_packets(),
                     g_shadow_packet_sink.draw_packets(),
                     static_cast<unsigned>(size),
                     static_cast<unsigned long long>(value),
                     g_shadow_frontend.last_error() != nullptr
                         ? g_shadow_frontend.last_error()
                         : "none",
                     static_cast<unsigned>(
                         g_shadow_frontend.last_error_opcode()),
                     static_cast<unsigned long long>(
                         g_shadow_frontend.last_error_offset()),
                     static_cast<unsigned long long>(
                         g_shadow_frontend.pending_fifo_size()),
                     g_shadow_frontend.last_error_a(),
                     g_shadow_frontend.last_error_b(),
                     g_shadow_frontend.last_error_c(),
                     g_shadow_frontend.last_error_d(),
                     g_shadow_packet_sink.failure_reason() != nullptr
                         ? g_shadow_packet_sink.failure_reason()
                         : "frontend-parse",
                     failed.sequence,
                     dolruntime::aurora_recomp::trace_event_name(
                         failed.event.kind),
                     static_cast<unsigned>(failed.event.kind), failed.event.a,
                     failed.event.b, failed.event.c, failed.event.d);
    }
}

unsigned long long shadow_transform_frame_number() {
    return g_frame_open ? g_present_count + 1ull : g_present_count;
}

void log_transform_matrix(const char* label, unsigned index,
                          const float* values) {
    std::fprintf(stderr,
                 "[gfx] draw-transform %s[%u] "
                 "%.8g %.8g %.8g %.8g | %.8g %.8g %.8g %.8g | "
                 "%.8g %.8g %.8g %.8g\n",
                 label, index, values[0], values[1], values[2], values[3],
                 values[4], values[5], values[6], values[7], values[8],
                 values[9], values[10], values[11]);
}

void shadow_transform_observer(
    const dolruntime::aurora_recomp::ConsumedDraw& draw,
    unsigned long long cumulative_draw, void*) {
    const std::size_t frame_draw = g_shadow_transform_next_draw_index++;
    if (!g_shadow_transform_log_enabled ||
        g_shadow_transform_log_count >= g_shadow_transform_log_limit)
        return;
    const unsigned long long frame = shadow_transform_frame_number();
    if (g_shadow_transform_log_min_frame != 0ull &&
        frame < g_shadow_transform_log_min_frame)
        return;
    if (g_shadow_transform_log_frame != 0ull &&
        g_shadow_transform_log_frame != frame)
        return;
    if (g_shadow_transform_log_draw >= 0 &&
        static_cast<unsigned long>(g_shadow_transform_log_draw) != frame_draw)
        return;
    if (g_shadow_transform_log_sequence_enabled &&
        g_shadow_transform_log_sequence != draw.sequence)
        return;
    if (g_shadow_light_log_lit_only) {
        // Keep only draws that actually light: some color channel has
        // enablelighting set (LitChannel bit 1) with a nonzero light mask
        // (bits 2-5 | 11-14). Skipped draws do not consume the log limit.
        bool lit = false;
        for (unsigned c = 0; c < 4 && !lit; ++c) {
            if ((draw.chan_reg_mask & (1u << (5u + c))) == 0u)
                continue;
            const std::uint32_t ctrl = draw.chan_regs[5u + c];
            const std::uint32_t mask =
                ((ctrl >> 2u) & 0xFu) | (((ctrl >> 11u) & 0xFu) << 4u);
            lit = ((ctrl >> 1u) & 0x1u) != 0u && mask != 0u;
        }
        if (!lit)
            return;
    }

    ++g_shadow_transform_log_count;
    std::uint32_t pn_used_mask = 0u;
    if ((draw.transform_flags &
         dolruntime::aurora_recomp::kDrawTransformPayloadPnMatrixValid) != 0u &&
        draw.payload_pn_matrix_mask != 0u) {
        pn_used_mask = draw.payload_pn_matrix_mask;
    } else if (draw.current_pn_matrix < DOL_GX_RECOMP_POSITION_MATRIX_COUNT) {
        pn_used_mask = 1u << draw.current_pn_matrix;
    }
    const std::uint32_t pn_valid_used =
        pn_used_mask & draw.position_matrix_valid_mask;
    std::fprintf(stderr,
                 "[gfx] draw-transform frame=%llu draw=%zu seq=%u "
                 "total=%llu prim=0x%02X fmt=%u count=%u vsize=%u "
                 "payload=%zu cull=%d tex=%u:0x%08X flags=0x%X "
                 "current_pn=%u payload_pn_mask=0x%03X pn_used=0x%03X "
                 "pn_valid=0x%03X pos_valid=0x%03X arrays=%u active=0x%04X\n",
                 frame, frame_draw, draw.sequence, cumulative_draw,
                 draw.primitive, draw.vtx_fmt, draw.vertex_count,
                 draw.vertex_size, draw.vertex_payload.size(),
                 draw.cull_all ? 1 : 0, draw.texture.slot,
                 draw.texture.address, draw.transform_flags,
                 draw.current_pn_matrix, draw.payload_pn_matrix_mask,
                 pn_used_mask, pn_valid_used,
                 draw.position_matrix_valid_mask, draw.array_input_count,
                 draw.active_array_mask);
    if ((draw.transform_flags &
         dolruntime::aurora_recomp::kDrawTransformViewportValid) != 0u) {
        std::fprintf(stderr,
                     "[gfx] draw-transform viewport %.8g %.8g %.8g %.8g "
                     "%.8g %.8g\n",
                     draw.viewport[0], draw.viewport[1], draw.viewport[2],
                     draw.viewport[3], draw.viewport[4], draw.viewport[5]);
    } else {
        std::fprintf(stderr, "[gfx] draw-transform viewport invalid\n");
    }
    if ((draw.transform_flags &
         dolruntime::aurora_recomp::kDrawTransformProjectionValid) != 0u) {
        std::fprintf(stderr,
                     "[gfx] draw-transform projection type=%u %.8g %.8g "
                     "%.8g %.8g %.8g %.8g\n",
                     draw.projection_type, draw.projection[0],
                     draw.projection[1], draw.projection[2],
                     draw.projection[3], draw.projection[4],
                     draw.projection[5]);
    } else {
        std::fprintf(stderr, "[gfx] draw-transform projection invalid\n");
    }
    for (unsigned i = 0; i < DOL_GX_RECOMP_POSITION_MATRIX_COUNT; ++i) {
        const std::uint32_t bit = 1u << i;
        if ((pn_used_mask & bit) == 0u)
            continue;
        if ((draw.position_matrix_valid_mask & bit) == 0u) {
            std::fprintf(stderr,
                         "[gfx] draw-transform pnmtx[%u] invalid\n", i);
            continue;
        }
        log_transform_matrix("pnmtx", i, draw.position_matrices[i]);
    }
    if (!g_shadow_light_log_enabled)
        return;
    // XF channel registers 0x1009..0x1011 in capture order: numChans,
    // amb0/1, mat0/1, color-ctrl0/1, alpha-ctrl0/1. Decode the LitChannel
    // control bits the way Dolphin's XFMemory does so lit draws are
    // greppable without a bit calculator.
    std::fprintf(stderr,
                 "[gfx] draw-light frame=%llu draw=%zu chan_mask=0x%03X "
                 "numchans=%u amb0=0x%08X amb1=0x%08X mat0=0x%08X "
                 "mat1=0x%08X\n",
                 frame, frame_draw, draw.chan_reg_mask,
                 (draw.chan_reg_mask & 0x1u) != 0u ? draw.chan_regs[0] : 0u,
                 draw.chan_regs[1], draw.chan_regs[2], draw.chan_regs[3],
                 draw.chan_regs[4]);
    static const char* const kChanNames[4] = {"cctrl0", "cctrl1", "actrl0",
                                              "actrl1"};
    for (unsigned c = 0; c < 4; ++c) {
        if ((draw.chan_reg_mask & (1u << (5u + c))) == 0u)
            continue;
        const std::uint32_t ctrl = draw.chan_regs[5u + c];
        const unsigned matsrc = ctrl & 0x1u;
        const unsigned lit = (ctrl >> 1u) & 0x1u;
        const unsigned ambsrc = (ctrl >> 6u) & 0x1u;
        const unsigned diffunc = (ctrl >> 7u) & 0x3u;
        const unsigned attnfunc = (ctrl >> 9u) & 0x3u;
        const unsigned light_mask =
            ((ctrl >> 2u) & 0xFu) | (((ctrl >> 11u) & 0xFu) << 4u);
        std::fprintf(stderr,
                     "[gfx] draw-light %s=0x%08X lit=%u matsrc=%u ambsrc=%u "
                     "diffunc=%u attnfunc=%u lights=0x%02X\n",
                     kChanNames[c], ctrl, lit, matsrc, ambsrc, diffunc,
                     attnfunc, light_mask);
    }
    for (unsigned i = 0; i < DOL_GX_RECOMP_LIGHT_COUNT; ++i) {
        if (draw.light_word_mask[i] == 0u)
            continue;
        // Word layout per light (Dolphin XFMemory Light): 0-2 reserved,
        // 3 RGBA8 color, 4-6 cosatt, 7-9 distatt, 10-12 pos, 13-15 dir.
        float f[16];
        std::memcpy(f, draw.light_words[i], sizeof(f));
        std::fprintf(stderr,
                     "[gfx] draw-light light[%u] mask=0x%04X col=0x%08X "
                     "cos=%.8g,%.8g,%.8g dist=%.8g,%.8g,%.8g "
                     "pos=%.8g,%.8g,%.8g dir=%.8g,%.8g,%.8g\n",
                     i, draw.light_word_mask[i], draw.light_words[i][3],
                     f[4], f[5], f[6], f[7], f[8], f[9], f[10], f[11], f[12],
                     f[13], f[14], f[15]);
    }
}
#endif
} // namespace

extern "C" {

void aurora_backend_set_array_guest(u32 attr, u32 guest_address,
                                    const void* data, u32 size, u8 stride);
void aurora_backend_load_texture_guest(u8 slot, u32 guest_address,
                                       const void* data, u32 width, u32 height,
                                       u32 format, u32 tlut, bool mipmap,
                                       u32 object_id, u32 data_version);
void aurora_backend_load_tlut_guest(u8 slot, u32 guest_address,
                                    const void* data, u32 format, u16 entries,
                                    u32 object_id, u32 data_version);
void aurora_backend_set_copy_destination_guest(u32 guest_address,
                                               const void* data);

bool dol_aurora_initialize(int argc, char** argv,
                           const AuroraBackendConfig* backend_config) {
    if (g_initialized)
        return true;

    const AuroraBackendConfig defaults = {
        .app_name = "DolRuntime",
        .window_width = 1280,
        .window_height = 960,
        .vsync = true,
        .allow_texture_dumps = false,
        .info_logging = false,
        .graphics_logging = false,
        .force_untextured = false,
    };
    if (backend_config == nullptr)
        backend_config = &defaults;

    AuroraConfig config{};
    config.appName =
        backend_config->app_name != nullptr ? backend_config->app_name
                                            : defaults.app_name;
    config.desiredBackend = BACKEND_AUTO;
    config.vsync = backend_config->vsync;
    config.windowWidth = backend_config->window_width != 0
                             ? backend_config->window_width
                             : defaults.window_width;
    config.windowHeight = backend_config->window_height != 0
                              ? backend_config->window_height
                              : defaults.window_height;
    config.allowTextureDumps = backend_config->allow_texture_dumps;
    config.logCallback = log_callback;
    config.logLevel = backend_config->info_logging ? LOG_INFO : LOG_ERROR;
    // The recomp runtime owns guest MEM1/ARAM. Aurora receives translated host
    // pointers for graphics resources and must not allocate duplicate memory.
    config.mem1Size = 0;
    config.mem2Size = 0;

    const AuroraInfo info = aurora_initialize(argc, argv, &config);
    g_initialized = info.window != nullptr;
    if (!g_initialized)
        return false;

    g_graphics_log = backend_config->graphics_logging;
    g_force_untextured = backend_config->force_untextured;
    g_frame_pacing_log = std::getenv("DOL_FRAME_PACING_LOG") != nullptr;
#if DOLRUNTIME_HAS_AURORA_RECOMP
    g_shadow_light_log_enabled =
        std::getenv("DOL_AURORA_RECOMP_DRAW_LIGHT_LOG") != nullptr;
    g_shadow_light_log_lit_only =
        std::getenv("DOL_AURORA_RECOMP_DRAW_LIGHT_LIT_ONLY") != nullptr;
    g_shadow_transform_log_enabled =
        std::getenv("DOL_AURORA_RECOMP_DRAW_TRANSFORM_LOG") != nullptr ||
        g_shadow_light_log_enabled;
    g_shadow_transform_log_frame =
        ull_env("DOL_AURORA_RECOMP_DRAW_TRANSFORM_FRAME", 0ull);
    g_shadow_transform_log_min_frame =
        ull_env("DOL_AURORA_RECOMP_DRAW_TRANSFORM_MIN_FRAME", 0ull);
    g_shadow_transform_log_draw =
        long_env("DOL_AURORA_RECOMP_DRAW_TRANSFORM_DRAW", -1);
    const char* transform_sequence =
        std::getenv("DOL_AURORA_RECOMP_DRAW_TRANSFORM_SEQUENCE");
    g_shadow_transform_log_sequence_enabled =
        transform_sequence != nullptr && transform_sequence[0] != '\0';
    g_shadow_transform_log_sequence =
        ull_env("DOL_AURORA_RECOMP_DRAW_TRANSFORM_SEQUENCE", 0ull);
    g_shadow_transform_log_limit = static_cast<unsigned long>(
        ull_env("DOL_AURORA_RECOMP_DRAW_TRANSFORM_LIMIT", 1ull));
    if (g_shadow_transform_log_limit == 0ul)
        g_shadow_transform_log_limit = 1ul;
    g_shadow_transform_log_count = 0;
    g_shadow_transform_next_draw_index = 0;
    g_trace_armed = false;
    g_trace_path.clear();
    g_trace_first_frame = 1ull;
    g_trace_last_frame = ~0ull;
    g_trace_present_scope_frame = 0ull;
    g_trace_frame_begun = false;
    g_trace_frames_recorded = 0ull;
    g_trace_mem_dedup.clear();
    const char* trace_out = std::getenv("DOL_AURORA_RECOMP_TRACE_OUT");
    if (trace_out != nullptr && trace_out[0] != '\0') {
        g_trace_path = trace_out;
        g_trace_armed = true;
        const char* trace_window =
            std::getenv("DOL_AURORA_RECOMP_TRACE_FRAMES");
        if (trace_window != nullptr && trace_window[0] != '\0') {
            char* end = nullptr;
            const unsigned long long first =
                std::strtoull(trace_window, &end, 10);
            bool window_ok = end != trace_window && *end == ':';
            if (window_ok) {
                const char* second = end + 1;
                char* end2 = nullptr;
                const unsigned long long last =
                    std::strtoull(second, &end2, 10);
                window_ok = end2 != second && *end2 == '\0' && last >= first;
                if (window_ok) {
                    g_trace_first_frame = first == 0ull ? 1ull : first;
                    g_trace_last_frame = last;
                }
            }
            if (!window_ok)
                std::fprintf(stderr,
                             "[trace] ignoring malformed "
                             "DOL_AURORA_RECOMP_TRACE_FRAMES=%s (want A:B); "
                             "recording all frames\n",
                             trace_window);
        }
        std::fprintf(stderr, "[trace] recording armed path=%s frames=%llu:%llu\n",
                     g_trace_path.c_str(), g_trace_first_frame,
                     g_trace_last_frame);
    }
    // 63/Mfin default flip: gxcore is the renderer. Opt out only with
    // DOL_GX_CORE=0 (kept as an escape hatch while the live Aurora gx layer
    // is still present in tree).
    {
        const char* core_env = std::getenv("DOL_GX_CORE");
        g_gx_core_enabled = !(core_env != nullptr && core_env[0] == '0');
    }
    g_shadow_frontend_enabled =
        std::getenv("DOL_AURORA_RECOMP_FRONTEND_SHADOW") != nullptr ||
        g_shadow_transform_log_enabled || g_trace_armed || g_gx_core_enabled;
    if (g_gx_core_enabled) {
        g_core_sink.set_plan_observer(core_plan_observer, nullptr);
        g_core_sink.set_copy_observer(core_copy_observer, nullptr);
        g_core_submitted = 0;
        g_core_rejected = 0;
        aurora::gfx::gxcore::reset_texture_cache();
        std::fprintf(stderr,
                     "[gx-core] renderer = gxcore (default): draws route "
                     "through the Dolphin-ported gxcore, live Aurora gx layer "
                     "bypassed\n");
    }
    g_shadow_frontend_failed = false;
    g_shadow_frontend.reset(nullptr);
    g_shadow_frontend.set_packet_drain_enabled(g_shadow_frontend_enabled);
    g_shadow_packet_sink.reset();
    g_shadow_packet_sink.set_streaming(true);
    g_shadow_packet_sink.set_draw_observer(
        g_shadow_transform_log_enabled ? shadow_transform_observer : nullptr,
        nullptr);
    g_shadow_last_draw_total = 0;
    g_shadow_last_vertex_total = 0;
    g_shadow_draw_mismatch_frames = 0;
    g_shadow_last_rawvert_total = 0;
    g_shadow_last_topoidx_total = 0;
    g_shadow_last_storage_total = 0;
    g_shadow_vert_extent_mismatch_frames = 0;
    g_shadow_last_zero_draw_total = 0;
    g_shadow_prev_frame_valid = false;
    if (g_shadow_transform_log_enabled) {
        if (g_shadow_transform_log_sequence_enabled) {
            std::fprintf(stderr,
                         "[gfx] draw-transform log enabled frame=%llu "
                         "draw=%ld min_frame=%llu sequence=%llu limit=%lu\n",
                         g_shadow_transform_log_frame,
                         g_shadow_transform_log_draw,
                         g_shadow_transform_log_min_frame,
                         g_shadow_transform_log_sequence,
                         g_shadow_transform_log_limit);
        } else {
            std::fprintf(stderr,
                         "[gfx] draw-transform log enabled frame=%llu "
                         "draw=%ld min_frame=%llu sequence=any limit=%lu\n",
                         g_shadow_transform_log_frame,
                         g_shadow_transform_log_draw,
                         g_shadow_transform_log_min_frame,
                         g_shadow_transform_log_limit);
        }
    }
#endif
    g_audio_queue_log = std::getenv("DOL_AUDIO_QUEUE_LOG") != nullptr;
    g_audio_prebuffer_ms = audio_ms_env("DOL_AUDIO_PREBUFFER_MS", 40, 20, 500);
    g_audio_max_queue_ms =
        audio_ms_env("DOL_AUDIO_MAX_QUEUE_MS", 250, g_audio_prebuffer_ms, 1000);
    g_audio_push_count = 0;
    g_audio_throttle_count = 0;
    g_audio_low_log_push = 0;
    g_audio_sample_rate = 32000;

    if (SDL_InitSubSystem(SDL_INIT_AUDIO)) {
        SDL_AudioSpec spec{};
        spec.format = SDL_AUDIO_S16;
        spec.channels = 2;
        spec.freq = static_cast<int>(g_audio_sample_rate);
        g_audio_stream = SDL_OpenAudioDeviceStream(
            SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, nullptr, nullptr);
        if (g_audio_stream == nullptr)
            std::fprintf(stderr, "[audio] SDL output unavailable: %s\n", SDL_GetError());
        else if (g_audio_queue_log)
            std::fprintf(stderr, "[audio-queue] prebuffer_ms=%d max_queue_ms=%d\n",
                         g_audio_prebuffer_ms, g_audio_max_queue_ms);
    } else {
        std::fprintf(stderr, "[audio] SDL audio initialization failed: %s\n", SDL_GetError());
    }

    poll_events();
    g_frame_open = !g_should_quit && aurora_begin_frame();
    install_platform_ops();
    return true;
}

void dol_aurora_shutdown(void) {
    if (!g_initialized)
        return;
    dol_platform_reset();
    if (g_frame_open) {
        aurora_end_frame();
        g_frame_open = false;
    }
#if DOLRUNTIME_HAS_AURORA_RECOMP
    trace_close_and_log();
    if (g_gx_core_enabled) {
        std::fprintf(stderr,
                     "[gx-core] shutdown: submitted=%llu rejected=%llu "
                     "failed=%d\n",
                     g_core_submitted, g_core_rejected,
                     g_shadow_frontend_failed ? 1 : 0);
    }
#endif
    if (g_audio_stream != nullptr) {
        SDL_DestroyAudioStream(g_audio_stream);
        g_audio_stream = nullptr;
        g_audio_playing = false;
    }
    aurora_shutdown();
    g_pending_textures = {};
    g_pending_tluts = {};
    g_draw_opcode_pending = false;
    g_initialized = false;
}

bool aurora_backend_should_quit(void) {
    return g_should_quit;
}

void aurora_backend_present(void) {
    if (!g_initialized)
        return;
#if DOLRUNTIME_HAS_AURORA_RECOMP
    // gxcore accumulates the frame's draws as their spans complete; the final
    // pending draw is only planned at the frame boundary. Submit it into the
    // still-open Aurora frame before end_frame flushes the command list.
    if (g_gx_core_enabled && !g_shadow_frontend_failed)
        g_core_sink.flush_frame();
    // TEMP DIAG: per-frame gxcore gap-counter deltas so a live
    // goal celebration reveals which skip reason dominates (missing_vcd /
    // "projection never captured" vertex_decode_failures / cull_all_draws vs.
    // draws actually planned). Gated by STRIKERS_CUTSCENE_DIAG.
    if (g_gx_core_enabled) {
        static int s_cutscene_diag = -1;
        if (s_cutscene_diag < 0)
            s_cutscene_diag = std::getenv("STRIKERS_CUTSCENE_DIAG") != nullptr ? 1 : 0;
        if (s_cutscene_diag) {
            static dolruntime::gxcore::GapCounters s_prev{};
            const dolruntime::gxcore::GapCounters& c = g_core_sink.counters();
            std::fprintf(stderr,
                "[cutscene] present=%llu planned=%llu skipped=%llu cull=%llu "
                "missing_vcd=%llu vtx_fail=%llu\n",
                g_present_count + 1,
                c.draws_planned - s_prev.draws_planned,
                c.draws_skipped - s_prev.draws_skipped,
                c.cull_all_draws - s_prev.cull_all_draws,
                c.missing_vcd - s_prev.missing_vcd,
                c.vertex_decode_failures - s_prev.vertex_decode_failures);
            s_prev = c;
        }
    }
#endif
    if (g_frame_open) {
        aurora_end_frame();
        g_frame_open = false;
    }
    ++g_present_count;
    if (g_frame_pacing_log &&
        (g_present_count <= 10 || (g_present_count % 60) == 0)) {
        const AuroraStats* stats = aurora_get_stats();
        std::fprintf(stderr,
                     "[frame-pacing] frame=%llu fps=%.1f draws=%u "
                     "texture-upload=%u fifo=%llu\n",
                     g_present_count, aurora_get_fps(), stats->drawCallCount,
                     stats->lastTextureUploadSize, g_fifo_bytes);
    }
    if (g_graphics_log) {
        const AuroraStats* s = aurora_get_stats();
        std::fprintf(stderr,
                     "[gfxN] present=%llu draws=%u merged=%u verts=%u indices=%u "
                     "fifo=%llu\n",
                     g_present_count, s->drawCallCount, s->mergedDrawCallCount,
                     s->lastVertSize, s->lastIndexSize, g_fifo_bytes);
    }
#if DOLRUNTIME_HAS_AURORA_RECOMP
    // Frame scope for trace records emitted during present (the sink's
    // final-draw assembly below resolves guest memory): they belong to the
    // frame that just presented, not the next one.
    g_trace_present_scope_frame = g_present_count;
    if (g_shadow_frontend_enabled && !g_gx_core_enabled) {
        // Diff shadow-decoded frame counters against live Aurora counters.
        // Aurora drawCallCount is per-frame (reset at begin_frame) but its
        // public AuroraStats copy is published by the render worker one
        // present late (see the g_shadow_prev_* comment above), so the sample
        // read here describes the PREVIOUS frame: gate it against the held
        // previous-frame frontend deltas. The sink counts are cumulative, so
        // subtract the previous present's totals to get per-frame deltas.
        // Assemble the frame's final draw so every draw this frame is folded
        // into the assembly counters (intermediate draws assemble on the next
        // draw's arrival).
        g_shadow_packet_sink.flush_assembly();
        const AuroraStats* astats = aurora_get_stats();
        const unsigned long long fe_draw_total = g_shadow_packet_sink.draw_packets();
        const unsigned long long fe_vtx_total = g_shadow_packet_sink.vertex_inputs();
        const unsigned long long fe_zero_draw_total =
            g_shadow_frontend.zero_vertex_draws();
        const unsigned long long fe_frame_draws =
            fe_draw_total - g_shadow_last_draw_total;
        const unsigned long long fe_frame_verts =
            fe_vtx_total - g_shadow_last_vertex_total;
        const unsigned long long fe_frame_zero_draws =
            fe_zero_draw_total - g_shadow_last_zero_draw_total;
        g_shadow_last_draw_total = fe_draw_total;
        g_shadow_last_vertex_total = fe_vtx_total;
        g_shadow_last_zero_draw_total = fe_zero_draw_total;
        const unsigned long long fe_rawvert_total =
            g_shadow_packet_sink.raw_vertex_bytes();
        const unsigned long long fe_topoidx_total =
            g_shadow_packet_sink.topology_index_bytes();
        const unsigned long long fe_storage_total =
            g_shadow_packet_sink.storage_bytes();
        const unsigned long long fe_frame_rawvert =
            fe_rawvert_total - g_shadow_last_rawvert_total;
        const unsigned long long fe_frame_topoidx =
            fe_topoidx_total - g_shadow_last_topoidx_total;
        const unsigned long long fe_frame_storage =
            fe_storage_total - g_shadow_last_storage_total;
        g_shadow_last_rawvert_total = fe_rawvert_total;
        g_shadow_last_topoidx_total = fe_topoidx_total;
        g_shadow_last_storage_total = fe_storage_total;
        // Gate the stats sample against the previous frame's frontend
        // deltas. Zero-vertex retail draws bump Aurora's drawCallCount but
        // are frontend no-ops, so they join the draw gate; they push no
        // vertex bytes, so the alignment band stays on real draws only.
        const bool have_prev = g_shadow_prev_frame_valid;
        const bool draw_match =
            !have_prev ||
            (!g_shadow_frontend_failed &&
             g_shadow_prev_draws + g_shadow_prev_zero_draws ==
                 astats->drawCallCount);
        if (!draw_match)
            ++g_shadow_draw_mismatch_frames;
        // Raw vertex bytes are merge-invariant; Aurora's push_verts pads each
        // unmerged draw up to 4 bytes, so native >= ours and the excess is
        // bounded by 4 bytes per frontend draw. Outside that band is a real
        // decode/payload divergence.
        const unsigned long long vert_align_slack =
            4ull * g_shadow_prev_draws + 4ull;
        const bool vert_extent_match =
            !have_prev ||
            (!g_shadow_frontend_failed &&
             astats->lastVertSize >= g_shadow_prev_rawvert &&
             (astats->lastVertSize - g_shadow_prev_rawvert) <=
                 vert_align_slack);
        if (!vert_extent_match)
            ++g_shadow_vert_extent_mismatch_frames;
        if (g_graphics_log && have_prev &&
            (g_present_count <= 10 || g_present_count % 60 == 0 ||
             !draw_match || !vert_extent_match)) {
            std::fprintf(stderr,
                         "[gfx] shadow-consume failed=%d packets=%llu "
                         "draws=%llu textures=%llu copies=%llu spans=%llu "
                         "array-inputs=%llu/%llu (resolved/unresolved) "
                         "assembled=%llu/%llu elems=%llu (ok/fail)\n",
                         g_shadow_frontend_failed ? 1 : 0,
                         g_shadow_packet_sink.packets(),
                         fe_draw_total,
                         g_shadow_packet_sink.texture_count(),
                         g_shadow_packet_sink.copy_count(),
                         g_shadow_packet_sink.indexed_span_count(),
                         g_shadow_packet_sink.resolved_array_inputs(),
                         g_shadow_packet_sink.unresolved_array_inputs(),
                         g_shadow_packet_sink.assembled_draws() -
                             g_shadow_packet_sink.assemble_failed_draws(),
                         g_shadow_packet_sink.assemble_failed_draws(),
                         g_shadow_packet_sink.assembled_elements());
            std::fprintf(stderr,
                         "[gfx] shadow-diff frame=%llu fe_draws=%llu "
                         "fe_zdraws=%llu aurora_draws=%u match=%d "
                         "fe_verts=%llu mismatch_frames=%llu\n",
                         g_shadow_prev_frame_index, g_shadow_prev_draws,
                         g_shadow_prev_zero_draws, astats->drawCallCount,
                         draw_match ? 1 : 0, g_shadow_prev_verts,
                         g_shadow_draw_mismatch_frames);
            // Draw-input extent parity: raw verts hard-gated (band), topology
            // index + storage logged with merge/cache caveats.
            std::fprintf(stderr,
                         "[gfx] shadow-extent frame=%llu fe_vertB=%llu "
                         "aurora_vertB=%u vmatch=%d vmiss=%llu | fe_idxB=%llu "
                         "aurora_idxB=%u | fe_storeB=%llu aurora_storeB=%u\n",
                         g_shadow_prev_frame_index, g_shadow_prev_rawvert,
                         astats->lastVertSize, vert_extent_match ? 1 : 0,
                         g_shadow_vert_extent_mismatch_frames,
                         g_shadow_prev_topoidx, astats->lastIndexSize,
                         g_shadow_prev_storage, astats->lastStorageSize);
        }
        g_shadow_prev_frame_valid = true;
        g_shadow_prev_frame_index = g_present_count;
        g_shadow_prev_draws = fe_frame_draws;
        g_shadow_prev_zero_draws = fe_frame_zero_draws;
        g_shadow_prev_verts = fe_frame_verts;
        g_shadow_prev_rawvert = fe_frame_rawvert;
        g_shadow_prev_topoidx = fe_frame_topoidx;
        g_shadow_prev_storage = fe_frame_storage;
    }
    trace_on_present();
#endif
    if (g_graphics_log && (g_present_count <= 10 || g_present_count % 60 == 0)) {
        const AuroraStats* stats = aurora_get_stats();
        const auto& gx = aurora::gx::g_gxState;
        std::fprintf(stderr,
                     "[gfx] frame=%llu fifo=%llu draws=%u merged=%u verts=%u "
                     "uniforms=%u indices=%u textures=%u fps=%.1f\n",
                     g_present_count, g_fifo_bytes, stats->drawCallCount,
                     stats->mergedDrawCallCount, stats->lastVertSize,
                     stats->lastUniformSize, stats->lastIndexSize,
                     stats->lastTextureUploadSize, aurora_get_fps());
        std::fprintf(
            stderr,
            "[gfx] parsed-state tev=%u texgen=%u chans=%u ind=%u cull=%u "
            "blend=%u/%u/%u depth=%u/%u/%u write=%u/%u "
            "viewport=(%.1f,%.1f %.1fx%.1f %.3f..%.3f) "
            "scissor=(%d,%d %dx%d) pnmtx=%u\n",
            gx.numTevStages, gx.numTexGens, gx.numChans, gx.numIndStages,
            static_cast<unsigned>(gx.cullMode),
            static_cast<unsigned>(gx.blendMode),
            static_cast<unsigned>(gx.blendFacSrc),
            static_cast<unsigned>(gx.blendFacDst), gx.depthCompare,
            static_cast<unsigned>(gx.depthFunc), gx.depthUpdate, gx.colorUpdate,
            gx.alphaUpdate, gx.logicalViewport.left, gx.logicalViewport.top,
            gx.logicalViewport.width, gx.logicalViewport.height,
            gx.logicalViewport.znear, gx.logicalViewport.zfar,
            gx.logicalScissor.x, gx.logicalScissor.y,
            gx.logicalScissor.width, gx.logicalScissor.height, gx.currentPnMtx);
        std::fprintf(stderr, "[gfx] projection");
        for (unsigned row = 0; row < 4; row++)
            for (unsigned col = 0; col < 4; col++)
                std::fprintf(stderr, " %.5g", gx.proj[row][col]);
        fputc('\n', stderr);
        std::fprintf(stderr, "[gfx] pnmtx[%u]", gx.currentPnMtx);
        const auto& pos = gx.pnMtx[gx.currentPnMtx].pos;
        const float* pos_values = reinterpret_cast<const float*>(&pos);
        for (unsigned i = 0; i < 12; i++)
            std::fprintf(stderr, " %.5g", pos_values[i]);
        fputc('\n', stderr);
        for (unsigned attr : {9u, 11u, 13u}) {
            const auto& fmt = gx.vtxFmts[0].attrs[attr];
            const auto& array = gx.arrays[attr];
            std::fprintf(stderr,
                         "[gfx] attr=%u desc=%u cnt=%u type=%u frac=%u "
                         "array=%p size=%u stride=%u le=%u\n",
                         attr, static_cast<unsigned>(gx.vtxDesc[attr]),
                         static_cast<unsigned>(fmt.cnt),
                         static_cast<unsigned>(fmt.type), fmt.frac, array.data,
                         array.size, array.stride, array.le);
        }
        if (gx.numTexGens != 0) {
            const auto& tcg = gx.tcgs[0];
            std::fprintf(stderr,
                         "[gfx] tcg0 type=%u src=%u mtx=%u post=%u norm=%u\n",
                         static_cast<unsigned>(tcg.type),
                         static_cast<unsigned>(tcg.src),
                         static_cast<unsigned>(tcg.mtx),
                         static_cast<unsigned>(tcg.postMtx), tcg.normalize);
        }
        if (gx.numTevStages != 0) {
            const auto& tev = gx.tevStages[0];
            std::fprintf(
                stderr,
                "[gfx] tev0 texcoord=%u texmap=%u chan=%u "
                "color=%u,%u,%u,%u alpha=%u,%u,%u,%u\n",
                static_cast<unsigned>(tev.texCoordId),
                static_cast<unsigned>(tev.texMapId),
                static_cast<unsigned>(tev.channelId),
                static_cast<unsigned>(tev.colorPass.a),
                static_cast<unsigned>(tev.colorPass.b),
                static_cast<unsigned>(tev.colorPass.c),
                static_cast<unsigned>(tev.colorPass.d),
                static_cast<unsigned>(tev.alphaPass.a),
                static_cast<unsigned>(tev.alphaPass.b),
                static_cast<unsigned>(tev.alphaPass.c),
                static_cast<unsigned>(tev.alphaPass.d));
        }
    }
    const unsigned long long present_fifo = g_fifo_bytes;
    g_fifo_bytes = 0;
    poll_events();
    if (!g_should_quit) {
        g_frame_open = aurora_begin_frame();
#if DOLRUNTIME_HAS_AURORA_RECOMP
        g_shadow_transform_next_draw_index = 0;
#endif
        if (g_graphics_log && !g_frame_open)
            std::fprintf(stderr,
                         "[gfx] WARN aurora_begin_frame()=false present=%llu "
                         "fifo_this_interval=%llu (FIFO will accumulate "
                         "undrained)\n",
                         g_present_count, present_fifo);
    }
    if (g_graphics_log && present_fifo > 50000ull)
        std::fprintf(stderr, "[gfx] LARGE present=%llu fifo_this_interval=%llu\n",
                     g_present_count, present_fifo);
}

void aurora_backend_mark_gx_begin(void) {
    g_draw_opcode_pending = true;
}

void aurora_backend_call_display_list(const void* data, u32 size) {
    if (!g_initialized || data == nullptr || size == 0)
        return;
#if DOLRUNTIME_HAS_AURORA_RECOMP
    // Mirror BEFORE the trace record: the shadow DL parse resolves interior
    // guest references (arrays/textures) and those MEM_UPDATE records must
    // precede this CALL_DL in the trace, same ordering as gx_write.
    shadow_frontend_call_display_list(data, size);
    // Guest address is not plumbed to this seam yet (guest_addr=0 is the
    // documented "unknown" value); replay feeds the recorded bytes directly.
    if (trace_should_record())
        g_trace_writer.call_display_list(0u, data, size);
    if (g_gx_core_enabled) {
        // gxcore decoded this DL via the shadow mirror above; the live gx layer
        // is bypassed. Account the bytes for pacing logs and stop.
        g_fifo_bytes += size;
        return;
    }
#endif
    flush_pending_resource_metadata();
    aurora::gx::fifo::write_data(data, size);
    g_fifo_bytes += size;
}

void aurora_backend_gx_write(u64 value, u8 size) {
    if (!g_initialized)
        return;
    g_fifo_bytes += size;
#if DOLRUNTIME_HAS_AURORA_RECOMP
    shadow_frontend_write(value, size);
    // Record AFTER the shadow write so MEM_UPDATEs its resolves emitted
    // precede this GX_WRITE in the trace (replay reads them first), and
    // BEFORE the live-Aurora forwarding below so the pristine value is
    // captured ahead of the cull-all interception rewrites.
    if (trace_should_record())
        g_trace_writer.gx_write(size, value);
    if (g_gx_core_enabled)
        return; // gxcore renders from the frontend stream; skip live Aurora gx
#endif

    static u32 s_last_zmode = 0x40000017; // depthCompare=1, depthFunc=LEQUAL(3), depthUpdate=1
    static u32 s_last_cmode0 = 0x410004BC; // dither=1, colorUpdate=1, alphaUpdate=1, blendDst=INVSRCALPHA(5), blendSrc=SRCALPHA(4)
    static bool s_cull_all_active = false;
    static u8 s_last_opcode = 0;

    switch (size) {
    case 1: {
        const u8 command = static_cast<u8>(value);
        s_last_opcode = command;
        if (g_draw_opcode_pending && command >= 0x80u) {
            g_draw_opcode_pending = false;
            flush_pending_resource_metadata();
            if (g_force_untextured) {
                GXSetCullMode(GX_CULL_NONE);
                GXSetZMode(GX_FALSE, GX_ALWAYS, GX_FALSE);
                GXSetBlendMode(GX_BM_NONE, GX_BL_ONE, GX_BL_ZERO, GX_LO_CLEAR);
                GXSetColorUpdate(GX_TRUE);
                GXSetAlphaUpdate(GX_TRUE);
                GXSetNumTexGens(0);
                GXSetNumChans(1);
                GXSetNumTevStages(1);
                GXSetTevOrder(GX_TEVSTAGE0, GX_TEXCOORD_NULL, GX_TEXMAP_NULL,
                              GX_COLOR0A0);
                GXSetTevOp(GX_TEVSTAGE0, GX_PASSCLR);
            }
        }
        GXParam1u8(command);
        break;
    }
    case 2: GXParam1u16(static_cast<u16>(value)); break;
    case 4: {
        u32 val32 = static_cast<u32>(value);

        if (s_last_opcode == 0x61) {
            u8 regId = (val32 >> 24) & 0xFF;

            if (regId == 0x40) {
                s_last_zmode = val32;
                if (s_cull_all_active) {
                    // Keep depthCompare and depthUpdate disabled during cull-all
                    val32 &= ~0x11;
                }
                GXParam1u32(val32);
            } else if (regId == 0x41) {
                s_last_cmode0 = val32;
                if (s_cull_all_active) {
                    // Keep colorUpdate and alphaUpdate disabled during cull-all
                    val32 &= ~0x18;
                }
                GXParam1u32(val32);
            } else if (regId == 0x00) {
                u32 hwCull = (val32 >> 14) & 3;
                if (hwCull == 3) {
                    // Clamp cull mode to GX_CULL_NONE (0) to avoid Aurora abort
                    val32 &= ~(3u << 14);
                    // Write the completed genMode parameter first to satisfy the pending 0x61 opcode!
                    GXParam1u32(val32);

                    if (!s_cull_all_active) {
                        s_cull_all_active = true;
                        // Write disabled zmode and cmode0 to FIFO with leading 0x61 opcodes
                        GXParam1u8(0x61);
                        GXParam1u32(s_last_zmode & ~0x11);
                        GXParam1u8(0x61);
                        GXParam1u32(s_last_cmode0 & ~0x18);
                    }
                } else {
                    // Write the completed genMode parameter first to satisfy the pending 0x61 opcode!
                    GXParam1u32(val32);

                    if (s_cull_all_active) {
                        s_cull_all_active = false;
                        // Restore game's intended zmode and cmode0 states with leading 0x61 opcodes
                        GXParam1u8(0x61);
                        GXParam1u32(s_last_zmode);
                        GXParam1u8(0x61);
                        GXParam1u32(s_last_cmode0);
                    }
                }
            } else {
                GXParam1u32(val32);
            }
        } else {
            GXParam1u32(val32);
        }
        break;
    }
    case 8: GXCmd1u64(value); break;
    default: break;
    }
}

void aurora_backend_set_array(u32 attr, const void* data, u32 size, u8 stride) {
    aurora_backend_set_array_guest(attr, 0u, data, size, stride);
}

void aurora_backend_set_array_guest(u32 attr, u32 guest_address,
                                    const void* data, u32 size, u8 stride) {
    if (!g_initialized || data == nullptr)
        return;
#if DOLRUNTIME_HAS_AURORA_RECOMP
    if (guest_address != 0u)
        shadow_frontend_set_array(attr, guest_address, stride);
#endif
    GXSetArray(static_cast<GXAttr>(attr), data, size, stride, false);
}

void aurora_backend_load_texture(u8 slot, const void* data, u32 width, u32 height,
                                 u32 format, u32 tlut, bool mipmap, u32 object_id,
                                 u32 data_version) {
    aurora_backend_load_texture_guest(slot, 0u, data, width, height, format,
                                      tlut, mipmap, object_id, data_version);
}

void aurora_backend_load_texture_guest(u8 slot, u32 guest_address,
                                       const void* data, u32 width, u32 height,
                                       u32 format, u32 tlut, bool mipmap,
                                       u32 object_id, u32 data_version) {
    (void)guest_address;
    if (!g_initialized || data == nullptr || slot >= 8)
        return;
    g_pending_textures[slot] = {
        .valid = true,
        .data = data,
        .width = width,
        .height = height,
        .format = format,
        .tlut = tlut,
        .mipmap = mipmap,
        .object_id = object_id,
        .data_version = data_version,
    };
}

void aurora_backend_load_tlut(u8 slot, const void* data, u32 format, u16 entries,
                              u32 object_id, u32 data_version) {
    aurora_backend_load_tlut_guest(slot, 0u, data, format, entries, object_id,
                                   data_version);
}

void aurora_backend_load_tlut_guest(u8 slot, u32 guest_address,
                                    const void* data, u32 format, u16 entries,
                                    u32 object_id, u32 data_version) {
    (void)guest_address;
    if (!g_initialized || data == nullptr || slot >= g_pending_tluts.size())
        return;
    g_pending_tluts[slot] = {
        .valid = true,
        .data = data,
        .format = format,
        .entries = entries,
        .object_id = object_id,
        .data_version = data_version,
    };
}

void aurora_backend_set_copy_destination(const void* data) {
    aurora_backend_set_copy_destination_guest(0u, data);
}

void aurora_backend_set_copy_destination_guest(u32 guest_address,
                                               const void* data) {
    (void)guest_address;
    if (!g_initialized || data == nullptr)
        return;
    write_aurora_command(GX_AURORA_LOAD_COPY_DEST);
    GXCmd1u64(reinterpret_cast<u64>(data));
}

void aurora_backend_set_guest_address_resolver(
    DolPlatformGuestAddressResolverFn resolve, void* user) {
    g_guest_address_resolver = resolve;
    g_guest_address_resolver_user = user;
#if DOLRUNTIME_HAS_AURORA_RECOMP
    if (resolve != nullptr) {
        DolGuestAddressResolver frontend_resolver;
        dol_guest_address_resolver_init_callback(
            &frontend_resolver, frontend_guest_address_resolver_bridge,
            nullptr);
        g_shadow_frontend.reset(&frontend_resolver);
        g_shadow_frontend.set_packet_drain_enabled(g_shadow_frontend_enabled);
        g_shadow_packet_sink.reset();
        // Resolve packet guest addresses to host bytes through the frontend's
        // own persistent resolver copy (the local above is stack-scoped).
        g_shadow_packet_sink.set_guest_resolver(
            &g_shadow_frontend.state().resolver);
        if (g_gx_core_enabled)
            g_core_sink.set_guest_resolver(&g_shadow_frontend.state().resolver);
        g_shadow_frontend_failed = false;
    } else {
        g_shadow_frontend.reset(nullptr);
        g_shadow_frontend.set_packet_drain_enabled(g_shadow_frontend_enabled);
        g_shadow_packet_sink.reset();
        g_shadow_packet_sink.set_guest_resolver(nullptr);
    }
    // Sink cumulative counters were just reset; realign the per-frame diff
    // baselines so the next present computes a correct frame delta.
    g_shadow_last_draw_total = 0;
    g_shadow_last_vertex_total = 0;
    g_shadow_last_rawvert_total = 0;
    g_shadow_last_topoidx_total = 0;
    g_shadow_last_storage_total = 0;
#endif
    if (resolve != nullptr) {
        aurora::gx::recomp::set_guest_address_resolver(
            aurora_guest_address_resolver_bridge, nullptr);
    } else {
        aurora::gx::recomp::clear_guest_address_resolver();
    }
}

void aurora_backend_configure_vi(u32 tv_mode, u16 fb_width, u16 efb_height,
                                 u16 xfb_height, u16 vi_width, u16 vi_height) {
    if (!g_initialized)
        return;
    GXRenderModeObj mode{};
    mode.viTVmode = static_cast<VITVMode>(tv_mode);
    mode.fbWidth = fb_width;
    mode.efbHeight = efb_height;
    mode.xfbHeight = xfb_height;
    mode.viWidth = vi_width;
    mode.viHeight = vi_height;
    VIConfigure(&mode);
}

bool aurora_backend_pad_init(void) {
    if (PADInit() == FALSE)
        return false;

    // Force Aurora to load any persisted keyboard mapping first. Install a
    // usable port-0 fallback only when the user has not configured one.
    PADStatus initial[4]{};
    (void)PADRead(initial);
    u32 count = 0;
    if (PADGetKeyButtonBindings(0, &count) == nullptr) {
        PADKeyButtonBinding buttons[PAD_BUTTON_COUNT] = {
            {SDL_SCANCODE_LEFT,   PAD_BUTTON_LEFT},
            {SDL_SCANCODE_RIGHT,  PAD_BUTTON_RIGHT},
            {SDL_SCANCODE_DOWN,   PAD_BUTTON_DOWN},
            {SDL_SCANCODE_UP,     PAD_BUTTON_UP},
            {SDL_SCANCODE_Q,      PAD_TRIGGER_Z},
            {SDL_SCANCODE_R,      PAD_TRIGGER_R},
            {SDL_SCANCODE_E,      PAD_TRIGGER_L},
            {SDL_SCANCODE_J,      PAD_BUTTON_A},
            {SDL_SCANCODE_K,      PAD_BUTTON_B},
            {SDL_SCANCODE_U,      PAD_BUTTON_X},
            {SDL_SCANCODE_I,      PAD_BUTTON_Y},
            {SDL_SCANCODE_RETURN, PAD_BUTTON_START},
        };
        PADKeyAxisBinding axes[PAD_AXIS_COUNT] = {
            {SDL_SCANCODE_D, PAD_AXIS_LEFT_X_POS, 32767},
            {SDL_SCANCODE_A, PAD_AXIS_LEFT_X_NEG, 32767},
            {SDL_SCANCODE_W, PAD_AXIS_LEFT_Y_POS, 32767},
            {SDL_SCANCODE_S, PAD_AXIS_LEFT_Y_NEG, 32767},
            {SDL_SCANCODE_H, PAD_AXIS_RIGHT_X_POS, 32767},
            {SDL_SCANCODE_F, PAD_AXIS_RIGHT_X_NEG, 32767},
            {SDL_SCANCODE_T, PAD_AXIS_RIGHT_Y_POS, 32767},
            {SDL_SCANCODE_G, PAD_AXIS_RIGHT_Y_NEG, 32767},
            {SDL_SCANCODE_E, PAD_AXIS_TRIGGER_L, 32767},
            {SDL_SCANCODE_R, PAD_AXIS_TRIGGER_R, 32767},
        };
        PADSetKeyButtonBindings(0, buttons);
        PADSetKeyAxisBindings(0, axes);
        PADSetKeyboardActive(0, TRUE);
    }
    return true;
}

u32 aurora_backend_pad_read(DolPadState state[4]) {
    PADStatus status[4]{};
    const u32 motor_mask = PADRead(status);
    for (u32 i = 0; i < 4; i++) {
        state[i] = {
            .button = status[i].button,
            .stick_x = status[i].stickX,
            .stick_y = status[i].stickY,
            .substick_x = status[i].substickX,
            .substick_y = status[i].substickY,
            .trigger_left = status[i].triggerLeft,
            .trigger_right = status[i].triggerRight,
            .analog_a = status[i].analogA,
            .analog_b = status[i].analogB,
            .error = status[i].err,
        };
    }
    return motor_mask;
}

bool aurora_backend_pad_reset(u32 mask) {
    return PADReset(mask) != FALSE;
}

bool aurora_backend_pad_recalibrate(u32 mask) {
    return PADRecalibrate(mask) != FALSE;
}

void aurora_backend_pad_control_motor(u32 channel, u32 command) {
    PADControlMotor(channel, command);
}

void aurora_backend_pad_set_spec(u32 spec) {
    PADSetSpec(spec);
}

void aurora_backend_audio_set_sample_rate(u32 sample_rate) {
    if (sample_rate != 48000)
        sample_rate = 32000;
    if (g_audio_sample_rate == sample_rate)
        return;
    g_audio_sample_rate = sample_rate;
    if (g_audio_stream == nullptr)
        return;

    SDL_AudioSpec spec{};
    spec.format = SDL_AUDIO_S16;
    spec.channels = 2;
    spec.freq = static_cast<int>(g_audio_sample_rate);
    if (!SDL_SetAudioStreamFormat(g_audio_stream, &spec, nullptr))
        std::fprintf(stderr, "[audio] failed to set input rate %u: %s\n",
                     g_audio_sample_rate, SDL_GetError());
    else if (g_audio_queue_log)
        std::fprintf(stderr, "[audio-queue] input-rate=%u\n",
                     g_audio_sample_rate);
}

void aurora_backend_audio_push(const s16* samples, u32 frames) {
    if (g_audio_stream == nullptr || samples == nullptr || frames == 0)
        return;
    const int bytes_per_second =
        static_cast<int>(g_audio_sample_rate) * 2 * static_cast<int>(sizeof(s16));
    const int prebuffer_bytes = bytes_per_second * g_audio_prebuffer_ms / 1000;
    const int max_queued_bytes = bytes_per_second * g_audio_max_queue_ms / 1000;
    const int bytes = static_cast<int>(frames * 2u * sizeof(s16));
    g_audio_push_count++;

    int queued = SDL_GetAudioStreamQueued(g_audio_stream);
    unsigned waited_ms = 0;
    while (queued > max_queued_bytes && waited_ms < 20u) {
        g_audio_throttle_count++;
        if (g_audio_queue_log &&
            (g_audio_throttle_count <= 8 ||
             (g_audio_throttle_count % 100) == 0))
            std::fprintf(stderr,
                         "[audio-queue] throttle push=%llu waits=%llu queued=%d "
                         "max=%d\n",
                         g_audio_push_count, g_audio_throttle_count, queued,
                         max_queued_bytes);
        SDL_Delay(1);
        waited_ms++;
        queued = SDL_GetAudioStreamQueued(g_audio_stream);
    }

    const bool low_queue =
        queued >= 0 && queued < bytes_per_second / 100;
    if (g_audio_queue_log &&
        (g_audio_push_count <= 16 || (g_audio_push_count % 4000) == 0 ||
         (low_queue && g_audio_push_count - g_audio_low_log_push >= 4000))) {
        if (low_queue)
            g_audio_low_log_push = g_audio_push_count;
        std::fprintf(stderr,
                     "[audio-queue] push=%llu queued_before=%d queued_after=%d "
                     "playing=%u throttles=%llu\n",
                     g_audio_push_count, queued, queued + bytes,
                     g_audio_playing ? 1u : 0u, g_audio_throttle_count);
    }
    if (!SDL_PutAudioStreamData(g_audio_stream, samples, bytes))
        std::fprintf(stderr, "[audio] failed to queue samples: %s\n", SDL_GetError());
    else if (!g_audio_playing && queued + bytes >= prebuffer_bytes) {
        if (SDL_ResumeAudioStreamDevice(g_audio_stream))
            g_audio_playing = true;
        else
            std::fprintf(stderr, "[audio] failed to start playback: %s\n", SDL_GetError());
    }
}

} // extern "C"

namespace {
void install_platform_ops() {
    const DolPlatformOps ops = {
        .should_quit = aurora_backend_should_quit,
        .present = aurora_backend_present,
        .mark_gx_begin = aurora_backend_mark_gx_begin,
        .gx_write = aurora_backend_gx_write,
        .call_display_list = aurora_backend_call_display_list,
        .set_array = aurora_backend_set_array,
        .set_array_guest = aurora_backend_set_array_guest,
        .load_texture = aurora_backend_load_texture,
        .load_texture_guest = aurora_backend_load_texture_guest,
        .load_tlut = aurora_backend_load_tlut,
        .load_tlut_guest = aurora_backend_load_tlut_guest,
        .set_copy_destination = aurora_backend_set_copy_destination,
        .set_copy_destination_guest = aurora_backend_set_copy_destination_guest,
        .set_guest_address_resolver = aurora_backend_set_guest_address_resolver,
        .configure_vi = aurora_backend_configure_vi,
        .pad_init = aurora_backend_pad_init,
        .pad_read = aurora_backend_pad_read,
        .pad_reset = aurora_backend_pad_reset,
        .pad_recalibrate = aurora_backend_pad_recalibrate,
        .pad_control_motor = aurora_backend_pad_control_motor,
        .pad_set_spec = aurora_backend_pad_set_spec,
        .audio_set_sample_rate = aurora_backend_audio_set_sample_rate,
        .audio_push = aurora_backend_audio_push,
    };
    dol_platform_install(&ops);
}
} // namespace
