/* SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once
#include "DVOutputSettings.h"
#include "DVPlaybackInfo.h"
#ifdef HAVE_DVBRIDGE
#include <array>

namespace DVBRIDGE
{
enum class OutputRoute { Vanilla, NativeDV, HDR10 };
struct OutputSelection
{
  OutputRoute route{OutputRoute::Vanilla};
  dvbridge_policy policy{};
  OutputReason reason{OutputReason::None};
};

inline OutputSelection SelectOutput(const OutputPreferences& preferences,
                                    const OutputCapabilities& caps)
{
  OutputSelection result;
  if (caps.stereoSource)
    return {OutputRoute::Vanilla, {}, OutputReason::SourceIneligible};
  if (caps.sourceKnown && caps.isHDR10 && !caps.isDV)
  {
    if (preferences.hdr10Source == HDR10SourceMode::Native) return result;
    auto policy = preferences.HDR10Policy();
    if (policy.mode == DVBRIDGE_MODE_DISABLED)
      return {OutputRoute::Vanilla,{},OutputReason::SourceIneligible};
    if (policy.mode != DVBRIDGE_MODE_STANDARD && !preferences.ProfileValid())
      return {OutputRoute::Vanilla,{},OutputReason::TVProfileRequired};
    if (!caps.nativeDV)
      return {OutputRoute::Vanilla,{},OutputReason::NoDVTransport};
    if (!caps.processingKnown || !caps.processing[policy.mode].usable)
      return {OutputRoute::Vanilla,{},OutputReason::NoGPUProcessing};
    return {OutputRoute::NativeDV,policy,OutputReason::None};
  }
  if (!caps.sourceKnown || !caps.isDV || preferences.output == OutputMode::Disabled)
    return result;
  if (!caps.processingKnown)
  {
    result.reason = OutputReason::NoGPUProcessing;
    return result;
  }
  auto policy = preferences.Policy();
  if (policy.mode == DVBRIDGE_MODE_MASTER_REFERENCE && preferences.fastRenderingMode == 2 &&
      automaticFastRendering.load())
    policy.mode = DVBRIDGE_MODE_STANDARD;
  const auto admission = [&](dvbridge_mode mode) {
    if ((mode == DVBRIDGE_MODE_HDR10_EXPERT || mode == DVBRIDGE_MODE_ENHANCED_DV) &&
        !preferences.ProfileValid())
      return ModeAdmission{false, OutputReason::TVProfileRequired};
    auto value = caps.processing[mode];
    if (!value.usable && value.reason == OutputReason::None)
      value.reason = OutputReason::NoGPUProcessing;
    return value;
  };
  const bool requestedDV = preferences.output != OutputMode::HDR10;
  auto candidate = admission(policy.mode);
  if ((requestedDV ? caps.nativeDV : caps.hdr10) && candidate.usable)
    return {requestedDV ? OutputRoute::NativeDV : OutputRoute::HDR10, policy, OutputReason::None};
  result.reason = !(requestedDV ? caps.nativeDV : caps.hdr10) ?
      (requestedDV ? OutputReason::NoDVTransport : OutputReason::NoHDR10Transport) : candidate.reason;
  if (!requestedDV) return result;
  if (!caps.hdr10)
  {
    result.reason = OutputReason::NoHDR10Transport;
    return result;
  }
  candidate = admission(DVBRIDGE_MODE_HDR10_BASIC);
  if (!candidate.usable)
  {
    result.reason = candidate.reason;
    return result;
  }
  policy.mode = DVBRIDGE_MODE_HDR10_BASIC;
  policy.adaptive = DVBRIDGE_ADAPTIVE_A;
  policy.tv = {};
  return {OutputRoute::HDR10, policy, result.reason};
}
} // namespace DVBRIDGE
#endif
