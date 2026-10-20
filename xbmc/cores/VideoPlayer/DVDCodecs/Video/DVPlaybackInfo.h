/* SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once
#include <atomic>
#include <string>

// The render thread publishes one coherent snapshot only after presentation.
// This is output telemetry, never a source-format or TV certification flag.
struct CDVPlaybackInfo
{
  enum : unsigned { HDMI = 1, FEL = 2, CM4 = 4 };
  inline static std::atomic<unsigned> presented{0};

  static std::string Detail(std::string source, unsigned output)
  {
    if (!(output & HDMI))
      return source;
    if ((output & FEL) && source.find("FEL") == std::string::npos)
      source += " FEL";
    source += (output & CM4) ? " · CM4" : " · CM2.9";
    return source + " · HDMI DV";
  }
};
