// Copyright 2026 ModernGekko Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "DolphinNoGUI/Platform.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <poll.h>
#include <thread>

#include <wayland-client.h>
#include "xdg-decoration-client-protocol.h"
#include "xdg-shell-client-protocol.h"

#include "Core/Config/MainSettings.h"
#include "Core/Core.h"
#include "Core/System.h"
#include "VideoCommon/Present.h"

namespace
{
class PlatformWayland final : public Platform
{
public:
  ~PlatformWayland() override;

  bool Init() override;
  void SetTitle(const std::string& title) override;
  void MainLoop() override;
  WindowSystemInfo GetWindowSystemInfo() const override;

  static void RegistryGlobal(void* data, wl_registry* registry, uint32_t name,
                             const char* interface, uint32_t version);
  static void RegistryGlobalRemove(void*, wl_registry*, uint32_t) {}
  static void WmBasePing(void*, xdg_wm_base* wm_base, uint32_t serial);
  static void SurfaceConfigure(void* data, xdg_surface* surface, uint32_t serial);
  static void ToplevelConfigure(void* data, xdg_toplevel*, int32_t width, int32_t height,
                                wl_array*);
  static void ToplevelClose(void* data, xdg_toplevel*);
  static void ToplevelConfigureBounds(void*, xdg_toplevel*, int32_t, int32_t) {}
  static void ToplevelWmCapabilities(void*, xdg_toplevel*, wl_array*) {}

private:
  wl_display* m_display = nullptr;
  wl_registry* m_registry = nullptr;
  wl_compositor* m_compositor = nullptr;
  xdg_wm_base* m_wm_base = nullptr;
  zxdg_decoration_manager_v1* m_decoration_manager = nullptr;
  wl_surface* m_surface = nullptr;
  xdg_surface* m_xdg_surface = nullptr;
  xdg_toplevel* m_toplevel = nullptr;
  zxdg_toplevel_decoration_v1* m_toplevel_decoration = nullptr;
  bool m_configured = false;
  int32_t m_width = Config::Get(Config::MAIN_RENDER_WINDOW_WIDTH);
  int32_t m_height = Config::Get(Config::MAIN_RENDER_WINDOW_HEIGHT);
};

constexpr wl_registry_listener s_registry_listener = {PlatformWayland::RegistryGlobal,
                                                       PlatformWayland::RegistryGlobalRemove};
constexpr xdg_wm_base_listener s_wm_base_listener = {PlatformWayland::WmBasePing};
constexpr xdg_surface_listener s_surface_listener = {PlatformWayland::SurfaceConfigure};
constexpr xdg_toplevel_listener s_toplevel_listener = {
    PlatformWayland::ToplevelConfigure, PlatformWayland::ToplevelClose,
    PlatformWayland::ToplevelConfigureBounds, PlatformWayland::ToplevelWmCapabilities};

PlatformWayland::~PlatformWayland()
{
  if (m_toplevel_decoration)
    zxdg_toplevel_decoration_v1_destroy(m_toplevel_decoration);
  if (m_toplevel)
    xdg_toplevel_destroy(m_toplevel);
  if (m_xdg_surface)
    xdg_surface_destroy(m_xdg_surface);
  if (m_surface)
    wl_surface_destroy(m_surface);
  if (m_wm_base)
    xdg_wm_base_destroy(m_wm_base);
  if (m_decoration_manager)
    zxdg_decoration_manager_v1_destroy(m_decoration_manager);
  if (m_compositor)
    wl_compositor_destroy(m_compositor);
  if (m_registry)
    wl_registry_destroy(m_registry);
  if (m_display)
    wl_display_disconnect(m_display);
}

bool PlatformWayland::Init()
{
  m_display = wl_display_connect(nullptr);
  if (!m_display)
    return false;

  m_registry = wl_display_get_registry(m_display);
  wl_registry_add_listener(m_registry, &s_registry_listener, this);
  if (wl_display_roundtrip(m_display) < 0 || !m_compositor || !m_wm_base)
    return false;

  xdg_wm_base_add_listener(m_wm_base, &s_wm_base_listener, this);
  m_surface = wl_compositor_create_surface(m_compositor);
  if (!m_surface)
    return false;

  m_xdg_surface = xdg_wm_base_get_xdg_surface(m_wm_base, m_surface);
  if (!m_xdg_surface)
    return false;

  m_toplevel = xdg_surface_get_toplevel(m_xdg_surface);
  if (!m_toplevel)
    return false;

  if (m_decoration_manager)
  {
    m_toplevel_decoration =
        zxdg_decoration_manager_v1_get_toplevel_decoration(m_decoration_manager, m_toplevel);
    if (m_toplevel_decoration)
    {
      zxdg_toplevel_decoration_v1_set_mode(
          m_toplevel_decoration, ZXDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE);
    }
  }

  xdg_surface_add_listener(m_xdg_surface, &s_surface_listener, this);
  xdg_toplevel_add_listener(m_toplevel, &s_toplevel_listener, this);
  xdg_toplevel_set_app_id(m_toplevel, "org.moderngekko.Runner");
  xdg_toplevel_set_title(m_toplevel, "ModernGekko");
  xdg_surface_set_window_geometry(m_xdg_surface, 0, 0, m_width, m_height);
  if (Config::Get(Config::MAIN_FULLSCREEN))
  {
    xdg_toplevel_set_fullscreen(m_toplevel, nullptr);
    m_window_fullscreen = true;
  }
  wl_surface_commit(m_surface);

  while (!m_configured)
  {
    if (wl_display_dispatch(m_display) < 0)
      return false;
  }
  return true;
}

void PlatformWayland::SetTitle(const std::string& title)
{
  if (m_toplevel)
  {
    xdg_toplevel_set_title(m_toplevel, title.c_str());
    wl_surface_commit(m_surface);
    wl_display_flush(m_display);
  }
}

void PlatformWayland::MainLoop()
{
  pollfd display_fd{wl_display_get_fd(m_display), POLLIN, 0};
  while (IsRunning())
  {
    UpdateRunningFlag();
    Core::HostDispatchJobs(Core::System::GetInstance());
    if (wl_display_dispatch_pending(m_display) < 0)
      break;
    wl_display_flush(m_display);
    display_fd.revents = 0;
    if (poll(&display_fd, 1, 1) > 0 && (display_fd.revents & POLLIN) &&
        wl_display_dispatch(m_display) < 0)
    {
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
}

WindowSystemInfo PlatformWayland::GetWindowSystemInfo() const
{
  return {WindowSystemType::Wayland, m_display, m_surface, m_surface};
}

void PlatformWayland::RegistryGlobal(void* data, wl_registry* registry, uint32_t name,
                                     const char* interface, uint32_t version)
{
  auto* platform = static_cast<PlatformWayland*>(data);
  if (std::strcmp(interface, wl_compositor_interface.name) == 0)
  {
    platform->m_compositor = static_cast<wl_compositor*>(
        wl_registry_bind(registry, name, &wl_compositor_interface, std::min(version, 4u)));
  }
  else if (std::strcmp(interface, xdg_wm_base_interface.name) == 0)
  {
    platform->m_wm_base = static_cast<xdg_wm_base*>(
        wl_registry_bind(registry, name, &xdg_wm_base_interface, std::min(version, 6u)));
  }
  else if (std::strcmp(interface, zxdg_decoration_manager_v1_interface.name) == 0)
  {
    platform->m_decoration_manager = static_cast<zxdg_decoration_manager_v1*>(wl_registry_bind(
        registry, name, &zxdg_decoration_manager_v1_interface, std::min(version, 1u)));
  }
}

void PlatformWayland::WmBasePing(void*, xdg_wm_base* wm_base, uint32_t serial)
{
  xdg_wm_base_pong(wm_base, serial);
}

void PlatformWayland::SurfaceConfigure(void* data, xdg_surface* surface, uint32_t serial)
{
  auto* platform = static_cast<PlatformWayland*>(data);
  xdg_surface_ack_configure(surface, serial);
  platform->m_configured = true;
}

void PlatformWayland::ToplevelConfigure(void* data, xdg_toplevel*, int32_t width, int32_t height,
                                        wl_array*)
{
  auto* platform = static_cast<PlatformWayland*>(data);
  if (width <= 0 || height <= 0 || (width == platform->m_width && height == platform->m_height))
    return;

  platform->m_width = width;
  platform->m_height = height;
  xdg_surface_set_window_geometry(platform->m_xdg_surface, 0, 0, width, height);
  if (g_presenter)
    g_presenter->ResizeSurface();
}

void PlatformWayland::ToplevelClose(void* data, xdg_toplevel*)
{
  static_cast<PlatformWayland*>(data)->Stop();
}
}  // namespace

std::unique_ptr<Platform> Platform::CreateWaylandPlatform()
{
  return std::make_unique<PlatformWayland>();
}
