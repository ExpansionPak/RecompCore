// RecompCore: optional profiling observation points for an embedder.
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <atomic>

#include "Common/CommonTypes.h"

// The core already samples its own guest PC and knows how long it spends
// executing guest code, but had no way to hand either to an embedder: the
// dispatch histogram in StaticRecompCore was only ever printed to stderr at
// shutdown. An embedder installs these pointers to observe both live.
//
// Both are null unless installed, so an unobserved core pays one predictable
// null check per timing slice and per sampled dispatch -- never per dispatch.
struct StaticRecompObservers
{
  // Live guest PC, published at the core's existing dispatch sample cadence.
  std::atomic<u32>* guest_pc = nullptr;
  // Cumulative nanoseconds spent executing guest code, summed per timing slice.
  std::atomic<u64>* guest_cpu_ns = nullptr;
  // Cumulative core tallies, republished per timing slice.
  std::atomic<u64>* dispatches = nullptr;
  std::atomic<u64>* interpreter_fallbacks = nullptr;
  std::atomic<u64>* exceptions = nullptr;
};

inline const StaticRecompObservers* g_static_recomp_observers = nullptr;

inline void SetStaticRecompObservers(const StaticRecompObservers* observers)
{
  g_static_recomp_observers = observers;
}

inline const StaticRecompObservers* GetStaticRecompObservers()
{
  return g_static_recomp_observers;
}
