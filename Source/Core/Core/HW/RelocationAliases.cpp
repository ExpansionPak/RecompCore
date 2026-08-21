// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/HW/RelocationAliases.h"

#include <atomic>
#include <cstdint>
#include <utility>

namespace Memory
{
namespace
{
constexpr u32 PHYSICAL_ADDRESS_MASK = 0x3fffffffu;
}

void RelocationAliases::Publish(std::vector<RelocationAlias> aliases)
{
  auto snapshot = std::make_shared<const std::vector<RelocationAlias>>(std::move(aliases));
  std::atomic_store_explicit(&m_aliases, std::move(snapshot), std::memory_order_release);
}

std::optional<ResolvedRelocationAlias> RelocationAliases::Resolve(u32 physical_address) const
{
  const auto snapshot = std::atomic_load_explicit(&m_aliases, std::memory_order_acquire);
  if (!snapshot)
    return std::nullopt;

  for (const RelocationAlias& alias : *snapshot)
  {
    const u32 linked = alias.linked_start & PHYSICAL_ADDRESS_MASK;
    const u64 delta = static_cast<u64>(physical_address) - linked;
    if (physical_address < linked || delta >= alias.size)
      continue;

    const u64 runtime = static_cast<u64>(alias.runtime_start & PHYSICAL_ADDRESS_MASK) + delta;
    if (runtime > UINT32_MAX)
      continue;
    return ResolvedRelocationAlias{static_cast<u32>(runtime),
                                   alias.size - static_cast<u32>(delta)};
  }
  return std::nullopt;
}
}  // namespace Memory
