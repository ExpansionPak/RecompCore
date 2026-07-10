// RecompCore: StaticRecomp CPU core.
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/PowerPC/StaticRecomp/StaticRecompCore.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "Common/Config/Config.h"
#include "Common/FileUtil.h"
#include "Common/Logging/Log.h"
#include "Common/Swap.h"
#include "Core/Config/MainSettings.h"
#include "Core/ConfigManager.h"
#include "Core/CoreTiming.h"
#include "Core/HW/CPU.h"
#include "Core/HW/GPFifo.h"
#include "Core/HW/Memmap.h"
#include "Core/HW/SystemTimers.h"
#include "Core/PowerPC/Gekko.h"
#include "Core/PowerPC/Interpreter/Interpreter.h"
#include "Core/PowerPC/JitInterface.h"
#include "Core/PowerPC/MMU.h"
#include "Core/PowerPC/PowerPC.h"
#include "Core/PowerPC/StaticRecomp/StaticRecompLockstep.h"
#include "Core/System.h"

#ifdef _M_X86_64
#include "Core/PowerPC/Jit64/Jit.h"
#endif
#ifdef _M_ARM_64
#include "Core/PowerPC/JitArm64/Jit.h"
#endif

namespace
{
// D3: guest time is charged by the generated code itself (ABI v2). Each
// accounting block subtracts its Dolphin-PPCTables-mirrored cycle cost from
// ctx->downcount; the burst loop flushes that into ppc_state.downcount after
// every dispatch, so CoreTiming/MMIO see time at block granularity — the
// same granularity as Dolphin's JITs. (History: a flat 64 cycles/dispatch
// warped guest time to 8 internal FPS at healthy gauges; the flat 6 that
// replaced it read 100% in-match but left heavy cutscenes at ~80%.)

/*
static bool VerboseCounters()
{
  static const bool enabled = std::getenv("STATICRECOMP_VERBOSE") != nullptr;
  return enabled;
}
*/

constexpr u32 LOCKED_CACHE_BASE = 0xE0000000u;



// Exception bits the chassis must deliver at a native-burst boundary (raised
// synchronously by MMU/hooks). External-interrupt-class bits are delivered by
// CheckExternalExceptions at slice start (CoreTiming::Advance), exactly as for
// the stock interpreter, so they must NOT abort a native burst: with MSR.EE=0
// they can stay pending for a long time and CheckExceptions cannot clear them.
constexpr u32 SYNC_EXCEPTION_MASK = ~static_cast<u32>(
    EXCEPTION_EXTERNAL_INT | EXCEPTION_DECREMENTER | EXCEPTION_PERFORMANCE_MONITOR);

#ifdef __APPLE__
constexpr const char* MODULE_SUFFIX = ".dylib";
#elif defined(_WIN32)
constexpr const char* MODULE_SUFFIX = ".dll";
#else
constexpr const char* MODULE_SUFFIX = ".so";
#endif
}  // namespace



namespace
{
// Is this external access actually a hardware (write-gather pipe / MMIO) access,
// as opposed to memory (RAM / locked cache) the interpreter's shadow sinks never
// capture? Classify by the POST-TRANSLATION physical address — exactly what
// MMU::WriteToHardware / ReadFromHardware see — because the guest maps hardware,
// MEM1 and locked cache behind arbitrary EFFECTIVE addresses via BAT/page tables.
// In particular Strikers demand-pages a virtual-memory region [0x7E000000,
// 0x80000000) (NL nlMemory VMAlloc) into MEM1, so a naive effective-address test
// misreads those RAM accesses as MMIO: writes report N>0 / I=0 for every
// VM-touching block, and reads desync the native-read replay index (the shadow
// serves VM/LC reads live from the restored pre-image, only replaying true
// hardware reads). An access that does not translate is treated as physical
// (untranslated accesses are already physical). Only called on the lockstep
// journaling path (deduped, first dispatch per block) — not hot.


// Is `end_pc` a LOOP HEADER — the target of a backward direct branch? The lockstep
// shadow steps the interpreter for native's charged cycles and stops at end_pc
// (native's post-dispatch PC). Native ends a dispatch only at a control transfer:
// the emitter turns BACKWARD branches into dispatcher round-trips (`ctx->pc = T;
// return;`) while forward branches stay local `goto`. So when native returns at a
// loop back-edge, end_pc is the loop header, and native ran exactly one body
// iteration. The shadow single-steps from the block entry and reaches that same
// loop header FIRST by sequential fall-through (entering the body), only later via
// the back-edge. The cycle-budget stop must NOT accept that first fall-through
// arrival, or the shadow stops one iteration early (registers/ctr/pointers off by
// one). It is aggravated by the D3 undercharge: when native dispatches into the
// middle of an accounting block, its charge omits the prologue, so the shadow's
// budget is already exhausted at the loop header before the body runs. A straight-
// line segment end (native returns by a bounded fall-through) is NOT a loop header,
// so its sequential arrival is the real boundary and is accepted. Detect a header
// by scanning forward for a direct branch (b / bc, AA=0) back to end_pc; bound the
// scan at the first unconditional branch that leaves the region (loop-body end) or
// a fixed window. Only used on the deduped journaling path — not hot.
// See dolphin-chassis.md §Arc-4 loop-align.

}  // namespace

