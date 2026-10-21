/*
 *  Copyright (C) 2005-2018 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#include "WinSystemGbmGLESContext.h"
#include "ServiceBroker.h"
#include "utils/DVBridgeDiagnosticLog.h"
#include "cores/DataCacheCore.h"
#include "cores/VideoSourceSideData.h"
#ifdef HAVE_DVBRIDGE
#include "cores/VideoPlayer/DVDCodecs/Video/DVPlaybackNotification.h"
#endif

#include "OptionalsReg.h"
#include "cores/RetroPlayer/process/gbm/RPProcessInfoGbm.h"
#include "cores/RetroPlayer/rendering/VideoRenderers/RPRendererDMAOpenGLES.h"
#include "cores/RetroPlayer/rendering/VideoRenderers/RPRendererFBO.h"
#include "cores/RetroPlayer/rendering/VideoRenderers/RPRendererOpenGLES.h"
#include "cores/VideoPlayer/DVDCodecs/DVDFactoryCodec.h"
#include "cores/VideoPlayer/DVDCodecs/Video/DVDVideoCodecDRMPRIME.h"
#include "cores/VideoPlayer/Process/gbm/ProcessInfoGBM.h"
#include "cores/VideoPlayer/VideoRenderers/HwDecRender/RendererDRMPRIME.h"
#include "cores/VideoPlayer/VideoRenderers/HwDecRender/RendererDRMPRIMEGLES.h"
#ifdef HAVE_DVBRIDGE
#include "cores/VideoPlayer/VideoRenderers/HwDecRender/RendererDRMPRIMEDV.h"
#endif
#include "cores/VideoPlayer/VideoRenderers/LinuxRendererGLES.h"
#include "cores/VideoPlayer/VideoRenderers/RenderFactory.h"
#include "rendering/gles/GuiCompositeShaderGLES.h"
#include "rendering/gles/ScreenshotSurfaceGLES.h"

extern "C"
{
#include <libavutil/pixfmt.h>
#ifdef HAVE_DVBRIDGE
#include "dvbridge_placebo.h"
#endif
}
#include "utils/BufferObjectFactory.h"
#include "utils/DMAHeapBufferObject.h"
#include "utils/DumbBufferObject.h"
#include "utils/GBMBufferObject.h"
#include "utils/UDMABufferObject.h"
#include "utils/XTimeUtils.h"
#include "utils/log.h"
#include "windowing/WindowSystemFactory.h"
#include "windowing/GraphicContext.h"
#include "drm/DRMAtomic.h"
#include "settings/DisplaySettings.h"

#include <mutex>
#include <algorithm>
#include <cmath>
#include <ctime>
#include <chrono>
#include <thread>
#include <cstdlib>
#include <bit>
#include <cstring>
#include <unistd.h>
#ifdef HAVE_DVBRIDGE
#include "DVBridgeCapture.h"
#include "application/ApplicationComponents.h"
#include "application/ApplicationPlayer.h"
#include "rendering/capture/CaptureService.h"
#include "cores/VideoPlayer/DVDCodecs/Video/DVPlaybackPolicy.h"
#include "cores/VideoPlayer/DVDCodecs/Video/DVOutputSettings.h"
#include "cores/VideoPlayer/DVDCodecs/Video/DVStreamTelemetry.h"
#include "cores/VideoPlayer/DVDStreamInfo.h"
#endif

#include <gbm.h>

using namespace KODI::WINDOWING::GBM;

using namespace std::chrono_literals;

#ifdef HAVE_DVBRIDGE
float CWinSystemGbmGLESContext::GetDisplayLatency()
{
  if (CB1AsyncPlayback())
  {
    const float refresh = m_DRM->GetCurrentMode().fRefreshRate;
    const float fps = CServiceBroker::GetDataCacheCore().GetVideoFps();
    // One source-picture preparation interval and one pending display refresh.
    if (refresh >= 10.0f)
      return 1000.0f / refresh +
          1000.0f / (std::isfinite(fps) && fps >= 10.0f ? std::min(fps, refresh) : refresh);
  }
  return CWinSystemGbmEGLContext::GetDisplayLatency();
}

std::optional<double> CWinSystemGbmGLESContext::CB1PresentationAge() const
{
  if (!CB1AsyncPlayback()) return std::nullopt;
  const auto drm = std::dynamic_pointer_cast<CDRMAtomic>(m_DRM);
  const uint64_t completed = drm ? drm->CB1PresentationTime() : 0;
  const double refresh = m_DRM->GetCurrentMode().fRefreshRate;
  timespec now{};
  if (!completed || !std::isfinite(refresh) || refresh < 10 ||
      clock_gettime(CLOCK_MONOTONIC, &now) != 0)
    return std::nullopt;
  const double nowNs = static_cast<double>(now.tv_sec) * 1e9 + now.tv_nsec;
  const double age = (nowNs - completed) / 1000;
  const double period = 1e6 / refresh;
  const double fps = CServiceBroker::GetDataCacheCore().GetVideoFps();
  const double sourcePeriod = std::isfinite(fps) && fps >= 10 ?
      std::max(period, 1e6 / fps) : period;
  // Keep one proven completion as the CPU crosses its estimated vblank timestamp.
  if (age < -period || age > 1.25 * sourcePeriod) return std::nullopt;
  return age;
}
#endif

CWinSystemGbmGLESContext::CWinSystemGbmGLESContext()
  : CWinSystemGbmEGLContext(EGL_PLATFORM_GBM_MESA, "EGL_MESA_platform_gbm")
{
}

void CWinSystemGbmGLESContext::Register()
{
  CWindowSystemFactory::RegisterWindowSystem(CreateWinSystem, "gbm");
}

CWinSystemGbmGLESContext::~CWinSystemGbmGLESContext()
{
#ifdef HAVE_DVBRIDGE
  if (m_dvBridge) m_dvBridge->Cancel();
  m_dvBridge.reset();
  if (m_dvContext && m_dvContext->GL())
  {
    m_nativeDVRetirement = std::move(m_dvContext);
    m_nativeDVProperties = std::make_shared<CDVBridgeState>(std::move(m_dvState));
  }
#endif
}

std::unique_ptr<CWinSystemBase> CWinSystemGbmGLESContext::CreateWinSystem()
{
  return std::make_unique<CWinSystemGbmGLESContext>();
}

bool CWinSystemGbmGLESContext::InitWindowSystem()
{
  VIDEOPLAYER::CRendererFactory::ClearRenderer();
  CDVDFactoryCodec::ClearHWAccels();
  CLinuxRendererGLES::Register();
  RETRO::CRPProcessInfoGbm::Register();
  RETRO::CRPProcessInfoGbm::RegisterRendererFactory(new RETRO::CRendererFactoryDMAOpenGLES);
  RETRO::CRPProcessInfoGbm::RegisterRendererFactory(new RETRO::CRendererFactoryOpenGLES);
  RETRO::CRPProcessInfoGbm::RegisterRendererFactory(new RETRO::CRendererFactoryFBO);

  // GLES 3.0 required for 10-bit texture format support (GL_RGB10_A2, GL_R16UI, etc.)
  // Fall back to GLES 2.0 for devices that don't support GLES 3.0 (e.g. lima driver)
  if (!CWinSystemGbmEGLContext::InitWindowSystemEGL(EGL_OPENGL_ES3_BIT, EGL_OPENGL_ES_API))
  {
    if (!CWinSystemGbmEGLContext::InitWindowSystemEGL(EGL_OPENGL_ES2_BIT, EGL_OPENGL_ES_API))
    {
      return false;
    }
  }

  bool general, deepColor;
  m_vaapiProxy.reset(GBM::VaapiProxyCreate(m_DRM->GetRenderNodeFileDescriptor()));
  GBM::VaapiProxyConfig(m_vaapiProxy.get(), m_eglContext.GetEGLDisplay());
  GBM::VAAPIRegisterRenderGLES(m_vaapiProxy.get(), general, deepColor);

  if (general)
  {
    GBM::VAAPIRegister(m_vaapiProxy.get(), deepColor);
  }

#ifdef HAVE_DVBRIDGE
  CRendererDRMPRIMEDV::Register();
#endif
  CRendererDRMPRIMEGLES::Register();
  CRendererDRMPRIME::Register();
  CDVDVideoCodecDRMPRIME::Register();
  VIDEOPLAYER::CProcessInfoGBM::Register();

  CScreenshotSurfaceGLES::Register();

  CBufferObjectFactory::ClearBufferObjects();
  CDumbBufferObject::Register();
#if defined(HAS_GBM_BO_MAP)
  CGBMBufferObject::Register();
#endif
#if defined(HAVE_LINUX_MEMFD) && defined(HAVE_LINUX_UDMABUF)
  CUDMABufferObject::Register();
#endif
#if defined(HAVE_LINUX_DMA_HEAP)
  CDMAHeapBufferObject::Register();
#endif

  return true;
}

#ifdef HAVE_DVBRIDGE
bool CWinSystemGbmGLESContext::PrepareNvidiaModeRestore(const RESOLUTION_INFO& res)
{
  if (m_dvState.CapturedBackend() != CDVBridgeState::Backend::NVIDIA) return true;
  const auto previous = m_DRM->GetCurrentMode();
  if (m_DRM->SetMode(res) && m_DRM->SetVideoMode(res,nullptr) && EndDVBridge())
    return true;
  m_DRM->SetMode(previous);
  m_DRM->SetVideoMode(previous,nullptr);
  return false;
}
#endif

bool CWinSystemGbmGLESContext::SetFullScreen(bool fullScreen,
                                             RESOLUTION_INFO& res,
                                             bool blankOtherDisplays)
{
#ifdef HAVE_DVBRIDGE
  if (m_modeHold.Holding() == CModeChangeHold::Kind::STOP && HoldCB1Mode(res))
  {
    m_modeHold.ModeChange();
    CRenderSystemGLES::ResetRenderSystem(res.iWidth,res.iHeight);
    return true;
  }
  m_modeHold.Cancel();
  m_modeAnnouncement.Dropped();
  if (!PrepareNvidiaModeRestore(res)) return false;
  if (!EndDVBridge())
    return false;
  m_dvBridge.reset();
  if (!RetireDVContext()) return false;
  const int opening = DVBRIDGE::modeChangeResolution.load();
  if (opening >= 0 && SupportsDVOutputMode())
  {
    const auto& requested=CDisplaySettings::GetInstance().GetResolutionInfo(static_cast<RESOLUTION>(opening));
    if (requested.strId == res.strId && requested.iWidth == res.iWidth &&
        requested.iHeight == res.iHeight && requested.dwFlags == res.dwFlags && HoldCB1Mode(res))
    {
      m_modeHold.Start(DVBRIDGE::DiagnosticNowMs());
      CRenderSystemGLES::ResetRenderSystem(res.iWidth,res.iHeight);
      return true;
    }
  }
#endif
  if (res.iWidth != m_nWidth || res.iHeight != m_nHeight)
  {
    CLog::Log(LOGDEBUG, "CWinSystemGbmGLESContext::{} - resolution changed, creating a new window",
              __FUNCTION__);
    if (!CreateNewWindow("", fullScreen, res)) return false;
  }

  if (!m_eglContext.TrySwapBuffers())
  {
    CEGLUtils::Log(LOGERROR, "eglSwapBuffers failed");
    throw std::runtime_error("eglSwapBuffers failed");
  }

  const bool switched = CWinSystemGbm::SetFullScreen(fullScreen, res, blankOtherDisplays);
  const auto atomic = std::dynamic_pointer_cast<CDRMAtomic>(m_DRM);
  if ((res.dwFlags & (D3DPRESENTFLAG_MODE3DSBS | D3DPRESENTFLAG_MODE3DTB)) &&
      (!switched || (atomic && !atomic->IsSelectedModeCommitted())))
  {
    GetGfxContext().SetStereoMode(RenderStereoMode::MONO);
    CLog::Log(LOGERROR, "3D output mode rejected; selecting mono playback");
    return false;
  }
  CRenderSystemGLES::ResetRenderSystem(res.iWidth, res.iHeight);

#ifdef HAVE_DVBRIDGE
  // Same-size refresh switches do not recreate the window. Recompute current
  // transport facts only after the selected mode has an accepted real receipt.
  if (switched && atomic && atomic->IsSelectedModeCommitted())
    RefreshDVOutputCapabilities();
#endif

  return true;
}

#ifdef HAVE_DVBRIDGE
bool CWinSystemGbmGLESContext::CB1ModeCanJoin(const RESOLUTION_INFO& res) const
{
  const auto drm=std::dynamic_pointer_cast<CDRMAtomic>(m_DRM);
  const auto* crtc=drm ? drm->GetCrtc() : nullptr;
  const auto current=drm ? drm->GetCurrentMode() : RESOLUTION_INFO{};
  return drm && crtc && !drm->HasPendingCB1Page() && drm->IsSelectedModeCommitted() &&
      crtc->GetPropertyValue("ACTIVE").value_or(0) &&
      res.iWidth == m_nWidth && res.iHeight == m_nHeight &&
      !(res.dwFlags & (D3DPRESENTFLAG_MODE3DSBS | D3DPRESENTFLAG_MODE3DTB)) &&
      GetGfxContext().GetStereoMode() == RenderStereoMode::OFF &&
      std::abs(current.fRefreshRate-res.fRefreshRate) > 0.001f;
}

bool CWinSystemGbmGLESContext::HoldCB1Mode(RESOLUTION_INFO& res)
{
  if (!CB1ModeCanJoin(res) || m_dispReset || !m_DRM->SetMode(res) ||
      !m_DRM->SetVideoMode(res,nullptr)) return false;
  m_modeAnnouncement.Held();
  DVBRIDGE::DiagnosticLog(LOGINFO,"CB1 HDMI transition: mode_selected={}x{}@{:.3f} commit=deferred",
      res.iScreenWidth,res.iScreenHeight,res.fRefreshRate);
  return true;
}

bool CWinSystemGbmGLESContext::HoldCB1Stop()
{
  if (!m_dvState.Captured() && !m_hdrConversion) return false;
  const auto settings=CServiceBroker::GetSettingsComponent()->GetSettings();
  if (settings->GetInt(CSettings::SETTING_VIDEOPLAYER_ADJUSTREFRESHRATE) == ADJUST_REFRESHRATE_ON_START)
    return false;
  auto resolution=CDisplaySettings::GetInstance().GetCurrentResolution();
  if (resolution < RES_DESKTOP) resolution=RES_DESKTOP;
  const auto target=CDisplaySettings::GetInstance().GetResolutionInfo(resolution);
  if (!CB1ModeCanJoin(target)) return false;
  m_modeHold.Stop(DVBRIDGE::DiagnosticNowMs());
  return true;
}

void CWinSystemGbmGLESContext::AnnounceCB1Mode(bool transport)
{
  if (!m_modeAnnouncement.Before(transport)) return;
  glFinish();
  OnLostDevice();
  m_dispResetTimer.Set(std::chrono::milliseconds(
      std::max(0,CServiceBroker::GetSettingsComponent()->GetSettings()->GetInt("videoscreen.delayrefreshchange"))*100));
}

void CWinSystemGbmGLESContext::CompleteCB1Mode(bool accepted, bool transport)
{
  m_modeAnnouncement.After(accepted,transport);
  if (accepted && m_modeHold.Holding() == CModeChangeHold::Kind::START)
    m_modeHold.Configured();
}

bool CWinSystemGbmGLESContext::Hide()
{
  m_modeHold.Cancel(); AnnounceCB1Mode(false);
  const bool result=CWinSystemGbm::Hide();
  const auto drm=std::dynamic_pointer_cast<CDRMAtomic>(m_DRM);
  CompleteCB1Mode(drm && drm->LastPageAccepted(),false);
  return result;
}

bool CWinSystemGbmGLESContext::Show(bool raise)
{
  m_modeHold.Cancel(); AnnounceCB1Mode(false);
  const bool result=CWinSystemGbm::Show(raise);
  const auto drm=std::dynamic_pointer_cast<CDRMAtomic>(m_DRM);
  CompleteCB1Mode(drm && drm->LastPageAccepted(),false);
  return result;
}

bool CWinSystemGbmGLESContext::BeginRender()
{
  // A prepared picture already includes its GUI; wait before drawing it again.
  if (m_cb1FramePending && m_dvFrame && !CompleteDVBridgePage(8)) return false;
  return CRenderSystemGLES::BeginRender();
}
#endif

void CWinSystemGbmGLESContext::PresentRender(bool rendered, bool videoLayer)
{
  if (!m_bRenderCreated)
    return;

  PresentFrame(rendered, videoLayer);
  if (m_dispReset && m_dispResetTimer.IsTimePast())
  {
    m_dispReset = false;
    CWinSystemGbm::OnResetDisplay();
  }
}

void CWinSystemGbmGLESContext::PublishVideoSource(
    bool accepted, const std::shared_ptr<const VideoSourceSideData>& source)
{
  if (accepted && source)
    CServiceBroker::GetDataCacheCore().SetVideoSourceSideData(source->Generation(), source);
}

void CWinSystemGbmGLESContext::PresentFrame(bool rendered, bool videoLayer)
{
#ifdef HAVE_DVBRIDGE
  // Yield to the pending scanout instead of repeatedly redrawing the GUI.
  if (!CompleteDVBridgePage(8)) return;
  ObserveCB1Headroom();
  if (m_modeHold.Holding() == CModeChangeHold::Kind::START && !m_dvFrame &&
      !m_modeHold.Commits(DVBRIDGE::DiagnosticNowMs()))
  {
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    return;
  }
  // An ordinary restoration must present before stop can stage its successor.
  if (m_dvStopPending && !m_dvOrdinaryFrame && !StopDVBridgePlayback()) return;
  if (m_dvEndPending && !EndDVBridge()) return;
  if (m_failedHDR10SetupRestore && !HasPendingDVOutputSurface() && !EndDVBridge())
    return;
  if (!PollRejectedDVSurface()) return;
  if (m_dvOrdinaryFrame)
  {
    if (!rendered || !m_eglContext.TrySwapBuffers()) return;
    auto drm = std::dynamic_pointer_cast<CDRMAtomic>(m_DRM);
    auto& surface = m_GBM->GetDevice().GetSurface();
    auto* bo = surface.LockTransactionalBuffer();
    AnnounceCB1Mode(false);
    const bool nvidiaRestore = m_dvState.CapturedBackend() == CDVBridgeState::Backend::NVIDIA;
    const bool submitted = drm && (nvidiaRestore ? FlipDVBridgePage(m_dvState,true,false,true) :
                                  drm->FlipPageTransactional(bo,true));
    CompleteCB1Mode(submitted,false);
    surface.CompleteTransactionalBuffer(submitted);
    if (!submitted)
    {
      DVBRIDGE::PlaybackNotification::OrdinaryOutputAccepted(false);
      RejectDVOutputSurface(); return;
    }
    if (!CompleteDVOutputSurface(true)) return;
    PublishVideoSource(true, m_videoSourceSideData);
    surface.EndTransactionalMode();
    m_dvState.Restored(); m_hdrDeepColorPrevious.reset();
    m_dvOrdinaryFrame = false;
    m_dvStopPending = false;
    m_hdr10Session.reset(); m_hdr10BasicSignal.reset(); m_hdr10Signal.reset();
    m_hdr10SessionStream=0;
    if (m_dvBridge) m_dvBridge->Reset();
    DVBRIDGE::StreamTelemetry::ClearOutput();
    DVBRIDGE::PlaybackNotification::OrdinaryOutputAccepted(true);
    return;
  }
  if (m_hdrConversion)
  {
    if (!m_dvFrame) return;
    bool presented = false;
    CDVBridgeGLES::Submission submitted;
    if (m_dvFrame && m_dvBridge && m_dvBridge->ResolveHDR10(UseLimitedColor()))
    {
      submitted = m_dvBridge->CaptureSubmission();
      submitted.sourceSideData = m_videoSourceSideData;
      const bool asynchronous = CreateCB1GPUFence(submitted);
      if (!asynchronous && !m_dvBridge->Advanced()) glFinish();
      if (m_eglContext.TrySwapBuffers())
      {
        auto drm = std::dynamic_pointer_cast<CDRMAtomic>(m_DRM);
        auto& surface = m_GBM->GetDevice().GetSurface();
        auto* bo = surface.LockTransactionalBuffer();
        const bool valid = drm && bo &&
            (drm->GetConnector()->SupportsProperty("DVBRIDGE_HDR10_DEEP_COLOR") ?
             drm->AddProperty(drm->GetConnector(), "DVBRIDGE_HDR10_DEEP_COLOR", 1) :
             FloatOutputUsable());
        AnnounceCB1Mode(true);
        const bool nvidiaRestore = m_dvState.CapturedBackend() == CDVBridgeState::Backend::NVIDIA;
        presented = drm && (nvidiaRestore ? (valid && FlipDVBridgePage(m_dvState,true,false,true)) :
                            drm->FlipPageTransactional(valid ? bo : nullptr, false, asynchronous));
        CompleteCB1Mode(presented,true);
        if (!drm || !drm->HasPendingCB1Page()) surface.CompleteTransactionalBuffer(presented);
      }
      else if (asynchronous)
        std::dynamic_pointer_cast<CDRMAtomic>(m_DRM)->FlipPageTransactional(nullptr);
    }
    if (m_dvBridge && m_dvBridge->Presented(presented,submitted))
    {
      PublishVideoSource(true, submitted.sourceSideData);
      m_fastSourceStream = m_fastPreparedStream;
      m_fastSourcePts = m_fastPreparedPts;
      DVBRIDGE::PlaybackNotification::NativeOutputAccepted(DVBRIDGE::StreamTelemetry::Read().output);
      const auto* plane=m_DRM->GetGuiPlane() ? m_DRM->GetGuiPlane() : m_DRM->GetVideoPlane();
      DVBRIDGE::StreamTelemetry::NativeFormat(
          {submitted.identity.stream,submitted.identity.picture,submitted.identity.revision},
          plane ? plane->GetFormat() : 0,m_nWidth,m_nHeight,m_DRM->GetCurrentMode().fRefreshRate);
      DVBRIDGE::StreamTelemetry::PublishNativeInfo(submitted.informationGeneration);
    }
    if (presented)
    {
      CompleteDVOutputSurface(true);
      m_dvActivating = false;
      if (auto drm = std::dynamic_pointer_cast<CDRMAtomic>(m_DRM)) drm->SetDVActivationPending(false);
      m_dvState.Restored();
    }
    else RejectDVOutputSurface();
    m_cb1FramePending = m_dvFrame = false;
    return;
  }
  if (m_dvState.Captured())
  {
    if (!m_dvFrame) return;
    bool presented = false;
    CDVBridgeGLES::Submission submitted;
    // EGL's window framebuffer has a bottom-left GL origin, while transport
    // metadata is indexed from the top-left of the HDMI raster.
    if (m_dvFrame && m_dvBridge && m_dvBridge->Pack(0, m_nWidth, m_nHeight, true))
    {
      submitted = m_dvBridge->CaptureSubmission();
      submitted.sourceSideData = m_videoSourceSideData;
      const bool asynchronous = CreateCB1GPUFence(submitted);
      if (!asynchronous && !m_dvBridge->Advanced()) glFinish();
      auto drm = std::dynamic_pointer_cast<CDRMAtomic>(m_DRM);
      if (m_eglContext.TrySwapBuffers())
      {
        const bool link = !m_hdrDeepColorPrevious || (drm && drm->AddProperty(
            drm->GetConnector(),"DVBRIDGE_HDR10_DEEP_COLOR",*m_hdrDeepColorPrevious));
        if (link)
        {
          const bool plain = !asynchronous && !m_dvActivating && !drm->HasPendingRequest();
          AnnounceCB1Mode(true);
          const auto started = std::chrono::steady_clock::now();
          presented = FlipDVBridgePage(m_dvState, false, asynchronous);
          CompleteCB1Mode(presented,true);
          if (presented)
          {
            const double elapsed = std::chrono::duration<double,std::milli>(
                std::chrono::steady_clock::now()-started).count();
            const auto verdict = m_presentStall.Presented(plain,elapsed);
            if (verdict != CDVPresentStall::Verdict::NONE)
              DVBRIDGE::DiagnosticLog(LOGWARNING,
                  "CB1 HDMI presentation: completed_commit_ms={:.1f} action={}",elapsed,
                  verdict == CDVPresentStall::Verdict::GIVE_UP ? "restore-ordinary" : "observe");
            if (verdict == CDVPresentStall::Verdict::GIVE_UP)
            {
              // The commit was accepted: retain its buffer until ordinary restoration.
              drm->FailDVActivation(ETIMEDOUT);
              m_dvRejectedRoute = DVBRIDGE::OutputRoute::NativeDV;
              m_dvRecoveryPending = true;
              m_dvBridge->Cancel();
              if (!m_dvContext->FenceLastUse()) m_dvRollbackFailed = true;
              std::lock_guard lock(m_dvCapabilityMutex);
              m_dvFallbackActive = true;
            }
          }
        }
        else if (asynchronous) drm->FlipPageTransactional(nullptr);
      }
      else if (asynchronous) drm->FlipPageTransactional(nullptr);
    }
    if (m_dvBridge && m_dvBridge->Presented(presented,submitted))
    {
      PublishVideoSource(true, submitted.sourceSideData);
      m_fastSourceStream = m_fastPreparedStream;
      m_fastSourcePts = m_fastPreparedPts;
      DVBRIDGE::PlaybackNotification::NativeOutputAccepted(DVBRIDGE::StreamTelemetry::Read().output);
      const auto* plane=m_DRM->GetGuiPlane() ? m_DRM->GetGuiPlane() : m_DRM->GetVideoPlane();
      DVBRIDGE::StreamTelemetry::NativeFormat(
          {submitted.identity.stream,submitted.identity.picture,submitted.identity.revision},
          plane ? plane->GetFormat() : 0,m_nWidth,m_nHeight,m_DRM->GetCurrentMode().fRefreshRate);
      DVBRIDGE::StreamTelemetry::PublishNativeInfo(submitted.informationGeneration);
    }
    if (presented)
    {
      CompleteDVOutputSurface(true);
      m_dvActivating = false;
      if (auto drm = std::dynamic_pointer_cast<CDRMAtomic>(m_DRM)) drm->SetDVActivationPending(false);
      m_hdrDeepColorPrevious.reset();
    }
    else RejectDVOutputSurface();
    m_cb1FramePending = m_dvFrame = false;
    return;
  }
#endif

  if (rendered || videoLayer)
  {
    bool async = !videoLayer && m_eglFence;
    if (rendered)
    {
#if defined(EGL_ANDROID_native_fence_sync) && defined(EGL_KHR_fence_sync)
      if (async)
      {
        int fd = m_DRM->TakeOutFenceFd();
        if (fd != -1)
        {
          m_eglFence->CreateKMSFence(fd);
          m_eglFence->WaitSyncGPU();
        }

        m_eglFence->CreateGPUFence();
      }
#endif

      if (!m_eglContext.TrySwapBuffers())
      {
        CEGLUtils::Log(LOGERROR, "eglSwapBuffers failed");
        throw std::runtime_error("eglSwapBuffers failed");
      }

#if defined(EGL_ANDROID_native_fence_sync) && defined(EGL_KHR_fence_sync)
      if (async)
      {
        int fd = m_eglFence->FlushFence();
        m_DRM->SetInFenceFd(fd);

        m_eglFence->WaitSyncCPU();
      }
#endif
    }

#ifdef HAVE_DVBRIDGE
    AnnounceCB1Mode(false);
#endif
    CWinSystemGbm::FlipPage(rendered, videoLayer, async);
#ifdef HAVE_DVBRIDGE
    if (auto drm=std::dynamic_pointer_cast<CDRMAtomic>(m_DRM))
      CompleteCB1Mode(drm->LastPageAccepted(),false);
#endif
    if (auto drm = std::dynamic_pointer_cast<CDRMAtomic>(m_DRM))
    {
      PublishVideoSource(drm->LastPageAccepted(), m_videoSourceSideData);
      CheckFloatOutput(drm->LastPageAccepted());
    }
#ifdef HAVE_DVBRIDGE
    if (auto drm=std::dynamic_pointer_cast<CDRMAtomic>(m_DRM))
    {
      DVBRIDGE::PlaybackNotification::OrdinaryOutputAccepted(drm->LastPageAccepted());
      if (drm->LastPageAccepted() &&
          CServiceBroker::GetAppComponents().GetComponent<CApplicationPlayer>()->IsPlayingVideo())
      {
        auto& cache=CServiceBroker::GetDataCacheCore();
        static uint64_t previousGeneration{};
        static std::chrono::steady_clock::time_point next;
        const auto generation=cache.GetVideoProcessingGeneration();
        const auto now=std::chrono::steady_clock::now();
        if (generation!=previousGeneration || now>=next || cache.GetVideoProcessingInfo().output.flags)
        {
          auto info=DVBRIDGE::NativeProcessingInfo(DVBRIDGE::StreamTelemetry::Read());
          info.output={}; info.outputType.reset();
          if (cache.SetVideoProcessingInfo(info.informationGeneration,info))
          {
            previousGeneration=generation;
            next=now+std::chrono::milliseconds(500);
          }
        }
      }
    }
#endif

  }
  else
  {
    KODI::TIME::Sleep(10ms);
  }
}

bool CWinSystemGbmGLESContext::SetGuiCompositing(int colorTransfer)
{
  m_guiCompositing = (colorTransfer != 0);

  if (m_guiCompositing)
  {
    if (!m_compositeShader)
    {
      std::string defines;
      if (UseLimitedColor()
#ifdef HAVE_DVBRIDGE
          && !m_dvState.Captured() && !m_hdrConversion
#endif
          )
        defines += "#define KODI_LIMITED_RANGE 1\n";
      m_compositeShader = std::make_unique<CGuiCompositeShaderGLES>(defines);
      if (!m_compositeShader->CompileAndLink())
      {
        CLog::Log(LOGERROR, "CWinSystemGbmGLESContext: failed to compile GUI composite shader");
        m_compositeShader.reset();
        m_guiCompositing = false;
        return false;
      }
    }

    if (!m_compositeShader->CreateLUTs(colorTransfer))
    {
      CLog::Log(LOGERROR, "CWinSystemGbmGLESContext: failed to create LUTs");
      m_compositeShader.reset();
      m_guiCompositing = false;
      return false;
    }
  }
  else
  {
#ifdef HAVE_DVBRIDGE
    m_dvPQSubtitleFbo.Cleanup();
    m_dvPQSubtitleWidth = m_dvPQSubtitleHeight = 0;
    m_dvPQSubtitleDraws = 0;
    m_dvPQSubtitleRendering = m_dvPQSubtitleCleared = m_dvNativeGui = false;
#endif
    m_guiFbo.Cleanup();
    m_guiFboWidth = 0;
    m_guiFboHeight = 0;
    m_compositeShader.reset();
  }

  return m_guiCompositing;
}

bool CWinSystemGbmGLESContext::BeginGuiComposite(bool guiWillRender)
{
  if (!m_guiCompositing)
    return false;

  m_guiWillRender = guiWillRender;

  int width = m_nWidth;
  int height = m_nHeight;

  // create or recreate FBO if size changed
  if (!m_guiFbo.IsValid() || m_guiFboWidth != width || m_guiFboHeight != height)
  {
    m_guiFbo.Cleanup();

    if (!m_guiFbo.Initialize())
    {
      CLog::Log(LOGERROR, "CWinSystemGbmGLESContext: failed to initialize GUI FBO");
      return false;
    }

    if (!m_guiFbo.CreateAndBindToTexture(GL_TEXTURE_2D, width, height, GL_RGBA))
    {
      CLog::Log(LOGERROR, "CWinSystemGbmGLESContext: failed to create GUI FBO texture {}x{}", width,
                height);
      m_guiFbo.Cleanup();
      return false;
    }

    if (GetEnabledFrontToBackRendering() && !m_guiFbo.AttachDepthBuffer(width, height))
    {
      CLog::Log(LOGERROR,
                "CWinSystemGbmGLESContext: failed to attach depth buffer to GUI FBO {}x{}", width,
                height);
      m_guiFbo.Cleanup();
      return false;
    }

    m_guiFboWidth = width;
    m_guiFboHeight = height;
    m_guiFboClean = false; // fresh FBO is undefined, force a clear
    CLog::Log(LOGDEBUG, "CWinSystemGbmGLESContext: created GUI FBO {}x{}", width, height);
  }

  // When GUI render is being skipped, leave the FBO bind/clear out: nothing
  // will draw into it this frame. The FBO's prior sRGB GUI content is
  // implicitly preserved across the skipped frame as a side effect.
  //! @todo The preserved sRGB FBO is currently not leveraged: D2P reuses the
  //! post-PQ GUI plane back buffer directly via display HW, and single-plane
  //! never reaches !guiWillRender (the dirty-driven skip is gated on
  //! IsRenderingVideoLayer). Future single-plane "gate, don't move" work
  //! lets the GUI walk skip while CompositeGui still runs each video frame,
  //! re-using this cached sRGB FBO as the composite source.
  if (!guiWillRender)
    return true;

#ifdef HAVE_DVBRIDGE
  m_dvSubtitleBounds = {};
  ++m_dvGUIContentGeneration;
  m_dvSubtitleDraws = 0;
  m_dvSubtitleContent = 0;
  m_dvPQSubtitleDraws = 0;
  m_dvPQSubtitleCleared = false;
  // The one prepared next picture already owns its packed subtitle composition.
  if (!m_cb1FramePending || !m_dvFrame)
    m_dvNativeGui = false;
#endif

  if (!m_guiFbo.BeginRender())
    return false;

  // Clear only when the FBO holds stale content; idle frames are already clean.
  if (!m_guiFboClean)
  {
    glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    m_guiFboClean = true;
  }
#ifdef HAVE_DVBRIDGE
  if (HasDVBridgeScreenshot() && (!m_dvPQSubtitleFbo.IsValid() ||
      m_dvPQSubtitleWidth != width || m_dvPQSubtitleHeight != height) && BeginDVPQSubtitle())
    EndDVPQSubtitle();
#endif

  return true;
}

void CWinSystemGbmGLESContext::EndGuiComposite()
{
  if (m_guiWillRender)
    m_guiFbo.EndRender();

#ifdef HAVE_DVBRIDGE
  // CB1 overwrites the full scanout raster, including exact black borders.
  if (m_dvState.Captured() || m_hdrConversion) return;
#endif

  // When the GUI render is skipped this frame, Flip(hasRendered=false, ...)
  // will skip eglSwapBuffers and the back-buffer contents never reach the
  // screen. Clearing it is pure waste. Gate on D2P because single-plane
  // never reaches !m_guiWillRender (the dirty-driven skip is gated on
  // IsRenderingVideoLayer()), so this is the only path that triggers.
  const bool isD2P = m_DRM && m_DRM->GetVideoPlane() != nullptr && m_DRM->GetGuiPlane() != nullptr;
  if (isD2P && !m_guiWillRender)
    return;

  // Clear the backbuffer before video renders. In the FBO compositing path,
  // video renders in the RenderEx pass with clear=false, so DrawBlackBars is
  // never called. Without this clear, letterbox areas retain stale content
  // from the swap chain when the display resolution doesn't change between
  // GUI and video playback.
  glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
  glClear(GL_COLOR_BUFFER_BIT);
}

// CompositeGui is the last GL operation in the frame (called just before EndRender).
// GL state (blend mode, active texture, vertex arrays) is not restored afterward;
// the next frame's rendering sets its own state.
void CWinSystemGbmGLESContext::CompositeGui()
{
  if (!m_guiFbo.IsValid() || !m_guiFbo.IsBound() || !m_compositeShader)
    return;

  // Only update m_guiFboClean when GUI render fired this frame; otherwise the
  // FBO is in the same state as the previous frame and the flag stays as-is.
  // m_guiFboClean meaning depends on context:
  //   single-plane: "FBO is empty/clean" (no composite work needed)
  //   D2P:          "FBO is empty/clean AND back buffer cache is invalid"
  bool hasPQSubtitles = false;
#ifdef HAVE_DVBRIDGE
  hasPQSubtitles = HasDVBridgeScreenshot() && m_dvPQSubtitleDraws != 0;
#endif
  if (m_guiWillRender)
  {
    const bool guiEmpty = (GetGUIElementCount() ==
#ifdef HAVE_DVBRIDGE
                          (hasPQSubtitles ? m_dvPQSubtitleDraws : 0u)
#else
                          0u
#endif
    );
    m_guiFboClean = guiEmpty;
    if (guiEmpty && !hasPQSubtitles)
      return;
  }
  else if (m_guiFboClean && !hasPQSubtitles)
  {
    return;
  }

#ifdef HAVE_DVBRIDGE
  if (m_dvNativeGui)
    return;
#endif

  // D2P with no new render: the cached PQ frame is already in the GUI plane
  // back buffer from the prior composite. Skip the shader pass entirely; Flip
  // will skip eglSwapBuffers too (hasRendered==false because Render was not
  // called), and the display HW keeps scanning out the cached frame while the
  // video plane updates independently via atomic commit.
  if (!m_guiWillRender && m_DRM && m_DRM->GetVideoPlane() != nullptr &&
      m_DRM->GetGuiPlane() != nullptr)
    return;

#ifdef HAVE_DVBRIDGE
  GLint previousFramebuffer = 0;
  GLint previousViewport[4]{};
  const bool dvComposite = m_dvState.Captured() || m_hdrConversion;
  if (dvComposite)
  {
    if (!m_dvFrame || !m_dvBridge)
      return;
    const auto target = m_dvBridge->GetPQFramebuffer();
    if (!target)
    {
      m_dvFrame = false;
      return;
    }
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &previousFramebuffer);
    glGetIntegerv(GL_VIEWPORT, previousViewport);
    glBindFramebuffer(GL_FRAMEBUFFER, target);
    glViewport(0, 0, 3840, 2160);
  }
#endif

  glActiveTexture(GL_TEXTURE0);
  glBindTexture(GL_TEXTURE_2D, m_guiFbo.Texture());

  glEnable(GL_BLEND);

  // In D2P, the GUI plane is composited against a separate video plane by
  // the display HW. The default glBlendFunc also blends the alpha channel,
  // leaving the GUI plane buffer with src.a^2; the hardware composite then
  // reads that squared alpha and translucent GUI pixels render at the
  // wrong opacity. Replace the stored alpha so the hardware sees src.a.
  if (m_DRM->GetVideoPlane() && m_DRM->GetGuiPlane())
    glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ZERO);
  else
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

  // set up orthographic projection (screen coords, Y-down)
  float w = static_cast<float>(m_guiFboWidth);
  float h = static_cast<float>(m_guiFboHeight);

  // BEGIN GUI target projection (also exercised by the GPU orientation regression).
  GLfloat proj[16] = {2.0f / w, 0, 0, 0, 0, -2.0f / h, 0, 0, 0, 0, -1, 0, -1.0f, 1.0f, 0, 1};
#ifdef HAVE_DVBRIDGE
  if (dvComposite)
  {
    // libplacebo's PQ texture stores logical top at row zero. The final HDMI
    // packing pass reverses it for EGL scanout. Compose the GUI in that same
    // logical orientation, not in the ordinary window framebuffer orientation.
    proj[5] = -proj[5];
    proj[13] = -proj[13];
  }
#endif
  // END GUI target projection

  m_compositeShader->SetProjection(proj);
  GLint posLoc = m_compositeShader->GetPosLoc();
  GLint texLoc = m_compositeShader->GetTexLoc();

  GLfloat vert[4][2] = {{0, 0}, {w, 0}, {w, h}, {0, h}};
  GLfloat tex[4][2] = {{0, 1}, {1, 1}, {1, 0}, {0, 0}};
  GLubyte idx[4] = {0, 1, 3, 2};

  glVertexAttribPointer(posLoc, 2, GL_FLOAT, GL_FALSE, 0, vert);
  glVertexAttribPointer(texLoc, 2, GL_FLOAT, GL_FALSE, 0, tex);
  glEnableVertexAttribArray(posLoc);
  glEnableVertexAttribArray(texLoc);

  // PQ subtitles keep their original luminance and premultiplied alpha.
#ifdef HAVE_DVBRIDGE
  if (hasPQSubtitles)
  {
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, m_dvPQSubtitleFbo.Texture());
    glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
    m_compositeShader->SetInputPQ(true);
    m_compositeShader->Enable();
    glDrawElements(GL_TRIANGLE_STRIP, 4, GL_UNSIGNED_BYTE, idx);
    m_compositeShader->Disable();
  }
#endif
  if (!m_guiFboClean)
  {
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, m_guiFbo.Texture());
    if (m_DRM->GetVideoPlane() && m_DRM->GetGuiPlane())
      glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ZERO);
    else
      glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    m_compositeShader->SetInputPQ(false);
    m_compositeShader->Enable();
    glDrawElements(GL_TRIANGLE_STRIP, 4, GL_UNSIGNED_BYTE, idx);
    m_compositeShader->Disable();
  }
  m_compositeShader->SetInputPQ(false);

  glDisableVertexAttribArray(posLoc);
  glDisableVertexAttribArray(texLoc);

#ifdef HAVE_DVBRIDGE
  if (dvComposite)
  {
    glBindFramebuffer(GL_FRAMEBUFFER, previousFramebuffer);
    glViewport(previousViewport[0], previousViewport[1], previousViewport[2], previousViewport[3]);
  }
#endif
}

#ifdef HAVE_DVBRIDGE
namespace
{
bool NativeDVEligible(bool source, bool software, bool interlaced, bool stereo)
{
  if (!source || software)
    return false;
  auto* window = dynamic_cast<CWinSystemGbmGLESContext*>(CServiceBroker::GetWinSystem());
  const auto mode = DVBRIDGE::playbackMode.load();
  const bool available = window && mode != DVBRIDGE::OutputMode::Disabled &&
      window->CanRetainDVSource();
  return DVBRIDGE::Eligible(available, available, source, software, interlaced, stereo);
}
} // namespace

bool DVBRIDGE::RetainsDVSource(const CDVDStreamInfo& stream)
{
  if (!(stream.codecOptions & CODEC_FORCE_SOFTWARE))
  {
    StreamTelemetry::Begin(stream.dovi.dv_profile, stream.dovi.dv_level,
        stream.dovi.dv_version_major, stream.dovi.dv_version_minor,
        stream.dovi.bl_present_flag, stream.dovi.el_present_flag, stream.dovi.rpu_present_flag);
    StreamTelemetry::StaticMetadata(stream.masteringMetadata.get(), stream.contentLightMetadata.get());
  }
  const bool eligible = NativeDVEligible(stream.dovi.dv_profile != 0,
      (stream.codecOptions & CODEC_FORCE_SOFTWARE) != 0,
      stream.interlaced,!stream.stereo_mode.empty());
  if (eligible) DVBRIDGE::DiagnosticCandidate();
  if (stream.dovi.dv_profile && DiagnosticsEnabled())
    DVBRIDGE::DiagnosticLog(LOGINFO, "DVBridge stream candidate: profile={} level={} compatibility={} native_eligible={} software={} interlaced={} stereo={} mode={} (selection probe, not active output)",
        stream.dovi.dv_profile, stream.dovi.dv_level, stream.dovi.dv_bl_signal_compatibility_id, eligible,
        (stream.codecOptions & CODEC_FORCE_SOFTWARE) != 0, stream.interlaced,
        !stream.stereo_mode.empty(), static_cast<int>(playbackMode.load()));
  return eligible;
}

bool DVBRIDGE::WantsNativeOutput(const CDVDStreamInfo& stream)
{
  return NativeDVEligible(stream.dovi.dv_profile != 0,
      (stream.codecOptions & CODEC_FORCE_SOFTWARE) != 0,
      stream.interlaced,!stream.stereo_mode.empty());
}

bool DVBRIDGE::WantsNativeOutput(const VideoPicture* picture)
{
  auto* window = dynamic_cast<CWinSystemGbmGLESContext*>(CServiceBroker::GetWinSystem());
  if (!window) return false;
  const auto selected = window->SelectedDVOutput(picture);
  if (selected.route != OutputRoute::Vanilla) return true;
  if (selected.reason != OutputReason::NoGPUProcessing &&
      selected.reason != OutputReason::NoDVTransport &&
      selected.reason != OutputReason::NoHDR10Transport) return false;
  auto caps = window->GetDVOutputCapabilities(picture, true);
  // Keep source/cadence intent before the GUI thread selects the required mode.
  // Cached current-mode facts and activation remain strict.
  if (SelectOutput(FrameOutputSettings(), caps).route != OutputRoute::Vanilla) return true;
  // Decoder configuration precedes GL import; unknown import must not erase DV cadence intent.
  return picture && caps.isDV && !caps.processingKnown &&
      (picture->doviSourceBinding.Valid() || picture->doviMetadata.Binding().Valid()) &&
      NativeDVEligible(true, false, (picture->iFlags & DVP_FLAG_INTERLACED) != 0,
                       !picture->stereoMode.empty());
}

bool CWinSystemGbmGLESContext::DVBridgeRequested(const VideoPicture* picture)
{
  return DVBRIDGE::WantsNativeOutput(picture);
}

int CWinSystemGbmGLESContext::VideoOutputBitDepth(const VideoPicture* picture) const
{
  const auto selection = SelectedDVOutput(picture);
  return selection.route == DVBRIDGE::OutputRoute::NativeDV ? 8 :
      selection.route == DVBRIDGE::OutputRoute::HDR10 ? 10 :
      CWinSystemGbmEGLContext::VideoOutputBitDepth(picture);
}

bool CWinSystemGbmGLESContext::CreateNewWindow(const std::string& name, bool fullScreen,
                                             RESOLUTION_INFO& res)
{
  if (!CWinSystemGbmEGLContext::CreateNewWindow(name, fullScreen, res)) return false;
  RefreshDVOutputCapabilities();
  return true;
}

void CWinSystemGbmGLESContext::RefreshDVOutputCapabilities()
{
  if (eglGetCurrentContext() == EGL_NO_CONTEXT ||
      eglGetCurrentContext() != m_eglContext.GetEGLContext()) return;
  auto caps = GetDVOutputCapabilities(nullptr);
  caps.nativeDV = SupportsNativeDVOutput();
  caps.hdr10 = SupportsHDR10Conversion();
  if (m_renderableType != EGL_OPENGL_ES3_BIT)
  {
    caps.processingKnown = true;
    for (auto& mode : caps.processing) mode = {false, DVBRIDGE::OutputReason::NoGPUProcessing};
    for (auto& mode : caps.hdr10Processing) mode = {false, DVBRIDGE::OutputReason::NoGPUProcessing};
  }
  else if (m_dvBridge && m_dvBridge->HasRenderer())
  {
    caps.processingKnown = true;
    for (unsigned mode = 1; mode < caps.processing.size(); ++mode)
    {
      const bool usable=m_dvBridge->SupportsMode(static_cast<dvbridge_mode>(mode));
      caps.processing[mode] = {usable, usable ? DVBRIDGE::OutputReason::None :
          DVBRIDGE::OutputReason::NoGPUProcessing};
      const bool ai=m_dvBridge->SupportsHDR10AI(static_cast<dvbridge_mode>(mode));
      caps.hdr10Processing[mode] = {ai,ai ? DVBRIDGE::OutputReason::None : DVBRIDGE::OutputReason::NoGPUProcessing};
    }
  }
  else
  {
    CDVBridgeGLES::Context probe;
    auto* context = m_dvContext ? m_dvContext.get() : &probe;
    if (context->Initialize(false))
    {
      if (auto* renderer = context->Attach())
      {
        const bool floatBlend = pl_opengl_has_ext(context->GL(), "GL_EXT_float_blend");
        const bool common = floatBlend &&
            pl_opengl_has_ext(context->GL(), "GL_OES_texture_float_linear");
        caps.processingKnown = true;
        for (unsigned mode = 1; mode < caps.processing.size(); ++mode)
        {
          const bool usable=floatBlend && dvbridge_renderer_supports_mode(renderer, static_cast<dvbridge_mode>(mode));
          caps.processing[mode] = {usable, usable ? DVBRIDGE::OutputReason::None :
              DVBRIDGE::OutputReason::NoGPUProcessing};
          const bool ai=common && dvbridge_renderer_supports_hdr10_ai(renderer,static_cast<dvbridge_mode>(mode));
          caps.hdr10Processing[mode] = {ai,ai ? DVBRIDGE::OutputReason::None : DVBRIDGE::OutputReason::NoGPUProcessing};
        }
        context->Detach(renderer);
      }
    }
  }
  const bool availableNativeDV = SupportsNativeDVOutput(false);
  const bool availableHDR10 = SupportsHDR10Conversion(false);
  std::lock_guard lock(m_dvCapabilityMutex);
  m_dvCapabilities = caps;
  m_dvAvailableNativeDV = availableNativeDV;
  m_dvAvailableHDR10 = availableHDR10;
  if (DVBRIDGE::DiagnosticsEnabled())
  {
    try
    {
      const auto* connector = m_DRM ? m_DRM->GetConnector() : nullptr;
      const auto* displayInfo = GetDisplayInfo();
      const bool inventory = connector && connector->HasPropertyInventory();
      const DVBRIDGE::DiagnosticProperty dv(inventory,
          inventory ? connector->GetPropertyValue("DVBRIDGE_DV_STANDARD_LAB") : std::nullopt);
      DVBRIDGE::DiagnosticLog(LOGINFO,
          "DVBridge capabilities: origin=initial-or-mode-refresh connector={} connection_generation=unknown graphics_generation={} edid_status={} edid_version={} dv_property={} dv_value={} native_dv={} available_native_dv={} hdr10={} link_type=unknown",
          connector ? connector->GetId() : 0,caps.generation,
          displayInfo ? dvbridge_edid_reason(displayInfo->GetDolbyVisionCaps().status) : "unknown",
          displayInfo ? std::to_string(displayInfo->GetDolbyVisionCaps().version) : "unavailable",
          dv.Presence(),dv.Value(),caps.nativeDV,availableNativeDV,caps.hdr10);
    }
    catch (...) {}
  }
}

bool CWinSystemGbmGLESContext::CanRetainDVSource() const
{
  const auto preferences = DVBRIDGE::FrameOutputSettings();
  if (preferences.output == DVBRIDGE::OutputMode::Disabled) return false;
  const auto caps = GetDVOutputCapabilities(nullptr, true);
  if (!caps.nativeDV && !caps.hdr10) return false;
  if (!caps.processingKnown) return true;
  const auto mode = preferences.Policy().mode;
  return (caps.processing[mode].usable &&
      (preferences.output == DVBRIDGE::OutputMode::HDR10 ? caps.hdr10 : caps.nativeDV)) ||
      (preferences.output != DVBRIDGE::OutputMode::HDR10 && caps.hdr10 &&
       caps.processing[DVBRIDGE_MODE_HDR10_BASIC].usable);
}

bool CWinSystemGbmGLESContext::CompleteDVModeSwitch(const VideoPicture& picture)
{
  auto drm = std::dynamic_pointer_cast<CDRMAtomic>(m_DRM);
  if (SupportsDVOutputMode() && drm && (drm->IsSelectedModeCommitted() ||
      m_modeHold.Holding() == CModeChangeHold::Kind::START)) return true;
  // An advertised mode can still fail its real modeset. Reuse the existing
  // per-source activation failure latch; never wait or retry every frame.
  ResetDVActivation(&picture);
  std::lock_guard lock(m_dvCapabilityMutex);
  m_dvNativeFailed = m_dvHDR10Failed = m_dvFallbackActive = true;
  m_dvActivationReason = DVBRIDGE::OutputReason::ActivationFailed;
  m_dvFailedStream = m_dvActivationStream;
  m_dvFailedPolicy = m_dvActivationPolicy;
  return false;
}

void CWinSystemGbmGLESContext::ObserveDVImport(DVBRIDGE::SourceBinding binding,
                                              dvbridge_geometry geometry, bool usable)
{
  std::lock_guard lock(m_dvCapabilityMutex);
  if (eglGetCurrentContext() != m_eglContext.GetEGLContext()) binding = {};
  m_dvImportBinding = binding;
  m_dvImportGeometry = geometry;
  m_dvImportUsable = usable;
  m_dvImportGeneration = m_dvCapabilities.generation;
  if (binding.Valid() && (binding.stream != m_dvFatalBinding.stream ||
                          binding.revision != m_dvFatalBinding.revision))
  {
    m_dvFatalBinding = binding;
    m_dvFatalReason = DVBRIDGE::OutputReason::None;
    m_dvFatalConsumed = false;
  }
}

DVBRIDGE::OutputCapabilities CWinSystemGbmGLESContext::GetDVOutputCapabilities(
    const VideoPicture* picture, bool allowModeSwitch) const
{
  return GetDVOutputCapabilities(picture, DVBRIDGE::FrameOutputSettings(), allowModeSwitch);
}

DVBRIDGE::OutputCapabilities CWinSystemGbmGLESContext::GetDVOutputCapabilities(
    const VideoPicture* picture, const DVBRIDGE::OutputPreferences& preferences, bool allowModeSwitch) const
{
  std::lock_guard lock(m_dvCapabilityMutex);
  auto caps = m_dvCapabilities;
  if (allowModeSwitch)
  {
    caps.nativeDV = m_dvAvailableNativeDV;
    caps.hdr10 = m_dvAvailableHDR10;
  }
  if (!picture) return caps;
  caps.sourceKnown = true;
  caps.stereoSource = !picture->stereoMode.empty();
  caps.isDV = picture->doviSourceInfo.profile || picture->hdrType == StreamHdrType::HDR_TYPE_DOLBYVISION;
  caps.isHDR10 = !caps.isDV && picture->hdrType == StreamHdrType::HDR_TYPE_HDR10;
  if (caps.stereoSource) return caps;
  if (caps.isHDR10)
  {
#ifdef CB1_HAS_HDR10_AI
    caps.processing=caps.hdr10Processing;
    const bool valid = m_dvImportUsable && m_dvImportBinding.Valid() &&
        m_dvImportBinding == picture->doviSourceBinding &&
        m_dvImportGeneration == caps.generation &&
        picture->doviSourcePts != AV_NOPTS_VALUE &&
        !(picture->iFlags & DVP_FLAG_INTERLACED) && picture->stereoMode.empty();
    for (auto& mode : caps.processing)
      if (!valid) mode = {false,DVBRIDGE::OutputReason::SourceIneligible};
#endif
    return caps;
  }
  if (!caps.isDV) return caps;
  const auto binding = picture->doviSourceBinding.Valid() ?
      picture->doviSourceBinding : picture->doviMetadata.Binding();
  if (!m_dvImportBinding.Valid() || m_dvImportBinding != binding ||
      m_dvImportGeneration != caps.generation)
  {
    caps.processingKnown = false;
    return caps;
  }
  const auto& metadata = picture->doviMetadata;
  const bool paired = picture->doviEnhancement && picture->doviEnhancementBinding.Valid() &&
      picture->doviEnhancementBinding == metadata.Binding() &&
      picture->doviEnhancement->pts == picture->doviSourcePts;
  dvbridge_color color{};
  unsigned margins[4];
  const bool valid = m_dvImportUsable && metadata.Binding().Valid() &&
      picture->doviSourcePts != AV_NOPTS_VALUE && !(picture->iFlags & DVP_FLAG_INTERLACED) &&
      picture->stereoMode.empty() &&
      dvbridge_frame_area(margins, metadata.Data(), metadata.Size(), m_dvImportGeometry) &&
      dvbridge_map_color(&color, metadata.Data(), metadata.Size(), paired);
  dvbridge_hdr10_session session{};
  const bool basic = valid && dvbridge_hdr10_session_init(&session, metadata.Data(), metadata.Size());
  for (unsigned mode = 1; mode < caps.processing.size(); ++mode)
  {
    if (!valid)
      caps.processing[mode] = {false, DVBRIDGE::OutputReason::SourceIneligible};
    else if (mode == DVBRIDGE_MODE_HDR10_BASIC && !basic)
      caps.processing[mode] = {false, DVBRIDGE::OutputReason::SourceIneligible};
    else if(mode == DVBRIDGE_MODE_MASTER_REFERENCE && caps.processing[mode].usable)
    {
      pl_cb1_dovi_reference raw{};
      if(cb1_master_reference_map(&raw,metadata.Data(),metadata.Size(),paired)!=CB1_MASTER_READY ||
         !cb1_master_reference_transport(metadata.Data(),metadata.Size()) ||
         ((margins[0]|margins[1])&1))
        caps.processing[mode]={false,DVBRIDGE::OutputReason::SourceIneligible};
      const auto masterChroma = picture->chroma_position == AVCHROMA_LOC_UNSPECIFIED ?
          (picture->doviSourceInfo.profile == 7 ? AVCHROMA_LOC_TOPLEFT : AVCHROMA_LOC_LEFT) : picture->chroma_position;
      const auto enhancementChroma = picture->doviEnhancement &&
          picture->doviEnhancement->chroma_location == AVCHROMA_LOC_UNSPECIFIED ?
          AVCHROMA_LOC_TOPLEFT : picture->doviEnhancement ? picture->doviEnhancement->chroma_location : AVCHROMA_LOC_UNSPECIFIED;
      const bool masterPhase = (masterChroma == AVCHROMA_LOC_LEFT ||
          masterChroma == AVCHROMA_LOC_TOPLEFT) &&
          (!raw.residual || masterChroma == enhancementChroma);
      if(!masterPhase) caps.processing[mode]={false,DVBRIDGE::OutputReason::SourceIneligible};
    }
    else if ((mode == DVBRIDGE_MODE_HDR10_EXPERT || mode == DVBRIDGE_MODE_ENHANCED_DV) && caps.processing[mode].usable && preferences.ProfileValid())
    {
      auto policy = preferences.Policy();
      policy.mode = static_cast<dvbridge_mode>(mode);
      policy.tv = {preferences.peak, static_cast<dvbridge_panel>(preferences.panel),
          static_cast<dvbridge_gamut>(preferences.gamut)};
      pl_color_space nominal{};
      dvbridge_hdr10_metadata signal{};
      if (!dvbridge_reference_map(&color, &nominal, &signal, &policy,
                                 metadata.Data(), metadata.Size(), paired))
        caps.processing[mode] = {false, DVBRIDGE::OutputReason::SourceIneligible};
    }
  }
  return caps;
}

DVBRIDGE::OutputSelection CWinSystemGbmGLESContext::SelectedDVOutput(const VideoPicture* picture) const
{
  const auto preferences = DVBRIDGE::FrameOutputSettings();
  auto caps = GetDVOutputCapabilities(picture, preferences);
  return SelectedDVOutput(picture, preferences, caps);
}

DVBRIDGE::OutputSelection CWinSystemGbmGLESContext::SelectedDVOutput(const VideoPicture* picture,
    const DVBRIDGE::OutputPreferences& preferences, DVBRIDGE::OutputCapabilities& caps) const
{
  const auto binding = picture ? (picture->doviSourceBinding.Valid() ?
      picture->doviSourceBinding : picture->doviMetadata.Binding()) : DVBRIDGE::SourceBinding{};
  auto reason = DVBRIDGE::OutputReason::None;
  {
    std::lock_guard lock(m_dvCapabilityMutex);
    if (binding.stream == m_dvActivationStream && preferences.revision == m_dvActivationPolicy)
    {
      caps.nativeDV &= !m_dvNativeFailed;
      caps.hdr10 &= !m_dvHDR10Failed;
      reason = m_dvActivationReason;
    }
  }
  auto result = DVBRIDGE::SelectOutput(preferences, caps);
#ifdef CB1_HAS_HDR10_AI
  if (caps.isHDR10)
  {
    std::lock_guard lock(m_dvCapabilityMutex);
    if (!m_aiReady || m_aiFallback || binding.stream != m_aiActivation.stream ||
        binding.revision != m_aiActivation.revision || preferences.revision != m_aiPolicy)
      result = {};
  }
#endif
  if (reason != DVBRIDGE::OutputReason::None) result.reason = reason;
  return result;
}

#ifdef CB1_HAS_HDR10_AI
#ifndef CB1_L1L3_BUNDLE_DIR
#define CB1_L1L3_BUNDLE_DIR "/usr/share/cb1/l1l3"
#endif
bool CWinSystemGbmGLESContext::HDR10AILookaheadActive() const
{
  std::lock_guard lock(m_dvCapabilityMutex);
  return m_aiActive;
}

bool CWinSystemGbmGLESContext::PrepareHDR10AIQueue(const VideoPicture& picture,
    std::span<const CDVBridgeGLES::Layer> queue, bool end, bool paused,
    uint64_t presentationDeadline)
{
  const auto preferences = DVBRIDGE::FrameOutputSettings();
  const auto caps = GetDVOutputCapabilities(&picture, preferences);
  if (!caps.isHDR10 ||
      DVBRIDGE::SelectOutput(preferences, caps).route == DVBRIDGE::OutputRoute::Vanilla)
  {
    {
      std::lock_guard lock(m_dvCapabilityMutex);
      m_aiActive=false;
    }
    return !m_dvContext || m_dvContext->StopHDR10AI();
  }
  const auto binding = picture.doviSourceBinding;
  if (queue.empty() || !(queue.front().binding == binding)) return false;
  {
    std::lock_guard lock(m_dvCapabilityMutex);
    if (binding.stream != m_aiActivation.stream ||
        binding.revision != m_aiActivation.revision || preferences.revision != m_aiPolicy)
    {
      m_aiActivation = binding;
      m_aiPolicy = preferences.revision;
      m_aiReady = m_aiFallback = false;
    }
    m_aiActive=!m_aiFallback;
    if (m_aiFallback) return true;
  }
  if (!m_dvContext) m_dvContext = std::make_shared<CDVBridgeGLES::Context>();
  auto decision = HDR10AISource::Decision::Native;
  cb1_ai_result prediction{};
  if (m_dvContext->Initialize(false))
  {
    decision = m_dvContext->PrepareHDR10AIQueue(queue, preferences,
        CB1_L1L3_BUNDLE_DIR, DVBRIDGE::DiagnosticNowMs(), end, &prediction, paused,
        presentationDeadline);
  }
  else m_dvContext->HDR10AI().RecordStatus("analysis-context",CB1_AI_INCOMPATIBLE);
  if (decision == HDR10AISource::Decision::Prepare &&
      (!m_dvBridge || !m_dvBridge->CommittedHDR10AI()))
  {
    // Prime the output before Kodi starts the playback clock.
    {
      std::lock_guard lock(m_dvCapabilityMutex);
      m_aiReady = true;
    }
    auto status = CDVBridgeGLES::FrameStatus::Pending;
    const bool outputReady = PrepareDVOutputFrame(picture) && BeginDVBridge();
    if (outputReady)
      status = m_dvBridge->PrepareHDR10AIFrame(queue.front(),
          m_dvImportGeometry, preferences, prediction, m_dvSelection.policy);
    const auto result = status == CDVBridgeGLES::FrameStatus::Ready ||
        status == CDVBridgeGLES::FrameStatus::Reused ? CB1_AI_READY :
        status == CDVBridgeGLES::FrameStatus::Failed ||
        status == CDVBridgeGLES::FrameStatus::Unsupported ? CB1_AI_INCOMPATIBLE : CB1_AI_PENDING;
    m_dvContext->HDR10AI().RecordStatus(outputReady ? "rgb-prepare" : "rgb-output-setup",result);
    decision = m_dvContext->HDR10AI().Preparation(binding, result, DVBRIDGE::DiagnosticNowMs());
  }
  {
    std::lock_guard lock(m_dvCapabilityMutex);
    m_aiReady |= decision == HDR10AISource::Decision::Prepare;
    m_aiFallback = decision == HDR10AISource::Decision::Native;
    m_aiActive = !m_aiFallback;
  }
  if (decision == HDR10AISource::Decision::Native)
  {
    const auto& source = m_dvContext->HDR10AI();
    const auto metrics = source.Metrics();
    DVBRIDGE::DiagnosticLog(LOGWARNING,
        "CB1 HDR10 AI: native HDR10 fallback stream={} revision={} policy={} stage={} status={} queue={} coverage_s={} pictures={} gpu_submissions={} readback_bytes={} inferences={} prepared={} committed={} end={} paused={} head={} last_submitted={}",
        binding.stream, binding.revision, preferences.revision,
        source.DiagnosticStage(),int(source.DiagnosticStatus()),queue.size(),
        queue.back().pts-queue.front().pts,metrics.pictures,metrics.gpu_submissions,
        metrics.readback_bytes,metrics.inferences,source.AwaitingPresentation(),bool(source.Committed()),
        end,paused,binding.picture,source.LastSubmitted());
    DVBRIDGE::PlaybackNotification::HDR10Recovery();
  }
  return decision == HDR10AISource::Decision::Prepare ||
      decision == HDR10AISource::Decision::Native;
}
#endif

void CWinSystemGbmGLESContext::ResetDVActivation(const VideoPicture* picture)
{
  if (!picture) return;
  const auto binding = picture->doviSourceBinding.Valid() ?
      picture->doviSourceBinding : picture->doviMetadata.Binding();
  const auto revision = DVBRIDGE::FrameOutputSettings().revision;
  {
    std::lock_guard lock(m_dvCapabilityMutex);
    if (binding.stream == m_dvActivationStream && revision == m_dvActivationPolicy) return;
    m_dvActivationStream = binding.stream;
    m_dvActivationPolicy = revision;
    m_dvNativeFailed = m_dvHDR10Failed = m_dvFallbackActive = false;
    m_dvActivationReason = DVBRIDGE::OutputReason::None;
    m_dvFailedStream = m_dvFailedPolicy = 0;
  }
  if (auto drm = std::dynamic_pointer_cast<CDRMAtomic>(m_DRM)) drm->ResetDVActivation();
}

bool CWinSystemGbmGLESContext::RecoverDVActivation()
{
  if (!m_dvRecoveryPending) return true;
  if (!PollRejectedDVSurface()) return false;
  // Retire the rejected GPU work before restoring properties with clean pixels.
  if (!m_dvContext || m_dvContext->Poll() != DVBRIDGE_DV_IDLE) return false;
  if (!EndDVBridge())
  {
    m_dvRollbackFailed = true;
    std::lock_guard lock(m_dvCapabilityMutex);
    m_dvActivationReason = DVBRIDGE::OutputReason::RestoreFailed;
    return false;
  }
  auto drm = std::dynamic_pointer_cast<CDRMAtomic>(m_DRM);
  bool fallback;
  {
    std::lock_guard lock(m_dvCapabilityMutex);
    fallback = m_dvFallbackActive;
  }
  const bool retry = drm && !fallback && drm->CanRetryDVActivation(true);
  if (!retry)
  {
    std::lock_guard lock(m_dvCapabilityMutex);
    if (m_dvRejectedRoute == DVBRIDGE::OutputRoute::NativeDV) m_dvNativeFailed = true;
    if (m_dvRejectedRoute == DVBRIDGE::OutputRoute::HDR10) m_dvHDR10Failed = true;
    m_dvFallbackActive = true;
    m_dvActivationReason = DVBRIDGE::OutputReason::ActivationFailed;
    m_dvFailedStream = m_dvActivationStream;
    m_dvFailedPolicy = m_dvActivationPolicy;
  }
  m_dvConfiguredOutput.reset();
  DVBRIDGE::StreamTelemetry::NativeFailure(DVBRIDGE::OutputReason::ActivationFailed,
      drm ? drm->DVActivationError() : 0,drm ? drm->DVActivationFailures() : 0,true);
  DVBRIDGE::StreamTelemetry::PublishNativePending(
      CServiceBroker::GetDataCacheCore().GetVideoProcessingGeneration());
  m_dvRecoveryPending = false;
  return true;
}

DVBRIDGE::OutputReason CWinSystemGbmGLESContext::ConsumeDVPlaybackFailure(DVBRIDGE::SourceBinding binding)
{
  std::lock_guard lock(m_dvCapabilityMutex);
  if (!binding.Valid() || binding.stream != m_dvFatalBinding.stream ||
      binding.revision != m_dvFatalBinding.revision || m_dvFatalConsumed)
    return DVBRIDGE::OutputReason::None;
  if (m_dvFatalReason != DVBRIDGE::OutputReason::None) m_dvFatalConsumed = true;
  return m_dvFatalReason;
}

bool CWinSystemGbmGLESContext::SupportsHDR10Conversion(bool requireCurrentMode)
{
  if (!IsHDRDisplay() || !GetDisplayHDRCapabilities().SupportsHDR10())
    return false;
  auto* connector = m_DRM->GetConnector();
  return connector && connector->IsConnected() && connector->SupportsProperty("HDR_OUTPUT_METADATA") &&
      (connector->SupportsProperty("DVBRIDGE_HDR10_DEEP_COLOR") || FloatOutputUsable()) &&
      SupportsDVOutputMode(requireCurrentMode);
}

bool CWinSystemGbmGLESContext::ConfigureHDR10Conversion(const VideoPicture& picture)
{
  if (!CompleteDVBridgePage()) return false;
  m_hdr10Signal.reset();
  const auto stream = picture.doviMetadata.Binding().stream;
  if (!stream) return false;
  if (m_failedHDR10SetupRestore && !EndDVBridge())
    return false;
  dvbridge_hdr10_session session{};
  if (m_hdr10Session && m_hdr10SessionStream == stream) session = *m_hdr10Session;
  else if (!dvbridge_hdr10_session_init(&session, picture.doviMetadata.Data(), picture.doviMetadata.Size()))
    return false;
  auto atomic = std::dynamic_pointer_cast<CDRMAtomic>(m_DRM);
  if (m_dvState.Captured() && (!atomic || !m_dvState.Stage(*atomic,true))) return false;
  VideoPicture output;
  output.SetParams(picture);
  output.color_transfer = AVCOL_TRC_SMPTE2084;
  output.color_primaries = AVCOL_PRI_BT2020;
  output.color_space = AVCOL_SPC_BT2020_NCL;
  output.colorBits = 10;
  const auto& metadata = session.output;
  if (metadata.max_luminance)
  {
    if (!output.hasDisplayMetadata)
      output.displayMetadata = {};
    output.hasDisplayMetadata = true;
    output.displayMetadata.has_luminance = metadata.max_luminance != 0;
    output.displayMetadata.max_luminance = {static_cast<int>(metadata.max_luminance), 1};
    output.displayMetadata.min_luminance = {static_cast<int>(metadata.min_luminance), 10000};
    output.hasLightMetadata = true;
    output.lightMetadata.MaxCLL = metadata.max_cll;
    output.lightMetadata.MaxFALL = metadata.max_fall;
  }
  // Every rejected setup after this point must restore ordinary HDR/color state.
  m_failedHDR10SetupRestore = true;
  SetColorimetry(&output);
  if (!SetHDR(&output))
    return false;
  auto drm = std::dynamic_pointer_cast<CDRMAtomic>(m_DRM);
  auto* connector = drm ? drm->GetConnector() : nullptr;
  if (!connector)
    return false;
  const auto previous = connector->GetPropertyValue("DVBRIDGE_HDR10_DEEP_COLOR");
  if (previous ? !drm->AddProperty(connector, "DVBRIDGE_HDR10_DEEP_COLOR", 1) : !FloatOutputUsable())
    return false;
  m_hdrDeepColorPrevious = previous;
  if (!BeginDVBridge() || !m_dvBridge->SetHDR10Session(session,stream,UseLimitedColor()))
  {
    if (previous && !drm->AddProperty(connector, "DVBRIDGE_HDR10_DEEP_COLOR", *previous))
    {
      drm->FlipPageTransactional(nullptr, true);
    }
    m_hdrDeepColorPrevious.reset();
    return false;
  }
  m_hdr10Session = session;
  m_hdr10SessionStream = stream;
  m_hdr10BasicSignal = std::make_unique<VideoPicture>();
  m_hdr10BasicSignal->color_transfer = output.color_transfer;
  m_hdr10BasicSignal->color_primaries = output.color_primaries;
  m_hdr10BasicSignal->color_space = output.color_space;
  m_hdr10BasicSignal->colorBits = output.colorBits;
  m_hdr10BasicSignal->hasDisplayMetadata = output.hasDisplayMetadata;
  m_hdr10BasicSignal->displayMetadata = output.displayMetadata;
  m_hdr10BasicSignal->hasLightMetadata = output.hasLightMetadata;
  m_hdr10BasicSignal->lightMetadata = output.lightMetadata;
  m_hdr10Signal = {metadata.max_luminance, metadata.min_luminance,
      metadata.max_cll, metadata.max_fall, ~0u};
  m_failedHDR10SetupRestore = false;
  if (DVBRIDGE::DiagnosticsEnabled())
  {
    DVBRIDGE::DiagnosticLog(LOGINFO, "DVBridge HDR10 configured: policy=Basic target={} nits origin={} curve=libplacebo-spline dynamic=L1-or-static inverse=false creative_trims=not-applied maxcll=unknown maxfall=unknown framebuffer_bits={} range={}",
        session.target.hdr.max_luma, session.output.level6 ? "RPU-L6" : "RPU-source-PQ",
        GetOutputBitDepth(), UseLimitedColor() ? "limited" : "full");
    if (m_hdrDeepColorPrevious)
      DVBRIDGE::DiagnosticLog(LOGINFO, "DVBridge HDR10: link minimum=10 bits; native HDMI bandwidth and format validation retained");
    else
      DVBRIDGE::DiagnosticLog(LOGINFO, "DVBridge HDR10: framebuffer_bits={} link_depth=driver-selected wire_depth=unknown", GetOutputBitDepth());
  }
  return true;
}

bool CWinSystemGbmGLESContext::RestoreFailedHDR10Setup()
{
  auto drm = std::dynamic_pointer_cast<CDRMAtomic>(m_DRM);
  if (!drm)
    return false;
  if (!StageOrdinaryHDR10Recovery())
  {
    drm->FlipPageTransactional(nullptr, true);
    return false;
  }
  glBindFramebuffer(GL_FRAMEBUFFER, 0);
  glDisable(GL_SCISSOR_TEST);
  glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
  glClearColor(0, 0, 0, 1);
  glClear(GL_COLOR_BUFFER_BIT);
  if (!m_dvBridge || !m_dvBridge->Advanced()) glFinish();
  if (!m_eglContext.TrySwapBuffers())
  {
    drm->FlipPageTransactional(nullptr, true);
    return false;
  }
  auto& surface = m_GBM->GetDevice().GetSurface();
  auto* bo = surface.LockTransactionalBuffer();
  const bool presented = drm->FlipPageTransactional(bo, true);
  surface.CompleteTransactionalBuffer(presented);
  if (!presented)
    return false;
  surface.EndTransactionalMode();
  CompleteOrdinaryHDR10Recovery();
  m_failedHDR10SetupRestore = false;
  return true;
}

bool CWinSystemGbmGLESContext::BeginDVBridge()
{
  if (!PollRejectedDVSurface())
  { DVBRIDGE::DiagnosticStage("output-surface-restoring"); return false; }
  if (m_dvShutdownFence && !RetireDVContext())
  { DVBRIDGE::DiagnosticStage("context-retiring"); return false; }
  const auto failure = [this](const char* stage, const char* detail = "none") {
    if (DVBRIDGE::DiagnosticsEnabled())
      DVBRIDGE::DiagnosticFailureLog(stage,
          m_dvSetupFailures.Record(DVBRIDGE::DiagnosticNowMs(), stage), LOGERROR,
          "DVBridge setup: stage={} detail={} failures={} size={}x{} depth={} renderable={}",
          stage, detail, m_dvSetupFailures.Count(), m_nWidth, m_nHeight,
          GetOutputBitDepth(), m_renderableType);
    return false;
  };
  const bool hdr10 = m_dvSelection.route == DVBRIDGE::OutputRoute::HDR10;
  if (!SupportsDVOutputMode()) return failure("output-format", "unsupported-current-mode");
  if (m_nWidth != 3840 || m_nHeight != 2160) return failure("output-format", "drawable-size");
  if (hdr10 ? GetOutputBitDepth() < 10 : GetOutputBitDepth() != 8)
    return failure("output-format", "drawable-depth");
  if (m_renderableType != EGL_OPENGL_ES3_BIT) return failure("output-format", "egl-renderable");
  if (m_dvBridge && m_dvConfiguredOutput == m_dvSelection.policy.mode &&
      (m_dvState.Captured() || m_hdrConversion))
    return m_dvBridge && m_guiCompositing && m_compositeShader;
  if (!CompleteDVBridgePage())
  { DVBRIDGE::DiagnosticStage("display-pending"); return false; }
  auto drm = std::dynamic_pointer_cast<CDRMAtomic>(m_DRM);
  if (!drm || (!drm->IsSelectedModeCommitted() &&
      m_modeHold.Holding() != CModeChangeHold::Kind::START))
    return failure("atomic-output-unavailable");
  CDVWindowRetirement::PollOnce();
  if (CDVBridgeGLES::Context::NewContextBlocked()) return failure("context-retained");
  if (!m_dvContext) m_dvContext = std::make_shared<CDVBridgeGLES::Context>();
  if (m_dvContext->GL() && m_dvContext->Poll() != DVBRIDGE_DV_IDLE)
    return failure("context-retiring");
  if (!m_dvBridge)
  {
    m_dvBridge = std::make_unique<CDVBridgeGLES>();
    if (!m_dvBridge->Initialize(false, hdr10,m_dvContext))
      return failure("renderer-init");
  }
  m_GBM->GetDevice().UseDVOutputLifetime();
  if (!hdr10 && !m_dvState.Captured())
  {
    if (!m_dvState.Capture(*drm))
      return failure("drm-state-capture", m_dvState.CaptureFailure());
    m_presentStall.Reset();
    if (DVBRIDGE::DiagnosticsEnabled())
      DVBRIDGE::DiagnosticLog(LOGINFO, "DVBridge TV-Led state captured: hdr_static_metadata={}",
          drm->GetConnector()->SupportsProperty("HDR_OUTPUT_METADATA") ? "preserved" : "not-exposed");
  }
  m_hdrConversion = hdr10;
  m_compositeShader.reset();
  const bool ready = SetGuiCompositing(AVCOL_TRC_SMPTE2084);
  if (ready) m_dvConfiguredOutput = m_dvSelection.policy.mode;
  return ready;
}

bool CWinSystemGbmGLESContext::SetVideoOutput(const VideoPicture* picture)
{
  if (picture && DVBRIDGE::DiagnosticsEnabled())
  {
    const auto binding = picture->doviSourceBinding.Valid() ? picture->doviSourceBinding : picture->doviMetadata.Binding();
    const auto preferences = DVBRIDGE::FrameOutputSettings();
    if (picture->doviSourceInfo.profile || picture->hdrType == StreamHdrType::HDR_TYPE_DOLBYVISION)
      DVBRIDGE::ObserveDiagnostic(binding.stream,binding.picture,binding.revision,
          preferences.revision,"output-admission",preferences.output != DVBRIDGE::OutputMode::Disabled,false);
  }
  if (picture && m_dvStopPending)
  {
    m_modeHold.Cancel();
    if (!StopDVBridgePlayback() || m_dvOrdinaryFrame) return false;
  }
  if (picture && !SupportsDVOutputMode() && DVBridgeRequested(picture))
    return true; // Configure/import first; the GUI owner must modeset before transport setup.
  if (!CompleteDVBridgePage()) return false;
  const auto preferences = DVBRIDGE::FrameOutputSettings();
  if (!RecoverDVActivation()) return false;
  ResetDVActivation(picture);
  const auto capabilities = GetDVOutputCapabilities(picture);
  if (picture && capabilities.isDV && preferences.output != DVBRIDGE::OutputMode::Disabled &&
      (!capabilities.processingKnown || (picture->strDVELType == "FEL" && !picture->doviEnhancement)))
    return true; // Configure must reach UploadTexture before source-dependent activation.
  m_dvSelection = SelectedDVOutput(picture);
  if (!PollRejectedDVSurface()) return false;
  const bool transaction = DVBridgeRequested(picture) || m_dvContext || m_dvState.Captured();
  if (transaction)
  {
    if (HasPendingDVOutputSurface()) return false;
    // Same-format policy/frame work bypasses this actual transport transition.
    const bool active = m_dvSelection.route == DVBRIDGE::OutputRoute::NativeDV ?
        m_dvState.Captured() : m_hdrConversion;
    m_dvActivating = DVBridgeRequested(picture) && (m_dvActivating || !active);
    if (auto drm = std::dynamic_pointer_cast<CDRMAtomic>(m_DRM))
      drm->SetDVActivationPending(m_dvActivating);
    if (!m_dvOutputRetiring)
    {
      m_dvPriorConfiguredOutput = m_dvState.Captured() || m_hdrConversion ?
          m_dvConfiguredOutput : std::nullopt;
      m_dvPriorHDRConversion = m_hdrConversion;
      m_dvPriorGuiCompositing = m_guiCompositing;
      m_dvPendingStream = picture ? (picture->doviSourceBinding.Valid() ?
          picture->doviSourceBinding.stream : picture->doviMetadata.Binding().stream) : 0;
      m_dvPendingPolicy = preferences.revision;
      if (m_dvBridge)
      {
        m_dvBridge->Cancel();
        // Cancelled readiness cannot cross an output-format transaction.
        m_cb1FramePending = m_dvFrame = m_dvNativeGui = false;
        m_dvOutputRetiring = true;
        if (!m_dvContext->FenceLastUse()) return false;
      }
    }
    if (m_dvContext && m_dvContext->GL() && m_dvContext->Poll() != DVBRIDGE_DV_IDLE)
      return false;
    if (!m_dvContext) m_dvContext = std::make_shared<CDVBridgeGLES::Context>();
    if (!m_dvContext->Initialize(false)) return false;
    m_dvOutputRetiring = false;
    m_GBM->GetDevice().UseDVOutputLifetime();
    UseDVOutputSurfaceTransaction(true);
    const bool staged = CWinSystemGbmEGLContext::SetVideoOutput(picture);
    UseDVOutputSurfaceTransaction(false);
    if (!staged) return false;
    m_hdr10Signal.reset();
    const bool requested = DVBridgeRequested(picture);
    const bool hdr10 = m_dvSelection.route == DVBRIDGE::OutputRoute::HDR10;
    if (requested && m_dvBridge && !m_dvBridge->RebindOutputSurface(hdr10))
    {
      RejectDVOutputSurface();
      return false;
    }
    m_hdrConversion = false;
    m_dvConfiguredOutput.reset();
    if (!requested && !StageOrdinaryDVOutput(picture)) return false;
    return true;
  }
  // Restore with the current surface before EGL replaces its buffers.
  if ((m_dvState.Captured() || m_hdrConversion || m_failedHDR10SetupRestore) && !EndDVBridge())
    return false;
  m_dvBridge.reset();
  return CWinSystemGbmEGLContext::SetVideoOutput(picture);
}

bool CWinSystemGbmGLESContext::PrepareDVOutputFrame(const VideoPicture& picture)
{
  if (DVBRIDGE::DiagnosticsEnabled())
  {
    const auto binding = picture.doviSourceBinding.Valid() ? picture.doviSourceBinding : picture.doviMetadata.Binding();
    const auto preferences = DVBRIDGE::FrameOutputSettings();
    if (picture.doviSourceInfo.profile || picture.hdrType == StreamHdrType::HDR_TYPE_DOLBYVISION)
      DVBRIDGE::ObserveDiagnostic(binding.stream,binding.picture,binding.revision,
          preferences.revision,"output-preparation",preferences.output != DVBRIDGE::OutputMode::Disabled,false);
  }
  if (m_dvStopPending) return false;
  if (!SupportsDVOutputMode() && DVBridgeRequested(&picture))
    return false; // A future-mode admission is not permission to activate at 1080p.
  if (m_dvRollbackFailed || !RecoverDVActivation())
  {
    if (m_dvRollbackFailed)
    {
      if (DVBRIDGE::StreamTelemetry::Read().transaction.reason != DVBRIDGE::OutputReason::RestoreFailed)
      {
        auto atomic=std::dynamic_pointer_cast<CDRMAtomic>(m_DRM);
        DVBRIDGE::StreamTelemetry::NativeFailure(DVBRIDGE::OutputReason::RestoreFailed,
            0,atomic ? atomic->DVActivationFailures() : 0,false);
        DVBRIDGE::StreamTelemetry::PublishNativePending(picture.doviSourceInfo.informationGeneration);
      }
      std::lock_guard lock(m_dvCapabilityMutex);
      if (!m_dvFatalConsumed) m_dvFatalReason = DVBRIDGE::OutputReason::RestoreFailed;
    }
    return false;
  }
  // A stopped file's GUI transaction must be presented before new video setup.
  if (m_dvOrdinaryFrame && HasPendingDVOutputSurface()) return false;
  ResetDVActivation(&picture);
  const auto preferences = DVBRIDGE::FrameOutputSettings();
  const auto capabilities = GetDVOutputCapabilities(&picture, preferences);
#ifdef CB1_HAS_HDR10_AI
  if ((!capabilities.isHDR10 || preferences.hdr10Source==DVBRIDGE::HDR10SourceMode::Native) &&
      m_dvContext && !m_dvContext->StopHDR10AI()) return false;
#endif
  if (capabilities.isDV && preferences.output != DVBRIDGE::OutputMode::Disabled &&
      (!capabilities.processingKnown || (picture.strDVELType == "FEL" && !picture.doviEnhancement)))
    return false;
  auto selectionCapabilities = capabilities;
  m_dvSelection = SelectedDVOutput(&picture, preferences, selectionCapabilities);
  // Retained renderer facts belong to this render-side phase, never Configure.
  const auto binding = picture.doviSourceBinding.Valid() ?
      picture.doviSourceBinding : picture.doviMetadata.Binding();
  if (capabilities.sourceKnown)
  {
    auto identity = m_dvBridge ? m_dvBridge->SelectionIdentity(binding, preferences, m_dvSelection.policy.mode) :
        DVBRIDGE::SourceBinding{};
    if (!identity.Valid() && binding.Valid()) identity = {binding.stream,binding.picture,preferences.revision};
    int requested=DVBRIDGE::PolicyCode(preferences.Policy());
#ifdef CB1_HAS_HDR10_AI
    if (capabilities.isHDR10) requested=static_cast<int>(preferences.HDR10Policy().mode);
#endif
    DVBRIDGE::StreamTelemetry::NativeSelection(identity, selectionCapabilities,
        requested,DVBRIDGE::PolicyCode(m_dvSelection.policy),m_dvSelection.reason);
    if (m_dvSelection.reason!=DVBRIDGE::OutputReason::None)
      DVBRIDGE::StreamTelemetry::PublishNativePending(picture.doviSourceInfo.informationGeneration);
  }
  if (auto drm = std::dynamic_pointer_cast<CDRMAtomic>(m_DRM))
    m_dvAttemptFailures = drm->DVActivationFailures();
  // Every selected automatic HDR10 fallback owns a single attempt for this file.
  if (capabilities.processingKnown && capabilities.isDV &&
      (m_dvSelection.route == DVBRIDGE::OutputRoute::HDR10 || !capabilities.nativeDV) &&
      preferences.output != DVBRIDGE::OutputMode::HDR10 &&
      preferences.output != DVBRIDGE::OutputMode::Disabled)
  {
    std::lock_guard lock(m_dvCapabilityMutex);
    m_dvNativeFailed = m_dvFallbackActive = true;
    if (m_dvActivationReason == DVBRIDGE::OutputReason::None)
      m_dvActivationReason = m_dvSelection.reason;
  }
  const auto& source = picture.doviSourceInfo;
  const bool ordinaryBL = !source.profile || source.profile == 7 ||
      source.compatibility == 1 || source.compatibility == 2 || source.compatibility == 4;
  if (m_dvSelection.route == DVBRIDGE::OutputRoute::Vanilla && capabilities.processingKnown &&
      !ordinaryBL &&
      preferences.output != DVBRIDGE::OutputMode::Disabled &&
      !(source.el && !picture.doviEnhancement))
  {
    std::lock_guard lock(m_dvCapabilityMutex);
    if (!m_dvFatalConsumed)
      m_dvFatalReason = m_dvSelection.reason == DVBRIDGE::OutputReason::None ?
          DVBRIDGE::OutputReason::SourceIneligible : m_dvSelection.reason;
    return false;
  }
  if (!PollRejectedDVSurface()) return false;
  // A format match cannot carry a fixed target across original source streams.
  if (m_dvSelection.route == DVBRIDGE::OutputRoute::HDR10 && m_hdr10Session &&
      m_hdr10SessionStream != picture.doviMetadata.Binding().stream)
    return ConfigureHDR10Conversion(picture);
  if (m_dvConfiguredOutput == m_dvSelection.policy.mode && !m_dvOutputRetiring &&
      (m_dvSelection.route == DVBRIDGE::OutputRoute::Vanilla || m_dvState.Captured() || m_hdrConversion))
    return true;
  const bool requested = DVBridgeRequested(&picture);
  if (requested && m_dvBridge && !HasPendingDVOutputSurface() &&
      (m_dvState.Captured() || m_hdrConversion) &&
      (m_dvSelection.route == DVBRIDGE::OutputRoute::HDR10) == m_hdrConversion)
  {
    // Same-format policies keep the renderer and its committed history intact.
    if (!CompleteDVBridgePage()) return false;
    m_dvConfiguredOutput = m_dvSelection.policy.mode;
    return true;
  }
  if (!HasPendingDVOutputSurface() && !SetVideoOutput(&picture)) return false;
  if (requested)
  {
    if (CDVBridgeGLES::Context::NewContextBlocked() ||
        (m_dvContext && m_dvContext->GL() && m_dvContext->Poll() != DVBRIDGE_DV_IDLE))
      return false;
    const bool ready = m_dvSelection.route == DVBRIDGE::OutputRoute::HDR10 ?
        ConfigureHDR10Conversion(picture) : BeginDVBridge();
    if (!ready) { RejectDVOutputSurface(); return false; }
  }
  else if (!StageOrdinaryDVOutput(&picture)) return false;
  m_dvConfiguredOutput = m_dvSelection.policy.mode;
  return true;
}

bool CWinSystemGbmGLESContext::StageOrdinaryDVOutput(const VideoPicture* picture)
{
  auto drm = std::dynamic_pointer_cast<CDRMAtomic>(m_DRM);
  if (!drm || (m_dvState.Captured() && !m_dvState.Stage(*drm,true)) ||
      (m_hdrDeepColorPrevious && !drm->AddProperty(drm->GetConnector(),
          "DVBRIDGE_HDR10_DEEP_COLOR",*m_hdrDeepColorPrevious)))
  { RejectDVOutputSurface(); return false; }
  SetColorimetry(picture);
  const bool hdr = SetHDR(picture);
  SetGuiCompositing(hdr && picture ? picture->color_transfer : 0);
  m_dvOrdinaryFrame = true;
  return true;
}

bool CWinSystemGbmGLESContext::RejectDVOutputSurface()
{
  m_hdr10Signal.reset();
  if (m_dvOrdinaryFrame)
  {
    m_dvRollbackFailed = true;
    std::lock_guard lock(m_dvCapabilityMutex);
    m_dvActivationReason = DVBRIDGE::OutputReason::RestoreFailed;
    return false;
  }
  if (!m_dvActivating && !HasPendingDVOutputSurface()) return true;
  if (!m_dvStopPending && !m_dvRecoveryPending)
  {
    m_dvRecoveryPending = true;
    m_dvRejectedRoute = m_dvSelection.route;
    if (auto drm = std::dynamic_pointer_cast<CDRMAtomic>(m_DRM);
        drm && drm->DVActivationFailures() == m_dvAttemptFailures)
      drm->FailDVActivation(EINVAL);
  }
  if (!HasPendingDVOutputSurface())
  {
    if (m_dvBridge) m_dvBridge->Cancel();
    if (!m_dvContext || !m_dvContext->FenceLastUse()) m_dvRollbackFailed = true;
    return !m_dvRollbackFailed;
  }
  if (!m_dvRejectedSurface)
  {
    m_dvRejectedSurface = true;
    m_dvConfiguredOutput.reset();
    if (m_dvBridge) m_dvBridge->Cancel();
    // The tail covers imports, resolve and the candidate's native swap commands.
    if (!m_dvContext || !m_dvContext->FenceLastUse()) m_dvRollbackFailed = true;
    if (!m_eglContext.BindPriorOutputSurface()) m_dvRollbackFailed = true;
  }
  return PollRejectedDVSurface();
}

bool CWinSystemGbmGLESContext::PollRejectedDVSurface()
{
  if (!m_dvRejectedSurface) return true;
  if (m_dvRollbackFailed || !m_dvContext ||
      m_dvContext->Poll() != DVBRIDGE_DV_IDLE) return false;
  if (!CompleteDVOutputSurface(false))
  {
    m_dvRollbackFailed = true;
    return false;
  }
  // Ordinary restoration may retain a 10-bit drawable without CB1 processing.
  if (m_dvBridge && m_dvPriorConfiguredOutput &&
      !m_dvBridge->RebindOutputSurface(m_dvPriorHDRConversion))
  {
    m_dvRollbackFailed = true;
    return false;
  }
  m_hdrConversion = m_dvPriorHDRConversion;
  m_hdr10Signal.reset();
  m_dvConfiguredOutput = m_dvPriorConfiguredOutput;
  SetGuiCompositing(m_dvPriorGuiCompositing ? AVCOL_TRC_SMPTE2084 : 0);
  m_dvRejectedSurface = false;
  return true;
}

bool KODI::WINDOWING::GBM::HasDVBridgeScreenshot(CWinSystemBase& window)
{
  auto* gbm = dynamic_cast<CWinSystemGbmGLESContext*>(&window);
  return gbm && gbm->HasDVBridgeScreenshot();
}

bool KODI::WINDOWING::GBM::ReadDVBridgeScreenshot(CWinSystemBase& window, KODI::RENDERING::CAPTURE::ReadbackBuffer& out)
{
  auto* gbm = dynamic_cast<CWinSystemGbmGLESContext*>(&window);
  return gbm && gbm->ReadDVBridgeScreenshot(out);
}

bool CWinSystemGbmGLESContext::CB1AsyncAvailable() const
{
  const auto drm = std::dynamic_pointer_cast<CDRMAtomic>(m_DRM);
  return m_eglFence && drm && drm->CanAsyncDVBridge() && m_dvBridge &&
      !m_dvBridge->Fallback() && !m_dvFallbackActive && !m_dvActivating &&
      !m_dvRecoveryPending && !m_dvRejectedSurface && !m_dvRollbackFailed &&
      !m_dvOutputRetiring && !m_dvShutdownFence && !m_dvStopPending && !HasPendingDVOutputSurface() &&
      m_dvSelection.route != DVBRIDGE::OutputRoute::Vanilla &&
      m_dvSelection.reason == DVBRIDGE::OutputReason::None &&
      (m_hdrConversion || m_dvState.Captured());
}

bool CWinSystemGbmGLESContext::CreateCB1GPUFence(const CDVBridgeGLES::Submission& submission)
{
  if (auto drm = std::dynamic_pointer_cast<CDRMAtomic>(m_DRM))
  {
    const double fps = CServiceBroker::GetDataCacheCore().GetVideoFps();
    const double refresh = m_DRM->GetCurrentMode().fRefreshRate;
    const auto preferences = DVBRIDGE::FrameOutputSettings();
    const bool matched = std::isfinite(fps) && fps >= 10 && fps <= 240 &&
        std::isfinite(refresh) && std::abs(fps-refresh) < fps * .005;
    const bool measure = preferences.fastRenderingMode == 2 && !m_fastRendering.Enabled() &&
        m_fastRenderEligible && matched;
    drm->SetCB1TimingPeriod(measure ? std::llround(1e9 / fps) : 0);
    m_fastSubmittedEligible = measure;
  }
#if defined(EGL_ANDROID_native_fence_sync) && defined(EGL_KHR_fence_sync)
  const auto current = m_dvBridge ? m_dvBridge->CaptureSubmission() : CDVBridgeGLES::Submission{};
  if (submission.asyncEligible && current.asyncEligible && submission.sequence &&
      submission.sequence == current.sequence && submission.target == current.target &&
      submission.resolveSerial == current.resolveSerial &&
      submission.identity.stream == current.identity.stream &&
      submission.identity.picture == current.identity.picture &&
      submission.identity.revision == current.identity.revision && CB1AsyncAvailable())
  {
    // The exported fence covers packing as well as source reconstruction.
    m_eglFence->CreateGPUFence();
    glFlush();
    const int fd = m_eglFence->FlushFence();
    if (fd >= 0)
    {
      const int prior = m_DRM->TakeOutFenceFd();
      if (prior >= 0) close(prior);
      m_DRM->SetInFenceFd(fd);
      return true;
    }
    glFinish();
  }
#endif
  return false;
}

void CWinSystemGbmGLESContext::ObserveCB1Headroom()
{
  const auto drm = std::dynamic_pointer_cast<CDRMAtomic>(m_DRM);
  if (!drm) return;
  const uint64_t presentation = drm->CB1PresentationTime();
  if (!presentation || presentation == m_fastObservedPresentation) return;
  m_fastObservedPresentation = presentation;
  const auto margin = drm->TakeCB1Headroom();
  const auto player = CServiceBroker::GetAppComponents().GetComponent<CApplicationPlayer>();
  const auto preferences = DVBRIDGE::FrameOutputSettings();
  const bool eligible = margin && m_fastSubmittedEligible && preferences.fastRenderingMode == 2 &&
      player && player->IsPlayingVideo() && player->GetPlaySpeed() == 1 &&
      !(m_guiWillRender && GetGUIElementCount()) && !m_dvOverlays;
  if (m_fastRendering.Enabled() && !DVBRIDGE::automaticFastRendering.load())
    m_fastRendering.Reset();
  if (m_fastRendering.Observe(presentation, margin.value_or(0), eligible))
  {
    DVBRIDGE::automaticFastRendering.store(true);
    if (m_dvSelection.policy.mode == DVBRIDGE_MODE_MASTER_REFERENCE)
      DVBRIDGE::PlaybackNotification::ArmMasterRecovery();
    DVBRIDGE::DiagnosticLog(LOGINFO, "CB1 fast rendering: decision=enable policy=Auto reason=low-presentation-headroom margin_ms={:.3f} threshold_ms=2.5",
        double(*margin) / 1e6);
  }
}

bool CWinSystemGbmGLESContext::RenderDVBridge(const CDVBridgeGLES::Layer& layer,
                                            std::shared_ptr<AVFrame> enhancement,
                                            DVBRIDGE::SourceBinding enhancementBinding,
                                            const CDVMetadataBuffer& metadata,
                                            dvbridge_geometry geometry,
                                            const CDVBridgeGLES::FELSuccessor* successor)
{
  // A delayed KMS handoff must not replace the one prepared next picture.
  if (m_cb1FramePending && m_dvFrame) return true;
  m_cb1FramePending = false;
  m_dvFrame = false;
  if (!BeginDVBridge())
    return false;
  const auto capture = CServiceBroker::GetCaptureService();
  const bool capturing = capture && !capture->TakeActive(
      KODI::RENDERING::CAPTURE::CaptureContent::COMPOSITE).empty();
  const bool masterCapture=capturing && m_dvSelection.policy.mode==DVBRIDGE_MODE_MASTER_REFERENCE;
  if (masterCapture) m_dvBridge->InvalidateScreenshot();
  const bool overlayFree = (!capturing || masterCapture) &&
      (!m_guiWillRender || GetGUIElementCount() == 0) && !m_dvOverlays;
  const auto player = CServiceBroker::GetAppComponents().GetComponent<CApplicationPlayer>();
  m_fastRenderEligible = !capturing && overlayFree && player && player->GetPlaySpeed() == 1 &&
      m_fastSourceStream == layer.binding.stream &&
      DVBRIDGE::ContinuousFastRenderingPicture(layer.pts, m_fastSourcePts) &&
      m_dvSelection.reason == DVBRIDGE::OutputReason::None;
  m_fastPreparedStream = layer.binding.stream;
  m_fastPreparedPts = layer.pts;
  const auto preferences = DVBRIDGE::FrameOutputSettings();
  const bool subtitlesOnly = !capturing && m_guiWillRender && m_guiFbo.IsValid() &&
      m_guiFboWidth == 3840 && m_guiFboHeight == 2160 && m_compositeShader &&
      m_dvSubtitleDraws && m_dvSubtitleDraws == GetGUIElementCount() &&
      (!m_dvPQSubtitleDraws || m_dvPQSubtitleDraws == m_dvSubtitleDraws);
  const bool pqSubtitles = subtitlesOnly && m_dvPQSubtitleDraws;
  const bool enhancedSubtitles = subtitlesOnly && metadata.Data() &&
      m_dvSelection.policy.mode == DVBRIDGE_MODE_ENHANCED_DV;
  const bool fullGUI = m_guiWillRender && GetGUIElementCount() &&
      CDVBridgeGLES::CanComposeSDRGUI(preferences.optimizedDV ||
          m_dvSelection.policy.mode == DVBRIDGE_MODE_MASTER_REFERENCE,m_hdrConversion,capturing && !masterCapture,
          m_guiFboWidth,m_guiFboHeight,m_dvPQSubtitleDraws);
  // The TV masks L5 margins. Keep controls/subtitles in letterbox bars visible.
  m_dvBridge->SetOverlayVisible((m_guiWillRender && GetGUIElementCount()) || m_dvOverlays);
  // Prime the subtitle shader once; empty frames retain the lean native shader.
  const bool nativeOverlay = !m_hdrConversion &&
      (fullGUI || subtitlesOnly || (overlayFree && !m_dvBridge->NativeOverlayPrimed())) &&
      m_guiFbo.IsValid() && m_guiFboWidth == 3840 && m_guiFboHeight == 2160 && m_compositeShader;
  const bool nativeSubtitles = m_dvBridge->SetNativeOverlay(
      nativeOverlay ? (pqSubtitles ? m_dvPQSubtitleFbo.Texture() : m_guiFbo.Texture()) : 0,
      nativeOverlay ? m_compositeShader->GetDegammaTexture() : 0,
      nativeOverlay ? m_compositeShader->GetTransferTexture() : 0,
      fullGUI && !enhancedSubtitles ? CRect(0,0,3840,2160) : m_dvSubtitleBounds,
      fullGUI && !enhancedSubtitles ? m_dvGUIContentGeneration : m_dvSubtitleContent, pqSubtitles);
  if (std::getenv("CB1_PRESENTATION_AUDIT"))
  {
    static unsigned prior = ~0u;
    const unsigned route = (overlayFree ? 1 : 0) | (nativeSubtitles ? 2 : 0) |
        (pqSubtitles ? 4 : 0) | (GetGUIElementCount() << 4) | (m_dvSubtitleDraws << 16);
    if (route != prior)
    {
      prior = route;
      DVBRIDGE::DiagnosticLog(LOGINFO, "CB1 subtitle route: gui={} subtitle_draws={} pq_draws={} gui_render={} free={} native={} bounds={},{},{},{}",
          GetGUIElementCount(), m_dvSubtitleDraws, m_dvPQSubtitleDraws, m_guiWillRender, overlayFree, nativeSubtitles,
          m_dvSubtitleBounds.x1, m_dvSubtitleBounds.y1, m_dvSubtitleBounds.x2, m_dvSubtitleBounds.y2);
    }
  }
  m_dvNativeGui = false;
  const auto policy = m_dvSelection.policy;
  const bool allowAsync = CB1AsyncAvailable() && !capturing;
#ifdef CB1_HAS_HDR10_AI
  const bool aiFrame = !metadata.Data();
  cb1_ai_result prediction{};
  if (aiFrame && m_dvBridge->PredictHDR10AI(layer.binding, preferences,
      DVBRIDGE::DiagnosticNowMs(), &prediction) != HDR10AISource::Decision::Prepare)
    return false;
#endif
  const auto prepare = [&]() {
#ifdef CB1_HAS_HDR10_AI
    if (aiFrame)
      return m_dvBridge->PrepareHDR10AIFrame(layer, geometry, preferences,
          prediction, policy, allowAsync, overlayFree || (preferences.optimizedDV && nativeSubtitles));
#endif
    return m_dvBridge->PrepareHardwareFrame(layer, enhancement, enhancementBinding,
        metadata, geometry, preferences, overlayFree || nativeSubtitles, &policy, allowAsync);
  };
  const double refresh = m_DRM->GetCurrentMode().fRefreshRate;
  const auto interval = std::chrono::duration<double>(refresh >= 10 ? 1.0 / refresh : 0.02);
  const auto preparationDeadline = std::chrono::steady_clock::now() +
      (capturing ? std::chrono::duration<double>(1.0) : interval);
  auto status = prepare();
  const auto preparedAt = std::chrono::steady_clock::now();
  const auto initialStatus = status;
  const bool prefetchFEL = successor && successor->frame && metadata.Data() &&
      policy.mode == DVBRIDGE_MODE_ENHANCED_DV && preferences.EffectiveFastChroma(policy.mode) &&
      allowAsync && m_dvContext->AsyncPreparation(layer.binding.stream) && m_fastRenderEligible &&
      overlayFree && geometry.source_width == 3840 && geometry.source_height == 2160 &&
      !geometry.x && !geometry.y && geometry.width == 3840 && geometry.height == 2160;
  if (!prefetchFEL)
    m_dvBridge->ClearFELPrefetch();
  else if (status == CDVBridgeGLES::FrameStatus::Pending)
    m_dvBridge->PrefetchHardwareFEL(*successor,preferences);
#ifdef CB1_HAS_HDR10_AI
  if (aiFrame)
  {
    // Future analysis may advance, but cannot withhold a ready current picture.
    for (;;)
    {
      const auto analysis = m_dvBridge->CollectHDR10AI();
      if (analysis != CB1_AI_READY && analysis != CB1_AI_PENDING)
        return false;
      const bool renderPending = status == CDVBridgeGLES::FrameStatus::Pending ||
          status == CDVBridgeGLES::FrameStatus::Busy;
      if ((!renderPending && status != CDVBridgeGLES::FrameStatus::Ready &&
           status != CDVBridgeGLES::FrameStatus::Reused) ||
          !renderPending || std::chrono::steady_clock::now() >= preparationDeadline)
        break;
      if (renderPending) status = prepare();
      if (status == CDVBridgeGLES::FrameStatus::Pending ||
          status == CDVBridgeGLES::FrameStatus::Busy)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
  }
  else
#endif
  if (capturing || (metadata.Data() && policy.mode == DVBRIDGE_MODE_ENHANCED_DV))
  {
    // Complete content analysis before Kodi advances this source picture.
    const auto deadline = preparationDeadline;
    while ((status == CDVBridgeGLES::FrameStatus::Pending ||
            status == CDVBridgeGLES::FrameStatus::Busy) &&
           std::chrono::steady_clock::now() < deadline)
    {
      if (status == CDVBridgeGLES::FrameStatus::Pending)
      {
        const auto remaining = std::chrono::duration_cast<std::chrono::nanoseconds>(
            deadline - std::chrono::steady_clock::now()).count();
        const uint64_t waitNs = prefetchFEL && remaining > 0 ?
            static_cast<uint64_t>(std::min<int64_t>(remaining,2000000)) : 0;
        if (prefetchFEL)
          status = m_dvBridge->PollStatus(waitNs);
        else if (m_dvBridge->PollReady())
          status = CDVBridgeGLES::FrameStatus::Ready;
      }
      else
      {
        status = prepare();
        if (prefetchFEL && status == CDVBridgeGLES::FrameStatus::Pending)
          m_dvBridge->PrefetchHardwareFEL(*successor,preferences);
      }
      if (status != CDVBridgeGLES::FrameStatus::Ready &&
          !(prefetchFEL && status == CDVBridgeGLES::FrameStatus::Pending))
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    // Completion during the last sleep still belongs to the current picture.
    if (status == CDVBridgeGLES::FrameStatus::Pending && m_dvBridge->PollReady())
      status = CDVBridgeGLES::FrameStatus::Ready;
  }
  // A paused final surface is already committed scanout, not a fresh resolve.
  const bool reused = status == CDVBridgeGLES::FrameStatus::Reused;
  m_dvFrame = status == CDVBridgeGLES::FrameStatus::Ready;
  m_dvNativeGui = (m_dvFrame || reused) && m_dvBridge->NativeOverlayComposed();
  if (m_dvFrame && m_hdrConversion)
  {
    if (const auto* policy = m_dvBridge->HDR10Output())
    {
      const std::array<unsigned, 5> signal{policy->hdr10.max_luminance,
          policy->hdr10.min_luminance, policy->hdr10.max_cll, policy->hdr10.max_fall,
          static_cast<unsigned>(policy->nominal_target.primaries)};
      // Stage static HDR metadata only when the output reference changes.
      if (m_hdr10Signal != signal)
      {
        VideoPicture output;
        output.color_transfer = AVCOL_TRC_SMPTE2084;
        output.color_primaries = AVCOL_PRI_BT2020;
        output.color_space = AVCOL_SPC_BT2020_NCL;
        output.colorBits = 10;
        output.hasDisplayMetadata = true;
        output.displayMetadata = {};
        output.displayMetadata.has_luminance = true;
        output.displayMetadata.max_luminance = {static_cast<int>(policy->hdr10.max_luminance),1};
        output.displayMetadata.min_luminance = {static_cast<int>(policy->hdr10.min_luminance),10000};
        const auto* primaries = pl_raw_primaries_get(policy->nominal_target.primaries);
        const auto coordinate = [](float value) {
          return AVRational{static_cast<int>(std::lround(value * 50000)), 50000};
        };
        output.displayMetadata.has_primaries = true;
        output.displayMetadata.display_primaries[0][0] = coordinate(primaries->red.x);
        output.displayMetadata.display_primaries[0][1] = coordinate(primaries->red.y);
        output.displayMetadata.display_primaries[1][0] = coordinate(primaries->green.x);
        output.displayMetadata.display_primaries[1][1] = coordinate(primaries->green.y);
        output.displayMetadata.display_primaries[2][0] = coordinate(primaries->blue.x);
        output.displayMetadata.display_primaries[2][1] = coordinate(primaries->blue.y);
        output.displayMetadata.white_point[0] = coordinate(primaries->white.x);
        output.displayMetadata.white_point[1] = coordinate(primaries->white.y);
        output.hasLightMetadata = true;
        output.lightMetadata = {policy->hdr10.max_cll,policy->hdr10.max_fall};
        m_dvFrame = SetHDR(&output);
        if (m_dvFrame) m_hdr10Signal = signal;
      }
    }
    else if (m_hdr10Session && m_hdr10BasicSignal)
    {
      const auto& metadata = m_hdr10Session->output;
      const std::array<unsigned, 5> signal{metadata.max_luminance, metadata.min_luminance,
          metadata.max_cll, metadata.max_fall, ~0u};
      if (m_hdr10Signal != signal)
      {
        m_dvFrame = SetHDR(m_hdr10BasicSignal.get());
        if (m_dvFrame) m_hdr10Signal = signal;
      }
    }
  }
  glBindFramebuffer(GL_FRAMEBUFFER, 0);
  const bool renderPending = status == CDVBridgeGLES::FrameStatus::Pending ||
      status == CDVBridgeGLES::FrameStatus::Busy;
  m_cb1FramePending = (metadata.Data() && policy.mode == DVBRIDGE_MODE_ENHANCED_DV && renderPending) ||
      (allowAsync && m_dvContext->AsyncPreparation(layer.binding.stream) && (m_dvFrame || renderPending));
  if (std::getenv("CB1_PRESENTATION_AUDIT"))
  {
    static unsigned samples=0,ready=0,pending=0,busy=0,repeats=0;
    static double submitMs=0,waitMs=0;
    static unsigned initialFailures=0;
    if (!m_dvFrame && initialFailures++<8)
      DVBRIDGE::DiagnosticLog(LOGINFO,"CB1 private preparation detail: initial={} final={} refresh={} mode={} authored={} capture={} submit_ms={:.3f} wait_ms={:.3f}",
          int(initialStatus),int(status),refresh,int(policy.mode),metadata.Data()!=nullptr,capturing,
          std::chrono::duration<double,std::milli>(preparedAt-(preparationDeadline-(capturing?std::chrono::duration<double>(1.0):interval))).count(),
          std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-preparedAt).count());
    ++samples; ready+=m_dvFrame; pending+=status==CDVBridgeGLES::FrameStatus::Pending;
    busy+=status==CDVBridgeGLES::FrameStatus::Busy; repeats+=reused;
    submitMs+=std::chrono::duration<double,std::milli>(preparedAt-(preparationDeadline-(capturing?std::chrono::duration<double>(1.0):interval))).count();
    waitMs+=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-preparedAt).count();
    if (samples==120)
    {
      DVBRIDGE::DiagnosticLog(LOGINFO,"CB1 private preparation: samples={} ready={} pending={} busy={} reused={} submit_ms={:.3f} wait_ms={:.3f}",samples,ready,pending,busy,repeats,submitMs/samples,waitMs/samples);
      samples=ready=pending=busy=repeats=0; submitMs=waitMs=0;
    }
  }
  return m_dvFrame || reused;
}

void CWinSystemGbmGLESContext::RecordDVSubtitle(const CRect& rect, unsigned texture)
{
  if (!m_guiCompositing || !m_guiWillRender || rect.IsEmpty())
    return;
  if (!m_dvSubtitleDraws++)
    m_dvSubtitleBounds = rect;
  else
    m_dvSubtitleBounds.Union(rect);
  // A reused glyph texture keeps a paused picture reusable. Position/fade edits rekey it.
  m_dvSubtitleContent = (m_dvSubtitleContent ^ texture) * 1099511628211ULL;
  for (float value : {rect.x1, rect.y1, rect.x2, rect.y2})
    m_dvSubtitleContent = (m_dvSubtitleContent ^ std::bit_cast<uint32_t>(value)) * 1099511628211ULL;
  if (m_dvPQSubtitleRendering)
    ++m_dvPQSubtitleDraws;
}

bool CWinSystemGbmGLESContext::BeginDVPQSubtitle()
{
  if (!HasDVBridgeScreenshot() || !m_guiCompositing || !m_guiWillRender)
    return false;
  glGetIntegerv(GL_FRAMEBUFFER_BINDING, &m_dvPQPreviousFbo);
  if (!m_dvPQSubtitleFbo.IsValid() || m_dvPQSubtitleWidth != m_guiFboWidth ||
      m_dvPQSubtitleHeight != m_guiFboHeight)
  {
    m_dvPQSubtitleFbo.Cleanup();
    if (!m_dvPQSubtitleFbo.Initialize() ||
        !m_dvPQSubtitleFbo.CreateAndBindToTexture(GL_TEXTURE_2D, m_guiFboWidth,
                                                m_guiFboHeight, GL_RGBA16F, GL_HALF_FLOAT) ||
        (GetEnabledFrontToBackRendering() &&
         !m_dvPQSubtitleFbo.AttachDepthBuffer(m_guiFboWidth, m_guiFboHeight)))
    {
      DVBRIDGE::DiagnosticLog(LOGERROR, "CB1: PQ subtitle framebuffer allocation failed");
      m_dvPQSubtitleFbo.Cleanup();
      glBindFramebuffer(GL_FRAMEBUFFER, m_dvPQPreviousFbo);
      return false;
    }
    m_dvPQSubtitleWidth = m_guiFboWidth;
    m_dvPQSubtitleHeight = m_guiFboHeight;
  }
  if (!m_dvPQSubtitleFbo.BeginRender())
  {
    glBindFramebuffer(GL_FRAMEBUFFER, m_dvPQPreviousFbo);
    return false;
  }
  if (!m_dvPQSubtitleCleared)
  {
    const bool scissor = glIsEnabled(GL_SCISSOR_TEST);
    glDisable(GL_SCISSOR_TEST);
    glClearColor(0, 0, 0, 0);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    if (scissor) glEnable(GL_SCISSOR_TEST);
    m_dvPQSubtitleCleared = true;
  }
  m_dvPQSubtitleRendering = true;
  return true;
}

void CWinSystemGbmGLESContext::EndDVPQSubtitle()
{
  m_dvPQSubtitleRendering = false;
  glBindFramebuffer(GL_FRAMEBUFFER, m_dvPQPreviousFbo);
}

void CWinSystemGbmGLESContext::DiscardDVBridgeFrame()
{
  // Discard unsubmitted work; accepted KMS buffers retain their retirement owner.
  if (m_dvBridge) m_dvBridge->Cancel();
  m_cb1FramePending = m_dvFrame = m_dvNativeGui = false;
}

bool CWinSystemGbmGLESContext::StopDVBridgePlayback()
{
  if (!m_dvStopPending && HoldCB1Stop())
  {
    m_dvStopPending=true;
    return false;
  }
  if (m_modeHold.Holding() == CModeChangeHold::Kind::STOP &&
      !m_modeHold.Commits(DVBRIDGE::DiagnosticNowMs())) return false;
  m_fastRendering.Reset();
  DVBRIDGE::automaticFastRendering.store(false);
  m_fastRenderEligible = m_fastSubmittedEligible = false;
  m_fastSourceStream = m_fastObservedPresentation = 0;
  m_fastSourcePts = 0;
  if (auto drm = std::dynamic_pointer_cast<CDRMAtomic>(m_DRM)) drm->SetCB1TimingPeriod(0);
  // The window outlives the renderer when a pending scanout defers teardown.
  m_dvStopPending = true;
  // Cancel an unpresented startup drawable with its existing GPU retirement.
  if (HasPendingDVOutputSurface() && !m_dvOrdinaryFrame && !RejectDVOutputSurface())
    return false;
  if (!SetVideoOutput(nullptr)) return false;
  if (!m_dvOrdinaryFrame) m_dvStopPending = false;
  return true;
}

bool CWinSystemGbmGLESContext::EndDVBridge()
{
  if (!CompleteDVBridgePage())
  {
    m_dvEndPending = true;
    return false;
  }
  if (m_failedHDR10SetupRestore && !RestoreFailedHDR10Setup())
    return false;
  if (m_hdrDeepColorPrevious)
  {
    auto drm = std::dynamic_pointer_cast<CDRMAtomic>(m_DRM);
    if (!drm)
      return false;
    if (!drm->AddProperty(drm->GetConnector(), "DVBRIDGE_HDR10_DEEP_COLOR",
                          *m_hdrDeepColorPrevious))
    {
      drm->FlipPageTransactional(nullptr, true);
      return false;
    }
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glDisable(GL_SCISSOR_TEST);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    if (!m_dvBridge || !m_dvBridge->Advanced()) glFinish();
    if (!m_eglContext.TrySwapBuffers())
      return false;
    auto& surface = m_GBM->GetDevice().GetSurface();
    auto* bo = surface.LockTransactionalBuffer();
    AnnounceCB1Mode(false);
    const bool presented = drm->FlipPageTransactional(bo, true);
    CompleteCB1Mode(presented,false);
    surface.CompleteTransactionalBuffer(presented);
    if (!presented)
      return false;
    surface.EndTransactionalMode();
    m_hdrDeepColorPrevious.reset();
    if (DVBRIDGE::DiagnosticsEnabled())
      DVBRIDGE::DiagnosticLog(LOGINFO, "DVBridge HDR10: native link policy restored");
  }
  m_cb1FramePending = m_dvFrame = false;
  m_presentStall.Reset();
  // Never reset Kodi's ordinary HDR GUI compositor unless we owned DV output.
  if (!m_dvState.Captured() && !m_hdrConversion)
  {
    m_dvEndPending = false;
    return true;
  }
  if (m_dvState.Captured())
  {
    // Restore signaling with a clean ordinary framebuffer, never tunnel pixels.
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glDisable(GL_SCISSOR_TEST);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    if (!m_dvBridge || !m_dvBridge->Advanced()) glFinish();
    AnnounceCB1Mode(false);
    const bool restored=m_eglContext.TrySwapBuffers() && FlipDVBridgePage(m_dvState, true);
    CompleteCB1Mode(restored,false);
    if (!restored)
    {
      if (DVBRIDGE::DiagnosticsEnabled() &&
          m_dvSetupFailures.Record(DVBRIDGE::DiagnosticNowMs(), "restore"))
        DVBRIDGE::DiagnosticLog(LOGERROR, "DVBridge: output restoration failed; retaining scanout state; failures={}",
            m_dvSetupFailures.Count());
      return false;
    }
  }
  if (m_hdrConversion)
    m_dvBridge.reset();
  else if (m_dvBridge)
    m_dvBridge->Reset();
  m_hdr10Session.reset();
  m_hdr10BasicSignal.reset();
  m_hdr10Signal.reset();
  m_hdr10SessionStream = 0;
  m_hdrConversion = false;
  if (DVBRIDGE::DiagnosticsEnabled() &&
      m_dvSetupFailures.Recovered(DVBRIDGE::DiagnosticNowMs()))
    DVBRIDGE::DiagnosticLog(LOGINFO, "DVBridge: normal display restored after failures={}", m_dvSetupFailures.Count());
  SetGuiCompositing(0);
  m_dvEndPending = false;
  return true;
}

bool CWinSystemGbmGLESContext::DestroyWindowSystem()
{
  m_modeHold.Cancel();
  if (!EndDVBridge())
    return false;
  m_dvBridge.reset();
  if (!RetireDVContext()) return false;
  if (!CWinSystemGbmEGLContext::DestroyWindowSystem()) return false;
  std::lock_guard lock(m_dvCapabilityMutex);
  const auto generation = m_dvCapabilities.generation + 1;
  m_dvCapabilities = {};
  m_dvAvailableNativeDV = m_dvAvailableHDR10 = false;
  m_dvCapabilities.generation = generation;
  m_dvImportBinding = {};
  return true;
}

bool CWinSystemGbmGLESContext::RetireDVContext()
{
  if (!CompleteDVBridgePage()) return false;
  if (m_dvBridge) m_dvBridge->Cancel();
  m_dvBridge.reset();
  if (!m_dvContext || !m_dvContext->GL()) return true;
  if (!m_dvShutdownFence)
  {
    m_dvShutdownFence = true;
    if (!m_dvContext->FenceLastUse()) return false;
  }
  if (m_dvContext->Poll() != DVBRIDGE_DV_IDLE || !m_dvContext->Close()) return false;
  // Retain the per-video setting snapshot across graphics-context recreation.
  m_dvShutdownFence = false;
  return true;
}

bool CWinSystemGbmGLESContext::DestroyWindow()
{
  m_modeHold.Cancel();
  if (!EndDVBridge() || !RetireDVContext()) return false;
  // A resize can precede presentation of the ordinary successor surface.
  if (HasPendingDVOutputSurface() &&
      (!m_dvOrdinaryFrame || !CompleteDVOutputSurface(false))) return false;
  m_dvOrdinaryFrame = m_dvStopPending = false;
  if (!CWinSystemGbmEGLContext::DestroyWindow()) return false;
  ObserveDVImport({}, {}, false);
  return true;
}
#endif

bool CWinSystemGbmGLESContext::CreateContext()
{
  const EGLint version = (m_renderableType == EGL_OPENGL_ES3_BIT) ? 3 : 2;

  CEGLAttributesVec contextAttribs;
  contextAttribs.Add({{EGL_CONTEXT_CLIENT_VERSION, version}});

  if (!m_eglContext.CreateContext(contextAttribs))
  {
    CLog::Log(LOGERROR, "EGL context creation failed");
    return false;
  }
#ifdef HAVE_DVBRIDGE
  {
    std::lock_guard lock(m_dvCapabilityMutex);
    const auto generation = m_dvCapabilities.generation + 1;
    m_dvCapabilities = {};
    m_dvAvailableNativeDV = m_dvAvailableHDR10 = false;
    m_dvCapabilities.generation = generation;
    m_dvImportBinding = {};
    m_dvImportUsable = false;
  }
#endif
  return true;
}
