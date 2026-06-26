/*
 *  Copyright (C) 2005-2018 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#include "WinSystemGbmEGLContext.h"

#include "OptionalsReg.h"
#include "cores/VideoPlayer/DVDCodecs/DVDFactoryCodec.h"
#include "cores/VideoPlayer/DVDCodecs/Video/DVDVideoCodec.h"
#include "cores/VideoPlayer/VideoRenderers/RenderFactory.h"
#include "utils/log.h"

#include <unistd.h>

using namespace KODI::WINDOWING::GBM;
using namespace KODI::WINDOWING::LINUX;

bool CWinSystemGbmEGLContext::InitWindowSystemEGL(EGLint renderableType, EGLint apiType)
{
  if (!m_DRM && !CWinSystemGbm::InitWindowSystem())
  {
    return false;
  }

  if (!m_eglContext.CreatePlatformDisplay(m_GBM->GetDevice().Get(), m_GBM->GetDevice().Get()))
  {
    return false;
  }

  if (!m_eglContext.InitializeDisplay(apiType))
  {
    m_eglContext.Destroy();
    return false;
  }

  m_renderableType = renderableType;

  if (!ChooseEGLConfig(renderableType))
  {
    m_eglContext.Destroy();
    return false;
  }

  if (!CreateContext())
  {
    m_eglContext.Destroy();
    return false;
  }

  if (CEGLUtils::HasExtension(m_eglContext.GetEGLDisplay(), "EGL_ANDROID_native_fence_sync") &&
      CEGLUtils::HasExtension(m_eglContext.GetEGLDisplay(), "EGL_KHR_fence_sync"))
  {
    if (m_DRM->SupportsFencing())
    {
      m_eglFence = std::make_unique<KODI::UTILS::EGL::CEGLFence>(m_eglContext.GetEGLDisplay());
    }
    else
    {
      CLog::Log(LOGWARNING, "[GBM] EGL_KHR_fence_sync and EGL_ANDROID_native_fence_sync supported"
                            ", but DRM backend doesn't support fencing");
    }
  }
  else
  {
    CLog::Log(LOGWARNING, "[GBM] missing support for EGL_KHR_fence_sync and "
                          "EGL_ANDROID_native_fence_sync - performance may be impacted");
  }

  return true;
}

bool CWinSystemGbmEGLContext::ChooseEGLConfig(EGLint renderableType, int bitDepth)
{
  // Pick the highest active output format whose bpp is in range for the
  // requested bit depth. Policy: when bitDepth > 8, accept only entries
  // with bpp >= 10 and bpp <= bitDepth (do not silently fall back to 8).
  // When bitDepth <= 8, accept entries with bpp <= 8.
  auto outputformats = m_DRM->GetOutputFormats();

  std::vector<outputformat> candidates;
  std::ranges::copy_if(outputformats, std::back_inserter(candidates),
                       [bitDepth](const outputformat& format)
                       {
                         if (!format.active)
                           return false;
                         if (bitDepth > 8)
                           return format.bpp >= 10 && format.bpp <= bitDepth;
                         return format.bpp <= 8;
                       });
  std::ranges::sort(candidates, std::greater<>{}, &outputformat::bpp);

  for (const auto& format : candidates)
  {
    if (m_eglContext.ChooseConfig(renderableType, format.drm, false, format.alpha))
      return true;
  }

  return false;
}

bool CWinSystemGbmEGLContext::CreateNewWindow(const std::string& name,
                                              bool fullScreen,
                                              RESOLUTION_INFO& res)
{
  //Notify other subsystems that we change resolution
  OnLostDevice();

  if (!DestroyWindow())
  {
    return false;
  }

  if (!m_DRM->SetMode(res))
  {
    CLog::Log(LOGERROR, "CWinSystemGbmEGLContext::{} - failed to set DRM mode", __FUNCTION__);
    return false;
  }

  uint32_t format = m_eglContext.GetConfigAttrib(EGL_NATIVE_VISUAL_ID);
  std::vector<uint64_t> fallbackModifiers = {DRM_FORMAT_MOD_LINEAR};
  std::vector<uint64_t>* modifiers = &fallbackModifiers;

  for (auto& fmt : m_DRM->GetOutputFormats())
  {
    if (fmt.drm == format && fmt.active)
    {
      modifiers = &fmt.modifiers;
      break;
    }
  }

  if (!m_GBM->GetDevice().CreateSurface(res.iWidth, res.iHeight, format, modifiers->data(),
                                        modifiers->size()))
  {
    CLog::Log(LOGERROR, "CWinSystemGbmEGLContext::{} - failed to initialize GBM", __FUNCTION__);
    return false;
  }

  // This check + the reinterpret cast is for security reason, if the user has outdated platform header files which often is the case
  static_assert(sizeof(EGLNativeWindowType) == sizeof(gbm_surface*),
                "Declaration specifier differs in size");

  if (!m_eglContext.CreatePlatformSurface(
          m_GBM->GetDevice().GetSurface().Get(),
          reinterpret_cast<khronos_uintptr_t>(m_GBM->GetDevice().GetSurface().Get())))
  {
    return false;
  }

  if (!m_eglContext.BindContext())
  {
    return false;
  }

  if (!m_eglContext.TrySwapBuffers())
  {
    return false;
  }

  struct gbm_bo* bo = m_GBM->GetDevice().GetSurface().LockFrontBuffer().Get();
  if (!bo)
  {
    CLog::Log(LOGERROR, "CWinSystemGbmEGLContext::{} - failed to lock front buffer", __FUNCTION__);
    return false;
  }

#if defined(HAS_GBM_MODIFIERS)
  uint64_t modifier = gbm_bo_get_modifier(bo);
#else
  uint64_t modifier = DRM_FORMAT_MOD_LINEAR;
#endif
  // Offscreen has no CRTC and no scanout planes; FindGuiPlane has nothing to
  // assign, so skip the call rather than violate its post-condition that
  // m_gui_plane is set on success.
  if (m_DRM->GetCrtc() && !m_DRM->FindGuiPlane(gbm_bo_get_format(bo), modifier))
  {
    return false;
  }
  m_bFullScreen = fullScreen;
  m_nWidth = res.iWidth;
  m_nHeight = res.iHeight;
  m_fRefreshRate = res.fRefreshRate;
  CLog::Log(LOGDEBUG, "CWinSystemGbmEGLContext::{} - initialized GBM", __FUNCTION__);
  return true;
}

bool CWinSystemGbmEGLContext::DestroyWindow()
{
  m_eglContext.DestroySurface();

  // Release the deferred out-fence that would otherwise be consumed (and
  // closed) by the next frame's FencePreSwap(), which will not run now.
  if (m_kmsFenceFd != -1)
  {
    close(m_kmsFenceFd);
    m_kmsFenceFd = -1;
  }

  CLog::Log(LOGDEBUG, "CWinSystemGbmEGLContext::{} - deinitialized GBM", __FUNCTION__);
  return true;
}

void CWinSystemGbmEGLContext::FencePreSwap()
{
#if defined(EGL_ANDROID_native_fence_sync) && defined(EGL_KHR_fence_sync)
  if (!m_eglFence)
    return;

  // out-fence of the previous commit (call it frame N-1).
  int fd = m_DRM->TakeOutFenceFd();

  // GPU side: gate this frame's render on the flip *before* last (N-2), not the
  // previous flip. N-2's out-fence guards the buffer mesa is about to recycle
  // as this frame's render target; gating on the previous flip instead pinned
  // every render to a vblank boundary, held the pipeline one frame deep, and on
  // a heavier-than-trivial composite lost a vblank intermittently - capping
  // throughput (~40fps on a 60Hz panel) with the GPU idle-waiting. Gating on
  // N-2 lets the GPU render this frame while frame N-1 is still pending its
  // flip. It stays safe: the recycled buffer is older than N-2, and KMS still
  // waits for this render via IN_FENCE_FD (set in FencePostSwap).
  if (m_kmsFenceFd != -1)
    m_eglFence->WaitSyncGPU(m_kmsFenceFd); // consumes (closes) the N-2 fd
  m_kmsFenceFd = (fd != -1) ? dup(fd) : -1; // keep a copy of N-1 for next frame's GPU gate

  // CPU side: WaitSyncCPU (in FencePostSwap) blocks on this same N-1 out-fence,
  // i.e. until the previous flip actually lands. That is the back-pressure that
  // paces the GUI loop to the display refresh; gating the GPU on N-2 while the
  // CPU waits on N-1 is what caps fps at refresh *and* keeps one frame of slack
  // to absorb a long composite. The wait overlaps this frame's render, so it
  // does not reintroduce the stall.
  if (fd != -1)
    m_eglFence->CreateKMSFence(fd); // CPU fence = N-1; consumed by WaitSyncCPU

  m_eglFence->CreateGPUFence();
#endif
}

void CWinSystemGbmEGLContext::FencePostSwap()
{
#if defined(EGL_ANDROID_native_fence_sync) && defined(EGL_KHR_fence_sync)
  if (!m_eglFence)
    return;

  // Hand this frame's GPU render fence to KMS as IN_FENCE_FD so the kernel
  // defers scanout until the render completes (tear-free).
  int fd = m_eglFence->FlushFence();
  m_DRM->SetInFenceFd(fd);

  // CPU back-pressure: block until the previous flip (the N-1 out-fence wrapped
  // in FencePreSwap) lands, pacing the GUI loop to the display refresh. This
  // overlaps the render just submitted, so it caps fps at refresh without
  // stalling. See WaitSyncCPU().
  m_eglFence->WaitSyncCPU();
#endif
}

bool CWinSystemGbmEGLContext::SetVideoOutput(const VideoPicture* videoPicture)
{
  // Override: the base only flips the gui/video plane role. On EGL
  // backends we additionally rebuild the GBM and EGL output surface
  // to a wider format (e.g. AR24 -> AR30) when the video needs more
  // than 8-bit through the single output plane, then chain to the
  // base so plane-role tracking sees the new format. EGL context survives
  // across the rebuild, no shader-rebuild. The next flip forces a modeset
  // when needed (amdgpu), on other drivers it is a fast flip."
  const int bitDepth = videoPicture ? videoPicture->colorBits : 8;

  // Pick an EGL config matching the target bit depth, then rebuild
  // the surface only if that pick changed the native visual ID.
  uint32_t currentFormat = m_eglContext.GetConfigAttrib(EGL_NATIVE_VISUAL_ID);

  if (!ChooseEGLConfig(m_renderableType, bitDepth))
  {
    CLog::LogF(LOGERROR, "failed to choose EGL config for {}-bit", bitDepth);
    return false;
  }

  uint32_t format = m_eglContext.GetConfigAttrib(EGL_NATIVE_VISUAL_ID);

  if (format != currentFormat)
  {
    m_eglContext.DestroySurface();

    std::vector<uint64_t> fallbackModifiers = {DRM_FORMAT_MOD_LINEAR};
    std::vector<uint64_t>* modifiers = &fallbackModifiers;

    for (auto& fmt : m_DRM->GetOutputFormats())
    {
      if (fmt.drm == format && fmt.active)
      {
        modifiers = &fmt.modifiers;
        break;
      }
    }

    if (!m_GBM->GetDevice().CreateSurface(m_nWidth, m_nHeight, format, modifiers->data(),
                                          modifiers->size()))
    {
      CLog::LogF(LOGERROR, "failed to create GBM surface");
      return false;
    }

    if (!m_eglContext.CreatePlatformSurface(
            m_GBM->GetDevice().GetSurface().Get(),
            reinterpret_cast<khronos_uintptr_t>(m_GBM->GetDevice().GetSurface().Get())))
    {
      return false;
    }

    if (!m_eglContext.BindContext())
    {
      return false;
    }

    if (!m_eglContext.TrySwapBuffers())
    {
      return false;
    }

    CLog::LogF(LOGINFO, "Output surface recreated at {}-bit", bitDepth);

    // amdgpu disables the CRTC when RMFB removes a still-bound FB during
    // surface destroy (primary-plane invariant). Force a full modeset on
    // the next commit to re-enable the CRTC. No-op on drivers without the
    // quirk (Intel, RPi, etc.) so they keep the fast-flip path.
    if (m_DRM->HasQuirk(KODI::WINDOWING::GBM::QUIRK_NEEDSPRIMARY))
      m_DRM->SetActive(true);

    // The chained base reads the active plane's cached format and
    // modifier to decide which DRM plane satisfies the new role.
    // After a surface rebuild that cache is stale, so refresh it from
    // the new GBM front buffer (made available by TrySwapBuffers above)
    // before chaining.
    if (auto* plane = m_DRM->GetGuiPlane() ? m_DRM->GetGuiPlane() : m_DRM->GetVideoPlane())
    {
      if (struct gbm_bo* bo = m_GBM->GetDevice().GetSurface().LockFrontBuffer().Get())
      {
        plane->SetFormat(gbm_bo_get_format(bo));
#if defined(HAS_GBM_MODIFIERS)
        plane->SetModifier(gbm_bo_get_modifier(bo));
#else
        plane->SetModifier(DRM_FORMAT_MOD_LINEAR);
#endif
      }
    }
  }

  // Chain to base for the fine-plane part: routes through FindVideoPlane
  // if videoPicture set or FindGuiPlane if videoPicture was nullptr, using
  // the now-current cached format/modifier on the active plane.
  return CWinSystemGbm::SetVideoOutput(videoPicture);
}

bool CWinSystemGbmEGLContext::DestroyWindowSystem()
{
  CDVDFactoryCodec::ClearHWAccels();
  VIDEOPLAYER::CRendererFactory::ClearRenderer();
  m_eglContext.Destroy();

  return CWinSystemGbm::DestroyWindowSystem();
}

void CWinSystemGbmEGLContext::delete_CVaapiProxy::operator()(CVaapiProxy* p) const
{
  VaapiProxyDelete(p);
}

bool CWinSystemGbmEGLContext::BindTextureUploadContext()
{
  return m_eglContext.BindTextureUploadContext();
}

bool CWinSystemGbmEGLContext::UnbindTextureUploadContext()
{
  return m_eglContext.UnbindTextureUploadContext();
}

bool CWinSystemGbmEGLContext::HasContext()
{
  return m_eglContext.HasContext();
}
