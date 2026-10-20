/* SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once
#include "DVOutputSettings.h"
#include "HDR10AIActivation.h"

namespace DVBRIDGE
{
struct HDR10AIBuffers
{
  static constexpr unsigned Native = 6, Maximum = 64;
  static unsigned Count(bool hdr10, HDR10SourceMode mode, double fps, bool interlaced)
  {
    if (!hdr10 || mode < HDR10SourceMode::ReferenceDV ||
        mode > HDR10SourceMode::EnhancedDV || interlaced) return Native;
    const auto count = HDR10AIActivation::BufferCount(fps);
    return count ? count : Native;
  }
  static unsigned References(unsigned count) { return count == Native ? 4 : count; }
  static unsigned DecodeSurfaces(unsigned nativeCount, unsigned count)
  { return nativeCount + (count > Native ? count-Native : 0); }
  static unsigned RenderPictures(unsigned count) { return count == Native ? 7 : count+1; }
};
}
