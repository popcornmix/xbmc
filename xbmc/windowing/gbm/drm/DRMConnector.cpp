/*
 *  Copyright (C) 2005-2020 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#include "DRMConnector.h"

#include "utils/XTimeUtils.h"
#include "utils/log.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <string_view>

using namespace KODI::WINDOWING::GBM;

using namespace std::chrono_literals;

namespace
{

std::map<int, std::string> connectorTypeNames = {
    {DRM_MODE_CONNECTOR_Unknown, "unknown"},
    {DRM_MODE_CONNECTOR_VGA, "VGA"},
    {DRM_MODE_CONNECTOR_DVII, "DVI-I"},
    {DRM_MODE_CONNECTOR_DVID, "DVI-D"},
    {DRM_MODE_CONNECTOR_DVIA, "DVI-A"},
    {DRM_MODE_CONNECTOR_Composite, "composite"},
    {DRM_MODE_CONNECTOR_SVIDEO, "s-video"},
    {DRM_MODE_CONNECTOR_LVDS, "LVDS"},
    {DRM_MODE_CONNECTOR_Component, "component"},
    {DRM_MODE_CONNECTOR_9PinDIN, "9-pin DIN"},
    {DRM_MODE_CONNECTOR_DisplayPort, "DP"},
    {DRM_MODE_CONNECTOR_HDMIA, "HDMI-A"},
    {DRM_MODE_CONNECTOR_HDMIB, "HDMI-B"},
    {DRM_MODE_CONNECTOR_TV, "TV"},
    {DRM_MODE_CONNECTOR_eDP, "eDP"},
    {DRM_MODE_CONNECTOR_VIRTUAL, "Virtual"},
    {DRM_MODE_CONNECTOR_DSI, "DSI"},
    {DRM_MODE_CONNECTOR_DPI, "DPI"},
};

std::map<drmModeConnection, std::string> connectorStatusNames = {
    {DRM_MODE_CONNECTED, "connected"},
    {DRM_MODE_DISCONNECTED, "disconnected"},
    {DRM_MODE_UNKNOWNCONNECTION, "unknown"},
};

/*!
 * \brief Does the sink's EDID say it accepts HDMI 1.4 3D video (3D_present in the
 * HDMI Vendor-Specific Data Block)? The kernel parses this too, but only turns it into
 * stereo modes on connectors flagged stereo_allowed, which DisplayPort never is.
 */
bool EdidAdvertisesHdmi3D(const std::vector<uint8_t>& edid)
{
  if (edid.size() < 256)
    return false;

  const size_t extensions = edid[126];
  for (size_t ext = 1; ext <= extensions && (ext + 1) * 128 <= edid.size(); ext++)
  {
    const uint8_t* block = edid.data() + ext * 128;
    if (block[0] != 0x02 || block[1] < 3) // CTA-861 extension, revision 3 or later
      continue;

    const size_t dtdStart = std::min<size_t>(block[2], 127);
    size_t pos = 4;
    while (pos < dtdStart)
    {
      const uint8_t tag = block[pos] >> 5;
      const size_t len = block[pos] & 0x1f;
      const uint8_t* body = block + pos + 1;
      if (pos + 1 + len > dtdStart)
        break;

      // Vendor-Specific Data Block with the HDMI Licensing OUI 00-0C-03
      if (tag == 3 && len >= 8 && body[0] == 0x03 && body[1] == 0x0c && body[2] == 0x00)
      {
        const uint8_t flags = body[7];
        if (flags & 0x20) // HDMI_Video_present
        {
          size_t q = 8;
          if (flags & 0x80) // Latency_Fields_Present
            q += 2;
          if (flags & 0x40) // I_Latency_Fields_Present
            q += 2;
          if (q < len && (body[q] & 0x80)) // 3D_present
            return true;
        }
      }
      pos += 1 + len;
    }
  }

  return false;
}

// CTA-861 timings for which HDMI 1.4a (section 8.2.3) makes frame packing mandatory
// on a 3D-capable sink: 1080p 23.98/24, 720p 59.94/60 and 720p 50 (VICs 32, 4 and 19).
bool IsMandatoryFramePackingTiming(const drmModeModeInfo& mode)
{
  if (mode.flags & (DRM_MODE_FLAG_INTERLACE | DRM_MODE_FLAG_DBLSCAN | DRM_MODE_FLAG_3D_MASK))
    return false;

  if (mode.hdisplay == 1920 && mode.vdisplay == 1080 && mode.htotal == 2750 &&
      mode.vtotal == 1125)
    return mode.clock == 74250 || mode.clock == 74176;

  if (mode.hdisplay == 1280 && mode.vdisplay == 720 && mode.vtotal == 750 &&
      (mode.htotal == 1650 || mode.htotal == 1980))
    return mode.clock == 74250 || mode.clock == 74176;

  return false;
}

} // namespace

CDRMConnector::CDRMConnector(int fd, uint32_t connector)
  : CDRMObject(fd), m_connector(drmModeGetConnector(m_fd, connector))
{
  if (!m_connector)
    throw std::runtime_error("drmModeGetConnector failed: " + std::string{strerror(errno)});

  if (!GetProperties(m_connector->connector_id, DRM_MODE_OBJECT_CONNECTOR))
    throw std::runtime_error("failed to get properties for connector: " +
                             std::to_string(m_connector->connector_id));

  UpdateModes();
}