StaticRecompCore* g_static_recomp_core = nullptr;

bool StaticRecompCore::IsModuleActive() const
{
  return m_module_active;
}

StaticRecompCore::StaticRecompCore(Core::System& system) : JitBase(system)
{
}

StaticRecompCore::~StaticRecompCore() = default;

void StaticRecompCore::Init()
{
  g_static_recomp_core = this;
  RefreshConfig();
  jo.enableBlocklink = false;
  jo.fastmem = false;
  jo.fastmem_arena = false;

  m_block_cache.Init();

  m_guest = CPUState{};
  m_guest.external_read = HookExternalRead;
  m_guest.external_write = HookExternalWrite;
  m_guest.external_read32 = HookExternalRead32;
  m_guest.external_write32 = HookExternalWrite32;
  m_guest.external_pointer = HookExternalPointer;
  m_guest.instruction_fallback = HookInstructionFallback;
  // No host_call: the chassis performs no HLE (Dolphin's hardware model runs
  // every guest function), so the generated per-dispatch ppc_host_call must not
  // pay an indirect cross-dylib call to a hook that always returns false. Left
  // null, ppc_host_call short-circuits (null-guarded) — worth ~2.7% of the CPU
  // thread that the profile charged to that dead call.
  m_guest.host_call = nullptr;
  m_guest.external_user_data = this;
  // ram/ram_size are bound at Run() entry; guest memory may not be mapped yet.

  // Build stamp: every log self-identifies its binary (stale-binary trap).
  std::fprintf(stderr, "[staticrecomp] core init (chassis built " __DATE__ " " __TIME__ ")\n");

  LoadModule();
  m_lockstep_verifier = std::make_unique<StaticRecompLockstep::StaticRecompLockstepVerifier>(*this);
  m_lockstep_verifier->Init();

#ifdef _M_ARM_64
  m_fallback_jit = std::make_unique<JitArm64>(m_system);
#elif defined(_M_X86_64)
  m_fallback_jit = std::make_unique<Jit64>(m_system);
#endif
  if (m_fallback_jit)
    m_fallback_jit->Init();
}

void StaticRecompCore::Shutdown()
{
  g_static_recomp_core = nullptr;
  std::fprintf(stderr,
               "[staticrecomp] shutdown: native=%llu fallback=%llu native_exc=%llu hook_fb=%llu "
               "smc_failed=%u verifications=%llu reverify_events=%llu\n",
               (unsigned long long)m_native_dispatches, (unsigned long long)m_fallback_steps,
               (unsigned long long)m_native_exceptions,
               (unsigned long long)m_hook_fallback_instructions, m_failed_chunks,
               (unsigned long long)m_verifications, (unsigned long long)m_reverify_events);
  NOTICE_LOG_FMT(POWERPC,
                 "StaticRecomp: shutdown. native_dispatches={} fallback_steps={} "
                 "native_exceptions={} hook_fallback_instructions={} smc_failed_chunks={} "
                 "verifications={} reverify_events={}",
                 m_native_dispatches, m_fallback_steps, m_native_exceptions,
                 m_hook_fallback_instructions, m_failed_chunks, m_verifications,
                 m_reverify_events);
  m_lockstep_verifier.reset();
  m_block_cache.Shutdown();
  m_module = nullptr;
  if (m_library.IsOpen())
    m_library.Close();

  if (m_fallback_jit)
  {
    m_fallback_jit->Shutdown();
    m_fallback_jit.reset();
  }
}

