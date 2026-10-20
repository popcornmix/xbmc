/* SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once
#include <string>
extern "C"
{
#include <libavutil/pixdesc.h>
}

namespace DVBRIDGE
{
// Describe the decoded source, never the opaque hardware surface or HDMI tunnel.
inline std::string SourcePixelFormat(AVPixelFormat format)
{
  const auto* desc = av_pix_fmt_desc_get(format);
  if (!desc || (desc->flags & AV_PIX_FMT_FLAG_HWACCEL))
    return {};
  if (desc->flags & AV_PIX_FMT_FLAG_RGB)
    return "RGB 4:4:4";
  if (desc->nb_components < 3)
    return "Grayscale";
  if (desc->log2_chroma_w == 1 && desc->log2_chroma_h == 1)
    return "YUV 4:2:0";
  if (desc->log2_chroma_w == 1 && desc->log2_chroma_h == 0)
    return "YUV 4:2:2";
  if (desc->log2_chroma_w == 0 && desc->log2_chroma_h == 0)
    return "YUV 4:4:4";
  return desc->name ? desc->name : "";
}
}
