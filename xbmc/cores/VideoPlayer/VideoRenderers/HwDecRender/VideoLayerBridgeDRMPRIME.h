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
#include "windowing/gbm/VideoLayerBridge.h"

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
  //! \brief One scanout rectangle pair: a crop of the buffer onto a screen area.
  struct PlaneRects
  {
    CRect source;
    CRect dest;
  };

  CVideoLayerBridgeDRMPRIME(std::shared_ptr<KODI::WINDOWING::GBM::CDRMAtomic> drm);
  ~CVideoLayerBridgeDRMPRIME() override;
  void Disable() override;

  virtual void Configure(CVideoBufferDRMPRIME* buffer);

  /*!
   * \brief Scan the buffer out on the video plane(s).
   *
   * One entry presents the buffer on the single video plane. Two entries need
   * a second video plane and present one stereoscopic eye on each; the second
   * is ignored if no second plane was claimed.
   */
  virtual void SetVideoPlane(CVideoBufferDRMPRIME* buffer, std::span<const PlaneRects> rects);
  virtual void UpdateVideoPlane();

protected:
  std::shared_ptr<KODI::WINDOWING::GBM::CDRMAtomic> m_DRM;

private:
  void Acquire(CVideoBufferDRMPRIME* buffer, uint32_t fbId);
  void Release(CVideoBufferDRMPRIME* buffer);
  bool PrepareBuffer(CVideoBufferDRMPRIME* buffer);
  //! \brief Convert the buffer's descriptor to a framebuffer; 0 on failure.
  uint32_t CreateFramebuffer(CVideoBufferDRMPRIME* buffer);
  void SetPlaneRects(KODI::WINDOWING::GBM::CDRMPlane* plane,
                     CVideoBufferDRMPRIME* buffer,
                     const PlaneRects& rects);

  static constexpr size_t MAX_FB_CACHE = 32;

  DRMPRIME::CDmaBufIdentityCache m_fbCache{MAX_FB_CACHE, "bridge-fb"};
  CVideoBufferDRMPRIME* m_buffer = nullptr;
  CVideoBufferDRMPRIME* m_prev_buffer = nullptr;
  uint32_t m_fb_id{0};
  uint32_t m_prev_fb_id{0};
};
