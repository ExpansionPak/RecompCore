// RecompCore: optional GX timing observation points for an embedder.
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <atomic>
#include <chrono>

#include "Common/CommonTypes.h"

// Cumulative nanoseconds per GX subsystem. Null unless an embedder installs
// them, so an unobserved build pays one predictable branch per scope.
struct VideoZoneObservers
{
  std::atomic<u64>* command_processor_ns = nullptr;
  std::atomic<u64>* vertex_loader_ns = nullptr;
  std::atomic<u64>* texture_decode_ns = nullptr;
  // Cumulative tallies for the same paths.
  std::atomic<u64>* draw_calls = nullptr;
  std::atomic<u64>* vertices_loaded = nullptr;
  std::atomic<u64>* texture_decodes = nullptr;
  std::atomic<u64>* shader_generation_ns = nullptr;
  std::atomic<u64>* shader_compilations = nullptr;
  std::atomic<u64>* efb_copies = nullptr;
};

inline const VideoZoneObservers* g_video_zone_observers = nullptr;

inline void SetVideoZoneObservers(const VideoZoneObservers* observers)
{
  g_video_zone_observers = observers;
}

inline const VideoZoneObservers* GetVideoZoneObservers()
{
  return g_video_zone_observers;
}

// Adds its lifetime to one accumulator. A null sink measures nothing.
class VideoZoneScope final
{
public:
  explicit VideoZoneScope(std::atomic<u64>* sink)
      : m_sink(sink), m_start(sink ? std::chrono::steady_clock::now()
                                   : std::chrono::steady_clock::time_point{})
  {
  }

  ~VideoZoneScope()
  {
    if (m_sink == nullptr)
      return;
    const auto elapsed = std::chrono::steady_clock::now() - m_start;
    m_sink->fetch_add(
        static_cast<u64>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed).count()),
        std::memory_order_relaxed);
  }

  VideoZoneScope(const VideoZoneScope&) = delete;
  VideoZoneScope& operator=(const VideoZoneScope&) = delete;

private:
  std::atomic<u64>* m_sink;
  std::chrono::steady_clock::time_point m_start;
};

// Picks one accumulator out of the installed set, or null when unobserved.
#define VIDEO_ZONE_SINK(field)                                                                     \
  (GetVideoZoneObservers() ? GetVideoZoneObservers()->field : nullptr)
