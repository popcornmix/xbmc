/*
 *  Copyright (C) 2017-2018 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#include "RendererDRMPRIME.h"

#include "ServiceBroker.h"
#include "cores/VideoPlayer/Buffers/VideoBufferDRMPRIME.h"
#include "cores/VideoPlayer/DVDCodecs/Video/DVDVideoCodec.h"
#include "cores/VideoPlayer/VideoRenderers/HwDecRender/DRMPRIMECaptureGLES.h"
#include "cores/VideoPlayer/VideoRenderers/HwDecRender/VideoLayerBridgeDRMPRIME.h"
#include "cores/VideoPlayer/VideoRenderers/RenderFactory.h"
#include "cores/VideoPlayer/VideoRenderers/RenderFlags.h"
#include "settings/DisplaySettings.h"
#include "settings/Settings.h"
#include "settings/SettingsComponent.h"
#include "settings/lib/Setting.h"
#include "utils/log.h"
#include "windowing/GraphicContext.h"
#include "windowing/gbm/WinSystemGbm.h"
#include "windowing/gbm/drm/DRMAtomic.h"

#include <algorithm>
#include <array>
#include <span>

using namespace KODI::WINDOWING::GBM;

CRendererDRMPRIME::~CRendererDRMPRIME()
{
  Flush(false);

  auto* winSystem = static_cast<CWinSystemGbm*>(CServiceBroker::GetWinSystem());

  // Clear the scanout colorspace and HDR metadata set during Configure so
  // the GUI after playback falls back to Default / SDR.
  winSystem->SetGuiCompositing(false);
  winSystem->SetHDR(nullptr);
  winSystem->SetColorimetry(nullptr);
}

CBaseRenderer* CRendererDRMPRIME::Create(CVideoBuffer* buffer)
{
  auto settings = CServiceBroker::GetSettingsComponent()->GetSettings();
  if (!settings->GetBool(CSettings::SETTING_VIDEOPLAYER_USEPRIMEDECODER))
    return nullptr;

  if (buffer && settings->GetInt(CSettings::SETTING_VIDEOPLAYER_USEPRIMERENDERER) == 0)
  {
    auto buf = dynamic_cast<CVideoBufferDRMPRIME*>(buffer);
    if (!buf)
      return nullptr;

    auto winSystem = static_cast<CWinSystemGbm*>(CServiceBroker::GetWinSystem());
    if (!winSystem)
      return nullptr;

    auto drm = std::static_pointer_cast<CDRMAtomic>(winSystem->GetDrm());
    if (!drm)
      return nullptr;

    if (!buf->AcquireDescriptor())
      return nullptr;

    AVDRMFrameDescriptor* desc = buf->GetDescriptor();
    if (!desc)
    {
      buf->ReleaseDescriptor();
      return nullptr;
    }

    AVDRMLayerDescriptor* layer = &desc->layers[0];
    uint32_t format = layer->format;
    uint64_t modifier = desc->objects[0].format_modifier;
    uint64_t width = buf->GetWidth();
    uint64_t height = buf->GetHeight();

    buf->ReleaseDescriptor();

    auto gui = drm->GetGuiPlane();
    if (!gui)
      return nullptr;

    if (!drm->FindVideoAndGuiPlane(format, modifier, width, height))
      return nullptr;

    return new CRendererDRMPRIME();
  }

  return nullptr;
}

void CRendererDRMPRIME::Register()
{
  CWinSystemGbm* winSystem = dynamic_cast<CWinSystemGbm*>(CServiceBroker::GetWinSystem());
  if (winSystem && std::dynamic_pointer_cast<CDRMAtomic>(winSystem->GetDrm()))
  {
    CServiceBroker::GetSettingsComponent()
        ->GetSettings()
        ->GetSetting(CSettings::SETTING_VIDEOPLAYER_USEPRIMERENDERER)
        ->SetVisible(true);
    VIDEOPLAYER::CRendererFactory::RegisterRenderer("drm_prime", CRendererDRMPRIME::Create);
    return;
  }
}

bool CRendererDRMPRIME::Configure(const VideoPicture& picture, float fps, unsigned int orientation)
{
  m_format = picture.videoBuffer->GetFormat();
  m_sourceWidth = picture.iWidth;
  m_sourceHeight = picture.iHeight;
  m_renderOrientation = orientation;

  m_iFlags = GetFlagsChromaPosition(picture.chroma_position) |
             GetFlagsColorMatrix(picture.color_space, picture.iWidth, picture.iHeight) |
             GetFlagsColorPrimaries(picture.color_primaries) |
             GetFlagsStereoMode(picture.stereoMode);

  // Signal source colorimetry and HDR metadata on the scanout via the DRM
  // Colorspace and HDR_OUTPUT_METADATA connector properties. The direct-to-
  // plane scanout path bypasses GL video rendering entirely.
  if (auto* winSystem = CServiceBroker::GetWinSystem())
  {
    winSystem->SetColorimetry(&picture);

    const bool passthroughHDR = winSystem->SetHDR(&picture);
    CLog::Log(LOGDEBUG, "CRendererDRMPRIME::Configure: HDR passthrough: {}",
              passthroughHDR ? "on" : "off");

    const bool hdrFboActive =
        passthroughHDR && winSystem->SetGuiCompositing(picture.color_transfer);
    if (passthroughHDR && !hdrFboActive)
      CLog::Log(LOGWARNING, "CRendererDRMPRIME::Configure: HDR passthrough active but "
                            "GUI compositing not supported by windowing system");
  }

  // A stereoscopic source needs one plane per eye; without a second plane the
  // whole packed frame is scanned out on the single plane instead.
  m_stereoPlaneWanted = CachePlaneParams(picture) && CONF_FLAGS_STEREO_MODE_MASK(m_iFlags) != 0;

  // Calculate the input frame aspect ratio.
  CalculateFrameAspectRatio(picture.iDisplayWidth, picture.iDisplayHeight);
  SetViewMode(m_videoSettings.m_ViewMode);
  ManageRenderArea();

  Flush(false);

  m_bConfigured = true;
  return true;
}

bool CRendererDRMPRIME::CachePlaneParams(const VideoPicture& picture)
{
  auto* buffer = dynamic_cast<CVideoBufferDRMPRIME*>(picture.videoBuffer);
  if (!buffer || !buffer->AcquireDescriptor())
    return false;

  const AVDRMFrameDescriptor* desc = buffer->GetDescriptor();
  if (desc)
  {
    m_planeParams.format = desc->layers[0].format;
    m_planeParams.modifier = desc->objects[0].format_modifier;
    m_planeParams.width = buffer->GetWidth();
    m_planeParams.height = buffer->GetHeight();
  }
  buffer->ReleaseDescriptor();

  return desc != nullptr;
}

void CRendererDRMPRIME::EnsurePlanes()
{
  auto* winSystem = dynamic_cast<CWinSystemGbm*>(CServiceBroker::GetWinSystem());
  if (!winSystem || m_planeParams.format == 0)
    return;

  auto drm = winSystem->GetDrm();

  // A display mode change re-runs FindGuiPlane(), whose "reuse the existing gui
  // plane" path drops the video plane - it assumes the single-plane model where
  // one plane flips between the gui and video roles. Direct-to-Plane needs both,
  // so claim the pair again or nothing is ever scanned out on the video plane.
  if (!drm->GetVideoPlane())
    drm->FindVideoAndGuiPlane(m_planeParams.format, m_planeParams.modifier, m_planeParams.width,
                              m_planeParams.height);

  // Same for the second video plane. Give up for good on failure rather than
  // searching the planes on every frame.
  if (m_stereoPlaneWanted && !drm->GetVideoPlane2())
    m_stereoPlaneWanted = drm->FindSecondVideoPlane(m_planeParams.format, m_planeParams.modifier,
                                                    m_planeParams.width, m_planeParams.height);
}

bool CRendererDRMPRIME::SetStereoPlaneGeometry()
{
  auto& gfxContext = CServiceBroker::GetWinSystem()->GetGfxContext();
  const RenderStereoMode stereoMode = gfxContext.GetStereoMode();
  if (stereoMode != RenderStereoMode::SPLIT_VERTICAL &&
      stereoMode != RenderStereoMode::SPLIT_HORIZONTAL)
    return false;

  auto* winSystem = dynamic_cast<CWinSystemGbm*>(CServiceBroker::GetWinSystem());
  if (!winSystem || !winSystem->GetDrm()->GetVideoPlane2())
    return SetPackedPlaneGeometry(stereoMode);

  // One plane per eye. CBaseRenderer::ManageRenderArea() crops the source to the
  // eye of the current view and fits it to the halved screen GetResInfo()
  // reports for a split mode, and StereoCorrection() then places that half where
  // the sink expects it - below the active space gap for frame packing. So both
  // the source layout and the output layout are handled by the shared code, and
  // a side-by-side source on a top-and-bottom display just works.
  const RenderStereoView view = gfxContext.GetStereoView();

  gfxContext.SetStereoView(RenderStereoView::LEFT);
  CBaseRenderer::ManageRenderArea();
  m_planeSourceRect = m_sourceRect;
  m_planeDestRect = gfxContext.StereoCorrection(m_destRect);

  gfxContext.SetStereoView(RenderStereoView::RIGHT);
  CBaseRenderer::ManageRenderArea();
  m_planeSourceRect2 = m_sourceRect;
  m_planeDestRect2 = gfxContext.StereoCorrection(m_destRect);

  // Restore the view, and with it m_sourceRect / m_destRect for GetVideoRect().
  gfxContext.SetStereoView(view);
  CBaseRenderer::ManageRenderArea();

  // Those rects are in GUI coordinates - the resolution's iWidth x iHeight - but
  // a plane scans out in physical coordinates, iScreenWidth x iScreenHeight. The
  // two differ when the GUI is rendered smaller than the mode (limitguisize),
  // where the GUI plane is scaled up to the mode; without scaling the eyes would
  // cover only the GUI-sized corner of the screen.
  const RESOLUTION_INFO base =
      CDisplaySettings::GetInstance().GetResolutionInfo(gfxContext.GetVideoResolution());
  if (base.iWidth > 0 && base.iHeight > 0 &&
      (base.iScreenWidth != base.iWidth || base.iScreenHeight != base.iHeight))
  {
    const float scaleX = static_cast<float>(base.iScreenWidth) / static_cast<float>(base.iWidth);
    const float scaleY = static_cast<float>(base.iScreenHeight) / static_cast<float>(base.iHeight);
    const auto toScanout = [scaleX, scaleY](CRect& rect)
    {
      rect.x1 *= scaleX;
      rect.x2 *= scaleX;
      rect.y1 *= scaleY;
      rect.y2 *= scaleY;
    };

    toScanout(m_planeDestRect);
    toScanout(m_planeDestRect2);
  }

  m_planeCount = 2;
  return true;
}

bool CRendererDRMPRIME::SetPackedPlaneGeometry(RenderStereoMode stereoMode)
{
  // Fallback with only one plane: it presents one buffer per commit and so
  // cannot build a split, which needs two independent source->dest mappings.
  // Scan out the whole packed frame across the whole screen instead - a 3D
  // display consumes it directly. Frame packing comes out misaligned this way,
  // as the active space gap cannot be reproduced by one uniform mapping.
  m_planeSourceRect =
      CRect(0.0f, 0.0f, static_cast<float>(m_sourceWidth), static_cast<float>(m_sourceHeight));

  // One uniform mapping means the per-eye aspect can only be corrected on the
  // axis both eyes share: vertically for side-by-side, horizontally for
  // top-and-bottom. The other axis must span the full screen, or the eye
  // boundary moves off the half the display expects and both eyes misalign.
  const RESOLUTION_INFO info = CServiceBroker::GetWinSystem()->GetGfxContext().GetResInfo();
  const float screenWidth = static_cast<float>(info.iScreenWidth);
  const float screenHeight = static_cast<float>(info.iScreenHeight);
  const float zoom = CDisplaySettings::GetInstance().GetZoomAmount();
  const float aspect = GetPerEyeAspectRatio() * CDisplaySettings::GetInstance().GetPixelRatio();

  float width = screenWidth;
  float height = screenHeight;
  if (aspect > 0.0f)
  {
    if (stereoMode == RenderStereoMode::SPLIT_VERTICAL)
      height = std::min(screenHeight, screenWidth / aspect * zoom);
    else
      width = std::min(screenWidth, screenHeight * aspect * zoom);
  }

  const float x = (screenWidth - width) * 0.5f;
  const float y = (screenHeight - height) * 0.5f;
  m_planeDestRect = CRect(x, y, x + width, y + height);

  m_planeCount = 1;
  return true;
}

void CRendererDRMPRIME::ManageRenderArea()
{
  CBaseRenderer::ManageRenderArea();

  EnsurePlanes();

  if (SetStereoPlaneGeometry())
    return;

  // Whole frame, except for "watch as 2D" (MONO) of a stereoscopic source, where
  // CBaseRenderer::ManageRenderArea() has cropped it to the eye to show.
  m_planeCount = 1;
  m_planeSourceRect = m_sourceRect;

  RESOLUTION_INFO info = CServiceBroker::GetWinSystem()->GetGfxContext().GetResInfo();
  if (info.iScreenWidth != info.iWidth)
  {
    CalcDestRect(0, 0, info.iScreenWidth, info.iScreenHeight,
                 GetAspectRatio() * CDisplaySettings::GetInstance().GetPixelRatio(),
                 CDisplaySettings::GetInstance().GetZoomAmount(),
                 CDisplaySettings::GetInstance().GetVerticalShift(), m_planeDestRect);
  }
  else
  {
    m_planeDestRect = m_destRect;
  }
}

void CRendererDRMPRIME::AddVideoPicture(const VideoPicture& picture, int index)
{
  BUFFER& buf = m_buffers[index];
  if (buf.videoBuffer)
  {
    CLog::LogF(LOGERROR, "unreleased video buffer");
    buf.videoBuffer->Release();
  }
  buf.videoBuffer = picture.videoBuffer;
  buf.videoBuffer->Acquire();

  // CDVDVideoCodecDRMPRIME fills its buffers at decode; CVideoBufferDMA arrives unfilled
  auto* drmBuffer = dynamic_cast<CVideoBufferDRMPRIME*>(picture.videoBuffer);
  if (drmBuffer && !dynamic_cast<CVideoBufferDRMPRIMEFFmpeg*>(drmBuffer))
    drmBuffer->SetPictureParams(picture);
}

bool CRendererDRMPRIME::Flush(bool saveBuffers)
{
  if (!saveBuffers)
    for (int i = 0; i < NUM_BUFFERS; i++)
      ReleaseBuffer(i);

  m_iLastRenderBuffer = -1;
  return saveBuffers;
}

void CRendererDRMPRIME::ReleaseBuffer(int index)
{
  BUFFER& buf = m_buffers[index];
  if (buf.videoBuffer)
  {
    buf.videoBuffer->Release();
    buf.videoBuffer = nullptr;
  }
}

bool CRendererDRMPRIME::NeedBuffer(int index)
{
  if (m_iLastRenderBuffer == index)
    return true;

  return false;
}

bool CRendererDRMPRIME::CaptureVideoFrame(const KODI::RENDERING::CAPTURE::CaptureSpec& spec,
                                          KODI::RENDERING::CAPTURE::CaptureResult& result)
{
  if (m_iLastRenderBuffer < 0)
    return false;

  auto* buffer = dynamic_cast<CVideoBufferDRMPRIME*>(m_buffers[m_iLastRenderBuffer].videoBuffer);
  if (!buffer || !buffer->IsValid())
    return false;

  return CaptureDRMPRIMEVideo(buffer, spec, result);
}

CRenderInfo CRendererDRMPRIME::GetRenderInfo()
{
  CRenderInfo info;
  info.max_buffer_size = NUM_BUFFERS;
  return info;
}

void CRendererDRMPRIME::Update()
{
  if (!m_bConfigured)
    return;

  ManageRenderArea();
}

void CRendererDRMPRIME::RenderUpdate(
    int index, int index2, bool clear, unsigned int flags, unsigned int alpha)
{
  if (m_iLastRenderBuffer == index && m_videoLayerBridge)
  {
    m_videoLayerBridge->UpdateVideoPlane();
    return;
  }

  CVideoBufferDRMPRIME* buffer = dynamic_cast<CVideoBufferDRMPRIME*>(m_buffers[index].videoBuffer);
  if (!buffer || !buffer->IsValid())
    return;

  if (!m_videoLayerBridge)
  {
    CWinSystemGbm* winSystem = static_cast<CWinSystemGbm*>(CServiceBroker::GetWinSystem());
    m_videoLayerBridge =
        std::dynamic_pointer_cast<CVideoLayerBridgeDRMPRIME>(winSystem->GetVideoLayerBridge());
    if (!m_videoLayerBridge)
      m_videoLayerBridge = std::make_shared<CVideoLayerBridgeDRMPRIME>(
          std::dynamic_pointer_cast<CDRMAtomic>(winSystem->GetDrm()));
    winSystem->RegisterVideoLayerBridge(m_videoLayerBridge);
  }

  if (m_iLastRenderBuffer == -1)
    m_videoLayerBridge->Configure(buffer);

  const std::array<CVideoLayerBridgeDRMPRIME::PlaneRects, 2> rects{
      {{m_planeSourceRect, m_planeDestRect}, {m_planeSourceRect2, m_planeDestRect2}}};
  m_videoLayerBridge->SetVideoPlane(buffer, std::span(rects).first(m_planeCount));

  m_iLastRenderBuffer = index;
}

bool CRendererDRMPRIME::ConfigChanged(const VideoPicture& picture)
{
  if (picture.videoBuffer->GetFormat() != m_format)
    return true;

  return false;
}

bool CRendererDRMPRIME::Supports(ERENDERFEATURE feature) const
{
  switch (feature)
  {
    case RENDERFEATURE_STRETCH:
    case RENDERFEATURE_ZOOM:
    case RENDERFEATURE_VERTICAL_SHIFT:
    case RENDERFEATURE_PIXEL_RATIO:
      return true;
    default:
      return false;
  }
}

bool CRendererDRMPRIME::Supports(ESCALINGMETHOD method) const
{
  return false;
}
