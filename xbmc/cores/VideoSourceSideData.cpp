/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "VideoSourceSideData.h"

#include "utils/Base64.h"
#include "utils/JSONVariantWriter.h"
#include "utils/Variant.h"

#include <array>
#include <algorithm>
#include <cstring>
#include <limits>
#include <new>

namespace
{
constexpr size_t MAX_RAW_RPU = 1024 * 1024;
constexpr std::array<uint8_t, 6> HDR10PLUS_HEADER{0xb5, 0, 0x3c, 0, 1, 4};

void PutBE(std::vector<uint8_t>& bytes, uint32_t value, unsigned count)
{
  for (unsigned i = count; i; --i)
    bytes.push_back(static_cast<uint8_t>(value >> (8 * (i - 1))));
}

bool Scaled(AVRational value, uint32_t scale, uint32_t maximum, uint32_t& result)
{
  if (value.den <= 0 || value.num < 0)
    return false;
  const uint64_t product = uint64_t(value.num) * scale;
  // Round rational source fields to the nearest representable wire unit.
  const uint64_t rounded = (product + uint64_t(value.den) / 2) / value.den;
  if (rounded > maximum)
    return false;
  result = static_cast<uint32_t>(rounded);
  return true;
}

bool Mastering(const AVMasteringDisplayMetadata& value, std::vector<uint8_t>& bytes)
{
  // The wire format cannot mark individual unknown fields; omit partial records.
  if (!value.has_primaries || !value.has_luminance)
    return false;
  std::vector<uint8_t> result;
  uint32_t scaled;
  for (unsigned primary : {1u, 2u, 0u}) // FFmpeg RGB -> SEI GBR
    for (unsigned axis = 0; axis < 2; ++axis)
    {
      if (!Scaled(value.display_primaries[primary][axis], 50000, 65535, scaled))
        return false;
      PutBE(result, scaled, 2);
    }
  for (const auto& point : value.white_point)
  {
    if (!Scaled(point, 50000, 65535, scaled))
      return false;
    PutBE(result, scaled, 2);
  }
  for (const auto& luminance : {value.max_luminance, value.min_luminance})
  {
    if (!Scaled(luminance, 10000, std::numeric_limits<uint32_t>::max(), scaled))
      return false;
    PutBE(result, scaled, 4);
  }
  bytes = std::move(result);
  return true;
}

// Validate exactly the public FFmpeg9 serializer's rational arithmetic/counts.
// Its internal integer multiplication must not overflow before division.
bool HDRValue(AVRational q, int scale, unsigned bits, bool wide = false)
{
  return q.den > 0 && q.num >= 0 && (wide || q.num <= std::numeric_limits<int>::max() / scale) &&
      uint64_t(q.num) * scale / q.den < (uint64_t{1} << bits);
}

bool HDR10Plus(const AVDynamicHDRPlus& value)
{
  // FFmpeg9 writes application version 1 and has no safe general multi-window
  // color-saturation serialization. Omit unsupported forms, never rewrite facts.
  if (value.application_version != 1 || value.num_windows != 1 ||
      value.targeted_system_display_actual_peak_luminance_flag > 1 ||
      value.mastering_display_actual_peak_luminance_flag > 1 ||
      !HDRValue(value.targeted_system_display_maximum_luminance, 1, 27, true))
    return false;
  const auto peak = [](bool present, unsigned rows, unsigned cols, const auto& values)
  {
    if (!present) return true;
    if (rows < 2 || rows > 25 || cols < 2 || cols > 25) return false;
    for (unsigned i = 0; i < rows; ++i)
      for (unsigned j = 0; j < cols; ++j)
        if (!HDRValue(values[i][j], 15, 4)) return false;
    return true;
  };
  if (!peak(value.targeted_system_display_actual_peak_luminance_flag,
            value.num_rows_targeted_system_display_actual_peak_luminance,
            value.num_cols_targeted_system_display_actual_peak_luminance,
            value.targeted_system_display_actual_peak_luminance) ||
      !peak(value.mastering_display_actual_peak_luminance_flag,
            value.num_rows_mastering_display_actual_peak_luminance,
            value.num_cols_mastering_display_actual_peak_luminance,
            value.mastering_display_actual_peak_luminance)) return false;
  const auto& p = value.params[0];
  if (p.num_distribution_maxrgb_percentiles > 15 || p.num_bezier_curve_anchors > 15 ||
      p.tone_mapping_flag > 1 || p.color_saturation_mapping_flag > 1 ||
      (!p.tone_mapping_flag && p.color_saturation_mapping_flag) ||
      !HDRValue(p.average_maxrgb, 100000, 17, true) ||
      !HDRValue(p.fraction_bright_pixels, 1000, 10)) return false;
  for (const auto& max : p.maxscl)
    if (!HDRValue(max, 100000, 17, true)) return false;
  for (unsigned i = 0; i < p.num_distribution_maxrgb_percentiles; ++i)
    if (p.distribution_maxrgb[i].percentage > 100 ||
        !HDRValue(p.distribution_maxrgb[i].percentile, 100000, 17, true)) return false;
  if (p.tone_mapping_flag)
  {
    if (!HDRValue(p.knee_point_x, 4095, 12) || !HDRValue(p.knee_point_y, 4095, 12)) return false;
    for (unsigned i = 0; i < p.num_bezier_curve_anchors; ++i)
      if (!HDRValue(p.bezier_curve_anchors[i], 1023, 10)) return false;
    if (p.color_saturation_mapping_flag && !HDRValue(p.color_saturation_weight, 8, 6)) return false;
  }
  return true;
}
}

