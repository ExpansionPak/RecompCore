// RecompCore: StaticRecomp CPU core - State synchronization.
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/PowerPC/StaticRecomp/StaticRecompCore.h"
#include "Core/System.h"
#include "Core/PowerPC/PowerPC.h"
#include "Core/HW/SystemTimers.h"
#include <cstring>

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
