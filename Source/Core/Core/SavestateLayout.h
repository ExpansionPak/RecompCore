// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

// Where savestates live, what they are called, and what order they are listed
// in. One definition, because more than one thing needs to agree about it: the
// emulator writes and lists them from its own menu, and a frontend launching a
// game offers the same set before boot. When those disagree the same states come
// back in a different order depending on where you look, which is a confusing
// bug to chase and an easy one to introduce by editing only one copy.
//
// Deliberately depends on nothing but the standard library. A frontend should be
// able to include this without linking any of Dolphin.

#include <algorithm>
#include <cstddef>
#include <ctime>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace State::Layout
{
namespace fs = std::filesystem;

// Savestates carry this extension. Dolphin's numbered slot saves (.s01 and
// friends) live in the same directory and are deliberately not matched: they are
// managed by slot, not by name, and listing them here would mix two schemes.
inline constexpr std::string_view EXTENSION = ".sav";

// States written on a timer or at a checkpoint are told apart from ones a player
// asked for by a filename prefix rather than by a separate directory, so a single
// listing pass returns both and the two cannot get out of step. A game may append
// whatever its own trigger is called -- room, chapter, checkpoint.
inline constexpr std::string_view AUTOMATIC_PREFIX = "recovery-";

// Prefix for a state the player asked for.
inline constexpr std::string_view MANUAL_PREFIX = "state-";

inline std::tm LocalTime(std::time_t when)
{
  std::tm out{};
#if defined(_WIN32)
  localtime_s(&out, &when);
#else
  localtime_r(&when, &out);
#endif
  return out;
}

// Named by wall clock rather than by slot, so repeated saves accumulate instead
// of overwriting one another, and so name order matches time order.
inline std::string TimestampedName(std::time_t when,
                                   std::string_view prefix = MANUAL_PREFIX)
{
  const std::tm local = LocalTime(when);
  char stamp[32] = {};
  std::strftime(stamp, sizeof(stamp), "%Y%m%d-%H%M%S", &local);
  return std::string(prefix) + stamp + std::string(EXTENSION);
}

// Newest first: the state wanted next is nearly always the one just written.
// Filename breaks ties so the order is stable when two states share a write
// time, which happens on filesystems with coarse timestamp granularity -- without
// it the same directory can list differently on consecutive reads.
inline bool NewestFirst(const fs::path& left, const fs::path& right)
{
  std::error_code left_ec;
  std::error_code right_ec;
  const auto left_time = fs::last_write_time(left, left_ec);
  const auto right_time = fs::last_write_time(right, right_ec);
  if (!left_ec && !right_ec && left_time != right_time)
    return left_time > right_time;
  return left.filename().string() < right.filename().string();
}

// A directory that is not there yields an empty list rather than throwing:
// callers ask before anything has been saved.
inline std::vector<fs::path> List(const fs::path& directory)
{
  std::vector<fs::path> paths;
  std::error_code ec;
  if (!fs::is_directory(directory, ec))
    return paths;

  for (const fs::directory_entry& entry : fs::directory_iterator(directory, ec))
  {
    if (entry.is_regular_file(ec) && entry.path().extension() == EXTENSION)
      paths.push_back(entry.path());
  }
  std::sort(paths.begin(), paths.end(), NewestFirst);
  return paths;
}

inline std::vector<fs::path> ListAutomatic(const fs::path& directory,
                                           std::string_view prefix = AUTOMATIC_PREFIX)
{
  std::vector<fs::path> paths;
  for (const fs::path& path : List(directory))
  {
    if (path.filename().string().starts_with(prefix))
      paths.push_back(path);
  }
  return paths;
}

inline std::optional<fs::path> LatestAutomatic(const fs::path& directory,
                                               std::string_view prefix = AUTOMATIC_PREFIX)
{
  const std::vector<fs::path> paths = ListAutomatic(directory, prefix);
  return paths.empty() ? std::nullopt : std::optional<fs::path>(paths.front());
}

// Keeps the newest `keep` automatic states and removes the rest, returning how
// many went. Only prefixed files are ever considered, so a player's own saves
// survive no matter how many automatic ones pile up.
inline std::size_t PruneAutomatic(const fs::path& directory, std::size_t keep,
                                  std::string_view prefix = AUTOMATIC_PREFIX)
{
  const std::vector<fs::path> automatic = ListAutomatic(directory, prefix);
  std::error_code ec;
  std::size_t removed = 0;
  for (std::size_t index = keep; index < automatic.size(); ++index)
  {
    ec.clear();
    if (fs::remove(automatic[index], ec))
      ++removed;
  }
  return removed;
}
}  // namespace State::Layout