void StaticRecompCore::LoadModule()
{
  // Product kill-switch: with the module disabled, the StaticRecomp core runs
  // interpreter-only — the PRIME INVARIANT path (behaves exactly like stock
  // Dolphin) on demand, without moving the module file. Command line:
  // -C Dolphin.Core.StaticRecompModule=False.
  if (!Config::Get(Config::MAIN_STATICRECOMP_MODULE))
  {
    std::fprintf(stderr,
                 "[staticrecomp] native module disabled by config "
                 "(Main.Core.StaticRecompModule=false); interpreter-only (invariant path).\n");
    NOTICE_LOG_FMT(POWERPC, "StaticRecomp: module disabled by config; interpreter-only.");
    return;
  }

  const std::string game_id = SConfig::GetInstance().GetGameID();
  std::string path;
  if (const char* env = std::getenv("STATICRECOMP_MODULE"))
  {
    path = env;
  }
  else if (!game_id.empty())
  {
    path = File::GetUserPath(D_USER_IDX) + "StaticRecompModules/g" + game_id + "_recomp" +
           MODULE_SUFFIX;
  }

  if (path.empty() || !File::Exists(path))
  {
    NOTICE_LOG_FMT(POWERPC, "StaticRecomp: no module for game '{}' (looked at '{}'); "
                            "running interpreter-only.",
                   game_id, path);
    return;
  }

  if (!m_library.Open(path.c_str()))
  {
    ERROR_LOG_FMT(POWERPC, "StaticRecomp: failed to dlopen module '{}'; interpreter-only.", path);
    return;
  }

  const auto get_module = reinterpret_cast<StaticRecompGetModuleFn>(
      m_library.GetSymbolAddress(STATICRECOMP_GET_MODULE_SYMBOL));
  const StaticRecompModuleDesc* desc = get_module ? get_module() : nullptr;

  const auto reject = [&](const std::string& why) {
    ERROR_LOG_FMT(POWERPC, "StaticRecomp: rejecting module '{}': {}. Interpreter-only.", path, why);
    m_module = nullptr;
    m_library.Close();
  };

  if (!desc)
    return reject("missing or null " STATICRECOMP_GET_MODULE_SYMBOL);
  if (desc->abi_version != STATICRECOMP_ABI_VERSION)
    return reject(fmt::format("abi_version {} != {}", desc->abi_version, STATICRECOMP_ABI_VERSION));
  if (desc->cpu_abi_version != GXRUNTIME_CPU_ABI_VERSION)
    return reject(fmt::format("cpu_abi_version {} != {}", desc->cpu_abi_version,
                              GXRUNTIME_CPU_ABI_VERSION));
  if (desc->cpu_state_size != sizeof(CPUState))
    return reject(fmt::format("cpu_state_size {} != sizeof(CPUState) {}", desc->cpu_state_size,
                              sizeof(CPUState)));
  if (!desc->dispatch || !desc->code_ranges || desc->num_code_ranges == 0)
    return reject("no dispatch entry or empty code ranges");
  if (!desc->chunk_ranges || desc->num_chunk_ranges == 0 || !desc->chunk_hashes)
    return reject("no chunk ranges/hashes (required for the SMC guard)");
  if (!std::getenv("STATICRECOMP_MODULE") && !game_id.empty() && game_id != desc->game_id)
    return reject(fmt::format("module game_id '{}' != running game '{}'", desc->game_id, game_id));

  m_module = desc;
  m_module_active = (desc != nullptr);
  m_chunk_state.assign(desc->num_chunk_ranges, CHUNK_UNVERIFIED);
  m_failed_chunks = 0;
  m_lookup_ram_size = 0;
  m_lookup_exram_size = 0;
  m_chunk_lookup_table.clear();

  std::fprintf(stderr,
               "[staticrecomp] module loaded: %s entry=0x%08X (chassis built " __DATE__
               " " __TIME__ ")\n",
               path.c_str(), desc->entry_point);
  NOTICE_LOG_FMT(POWERPC,
                 "StaticRecomp: loaded module '{}' (game_id={} entry=0x{:08X} "
                 "code_ranges={} smc_ranges={})",
                 path, desc->game_id, desc->entry_point, desc->num_code_ranges,
                 desc->num_smc_ranges);
}

void StaticRecompCore::ClearCache()
{
  if (m_fallback_jit)
    m_fallback_jit->ClearCache();

  if (!m_module)
    return;
  std::fill(m_chunk_state.begin(), m_chunk_state.end(), u8{CHUNK_UNVERIFIED});
  m_failed_chunks = 0;
  ++m_reverify_events;
}

int StaticRecompCore::GetAddressLookupIndex(u32 address) const
{
  if (address >= 0x80000000u && address < 0x80000000u + m_lookup_ram_size)
    return static_cast<int>((address - 0x80000000u) >> 2);
  if (address >= 0x90000000u && address < 0x90000000u + m_lookup_exram_size)
    return static_cast<int>((m_lookup_ram_size >> 2) + ((address - 0x90000000u) >> 2));
  return -1;
}