std::shared_ptr<const VideoSourceSideData> VideoSourceSideData::Capture(
    uint64_t generation, uint64_t serial, AVCodecID codec, const AVFrame* frame,
    const AVDOVIDecoderConfigurationRecord* config,
    const AVMasteringDisplayMetadata* mastering, const AVContentLightMetadata* light) noexcept
{
  try
  {
    auto result = std::shared_ptr<VideoSourceSideData>(new VideoSourceSideData(generation, serial));
    const auto section = [&](unsigned bit, const auto& capture)
    {
      try { if (!capture()) result->m_rejected |= bit; }
      catch (const std::bad_alloc&) { result->m_rejected |= bit; }
    };
    if (config && config->dv_profile)
      section(1, [&]
      {
        if (config->dv_profile > 127 || config->dv_level > 63 ||
            config->dv_bl_signal_compatibility_id > 15 || config->dv_md_compression > 3)
          return false;
        result->m_config.assign(24, 0);
        auto& bytes = result->m_config;
        bytes[0] = config->dv_version_major; bytes[1] = config->dv_version_minor;
        bytes[2] = config->dv_profile << 1 | config->dv_level >> 5;
        bytes[3] = (config->dv_level & 31) << 3 | !!config->rpu_present_flag << 2 |
            !!config->el_present_flag << 1 | !!config->bl_present_flag;
        bytes[4] = config->dv_bl_signal_compatibility_id << 4 | config->dv_md_compression << 2;
        return true;
      });
    if (frame)
    {
      if (const auto* sd = av_frame_get_side_data(frame, AV_FRAME_DATA_DOVI_RPU_BUFFER))
        section(2, [&]
        {
          if (!sd->data || !sd->size || sd->size > MAX_RAW_RPU - 2) return false;
          std::vector<uint8_t> bytes;
          if (codec == AV_CODEC_ID_HEVC)
            bytes = {0x7c, 1}; // pinned FFmpeg9 excludes this NAL header
          else if (codec != AV_CODEC_ID_AV1 || sd->size < 8 ||
                   std::memcmp(sd->data, "\xb5\x00\x3b\x00\x00\x08\x00", 7))
            return false;
          bytes.insert(bytes.end(), sd->data, sd->data + sd->size);
          result->m_rpu = std::move(bytes);
          return true;
        });
      if (const auto* sd = av_frame_get_side_data(frame, AV_FRAME_DATA_DYNAMIC_HDR_PLUS))
        section(4, [&]
        {
          if (!sd->data || sd->size != sizeof(AVDynamicHDRPlus)) return false;
          const auto& value = *reinterpret_cast<const AVDynamicHDRPlus*>(sd->data);
          if (!HDR10Plus(value)) return false;
          result->m_hdr10plus = std::make_unique<AVDynamicHDRPlus>(value);
          return true;
        });
      if (const auto* sd = av_frame_get_side_data(frame, AV_FRAME_DATA_MASTERING_DISPLAY_METADATA))
      {
        mastering = sd->data && sd->size == sizeof(*mastering) ?
            reinterpret_cast<const AVMasteringDisplayMetadata*>(sd->data) : nullptr;
        if (!mastering) result->m_rejected |= 8;
      }
      if (const auto* sd = av_frame_get_side_data(frame, AV_FRAME_DATA_CONTENT_LIGHT_LEVEL))
      {
        light = sd->data && sd->size == sizeof(*light) ?
            reinterpret_cast<const AVContentLightMetadata*>(sd->data) : nullptr;
        if (!light) result->m_rejected |= 16;
      }
    }
    if (mastering) section(8, [&] { return Mastering(*mastering, result->m_mdcv); });
    if (light) section(16, [&]
    {
      if (light->MaxCLL > 65535 || light->MaxFALL > 65535) return false;
      std::vector<uint8_t> bytes;
      PutBE(bytes, light->MaxCLL, 2); PutBE(bytes, light->MaxFALL, 2);
      result->m_cll = std::move(bytes);
      return true;
    });
    return result;
  }
  catch (const std::bad_alloc&) { return {}; } // diagnostics must not stop decoding
}

const std::string& VideoSourceSideData::JSON() const
{
  std::call_once(m_jsonOnce, [&]
  {
    try
    {
      CVariant object(CVariant::VariantTypeObject);
      const auto add = [&](const char* key, const auto& bytes)
      {
        if (!bytes.empty())
          object[key] = Base64::Encode(reinterpret_cast<const char*>(bytes.data()), bytes.size());
      };
      add("dovi.config", m_config); add("dovi.rpu", m_rpu);
      add("mdcv", m_mdcv); add("cll", m_cll);
      if (m_hdr10plus)
      {
        // Stack storage avoids a raw allocation whose ownership could escape on failure.
        std::array<uint8_t, AV_HDR_PLUS_MAX_PAYLOAD_SIZE + HDR10PLUS_HEADER.size()> bytes{};
        std::copy(HDR10PLUS_HEADER.begin(), HDR10PLUS_HEADER.end(), bytes.begin());
        uint8_t* data = bytes.data() + HDR10PLUS_HEADER.size();
        size_t size = AV_HDR_PLUS_MAX_PAYLOAD_SIZE;
        if (av_dynamic_hdr_plus_to_t35(m_hdr10plus.get(), &data, &size) >= 0)
          object["hdr10plus"] = Base64::Encode(reinterpret_cast<const char*>(bytes.data()),
                                              size + HDR10PLUS_HEADER.size());
      }
      if (!CJSONVariantWriter::Write(object, m_json, true)) m_json = "{}";
    }
    catch (const std::bad_alloc&) { m_json.clear(); }
  });
  return m_json;
}
