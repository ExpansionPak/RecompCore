// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <memory>
#include <optional>
#include <vector>

#include "Common/CommonTypes.h"

namespace Memory
{
struct RelocationAlias
{
  u32 linked_start;
  u32 runtime_start;
  u32 size;
};

struct ResolvedRelocationAlias
{
  u32 address;
  u32 remaining;
};

class RelocationAliases
{
public:
  void Publish(std::vector<RelocationAlias> aliases);
  std::optional<ResolvedRelocationAlias> Resolve(u32 physical_address) const;

private:
  std::shared_ptr<const std::vector<RelocationAlias>> m_aliases;
};
}  // namespace Memory
