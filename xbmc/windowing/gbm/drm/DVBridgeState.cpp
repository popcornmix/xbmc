/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "DVBridgeState.h"
#include "DRMAtomic.h"
#include "DRMConnector.h"

using namespace KODI::WINDOWING::GBM;

CDVBridgeState::Backend CDVBridgeState::GetBackend(CDRMConnector& connector)
{
  if (connector.SupportsProperty("DVBRIDGE_DV_STANDARD_LAB"))
    return Backend::KERNEL;
  if (connector.GetType() == "HDMI-A" && connector.SupportsProperty("NV_HDMI_VSIF_METADATA") &&
      connector.SupportsProperty("max bpc") && connector.SupportsProperty("dithering mode"))
    return Backend::NVIDIA;
  return Backend::NONE;
}

bool CDVBridgeState::Capture(CDRMAtomic& drm, uint64_t ownVsif)
{
  m_captureFailure = "none";
  const auto reject = [this](const char* reason) {
    m_captureFailure = reason;
    return false;
  };
  if (Captured())
    return reject("already-captured");
  auto* connector = drm.GetConnector();
  auto* crtc = drm.GetCrtc();
  auto* plane = drm.GetGuiPlane() ? drm.GetGuiPlane() : drm.GetVideoPlane();
  if (!connector || !crtc || !plane || (drm.GetGuiPlane() && drm.GetVideoPlane()))
    return reject("output-objects");
  const Backend backend = GetBackend(*connector);
  if (backend == Backend::NONE ||
      (backend == Backend::KERNEL && connector->GetPropertyValue("DVBRIDGE_DV_STANDARD_LAB") != 0))
    return reject("DVBRIDGE_DV_STANDARD_LAB");
  const auto vsifBefore = connector->GetPropertyValue("NV_HDMI_VSIF_METADATA");
  const bool vsifOwn = ownVsif != 0 && vsifBefore == ownVsif;
  if (backend == Backend::NVIDIA && vsifBefore != 0 && !vsifOwn)
    return reject("NV_HDMI_VSIF_METADATA");

  std::vector<Property> saved;
  const auto save = [&](CDRMObject* object, const char* name, uint64_t transport,
                        bool required, bool blob, const char* enumName = nullptr) {
    auto value = object->GetPropertyValue(name);
    if (!value)
      return !required || reject(name);
    if (enumName)
    {
      auto selected = object->GetPropertyEnumValue(name, enumName);
      if (!selected)
        return reject(name);
      transport = *selected;
    }
    Property property{object, name, *value, transport, {}, {}};
    if (blob && *value)
    {
      auto* original = drmModeGetPropertyBlob(drm.GetFileDescriptor(), *value);
      if (!original)
        return reject(name);
      property.blob = CDRMPropertyBlob(drm.GetFileDescriptor(), original->data, original->length);
      drmModeFreePropertyBlob(original);
      if (!property.blob.IsValid())
        return reject(name);
      property.original = property.blob.Get();
    }
    saved.push_back(std::move(property));
    return true;
  };

  // TV-Led does not require HDR10 static metadata; preserve it when exposed.
  if (backend == Backend::KERNEL &&
      (!save(connector, "DVBRIDGE_DV_STANDARD_LAB", 1, true, false) ||
      !save(connector, "HDR_OUTPUT_METADATA", 0, false, true) ||
      !save(connector, "max bpc", 8, true, false) ||
      !save(connector, "Broadcast RGB", 0, true, false, "Full") ||
      !save(connector, "Colorspace", 0, true, false, "Default")))
    return false;

  if (backend == Backend::NVIDIA)
  {
    // Hades RC2: vendor payload, RGB8 depth and neutral NVIDIA color pipeline.
    constexpr uint8_t payload[27] = {0x46, 0xd0, 0x00, 0x02};
    constexpr uint64_t unity = uint64_t{1} << 32;
    CDRMPropertyBlob vsif(drm.GetFileDescriptor(), payload, sizeof(payload));
    if (!vsif.IsValid()) return reject("NV_HDMI_VSIF_METADATA");
    if (!save(connector, "NV_HDMI_VSIF_METADATA", vsif.Get(), true, true)) return false;
    saved.back().transportBlob = std::move(vsif);
    if (vsifOwn)
    {
      saved.back().blob = CDRMPropertyBlob();
      saved.back().original = 0;
    }
    if (!save(connector, "HDR_OUTPUT_METADATA", 0, true, true) ||
        !save(connector, "max bpc", 8, true, false) ||
        !save(connector, "dithering mode", 0, true, false, "off") ||
        !save(connector, "Colorspace", 0, true, false, "Default") ||
        !save(crtc, "NV_CRTC_REGAMMA_TF", 0, false, false, "Default") ||
        !save(crtc, "NV_CRTC_REGAMMA_LUT", 0, false, true) ||
        !save(crtc, "NV_CRTC_REGAMMA_DIVISOR", unity, false, false) ||
        !save(crtc, "NV_CRTC_COLOR_PASSTHROUGH", 0, false, false) ||
        !save(plane, "NV_INPUT_COLORSPACE", 0, false, false, "None") ||
        !save(plane, "NV_HDR_STATIC_METADATA", 0, false, true) ||
        !save(plane, "NV_PLANE_LMS_CTM", 0, false, true) ||
        !save(plane, "NV_PLANE_LMS_TO_ITP_CTM", 0, false, true) ||
        !save(plane, "NV_PLANE_ITP_TO_LMS_CTM", 0, false, true) ||
        !save(plane, "NV_PLANE_BLEND_CTM", 0, false, true) ||
        !save(plane, "NV_PLANE_DEGAMMA_TF", 0, false, false, "Default") ||
        !save(plane, "NV_PLANE_DEGAMMA_LUT", 0, false, true) ||
        !save(plane, "NV_PLANE_DEGAMMA_MULTIPLIER", unity, false, false) ||
        !save(plane, "NV_PLANE_TMO_LUT", 0, false, true) ||
        !save(plane, "NV_PLANE_COLOR_PASSTHROUGH", 0, false, false))
      return false;
  }
  if (!save(crtc, "CTM", 0, false, true) ||
      !save(crtc, "GAMMA_LUT", 0, false, true) ||
      !save(crtc, "DEGAMMA_LUT", 0, false, true) ||
      !save(crtc, "VRR_ENABLED", 0, false, false) ||
      !save(plane, "rotation", DRM_MODE_ROTATE_0, false, false) ||
      !save(plane, "alpha", 65535, false, false) ||
      !save(plane, "COLOR_PIPELINE", 0, false, false) ||
      !save(plane, "pixel blend mode", 0, false, false, "None"))
    return false;

  // Do not silently corrupt tunnel bytes with another plane or hardware cursor.
  auto* planes = drmModeGetPlaneResources(drm.GetFileDescriptor());
  if (!planes)
    return reject("plane-resources");
  bool safe = true;
  for (uint32_t i = 0; i < planes->count_planes; ++i)
  {
    auto* other = drmModeGetPlane(drm.GetFileDescriptor(), planes->planes[i]);
    if (!other)
    {
      reject("plane-query");
      safe = false;
      break;
    }
    safe &= other->plane_id == plane->GetId() || other->crtc_id != crtc->GetCrtcId();
    if (!safe)
      reject("active-plane");
    drmModeFreePlane(other);
  }
  drmModeFreePlaneResources(planes);
  if (!safe)
    return false;
  m_saved = std::move(saved);
  m_owner = &drm;
  m_backend = backend;
  return true;
}

bool CDVBridgeState::Stage(CDRMAtomic& drm, bool restore)
{
  if (!Captured() || m_owner != &drm || m_saved.front().object != drm.GetConnector())
    return false;
  bool valid = true;
  for (const auto& property : m_saved)
    valid &= drm.AddProperty(property.object, property.name.c_str(),
                             restore ? property.original : property.transport);
  return valid;
}
