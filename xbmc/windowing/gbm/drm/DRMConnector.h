/*
 *  Copyright (C) 2005-2020 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#pragma once

#include "DRMObject.h"

#include <string>
#include <vector>

namespace KODI
{
namespace WINDOWING
{
namespace GBM
{

class CDRMConnector : public CDRMObject
{
public:
  explicit CDRMConnector(int fd, uint32_t connector);
  CDRMConnector(const CDRMConnector&) = delete;
  CDRMConnector& operator=(const CDRMConnector&) = delete;
  ~CDRMConnector() = default;

  std::string GetType();
  std::string GetStatus();
  std::string GetName();

  uint32_t GetEncoderId() const { return m_connector->encoder_id; }
  uint32_t* GetConnectorId() const { return &m_connector->connector_id; }
  int GetModesCount() const { return static_cast<int>(m_modes.size()); }
  drmModeModeInfoPtr GetModeForIndex(int index) { return &m_modes[index]; }

  /*!
   * \brief Whether a mode is a frame-packing mode this class synthesised because the
   * kernel offers none on this connector (DisplayPort connectors, including the
   * DP-to-HDMI converters that drive the HDMI port of many Intel boards, never carry
   * stereo modes) although the sink's EDID advertises HDMI 3D.
   */
  static bool IsEmulatedFramePacking(const drmModeModeInfo& mode);

  /*!
   * \brief The timing to hand to the kernel for a mode: unchanged for a native mode,
   * the doubled frame-packing raster (both eyes plus the active-space gap as one
   * progressive picture, e.g. 1920x2205 @ 24 Hz) for an emulated frame-packing mode.
   * Mirrors what drm_mode_set_crtcinfo(CRTC_STEREO_DOUBLE) computes for a real one.
   */
  static drmModeModeInfo ToScanoutMode(const drmModeModeInfo& mode);

  bool IsConnected() { return m_connector->connection == DRM_MODE_CONNECTED; }
  bool CheckConnector();

  std::vector<uint8_t> GetEDID() const;

private:
  void UpdateModes();
  void AddEmulatedFramePackingModes();

  std::vector<drmModeModeInfo> m_modes;

  struct DrmModeConnectorDeleter
  {
    void operator()(drmModeConnector* p) { drmModeFreeConnector(p); }
  };

  std::unique_ptr<drmModeConnector, DrmModeConnectorDeleter> m_connector;
};

} // namespace GBM
} // namespace WINDOWING
} // namespace KODI