void StaticRecompCore::InitLookupTable(u32 ram_size, u32 exram_size)
{
  if (m_lookup_ram_size == ram_size && m_lookup_exram_size == exram_size)
    return;

  m_lookup_ram_size = ram_size;
  m_lookup_exram_size = exram_size;

  if (!m_module)
  {
    m_chunk_lookup_table.clear();
    return;
  }

  u32 total_instructions = (ram_size + exram_size) >> 2;
  m_chunk_lookup_table.assign(total_instructions, -1);

  for (u32 i = 0; i < m_module->num_chunk_ranges; ++i)
  {
    const auto& chunk = m_module->chunk_ranges[i];
    int start_idx = GetAddressLookupIndex(chunk.start);
    int end_idx = GetAddressLookupIndex(chunk.end);

    if (start_idx >= 0 && end_idx >= start_idx)
    {
      for (int idx = start_idx; idx < end_idx; ++idx)
      {
        m_chunk_lookup_table[idx] = static_cast<int>(i);
      }
    }
  }
}

int StaticRecompCore::ChunkIndexOf(u32 address) const
{
  if (!m_module_active || m_chunk_lookup_table.empty())
    return -1;

  int idx = GetAddressLookupIndex(address);
  if (idx < 0 || idx >= static_cast<int>(m_chunk_lookup_table.size()))
    return -1;

  return m_chunk_lookup_table[idx];
}

bool StaticRecompCore::FastDispatchableAt(u32 address) const
{
  const int index = ChunkIndexOf(address);
  return index >= 0 && m_chunk_state[index] == CHUNK_VERIFIED;
}

bool StaticRecompCore::DispatchableAt(u32 address)
{
  const int index = ChunkIndexOf(address);
  if (index < 0)
    return false;
  if (m_chunk_state[index] == CHUNK_UNVERIFIED)
    VerifyChunk(static_cast<u32>(index));
  return m_chunk_state[index] == CHUNK_VERIFIED;
}

void StaticRecompCore::VerifyChunk(u32 index)
{
  const auto& chunk = m_module->chunk_ranges[index];
  auto& memory = m_system.GetMemory();
  const u32 ram_size = memory.GetRamSizeReal();
  const u32 offset = chunk.start - 0x80000000u;
  const u32 length = chunk.end - chunk.start;
  ++m_verifications;

  if (chunk.start < 0x80000000u || offset >= ram_size || length > ram_size - offset)
  {
    m_chunk_state[index] = CHUNK_FAILED;
    ++m_failed_chunks;
    ERROR_LOG_FMT(POWERPC, "StaticRecomp: chunk [0x{:08X},0x{:08X}) outside guest RAM",
                  chunk.start, chunk.end);
    return;
  }

  // FNV-1a 64, matching gen_module_tables.py.
  const u8* bytes = memory.GetRAM() + offset;
  u64 hash = 0xCBF29CE484222325ull;
  for (u32 i = 0; i < length; ++i)
  {
    hash ^= bytes[i];
    hash *= 0x100000001B3ull;
  }

  if (hash == m_module->chunk_hashes[index])
  {
    m_chunk_state[index] = CHUNK_VERIFIED;
  }
  else
  {
    m_chunk_state[index] = CHUNK_FAILED;
    ++m_failed_chunks;
    std::fprintf(stderr,
                 "[staticrecomp] SMC: chunk [0x%08X,0x%08X) hash mismatch; interpreter until "
                 "next invalidation (%u failed)\n",
                 chunk.start, chunk.end, m_failed_chunks);
    WARN_LOG_FMT(POWERPC,
                 "StaticRecomp: chunk [0x{:08X},0x{:08X}) failed verification (guest code "
                 "differs from module); interpreter until next invalidation",
                 chunk.start, chunk.end);
  }
}

void StaticRecompCore::OnICacheInvalidate(u32 address, u32 length)
{
  if (m_fallback_jit)
  {
    m_fallback_jit->GetBlockCache()->InvalidateICache(address, length, false);
  }

  if (!m_module_active || length == 0)
    return;
  const u32 last = address + (length - 1u);

  // Binary search to find the first chunk index that could possibly overlap (chunk.end > address)
  u32 lo = 0;
  u32 hi = m_module->num_chunk_ranges;
  while (lo < hi)
  {
    const u32 mid = lo + (hi - lo) / 2;
    if (m_module->chunk_ranges[mid].end <= address)
      lo = mid + 1;
    else
      hi = mid;
  }

  for (u32 i = lo; i < m_module->num_chunk_ranges; ++i)
  {
    const auto& chunk = m_module->chunk_ranges[i];
    if (chunk.start > last)
      break;

    if (m_chunk_state[i] != CHUNK_UNVERIFIED)
    {
      if (m_chunk_state[i] == CHUNK_FAILED)
        --m_failed_chunks;
      m_chunk_state[i] = CHUNK_UNVERIFIED;
      ++m_reverify_events;
    }
  }
}

