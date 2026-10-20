/* SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once

#include "ServiceBroker.h"
#include "settings/Settings.h"
#include "settings/SettingsComponent.h"
#include "DVMetadataBuffer.h"
#include "utils/DVBridgeFastRendering.h"
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
enum class OutputMode : int { Standard = 0, HDR10 = 1, Disabled = 2, EnhancedDV = 3, MasterReference = 4, AdaptiveB = 5 };
enum class HDR10SourceMode : int { Native = 0, ReferenceDV = 1, EnhancedDV = 2 };

// Decoder ingress sees the last complete frame-boundary preference snapshot.
inline std::atomic<OutputMode> playbackMode{OutputMode::Standard};
inline std::atomic<HDR10SourceMode> hdr10PlaybackMode{HDR10SourceMode::Native};
inline std::atomic<bool> automaticFastRendering{false};

#ifdef HAVE_DVBRIDGE
inline int PolicyCode(const dvbridge_policy& policy)
{
  return policy.mode==DVBRIDGE_MODE_ENHANCED_DV && policy.adaptive!=DVBRIDGE_ADAPTIVE_A ? 6 : int(policy.mode);
}
#endif

struct OutputPreferences
{
  OutputMode output{OutputMode::Standard};
  HDR10SourceMode hdr10Source{HDR10SourceMode::Native};
  int method{0}, panel{0}, gamut{0};
  double peak{0};
  bool optimizedDV{true};
  bool fastChroma{false};
  int fastRenderingMode{0};
  int felUpscaling{0};
  bool processingOptimizations{false};
  uint64_t revision{1};

  bool ProfileValid() const
  {
    return std::isfinite(peak) && peak > 0 && peak <= 10000 &&
        panel >= 0 && panel <= 2 && gamut >= 1 && gamut <= 3;
  }

  bool SameValues(const OutputPreferences& other) const
  {
    return output == other.output && hdr10Source == other.hdr10Source && method == other.method &&
        panel == other.panel && gamut == other.gamut && optimizedDV == other.optimizedDV &&
        fastChroma == other.fastChroma && fastRenderingMode == other.fastRenderingMode &&
        felUpscaling == other.felUpscaling &&
        processingOptimizations == other.processingOptimizations &&
        std::bit_cast<uint64_t>(peak) == std::bit_cast<uint64_t>(other.peak);
  }

#ifdef HAVE_DVBRIDGE
  int EffectiveFastRenderingMode(dvbridge_mode mode, bool hdr10AI = false) const
  {
    return hdr10AI ? fastRenderingMode : mode == DVBRIDGE_MODE_STANDARD ? 1 :
        mode == DVBRIDGE_MODE_MASTER_REFERENCE ? 0 : fastRenderingMode;
  }
  bool EffectiveFastChroma(dvbridge_mode mode, bool hdr10AI = false) const
  {
    return FastRenderingEnabled(EffectiveFastRenderingMode(mode, hdr10AI), fastChroma);
  }
  dvbridge_policy Policy() const
  {
    dvbridge_policy result{};
    result.revision = revision;
    switch (output)
    {
      case OutputMode::Disabled: result.mode = DVBRIDGE_MODE_DISABLED; break;
      case OutputMode::Standard: result.mode = DVBRIDGE_MODE_STANDARD; break;
      case OutputMode::EnhancedDV:
      case OutputMode::AdaptiveB:
        result.mode=DVBRIDGE_MODE_ENHANCED_DV;
        result.adaptive=DVBRIDGE_ADAPTIVE_B_FP16;
        break;
      case OutputMode::MasterReference: result.mode = DVBRIDGE_MODE_MASTER_REFERENCE; break;
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
    result.adaptive=DVBRIDGE_ADAPTIVE_A;
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
  next.output = mode == 3 ? OutputMode::AdaptiveB :
      mode >= 0 && mode <= 5 ? static_cast<OutputMode>(mode) : OutputMode::Standard;
  if (settings)
  {
    const int hdr10 = settings->GetInt("dvbridge.hdr10source");
    next.hdr10Source = hdr10 >= 0 && hdr10 <= 2 ?
        static_cast<HDR10SourceMode>(hdr10) : HDR10SourceMode::Native;
    next.method = settings->GetInt("dvbridge.hdr10method");
    next.peak = settings->GetNumber("dvbridge.tvpeak");
    next.panel = settings->GetInt("dvbridge.tvpanel");
    next.gamut = settings->GetInt("dvbridge.tvgamut");
    next.fastRenderingMode = settings->GetInt("dvbridge.fastchroma");
    next.felUpscaling = settings->GetInt("dvbridge.felupscaling");
    next.fastChroma = FastRenderingEnabled(next.fastRenderingMode, automaticFastRendering.load());
  }
  if (next.method < 0 || next.method > 1) next.method = 0;
  if (next.felUpscaling < 0 || next.felUpscaling > 1) next.felUpscaling = 0;
  next.revision = previous.revision;
  if (next.output!=previous.output || next.hdr10Source!=previous.hdr10Source ||
      next.fastRenderingMode!=previous.fastRenderingMode)
  {
    automaticFastRendering.store(false);
    next.fastChroma=FastRenderingEnabled(next.fastRenderingMode,false);
  }
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
  return true;
}
inline bool AsyncPreparationEnabled()
{
  const auto component = CServiceBroker::GetSettingsComponent();
  const auto settings = component ? component->GetSettings() : nullptr;
  return settings && settings->GetBool("dvbridge.asyncpreparation");
}
inline bool FELHardwareUpscalingEnabled()
{
  return true;
}
} // namespace DVBRIDGE
