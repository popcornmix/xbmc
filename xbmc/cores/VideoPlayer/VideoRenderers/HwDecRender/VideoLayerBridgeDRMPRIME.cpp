/*
 *  Copyright (C) 2017-2018 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#include "VideoLayerBridgeDRMPRIME.h"

#include "ServiceBroker.h"
#include "cores/VideoPlayer/Buffers/VideoBufferDRMPRIME.h"
#include "utils/MathUtils.h"
#include "utils/log.h"
#include "windowing/WinSystem.h"
#include "windowing/gbm/drm/DRMAtomic.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <utility>

using namespace KODI::WINDOWING::GBM;
using namespace DRMPRIME;

CVideoLayerBridgeDRMPRIME::CVideoLayerBridgeDRMPRIME(std::shared_ptr<CDRMAtomic> drm)
  : m_DRM(std::move(drm))
{
}

CVideoLayerBridgeDRMPRIME::~CVideoLayerBridgeDRMPRIME()
{
  Release(m_prev_buffer);
  Release(m_buffer);

  // the plane-off commit from Disable has run by now, so these are plain frees
  for (uint32_t fbId : m_fbCache.TakeAll())
    drmModeRmFB(m_DRM->GetFileDescriptor(), fbId);
}

void CVideoLayerBridgeDRMPRIME::Disable()
{
  auto plane = m_DRM->GetVideoPlane();
  if (!plane)
    return;

  // disable video plane
  auto connector = m_DRM->GetConnector();

  // reset max bpc back to default of 8
  int bpc = 8;
  bool result = m_DRM->AddProperty(connector, "max bpc", bpc);
  CLog::Log(LOGDEBUG, "CVideoLayerBridgeDRMPRIME::{} - setting max bpc to {} ({})",
            __FUNCTION__, bpc, result);

  m_DRM->AddProperty(plane, "FB_ID", 0);
  m_DRM->AddProperty(plane, "CRTC_ID", 0);

  if (auto* plane2 = m_DRM->GetVideoPlane2())
  {
    m_DRM->AddProperty(plane2, "FB_ID", 0);
    m_DRM->AddProperty(plane2, "CRTC_ID", 0);
  }
}

void CVideoLayerBridgeDRMPRIME::Acquire(CVideoBufferDRMPRIME* buffer, uint32_t fbId)
{
  // release the buffer that is no longer presented on screen
  Release(m_prev_buffer);

  // release the buffer currently being presented next call
  m_prev_buffer = m_buffer;
  m_prev_fb_id = m_fb_id;

  // reference count the buffer that is going to be presented on screen
  m_buffer = buffer;
  m_fb_id = fbId;
  m_buffer->Acquire();
}

void CVideoLayerBridgeDRMPRIME::Release(CVideoBufferDRMPRIME* buffer)
{
  if (!buffer)
    return;

  buffer->Release();
}

bool CVideoLayerBridgeDRMPRIME::PrepareBuffer(CVideoBufferDRMPRIME* buffer)
{
  if (!buffer->AcquireDescriptor())
  {
    CLog::Log(LOGERROR, "CVideoLayerBridgeDRMPRIME::{} - failed to acquire descriptor",
              __FUNCTION__);
    return false;
  }

  const auto identity =
      DRMPRIME::GetDmaBufIdentity(buffer->GetDescriptor(), buffer->GetWidth(), buffer->GetHeight());
  if (!identity)
  {
    buffer->ReleaseDescriptor();
    CLog::Log(LOGERROR, "CVideoLayerBridgeDRMPRIME::{} - failed to identify buffer memory",
              __FUNCTION__);
    return false;
  }

  uint32_t fbId = m_fbCache.Lookup(*identity);
  if (!fbId)
  {
    fbId = CreateFramebuffer(buffer);
    if (!fbId)
    {
      buffer->ReleaseDescriptor();
      return false;
    }
    m_fbCache.Insert(*identity, fbId);
  }
  buffer->ReleaseDescriptor();

  if (m_buffer != buffer)
    Acquire(buffer, fbId);
  else
    m_fb_id = fbId;

  // reap after the id shift so protection covers the new presented pair
  for (uint32_t doomed : m_fbCache.Reap({m_fb_id, m_prev_fb_id}))
    drmModeRmFB(m_DRM->GetFileDescriptor(), doomed);

  return true;
}

uint32_t CVideoLayerBridgeDRMPRIME::CreateFramebuffer(CVideoBufferDRMPRIME* buffer)
{
  AVDRMFrameDescriptor* descriptor = buffer->GetDescriptor();
  uint32_t fbId = 0;
  uint32_t objectHandles[AV_DRM_MAX_PLANES] = {};
  uint32_t handles[4] = {}, pitches[4] = {}, offsets[4] = {}, flags = 0;
  uint64_t modifier[4] = {};
  int ret = 0;

  // convert Prime FD to GEM handle
  for (int object = 0; object < descriptor->nb_objects; object++)
  {
    ret = drmPrimeFDToHandle(m_DRM->GetFileDescriptor(), descriptor->objects[object].fd,
                             &objectHandles[object]);
    if (ret < 0)
    {
      CLog::Log(LOGERROR,
                "CVideoLayerBridgeDRMPRIME::{} - failed to convert prime fd {} to gem handle {}, "
                "ret = {}",
                __FUNCTION__, descriptor->objects[object].fd, objectHandles[object], ret);
      break;
    }
  }

  if (ret == 0)
  {
    AVDRMLayerDescriptor* layer = &descriptor->layers[0];

    for (int plane = 0; plane < layer->nb_planes; plane++)
    {
      int object = layer->planes[plane].object_index;
      uint32_t handle = objectHandles[object];
      if (handle)
      {
        handles[plane] = handle;
        pitches[plane] = layer->planes[plane].pitch;
        offsets[plane] = layer->planes[plane].offset;
        modifier[plane] = descriptor->objects[object].format_modifier;
      }
    }

    if (modifier[0] && modifier[0] != DRM_FORMAT_MOD_INVALID)
      flags = DRM_MODE_FB_MODIFIERS;

    // add the video frame FB
    ret = drmModeAddFB2WithModifiers(m_DRM->GetFileDescriptor(), buffer->GetWidth(),
                                     buffer->GetHeight(), layer->format, handles, pitches, offsets,
                                     modifier, &fbId, flags);
    if (ret < 0)
      CLog::Log(LOGERROR,
                "CVideoLayerBridgeDRMPRIME::{} - failed to add fb, format {:#x} modifier {:#x} "
                "ret = {}",
                __FUNCTION__, layer->format, modifier[0], ret);
  }

  // close the GEM handles now: the fb holds its own references, and the
  // dedup entry drops with them, so a later importer of this dma-buf (the
  // screencap EGL import) solely owns a fresh handle instead of sharing ours
  for (int i = 0; i < AV_DRM_MAX_PLANES; i++)
  {
    if (objectHandles[i])
    {
      struct drm_gem_close gem_close;
      gem_close.handle = objectHandles[i];
      drmIoctl(m_DRM->GetFileDescriptor(), DRM_IOCTL_GEM_CLOSE, &gem_close);
    }
  }

  if (ret < 0)
    return 0;

  return fbId;
}

void CVideoLayerBridgeDRMPRIME::Configure(CVideoBufferDRMPRIME* buffer)
{
  // a new renderer generation brings a new buffer pool; old entries can never match again
  m_fbCache.InvalidateAll();
  for (uint32_t doomed : m_fbCache.Reap({m_fb_id, m_prev_fb_id}))
    drmModeRmFB(m_DRM->GetFileDescriptor(), doomed);

  auto plane = m_DRM->GetVideoPlane();
  if (!plane)
    return;

  const VideoPicture& picture = buffer->GetPicture();

  for (auto* p : {plane, m_DRM->GetVideoPlane2()})
  {
    if (!p)
      continue;

    std::optional<uint64_t> colorEncoding =
        p->GetPropertyEnumValue("COLOR_ENCODING", GetColorEncoding(picture));
    if (colorEncoding)
      m_DRM->AddProperty(p, "COLOR_ENCODING", colorEncoding.value());

    std::optional<uint64_t> colorRange =
        p->GetPropertyEnumValue("COLOR_RANGE", GetColorRange(picture));
    if (colorRange)
      m_DRM->AddProperty(p, "COLOR_RANGE", colorRange.value());
  }

  // set max bpc to allow the drm driver to choose a deep colour mode
  int bpc = buffer->GetPicture().colorBits > 8 ? 12 : 8;
  auto connector = m_DRM->GetConnector();
  bool result = m_DRM->AddProperty(connector, "max bpc", bpc);
  CLog::Log(LOGDEBUG, "CVideoLayerBridgeDRMPRIME::{} - setting max bpc to {} ({})", __FUNCTION__,
            bpc, result);
}

void CVideoLayerBridgeDRMPRIME::SetPlaneRects(CDRMPlane* plane,
                                              CVideoBufferDRMPRIME* buffer,
                                              const PlaneRects& rects)
{
  // Buffer dimensions equal the picture dimensions, so the source rect maps
  // straight to the plane crop. SRC_* are 16.16 fixed point, which exists so a
  // crop can start and end between pixels - that is how a plane pans and zooms
  // smoothly. Keep the fractional part rather than truncating to whole pixels,
  // and leave any alignment the hardware needs to the driver: userspace cannot
  // enumerate that constraint, and the property advertises fractional support.
  // Clamp the edges to the buffer and derive the size from them so the crop
  // stays consistent, falling back to the whole buffer if it is empty.
  constexpr int64_t fpOne = 1 << 16;
  const int64_t bufferWidth = static_cast<int64_t>(buffer->GetWidth()) * fpOne;
  const int64_t bufferHeight = static_cast<int64_t>(buffer->GetHeight()) * fpOne;

  const auto toFixed = [](float value)
  { return static_cast<int64_t>(std::lround(static_cast<double>(value) * fpOne)); };

  int64_t srcX = std::clamp<int64_t>(toFixed(rects.source.x1), 0, bufferWidth);
  int64_t srcY = std::clamp<int64_t>(toFixed(rects.source.y1), 0, bufferHeight);
  int64_t srcW = std::clamp<int64_t>(toFixed(rects.source.x2), srcX, bufferWidth) - srcX;
  int64_t srcH = std::clamp<int64_t>(toFixed(rects.source.y2), srcY, bufferHeight) - srcY;
  if (srcW == 0 || srcH == 0)
  {
    srcX = srcY = 0;
    srcW = bufferWidth;
    srcH = bufferHeight;
  }

  m_DRM->AddProperty(plane, "FB_ID", m_fb_id);
  m_DRM->AddProperty(plane, "CRTC_ID", m_DRM->GetCrtc()->GetCrtcId());
  m_DRM->AddProperty(plane, "SRC_X", static_cast<uint64_t>(srcX));
  m_DRM->AddProperty(plane, "SRC_Y", static_cast<uint64_t>(srcY));
  m_DRM->AddProperty(plane, "SRC_W", static_cast<uint64_t>(srcW));
  m_DRM->AddProperty(plane, "SRC_H", static_cast<uint64_t>(srcH));
  // The CRTC rect addresses the composited output, which is not subsampled, so
  // it needs no even alignment - and must not be forced to it. Frame packing
  // starts the second eye at vdisplay + vblank, which is odd for 1080p24
  // (1080 + 45 = 1125); rounding that down to 1124 shifts one eye up a line and
  // takes its last line from the active space gap. Use the rounded edges so the
  // position and the size stay consistent with each other.
  const int32_t dstX1 = MathUtils::round_int(static_cast<double>(rects.dest.x1));
  const int32_t dstY1 = MathUtils::round_int(static_cast<double>(rects.dest.y1));
  const int32_t dstX2 = MathUtils::round_int(static_cast<double>(rects.dest.x2));
  const int32_t dstY2 = MathUtils::round_int(static_cast<double>(rects.dest.y2));

  m_DRM->AddProperty(plane, "CRTC_X", dstX1);
  m_DRM->AddProperty(plane, "CRTC_Y", dstY1);
  m_DRM->AddProperty(plane, "CRTC_W", static_cast<uint32_t>(std::max(0, dstX2 - dstX1)));
  m_DRM->AddProperty(plane, "CRTC_H", static_cast<uint32_t>(std::max(0, dstY2 - dstY1)));
}

void CVideoLayerBridgeDRMPRIME::SetVideoPlane(CVideoBufferDRMPRIME* buffer,
                                              std::span<const PlaneRects> rects)
{
  CDRMPlane* planes[] = {m_DRM->GetVideoPlane(), m_DRM->GetVideoPlane2()};
  if (!planes[0] || rects.empty())
    return;

  if (!PrepareBuffer(buffer))
    return;

  size_t used = 0;
  for (auto* plane : planes)
  {
    if (!plane || used == rects.size())
      break;
    SetPlaneRects(plane, buffer, rects[used++]);
  }

  // Detach a claimed second plane that this frame does not use, so a switch out
  // of a split stereo mode does not leave the second eye on screen.
  if (planes[1] && used < 2)
  {
    m_DRM->AddProperty(planes[1], "FB_ID", 0);
    m_DRM->AddProperty(planes[1], "CRTC_ID", 0);
  }
}

void CVideoLayerBridgeDRMPRIME::UpdateVideoPlane()
{
  if (!m_buffer || !m_fb_id)
    return;

  // release the buffer that is no longer presented on screen
  Release(m_prev_buffer);
  m_prev_buffer = nullptr;

  auto plane = m_DRM->GetVideoPlane();
  if (!plane)
    return;

  m_DRM->AddProperty(plane, "FB_ID", m_fb_id);
  m_DRM->AddProperty(plane, "CRTC_ID", m_DRM->GetCrtc()->GetCrtcId());
}
