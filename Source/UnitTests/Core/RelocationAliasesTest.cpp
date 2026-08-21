// SPDX-License-Identifier: GPL-2.0-or-later

#include <gtest/gtest.h>

#include "Core/HW/RelocationAliases.h"

TEST(RelocationAliases, ResolvesPhysicalDeviceAddresses)
{
  Memory::RelocationAliases aliases;
  aliases.Publish({{0x81e74040, 0x805fb080, 0x000ba6f4}});

  const auto texture = aliases.Resolve(0x01e8e760);
  ASSERT_TRUE(texture);
  EXPECT_EQ(texture->address, 0x006157a0u);
  EXPECT_EQ(texture->remaining, 0x0009ffd4u);

  const auto dvd = aliases.Resolve(0x01f04060);
  ASSERT_TRUE(dvd);
  EXPECT_EQ(dvd->address, 0x0068b0a0u);
  EXPECT_GE(dvd->remaining, 0x800u);
}

TEST(RelocationAliases, RejectsOutsideAndClearedRanges)
{
  Memory::RelocationAliases aliases;
  aliases.Publish({{0x81e74040, 0x805fb080, 0x000ba6f4}});
  EXPECT_FALSE(aliases.Resolve(0x01e7403f));
  EXPECT_FALSE(aliases.Resolve(0x01f2e734));

  aliases.Publish({});
  EXPECT_FALSE(aliases.Resolve(0x01e8e760));
}