void StaticRecompCore::SyncIn()
{
  auto& power_pc = m_system.GetPowerPC();
  auto& ppc = power_pc.GetPPCState();

  std::memcpy(m_guest.gpr, ppc.gpr, sizeof(m_guest.gpr));
  for (int i = 0; i < 32; ++i)
  {
    std::memcpy(&m_guest.fpr[i], &ppc.ps[i].ps0, sizeof(u64));
    std::memcpy(&m_guest.ps1[i], &ppc.ps[i].ps1, sizeof(u64));
  }
  m_guest.pc = ppc.pc;
  m_guest.lr = ppc.spr[SPR_LR];
  m_guest.ctr = ppc.spr[SPR_CTR];
  m_guest.cr = ppc.cr.Get();
  m_guest.xer = ppc.GetXER().Hex;
  m_guest.fpscr = ppc.fpscr.Hex;
  m_guest.msr = ppc.msr.Hex;
  m_guest.srr0 = ppc.spr[SPR_SRR0];
  m_guest.srr1 = ppc.spr[SPR_SRR1];
  m_guest.dar = ppc.spr[SPR_DAR];
  m_guest.dsisr = ppc.spr[SPR_DSISR];
  m_guest.ear = ppc.spr[SPR_EAR];
  m_guest.hid2 = ppc.spr[SPR_HID2];
  for (int i = 0; i < 16; ++i)
    m_guest.sr[i] = ppc.sr[i];
  for (int i = 0; i < 8; ++i)
    m_guest.gqr[i] = ppc.spr[SPR_GQR0 + i];
  // Dolphin materializes TB lazily on read (spr[TL/TU] is a stale cache);
  // GetFakeTimeBase() is the live value, matching the interpreter's mftb.
  m_guest.timebase = m_system.GetSystemTimers().GetFakeTimeBase();
  m_guest.reserve_addr = ppc.reserve_address;
  m_guest.reserve_valid = ppc.reserve;
  m_guest.exception = 0;
  m_guest.program_exception = 0;
  m_guest.downcount = 0;  // charge accumulator, not a copy of ppc.downcount

  if (m_module && m_module->on_state_loaded)
    m_module->on_state_loaded(&m_guest);
}

void StaticRecompCore::SyncOut()
{
  auto& power_pc = m_system.GetPowerPC();
  auto& ppc = power_pc.GetPPCState();

  // Flush any cycle charge the module accumulated since the last flush
  // (matters on the mid-dispatch fallback path, where the interpreter step
  // that follows must see up-to-date time).
  ppc.downcount += static_cast<int>(m_guest.downcount);
  m_guest.downcount = 0;

  std::memcpy(ppc.gpr, m_guest.gpr, sizeof(m_guest.gpr));
  for (int i = 0; i < 32; ++i)
  {
    std::memcpy(&ppc.ps[i].ps0, &m_guest.fpr[i], sizeof(u64));
    std::memcpy(&ppc.ps[i].ps1, &m_guest.ps1[i], sizeof(u64));
  }
  ppc.pc = m_guest.pc;
  ppc.npc = m_guest.pc;
  ppc.spr[SPR_LR] = m_guest.lr;
  ppc.spr[SPR_CTR] = m_guest.ctr;
  ppc.cr.Set(m_guest.cr);
  ppc.SetXER(UReg_XER{m_guest.xer});
  ppc.fpscr.Hex = m_guest.fpscr;
  ppc.spr[SPR_SRR0] = m_guest.srr0;
  ppc.spr[SPR_SRR1] = m_guest.srr1;
  ppc.spr[SPR_DAR] = m_guest.dar;
  ppc.spr[SPR_DSISR] = m_guest.dsisr;
  ppc.spr[SPR_EAR] = m_guest.ear;
  ppc.spr[SPR_HID2] = m_guest.hid2;
  for (int i = 0; i < 16; ++i)
    ppc.sr[i] = m_guest.sr[i];
  for (int i = 0; i < 8; ++i)
    ppc.spr[SPR_GQR0 + i] = m_guest.gqr[i];
  ppc.reserve_address = m_guest.reserve_addr;
  ppc.reserve = m_guest.reserve_valid;
  // Timebase is owned by Dolphin's CoreTiming; native code cannot write it.

  if (ppc.msr.Hex != m_guest.msr)
  {
    ppc.msr.Hex = m_guest.msr;
    power_pc.MSRUpdated();
  }
  PowerPC::RoundingModeUpdated(ppc);
}

