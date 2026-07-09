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

// Extra interpreter cycles the lockstep shadow may run past native's charged
// budget to reach native's true block boundary when native undercharged on a
// mid-accounting-block dispatch entry (deficit is bounded by one accounting
// block's cost; max emitted block charge is well under this).
constexpr s64 LS_UNDERCHARGE_GRACE = 256;

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

namespace StaticRecompLockstep
{
// Definitions for the MMU write-suppression hook (declared in the shared
// header). Null except while an interpreter shadow re-run is in progress.
HwWriteSink g_hw_write_sink = nullptr;
void* g_hw_write_sink_user = nullptr;
HwReadSink g_hw_read_sink = nullptr;
void* g_hw_read_sink_user = nullptr;
bool g_tb_override_active = false;
u64 g_tb_override_value = 0;
RamWriteJournal g_ram_write_journal = nullptr;
void* g_ram_write_journal_user = nullptr;
LcWriteJournal g_lc_write_journal = nullptr;
void* g_lc_write_journal_user = nullptr;
VmemWriteJournal g_vmem_write_journal = nullptr;
void* g_vmem_write_journal_user = nullptr;
}  // namespace StaticRecompLockstep

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
bool LsHwAccessInScope(PowerPC::MMU& mmu, u32 ea)
{
  u32 phys = ea;
  if (const std::optional<u32> t = mmu.GetTranslatedAddress(ea))
    phys = *t;
  const bool is_gather = (phys & 0xFFFFF000u) == GPFifo::GATHER_PIPE_PHYSICAL_ADDRESS;
  const bool is_mmio = (phys & 0xF8000000u) == 0x08000000u;
  return is_gather || is_mmio;
}

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
bool LsIsLoopHeader(const u8* ram, u32 ram_size, u32 end_pc)
{
  constexpr u32 kBase = 0x80000000u;
  if (end_pc < kBase)
    return false;
  const u32 start_off = end_pc - kBase;
  // Scan forward for a direct branch back to end_pc. A backward branch (source >
  // end_pc) whose target IS end_pc is by definition end_pc's own loop back-edge,
  // so no early break is needed (and breaking at an interior unconditional branch
  // in the loop body — a common if/else merge — would miss back-edges far past
  // end_pc, e.g. CalcDesiredTarget's is 116 insns down). If native's end_pc is a
  // loop header, native reached it via that back-edge (a fall-through into a
  // leader does not return; bl/indirect arrivals are handled at the boundary
  // check), so classifying it here is always correct. The window only bounds the
  // scan for NON-loop-header addresses (which read to the cap and return false).
  constexpr u32 kWindowInsns = 2048;
  for (u32 i = 0; i < kWindowInsns; ++i)
  {
    const u32 off = start_off + i * 4u;
    if (off + 4u > ram_size)
      break;
    const u32 insn = Common::swap32(&ram[off]);
    const u32 opcd = insn >> 26;
    const u32 addr = end_pc + i * 4u;
    if (addr <= end_pc)
      continue;
    if (opcd == 16u && (insn & 0x2u) == 0u)  // bc: B-form, 14-bit signed BD, AA=0
    {
      const s32 bd = static_cast<s32>(static_cast<s16>(insn & 0xFFFCu));
      if (addr + static_cast<u32>(bd) == end_pc)
        return true;
    }
    else if (opcd == 18u && (insn & 0x2u) == 0u)  // b: I-form, 24-bit signed LI, AA=0
    {
      s32 li = static_cast<s32>(insn & 0x03FFFFFCu);
      if (li & 0x02000000)
        li |= static_cast<s32>(0xFC000000u);
      if (addr + static_cast<u32>(li) == end_pc)
        return true;
    }
  }
  return false;
}
}  // namespace

StaticRecompCore::StaticRecompCore(Core::System& system) : JitBase(system)
{
}

StaticRecompCore::~StaticRecompCore() = default;

void StaticRecompCore::Init()
{
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
  InitLockstep();
}

void StaticRecompCore::Shutdown()
{
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
  if (m_lockstep)
  {
    std::fprintf(stderr,
                 "[lockstep] summary: checks=%llu reports=%llu skipped_fallback=%llu "
                 "skipped_zero=%llu undercharges=%llu max_deficit=%lld distinct_pcs=%zu\n",
                 (unsigned long long)m_ls_checks, (unsigned long long)m_ls_reports,
                 (unsigned long long)m_ls_skipped_fallback, (unsigned long long)m_ls_skipped_zero,
                 (unsigned long long)m_ls_undercharges, (long long)m_ls_max_undercharge,
                 m_ls_checked.size());
    if (m_set_mem_journal)
      m_set_mem_journal(nullptr, nullptr);
  }
  m_block_cache.Shutdown();
  m_module = nullptr;
  if (m_library.IsOpen())
    m_library.Close();
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
  if (desc->cpu_abi_version != DOLRUNTIME_CPU_ABI_VERSION)
    return reject(fmt::format("cpu_abi_version {} != {}", desc->cpu_abi_version,
                              DOLRUNTIME_CPU_ABI_VERSION));
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
  m_chunk_state.assign(desc->num_chunk_ranges, CHUNK_UNVERIFIED);
  m_failed_chunks = 0;
  // Optional: the module's DolRuntime cpu.c exports a setter for the RAM-write
  // journal that the differential ("lockstep") harness relies on. Absent =>
  // lockstep stays disabled even if requested (warned in InitLockstep).
  m_set_mem_journal = reinterpret_cast<SetMemJournalFn>(
      m_library.GetSymbolAddress("ppc_set_mem_write_journal"));
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
  // JitInterface::DoState calls this on savestate LOAD (and generic cache
  // clears reach it too): the freshly loaded guest RAM has no relationship
  // to what was previously verified, so every chunk re-verifies against the
  // module's DOL-text hash before its next native dispatch. Register state
  // needs nothing here: outside a burst it lives entirely in PowerPCState,
  // and the next SyncIn rebuilds m_guest (and on_state_loaded re-arms host
  // FP rounding from the loaded FPSCR).
  if (!m_module)
    return;
  std::fill(m_chunk_state.begin(), m_chunk_state.end(), u8{CHUNK_UNVERIFIED});
  m_failed_chunks = 0;
  ++m_reverify_events;
}

