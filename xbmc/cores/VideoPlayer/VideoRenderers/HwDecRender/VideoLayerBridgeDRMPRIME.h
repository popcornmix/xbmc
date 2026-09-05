/*
 *  Copyright (C) 2018 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#pragma once

#include "cores/VideoPlayer/Buffers/DmaBufIdentityCache.h"
#include "cores/VideoPlayer/Interface/StreamInfo.h"
#include "threads/SystemClock.h"
#include "windowing/gbm/VideoLayerBridge.h"

#include <array>
#include <cstdint>
#include <memory>
#include <span>

#include <drm_mode.h>

namespace KODI
{
namespace WINDOWING
{
namespace GBM
{
class CDRMAtomic;
class CDRMPlane;
}
} // namespace WINDOWING
} // namespace KODI

class CVideoBufferDRMPRIME;

class CVideoLayerBridgeDRMPRIME : public KODI::WINDOWING::GBM::CVideoLayerBridge
{
public:
  //! \brief One scanout: a crop of a buffer onto a screen area.
  struct PlaneLayer
  {
    CVideoBufferDRMPRIME* buffer{nullptr};
    CRect source;
    CRect dest;
  };

  CVideoLayerBridgeDRMPRIME(std::shared_ptr<KODI::WINDOWING::GBM::CDRMAtomic> drm);
  ~CVideoLayerBridgeDRMPRIME() override;
  void Disable() override;

  virtual void Configure(CVideoBufferDRMPRIME* buffer);

  /*!
   * \brief Scan the layers out on the video plane(s).
   *
   * One layer presents on the single video plane. Two need a second video plane and
   * present one stereoscopic eye on each - the two halves of one buffer where the views
   * are packed into a frame, a buffer each where they are not. The second layer is
   * ignored if no second plane was claimed.
   */
  virtual void SetVideoPlane(std::span<const PlaneLayer> layers);
  virtual void UpdateVideoPlane();

protected:
  std::shared_ptr<KODI::WINDOWING::GBM::CDRMAtomic> m_DRM;

private:
  //! An eye per plane is as many as anything here presents at once.
  static constexpr size_t MAX_VIDEO_PLANES = 2;
  using BufferSet = std::array<CVideoBufferDRMPRIME*, MAX_VIDEO_PLANES>;
  using FbIdSet = std::array<uint32_t, MAX_VIDEO_PLANES>;

  //! \brief Take up the buffers to present and let go of the ones they replace.
  void Present(const BufferSet& buffers, const FbIdSet& fbIds);
  void Release(CVideoBufferDRMPRIME* buffer);
  //! \brief The framebuffer for a buffer, made if the cache does not have one; 0 on failure.
  uint32_t FramebufferFor(CVideoBufferDRMPRIME* buffer);
  //! \brief Destroy the framebuffers the cache has done with, keeping the presented ones.
  void ReapFramebuffers();
  //! \brief Convert the buffer's descriptor to a framebuffer; 0 on failure.
  uint32_t CreateFramebuffer(CVideoBufferDRMPRIME* buffer);
  void SetPlaneRects(KODI::WINDOWING::GBM::CDRMPlane* plane,
                     const PlaneLayer& layer,
                     uint32_t fbId);
  /*!
   * \brief Count the release of the previously presented buffers as safe or not.
   *
   * Call immediately before letting go of m_prevBuffers.
   */
  void CheckScanoutRelease();

  //! Must exceed the decoder's dma-buf pool, which has a frame per view in flight
  //! for a multiview stream; an overflow re-imports the buffer it just evicted.
  static constexpr size_t MAX_FB_CACHE = 64;

  DRMPRIME::CDmaBufIdentityCache m_fbCache{MAX_FB_CACHE, "bridge-fb"};
  BufferSet m_buffers{};
  BufferSet m_prevBuffers{};
  FbIdSet m_fbIds{};
  FbIdSet m_prevFbIds{};

  //! Commit that presents m_buffers, i.e. that takes m_prevBuffers off screen.
  uint64_t m_commitSeq{0};
  unsigned int m_releases{0};
  unsigned int m_earlyReleases{0};
  unsigned int m_indeterminateReleases{0};
  unsigned int m_loggedEarlyReleases{0};
  XbmcThreads::EndTime<> m_earlyReleaseLogTimer;
};