void StaticRecompCore::PropagateGuestMSR()
{
  auto& power_pc = m_system.GetPowerPC();
  auto& ppc = power_pc.GetPPCState();
  if (ppc.msr.Hex != m_guest.msr)
  {
    ppc.msr.Hex = m_guest.msr;
    power_pc.MSRUpdated();
  }
}

u64 StaticRecompCore::HookExternalRead(CPUState* cpu, u32 ea, u8 size)
{
  auto* core = static_cast<StaticRecompCore*>(cpu->external_user_data);
  core->PropagateGuestMSR();
  auto& mmu = core->m_system.GetMMU();
  u64 value;
  switch (size)
  {
  case 1:
    value = mmu.Read<u8>(ea);
    break;
  case 2:
    value = mmu.Read<u16>(ea);
    break;
  case 4:
    value = mmu.Read<u32>(ea);
    break;
  case 8:
    value = mmu.Read<u64>(ea);
    break;
  default:
    ERROR_LOG_FMT(POWERPC, "StaticRecomp: external read of bad size {} at 0x{:08X}", size, ea);
    return 0;
  }
  if (core->m_lockstep_verifier->m_ls_journaling &&
      StaticRecompLockstep::LsHwAccessInScope(mmu, ea))
  {
    core->m_lockstep_verifier->m_ls_native_reads.push_back({ea, static_cast<u32>(value), size});
  }
  return value;
}

void StaticRecompCore::HookExternalWrite(CPUState* cpu, u32 ea, u64 value, u8 size)
{
  auto* core = static_cast<StaticRecompCore*>(cpu->external_user_data);

  // Gather-pipe fast path: stores to the write-gather pipe page at effective
  // 0xCC008000 go straight to GPFifo, mirroring the MMU's masked-write
  // special case without an MMU round trip. Keying on the effective page is
  // the same shortcut Dolphin's JITs take (optimizeGatherPipe). GPFifo
  // maintains ppc_state.gather_pipe_ptr internally.
  if ((ea & 0xFFFFF000) == 0xCC008000u)
  {
    if (core->m_lockstep_verifier->m_ls_journaling)
      core->m_lockstep_verifier->m_ls_native_mmio.push_back({ea, static_cast<u32>(value), size});
    auto& gpfifo = core->m_system.GetGPFifo();
    switch (size)
    {
    case 1:
      gpfifo.Write8(static_cast<u8>(value));
      return;
    case 2:
      gpfifo.Write16(static_cast<u16>(value));
      return;
    case 4:
      gpfifo.Write32(static_cast<u32>(value));
      return;
    default:
      for (u32 i = size * 8u; i > 0;)
      {
        i -= 8;
        gpfifo.Write8(static_cast<u8>(value >> i));
      }
      return;
    }
  }

  core->PropagateGuestMSR();
  auto& mmu = core->m_system.GetMMU();
  if (core->m_lockstep_verifier->m_ls_journaling &&
      StaticRecompLockstep::LsHwAccessInScope(mmu, ea))
  {
    core->m_lockstep_verifier->m_ls_native_mmio.push_back({ea, static_cast<u32>(value), size});
  }
  switch (size)
  {
  case 1:
    mmu.Write<u8>(static_cast<u8>(value), ea);
    break;
  case 2:
    mmu.Write<u16>(static_cast<u16>(value), ea);
    break;
  case 4:
    mmu.Write<u32>(static_cast<u32>(value), ea);
    break;
  case 8:
    mmu.Write<u64>(value, ea);
    break;
  default:
    ERROR_LOG_FMT(POWERPC, "StaticRecomp: external write of bad size {} at 0x{:08X}", size, ea);
    break;
  }
}

u32 StaticRecompCore::HookExternalRead32(CPUState* cpu, u32 ea, u8 rid)
{
  // eciwx external-control read. EAR-enable and alignment were checked by the
  // generated helper; Dolphin's interpreter services the access as a plain
  // MMU read (the rid is carried in EAR only).
  auto* core = static_cast<StaticRecompCore*>(cpu->external_user_data);
  core->PropagateGuestMSR();
  auto& mmu = core->m_system.GetMMU();
  const u32 value = mmu.Read<u32>(ea);
  if (core->m_lockstep_verifier->m_ls_journaling &&
      StaticRecompLockstep::LsHwAccessInScope(mmu, ea))
  {
    core->m_lockstep_verifier->m_ls_native_reads.push_back({ea, value, 4});
  }
  return value;
}

