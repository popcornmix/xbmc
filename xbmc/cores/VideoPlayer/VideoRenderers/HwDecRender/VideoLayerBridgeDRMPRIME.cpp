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

using namespace std::chrono_literals;
using namespace KODI::WINDOWING::GBM;
using namespace DRMPRIME;

namespace
{
//! How often the early release counters are logged while releases are early
constexpr auto EARLY_RELEASE_LOG_INTERVAL = 5000ms;
} // namespace

CVideoLayerBridgeDRMPRIME::CVideoLayerBridgeDRMPRIME(std::shared_ptr<CDRMAtomic> drm)
  : m_DRM(std::move(drm))
{
}

CVideoLayerBridgeDRMPRIME::~CVideoLayerBridgeDRMPRIME()
{
  for (auto* buffer : m_prevBuffers)
    Release(buffer);
  for (auto* buffer : m_buffers)
    Release(buffer);

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

void CVideoLayerBridgeDRMPRIME::Present(const BufferSet& buffers, const FbIdSet& fbIds)
{
  // Presenting the same buffers again - the framebuffer can still have changed under
  // them, so take the ids, but nothing has left the screen.
  if (buffers == m_buffers)
  {
    m_fbIds = fbIds;
    return;
  }

  // release the buffers that are no longer presented on screen
  CheckScanoutRelease();
  for (auto* buffer : m_prevBuffers)
    Release(buffer);

  // release the buffers currently being presented next call
  m_prevBuffers = m_buffers;
  m_prevFbIds = m_fbIds;

  // reference count the buffers that are going to be presented on screen
  m_buffers = buffers;
  m_fbIds = fbIds;
  for (auto* buffer : m_buffers)
    if (buffer)
      buffer->Acquire();

  // The plane properties for these buffers go into the pending atomic request, so
  // the next commit is the one that will put them on screen.
  m_commitSeq = m_DRM->GetNextCommitSequence();
}

void CVideoLayerBridgeDRMPRIME::CheckScanoutRelease()
{
  if (!m_prevBuffers[0])
    return;

  m_releases++;

  // m_prevBuffers are only off screen once the commit that presented m_buffers in
  // their place has flipped. Holding just the two generations assumes that has
  // happened by the time the next buffer arrives, which is true of a one frame
  // deep present pipeline. Count where it has not: the buffer goes back to the
  // decoder pool while the plane is still scanning it out, and whatever the
  // decoder writes there next appears on screen for a frame.
  switch (m_DRM->GetCommitState(m_commitSeq))
  {
    case CDRMAtomic::CommitState::COMPLETE:
      return;
    case CDRMAtomic::CommitState::UNKNOWN:
      m_indeterminateReleases++;
      return;
    case CDRMAtomic::CommitState::PENDING:
      m_earlyReleases++;
      break;
  }

  // The timer starts expired, so the first one is logged as it happens and the
  // rest are summarised - at a display rate this can be every frame.
  if (!m_earlyReleaseLogTimer.IsTimePast())
    return;

  m_earlyReleaseLogTimer.Set(EARLY_RELEASE_LOG_INTERVAL);

  CLog::LogF(LOGWARNING,
             "buffer released while the commit replacing it on screen was still pending: {} in "
             "the last {}s - totals: early:{} of {} releases (indeterminate:{})",
             m_earlyReleases - m_loggedEarlyReleases, EARLY_RELEASE_LOG_INTERVAL.count() / 1000,
             m_earlyReleases, m_releases, m_indeterminateReleases);

  m_loggedEarlyReleases = m_earlyReleases;
}

void CVideoLayerBridgeDRMPRIME::Release(CVideoBufferDRMPRIME* buffer)
{
  if (!buffer)
    return;

  buffer->Release();
}

uint32_t CVideoLayerBridgeDRMPRIME::FramebufferFor(CVideoBufferDRMPRIME* buffer)
{
  if (!buffer->AcquireDescriptor())
  {
    CLog::Log(LOGERROR, "CVideoLayerBridgeDRMPRIME::{} - failed to acquire descriptor",
              __FUNCTION__);
    return 0;
  }

  const auto identity =
      DRMPRIME::GetDmaBufIdentity(buffer->GetDescriptor(), buffer->GetWidth(), buffer->GetHeight());
  if (!identity)
  {
    buffer->ReleaseDescriptor();
    CLog::Log(LOGERROR, "CVideoLayerBridgeDRMPRIME::{} - failed to identify buffer memory",
              __FUNCTION__);
    return 0;
  }

  uint32_t fbId = m_fbCache.Lookup(*identity);
  if (!fbId)
  {
    fbId = CreateFramebuffer(buffer);
    if (fbId)
      m_fbCache.Insert(*identity, fbId);
  }
  buffer->ReleaseDescriptor();

  return fbId;
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

void CVideoLayerBridgeDRMPRIME::ReapFramebuffers()
{
  const std::array<uint32_t, MAX_VIDEO_PLANES * 2> presented{
      m_fbIds[0], m_fbIds[1], m_prevFbIds[0], m_prevFbIds[1]};

  for (uint32_t doomed : m_fbCache.Reap(presented))
    drmModeRmFB(m_DRM->GetFileDescriptor(), doomed);
}

void CVideoLayerBridgeDRMPRIME::Configure(CVideoBufferDRMPRIME* buffer)
{
  // a new renderer generation brings a new buffer pool; old entries can never match again
  m_fbCache.InvalidateAll();
  ReapFramebuffers();

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
                                              const PlaneLayer& layer,
                                              uint32_t fbId)
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
  const int64_t bufferWidth = static_cast<int64_t>(layer.buffer->GetWidth()) * fpOne;
  const int64_t bufferHeight = static_cast<int64_t>(layer.buffer->GetHeight()) * fpOne;

  const auto toFixed = [](float value)
  { return static_cast<int64_t>(std::lround(static_cast<double>(value) * fpOne)); };

  int64_t srcX = std::clamp<int64_t>(toFixed(layer.source.x1), 0, bufferWidth);
  int64_t srcY = std::clamp<int64_t>(toFixed(layer.source.y1), 0, bufferHeight);
  int64_t srcW = std::clamp<int64_t>(toFixed(layer.source.x2), srcX, bufferWidth) - srcX;
  int64_t srcH = std::clamp<int64_t>(toFixed(layer.source.y2), srcY, bufferHeight) - srcY;
  if (srcW == 0 || srcH == 0)
  {
    srcX = srcY = 0;
    srcW = bufferWidth;
    srcH = bufferHeight;
  }

  m_DRM->AddProperty(plane, "FB_ID", fbId);
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
  const int32_t dstX1 = MathUtils::round_int(static_cast<double>(layer.dest.x1));
  const int32_t dstY1 = MathUtils::round_int(static_cast<double>(layer.dest.y1));
  const int32_t dstX2 = MathUtils::round_int(static_cast<double>(layer.dest.x2));
  const int32_t dstY2 = MathUtils::round_int(static_cast<double>(layer.dest.y2));

  m_DRM->AddProperty(plane, "CRTC_X", dstX1);
  m_DRM->AddProperty(plane, "CRTC_Y", dstY1);
  m_DRM->AddProperty(plane, "CRTC_W", static_cast<uint32_t>(std::max(0, dstX2 - dstX1)));
  m_DRM->AddProperty(plane, "CRTC_H", static_cast<uint32_t>(std::max(0, dstY2 - dstY1)));
}

void CVideoLayerBridgeDRMPRIME::SetVideoPlane(std::span<const PlaneLayer> layers)
{
  CDRMPlane* planes[] = {m_DRM->GetVideoPlane(), m_DRM->GetVideoPlane2()};
  if (!planes[0] || layers.empty())
    return;

  const size_t used = std::min(layers.size(), planes[1] ? MAX_VIDEO_PLANES : size_t{1});

  BufferSet buffers{};
  FbIdSet fbIds{};
  for (size_t i = 0; i < used; i++)
  {
    fbIds[i] = FramebufferFor(layers[i].buffer);
    if (!fbIds[i])
      return;
    buffers[i] = layers[i].buffer;
  }

  Present(buffers, fbIds);

  // reap after the id shift so protection covers the newly presented buffers
  ReapFramebuffers();

  for (size_t i = 0; i < used; i++)
    SetPlaneRects(planes[i], layers[i], fbIds[i]);

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
  if (!m_buffers[0] || !m_fbIds[0])
    return;

  // release the buffers that are no longer presented on screen
  CheckScanoutRelease();
  for (auto*& buffer : m_prevBuffers)
  {
    Release(buffer);
    buffer = nullptr;
  }

  CDRMPlane* planes[] = {m_DRM->GetVideoPlane(), m_DRM->GetVideoPlane2()};
  for (size_t i = 0; i < MAX_VIDEO_PLANES; i++)
  {
    if (!planes[i] || !m_buffers[i])
      break;

    m_DRM->AddProperty(planes[i], "FB_ID", m_fbIds[i]);
    m_DRM->AddProperty(planes[i], "CRTC_ID", m_DRM->GetCrtc()->GetCrtcId());
  }
}
