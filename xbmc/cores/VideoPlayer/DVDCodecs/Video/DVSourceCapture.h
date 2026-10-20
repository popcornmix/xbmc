/*
 *  Copyright (C) 2026 Team LibreELEC
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#pragma once

#include "DVMetadataBuffer.h"

#include <cstdint>

extern "C"
{
#include <libavutil/mastering_display_metadata.h>
}

struct AVFrame;
class CDVDStreamInfo;
struct VideoPicture;

namespace DVBRIDGE
{
/*!
 * \brief Attaches CB1 source information to decoded pictures for decoders that
 *        do not run through CDVDVideoCodecFFmpeg (DRM PRIME / V4L2 stateless).
 *
 * Mirrors the capture done in CDVDVideoCodecFFmpeg::GetPictureCommon(): the
 * per-picture source binding, source info, RPU metadata and FEL/MEL type, and
 * the HDR10 static metadata latch that keeps a DV stream from being treated as
 * a format change when the SEI is only carried on the first IRAP.
 */
class CSourceCapture
{
public:
  void Open(const CDVDStreamInfo& hints);
  void Seek();
  void Close();
  bool NativeOutput() const { return m_nativeOutput; }

  //! Call after the decoder has filled hdrType, colorBits and the HDR10 metadata.
  void Capture(VideoPicture* picture,
               const AVFrame* frame,
               const CDVDStreamInfo& hints,
               uint64_t informationGeneration);

private:
  bool m_nativeOutput{false};
  SourceOwner m_owner;
  bool m_hasDisplayMetadata{false};
  AVMasteringDisplayMetadata m_displayMetadata{};
  bool m_hasLightMetadata{false};
  AVContentLightMetadata m_lightMetadata{};
};
} // namespace DVBRIDGE
