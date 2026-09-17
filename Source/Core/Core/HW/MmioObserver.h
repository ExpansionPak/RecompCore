// RecompCore: optional MMIO tallies for an embedder.
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <atomic>

#include "Common/CommonTypes.h"

// MMIO is far hotter than the GX paths, so these stay null unless an embedder
// asks for them; an unobserved build pays one predictable branch per access.
struct MmioObservers
{
  std::atomic<u64>* reads = nullptr;
  std::atomic<u64>* writes = nullptr;
};

inline const MmioObservers* g_mmio_observers = nullptr;

inline void SetMmioObservers(const MmioObservers* observers)
{
  g_mmio_observers = observers;
}

inline const MmioObservers* GetMmioObservers()
{
  return g_mmio_observers;
}
