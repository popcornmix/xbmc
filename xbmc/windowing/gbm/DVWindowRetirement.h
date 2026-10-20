/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

#include "cores/VideoPlayer/VideoRenderers/DVBridgeGLES.h"
#include "utils/EGLUtils.h"
#include <functional>
#include <memory>

namespace KODI::WINDOWING::GBM
{
// The same matching display/context/device/surfaces survive implicit window teardown.
// One reachable process-lifetime slot also bounds WAIT_FAILED quarantine.
class CDVWindowRetirement
{
public:
  struct Resources
  {
    std::shared_ptr<void> gbm, drm, vaapi, fence, properties;
    std::function<bool()> completeDisplay;
  };
  static bool Occupied() { return m_retained != nullptr; }
  static bool Keep(std::shared_ptr<CDVBridgeGLES::Context> context,
      CEGLContextUtils::RetainedContext egl, Resources resources)
  {
    if (Occupied() || !context) return false;
    m_retained = new CDVWindowRetirement(std::move(context),egl,std::move(resources));
    CDVBridgeGLES::Context::NativeWindowRetained(true);
    return true;
  }
  // One later render-thread callback, with timeout zero. No wait or reclaim loop.
  static bool PollOnce()
  {
    if (!Occupied()) return true;
    auto& held = *m_retained;
    if (held.m_quarantined) return false;
    if (held.m_resources.completeDisplay && !held.m_resources.completeDisplay()) return false;
    const auto display = eglGetCurrentDisplay();
    const auto context = eglGetCurrentContext();
    const auto draw = eglGetCurrentSurface(EGL_DRAW);
    const auto read = eglGetCurrentSurface(EGL_READ);
    if (!held.m_gpuClosed)
    {
      if (!eglMakeCurrent(held.m_egl.display,held.m_egl.surface,held.m_egl.surface,held.m_egl.context))
      {
        held.m_quarantined = true;
        return false;
      }
      const auto status = held.m_context->Poll();
      if (status != DVBRIDGE_DV_IDLE)
      {
        held.m_quarantined = status == DVBRIDGE_DV_FAILED;
        if (display != EGL_NO_DISPLAY && context != held.m_egl.context)
          eglMakeCurrent(display,draw,read,context);
        return false;
      }
      if (!held.m_context->Close()) return false;
      held.m_gpuClosed = true;
      held.m_resources.vaapi.reset();
      held.m_resources.fence.reset();
    }
    const bool destroyed = CEGLContextUtils::DestroyRetiredContext(held.m_egl);
    if (!destroyed)
    {
      held.m_quarantined = true;
      return false;
    }
    const bool restore = display == EGL_NO_DISPLAY || context == EGL_NO_CONTEXT ||
        context == held.m_originalContext || eglMakeCurrent(display,draw,read,context);
    held.m_resources.properties.reset();
    held.m_resources.gbm.reset();
    held.m_resources.drm.reset();
    delete m_retained;
    m_retained = nullptr;
    CDVBridgeGLES::Context::NativeWindowRetained(false);
    return restore;
  }
private:
  CDVWindowRetirement(std::shared_ptr<CDVBridgeGLES::Context> context,
      CEGLContextUtils::RetainedContext egl, Resources resources)
    : m_context(std::move(context)), m_egl(egl), m_resources(std::move(resources)),
      m_originalContext(egl.context) {}
  inline static CDVWindowRetirement* m_retained{};
  std::shared_ptr<CDVBridgeGLES::Context> m_context;
  CEGLContextUtils::RetainedContext m_egl;
  Resources m_resources;
  EGLContext m_originalContext;
  bool m_gpuClosed{}, m_quarantined{};
};
}