void StaticRecompCore::HookExternalWrite32(CPUState* cpu, u32 ea, u32 value, u8 rid)
{
  // ecowx external-control write; see HookExternalRead32.
  auto* core = static_cast<StaticRecompCore*>(cpu->external_user_data);
  core->PropagateGuestMSR();
  auto& mmu = core->m_system.GetMMU();
  if (core->m_lockstep_verifier->m_ls_journaling &&
      StaticRecompLockstep::LsHwAccessInScope(mmu, ea))
  {
    core->m_lockstep_verifier->m_ls_native_mmio.push_back({ea, value, 4});
  }
  mmu.Write<u32>(value, ea);
}

void* StaticRecompCore::HookExternalPointer(CPUState* cpu, u32 ea, u32 size)
{
  auto* core = static_cast<StaticRecompCore*>(cpu->external_user_data);
  auto& memory = core->m_system.GetMemory();
  if (ea >= LOCKED_CACHE_BASE && size != 0 &&
      (ea - LOCKED_CACHE_BASE) + size <= memory.GetL1CacheSize())
  {
    return memory.GetL1Cache() + (ea - LOCKED_CACHE_BASE);
  }
  // Everything else stays on the per-access MMU hooks: this hook receives
  // *effective* addresses, and whether one maps to RAM depends on live
  // MSR/BAT state that only the MMU can answer. Handing out a raw pointer
  // here would bypass MMIO and translation. (Memory::GetPointerForRange was
  // considered and rejected for exactly that reason.)
  return nullptr;
}

void StaticRecompCore::HookInstructionFallback(CPUState* cpu, u32 raw, u32 cia)
{
  auto* core = static_cast<StaticRecompCore*>(cpu->external_user_data);
  ++core->m_hook_fallback_instructions;

  // Lockstep: a block that fell back to the interpreter for an unmodeled
  // instruction (DMA mtspr, cache op, ...) performed side effects not captured
  // by the RAM journal / MMIO hooks, so re-running it on the shadow would
  // double-issue them. Mark it unsafe to differentially check.
  if (core->m_lockstep_verifier->m_ls_journaling)
    core->m_lockstep_verifier->m_ls_fallback_seen = true;

  auto& system = core->m_system;
  auto& ppc = system.GetPPCState();

  // Fast path for dcbf/dcbst/dcbi/icbi: streaming code flushes caches in
  // 32-byte loops (thousands per frame), and these ops read two GPRs and
  // change no CPU state, so they run straight off ctx without the full
  // SyncOut/interpreter/SyncIn round trip. This mirrors Dolphin's
  // interpreter with dcache emulation off: every one funnels into
  // InvalidateICacheLine (keeping the SMC guard exact). dcbi's PR!=0
  // privilege trap and dcache-on configs take the slow path.
  if ((raw >> 26) == 31u && !ppc.m_enable_dcache)
  {
    const u32 xo = (raw >> 1) & 0x3FFu;
    if (xo == 86u || xo == 54u || xo == 982u || (xo == 470u && (cpu->msr & 0x4000u) == 0))
    {
      const u32 ra = (raw >> 16) & 31u;
      const u32 rb = (raw >> 11) & 31u;
      const u32 ea = (ra ? cpu->gpr[ra] : 0u) + cpu->gpr[rb];
      if (xo == 982u)
      {
        system.GetJitInterface().InvalidateICacheLine(ea);
      }
      // These bypass SingleStepInner, so charge Dolphin's PPCTables cost
      // here (icbi 4, dcbf/dcbst/dcbi 5); their emitted block cost is zero.
      ppc.downcount -= (xo == 982u) ? 4 : 5;
      cpu->pc = cia + 4u;
      return;
    }
  }

  // The recompiled segment resumes via the dispatcher at the PC this leaves
  // behind, so this must execute exactly the instruction at cia via
  // Dolphin's interpreter and hand the register state back.
  core->SyncOut();
  ppc.pc = cia;
  ppc.npc = cia + 4;
  ppc.downcount -= system.GetInterpreter().SingleStepInner();
  core->SyncIn();
}





