// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <algorithm>
#include <atomic>
#include <cmath>
#include <utility>

namespace AnimaniacsPC
{
inline std::atomic<bool> s_overlay_visible{false};
inline std::atomic<bool> s_widescreen{true};
inline std::atomic<float> s_aspect_override{0.0f};
inline std::atomic<float> s_host_aspect{16.0f / 9.0f};
inline std::atomic<int> s_surface_width{0};
inline std::atomic<int> s_surface_height{0};
inline std::atomic<float> s_fov_degrees{0.0f};
inline std::atomic<int> s_fps_target{60};

inline bool OverlayVisible()
{
  return s_overlay_visible.load(std::memory_order_relaxed);
}

inline void ToggleOverlay()
{
  s_overlay_visible.store(!OverlayVisible(), std::memory_order_relaxed);
}

inline void SetOverlayVisible(bool visible)
{
  s_overlay_visible.store(visible, std::memory_order_relaxed);
}

inline bool WidescreenEnabled()
{
  return s_widescreen.load(std::memory_order_relaxed);
}

inline void SetWidescreenEnabled(bool enabled)
{
  s_widescreen.store(enabled, std::memory_order_relaxed);
}

inline void SetHostAspect(float aspect)
{
  if (std::isfinite(aspect) && aspect >= 1.0f && aspect <= 4.0f)
    s_host_aspect.store(aspect, std::memory_order_relaxed);
}

inline void SetHostSurfaceSize(int width, int height)
{
  if (width <= 0 || height <= 0)
    return;

  s_surface_width.store(width, std::memory_order_relaxed);
  s_surface_height.store(height, std::memory_order_relaxed);
  SetHostAspect(static_cast<float>(width) / static_cast<float>(height));
}

inline int HostSurfaceWidth()
{
  return s_surface_width.load(std::memory_order_relaxed);
}

inline int HostSurfaceHeight()
{
  return s_surface_height.load(std::memory_order_relaxed);
}

inline float HostAspect()
{
  return s_host_aspect.load(std::memory_order_relaxed);
}

inline void SetAspectOverride(float aspect)
{
  if (!std::isfinite(aspect) || aspect < 1.0f || aspect > 4.0f)
    aspect = 0.0f;
  s_aspect_override.store(aspect, std::memory_order_relaxed);
}

inline float AspectOverride()
{
  return s_aspect_override.load(std::memory_order_relaxed);
}

inline float TargetAspect()
{
  if (!WidescreenEnabled())
    return 4.0f / 3.0f;

  const float fixed = AspectOverride();
  return std::clamp(fixed > 0.0f ? fixed : HostAspect(), 1.0f, 4.0f);
}

inline std::pair<float, float> GuestAspectPair()
{
  if (!WidescreenEnabled())
    return {4.0f, 3.0f};

  const float aspect = TargetAspect();
  const auto near = [aspect](float value) { return std::fabs(aspect - value) < 0.01f; };

  if (near(16.0f / 9.0f))
    return {16.0f, 9.0f};
  if (near(16.0f / 10.0f))
    return {16.0f, 10.0f};
  if (near(21.0f / 9.0f))
    return {21.0f, 9.0f};
  if (near(32.0f / 9.0f))
    return {32.0f, 9.0f};

  return {aspect * 9.0f, 9.0f};
}

inline float FovDegrees()
{
  return s_fov_degrees.load(std::memory_order_relaxed);
}

inline void SetFovDegrees(float fov)
{
  if (!std::isfinite(fov) || fov < 45.0f || fov > 140.0f)
    fov = 0.0f;
  s_fov_degrees.store(fov, std::memory_order_relaxed);
}

inline int FpsTarget()
{
  return s_fps_target.load(std::memory_order_relaxed);
}

inline void SetFpsTarget(int fps)
{
  s_fps_target.store(std::clamp(fps, 60, 360), std::memory_order_relaxed);
}

}  // namespace AnimaniacsPC
