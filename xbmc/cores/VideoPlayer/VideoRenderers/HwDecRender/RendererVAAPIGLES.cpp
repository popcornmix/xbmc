/*
 *  Copyright (C) 2007-2018 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#include "RendererVAAPIGLES.h"

#include "../RenderFactory.h"
#include "cores/VideoPlayer/DVDCodecs/DVDCodecUtils.h"
#include "cores/VideoPlayer/DVDCodecs/Video/VAAPI.h"
#include "settings/AdvancedSettings.h"
#include "settings/Settings.h"
#include "utils/EGLFence.h"
#include "utils/GLUtils.h"
#include "utils/log.h"
#ifdef HAVE_DVBRIDGE
#include "ServiceBroker.h"
#include "windowing/gbm/WinSystemGbmGLESContext.h"
#include <cmath>
#endif

#include <memory>

using namespace VAAPI;
using namespace KODI::UTILS::EGL;

IVaapiWinSystem* CRendererVAAPIGLES::m_pWinSystem = nullptr;

CBaseRenderer* CRendererVAAPIGLES::Create(CVideoBuffer* buffer)
{
  CVaapiRenderPicture *vb = dynamic_cast<CVaapiRenderPicture*>(buffer);
  if (vb)
    return new CRendererVAAPIGLES();

  return nullptr;
}

void CRendererVAAPIGLES::Register(IVaapiWinSystem* winSystem,
                                  VADisplay vaDpy,
                                  EGLDisplay eglDisplay,
                                  bool& general,
                                  bool& deepColor)
{
  general = deepColor = false;

  int major_version, minor_version;
  if (vaInitialize(vaDpy, &major_version, &minor_version) != VA_STATUS_SUCCESS)
  {
    vaTerminate(vaDpy);
    return;
  }

  // Probe importable surface formats via vaExportSurfaceHandle.
  CCapabilities& caps = CDecoder::GetCaps();
  CVaapi2Texture::TestInteropFormats(vaDpy, eglDisplay, caps);

  CLog::Log(LOGDEBUG, "VAAPI EGL interop: {}", caps.ToString());

  // Bool out-params are views over caps for the OptionalsReg / WinSystem
  // boundary that still expresses capability as the general/deepColor pair.
  general = caps.Supports(AV_PIX_FMT_NV12);
  deepColor = caps.Supports(AV_PIX_FMT_P010);

  vaTerminate(vaDpy);

  if (general)
  {
    VIDEOPLAYER::CRendererFactory::RegisterRenderer("vaapi", CRendererVAAPIGLES::Create);
    m_pWinSystem = winSystem;
  }
}

CRendererVAAPIGLES::CRendererVAAPIGLES() = default;

CRendererVAAPIGLES::~CRendererVAAPIGLES()
{
  for (int i = 0; i < MAX_RENDER_BUFFERS; ++i)
  {
    DeleteTexture(i);
  }
  // renderer destruction runs on the render thread with the GL context current
  m_texturePool.ReleaseAll();
}

bool CRendererVAAPIGLES::Configure(const VideoPicture& picture, float fps, unsigned int orientation)
{
  CVaapiRenderPicture *pic = dynamic_cast<CVaapiRenderPicture*>(picture.videoBuffer);
  if (pic->procPic.videoSurface != VA_INVALID_ID)
    m_isVAAPIBuffer = true;
  else
    m_isVAAPIBuffer = false;

  m_vaapiFourcc = pic->procPic.fourcc;
#ifdef CB1_HAS_HDR10_AI
  m_aiBuffers = m_isVAAPIBuffer && picture.hdrType == StreamHdrType::HDR_TYPE_HDR10 &&
      !picture.doviSourceInfo.profile && !(picture.iFlags & DVP_FLAG_INTERLACED) &&
      DVBridgeOrientationSupported(orientation) ? pic->renderBufferCount : NUM_BUFFERS;
  m_aiLookahead=m_aiBuffers>NUM_BUFFERS;
#endif

  if (m_isVAAPIBuffer)
  {
    InteropInfo interop;
    interop.textureTarget = GL_TEXTURE_2D;
    interop.eglCreateImageKHR = (PFNEGLCREATEIMAGEKHRPROC)eglGetProcAddress("eglCreateImageKHR");
    interop.eglDestroyImageKHR = (PFNEGLDESTROYIMAGEKHRPROC)eglGetProcAddress("eglDestroyImageKHR");
    interop.glEGLImageTargetTexture2DOES =
        (PFNGLEGLIMAGETARGETTEXTURE2DOESPROC)eglGetProcAddress("glEGLImageTargetTexture2DOES");
    interop.eglDisplay = m_pWinSystem->GetEGLDisplay();

    m_texturePool.ReleaseAll();
#ifdef CB1_HAS_HDR10_AI
    // Keep imported textures for the expanded decoder pool.
    m_texturePool.Init(interop, VAAPI::CVaapiTexturePool::MAX_ENTRIES +
                                  (m_aiBuffers > NUM_BUFFERS ? m_aiBuffers - NUM_BUFFERS : 0));
#else
    m_texturePool.Init(interop);
#endif
    for (auto& tex : m_vaapiTextures)
      tex = nullptr;

    for (auto& fence : m_fences)
    {
      fence = std::make_unique<CEGLFence>(CRendererVAAPIGLES::m_pWinSystem->GetEGLDisplay());
    }
  }

  return CLinuxRendererGLES::Configure(picture, fps, orientation);
}

#ifdef CB1_HAS_HDR10_AI
CRenderInfo CRendererVAAPIGLES::GetRenderInfo()
{
  CRenderInfo info;
  info.max_buffer_size = m_aiBuffers;
  return info;
}

bool CRendererVAAPIGLES::HDR10AILookahead() const
{
  return m_aiBuffers > NUM_BUFFERS && m_aiLookahead;
}

#endif

bool CRendererVAAPIGLES::ConfigChanged(const VideoPicture& picture)
{
  CVaapiRenderPicture *pic = dynamic_cast<CVaapiRenderPicture*>(picture.videoBuffer);
#ifdef CB1_HAS_HDR10_AI
  const unsigned buffers = pic->procPic.videoSurface != VA_INVALID_ID &&
      picture.hdrType == StreamHdrType::HDR_TYPE_HDR10 && !picture.doviSourceInfo.profile &&
      !(picture.iFlags & DVP_FLAG_INTERLACED) && DVBridgeOrientationSupported(m_renderOrientation) ?
      pic->renderBufferCount : NUM_BUFFERS;
  if (m_aiBuffers != buffers)
    return true;
#endif
  if ((pic->procPic.videoSurface != VA_INVALID_ID && !m_isVAAPIBuffer) ||
      (pic->procPic.videoSurface == VA_INVALID_ID && m_isVAAPIBuffer))
    return true;

  return false;
}

EShaderFormat CRendererVAAPIGLES::GetShaderFormat()
{
  if (!m_isVAAPIBuffer)
    return SHADER_NV12;

  // Per-fourcc sampling path. Semi-planar 4:2:0 surfaces (NV12 / P010 /
  // P012 / P016) all share SHADER_NV12_RRG; Mesa imports their DMA-BUF as
  // GL_R16 / GL_RG16 textures whose normalized 0..1 sampled floats span
  // the full range regardless of underlying bit depth, so one shader
  // covers all 4:2:0 bit depths. Packed 4:2:2 and 4:4:4 each have their
  // own shader because their channel layouts differ.
  switch (m_vaapiFourcc)
  {
    case VA_FOURCC_NV12:
    case VA_FOURCC_P010:
    case VA_FOURCC_P012:
    case VA_FOURCC_P016:
      return SHADER_NV12_RRG;
    case VA_FOURCC_Y210:
    case VA_FOURCC_Y212:
    case VA_FOURCC_Y216:
      return SHADER_Y210;
    case VA_FOURCC_AYUV:
    case VA_FOURCC_XYUV:
      return SHADER_AYUV;
    case VA_FOURCC_Y410:
      return SHADER_Y410;
    case VA_FOURCC_Y412:
    case VA_FOURCC_Y416:
      return SHADER_Y412;
    default:
      CLog::Log(LOGDEBUG, "CRendererVAAPIGLES::GetShaderFormat - unrecognized fourcc: {:#x}",
                m_vaapiFourcc);
      return SHADER_NV12_RRG;
  }
}

bool CRendererVAAPIGLES::LoadShadersHook()
{
  return false;
}

bool CRendererVAAPIGLES::RenderHook(int idx)
{
  return false;
}

#ifdef HAVE_DVBRIDGE
bool CRendererVAAPIGLES::DVBridgeOrientationSupported(unsigned int orientation) const
{
  // Native orientation includes the saved playback rotation; CB1 ignores only that part.
  return orientation == static_cast<unsigned int>(m_videoSettings.m_Orientation);
}

bool CRendererVAAPIGLES::BuildDVBridgeLayer(int idx, CDVBridgeGLES::Layer& layer)
{
  // CB1 reads the full decoded texture, independent of Kodi's view-mode crop.
  if (!m_isVAAPIBuffer || m_textureTarget != GL_TEXTURE_2D ||
      !DVBridgeOrientationSupported(m_renderOrientation))
    return false;
  auto& buffer = m_buffers[idx];
  if (!buffer.doviSourcePicture)
    return false;
  const auto& source = *buffer.doviSourcePicture;
  bool hdr10 = false;
#ifdef CB1_HAS_HDR10_AI
  hdr10 = source.hdrType == StreamHdrType::HDR_TYPE_HDR10 &&
      source.doviSourceBinding.Valid() && !source.doviSourceInfo.profile;
#endif
  if (!hdr10 && (!buffer.doviMetadata.Data() || buffer.doviPts == DVD_NOPTS_VALUE))
    return false;
  switch (m_vaapiFourcc)
  {
    case VA_FOURCC_NV12: layer.layout = DVBRIDGE_NV12; break;
    case VA_FOURCC_P010: layer.layout = DVBRIDGE_P010; break;
    case VA_FOURCC_P012: layer.layout = DVBRIDGE_P012; break;
    case VA_FOURCC_P016: layer.layout = DVBRIDGE_P016; break;
    default: return false;
  }
  layer.width = m_sourceWidth;
  layer.height = m_sourceHeight;
  layer.pts = hdr10 ? double(source.doviSourcePts) / DVD_TIME_BASE : buffer.doviPts;
  layer.binding = hdr10 ? source.doviSourceBinding : buffer.doviMetadata.Binding();
  if (!std::isfinite(layer.pts) || (hdr10 && source.doviSourcePts == AV_NOPTS_VALUE))
    return false;
  const auto texture = m_texturePool.Retain(m_vaapiTextures[idx]);
  if (!texture || !buffer.doviSourcePicture) return false;
  layer.sourceInfo = buffer.doviSourcePicture->doviSourceInfo;
  layer.chromaLocation = source.chroma_position;
  if (hdr10)
  {
    layer.fullRange = source.color_range != 0;
    if (source.hasDisplayMetadata && source.displayMetadata.has_luminance)
    {
      layer.hdr.max_luma = av_q2d(source.displayMetadata.max_luminance);
      layer.hdr.min_luma = av_q2d(source.displayMetadata.min_luminance);
    }
    if (source.hasLightMetadata)
    {
      layer.hdr.max_cll = source.lightMetadata.MaxCLL;
      layer.hdr.max_fall = source.lightMetadata.MaxFALL;
    }
  }
  struct ForeignLease
  {
    std::shared_ptr<void> imported;
    std::shared_ptr<VideoPicture> decoded;
  };
  layer.lease = std::make_shared<ForeignLease>(texture,buffer.doviSourcePicture);
  for (int p = 0; p < 2; ++p)
  {
    const auto& plane = buffer.fields[FIELD_FULL][p];
    layer.planes[p] = {plane.id, static_cast<int>(plane.texwidth),
                      static_cast<int>(plane.texheight)};
  }
  return true;
}

#ifdef CB1_HAS_HDR10_AI
bool CRendererVAAPIGLES::PrepareHDR10AIQueue(const std::deque<int>& queue, bool end, bool paused,
    uint64_t presentationDeadline)
{
  auto preparation = CaptureHDR10AIQueue(queue,end,paused,presentationDeadline);
  if (!preparation.execute) return preparation.ready;
  const auto outcome = preparation.execute();
  return preparation.publish(outcome) && outcome.ready;
}

HDR10AIQueuePreparation CRendererVAAPIGLES::CaptureHDR10AIQueue(
    const std::deque<int>& queue, bool end, bool paused, uint64_t presentationDeadline)
{
  if (queue.empty() || !HDR10AIQueueEligible()) return {};
  auto* window = dynamic_cast<KODI::WINDOWING::GBM::CWinSystemGbmGLESContext*>(
      CServiceBroker::GetWinSystem());
  if (!window) { m_aiLookahead=false; return {}; }
  std::vector<CDVBridgeGLES::Layer> layers;
  // Copy ownership, not a reference to a slot that the decoder may replace.
  const auto head = m_buffers[queue.front()].doviSourcePicture;
  if (!head) return {};
  const auto horizon = double(head->doviSourcePts) / DVD_TIME_BASE + 1.0;
  for (const auto idx : queue)
  {
    const auto* pic = dynamic_cast<CVaapiRenderPicture*>(m_buffers[idx].videoBuffer);
    if (!pic || !pic->valid) break;
    // ReleaseBuffer invalidates the import before a slot receives another picture.
    if (!m_vaapiTextures[idx] && !UploadTexture(idx)) break;
    CDVBridgeGLES::Layer layer{};
    if (!BuildDVBridgeLayer(idx,layer)) break;
    layers.push_back(std::move(layer));
    if (layers.back().pts >= horizon - 1e-9) break;
  }
  // Future imports must not replace the current picture's admission identity.
  if (!UploadTexture(queue.front())) return {false, {}, {}};
  const auto index = queue.front();
  const auto binding = head->doviSourceBinding;
  const auto preferences = DVBRIDGE::FrameOutputSettings();
  if (preferences.hdr10Source == DVBRIDGE::HDR10SourceMode::Native)
  {
    // Preserve the original locked/native stop and readiness path.
    const bool ready = window->PrepareHDR10AIQueue(*head,layers,end,paused,presentationDeadline);
    m_aiLookahead = window->HDR10AILookaheadActive();
    if (ready && m_aiOutputPending)
    {
      if (!ConfigureVideoOutput(*head)) return {};
      m_aiOutputPending = false;
    }
    return {ready, {}, {}};
  }
  return {false,
      [window,head,layers=std::move(layers),end,paused,presentationDeadline]() {
        const bool ready = window->PrepareHDR10AIQueue(*head,layers,end,paused,presentationDeadline);
        return HDR10AIQueuePreparation::Outcome{ready,window->HDR10AILookaheadActive()};
      },
      [this,index,binding,preferences](const HDR10AIQueuePreparation::Outcome& outcome) {
        const auto& current = m_buffers[index].doviSourcePicture;
        const auto settings = DVBRIDGE::FrameOutputSettings();
        if (!current || !(current->doviSourceBinding == binding) ||
            settings.revision != preferences.revision || !settings.SameValues(preferences))
          return false;
        m_aiLookahead = outcome.lookahead;
        if (outcome.ready && m_aiOutputPending)
        {
          if (!ConfigureVideoOutput(*current)) return false;
          m_aiOutputPending = false;
        }
        return true;
      }};
}

bool CRendererVAAPIGLES::HDR10AIFrameCommitted() const
{
  auto* window = dynamic_cast<KODI::WINDOWING::GBM::CWinSystemGbmGLESContext*>(CServiceBroker::GetWinSystem());
  return window && window->HDR10AIFrameCommitted();
}
#endif

dvbridge_geometry CRendererVAAPIGLES::DVBridgeGeometry()
{
  const CRect view = CServiceBroker::GetWinSystem()->GetGfxContext().GetViewWindow();
  CRect dest;
  CalcDestRect(view.x1, view.y1, view.Width(), view.Height(), GetAspectRatio(),
               1.0f, 0.0f, dest, false);
  return {static_cast<int>(m_sourceWidth), static_cast<int>(m_sourceHeight),
          static_cast<int>(std::lround(dest.x1)), static_cast<int>(std::lround(dest.y1)),
          static_cast<int>(std::lround(dest.Width())),
          static_cast<int>(std::lround(dest.Height()))};
}

void CRendererVAAPIGLES::SetDVBridgeFELSuccessor(int index)
{
  m_felSuccessor = {};
  if (index < 0 || index >= MAX_RENDER_BUFFERS) return;
  const auto& buffer = m_buffers[index];
  if (!buffer.doviEnhancement || !buffer.doviMetadata.Data() ||
      !(buffer.doviEnhancementBinding == buffer.doviMetadata.Binding())) return;
  m_felSuccessor = {buffer.doviEnhancement,buffer.doviEnhancementBinding,buffer.doviMetadata};
}

bool CRendererVAAPIGLES::RenderDVBridge(int idx)
{
  auto* window = dynamic_cast<KODI::WINDOWING::GBM::CWinSystemGbmGLESContext*>(
      CServiceBroker::GetWinSystem());
  CDVBridgeGLES::Layer layer{};
  if (!window || !BuildDVBridgeLayer(idx,layer)) return false;
  auto& buffer = m_buffers[idx];
  return window->RenderDVBridge(layer, buffer.doviEnhancement,
      buffer.doviEnhancementBinding, buffer.doviMetadata, DVBridgeGeometry(), &m_felSuccessor);
}
#endif

bool CRendererVAAPIGLES::CreateTexture(int index)
{
  if (!m_isVAAPIBuffer)
  {
    DeleteTexture(index);

    if (!CreateNV12Texture(index))
      return false;

    // Allocate backing memory for NV12 plane copy (base class leaves planes null).
    // CFFmpegPostproc AVFrame data may be recycled before upload, so we copy into
    // stable buffers — matching the GL renderer's approach.
    YuvImage& im = m_buffers[index].image;
    for (int i = 0; i < 2; i++)
      im.plane[i] = new uint8_t[im.planesize[i]];

    m_nv12Allocated[index] = true;
    return true;
  }

  CPictureBuffer &buf = m_buffers[index];
  YuvImage &im = buf.image;
  CYuvPlane (&planes)[YuvImage::MAX_PLANES] = buf.fields[0];

  DeleteTexture(index);

  im = {};
  std::fill(std::begin(planes), std::end(planes), CYuvPlane{});
  im.height = m_sourceHeight;
  im.width  = m_sourceWidth;
  im.cshift_x = 1;
  im.cshift_y = 1;

  planes[0].id = 1;

  return true;
}

void CRendererVAAPIGLES::DeleteTexture(int index)
{
  ReleaseBuffer(index);

  if (!m_isVAAPIBuffer)
  {
    if (m_nv12Allocated[index])
    {
      YuvImage& im = m_buffers[index].image;
      for (int i = 0; i < 2; i++)
      {
        delete[] im.plane[i];
        im.plane[i] = nullptr;
      }
      m_nv12Allocated[index] = false;
    }
    DeleteNV12Texture(index);
    return;
  }

  CPictureBuffer &buf = m_buffers[index];
  buf.fields[FIELD_FULL][0].id = 0;
  buf.fields[FIELD_FULL][1].id = 0;
  buf.fields[FIELD_FULL][2].id = 0;
}

bool CRendererVAAPIGLES::UploadTexture(int index)
{
#ifdef HAVE_DVBRIDGE
  auto* window = dynamic_cast<KODI::WINDOWING::GBM::CWinSystemGbmGLESContext*>(
      CServiceBroker::GetWinSystem());
  if (window) window->ObserveDVImport({}, {}, false);
#endif
  if (!m_isVAAPIBuffer)
  {
    CPictureBuffer& buf = m_buffers[index];
    CVaapiRenderPicture* pic = dynamic_cast<CVaapiRenderPicture*>(buf.videoBuffer);
    if (!pic || !pic->valid)
      return false;

    if (!buf.loaded)
    {
      YuvImage& dst = buf.image;
      YuvImage src;
      pic->GetPlanes(src.plane);
      pic->GetStrides(src.stride);
      CVideoBuffer::CopyNV12Picture(&dst, &src);
    }
    CalculateTextureSourceRects(index, 3);
    return UploadNV12Texture(index);
  }

  CPictureBuffer &buf = m_buffers[index];

  CVaapiRenderPicture *pic = dynamic_cast<CVaapiRenderPicture*>(buf.videoBuffer);

  if (!pic || !pic->valid)
  {
    return false;
  }

  m_vaapiTextures[index] = m_texturePool.Get(pic, m_vaapiTextures);
  if (!m_vaapiTextures[index])
    return false;

  YuvImage &im = buf.image;
  CYuvPlane (&planes)[3] = buf.fields[0];

  auto size = m_vaapiTextures[index]->GetTextureSize();
  planes[0].texwidth  = size.Width();
  planes[0].texheight = size.Height();

  // Packed 4:2:2 fourccs (YUY2 / Y210 / Y212 / Y216) hold two luma per
  // texel, so the GL texture covers half the source width with each texel
  // representing a (Y0, Cb, Y1, Cr) macropixel. Match the LinuxRendererGLES
  // YUYV path: halve texwidth (ceiling for odd widths) and tell the shader
  // pixpertex_x = 2.
  const bool packed422 = (m_vaapiFourcc == VA_FOURCC_Y210 || m_vaapiFourcc == VA_FOURCC_Y212 ||
                          m_vaapiFourcc == VA_FOURCC_Y216);
  if (packed422)
    planes[0].texwidth = (planes[0].texwidth + 1) / 2;

  planes[1].texwidth  = planes[0].texwidth  >> im.cshift_x;
  planes[1].texheight = planes[0].texheight >> im.cshift_y;
  planes[2].texwidth  = planes[1].texwidth;
  planes[2].texheight = planes[1].texheight;

  for (int p = 0; p < 3; p++)
  {
    planes[p].pixpertex_x = packed422 ? 2 : 1;
    planes[p].pixpertex_y = 1;
  }

  // set textures
  planes[0].id = m_vaapiTextures[index]->GetTextureY();
  planes[1].id = m_vaapiTextures[index]->GetTextureVU();
  planes[2].id = m_vaapiTextures[index]->GetTextureVU();

  for (int p=0; p<2; p++)
  {
    glBindTexture(m_textureTarget, planes[p].id);
    glTexParameteri(m_textureTarget, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(m_textureTarget, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(m_textureTarget, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(m_textureTarget, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    glBindTexture(m_textureTarget, 0);
    VerifyGLState();
  }

  CalculateTextureSourceRects(index, 3);
#ifdef HAVE_DVBRIDGE
  if (window && buf.doviSourcePicture)
  {
    const bool layout = m_vaapiFourcc == VA_FOURCC_NV12 || m_vaapiFourcc == VA_FOURCC_P010 ||
        m_vaapiFourcc == VA_FOURCC_P012 || m_vaapiFourcc == VA_FOURCC_P016;
    const bool usable = layout && m_textureTarget == GL_TEXTURE_2D &&
        DVBridgeOrientationSupported(m_renderOrientation);
    const auto binding = buf.doviSourcePicture->doviSourceBinding.Valid() ?
        buf.doviSourcePicture->doviSourceBinding : buf.doviMetadata.Binding();
    window->ObserveDVImport(binding, DVBridgeGeometry(), usable);
  }
#endif
  return true;
}

void CRendererVAAPIGLES::AfterRenderHook(int index)
{
  if (m_fences[index])
  {
    m_fences[index]->DestroyFence();
    m_fences[index]->CreateFence();
  }
}

bool CRendererVAAPIGLES::NeedBuffer(int index)
{
  if (m_fences[index])
    return !m_fences[index]->IsSignaled();

  return false;
}

void CRendererVAAPIGLES::ReleaseBuffer(int index)
{
  if (m_fences[index])
    m_fences[index]->DestroyFence();

  if (m_isVAAPIBuffer)
    m_vaapiTextures[index] = nullptr;

  CLinuxRendererGLES::ReleaseBuffer(index);
}
