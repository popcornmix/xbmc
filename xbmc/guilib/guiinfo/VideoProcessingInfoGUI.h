/* SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once
#include "GUIInfoLabels.h"
#include "cores/VideoProcessingInfo.h"
#include "cores/VideoPlayer/DVDCodecs/Video/DVPlaybackInfo.h"
#include "ServiceBroker.h"
#include "resources/LocalizeStrings.h"
#include "resources/ResourcesComponent.h"

struct VideoProcessingInfoGUI
{
  static std::string Mode(const VideoProcessingInfo& info)
  {
    if (!info.output.flags) return "";
    if (info.output.hdr10AI)
      return CServiceBroker::GetResourcesComponent().GetLocalizeStrings().Get(
          info.output.effective==1 ? 40867 : 40868);
    return CDVPlaybackInfo::Mode(info.output);
  }
  static bool GetLabel(uint32_t id, const VideoProcessingInfo& info, std::string& value)
  {
    value.clear();
    switch (id)
    {
      case PLAYER_PROCESS_VIDEOOUTPUTHDRTYPE:
        if (info.outputType)
          switch (*info.outputType)
          {
            case VideoOutputType::SDR: value="sdr"; break;
            case VideoOutputType::HDR10: value="hdr10"; break;
            case VideoOutputType::HLG: value="hlg"; break;
            case VideoOutputType::DolbyVision: value="dolbyvision"; break;
          }
        break;
      case PLAYER_PROCESS_VIDEOPROCESSINGMODE: value=Mode(info); break;
      case PLAYER_PROCESS_VIDEOPROCESSINGENGINE: value=info.output.engine; break;
      case PLAYER_PROCESS_VIDEOPROCESSINGCIRCUIT: value=info.output.circuit; break;
      case PLAYER_PROCESS_VIDEOPROCESSINGFALLBACK:
        if (info.output.fallback)
          value=CDVPlaybackInfo::Reason(static_cast<DVBRIDGE::OutputReason>(info.output.reason));
        break;
      case PLAYER_PROCESS_VIDEODVCMVERSION:
        if (info.metadata) value=info.cm4 ? "CM4 source" : "CM2.9 source";
        break;
      case PLAYER_PROCESS_VIDEODVPARSEDBLOCKS:
        if (info.metadata) value=CDVPlaybackInfo::Blocks(info.blocks);
        break;
      case PLAYER_PROCESS_VIDEODVAPPLIEDBLOCKS:
        if (info.metadata) value=CDVPlaybackInfo::Blocks(info.appliedBlocks);
        break;
      case PLAYER_PROCESS_VIDEODVTRANSMITTEDBLOCKS:
        value=CDVPlaybackInfo::Blocks(info.transmittedBlocks);
        break;
      case PLAYER_PROCESS_VIDEOSOURCEBITS:
        if (info.sourceBits) value=std::to_string(info.sourceBits)+"-bit";
        break;
      case PLAYER_PROCESS_VIDEODVPROFILE:
        if (info.profile) value=std::to_string(info.profile);
        break;
      case PLAYER_PROCESS_VIDEODVLEVEL:
        if (info.profile) value=std::to_string(info.level);
        break;
      case PLAYER_PROCESS_VIDEODVVERSION:
        if (info.profile) value=std::to_string(info.major)+"."+std::to_string(info.minor);
        break;
      case PLAYER_PROCESS_VIDEODVLAYERS:
        if (info.profile) value=info.fel ? "FEL" : info.el ? "MEL" : "BL";
        break;
      case PLAYER_PROCESS_VIDEOPROCESSINGFAST:
        if (info.output.flags) value=info.output.fastChroma ? "On" : "Off";
        break;
      case PLAYER_PROCESS_VIDEOOUTPUTFORMAT:
        if (info.output.flags) value=CDVPlaybackInfo::Transport(info.output.format);
        break;
      case PLAYER_PROCESS_VIDEOOUTPUTBITS:
        if (info.output.bits) value=std::to_string(info.output.bits)+"-bit";
        break;
      default: return false;
    }
    return true;
  }
};
