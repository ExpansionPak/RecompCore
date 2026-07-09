// RecompCore: StaticRecomp CPU core.
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "Common/CommonTypes.h"
#include "Common/DynamicLibrary.h"
#include "Core/PowerPC/JitCommon/JitBase.h"
#include "Core/PowerPC/JitCommon/JitCache.h"
#include "Core/PowerPC/StaticRecomp/StaticRecompABI.h"

namespace Core
{
class System;
}

// Executes statically recompiled per-game native code when the PC is covered by
// a loaded module; falls back to Dolphin's interpreter for everything else.
// With no module loaded this core is exactly an interpreter loop.
class StaticRecompCore : public JitBase
{
public:
  explicit StaticRecompCore(Core::System& system);
  StaticRecompCore(const StaticRecompCore&) = delete;
  StaticRecompCore(StaticRecompCore&&) = delete;
  StaticRecompCore& operator=(const StaticRecompCore&) = delete;
  StaticRecompCore& operator=(StaticRecompCore&&) = delete;
  ~StaticRecompCore() override;

  void Init() override;
  void Shutdown() override;

  void Run() override;
  void SingleStep() override;

  void ClearCache() override;
  void Jit(u32 em_address) override {}
  bool HandleFault(uintptr_t access_address, SContext* ctx) override { return false; }

  JitBaseBlockCache* GetBlockCache() override { return &m_block_cache; }
  void EraseSingleBlock(const JitBlock& block) override {}
  std::vector<MemoryStats> GetMemoryStats() const override { return {}; }
  std::size_t DisassembleNearCode(const JitBlock& block, std::ostream& stream) const override
  {
    return 0;
  }
  std::size_t DisassembleFarCode(const JitBlock& block, std::ostream& stream) const override
  {
    return 0;
  }
  const CommonAsmRoutinesBase* GetAsmRoutines() override { return nullptr; }
  const char* GetName() const override { return "StaticRecomp"; }

private:
  // JitBaseBlockCache with no generated blocks; exists so generic
  // icache-invalidation plumbing in JitInterface has a real object to talk
  // to, and to feed every invalidation into the SMC demotion guard (D4).
  class EmptyBlockCache : public JitBaseBlockCache
  {
  public:
    explicit EmptyBlockCache(StaticRecompCore& core) : JitBaseBlockCache(core), m_core(core) {}
    void WriteLinkBlock(const JitBlock::LinkData& source, const JitBlock* dest) override {}

  protected:
    void InvalidateICacheInternal(u32 physical_address, u32 address, u32 length,
                                  bool forced) override
    {
      m_core.OnICacheInvalidate(address, length);
    }

  private:
    StaticRecompCore& m_core;
  };

  void LoadModule();

  // D4 SMC guard, verify-on-entry model. Every chunk starts Unverified; the
  // first native dispatch into it hashes its guest RAM against the module's
  // recorded hash of the original text. An icache invalidation touching a
  // chunk resets it to Unverified (Dolphin invalidates while *loading* code,
  // so invalidation alone must not retire coverage); a hash mismatch (real
  // SMC) marks it Failed, interpreter-only until the next invalidation.
  enum ChunkState : u8
  {
    CHUNK_UNVERIFIED = 0,
    CHUNK_VERIFIED = 1,
    CHUNK_FAILED = 2,
  };

  void OnICacheInvalidate(u32 address, u32 length);
  int ChunkIndexOf(u32 address) const;
  bool DispatchableAt(u32 address);              // lazily verifies Unverified chunks
  bool FastDispatchableAt(u32 address) const;    // Verified chunks only
  void VerifyChunk(u32 index);

  // D1 state residency: registers live in m_guest while native code runs;
  // full sync at every native-burst boundary.
  void SyncIn();   // Dolphin PowerPCState -> m_guest
  void SyncOut();  // m_guest -> Dolphin PowerPCState

  // CPUState hooks (module -> chassis environment). `cpu->external_user_data`
  // is the StaticRecompCore*.
  static u64 HookExternalRead(CPUState* cpu, u32 ea, u8 size);
  static void HookExternalWrite(CPUState* cpu, u32 ea, u64 value, u8 size);
  static u32 HookExternalRead32(CPUState* cpu, u32 ea, u8 rid);
  static void HookExternalWrite32(CPUState* cpu, u32 ea, u32 value, u8 rid);
  static void* HookExternalPointer(CPUState* cpu, u32 ea, u32 size);
  static void HookInstructionFallback(CPUState* cpu, u32 raw, u32 cia);

  // Keep Dolphin's MSR-derived state (translation mode, feature flags) in step
  // with the guest MSR before any MMU access or exception delivery.
  void PropagateGuestMSR();

