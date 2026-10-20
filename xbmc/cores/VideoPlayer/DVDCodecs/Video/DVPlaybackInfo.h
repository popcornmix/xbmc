/* SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once
#include <atomic>
#include <string>
#include <sstream>
#include <bitset>
#include <array>
#include <cstdint>

namespace DVBRIDGE
{
enum class OutputReason { None, NoDVTransport, NoHDR10Transport,
  NoGPUProcessing, SourceIneligible, TVProfileRequired, ActivationFailed, RestoreFailed };
struct ModeAdmission { bool usable{}; OutputReason reason{OutputReason::None}; };
struct OutputCapabilities
{
  bool sourceKnown{}, isDV{}, processingKnown{}, nativeDV{}, hdr10{};
  uint64_t generation{};
  std::array<ModeAdmission, 5> processing{};
  bool isHDR10{};
  std::array<ModeAdmission, 5> hdr10Processing{};
};
}

// The render thread publishes one coherent snapshot only after presentation.
// This is output telemetry, never a source-format or TV certification flag.
struct CDVPlaybackInfo
{
  enum : unsigned { HDMI = 1, FEL = 2, CM4 = 4, HDR10 = 8 };
  inline static std::atomic<unsigned> presented{0};
  static int ReasonLabel(DVBRIDGE::OutputReason reason)
  { return reason == DVBRIDGE::OutputReason::None ? 0 : 40840 + static_cast<int>(reason); }

  static const char* Reason(DVBRIDGE::OutputReason reason)
  {
    switch(reason)
    {
      case DVBRIDGE::OutputReason::NoDVTransport: return "no-dv-transport";
      case DVBRIDGE::OutputReason::NoHDR10Transport: return "no-hdr10-transport";
      case DVBRIDGE::OutputReason::NoGPUProcessing: return "gpu-processing-unavailable";
      case DVBRIDGE::OutputReason::SourceIneligible: return "source-ineligible";
      case DVBRIDGE::OutputReason::TVProfileRequired: return "tv-profile-required";
      case DVBRIDGE::OutputReason::ActivationFailed: return "activation-failed";
      case DVBRIDGE::OutputReason::RestoreFailed: return "restoration-failed";
      default: return "none";
    }
  }

  static std::string Detail(std::string source, unsigned output)
  {
    if (!(output & (HDMI | HDR10)))
      return source;
    if ((output & FEL) && source.find("FEL") == std::string::npos)
      source += " FEL";
    source += (output & CM4) ? " · CM4 metadata" : " · CM2.9 metadata";
    return source + ((output & HDR10) ? " · HDR10 Basic" : " · HDMI DV");
  }

  template<class Output> static const char* Policy(const Output& output)
  {
    switch (output.effective)
    {
      case 2: return "HDR10 Reference Basic";
      case 3: return "HDR10 Reference Expert";
      case 4: return "DV Enhanced";
      default: return output.flags & HDR10 ? "HDR10 Reference Basic" : "DV Standard";
    }
  }
  static const char* RequestedPolicy(int mode)
  {
    switch(mode)
    {
      case 0:return "DV Disabled (native Kodi)";
      case 1:return "DV Standard";
      case 2:return "HDR10 Reference Basic";
      case 3:return "HDR10 Reference Expert";
      case 4:return "DV Enhanced";
      default:return "";
    }
  }

  template<class Output> static std::string Mode(const Output& output)
  {
    if (!output.flags) return "Kodi default";
    std::string mode = output.flags & HDR10 ? Policy(output) :
        output.effective == 4 ? "DV Enhanced" : "DV Standard (TV-Led)";
    if (output.effective == 4)
      mode += output.enhancement ? " - Signature" : " - Natural";
    if (output.fallback)
      mode += " / compatibility fallback";
    return mode;
  }

  static std::string Blocks(const std::bitset<256>& blocks)
  {
    std::string text;
    for(unsigned level=0;level<blocks.size();++level)
      if(blocks[level]) text+=(text.empty() ? "" : " ")+std::string("L")+std::to_string(level);
    return text;
  }

  static std::string Transport(uint32_t format)
  {
    switch(format)
    {
      case 0x34325258: case 0x34325241: return "RGB 4:4:4 / 8-bit";
      case 0x30335258: case 0x30335241: return "RGB 4:4:4 / 10-bit";
      default: return ""; // Unknown negotiated format is unavailable, never guessed.
    }
  }

  template<class Output> static std::string Detail(std::string source, const Output& output)
  {
    if (!output.flags) return source;
    if ((output.flags & FEL) && source.find("FEL") == std::string::npos) source += " FEL";
    source += output.flags & CM4 ? " · CM4 metadata" : " · CM2.9 metadata";
    source += " · " + Mode(output);
    return source;
  }
};