int StaticRecompCore::ChunkIndexOf(u32 address) const
{
  // Dispatch locality fast path: check the last chunk hit first.
  {
    const auto& last = m_module->chunk_ranges[m_last_chunk_index];
    if (address >= last.start && address < last.end)
      return static_cast<int>(m_last_chunk_index);
  }
  // Binary search over the sorted, non-overlapping chunk table (which tiles
  // the module's code ranges exactly).
  u32 lo = 0;
  u32 hi = m_module->num_chunk_ranges;
  while (lo < hi)
  {
    const u32 mid = lo + (hi - lo) / 2;
    const auto& chunk = m_module->chunk_ranges[mid];
    if (address < chunk.start)
      hi = mid;
    else if (address >= chunk.end)
      lo = mid + 1;
    else
    {
      m_last_chunk_index = mid;
      return static_cast<int>(mid);
    }
  }
  return -1;
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
  if (!m_module || length == 0)
    return;
  const u32 last = address + (length - 1u);
  for (u32 i = 0; i < m_module->num_chunk_ranges; ++i)
  {
    const auto& chunk = m_module->chunk_ranges[i];
    if (address < chunk.end && last >= chunk.start && m_chunk_state[i] != CHUNK_UNVERIFIED)
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
  // Lockstep: record native's hardware read so the shadow can replay it. Only
  // true hardware (gather/MMIO) reads — the shadow replays those from
  // ReadFromHardware; VM/LC reads it serves live from the restored pre-image, so
  // recording them here would desync the replay index and feed a later hardware
  // read the wrong value (root cause of the glSetRasterState / nlListAddStart
  // VM-read divergences). Classify post-translation, matching the write side.
  if (core->m_ls_journaling && LsHwAccessInScope(mmu, ea))
    core->m_ls_native_reads.push_back({ea, static_cast<u32>(value), size});
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
    // Lockstep: the gather pipe is unambiguously hardware — record it (the shadow
    // sink captures the interpreter's matching gather write, reconciled by
    // physical mask in the compare).
    if (core->m_ls_journaling)
      core->m_ls_native_mmio.push_back({ea, static_cast<u32>(value), size});
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
  // Lockstep: record native's hardware (MMIO) writes so the shadow interpreter's
  // suppressed writes can be diffed against them. Classify by post-translation
  // physical (after PropagateGuestMSR so translation uses the guest's MSR): only
  // writes the shadow sink also captures (gather+MMIO) are comparable; MEM1
  // (incl. the demand-paged virtual-memory window) and locked cache are memory,
  // journaled+restored separately, not compared here.
  if (core->m_ls_journaling && LsHwAccessInScope(mmu, ea))
    core->m_ls_native_mmio.push_back({ea, static_cast<u32>(value), size});
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
  if (core->m_ls_journaling && LsHwAccessInScope(mmu, ea))
    core->m_ls_native_reads.push_back({ea, value, 4});
  return value;
}

void StaticRecompCore::HookExternalWrite32(CPUState* cpu, u32 ea, u32 value, u8 rid)
{
  // ecowx external-control write; see HookExternalRead32.
  auto* core = static_cast<StaticRecompCore*>(cpu->external_user_data);
  core->PropagateGuestMSR();
  auto& mmu = core->m_system.GetMMU();
  if (core->m_ls_journaling && LsHwAccessInScope(mmu, ea))
    core->m_ls_native_mmio.push_back({ea, value, 4});
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
  if (core->m_ls_journaling)
    core->m_ls_fallback_seen = true;

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

void StaticRecompCore::InitLockstep()
{
  const char* enable = std::getenv("STATICRECOMP_LOCKSTEP");
  if (!enable || enable[0] == '0' || enable[0] == '\0')
    return;
  if (!m_module)
  {
    ERROR_LOG_FMT(POWERPC, "StaticRecomp: LOCKSTEP requested but no module loaded; ignored.");
    return;
  }
  if (!m_set_mem_journal)
  {
    std::fprintf(stderr, "[lockstep] module lacks ppc_set_mem_write_journal export; "
                         "lockstep DISABLED (rebuild the module).\n");
    return;
  }

  m_lockstep = true;
  if (const char* s = std::getenv("STATICRECOMP_LOCKSTEP_START"))
    m_ls_start = std::strtoull(s, nullptr, 0);
  if (const char* s = std::getenv("STATICRECOMP_LOCKSTEP_LIMIT"))
    m_ls_limit = std::strtoull(s, nullptr, 0);
  if (const char* s = std::getenv("STATICRECOMP_LOCKSTEP_MAXREPORT"))
    m_ls_max_report = std::strtoull(s, nullptr, 0);
  if (const char* s = std::getenv("STATICRECOMP_LOCKSTEP_STEPCAP"))
    m_ls_step_cap = std::atoi(s);
  // Diagnostic: dump the interpreter shadow's per-instruction pc/op/fpscr/cr/gpr
  // (+ entry regs and the RAM bytes at r3/r4) for one entry PC, to pin exactly
  // which op diverges. Off unless STATICRECOMP_LOCKSTEP_TRACE is set.
  if (const char* s = std::getenv("STATICRECOMP_LOCKSTEP_TRACE"))
    m_ls_trace_pc = static_cast<u32>(std::strtoull(s, nullptr, 0));
  // Comma/space-separated hex entry PCs never reported (known-benign classes,
  // e.g. mftb-reading blocks whose only divergence is the timebase low bits).
  if (const char* s = std::getenv("STATICRECOMP_LOCKSTEP_WHITELIST"))
  {
    const char* p = s;
    while (*p)
    {
      char* end = nullptr;
      const unsigned long long pc = std::strtoull(p, &end, 0);
      if (end == p)
        break;
      m_ls_whitelist.insert(static_cast<u32>(pc));
      p = end;
      while (*p == ',' || *p == ' ')
        ++p;
    }
  }

  std::fprintf(
      stderr,
      "[lockstep] ENABLED: start=%llu limit=%llu maxreport=%llu stepcap=%d whitelist=%zu\n",
      (unsigned long long)m_ls_start, (unsigned long long)m_ls_limit,
      (unsigned long long)m_ls_max_report, m_ls_step_cap, m_ls_whitelist.size());
}

bool StaticRecompCore::LockstepWindowOpen() const
{
  if (m_native_dispatches < m_ls_start)
    return false;
  if (m_ls_limit != 0 && m_native_dispatches >= m_ls_limit)
    return false;
  return true;
}

void StaticRecompCore::LoadEntryRegsToPPC(const CPUState& s)
{
  auto& power_pc = m_system.GetPowerPC();
  auto& ppc = power_pc.GetPPCState();
  std::memcpy(ppc.gpr, s.gpr, sizeof(ppc.gpr));
  for (int i = 0; i < 32; ++i)
  {
    std::memcpy(&ppc.ps[i].ps0, &s.fpr[i], sizeof(u64));
    std::memcpy(&ppc.ps[i].ps1, &s.ps1[i], sizeof(u64));
  }
  ppc.pc = s.pc;
  ppc.npc = s.pc;
  ppc.spr[SPR_LR] = s.lr;
  ppc.spr[SPR_CTR] = s.ctr;
  ppc.cr.Set(s.cr);
  ppc.SetXER(UReg_XER{s.xer});
  ppc.fpscr.Hex = s.fpscr;
  ppc.spr[SPR_SRR0] = s.srr0;
  ppc.spr[SPR_SRR1] = s.srr1;
  ppc.spr[SPR_DAR] = s.dar;
  ppc.spr[SPR_DSISR] = s.dsisr;
  ppc.spr[SPR_EAR] = s.ear;
  ppc.spr[SPR_HID2] = s.hid2;
  for (int i = 0; i < 16; ++i)
    ppc.sr[i] = s.sr[i];
  for (int i = 0; i < 8; ++i)
    ppc.spr[SPR_GQR0 + i] = s.gqr[i];
  ppc.reserve_address = s.reserve_addr;
  ppc.reserve = s.reserve_valid;
  ppc.Exceptions = 0;
  ppc.msr.Hex = s.msr;
  power_pc.MSRUpdated();
  PowerPC::RoundingModeUpdated(ppc);
}

void StaticRecompCore::LsJournalTrampoline(u32 offset, u32 size, void* user)
{
  auto* core = static_cast<StaticRecompCore*>(user);
  const u8* ram = core->m_guest.ram;
  const u32 ram_size = core->m_guest.ram_size;
  for (u32 i = 0; i < size; ++i)
  {
    const u32 off = offset + i;
    if (off >= ram_size)
      break;
    core->m_ls_pre.emplace(off, ram[off]);  // first touch => the pre-block byte
  }
}

// Records the interpreter shadow's MEM1 stores so they can be undone after the
// check (the shadow must be side-effect-free on the canonical native RAM).
void StaticRecompCore::LsShadowJournalTrampoline(u32 offset, u32 size, void* user)
{
  auto* core = static_cast<StaticRecompCore*>(user);
  const u8* ram = core->m_guest.ram;
  const u32 ram_size = core->m_guest.ram_size;
  for (u32 i = 0; i < size; ++i)
  {
    const u32 off = offset + i;
    if (off >= ram_size)
      break;
    core->m_ls_shadow_pre.emplace(off, ram[off]);  // first touch => the pre-shadow byte
  }
}

// Records NATIVE's locked-cache stores (via HookExternalWrite -> MMU LC branch)
// during the journaled dispatch, so the block's LC pre-image can be restored
// before the shadow re-run (correct read-modify-write) and native's LC post-image
// redone afterward. Native's MMU-path LC writes bypass the generated ctx->ram
// journal, so without this the shadow would read native's post-LC values.
void StaticRecompCore::LsNativeLcJournalTrampoline(u32 lc_offset, u32 size, void* user)
{
  auto* core = static_cast<StaticRecompCore*>(user);
  auto& memory = core->m_system.GetMemory();
  const u8* l1 = memory.GetL1Cache();
  const u32 l1_size = memory.GetL1CacheSize();
  if (!l1)
    return;
  for (u32 i = 0; i < size; ++i)
  {
    const u32 off = lc_offset + i;
    if (off >= l1_size)
      break;
    core->m_ls_lc_pre.emplace(off, l1[off]);  // first touch => the pre-block byte
  }
}

// Records the interpreter shadow's locked-cache (L1Cache) stores so they can be
// undone after the check. Locked cache is memory the shadow commits normally
// (intra-block RMW must see it), but the canonical run is native, whose LC values
// were committed before the shadow ran; a leaked shadow LC store would corrupt
// the locked cache for later native blocks. Offset is the L1Cache byte index.
void StaticRecompCore::LsShadowLcJournalTrampoline(u32 lc_offset, u32 size, void* user)
{
  auto* core = static_cast<StaticRecompCore*>(user);
  auto& memory = core->m_system.GetMemory();
  const u8* l1 = memory.GetL1Cache();
  const u32 l1_size = memory.GetL1CacheSize();
  if (!l1)
    return;
  for (u32 i = 0; i < size; ++i)
  {
    const u32 off = lc_offset + i;
    if (off >= l1_size)
      break;
    core->m_ls_lc_shadow_pre.emplace(off, l1[off]);  // first touch => the pre-shadow byte
  }
}

// Records native's Fake-VMEM (guest VM window [0x7E000000,0x80000000)) writes so
// the shadow reads the block pre-image instead of native's committed value. This
// window maps to a dedicated buffer, not MEM1, so its writes bypass both the
// generated ctx->ram journal AND the MEM1 MMU journal; without this the shadow
// reads native's post-write bytes (the glSetRasterState / nlListAddStart stale
// reads). Offset is the Fake-VMEM byte index (em_address & GetFakeVMemMask()).
void StaticRecompCore::LsNativeVmemJournalTrampoline(u32 vmem_offset, u32 size, void* user)
{
  auto* core = static_cast<StaticRecompCore*>(user);
  auto& memory = core->m_system.GetMemory();
  const u8* vmem = memory.GetFakeVMEM();
  const u32 vmem_size = memory.GetFakeVMemSize();
  if (!vmem)
    return;
  for (u32 i = 0; i < size; ++i)
  {
    const u32 off = vmem_offset + i;
    if (off >= vmem_size)
      break;
    core->m_ls_vmem_pre.emplace(off, vmem[off]);  // first touch => the pre-block byte
  }
}

// Records the interpreter shadow's Fake-VMEM stores so they can be undone after
// the check (like MEM1/LC): the shadow commits normally for intra-block RMW, but
// the canonical run is native, whose VM-window values were committed before the
// shadow; a leaked shadow store would corrupt the window for later native blocks.
void StaticRecompCore::LsShadowVmemJournalTrampoline(u32 vmem_offset, u32 size, void* user)
{
  auto* core = static_cast<StaticRecompCore*>(user);
  auto& memory = core->m_system.GetMemory();
  const u8* vmem = memory.GetFakeVMEM();
  const u32 vmem_size = memory.GetFakeVMemSize();
  if (!vmem)
    return;
  for (u32 i = 0; i < size; ++i)
  {
    const u32 off = vmem_offset + i;
    if (off >= vmem_size)
      break;
    core->m_ls_vmem_shadow_pre.emplace(off, vmem[off]);  // first touch => the pre-shadow byte
  }
}

void StaticRecompCore::LsHwWriteTrampoline(u32 physical_address, u32 data, u32 size, void* user)
{
  auto* core = static_cast<StaticRecompCore*>(user);
  core->m_ls_interp_mmio.push_back({physical_address, data, size});
}

u32 StaticRecompCore::LsHwReadTrampoline(u32 physical_address, u32 size, void* user)
{
  auto* core = static_cast<StaticRecompCore*>(user);
  if (core->m_ls_read_index < core->m_ls_native_reads.size())
  {
    const LsWrite& rec = core->m_ls_native_reads[core->m_ls_read_index++];
    // Native effective vs shadow physical: a mismatch means the shadow diverged
    // from native's read sequence (a genuine control-flow split, not drift).
    if ((rec.addr & 0x0FFFFFFFu) != (physical_address & 0x0FFFFFFFu))
      core->m_ls_read_overflow = true;
    return rec.data;
  }
  core->m_ls_read_overflow = true;  // shadow issued more MMIO reads than native
  return 0;
}

// Re-run the just-executed native block on Dolphin's interpreter from the same
// entry snapshot and report any architectural / memory-write divergence. On
// entry m_guest holds the native result (N) and its pc is the block end PC; the
// RAM journal (m_ls_pre) holds pre-images of every byte native wrote.
void StaticRecompCore::LockstepCheck(u32 entry_pc, u32 end_pc, const CPUState& entry_state)
{
  ++m_ls_checks;

  // Blocks whose native run used the instruction fallback performed side
  // effects (DMA, decrementer reschedule, icache invalidation) the shadow would
  // double-issue; they are unsafe to re-run. Leave RAM as native committed it.
  if (m_ls_fallback_seen)
  {
    ++m_ls_skipped_fallback;
    return;
  }

  auto& power_pc = m_system.GetPowerPC();
  auto& ppc = power_pc.GetPPCState();
  auto& interp = m_system.GetInterpreter();
  auto& memory = m_system.GetMemory();
  u8* ram = m_guest.ram;
  const u32 ram_size = m_guest.ram_size;
  u8* const l1 = memory.GetL1Cache();
  const u32 l1_size = memory.GetL1CacheSize();
  u8* const vmem = memory.GetFakeVMEM();
  const u32 vmem_size = memory.GetFakeVMemSize();

  // Cycle budget native charged for this dispatch: native subtracts each basic
  // block's Dolphin-PPCTables cost into ctx->downcount before executing it, so
  // this equals the summed cost of every instruction native ran. A zero-charge
  // dispatch (embedded data / degenerate segment) has no alignable work; skip.
  const s64 native_charge = -m_guest.downcount;
  if (native_charge <= 0)
  {
    ++m_ls_skipped_zero;
    return;
  }

  // Native's post-images: realRAM currently holds native's committed writes.
  m_ls_post.clear();
  for (const auto& [off, pre] : m_ls_pre)
    m_ls_post.emplace(off, off < ram_size ? ram[off] : pre);
  // Undo native's RAM writes so the interpreter shadow reads the block's
  // pre-image (correct read-modify-write comparison).
  for (const auto& [off, pre] : m_ls_pre)
    if (off < ram_size)
      ram[off] = pre;

  // Same for native's locked-cache writes (MMU-path, journaled in m_ls_lc_pre).
  m_ls_lc_post.clear();
  if (l1)
  {
    for (const auto& [off, pre] : m_ls_lc_pre)
      m_ls_lc_post.emplace(off, off < l1_size ? l1[off] : pre);
    for (const auto& [off, pre] : m_ls_lc_pre)
      if (off < l1_size)
        l1[off] = pre;
  }

  // Same for native's Fake-VMEM writes (guest VM window, journaled in m_ls_vmem_pre).
  m_ls_vmem_post.clear();
  if (vmem)
  {
    for (const auto& [off, pre] : m_ls_vmem_pre)
      m_ls_vmem_post.emplace(off, off < vmem_size ? vmem[off] : pre);
    for (const auto& [off, pre] : m_ls_vmem_pre)
      if (off < vmem_size)
        vmem[off] = pre;
  }

  // Load the block-entry registers and single-step to the native end PC with
  // hardware writes captured + suppressed. Save ONLY the burst-visible scratch
  // the shadow perturbs: PowerPCState owns non-trivial caches (iCache/dCache),
  // so bulk-copying it would double-free their heap storage. All register
  // fields are stale during a native burst (native runs on m_guest; ppc is
  // rebuilt at SyncOut / resynced by PropagateGuestMSR), so they need no
  // save/restore here.
  const u64 saved_msr = ppc.msr.Hex;
  const int saved_downcount = ppc.downcount;
  const u32 saved_exceptions = ppc.Exceptions;
  LoadEntryRegsToPPC(entry_state);

  m_ls_interp_mmio.clear();
  m_ls_shadow_pre.clear();
  m_ls_lc_shadow_pre.clear();
  m_ls_vmem_shadow_pre.clear();
  m_ls_read_index = 0;
  m_ls_read_overflow = false;
  StaticRecompLockstep::g_hw_write_sink = &StaticRecompCore::LsHwWriteTrampoline;
  StaticRecompLockstep::g_hw_write_sink_user = this;
  StaticRecompLockstep::g_hw_read_sink = &StaticRecompCore::LsHwReadTrampoline;
  StaticRecompLockstep::g_hw_read_sink_user = this;
  // Journal the shadow's MEM1 stores so they can be undone (shadow must not
  // mutate the canonical native RAM).
  StaticRecompLockstep::g_ram_write_journal = &StaticRecompCore::LsShadowJournalTrampoline;
  StaticRecompLockstep::g_ram_write_journal_user = this;
  // Same for the shadow's locked-cache (L1Cache) stores.
  StaticRecompLockstep::g_lc_write_journal = &StaticRecompCore::LsShadowLcJournalTrampoline;
  StaticRecompLockstep::g_lc_write_journal_user = this;
  // Same for the shadow's Fake-VMEM (guest VM window) stores.
  StaticRecompLockstep::g_vmem_write_journal = &StaticRecompCore::LsShadowVmemJournalTrampoline;
  StaticRecompLockstep::g_vmem_write_journal_user = this;
  // Feed the shadow the same timebase native's mftb observed (cached per burst).
  StaticRecompLockstep::g_tb_override_active = true;
  StaticRecompLockstep::g_tb_override_value = entry_state.timebase;

  // Step the interpreter for exactly native's charged cycles. This reproduces
  // native's instruction count precisely regardless of how end_pc is reached
  // (a loop body may pass through end_pc sequentially before native's real
  // boundary at the back-edge; a segment may end on a straight-line fall-
  // through). Crucially it BOUNDS the shadow to native's work, so it can never
  // overshoot into data / unknown instructions. A clean alignment lands exactly
  // on native's charge with pc == end_pc; anything else is a real divergence.
  if (m_ls_trace_pc != 0 && entry_pc == m_ls_trace_pc)
  {
    const auto dump = [&](const char* tag, u32 gaddr) {
      const u32 o = gaddr - 0x80000000u;
      std::fprintf(stderr, "[ls-trace] entry %s=0x%08X bytes:", tag, gaddr);
      for (u32 k = 0; k < 20 && o + k < ram_size; ++k)
        std::fprintf(stderr, " %02X", ram[o + k]);
      std::fprintf(stderr, "\n");
    };
    std::fprintf(stderr, "[ls-trace] ENTRY r3=0x%08X r4=0x%08X r5=0x%08X charge=%lld\n",
                 entry_state.gpr[3], entry_state.gpr[4], entry_state.gpr[5],
                 (long long)native_charge);
    if ((entry_state.gpr[3] >> 28) == 8)
      dump("r3", entry_state.gpr[3]);
    if ((entry_state.gpr[4] >> 28) == 8)
      dump("r4", entry_state.gpr[4]);
  }
  int steps = 0;
  s64 interp_cycles = 0;
  // If end_pc is a loop header (native returned at the back-edge), the shadow
  // reaches it FIRST by sequential fall-through into the body; the cycle-budget
  // stop must ignore that arrival and wait for the back-edge (a non-sequential
  // arrival, caught above), or it stops one iteration short. Computed once.
  const bool end_is_loop_header = LsIsLoopHeader(ram, ram_size, end_pc);
  while (steps < m_ls_step_cap)
  {
    const u32 before = ppc.pc;
    interp_cycles += interp.SingleStepInner();
    ++steps;
    // Diagnostic (STATICRECOMP_LOCKSTEP_TRACE): per-instruction shadow evolution
    // for one entry PC — pins which op diverges (fpscr/cr for FP, gpr for data).
    if (m_ls_trace_pc != 0 && entry_pc == m_ls_trace_pc)
    {
      u32 op = 0;
      const u32 boff = before - 0x80000000u;
      if (boff + 4 <= ram_size)
        op = Common::swap32(&ram[boff]);
      const u32 fx = ppc.fpscr.Hex;
      std::fprintf(stderr,
                   "[ls-trace] %3d pc=0x%08X op=0x%08X (op0=%2u xo=%4u) fpscr=0x%08X "
                   "FG=%u FR=%u FI=%u FPRF=0x%02X cr=0x%08X r0=0x%08X r3=0x%08X r4=0x%08X r5=0x%08X\n",
                   steps, before, op, op >> 26, (op >> 1) & 0x3FF, fx, (fx >> 14) & 1,
                   (fx >> 17) & 1, (fx >> 16) & 1, (fx >> 12) & 0x1F, ppc.cr.Get(), ppc.gpr[0],
                   ppc.gpr[3], ppc.gpr[4], ppc.gpr[5]);
    }
    // Native ends a dispatch only at a control transfer, so a NON-sequential
    // arrival at end_pc (branch, loop back-edge, indirect return, exception
    // vector) is native's boundary — stop there even if native over-charged (a
    // mid-block lazy-FP/sc fault exits before the block's full static charge).
    //
    // Loop-header exception: when end_pc is a loop header, the emitter emits the
    // back-edge as a dispatcher round-trip (native's real boundary) but the
    // forward branch that ENTERS the loop from the prologue stays a local `goto`
    // native does NOT return from. So the shadow reaches end_pc non-sequentially
    // first at the loop ENTRY (a forward, non-linking, same-chunk branch), then
    // at the back-edge; native ran one body iteration and returned at the back-
    // edge. Skip only that local forward entry. A forward arrival that LINKS (bl/
    // bcl call), is INDIRECT (blr/bctr), or CROSSES a chunk is a dispatcher round-
    // trip the emitter DID return from — a real boundary (e.g. a `bl` to a
    // function whose own entry is a loop header, GetLinearVelocity/AngularVelocity
    // in Strikers) — so keep it.
    if (ppc.pc == end_pc && ppc.pc != before + 4)
    {
      bool is_boundary = true;
      if (end_is_loop_header && before < end_pc)
      {
        const u32 boff = before - 0x80000000u;
        const u32 binsn = (boff + 4u <= ram_size) ? Common::swap32(&ram[boff]) : 0u;
        const u32 opcd = binsn >> 26;
        const bool links = (opcd == 16u || opcd == 18u) && (binsn & 1u);  // bcl / bl
        const bool indirect = (opcd == 19u);                              // bclr / bcctr
        const bool cross_chunk = ChunkIndexOf(before) != ChunkIndexOf(end_pc);
        if (!links && !indirect && !cross_chunk)
          is_boundary = false;  // local forward goto into the loop body, not a return
      }
      if (is_boundary)
        break;
    }
    if (interp_cycles >= native_charge)
    {
      // Native's static charge is consumed. At end_pc now => a clean straight-
      // line segment boundary; stop — UNLESS end_pc is a loop header, where this
      // first arrival is the sequential fall-through into the body and native's
      // real boundary is the later back-edge (caught non-sequentially above); in
      // that case step on. Otherwise native UNDERCHARGED: it was dispatched into
      // an address past an accounting block's `downcount -=` charge (mid-
      // accounting-block entry: a cross-function / indirect / jump-table target
      // that is not an accounting leader), so native_charge is short of the
      // block's true cost. Rather than stop here and misreport a charge-only
      // deficit as a control-flow divergence, step on toward native's real
      // boundary (end_pc, which native provably reached) for a bounded grace,
      // then compare registers. A genuine split would fail to reach end_pc within
      // the grace and still report CTRLFLOW. The loop-header undercharge (the
      // prologue native skipped) is a handful of cycles, far under the grace, so
      // the back-edge is always reached in-window. (The D3 mid-accounting-block-
      // entry undercharge itself is a separate downcount item; see
      // dolphin-chassis.md.)
      if (ppc.pc == end_pc && !end_is_loop_header)
        break;
      if (interp_cycles >= native_charge + LS_UNDERCHARGE_GRACE)
        break;
    }
  }
  const bool reached = (ppc.pc == end_pc);
  const bool undercharged = reached && interp_cycles > native_charge;

  StaticRecompLockstep::g_hw_write_sink = nullptr;
  StaticRecompLockstep::g_hw_write_sink_user = nullptr;
  StaticRecompLockstep::g_hw_read_sink = nullptr;
  StaticRecompLockstep::g_hw_read_sink_user = nullptr;
  StaticRecompLockstep::g_tb_override_active = false;
  StaticRecompLockstep::g_ram_write_journal = nullptr;
  StaticRecompLockstep::g_ram_write_journal_user = nullptr;
  StaticRecompLockstep::g_lc_write_journal = nullptr;
  StaticRecompLockstep::g_lc_write_journal_user = nullptr;
  StaticRecompLockstep::g_vmem_write_journal = nullptr;
  StaticRecompLockstep::g_vmem_write_journal_user = nullptr;

  // ---- Compare native (N = m_guest) vs interpreter (I = ppc) ---------------
  std::string diff;
  const auto addu = [&](const std::string& name, u64 n, u64 i) {
    if (n != i)
      diff += fmt::format(" {}:N={:#x},I={:#x}", name, n, i);
  };

  if (!reached)
  {
    diff += fmt::format(" CTRLFLOW:N_end={:#010x},I_pc={:#010x},steps={},N_cyc={},I_cyc={}", end_pc,
                        ppc.pc, steps, native_charge, interp_cycles);
  }
  else
  {
    for (int r = 0; r < 32; ++r)
      addu(fmt::format("r{}", r), m_guest.gpr[r], ppc.gpr[r]);
    for (int r = 0; r < 32; ++r)
    {
      u64 n, i;
      std::memcpy(&n, &m_guest.fpr[r], sizeof(u64));
      std::memcpy(&i, &ppc.ps[r].ps0, sizeof(u64));
      addu(fmt::format("f{}", r), n, i);
      std::memcpy(&n, &m_guest.ps1[r], sizeof(u64));
      std::memcpy(&i, &ppc.ps[r].ps1, sizeof(u64));
      addu(fmt::format("ps1_{}", r), n, i);
    }
    addu("lr", m_guest.lr, ppc.spr[SPR_LR]);
    addu("ctr", m_guest.ctr, ppc.spr[SPR_CTR]);
    addu("cr", m_guest.cr, ppc.cr.Get());
    addu("xer", m_guest.xer, ppc.GetXER().Hex);
    addu("fpscr", m_guest.fpscr, ppc.fpscr.Hex);
    addu("msr", m_guest.msr, ppc.msr.Hex);
    addu("srr0", m_guest.srr0, ppc.spr[SPR_SRR0]);
    addu("srr1", m_guest.srr1, ppc.spr[SPR_SRR1]);
    addu("pc", m_guest.pc, ppc.pc);

    // RAM writes: native's committed value (post) vs the interpreter's value
    // now in realRAM (it committed onto the restored pre-image).
    for (const auto& [off, post] : m_ls_post)
    {
      const u8 iv = (off < ram_size) ? ram[off] : post;
      if (iv != post)
        diff += fmt::format(" mem[{:#010x}]:N={:#04x},I={:#04x}", 0x80000000u + off, post, iv);
    }

    // Locked-cache writes: same comparison against L1Cache.
    if (l1)
    {
      for (const auto& [off, post] : m_ls_lc_post)
      {
        const u8 iv = (off < l1_size) ? l1[off] : post;
        if (iv != post)
          diff += fmt::format(" lc[{:#010x}]:N={:#04x},I={:#04x}", 0xE0000000u + off, post, iv);
      }
    }

    // Fake-VMEM writes: same comparison against the guest VM window buffer.
    if (vmem)
    {
      for (const auto& [off, post] : m_ls_vmem_post)
      {
        const u8 iv = (off < vmem_size) ? vmem[off] : post;
        if (iv != post)
          diff += fmt::format(" vmem[{:#08x}]:N={:#04x},I={:#04x}", off, post, iv);
      }
    }

    // MMIO / gather-pipe writes: native (hooks) vs interpreter (suppressed
    // sink). Compare only the low `size` bytes — native's hook value is already
    // width-masked, but the interpreter's sink captures the raw store register.
    // Normalise the address to physical (strip cache/segment bits).
    const auto low = [](u32 v, u32 sz) { return sz >= 4 ? v : (v & ((1u << (sz * 8)) - 1)); };
    if (m_ls_native_mmio.size() != m_ls_interp_mmio.size())
    {
      diff += fmt::format(" mmio#:N={},I={}", m_ls_native_mmio.size(), m_ls_interp_mmio.size());
      // List each side's addresses so a residual count mismatch is diagnosable
      // (which region/path split), not just a bare count.
      for (const LsWrite& w : m_ls_native_mmio)
        diff += fmt::format(" N@{:#010x}/{}", w.addr, w.size);
      for (const LsWrite& w : m_ls_interp_mmio)
        diff += fmt::format(" I@{:#010x}/{}", w.addr, w.size);
    }
    else
    {
      for (size_t k = 0; k < m_ls_native_mmio.size(); ++k)
      {
        const LsWrite& a = m_ls_native_mmio[k];
        const LsWrite& b = m_ls_interp_mmio[k];
        if ((a.addr & 0x0FFFFFFFu) != (b.addr & 0x0FFFFFFFu) || a.size != b.size ||
            low(a.data, a.size) != low(b.data, b.size))
        {
          diff += fmt::format(" mmio[{}]:N={:#x}={:#x}/{},I={:#x}={:#x}/{}", k, a.addr,
                              low(a.data, a.size), a.size, b.addr, low(b.data, b.size), b.size);
        }
      }
    }
    if (m_ls_read_overflow)
      diff += " mmio-read-seq-divergence";
  }

  if (!diff.empty() && m_ls_whitelist.find(entry_pc) == m_ls_whitelist.end())
  {
    ++m_ls_reports;
    if (m_ls_max_report == 0 || m_ls_reports <= m_ls_max_report)
    {
      std::fprintf(stderr, "[lockstep] DIVERGE #%llu entry=0x%08X end=0x%08X:%s\n",
                   (unsigned long long)m_ls_reports, entry_pc, end_pc, diff.c_str());
    }
  }
  else if (undercharged)
  {
    // Register/memory/MMIO all matched at native's true boundary, but native
    // charged fewer cycles than the interpreter needed to reach it: a D3
    // downcount undercharge from mid-accounting-block dispatch entry. Not an
    // architectural divergence — quantify it separately (deficit in cycles).
    ++m_ls_undercharges;
    const s64 deficit = interp_cycles - native_charge;
    if (deficit > m_ls_max_undercharge)
      m_ls_max_undercharge = deficit;
    if (m_ls_undercharges <= 64)
    {
      std::fprintf(stderr,
                   "[lockstep] UNDERCHARGE #%llu entry=0x%08X end=0x%08X: "
                   "N_cyc=%lld I_cyc=%lld deficit=%lld (regs/mem exact)\n",
                   (unsigned long long)m_ls_undercharges, entry_pc, end_pc,
                   (long long)native_charge, (long long)interp_cycles, (long long)deficit);
    }
  }

  // Undo the shadow's MEM1 stores (back to the block pre-image), then redo
  // native's writes so RAM is canonical for continued native execution. Undo
  // first: the shadow may have written offsets native did not (e.g. a hardware
  // block whose interpreter path stores to RAM where native's hook routed the
  // write elsewhere); those are absent from m_ls_post and would otherwise leak.
  for (const auto& [off, pre] : m_ls_shadow_pre)
    if (off < ram_size)
      ram[off] = pre;
  for (const auto& [off, post] : m_ls_post)
    if (off < ram_size)
      ram[off] = post;
  // Same for locked cache: undo the shadow's LC stores back to the block
  // pre-image, then redo native's committed LC writes so canonical LC is correct
  // for continued native execution.
  if (l1)
  {
    for (const auto& [off, pre] : m_ls_lc_shadow_pre)
      if (off < l1_size)
        l1[off] = pre;
    for (const auto& [off, post] : m_ls_lc_post)
      if (off < l1_size)
        l1[off] = post;
  }
  // Same for the Fake-VMEM guest VM window: undo the shadow's stores, then redo
  // native's committed writes so the window is canonical for continued native.
  if (vmem)
  {
    for (const auto& [off, pre] : m_ls_vmem_shadow_pre)
      if (off < vmem_size)
        vmem[off] = pre;
    for (const auto& [off, post] : m_ls_vmem_post)
      if (off < vmem_size)
        vmem[off] = post;
  }
  // Restore burst scratch (feature flags from msr, downcount, exceptions) and
  // re-arm the host FP mode for continued native execution off m_guest.
  ppc.msr.Hex = saved_msr;
  power_pc.MSRUpdated();
  ppc.downcount = saved_downcount;
  ppc.Exceptions = saved_exceptions;
  if (m_module->on_state_loaded)
    m_module->on_state_loaded(&m_guest);
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

  while (*state_ptr == CPU::State::Running)
  {
    core_timing.Advance();

    do
    {
      // MSR.FP needs no gate here: generated FPU instructions raise the
      // FP-unavailable exception themselves (ppc_fp_available).
      if (m_module && DispatchableAt(ppc.pc))
      {
        SyncIn();
        ++m_bursts;
        do
        {
          // Lockstep differential: for the first native dispatch of each
          // distinct entry PC (deduped), journal native's RAM writes so the
          // block can be re-run on the interpreter and compared. Inert unless
          // STATICRECOMP_LOCKSTEP is set (m_lockstep short-circuits first).
          const u32 ls_entry = m_guest.pc;
          const bool do_ls = m_lockstep && LockstepWindowOpen() &&
                             m_ls_checked.find(ls_entry) == m_ls_checked.end();
          CPUState ls_snapshot;
          if (do_ls)
          {
            ls_snapshot = m_guest;
            m_ls_pre.clear();
            m_ls_lc_pre.clear();
            m_ls_vmem_pre.clear();
            m_ls_native_mmio.clear();
            m_ls_native_reads.clear();
            m_ls_fallback_seen = false;
            m_ls_journaling = true;
            // Generated flat-RAM (ctx->ram) writes journal through the module hook.
            m_set_mem_journal(&StaticRecompCore::LsJournalTrampoline, this);
            // Native's MMU-path writes (MEM1, locked cache, and the guest VM
            // window's Fake-VMEM buffer) bypass that hook — capture their pre-
            // images here too so the shadow re-run reads the block pre-image
            // (correct RMW), not native's committed values. The same trampoline
            // funnels MEM1 writes into m_ls_pre; LC and Fake-VMEM have their own.
            StaticRecompLockstep::g_ram_write_journal = &StaticRecompCore::LsJournalTrampoline;
            StaticRecompLockstep::g_ram_write_journal_user = this;
            StaticRecompLockstep::g_lc_write_journal = &StaticRecompCore::LsNativeLcJournalTrampoline;
            StaticRecompLockstep::g_lc_write_journal_user = this;
            StaticRecompLockstep::g_vmem_write_journal = &StaticRecompCore::LsNativeVmemJournalTrampoline;
            StaticRecompLockstep::g_vmem_write_journal_user = this;
          }

          m_module->dispatch(&m_guest, m_guest.pc);
          ++m_native_dispatches;

          if (do_ls)
          {
            m_ls_journaling = false;
            m_set_mem_journal(nullptr, nullptr);
            StaticRecompLockstep::g_ram_write_journal = nullptr;
            StaticRecompLockstep::g_ram_write_journal_user = nullptr;
            StaticRecompLockstep::g_lc_write_journal = nullptr;
            StaticRecompLockstep::g_lc_write_journal_user = nullptr;
            StaticRecompLockstep::g_vmem_write_journal = nullptr;
            StaticRecompLockstep::g_vmem_write_journal_user = nullptr;
            m_ls_checked.insert(ls_entry);
            LockstepCheck(ls_entry, m_guest.pc, ls_snapshot);
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
        } while (FastDispatchableAt(m_guest.pc) && ppc.downcount > 0 &&
                 *state_ptr == CPU::State::Running);
        SyncOut();
        if ((ppc.Exceptions & SYNC_EXCEPTION_MASK) != 0)
          power_pc.CheckExceptions();
      }
      else
      {
        // SingleStepInner delivers synchronous exceptions itself; external
        // interrupts are delivered at slice start, as in Interpreter::Run.
        do
        {
          ppc.downcount -= interpreter.SingleStepInner();
          ++m_fallback_steps;
          /*
          if (VerboseCounters() && (m_fallback_steps & 0xFFFFFu) == 1u)
          {
            const int idx = m_module ? ChunkIndexOf(ppc.pc) : -2;
            std::fprintf(stderr,
                         "[staticrecomp] fallback=%llu native=%llu pc=0x%08X idx=%d st=%d "
                         "msr=0x%08X dc=%d rv=%llu hookfb=%llu\n",
                         (unsigned long long)m_fallback_steps,
                         (unsigned long long)m_native_dispatches, ppc.pc, idx,
                         idx >= 0 ? (int)m_chunk_state[idx] : -1, ppc.msr.Hex, ppc.downcount,
                         (unsigned long long)m_reverify_events,
                         (unsigned long long)m_hook_fallback_instructions);
          }
          */
        } while (!(m_module && DispatchableAt(ppc.pc)) && ppc.downcount > 0 &&
                 *state_ptr == CPU::State::Running);
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