  // --- Lockstep differential (STATICRECOMP_LOCKSTEP) -------------------------
  // For the first native dispatch of each distinct entry PC (deduped), re-run
  // the same basic block on Dolphin's interpreter from an identical snapshot
  // and compare every architectural field + memory writes. Native remains the
  // canonical run; the interpreter is a shadow whose hardware writes are
  // suppressed and whose RAM view is the block's pre-image (so read-modify-
  // write compares correctly). Divergences dump the entry PC and differing
  // fields. Entirely inert unless enabled (zero behaviour change when off).
  void InitLockstep();
  bool LockstepWindowOpen() const;
  void LockstepCheck(u32 entry_pc, u32 end_pc, const CPUState& entry_state);
  void LoadEntryRegsToPPC(const CPUState& s);  // CPUState -> PowerPCState (regs only)
  static void LsJournalTrampoline(u32 offset, u32 size, void* user);
  static void LsShadowJournalTrampoline(u32 offset, u32 size, void* user);
  static void LsNativeLcJournalTrampoline(u32 lc_offset, u32 size, void* user);
  static void LsShadowLcJournalTrampoline(u32 lc_offset, u32 size, void* user);
  static void LsNativeVmemJournalTrampoline(u32 vmem_offset, u32 size, void* user);
  static void LsShadowVmemJournalTrampoline(u32 vmem_offset, u32 size, void* user);
  static void LsHwWriteTrampoline(u32 physical_address, u32 data, u32 size, void* user);
  static u32 LsHwReadTrampoline(u32 physical_address, u32 size, void* user);

  using SetMemJournalFn = void (*)(void (*)(u32, u32, void*), void*);

  struct LsWrite
  {
    u32 addr;
    u32 data;
    u32 size;
  };

  bool m_lockstep = false;
  u64 m_ls_start = 0;             // begin checking at this native-dispatch index
  u64 m_ls_limit = 0;            // stop checking after this index (0 = no bound)
  u64 m_ls_max_report = 0;      // cap divergence reports (0 = unlimited)
  int m_ls_step_cap = 512;      // interpreter single-steps before giving up on end PC
  u64 m_ls_checks = 0;          // distinct blocks differentially checked
  u64 m_ls_reports = 0;         // divergences reported
  u64 m_ls_skipped_fallback = 0;  // blocks skipped (native used instruction fallback)
  u64 m_ls_skipped_zero = 0;      // blocks skipped (zero cycle charge: no alignable work)
  u64 m_ls_undercharges = 0;      // blocks regs-exact but native undercharged downcount (D3)
  s64 m_ls_max_undercharge = 0;   // worst per-block cycle deficit observed
  SetMemJournalFn m_set_mem_journal = nullptr;  // resolved from the module
  std::unordered_set<u32> m_ls_checked;    // entry PCs already checked (dedupe)
  std::unordered_set<u32> m_ls_whitelist;  // entry PCs never reported (known-benign)
  u32 m_ls_trace_pc = 0;  // STATICRECOMP_LOCKSTEP_TRACE: per-instr shadow dump for one entry PC

  // per-check scratch, reused to avoid per-block allocation churn:
  bool m_ls_journaling = false;      // native journal active (guards trampolines)
  bool m_ls_fallback_seen = false;   // native took the instruction-fallback path
  std::unordered_map<u32, u8> m_ls_pre;   // ram offset -> pre-block byte (native writes)
  std::unordered_map<u32, u8> m_ls_post;  // ram offset -> native post byte
  std::unordered_map<u32, u8> m_ls_shadow_pre;  // ram offset -> pre-shadow byte (shadow writes)
  std::unordered_map<u32, u8> m_ls_lc_pre;   // L1Cache offset -> pre-block byte (native LC writes)
  std::unordered_map<u32, u8> m_ls_lc_post;  // L1Cache offset -> native post byte
  std::unordered_map<u32, u8> m_ls_lc_shadow_pre;  // L1Cache offset -> pre-shadow byte (shadow LC writes)
  std::unordered_map<u32, u8> m_ls_vmem_pre;   // Fake-VMEM offset -> pre-block byte (native VM writes)
  std::unordered_map<u32, u8> m_ls_vmem_post;  // Fake-VMEM offset -> native post byte
  std::unordered_map<u32, u8> m_ls_vmem_shadow_pre;  // Fake-VMEM offset -> pre-shadow byte (shadow VM writes)
  std::vector<LsWrite> m_ls_native_mmio;   // native MMIO/gather writes (hooks)
  std::vector<LsWrite> m_ls_interp_mmio;   // interpreter MMIO/gather writes (sink)
  std::vector<LsWrite> m_ls_native_reads;  // native MMIO reads (hooks), replayed to the shadow
  size_t m_ls_read_index = 0;              // replay cursor into m_ls_native_reads
  bool m_ls_read_overflow = false;         // shadow read more MMIO than native (path split)

  EmptyBlockCache m_block_cache{*this};

  CPUState m_guest{};
  Common::DynamicLibrary m_library;
  const StaticRecompModuleDesc* m_module = nullptr;

  u64 m_native_dispatches = 0;
  u64 m_fallback_steps = 0;
  u64 m_native_exceptions = 0;
  u64 m_hook_fallback_instructions = 0;
  u64 m_bursts = 0;          // SyncIn..SyncOut native runs (diagnostic)
  u64 m_charged_cycles = 0;  // cycles flushed from module charges (diagnostic)

  // D4 guard state: parallel to m_module->chunk_ranges.
  std::vector<u8> m_chunk_state;
  u32 m_failed_chunks = 0;    // chunks currently failing verification (real SMC)
  u64 m_verifications = 0;    // chunk hash checks performed
  u64 m_reverify_events = 0;  // invalidations that reset a chunk to Unverified

  // Dispatch locality: most control transfers stay inside one chunk, so the
  // last hit short-circuits the chunk binary search on the hot path.
  mutable u32 m_last_chunk_index = 0;
};
