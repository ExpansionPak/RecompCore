// RecompCore: StaticRecomp lockstep differential hook.
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/PowerPC/StaticRecomp/lockstep/StaticRecompLockstep.h"
#include "Core/PowerPC/StaticRecomp/StaticRecompCore.h"
#include "Core/PowerPC/PowerPC.h"
#include "Core/PowerPC/Interpreter/Interpreter.h"
#include "Core/PowerPC/JitCommon/JitBase.h"
#include "Core/PowerPC/JitCommon/JitCache.h"
#include "Core/HW/GPFifo.h"
#include "Core/HW/HW.h"
#include "Core/HW/Memmap.h"
#include "Core/System.h"
#include "Core/Core.h"
#include "Core/ConfigManager.h"
#include "Common/Logging/Log.h"
#include "Common/StringUtil.h"

#include <cstdlib>
#include <cstring>
#include <chrono>
#include <fmt/format.h>

namespace StaticRecompLockstep
{
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

namespace
{
constexpr s64 LS_UNDERCHARGE_GRACE = 1200;

bool LsIsLoopHeader(const u8* ram, u32 ram_size, u32 end_pc)
{
  constexpr u32 kBase = 0x80000000u;
  if (end_pc < kBase)
    return false;
  const u32 start_off = end_pc - kBase;
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

bool LsHwAccessInScope(PowerPC::MMU& mmu, u32 ea)
{
  u32 phys = ea;
  if (const std::optional<u32> t = mmu.GetTranslatedAddress(ea))
    phys = *t;
  const bool is_gather = (phys & 0xFFFFF000u) == GPFifo::GATHER_PIPE_PHYSICAL_ADDRESS;
  const bool is_mmio = (phys & 0xF8000000u) == 0x08000000u;
  return is_gather || is_mmio;
}

StaticRecompLockstepVerifier::StaticRecompLockstepVerifier(StaticRecompCore& core)
    : m_core(core)
{
}

StaticRecompLockstepVerifier::~StaticRecompLockstepVerifier()
{
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
}

void StaticRecompLockstepVerifier::Init()
{
  const char* enable = std::getenv("STATICRECOMP_LOCKSTEP");
  if (!enable || enable[0] == '0' || enable[0] == '\0')
    return;
  if (!m_core.m_module)
  {
    ERROR_LOG_FMT(POWERPC, "StaticRecomp: LOCKSTEP requested but no module loaded; ignored.");
    return;
  }
  m_set_mem_journal = reinterpret_cast<SetMemJournalFn>(m_core.m_library.GetSymbolAddress("ppc_set_mem_write_journal"));
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
  if (const char* s = std::getenv("STATICRECOMP_LOCKSTEP_TRACE"))
    m_ls_trace_pc = static_cast<u32>(std::strtoull(s, nullptr, 0));
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

bool StaticRecompLockstepVerifier::ShouldCheck(u32 address) const
{
  if (!m_lockstep)
    return false;
  if (!LockstepWindowOpen())
    return false;
  return m_ls_checked.find(address) == m_ls_checked.end();
}

bool StaticRecompLockstepVerifier::LockstepWindowOpen() const
{
  if (m_core.m_native_dispatches < m_ls_start)
    return false;
  if (m_ls_limit != 0 && m_core.m_native_dispatches >= m_ls_limit)
    return false;
  return true;
}

void StaticRecompLockstepVerifier::Prepare(const CPUState& guest)
{
  m_ls_entry = guest.pc;
  m_ls_snapshot = guest;
  m_journal.Clear();
  m_ls_fallback_seen = false;
  m_ls_journaling = true;
  m_set_mem_journal(&StaticRecompLockstepVerifier::LsJournalTrampoline, this);
  StaticRecompLockstep::g_ram_write_journal = &StaticRecompLockstepVerifier::LsJournalTrampoline;
  StaticRecompLockstep::g_ram_write_journal_user = this;
  StaticRecompLockstep::g_lc_write_journal = &StaticRecompLockstepVerifier::LsNativeLcJournalTrampoline;
  StaticRecompLockstep::g_lc_write_journal_user = this;
  StaticRecompLockstep::g_vmem_write_journal = &StaticRecompLockstepVerifier::LsNativeVmemJournalTrampoline;
  StaticRecompLockstep::g_vmem_write_journal_user = this;
}

void StaticRecompLockstepVerifier::Verify(const CPUState& guest)
{
  m_ls_journaling = false;
  m_set_mem_journal(nullptr, nullptr);
  StaticRecompLockstep::g_ram_write_journal = nullptr;
  StaticRecompLockstep::g_ram_write_journal_user = nullptr;
  StaticRecompLockstep::g_lc_write_journal = nullptr;
  StaticRecompLockstep::g_lc_write_journal_user = nullptr;
  StaticRecompLockstep::g_vmem_write_journal = nullptr;
  StaticRecompLockstep::g_vmem_write_journal_user = nullptr;
  m_ls_checked.insert(m_ls_entry);
  LockstepCheck(m_ls_entry, guest.pc, m_ls_snapshot);
}

void StaticRecompLockstepVerifier::LoadEntryRegsToPPC(const CPUState& s)
{
  auto& power_pc = m_core.m_system.GetPowerPC();
  auto& ppc = power_pc.GetPPCState();
  StaticRecompCore::SetPPCStateFromGuestState(s, ppc);
  ppc.Exceptions = 0;
  ppc.msr.Hex = s.msr;
  power_pc.MSRUpdated();
  PowerPC::RoundingModeUpdated(ppc);
}

void StaticRecompLockstepVerifier::LsJournalTrampoline(u32 offset, u32 size, void* user)
{
  auto* verifier = static_cast<StaticRecompLockstepVerifier*>(user);
  const u8* ram = verifier->m_core.m_guest.ram;
  const u32 ram_size = verifier->m_core.m_guest.ram_size;
  for (u32 i = 0; i < size; ++i)
  {
    const u32 off = offset + i;
    if (off >= ram_size)
      break;
    verifier->m_journal.ram_pre.emplace(off, ram[off]);
  }
}

void StaticRecompLockstepVerifier::LsShadowJournalTrampoline(u32 offset, u32 size, void* user)
{
  auto* verifier = static_cast<StaticRecompLockstepVerifier*>(user);
  const u8* ram = verifier->m_core.m_guest.ram;
  const u32 ram_size = verifier->m_core.m_guest.ram_size;
  for (u32 i = 0; i < size; ++i)
  {
    const u32 off = offset + i;
    if (off >= ram_size)
      break;
    verifier->m_journal.ram_shadow_pre.emplace(off, ram[off]);
  }
}

void StaticRecompLockstepVerifier::LsNativeLcJournalTrampoline(u32 lc_offset, u32 size, void* user)
{
  auto* verifier = static_cast<StaticRecompLockstepVerifier*>(user);
  auto& memory = verifier->m_core.m_system.GetMemory();
  const u8* l1 = memory.GetL1Cache();
  const u32 l1_size = memory.GetL1CacheSize();
  if (!l1)
    return;
  for (u32 i = 0; i < size; ++i)
  {
    const u32 off = lc_offset + i;
    if (off >= l1_size)
      break;
    verifier->m_journal.lc_pre.emplace(off, l1[off]);
  }
}

void StaticRecompLockstepVerifier::LsShadowLcJournalTrampoline(u32 lc_offset, u32 size, void* user)
{
  auto* verifier = static_cast<StaticRecompLockstepVerifier*>(user);
  auto& memory = verifier->m_core.m_system.GetMemory();
  const u8* l1 = memory.GetL1Cache();
  const u32 l1_size = memory.GetL1CacheSize();
  if (!l1)
    return;
  for (u32 i = 0; i < size; ++i)
  {
    const u32 off = lc_offset + i;
    if (off >= l1_size)
      break;
    verifier->m_journal.lc_shadow_pre.emplace(off, l1[off]);
  }
}

void StaticRecompLockstepVerifier::LsNativeVmemJournalTrampoline(u32 vmem_offset, u32 size, void* user)
{
  auto* verifier = static_cast<StaticRecompLockstepVerifier*>(user);
  auto& memory = verifier->m_core.m_system.GetMemory();
  const u8* vmem = memory.GetFakeVMEM();
  const u32 vmem_size = memory.GetFakeVMemSize();
  if (!vmem)
    return;
  for (u32 i = 0; i < size; ++i)
  {
    const u32 off = vmem_offset + i;
    if (off >= vmem_size)
      break;
    verifier->m_journal.vmem_pre.emplace(off, vmem[off]);
  }
}

void StaticRecompLockstepVerifier::LsShadowVmemJournalTrampoline(u32 vmem_offset, u32 size, void* user)
{
  auto* verifier = static_cast<StaticRecompLockstepVerifier*>(user);
  auto& memory = verifier->m_core.m_system.GetMemory();
  const u8* vmem = memory.GetFakeVMEM();
  const u32 vmem_size = memory.GetFakeVMemSize();
  if (!vmem)
    return;
  for (u32 i = 0; i < size; ++i)
  {
    const u32 off = vmem_offset + i;
    if (off >= vmem_size)
      break;
    verifier->m_journal.vmem_shadow_pre.emplace(off, vmem[off]);
  }
}

void StaticRecompLockstepVerifier::LsHwWriteTrampoline(u32 physical_address, u32 data, u32 size, void* user)
{
  auto* verifier = static_cast<StaticRecompLockstepVerifier*>(user);
  verifier->m_journal.interp_mmio.push_back({physical_address, data, size});
}

u32 StaticRecompLockstepVerifier::LsHwReadTrampoline(u32 physical_address, u32 size, void* user)
{
  auto* verifier = static_cast<StaticRecompLockstepVerifier*>(user);
  if (verifier->m_ls_read_index < verifier->m_journal.native_reads.size())
  {
    const LsWrite& rec = verifier->m_journal.native_reads[verifier->m_ls_read_index++];
    if ((rec.addr & 0x0FFFFFFFu) != (physical_address & 0x0FFFFFFFu))
      verifier->m_ls_read_overflow = true;
    return rec.data;
  }
  verifier->m_ls_read_overflow = true;
  return 0;
}

void StaticRecompLockstepVerifier::LockstepCheck(u32 entry_pc, u32 end_pc, const CPUState& entry_state)
{
  ++m_ls_checks;

  if (m_ls_fallback_seen)
  {
    ++m_ls_skipped_fallback;
    return;
  }

  auto& power_pc = m_core.m_system.GetPowerPC();
  auto& ppc = power_pc.GetPPCState();
  auto& interp = m_core.m_system.GetInterpreter();
  auto& memory = m_core.m_system.GetMemory();
  u8* ram = m_core.m_guest.ram;
  const u32 ram_size = m_core.m_guest.ram_size;
  u8* const l1 = memory.GetL1Cache();
  const u32 l1_size = memory.GetL1CacheSize();
  u8* const vmem = memory.GetFakeVMEM();
  const u32 vmem_size = memory.GetFakeVMemSize();

  const s64 native_charge = -m_core.m_guest.downcount;
  if (native_charge <= 0)
  {
    ++m_ls_skipped_zero;
    return;
  }

  m_journal.ram_post.clear();
  for (const auto& [off, pre] : m_journal.ram_pre)
    m_journal.ram_post.emplace(off, off < ram_size ? ram[off] : pre);
  for (const auto& [off, pre] : m_journal.ram_pre)
    if (off < ram_size)
      ram[off] = pre;

  m_journal.lc_post.clear();
  if (l1)
  {
    for (const auto& [off, pre] : m_journal.lc_pre)
      m_journal.lc_post.emplace(off, off < l1_size ? l1[off] : pre);
    for (const auto& [off, pre] : m_journal.lc_pre)
      if (off < l1_size)
        l1[off] = pre;
  }

  m_journal.vmem_post.clear();
  if (vmem)
  {
    for (const auto& [off, pre] : m_journal.vmem_pre)
      m_journal.vmem_post.emplace(off, off < vmem_size ? vmem[off] : pre);
    for (const auto& [off, pre] : m_journal.vmem_pre)
      if (off < vmem_size)
        vmem[off] = pre;
  }

  const u64 saved_msr = ppc.msr.Hex;
  const int saved_downcount = ppc.downcount;
  const u32 saved_exceptions = ppc.Exceptions;
  LoadEntryRegsToPPC(entry_state);

  m_ls_read_index = 0;
  m_ls_read_overflow = false;
  StaticRecompLockstep::g_hw_write_sink = &StaticRecompLockstepVerifier::LsHwWriteTrampoline;
  StaticRecompLockstep::g_hw_write_sink_user = this;
  StaticRecompLockstep::g_hw_read_sink = &StaticRecompLockstepVerifier::LsHwReadTrampoline;
  StaticRecompLockstep::g_hw_read_sink_user = this;
  StaticRecompLockstep::g_ram_write_journal = &StaticRecompLockstepVerifier::LsShadowJournalTrampoline;
  StaticRecompLockstep::g_ram_write_journal_user = this;
  StaticRecompLockstep::g_lc_write_journal = &StaticRecompLockstepVerifier::LsShadowLcJournalTrampoline;
  StaticRecompLockstep::g_lc_write_journal_user = this;
  StaticRecompLockstep::g_vmem_write_journal = &StaticRecompLockstepVerifier::LsShadowVmemJournalTrampoline;
  StaticRecompLockstep::g_vmem_write_journal_user = this;
  StaticRecompLockstep::g_tb_override_active = true;
  StaticRecompLockstep::g_tb_override_value = entry_state.timebase;

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
  const bool end_is_loop_header = LsIsLoopHeader(ram, ram_size, end_pc);
  while (steps < m_ls_step_cap)
  {
    const u32 before = ppc.pc;
    interp_cycles += interp.SingleStepInner();
    ++steps;
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
    if (ppc.pc == end_pc && ppc.pc != before + 4)
    {
      bool is_boundary = true;
      if (end_is_loop_header && before < end_pc)
      {
        const u32 boff = before - 0x80000000u;
        const u32 binsn = (boff + 4u <= ram_size) ? Common::swap32(&ram[boff]) : 0u;
        const u32 opcd = binsn >> 26;
        const bool links = (opcd == 16u || opcd == 18u) && (binsn & 1u);
        const bool indirect = (opcd == 19u);
        const bool cross_chunk = m_core.ChunkIndexOf(before) != m_core.ChunkIndexOf(end_pc);
        if (!links && !indirect && !cross_chunk)
          is_boundary = false;
      }
      if (is_boundary)
        break;
    }
    if (interp_cycles >= native_charge)
    {
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
      addu(fmt::format("r{}", r), m_core.m_guest.gpr[r], ppc.gpr[r]);
    for (int r = 0; r < 32; ++r)
    {
      u64 n, i;
      std::memcpy(&n, &m_core.m_guest.fpr[r], sizeof(u64));
      std::memcpy(&i, &ppc.ps[r].ps0, sizeof(u64));
      addu(fmt::format("f{}", r), n, i);
      std::memcpy(&n, &m_core.m_guest.ps1[r], sizeof(u64));
      std::memcpy(&i, &ppc.ps[r].ps1, sizeof(u64));
      addu(fmt::format("ps1_{}", r), n, i);
    }
    addu("lr", m_core.m_guest.lr, ppc.spr[SPR_LR]);
    addu("ctr", m_core.m_guest.ctr, ppc.spr[SPR_CTR]);
    addu("cr", m_core.m_guest.cr, ppc.cr.Get());
    addu("xer", m_core.m_guest.xer, ppc.GetXER().Hex);
    addu("fpscr", m_core.m_guest.fpscr, ppc.fpscr.Hex);
    addu("msr", m_core.m_guest.msr, ppc.msr.Hex);
    addu("srr0", m_core.m_guest.srr0, ppc.spr[SPR_SRR0]);
    addu("srr1", m_core.m_guest.srr1, ppc.spr[SPR_SRR1]);
    addu("pc", m_core.m_guest.pc, ppc.pc);

    for (const auto& [off, post] : m_journal.ram_post)
    {
      const u8 iv = (off < ram_size) ? ram[off] : post;
      if (iv != post)
        diff += fmt::format(" mem[{:#010x}]:N={:#04x},I={:#04x}", 0x80000000u + off, post, iv);
    }

    if (l1)
    {
      for (const auto& [off, post] : m_journal.lc_post)
      {
        const u8 iv = (off < l1_size) ? l1[off] : post;
        if (iv != post)
          diff += fmt::format(" lc[{:#010x}]:N={:#04x},I={:#04x}", 0xE0000000u + off, post, iv);
      }
    }

    if (vmem)
    {
      for (const auto& [off, post] : m_journal.vmem_post)
      {
        const u8 iv = (off < vmem_size) ? vmem[off] : post;
        if (iv != post)
          diff += fmt::format(" vmem[{:#08x}]:N={:#04x},I={:#04x}", off, post, iv);
      }
    }

    const auto low = [](u32 v, u32 sz) { return sz >= 4 ? v : (v & ((1u << (sz * 8)) - 1)); };
    if (m_journal.native_mmio.size() != m_journal.interp_mmio.size())
    {
      diff += fmt::format(" mmio#:N={},I={}", m_journal.native_mmio.size(), m_journal.interp_mmio.size());
      for (const LsWrite& w : m_journal.native_mmio)
        diff += fmt::format(" N@{:#010x}/{}", w.addr, w.size);
      for (const LsWrite& w : m_journal.interp_mmio)
        diff += fmt::format(" I@{:#010x}/{}", w.addr, w.size);
    }
    else
    {
      for (size_t k = 0; k < m_journal.native_mmio.size(); ++k)
      {
        const LsWrite& a = m_journal.native_mmio[k];
        const LsWrite& b = m_journal.interp_mmio[k];
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

  for (const auto& [off, pre] : m_journal.ram_shadow_pre)
    if (off < ram_size)
      ram[off] = pre;
  for (const auto& [off, post] : m_journal.ram_post)
    if (off < ram_size)
      ram[off] = post;

  if (l1)
  {
    for (const auto& [off, pre] : m_journal.lc_shadow_pre)
      if (off < l1_size)
        l1[off] = pre;
    for (const auto& [off, post] : m_journal.lc_post)
      if (off < l1_size)
        l1[off] = post;
  }

  if (vmem)
  {
    for (const auto& [off, pre] : m_journal.vmem_shadow_pre)
      if (off < vmem_size)
        vmem[off] = pre;
    for (const auto& [off, post] : m_journal.vmem_post)
      if (off < vmem_size)
        vmem[off] = post;
  }

  ppc.msr.Hex = saved_msr;
  power_pc.MSRUpdated();
  ppc.downcount = saved_downcount;
  ppc.Exceptions = saved_exceptions;
  if (m_core.m_module->on_state_loaded)
    m_core.m_module->on_state_loaded(&m_core.m_guest);
}

}  // namespace StaticRecompLockstep