void StaticRecompCore::Run()
{
  auto& core_timing = m_system.GetCoreTiming();
  auto& power_pc = m_system.GetPowerPC();
  auto& ppc = power_pc.GetPPCState();
  auto& interpreter = m_system.GetInterpreter();
  auto& memory = m_system.GetMemory();
  const CPU::State* state_ptr = m_system.GetCPU().GetStatePtr();

  m_guest.ram = memory.GetRAM();
  m_guest.ram_size = memory.GetRamSizeReal();
  m_guest.exram = memory.GetEXRAM();
  m_guest.exram_size = memory.GetExRamSizeReal();
  InitLookupTable(m_guest.ram_size, m_guest.exram_size);

  const std::string initial_game_id = SConfig::GetInstance().GetGameID();
  m_module_active = m_module && (initial_game_id.empty() || initial_game_id == m_module->game_id);

  if (!m_module_active && m_fallback_jit)
  {
    m_fallback_jit->Run();
    return;
  }

  while (*state_ptr == CPU::State::Running)
  {
    core_timing.Advance();
    const std::string current_game_id = SConfig::GetInstance().GetGameID();
    m_module_active = m_module && (current_game_id.empty() || current_game_id == m_module->game_id);

    do
    {
      // MSR.FP needs no gate here: generated FPU instructions raise the
      // FP-unavailable exception themselves (ppc_fp_available).
      if (m_module_active && DispatchableAt(ppc.pc))
      {
        SyncIn();
        ++m_bursts;
        do
        {
          const bool do_ls = m_lockstep_verifier->ShouldCheck(m_guest.pc);
          if (do_ls)
          {
            m_lockstep_verifier->Prepare(m_guest);
          }

          m_module->dispatch(&m_guest, m_guest.pc);
          ++m_native_dispatches;

          if (do_ls)
          {
            m_lockstep_verifier->Verify(m_guest);
          }
          /*
          if (VerboseCounters() && (m_native_dispatches & 0x3FFFFFu) == 1u)
          {
            static const auto start = std::chrono::steady_clock::now();
            const double wall =
                std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
            std::fprintf(stderr,
                         "[staticrecomp] native=%llu fallback=%llu smc_failed=%u pc=0x%08X "
                         "tb=%llu wall=%.1fs bursts=%llu charged=%llu\n",
                         (unsigned long long)m_native_dispatches,
                         (unsigned long long)m_fallback_steps, m_failed_chunks, m_guest.pc,
                         (unsigned long long)m_system.GetSystemTimers().GetFakeTimeBase(), wall,
                         (unsigned long long)m_bursts, (unsigned long long)m_charged_cycles);
          }
          */
          // Flush the module's per-block cycle charges into Dolphin's
          // downcount. A dispatch that charged nothing (PC-switch default,
          // pure embedded data) still costs 1 so the burst always makes
          // downcount progress; this per-dispatch flush is also the
          // dispatcher back-edge timing check — CoreTiming regains control
          // with at least CachedInterpreter's per-block frequency, so
          // external-interrupt latency matches stock.
          const s64 charge = -m_guest.downcount;
          m_guest.downcount = 0;
          ppc.downcount -= static_cast<int>(charge > 0 ? charge : 1);
          m_charged_cycles += static_cast<u64>(charge > 0 ? charge : 1);
          m_guest.timebase += static_cast<u64>(charge > 0 ? charge : 1);

          // Idle loop skipping for the Wii Menu's OSIdleThread
          if (m_guest.pc == 0x8150D1D0)
          {
            m_system.GetCoreTiming().Idle();
          }

          // ctx->timebase is refreshed at burst start (SyncIn), and here we
          // incrementally advance it by the exact block cycle charges to
          // prevent guest busy-wait loops from spinning on a stale timebase.
          if (m_guest.exception)
          {
            // DolRecomp's runtime already redirected pc/msr/srr to the guest
            // exception vector; the flag only signals that it happened.
            m_guest.exception = 0;
            m_guest.program_exception = 0;
            ++m_native_exceptions;
          }
          if ((ppc.Exceptions & SYNC_EXCEPTION_MASK) != 0)
            break;  // Hook-raised synchronous exception: deliver via Dolphin below.
        } while (m_module_active && FastDispatchableAt(m_guest.pc) && ppc.downcount > 0 &&
                 *state_ptr == CPU::State::Running);
        SyncOut();
        if ((ppc.Exceptions & SYNC_EXCEPTION_MASK) != 0)
          power_pc.CheckExceptions();
      }
      else
      {
        // SingleStepInner delivers synchronous exceptions itself; external
        // interrupts are delivered at slice start, as in Interpreter::Run.
        if (m_fallback_jit)
        {
          m_fallback_jit->Run();
        }
        else
        {
          do
          {
            ppc.downcount -= interpreter.SingleStepInner();
            ++m_fallback_steps;
          } while (!(m_module_active && DispatchableAt(ppc.pc)) && ppc.downcount > 0 &&
                   *state_ptr == CPU::State::Running);
        }
      }
    } while (ppc.downcount > 0 && *state_ptr == CPU::State::Running);
  }
}

void StaticRecompCore::SingleStep()
{
  // Debugger stepping runs through the interpreter; state outside Run() lives
  // in PowerPCState, so no sync is needed.
  auto& system = m_system;
  system.GetCoreTiming().Advance();
  system.GetPPCState().downcount -= system.GetInterpreter().SingleStepInner();
}
