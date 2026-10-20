/* SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once

#include <cstdint>

extern "C"
{
#include <libavcodec/codec_id.h>
#include <libavutil/dovi_meta.h>
#include <libavutil/frame.h>
#include <libavutil/hdr_dynamic_metadata.h>
#include <libavutil/mastering_display_metadata.h>
}

#include <memory>
#include <mutex>
#include <string>
#include <vector>

// Original source facts, owned with a picture and published only after acceptance.
// Logical data is immutable; only the on-demand wire representation is cached.
class VideoSourceSideData
{
public:
  static std::shared_ptr<const VideoSourceSideData> Capture(
      uint64_t generation, uint64_t serial, AVCodecID codec, const AVFrame* frame,
      const AVDOVIDecoderConfigurationRecord* config,
      const AVMasteringDisplayMetadata* mastering = nullptr,
      const AVContentLightMetadata* light = nullptr) noexcept;
  uint64_t Generation() const { return m_generation; }
  uint64_t Serial() const { return m_serial; }
  unsigned RejectedSections() const { return m_rejected; }
  const std::string& JSON() const;

private:
  VideoSourceSideData(uint64_t generation, uint64_t serial)
    : m_generation(generation), m_serial(serial) {}
  const uint64_t m_generation, m_serial;
  unsigned m_rejected{};
  std::vector<uint8_t> m_config, m_rpu, m_mdcv, m_cll;
  std::unique_ptr<const AVDynamicHDRPlus> m_hdr10plus;
  mutable std::once_flag m_jsonOnce;
  mutable std::string m_json;
};
