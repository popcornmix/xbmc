/* SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once
#include <atomic>

class CDVDStreamInfo;
class VideoPicture;

namespace DVBRIDGE
{
inline std::atomic<int> modeChangeResolution{-1};
// Keep software metadata probes and ordinary HDR/SDR playback on Kodi's path.
constexpr bool Eligible(bool standardDVDisplay, bool transportAvailable,
                        bool dolbySource, bool softwareProbe, bool interlaced, bool stereo)
{
  return standardDVDisplay && transportAvailable && dolbySource &&
         !softwareProbe && !interlaced && !stereo;
}

bool WantsNativeOutput(const CDVDStreamInfo& stream);
bool RetainsDVSource(const CDVDStreamInfo& stream);
bool WantsNativeOutput(const VideoPicture* picture);
} // namespace DVBRIDGE