bool CDRMConnector::CheckConnector()
{
  unsigned retryCnt = 7;
  while (!IsConnected() && retryCnt > 0)
  {
    CLog::LogF(LOGDEBUG, "connector is disconnected");
    retryCnt--;
    KODI::TIME::Sleep(1s);

    m_connector.reset(drmModeGetConnector(m_fd, m_connector->connector_id));
    UpdateModes();
  }

  return m_connector->connection == DRM_MODE_CONNECTED;
}

void CDRMConnector::UpdateModes()
{
  m_modes.assign(m_connector->modes, m_connector->modes + m_connector->count_modes);

  if (m_connector->connection == DRM_MODE_CONNECTED)
    AddEmulatedFramePackingModes();
}

void CDRMConnector::AddEmulatedFramePackingModes()
{
  // An environment switch for bring-up and bug reports.
  const char* env = std::getenv("KODI_3D_EMULATE_FRAME_PACKING");
  if (env && std::string_view{env} == "0")
    return;

  // Native HDMI connectors get stereo modes from the kernel; only emulate when the
  // kernel offers none at all for this connector.
  const bool kernelHasStereo = std::any_of(m_modes.begin(), m_modes.end(), [](const auto& m) {
    return (m.flags & DRM_MODE_FLAG_3D_MASK) != 0;
  });
  if (kernelHasStereo)
    return;

  if (!EdidAdvertisesHdmi3D(GetEDID()))
    return;

  std::vector<drmModeModeInfo> emulated;
  for (const auto& mode : m_modes)
  {
    if (!IsMandatoryFramePackingTiming(mode))
      continue;

    drmModeModeInfo fp = mode;
    fp.flags |= DRM_MODE_FLAG_3D_FRAME_PACKING;
    fp.type = (fp.type & ~DRM_MODE_TYPE_PREFERRED) | DRM_MODE_TYPE_USERDEF;
    emulated.push_back(fp);
  }

  if (emulated.empty())
    return;

  CLog::LogF(LOGINFO,
             "connector {} offers no stereo modes but the sink advertises HDMI 3D: emulating "
             "frame packing for {} timing(s) by scanning out the doubled raster as a 2D mode",
             m_connector->connector_id, emulated.size());

  m_modes.insert(m_modes.end(), emulated.begin(), emulated.end());
}

bool CDRMConnector::IsEmulatedFramePacking(const drmModeModeInfo& mode)
{
  return (mode.type & DRM_MODE_TYPE_USERDEF) &&
         (mode.flags & DRM_MODE_FLAG_3D_MASK) == DRM_MODE_FLAG_3D_FRAME_PACKING;
}

drmModeModeInfo CDRMConnector::ToScanoutMode(const drmModeModeInfo& mode)
{
  drmModeModeInfo out = mode;
  if (!IsEmulatedFramePacking(mode))
    return out;

  // Same arithmetic as drm_mode_set_crtcinfo() with CRTC_STEREO_DOUBLE: the picture
  // becomes (vdisplay + vtotal) lines tall - left eye, active-space gap, right eye -
  // at twice the pixel clock, so the refresh rate is unchanged.
  out.clock = mode.clock * 2;
  out.vdisplay = mode.vdisplay + mode.vtotal;
  out.vsync_start = mode.vsync_start + mode.vtotal;
  out.vsync_end = mode.vsync_end + mode.vtotal;
  out.vtotal = mode.vtotal * 2;
  out.flags &= ~DRM_MODE_FLAG_3D_MASK;
  out.type &= ~DRM_MODE_TYPE_USERDEF;
  snprintf(out.name, sizeof(out.name), "%ux%u", out.hdisplay, out.vdisplay);
  return out;
}

std::string CDRMConnector::GetType()
{
  auto typeName = connectorTypeNames.find(m_connector->connector_type);
  if (typeName == connectorTypeNames.end())
    return connectorTypeNames[DRM_MODE_CONNECTOR_Unknown];

  return typeName->second;
}

std::string CDRMConnector::GetStatus()
{
  auto statusName = connectorStatusNames.find(m_connector->connection);
  if (statusName == connectorStatusNames.end())
    return connectorStatusNames[DRM_MODE_UNKNOWNCONNECTION];

  return statusName->second;
}

std::string CDRMConnector::GetName()
{
  return GetType() + "-" + std::to_string(m_connector->connector_type_id);
}

std::vector<uint8_t> CDRMConnector::GetEDID() const
{

  uint64_t blob_id = GetPropertyValue("EDID").value_or(0);

  if (blob_id == 0)
  {
    CLog::LogF(LOGDEBUG, "failed to find EDID property for connector: {}",
               m_connector->connector_id);
    return {};
  }

  drmModePropertyBlobPtr blob = drmModeGetPropertyBlob(m_fd, blob_id);
  if (!blob)
    return {};

  auto data = static_cast<uint8_t*>(blob->data);
  auto edid = std::vector<uint8_t>(data, data + blob->length);

  drmModeFreePropertyBlob(blob);

  return edid;
}
