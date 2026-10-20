/* SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once

#include "ServiceBroker.h"
#include "settings/Settings.h"
#include "settings/SettingsComponent.h"
#include "DVMetadataBuffer.h"
#include <atomic>
#include <bit>
#include <cmath>
#include <cstdint>
#include <mutex>
#ifdef HAVE_DVBRIDGE
#include "dvbridge_policy.h"
#endif

namespace DVBRIDGE
{
enum class OutputMode : int { Standard = 0, HDR10 = 1, Disabled = 2, EnhancedDV = 3 };
enum class HDR10SourceMode : int { Native = 0, ReferenceDV = 1, EnhancedDV = 2 };

// Decoder ingress sees the last complete frame-boundary preference snapshot.
inline std::atomic<OutputMode> playbackMode{OutputMode::Standard};
inline std::atomic<HDR10SourceMode> hdr10PlaybackMode{HDR10SourceMode::Native};

struct OutputPreferences
{
  OutputMode output{OutputMode::Standard};
  HDR10SourceMode hdr10Source{HDR10SourceMode::Native};
  int method{0}, preset{0}, panel{0}, gamut{0};
  double peak{0};
  uint64_t revision{1};

  bool ProfileValid() const
  {
    return std::isfinite(peak) && peak > 0 && peak <= 10000 &&
        panel >= 0 && panel <= 2 && gamut >= 1 && gamut <= 3;
  }

  bool SameValues(const OutputPreferences& other) const
  {
    return output == other.output && hdr10Source == other.hdr10Source && method == other.method && preset == other.preset &&
        panel == other.panel && gamut == other.gamut &&
        std::bit_cast<uint64_t>(peak) == std::bit_cast<uint64_t>(other.peak);
  }

#ifdef HAVE_DVBRIDGE
  dvbridge_policy Policy() const
  {
    dvbridge_policy result{};
    result.revision = revision;
    result.enhancement = preset == 1 ? DVBRIDGE_ENHANCEMENT_INTENSE : DVBRIDGE_ENHANCEMENT_NATURAL;
    switch (output)
    {
      case OutputMode::Disabled: result.mode = DVBRIDGE_MODE_DISABLED; break;
      case OutputMode::Standard: result.mode = DVBRIDGE_MODE_STANDARD; break;
      case OutputMode::EnhancedDV: result.mode = DVBRIDGE_MODE_ENHANCED_DV; break;
      case OutputMode::HDR10:
        result.mode = method == 1 ? DVBRIDGE_MODE_HDR10_EXPERT : DVBRIDGE_MODE_HDR10_BASIC;
        break;
    }
    // Basic/native paths never consume calibration. Keep invalid user values
    // in this snapshot, rather than silently completing the stored profile.
    if (result.mode == DVBRIDGE_MODE_HDR10_EXPERT ||
        result.mode == DVBRIDGE_MODE_ENHANCED_DV)
      result.tv = {peak, static_cast<dvbridge_panel>(panel), static_cast<dvbridge_gamut>(gamut)};
    return result;
  }
  dvbridge_policy HDR10Policy() const
  {
    auto result = Policy();
    result.mode = hdr10Source == HDR10SourceMode::ReferenceDV ? DVBRIDGE_MODE_STANDARD :
        hdr10Source == HDR10SourceMode::EnhancedDV ? DVBRIDGE_MODE_ENHANCED_DV : DVBRIDGE_MODE_DISABLED;
    result.tv = result.mode == DVBRIDGE_MODE_ENHANCED_DV ?
        dvbridge_tv_profile{peak,static_cast<dvbridge_panel>(panel),static_cast<dvbridge_gamut>(gamut)} : dvbridge_tv_profile{};
    return result;
  }
#endif
};

inline std::mutex& OutputSettingsMutex()
{
  static std::mutex mutex;
  return mutex;
}

#ifdef CB1_HAS_HDR10_AI
// Protected by OutputSettingsMutex; confirmation retries without changing settings.
inline uint64_t hdr10Reselection{};
#endif

inline OutputPreferences FrameOutputSettings()
{
  static OutputPreferences previous;
#ifdef CB1_HAS_HDR10_AI
  static uint64_t previousReselection{};
#endif
  std::lock_guard<std::mutex> lock(OutputSettingsMutex());
  const auto component = CServiceBroker::GetSettingsComponent();
  const auto settings = component ? component->GetSettings() : nullptr;
  const int mode = settings ? settings->GetInt("dvbridge.outputmode") : 0;
  OutputPreferences next;
  next.output = mode >= 0 && mode <= 3 ? static_cast<OutputMode>(mode) : OutputMode::Standard;
  if (settings)
  {
    const int hdr10 = settings->GetInt("dvbridge.hdr10source");
    next.hdr10Source = hdr10 >= 0 && hdr10 <= 2 ?
        static_cast<HDR10SourceMode>(hdr10) : HDR10SourceMode::Native;
    next.method = settings->GetInt("dvbridge.hdr10method");
    next.preset = settings->GetInt("dvbridge.enhancement");
    next.peak = settings->GetNumber("dvbridge.tvpeak");
    next.panel = settings->GetInt("dvbridge.tvpanel");
    next.gamut = settings->GetInt("dvbridge.tvgamut");
  }
  if (next.method < 0 || next.method > 1) next.method = 0;
  if (next.preset < 0 || next.preset > 1) next.preset = 0;
  next.revision = previous.revision;
  bool changed = !next.SameValues(previous);
#ifdef CB1_HAS_HDR10_AI
  changed |= previousReselection != hdr10Reselection;
  previousReselection = hdr10Reselection;
#endif
  if (changed) next.revision = NextRevision();
  previous = next;
  playbackMode.store(next.output);
  hdr10PlaybackMode.store(next.hdr10Source);
  return next;
}

inline void SelectOutputSettings()
{
  FrameOutputSettings();
}

inline bool EnhancedInfoEnabled()
{
  const auto component = CServiceBroker::GetSettingsComponent();
  const auto settings = component ? component->GetSettings() : nullptr;
  return settings && settings->GetBool("dvbridge.enhancedinfo");
}
inline bool PlaybackMenuEnabled()
{
  const auto component = CServiceBroker::GetSettingsComponent();
  const auto settings = component ? component->GetSettings() : nullptr;
  return settings && settings->GetBool("dvbridge.playbackmenu");
}
inline bool AsyncPreparationEnabled()
{
  const auto component = CServiceBroker::GetSettingsComponent();
  const auto settings = component ? component->GetSettings() : nullptr;
  return settings && settings->GetBool("dvbridge.asyncpreparation");
}
} // namespace DVBRIDGE
