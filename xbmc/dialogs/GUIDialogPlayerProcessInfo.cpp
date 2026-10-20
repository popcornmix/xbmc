/*
 *  Copyright (C) 2005-2018 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#include "GUIDialogPlayerProcessInfo.h"

#include "input/actions/Action.h"
#include "input/actions/ActionIDs.h"
#ifdef HAVE_DVBRIDGE
#include "DVPlaybackModes.h"
#include "DVPixelFormat.h"
#include "cores/VideoPlayer/DVDCodecs/Video/DVOutputSettings.h"
#include "cores/VideoPlayer/DVDCodecs/Video/DVPlaybackInfo.h"
#include "cores/DataCacheCore.h"
#include "guilib/guiinfo/VideoProcessingInfoGUI.h"
#include "utils/StringUtils.h"
#include "utils/Variant.h"
#include "utils/CPUInfo.h"
#include "resources/LocalizeStrings.h"
#include "resources/ResourcesComponent.h"
#include "guilib/GUIMessage.h"
#include "guilib/GUIControlGroupList.h"
#include "guilib/GUIFontManager.h"
#include "windowing/WinSystem.h"
#include "windowing/GraphicContext.h"
#include <algorithm>
#include <cmath>
#endif

CGUIDialogPlayerProcessInfo::CGUIDialogPlayerProcessInfo(void)
    : CGUIDialog(WINDOW_DIALOG_PLAYER_PROCESS_INFO, "DialogPlayerProcessInfo.xml")
{
  m_loadType = KEEP_IN_MEMORY;
}

CGUIDialogPlayerProcessInfo::~CGUIDialogPlayerProcessInfo(void) = default;

bool CGUIDialogPlayerProcessInfo::OnMessage(CGUIMessage& message)
{
#ifdef HAVE_DVBRIDGE
  if (message.GetMessage() == GUI_MSG_WINDOW_INIT)
  {
    const bool enhanced = DVBRIDGE::EnhancedInfoEnabled();
    const std::string file = enhanced ?
        "special://xbmc/media/cb1/DialogPlayerProcessInfo.xml" :
        "DialogPlayerProcessInfo.xml";
    if (GetProperty("xmlfile").asString() != file)
    {
      FreeResources(true);
      SetProperty("xmlfile", file);
    }
    if (enhanced)
    {
      const RESOLUTION_INFO resolution(1920, 1080);
      SetCoordsRes(resolution);
      g_fontManager.LoadTTF("CB1.InfoBody", "special://xbmc/media/Fonts/arial.ttf",
                            0xfff4f6f6, 0, 23, 0, false, 1.0f, 1.0f, &resolution);
      g_fontManager.LoadTTF("CB1.InfoSection", "special://xbmc/media/Fonts/arial.ttf",
                            0xfff4f6f6, 0, 25, 0, false, 1.0f, 1.0f, &resolution);
      g_fontManager.LoadTTF("CB1.InfoTitle", "special://xbmc/media/Fonts/arial.ttf",
                            0xfff4f6f6, 0, 30, 1, false, 1.0f, 1.0f, &resolution);
    }
  }
#endif
  return CGUIDialog::OnMessage(message);
}

void CGUIDialogPlayerProcessInfo::OnInitWindow()
{
#ifdef HAVE_DVBRIDGE
  const bool enhanced = DVBRIDGE::EnhancedInfoEnabled();
  SetProperty("DV.Enhanced", enhanced);
  m_nextRefresh = 0;
  m_renderUsage.Reset();
  SetProperty("DV.GPUAvailable", false);
  SetProperty("DV.GPURender", "N/A");
  if (enhanced)
    RefreshInfo();
#endif
  CGUIDialog::OnInitWindow();
}

void CGUIDialogPlayerProcessInfo::OnDeinitWindow(int nextWindowID)
{
#ifdef HAVE_DVBRIDGE
  m_renderUsage.Reset();
#endif
  CGUIDialog::OnDeinitWindow(nextWindowID);
}

#ifdef HAVE_DVBRIDGE
void CGUIDialogPlayerProcessInfo::RefreshInfo()
{
    const auto render = m_renderUsage.Sample();
    SetProperty("DV.GPUAvailable", render.has_value());
    SetProperty("DV.GPURender", render ? StringUtils::Format("{:.0f} %", *render) : "N/A");
    SetProperty("DV.GPURenderPercent", render ? StringUtils::Format("{:.1f}", *render) : "0");
    for (const int gauge : {9400, 9401})
    {
      CGUIMessage usage(GUI_MSG_ITEM_SELECT, GetID(), gauge, render ? std::lround(*render) : 0);
      OnMessage(usage);
    }
    const auto data = CServiceBroker::GetDataCacheCore().GetVideoProcessingInfo();
    SetProperty("DV.SourcePixelFormat", DVBRIDGE::SourcePixelFormat(static_cast<AVPixelFormat>(data.sourceFormat)));
    SetProperty("DV.SourceBits", data.sourceBits ? StringUtils::Format("{}-bit", data.sourceBits) : "N/A");
    const auto active = data.output.flags;
    const bool hdr10 = active & CDVPlaybackInfo::HDR10;
    const bool metadata = active && data.metadata;
    const auto yesno = [](bool value) { return value ? "Yes" : "No"; };
    const auto resolution = CServiceBroker::GetWinSystem()->GetGfxContext().GetResInfo();
    SetProperty("DV.Display", StringUtils::Format("{}x{} / {:.3f} Hz",
        resolution.iScreenWidth, resolution.iScreenHeight, resolution.fRefreshRate));
    if (active && data.output.width && data.output.height && data.output.refresh>0)
      SetProperty("DV.Display", StringUtils::Format("{}x{} / {:.3f} Hz",
          data.output.width,data.output.height,data.output.refresh));
    const auto cpu = CServiceBroker::GetCPUInfo();
    SetProperty("DV.CPU", cpu ? StringUtils::Format("{} %", cpu->GetUsedPercentage()) : "N/A");
    SetProperty("DV.DroppedSkipped", data.counters ?
        StringUtils::Format("{} / {}", data.dropped, data.skipped) : "N/A");
    SetProperty("DV.SourceMastering", data.sourceMastering ?
        StringUtils::Format("{:.0f} / {:.4f} nits", data.sourceMax, data.sourceMin) : "N/A");
    SetProperty("DV.SourceLight", data.sourceLight ?
        StringUtils::Format("{} / {} nits", data.sourceCLL, data.sourceFALL) : "N/A");
    SetProperty("DV.Profile", data.profile ? StringUtils::Format("{}", data.profile) : "N/A");
    SetProperty("DV.Level", data.profile ? StringUtils::Format("{}", data.level) : "N/A");
    SetProperty("DV.Version", data.profile ? StringUtils::Format("{}.{}", data.major, data.minor) : "N/A");
    SetProperty("DV.CM", metadata ? (data.cm4 ? "CM4 source" : "CM2.9 source") : "");
    const auto applied=data.AppliedBlocks();
    SetProperty("DV.ParsedBlocks", metadata ? CDVPlaybackInfo::Blocks(data.blocks) : "");
    SetProperty("DV.AppliedBlocks", metadata ? CDVPlaybackInfo::Blocks(applied) : "");
    SetProperty("DV.TransmittedBlocks", active & CDVPlaybackInfo::HDMI ? CDVPlaybackInfo::Blocks(data.TransmittedBlocks())+
        (data.output.authoredLegacy ? " (authored)" : "") : "");
    SetProperty("DV.UnappliedBlocks", metadata ? CDVPlaybackInfo::Blocks(data.blocks & ~applied) : "");
    SetProperty("DV.RPU", data.profile ? yesno(data.rpu) : "N/A");
    SetProperty("DV.BL", data.profile ? yesno(data.bl) : "N/A");
    SetProperty("DV.EL", data.profile ? yesno(data.el) : "N/A");
    SetProperty("DV.FEL", active ? (active & CDVPlaybackInfo::FEL ? "Full FEL" : "No FEL") : "");
    SetProperty("DV.FELUpscaling", active & CDVPlaybackInfo::FEL ?
        (data.output.felUpscaling == 2 ? "Reference GPU" : data.output.felUpscaling == 0 ? "VAAPI" : data.output.felUpscaling == 1 ? "Nearest neighbour (GPU)" : "") : "");
    SetProperty("DV.Mode", active ? VideoProcessingInfoGUI::Mode(data) : "Kodi default");
    SetProperty("DV.FastRendering", active ?
        std::string(data.output.fastChroma ? "On" : "Off") +
        (data.output.fastRenderingMode == 2 ? " (Auto)" : "") : "");
    SetProperty("DV.Engine", data.output.engine);
    SetProperty("DV.Circuit", data.output.circuit);
    SetProperty("DV.Reconstruction", "");
    SetProperty("DV.AIL1", "");
    SetProperty("DV.AIL3", "");
    SetProperty("DV.OutputL1", "");
    SetProperty("DV.PartialBlocks", hdr10 && data.output.effective==3 && metadata ?
        std::string(data.blocks[1] ? "L1 max/avg" : "")+
        (data.blocks[3] ? (data.blocks[1] ? "; L3 max/avg" : "L3 max/avg") : "") : "");
    SetProperty("DV.ActivePolicy", active ? std::string(CDVPlaybackInfo::RequestedPolicy(data.output.effective)) : "Kodi default");
    if (active && data.output.hdr10AI)
    {
      const auto& localizer=CServiceBroker::GetResourcesComponent().GetLocalizeStrings();
      const int label=data.output.effective==1 ? 40867 : 40868;
      SetProperty("DV.Mode", localizer.Get(label));
      SetProperty("DV.ActivePolicy", localizer.Get(label));
      SetProperty("DV.Reconstruction", "AI-estimated L1/L3");
      SetProperty("DV.Profile", "");
      SetProperty("DV.Level", "");
      SetProperty("DV.Version", "");
      const auto& raw=data.output.aiL1L3;
      SetProperty("DV.AIL1", StringUtils::Format("{} | {} | {}",raw[0],raw[1],raw[2]));
      SetProperty("DV.AIL3", StringUtils::Format("{} | {} | {}",raw[3],raw[4],raw[5]));
      if (data.output.emittedL1Known)
        SetProperty("DV.OutputL1", StringUtils::Format("{} | {} | {}",
            data.output.emittedL1[0],data.output.emittedL1[1],data.output.emittedL1[2]));
    }
    const auto& localizer=CServiceBroker::GetResourcesComponent().GetLocalizeStrings();
    const auto reasonText=[&](VideoProcessingReason reason) {
      const int id=CDVPlaybackInfo::ReasonLabel(static_cast<DVBRIDGE::OutputReason>(reason));
      return id ? localizer.Get(id) : std::string{};
    };
    SetProperty("DV.Fallback", active && data.output.fallback ? reasonText(data.output.reason) : "");
    SetProperty("DV.MinimumLinkBits", active && data.output.minimumLinkBits ?
        StringUtils::Format(localizer.Get(40852),data.output.minimumLinkBits) : "");
    SetProperty("DV.Transport", active ? CDVPlaybackInfo::Transport(data.output.format) : "");
    SetProperty("DV.SignalIntent", active & CDVPlaybackInfo::HDMI ? "TV-Led Dolby Vision tunnel" :
        hdr10 ? "PQ / BT.2020" : "");
    SetProperty("DV.Committed", active && data.output.stream ? StringUtils::Format("{} / {} / {}",
        data.output.stream,data.output.picture,data.output.revision) : "");
    // Next-picture preparation is not an output-mode transition.
    const bool transaction=data.transaction.identity.Valid() &&
        std::string_view(data.transaction.stage)!="Committed" &&
        (data.transaction.atomicError ||
         data.transaction.reason==VideoProcessingReason::ActivationFailed ||
         data.transaction.reason==VideoProcessingReason::RestoreFailed ||
         std::string_view(data.transaction.stage)=="Failed commit" ||
         (active ? (
          data.transaction.identity.stream!=data.output.stream ||
          data.transaction.requested!=data.output.requested ||
          data.transaction.effective!=data.output.effective ||
          data.transaction.reason!=data.output.reason) :
          (data.transaction.requested || data.transaction.reason!=VideoProcessingReason::None)));
    SetProperty("DV.ProcessingVisible", bool(active) || transaction);
    SetProperty("DV.RequestedPolicy", transaction ? std::string(CDVPlaybackInfo::RequestedPolicy(data.transaction.requested,
        data.transaction.hdr10AI)) :
        active ? CDVPlaybackInfo::RequestedPolicy(data.output.requested,data.output.hdr10AI) : "");
    SetProperty("DV.AdmissionReason", transaction ? reasonText(data.transaction.reason) : "");
    SetProperty("DV.AtomicResult", transaction && data.transaction.atomicError ?
        StringUtils::Format("errno={} / failures={} / restored={}",data.transaction.atomicError,
            data.transaction.retryCount,yesno(data.transaction.restored)) : "");
    SetProperty("DV.OutputStage", transaction ?
        std::string(data.transaction.stage)+" (not committed)" : "");
    SetProperty("DV.ReferenceWarning", data.referenceWarning ? "Consider Reference" : "");
    SetProperty("DV.TVProfile", active && (data.output.effective==3 || data.output.effective==4 || data.output.effective==6) ?
        StringUtils::Format("{} / {:g} nits",
            data.output.gamut==1 ? "BT.709" : data.output.gamut==2 ? "P3-D65" : "BT.2020",
            data.output.manualPeak) : "");
    SetProperty("DV.HDR10Policy", hdr10 ? CDVPlaybackInfo::Policy(data.output) : "");
    SetProperty("DV.HDR10Target", hdr10 ? StringUtils::Format("{:g} nits", data.output.target) : "");
    SetProperty("DV.HDR10TargetLabel", hdr10 ? (data.output.effective==2 ? "Source reference" :
        data.output.effective==3 ? "Display reference" : "Mapping target") : "");
    SetProperty("DV.CreativeTrims", active ? (data.output.creativeKnown ?
        (data.output.creativeStatus==static_cast<int>(VideoCreativeStatus::Invalid) ||
         data.output.creativeStatus==static_cast<int>(VideoCreativeStatus::Unsupported) ?
            "Treatment unavailable / original metadata preserved" :
         data.output.creativeStatus==static_cast<int>(VideoCreativeStatus::Neutral) ?
            "Neutral / original metadata preserved" :
         hdr10 ? (data.output.creativeApplied & ((1u<<2)|(1u<<8)) ?
            "Applied in HDR10 processing" : "No compatible creative trims") :
          data.output.creativeApplied ? "Metadata-guided expansion" :
          data.output.creativeEdited ? "Metadata adjusted for TV" : "Original metadata preserved") :
        data.output.authoredLegacy || hdr10 ? "Source trims not applied" : "Forwarded to TV") : "");
    SetProperty("DV.Precision", active ? StringUtils::Format("{}-bit framebuffer", data.output.bits) : "N/A");
    SetProperty("DV.L1PQ", metadata && data.l1 ? StringUtils::Format("{} | {} | {}", data.pq[0], data.pq[1], data.pq[2]) : "N/A");
    const auto nits = [](unsigned pq) {
      const double p = std::pow(pq / 4095.0, 32.0 / 2523.0);
      return 10000.0 * std::pow(std::max(p - 3424.0 / 4096.0, 0.0) /
          (2413.0 / 128.0 - 2392.0 / 128.0 * p), 16384.0 / 2610.0);
    };
    SetProperty("DV.L1Nits", metadata && data.l1 ? StringUtils::Format("{:.1f} | {:.1f} | {:.1f} nits",
        nits(data.pq[0]), nits(data.pq[1]), nits(data.pq[2])) : "N/A");
    SetProperty("DV.L5", metadata && data.l5 ? StringUtils::Format("{} | {} | {} | {}",
        data.area[0], data.area[1], data.area[2], data.area[3]) : "N/A");
    SetProperty("DV.Mastering", metadata && data.l6 ? StringUtils::Format("{} | {:.4f} nits",
        data.maxMaster, data.minMaster / 10000.0) : "N/A");
    SetProperty("DV.CLL", metadata && data.l6 ? StringUtils::Format("{} | {} nits", data.maxCLL, data.maxFALL) : "N/A");
}
#endif

void CGUIDialogPlayerProcessInfo::Process(unsigned int currentTime, CDirtyRegionList& dirtyregions)
{
#ifdef HAVE_DVBRIDGE
  const bool refresh = DVBRIDGE::EnhancedInfoEnabled() && currentTime >= m_nextRefresh;
  if (refresh)
  {
    m_nextRefresh = currentTime + 500;
    RefreshInfo();
  }
#endif
  CGUIDialog::Process(currentTime, dirtyregions);
#ifdef HAVE_DVBRIDGE
  if (refresh)
  {
    // Measure native grouplists after hidden rows have collapsed.
    for (int layout = 0; layout < 2; ++layout)
    {
      auto* panel = GetControl(9600 + layout);
      auto* surface = GetControl(9610 + layout);
      if (!panel || !surface)
        continue;
      float content = 0.0f;
      for (int column = 0; column < 3; ++column)
        if (const auto* list = dynamic_cast<const CGUIControlGroupList*>(
                GetControl(9620 + layout * 10 + column)))
          content = std::max(content, list->GetTotalSize());
      if (content <= 0.0f)
        continue;
      const float height = 108.0f + content + 16.0f;
      panel->SetHeight(height);
      panel->SetPosition(panel->GetXPosition(), GetCoordsRes().iHeight - 24.0f - height);
      surface->SetHeight(height + 40.0f);
    }
  }
#endif
}

bool CGUIDialogPlayerProcessInfo::OnAction(const CAction &action)
{
  if (action.GetID() == ACTION_PLAYER_PROCESS_INFO)
  {
#ifdef HAVE_DVBRIDGE
    const bool showMenu = CDVPlaybackModes::MenuEnabled();
    Close(showMenu);
    if (showMenu)
      CDVPlaybackModes::ShowMenu();
#else
    Close();
#endif
    return true;
  }
  return CGUIDialog::OnAction(action);
}
