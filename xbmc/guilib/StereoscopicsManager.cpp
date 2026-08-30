/*
 *  Copyright (C) 2005-2018 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *  See LICENSES/README.md for more information.
 */

/*!
 * @file StereoscopicsManager.cpp
 * @brief This class acts as container for stereoscopic related functions
 */

#include "StereoscopicsManager.h"

#include "GUIComponent.h"
#include "GUIUserMessages.h"
#include "ServiceBroker.h"
#include "application/ApplicationComponents.h"
#include "application/ApplicationPlayer.h"
#include "cores/DataCacheCore.h"
#include "cores/IPlayer.h"
#include "cores/VideoPlayer/Interface/StreamInfo.h"
#include "dialogs/GUIDialogKaiToast.h"
#include "dialogs/GUIDialogSelect.h"
#include "guilib/GUIWindowManager.h"
#include "input/actions/Action.h"
#include "input/actions/ActionIDs.h"
#include "messaging/ApplicationMessenger.h"
#include "rendering/RenderSystem.h"
#include "resources/LocalizeStrings.h"
#include "resources/ResourcesComponent.h"
#include "settings/AdvancedSettings.h"
#include "settings/Settings.h"
#include "settings/SettingsComponent.h"
#include "settings/lib/Setting.h"
#include "settings/lib/SettingsManager.h"
#include "utils/RegExp.h"
#include "utils/StringUtils.h"
#include "utils/Variant.h"
#include "utils/log.h"
#include "windowing/Resolution.h"
#include "windowing/WinSystem.h"

#include <stdlib.h>

struct StereoModeMap
{
  const char* name;
  RenderStereoMode mode;
};

static const struct StereoModeMap VideoModeToGuiModeMap[] = {
    {"mono", RenderStereoMode::OFF},
    {"left_right", RenderStereoMode::SPLIT_VERTICAL},
    {"right_left", RenderStereoMode::SPLIT_VERTICAL},
    {"top_bottom", RenderStereoMode::SPLIT_HORIZONTAL},
    {"bottom_top", RenderStereoMode::SPLIT_HORIZONTAL},
    {"checkerboard_rl", RenderStereoMode::CHECKERBOARD},
    {"checkerboard_lr", RenderStereoMode::CHECKERBOARD},
    {"row_interleaved_rl", RenderStereoMode::INTERLACED},
    {"row_interleaved_lr", RenderStereoMode::INTERLACED},
    {"col_interleaved_rl", RenderStereoMode::OFF}, // unsupported
    {"col_interleaved_lr", RenderStereoMode::OFF}, // unsupported
    {"anaglyph_cyan_red", RenderStereoMode::ANAGLYPH_RED_CYAN},
    {"anaglyph_green_magenta", RenderStereoMode::ANAGLYPH_GREEN_MAGENTA},
    {"anaglyph_yellow_blue", RenderStereoMode::ANAGLYPH_YELLOW_BLUE},
    {"block_lr", RenderStereoMode::OFF}, // unsupported
    {"block_rl", RenderStereoMode::OFF}, // unsupported
    {}};

namespace
{

//! @brief The split arrangement to output, given the one @p mode asks for.
//!
//! Which of the two the source uses is not what decides this - the renderer crops the
//! correct eye either way - and neither is the viewer's taste: a split arrangement is
//! signalled to the sink, so it has to be one the sink can receive. A side-by-side
//! source on a display whose only 3D mode at this timing is frame packed has to go out
//! top-and-bottom, or the resolution search finds no 3D mode and the display stays 2D.
//! The stream parameters the display modes are matched against come from the player,
//! so with nothing playing the arrangement is left as asked for. So it is with refresh
//! rate adjusting turned off: Kodi may not change the display mode then, the mode is
//! whatever was picked by hand in videoscreen.resolution, and overriding the
//! arrangement would only split the frame the way a mode nobody is going to switch to
//! wants it.
RenderStereoMode ArrangementForDisplay(RenderStereoMode mode)
{
  if (mode != RenderStereoMode::SPLIT_VERTICAL && mode != RenderStereoMode::SPLIT_HORIZONTAL)
    return mode;

  if (CServiceBroker::GetSettingsComponent()->GetSettings()->GetInt(
          CSettings::SETTING_VIDEOPLAYER_ADJUSTREFRESHRATE) == ADJUST_REFRESHRATE_OFF)
    return mode;

  const auto& components = CServiceBroker::GetAppComponents();
  const auto appPlayer = components.GetComponent<CApplicationPlayer>();
  if (!appPlayer || !appPlayer->IsPlaying())
    return mode;

  VideoStreamInfo info;
  appPlayer->GetVideoStreamInfo(CURRENT_STREAM, info);
  if (!info.valid || info.fpsRate == 0 || info.fpsScale == 0)
    return mode;

  const float fps = static_cast<float>(info.fpsRate) / static_cast<float>(info.fpsScale);
  const RenderStereoMode arrangement =
      CResolutionUtils::ChooseStereoArrangement(mode, fps, info.width, info.height);

  // A display mode the renderer cannot fill is worse than none: SetStereoMode() applies
  // nothing at all for an unsupported arrangement, which would leave a viewer with no
  // 3D rather than the arrangement they asked for.
  return CServiceBroker::GetRenderSystem()->SupportsStereo(arrangement) ? arrangement : mode;
}

} // namespace

static const struct StereoModeMap StringToGuiModeMap[] = {
    {"off", RenderStereoMode::OFF},
    {"split_vertical", RenderStereoMode::SPLIT_VERTICAL},
    {"side_by_side", RenderStereoMode::SPLIT_VERTICAL}, // alias
    {"sbs", RenderStereoMode::SPLIT_VERTICAL}, // alias
    {"split_horizontal", RenderStereoMode::SPLIT_HORIZONTAL},
    {"over_under", RenderStereoMode::SPLIT_HORIZONTAL}, // alias
    {"tab", RenderStereoMode::SPLIT_HORIZONTAL}, // alias
    {"row_interleaved", RenderStereoMode::INTERLACED},
    {"interlaced", RenderStereoMode::INTERLACED}, // alias
    {"checkerboard", RenderStereoMode::CHECKERBOARD},
    {"anaglyph_cyan_red", RenderStereoMode::ANAGLYPH_RED_CYAN},
    {"anaglyph_green_magenta", RenderStereoMode::ANAGLYPH_GREEN_MAGENTA},
    {"anaglyph_yellow_blue", RenderStereoMode::ANAGLYPH_YELLOW_BLUE},
    {"hardware_based", RenderStereoMode::HARDWAREBASED},
    {"monoscopic", RenderStereoMode::MONO},
    {}};

CStereoscopicsManager::CStereoscopicsManager()
  : m_settings(CServiceBroker::GetSettingsComponent()->GetSettings())
{
  m_stereoModeSetByUser = RenderStereoMode::UNDEFINED;
  m_lastStereoModeSetByUser = RenderStereoMode::UNDEFINED;

  //! @todo Move this to Initialize() to avoid potential problems in ctor
  m_settings->GetSettingsManager()->RegisterCallback(
      this, {CSettings::SETTING_VIDEOSCREEN_STEREOSCOPICMODE});
}

CStereoscopicsManager::~CStereoscopicsManager(void)
{
  m_settings->GetSettingsManager()->UnregisterCallback(this);
}

void CStereoscopicsManager::Initialize()
{
  // turn off stereo mode on XBMC startup
  SetStereoMode(RenderStereoMode::OFF);
}

RenderStereoMode CStereoscopicsManager::GetStereoMode(void) const
{
  return static_cast<RenderStereoMode>(
      m_settings->GetInt(CSettings::SETTING_VIDEOSCREEN_STEREOSCOPICMODE));
}

void CStereoscopicsManager::SetStereoModeByUser(const RenderStereoMode mode)
{
  // only update last user mode if desired mode is different from current
  if (mode != m_stereoModeSetByUser)
    m_lastStereoModeSetByUser = m_stereoModeSetByUser;

  m_stereoModeSetByUser = mode;
  SetStereoMode(mode);
}

void CStereoscopicsManager::SetStereoMode(const RenderStereoMode mode)
{
  RenderStereoMode currentMode = GetStereoMode();
  RenderStereoMode applyMode = mode;

  // resolve automatic mode before applying
  if (mode == RenderStereoMode::AUTO)
    applyMode = GetStereoModeOfPlayingVideo();

  applyMode = ArrangementForDisplay(applyMode);

  if (applyMode != currentMode && applyMode >= RenderStereoMode::OFF)
  {
    if (CServiceBroker::GetRenderSystem()->SupportsStereo(applyMode))
      m_settings->SetInt(CSettings::SETTING_VIDEOSCREEN_STEREOSCOPICMODE,
                         static_cast<int>(applyMode));
  }
}

RenderStereoMode CStereoscopicsManager::GetNextSupportedStereoMode(
    const RenderStereoMode currentMode, int step) const
{
  RenderStereoMode mode = currentMode;

  do
  {
    mode = static_cast<RenderStereoMode>((static_cast<int>(mode) + step) %
                                         static_cast<int>(RenderStereoMode::COUNT));

    if (CServiceBroker::GetRenderSystem()->SupportsStereo(mode))
      break;
  } while (mode != currentMode);

  return mode;
}

std::string CStereoscopicsManager::DetectStereoModeByString(const std::string &needle) const
{
  std::string stereoMode;
  const std::string& searchString(needle);
  CRegExp re(true);

  if (!re.RegComp(CServiceBroker::GetSettingsComponent()->GetAdvancedSettings()->m_stereoscopicregex_3d.c_str()))
  {
    CLog::Log(
        LOGERROR, "{}: Invalid RegExp for matching 3d content:'{}'", __FUNCTION__,
        CServiceBroker::GetSettingsComponent()->GetAdvancedSettings()->m_stereoscopicregex_3d);
    return stereoMode;
  }

  if (re.RegFind(searchString) == -1)
    return stereoMode;    // no match found for 3d content, assume mono mode

  if (!re.RegComp(CServiceBroker::GetSettingsComponent()->GetAdvancedSettings()->m_stereoscopicregex_sbs.c_str()))
  {
    CLog::Log(
        LOGERROR, "{}: Invalid RegExp for matching 3d SBS content:'{}'", __FUNCTION__,
        CServiceBroker::GetSettingsComponent()->GetAdvancedSettings()->m_stereoscopicregex_sbs);
    return stereoMode;
  }

  if (re.RegFind(searchString) > -1)
  {
    stereoMode = "left_right";
    return stereoMode;
  }

  if (!re.RegComp(CServiceBroker::GetSettingsComponent()->GetAdvancedSettings()->m_stereoscopicregex_tab.c_str()))
  {
    CLog::Log(
        LOGERROR, "{}: Invalid RegExp for matching 3d TAB content:'{}'", __FUNCTION__,
        CServiceBroker::GetSettingsComponent()->GetAdvancedSettings()->m_stereoscopicregex_tab);
    return stereoMode;
  }

  if (re.RegFind(searchString) > -1)
    stereoMode = "top_bottom";

  return stereoMode;
}

RenderStereoMode CStereoscopicsManager::GetStereoModeByUserChoice() const
{
  RenderStereoMode mode = GetStereoMode();

  // if no stereo mode is set already, suggest mode of current video by preselecting it
  if (mode == RenderStereoMode::OFF)
    mode = GetStereoModeOfPlayingVideo();

  CGUIDialogSelect* pDlgSelect = CServiceBroker::GetGUI()->GetWindowManager().GetWindow<CGUIDialogSelect>(WINDOW_DIALOG_SELECT);
  pDlgSelect->Reset();

  // "Select stereoscopic 3D mode"
  pDlgSelect->SetHeading(
      CVariant{CServiceBroker::GetResourcesComponent().GetLocalizeStrings().Get(36528)});

  // prepare selectable stereo modes
  std::vector<RenderStereoMode> selectableModes;
  for (int i = static_cast<int>(RenderStereoMode::OFF);
       i < static_cast<int>(RenderStereoMode::COUNT); ++i)
  {
    RenderStereoMode selectableMode = static_cast<RenderStereoMode>(i);
    if (CServiceBroker::GetRenderSystem()->SupportsStereo(selectableMode))
    {
      selectableModes.push_back(selectableMode);
      const std::string label = GetLabelForStereoMode(static_cast<RenderStereoMode>(i));
      pDlgSelect->Add(label);
      if (mode == selectableMode)
        pDlgSelect->SetSelected(label);
    }

    // inject AUTO pseudo mode after OFF
    if (i == static_cast<int>(RenderStereoMode::OFF))
    {
      selectableModes.push_back(RenderStereoMode::AUTO);
      pDlgSelect->Add(GetLabelForStereoMode(RenderStereoMode::AUTO));
    }
  }

  pDlgSelect->Open();

  int iItem = pDlgSelect->GetSelectedItem();
  if (iItem > -1 && pDlgSelect->IsConfirmed())
    mode = selectableModes[iItem];
  else
    mode = GetStereoMode();

  return mode;
}

RenderStereoMode CStereoscopicsManager::GetStereoModeOfPlayingVideo(void) const
{
  RenderStereoMode mode = RenderStereoMode::OFF;
  std::string playerMode = GetVideoStereoMode();

  if (!playerMode.empty())
  {
    auto convertedMode = ConvertVideoToGuiStereoMode(playerMode);
    if (convertedMode != RenderStereoMode::UNDEFINED)
      mode = convertedMode;
  }

  CLog::Log(LOGDEBUG, "StereoscopicsManager: autodetected stereo mode for movie mode {} is: {}",
            playerMode, ConvertGuiStereoModeToString(mode));
  return mode;
}

std::string CStereoscopicsManager::GetLabelForStereoMode(const RenderStereoMode mode) const
{
  int msgId;
  switch(mode) {
    case RenderStereoMode::AUTO:
      msgId = 36532;
      break;
    case RenderStereoMode::ANAGLYPH_YELLOW_BLUE:
      msgId = 36510;
      break;
    case RenderStereoMode::INTERLACED:
      msgId = 36507;
      break;
    case RenderStereoMode::CHECKERBOARD:
      msgId = 36511;
      break;
    case RenderStereoMode::HARDWAREBASED:
      msgId = 36508;
      break;
    case RenderStereoMode::MONO:
      msgId = 36509;
      break;
    default:
      msgId = 36502 + static_cast<int>(mode);
  }

  return CServiceBroker::GetResourcesComponent().GetLocalizeStrings().Get(msgId);
}

RenderStereoMode CStereoscopicsManager::GetPreferredPlaybackMode(void) const
{
  return static_cast<RenderStereoMode>(
      m_settings->GetInt(CSettings::SETTING_VIDEOSCREEN_PREFEREDSTEREOSCOPICMODE));
}

RenderStereoMode CStereoscopicsManager::ConvertVideoToGuiStereoMode(const std::string& mode)
{
  size_t i = 0;
  while (VideoModeToGuiModeMap[i].name)
  {
    if (mode == VideoModeToGuiModeMap[i].name)
      return VideoModeToGuiModeMap[i].mode;
    ++i;
  }
  return RenderStereoMode::UNDEFINED;
}

RenderStereoMode CStereoscopicsManager::ConvertStringToGuiStereoMode(const std::string& mode)
{
  size_t i = 0;
  while (StringToGuiModeMap[i].name)
  {
    if (mode == StringToGuiModeMap[i].name)
      return StringToGuiModeMap[i].mode;
    ++i;
  }
  return ConvertVideoToGuiStereoMode(mode);
}

const char* CStereoscopicsManager::ConvertGuiStereoModeToString(const RenderStereoMode mode)
{
  size_t i = 0;
  while (StringToGuiModeMap[i].name)
  {
    if (StringToGuiModeMap[i].mode == mode)
      return StringToGuiModeMap[i].name;
    ++i;
  }
  return "";
}

std::string CStereoscopicsManager::NormalizeStereoMode(const std::string& mode)
{
  if (!mode.empty() && mode != "mono")
  {
    auto guiMode = ConvertStringToGuiStereoMode(mode);

    if (guiMode != RenderStereoMode::UNDEFINED)
      return ConvertGuiStereoModeToString((RenderStereoMode)guiMode);
    else
      return mode;
  }

  return "mono";
}

CAction CStereoscopicsManager::ConvertActionCommandToAction(const std::string &command, const std::string &parameter)
{
  std::string cmd = command;
  std::string para = parameter;
  StringUtils::ToLower(cmd);
  StringUtils::ToLower(para);
  if (cmd == "setstereomode")
  {
    int actionId = -1;
    if (para == "next")
      actionId = ACTION_STEREOMODE_NEXT;
    else if (para == "previous")
      actionId = ACTION_STEREOMODE_PREVIOUS;
    else if (para == "toggle")
      actionId = ACTION_STEREOMODE_TOGGLE;
    else if (para == "select")
      actionId = ACTION_STEREOMODE_SELECT;
    else if (para == "tomono")
      actionId = ACTION_STEREOMODE_TOMONO;

    // already have a valid actionID return it
    if (actionId > -1)
      return CAction(actionId);

    // still no valid action ID, check if parameter is a supported stereomode
    if (ConvertStringToGuiStereoMode(para) != RenderStereoMode::UNDEFINED)
      return CAction(ACTION_STEREOMODE_SET, para);
  }
  return CAction(ACTION_NONE);
}

void CStereoscopicsManager::OnSettingChanged(const std::shared_ptr<const CSetting>& setting)
{
  if (setting == NULL)
    return;

  const std::string &settingId = setting->GetId();

  if (settingId == CSettings::SETTING_VIDEOSCREEN_STEREOSCOPICMODE)
  {
    RenderStereoMode mode = GetStereoMode();
    CLog::Log(LOGDEBUG, "StereoscopicsManager: stereo mode setting changed to {}",
              ConvertGuiStereoModeToString(mode));
    ApplyStereoMode(mode);
  }
}

bool CStereoscopicsManager::OnMessage(CGUIMessage &message)
{
  switch (message.GetMessage())
  {
  case GUI_MSG_PLAYBACK_STARTED:
    // A new item has nothing settled about it. Not OnPlaybackStopped(): the stereo mode is
    // deliberately kept from one item of a playlist to the next.
    m_stereoModeSettled = false;
    break;
  case GUI_MSG_PLAYBACK_STOPPED:
  case GUI_MSG_PLAYLISTPLAYER_STOPPED:
    OnPlaybackStopped();
    break;
  }

  return false;
}

bool CStereoscopicsManager::OnAction(const CAction &action)
{
  RenderStereoMode mode = GetStereoMode();

  if (action.GetID() == ACTION_STEREOMODE_NEXT)
  {
    SetStereoModeByUser(GetNextSupportedStereoMode(mode));
    return true;
  }
  else if (action.GetID() == ACTION_STEREOMODE_PREVIOUS)
  {
    SetStereoModeByUser(
        GetNextSupportedStereoMode(mode, static_cast<int>(RenderStereoMode::COUNT) - 1));
    return true;
  }
  else if (action.GetID() == ACTION_STEREOMODE_TOGGLE)
  {
    if (mode == RenderStereoMode::OFF)
    {
      RenderStereoMode targetMode = GetPreferredPlaybackMode();

      // if user selected a specific mode before, make sure to
      // switch back into that mode on toggle.
      if (m_stereoModeSetByUser != RenderStereoMode::UNDEFINED)
      {
        // if user mode is set to OFF, he manually turned it off before. In this case use the last user applied mode
        if (m_stereoModeSetByUser != RenderStereoMode::OFF)
          targetMode = m_stereoModeSetByUser;
        else if (m_lastStereoModeSetByUser != RenderStereoMode::UNDEFINED &&
                 m_lastStereoModeSetByUser != RenderStereoMode::OFF)
          targetMode = m_lastStereoModeSetByUser;
      }

      SetStereoModeByUser(targetMode);
    }
    else
    {
      SetStereoModeByUser(RenderStereoMode::OFF);
    }
    return true;
  }
  else if (action.GetID() == ACTION_STEREOMODE_SELECT)
  {
    SetStereoModeByUser(GetStereoModeByUserChoice());
    return true;
  }
  else if (action.GetID() == ACTION_STEREOMODE_TOMONO)
  {
    if (mode == RenderStereoMode::MONO)
    {
      RenderStereoMode targetMode = GetPreferredPlaybackMode();

      // if we have an old userdefined stereomode, use that one as toggle target
      if (m_stereoModeSetByUser != RenderStereoMode::UNDEFINED)
      {
        // if user mode is set to OFF, he manually turned it off before. In this case use the last user applied mode
        if (m_stereoModeSetByUser != RenderStereoMode::OFF && m_stereoModeSetByUser != mode)
          targetMode = m_stereoModeSetByUser;
        else if (m_lastStereoModeSetByUser != RenderStereoMode::UNDEFINED &&
                 m_lastStereoModeSetByUser != RenderStereoMode::OFF &&
                 m_lastStereoModeSetByUser != mode)
          targetMode = m_lastStereoModeSetByUser;
      }

      SetStereoModeByUser(targetMode);
    }
    else
    {
      SetStereoModeByUser(RenderStereoMode::MONO);
    }
    return true;
  }
  else if (action.GetID() == ACTION_STEREOMODE_SET)
  {
    auto stereoMode = ConvertStringToGuiStereoMode(action.GetName());
    if (stereoMode != RenderStereoMode::UNDEFINED)
      SetStereoModeByUser(stereoMode);
    return true;
  }

  return false;
}

RESOLUTION CStereoscopicsManager::GetResolutionForPlayingVideo(RenderStereoMode mode) const
{
  // Only where playback is allowed to change the display mode at all: with refresh rate
  // adjusting off it is the one picked by hand in videoscreen.resolution.
  if (m_settings->GetInt(CSettings::SETTING_VIDEOPLAYER_ADJUSTREFRESHRATE) ==
      ADJUST_REFRESHRATE_OFF)
    return RES_INVALID;

  const auto& components = CServiceBroker::GetAppComponents();
  const auto appPlayer = components.GetComponent<CApplicationPlayer>();
  if (!appPlayer || !appPlayer->IsPlaying())
    return RES_INVALID;

  VideoStreamInfo info;
  appPlayer->GetVideoStreamInfo(CURRENT_STREAM, info);
  if (!info.valid || info.fpsRate == 0 || info.fpsScale == 0)
    return RES_INVALID;

  const std::string stereoMode{CServiceBroker::GetDataCacheCore().GetVideoStereoMode()};
  const bool is3D{!stereoMode.empty() && stereoMode != "mono"};

  return CResolutionUtils::ChooseBestResolution(static_cast<float>(info.fpsRate) /
                                                    static_cast<float>(info.fpsScale),
                                                info.width, info.height, is3D, mode);
}

void CStereoscopicsManager::ApplyStereoMode(const RenderStereoMode mode, bool notify)
{
  RenderStereoMode currentMode = CServiceBroker::GetWinSystem()->GetGfxContext().GetStereoMode();
  CLog::Log(LOGDEBUG,
            "StereoscopicsManager::ApplyStereoMode: trying to apply stereo mode. Current: {} | "
            "Target: {}",
            ConvertGuiStereoModeToString(currentMode), ConvertGuiStereoModeToString(mode));
  if (currentMode != mode)
  {
    // The display mode the arrangement needs is chosen as the mode is applied, in
    // CGraphicContext::Flip() on the render thread, so that the display is reconfigured
    // once rather than twice: applying a stereo mode re-applies the display mode by itself,
    // and a mode change arriving a frame later would re-apply it again.
    CServiceBroker::GetWinSystem()->GetGfxContext().SetStereoMode(mode);
    CLog::Log(LOGDEBUG, "StereoscopicsManager: stereo mode changed to {}",
              ConvertGuiStereoModeToString(mode));
    {
      auto& components = CServiceBroker::GetAppComponents();
      const auto appPlayer = components.GetComponent<CApplicationPlayer>();
      if (appPlayer && appPlayer->IsPlaying())
      {
        // A hardware 3D display mode is only correct while the GUI is in the
        // matching split mode, so the mode has to be re-chosen for the new output
        // arrangement mid-playback too. Left in place for "Play as 2D" the sink
        // keeps signalling 3D, and a half 3D mode's doubled fPixelRatio is no
        // longer compensated for by GetResInfo() once the GUI leaves the split
        // mode, which squeezes the single eye into half the screen width. The mode it
        // settles on is the one just applied above, so this changes nothing further; it
        // is what keeps the display latency and the player's own parameters in step.
        appPlayer->TriggerUpdateResolution();
      }
      // With nothing playing there is no search to trigger, so run it here - but only
      // where playback would be allowed to: with refresh rate adjusting off the display
      // mode is the one picked by hand in videoscreen.resolution, and choosing another
      // would discard that.
      else if (appPlayer && m_settings->GetInt(CSettings::SETTING_VIDEOPLAYER_ADJUSTREFRESHRATE) !=
                                ADJUST_REFRESHRATE_OFF)
      {
        const RESOLUTION_INFO curInfo =
            CServiceBroker::GetWinSystem()->GetGfxContext().GetResInfo();
        const bool is3D = (mode != RenderStereoMode::OFF && mode != RenderStereoMode::MONO);
        RESOLUTION res = CResolutionUtils::ChooseBestResolution(
            curInfo.fRefreshRate, curInfo.iScreenWidth, curInfo.iScreenHeight, is3D);
        CServiceBroker::GetWinSystem()->GetGfxContext().SetVideoResolution(res, true);
      }
    }

    if (notify)
      CGUIDialogKaiToast::QueueNotification(
          CGUIDialogKaiToast::Info,
          CServiceBroker::GetResourcesComponent().GetLocalizeStrings().Get(36501),
          GetLabelForStereoMode(mode));
  }
}

std::string CStereoscopicsManager::GetVideoStereoMode() const
{
  std::string playerMode;

  const auto& components = CServiceBroker::GetAppComponents();
  const auto appPlayer = components.GetComponent<CApplicationPlayer>();
  if (appPlayer->IsPlaying())
    playerMode = CServiceBroker::GetDataCacheCore().GetVideoStereoMode();

  return playerMode;
}

bool CStereoscopicsManager::IsVideoStereoscopic() const
{
  std::string mode = GetVideoStereoMode();
  return !mode.empty() && mode != "mono";
}

void CStereoscopicsManager::OnStreamChange()
{
  UpdateStereoModeForStream();

  // The display mode a stereoscopic source needs follows from the stereo mode, so the
  // search for it is left until that mode is known - which is now. Where the decision
  // above changed the mode, applying it has already re-run the search; where it left the
  // mode as it was, nothing else will, and the source would keep whatever mode the last
  // one left behind.
  auto& components = CServiceBroker::GetAppComponents();
  const auto appPlayer = components.GetComponent<CApplicationPlayer>();
  // Not settled until the source is known to be stereoscopic: a stream change that
  // arrives before the player has said what it is playing has decided nothing, and
  // taking it for a decision leaves the display mode to be chosen against a stereo mode
  // that has not been applied yet.
  if (m_stereoModeSettled || !appPlayer || !appPlayer->IsPlaying() || !IsVideoStereoscopic())
    return;

  m_stereoModeSettled = true;

  // Where the decision above changed the stereo mode, applying it chose the display mode
  // to go with it; where it left the mode alone, nothing has, and a stereoscopic source
  // would keep whatever mode the title before it left behind. Ask for the search that was
  // left until now - once, since servicing it is announced as another stream change and
  // asking again on that would not end.
  appPlayer->TriggerUpdateResolution();
}

void CStereoscopicsManager::UpdateStereoModeForStream()
{
  STEREOSCOPIC_PLAYBACK_MODE playbackMode = static_cast<STEREOSCOPIC_PLAYBACK_MODE>(m_settings->GetInt(CSettings::SETTING_VIDEOPLAYER_STEREOSCOPICPLAYBACKMODE));
  RenderStereoMode mode = GetStereoMode();

  // early return if playback mode should be ignored and we're in no stereoscopic mode right now
  if (playbackMode == STEREOSCOPIC_PLAYBACK_MODE_IGNORE && mode == RenderStereoMode::OFF)
    return;

  if (!CStereoscopicsManager::IsVideoStereoscopic())
  {
    // exit stereo mode if started item is not stereoscopic
    // and if user prefers to stop 3D playback when movie is finished
    if (mode != RenderStereoMode::OFF &&
        m_settings->GetBool(CSettings::SETTING_VIDEOPLAYER_QUITSTEREOMODEONSTOP))
      SetStereoMode(RenderStereoMode::OFF);
    return;
  }

  // if we're not in stereomode yet, restore previously selected stereo mode in case it was user selected
  if (m_stereoModeSetByUser != RenderStereoMode::UNDEFINED)
  {
    SetStereoMode(m_stereoModeSetByUser);
    return;
  }

  RenderStereoMode preferred = GetPreferredPlaybackMode();
  RenderStereoMode playing = GetStereoModeOfPlayingVideo();

  if (mode != RenderStereoMode::OFF)
  {
    // don't change mode if user selected to not exit stereomode on playback stop
    // users selecting this option usually have to manually switch their TV into 3D mode
    // and would be annoyed by having to switch TV modes when next movies comes up
    // @todo probably add a new setting for just this behavior
    if (m_settings->GetBool(CSettings::SETTING_VIDEOPLAYER_QUITSTEREOMODEONSTOP) == false)
      return;

    // only change to new stereo mode if not yet in preferred stereo mode, comparing
    // against the arrangement the display can carry, since that is what would be applied.
    // A mode that suits the source but not the display - side by side where the display
    // offers only top and bottom, which is how a stream whose geometry was not yet known
    // ends up - is not a mode to settle for, and nothing else would revisit it.
    if (mode == ArrangementForDisplay(preferred) ||
        (preferred == RenderStereoMode::AUTO && mode == ArrangementForDisplay(playing)))
      return;
  }

  switch (playbackMode)
  {
  case STEREOSCOPIC_PLAYBACK_MODE_ASK: // Ask
    {
      CServiceBroker::GetAppMessenger()->SendMsg(TMSG_MEDIA_PAUSE);

      CGUIDialogSelect* pDlgSelect = CServiceBroker::GetGUI()->GetWindowManager().GetWindow<CGUIDialogSelect>(WINDOW_DIALOG_SELECT);
      pDlgSelect->Reset();
      pDlgSelect->SetHeading(
          CVariant{CServiceBroker::GetResourcesComponent().GetLocalizeStrings().Get(36527)});

      int idx_playing   = -1;

      // add choices
      int idx_preferred = pDlgSelect->Add(
          CServiceBroker::GetResourcesComponent().GetLocalizeStrings().Get(36524) // preferred
          + " (" + GetLabelForStereoMode(preferred) + ")");

      int idx_mono = pDlgSelect->Add(GetLabelForStereoMode(RenderStereoMode::MONO)); // mono / 2d

      if (playing != RenderStereoMode::OFF && playing != preferred &&
          preferred != RenderStereoMode::AUTO &&
          CServiceBroker::GetRenderSystem()->SupportsStereo(playing)) // same as movie
        idx_playing = pDlgSelect->Add(
            CServiceBroker::GetResourcesComponent().GetLocalizeStrings().Get(36532) + " (" +
            GetLabelForStereoMode(playing) + ")");

      int idx_select =
          pDlgSelect->Add(CServiceBroker::GetResourcesComponent().GetLocalizeStrings().Get(
              36531)); // other / select

      pDlgSelect->Open();

      if (pDlgSelect->IsConfirmed())
      {
        int iItem = pDlgSelect->GetSelectedItem();
        if      (iItem == idx_preferred) mode = preferred;
        else if (iItem == idx_mono)
          mode = RenderStereoMode::MONO;
        else if (iItem == idx_playing)
          mode = RenderStereoMode::AUTO;
        else if (iItem == idx_select)    mode = GetStereoModeByUserChoice();

        SetStereoModeByUser(mode);
      }

      CServiceBroker::GetAppMessenger()->SendMsg(TMSG_MEDIA_UNPAUSE);
    }
    break;
  case STEREOSCOPIC_PLAYBACK_MODE_PREFERRED: // Stereoscopic
    SetStereoMode(preferred);
    break;
  case 2: // Mono
    SetStereoMode(RenderStereoMode::MONO);
    break;
  default:
    break;
  }
}

void CStereoscopicsManager::OnPlaybackStopped(void)
{
  RenderStereoMode mode = GetStereoMode();

  if (m_settings->GetBool(CSettings::SETTING_VIDEOPLAYER_QUITSTEREOMODEONSTOP) &&
      mode != RenderStereoMode::OFF)
    SetStereoMode(RenderStereoMode::OFF);

  m_stereoModeSettled = false;

  // reset user modes on playback end to start over new on next playback and not end up in a probably unwanted mode
  if (m_stereoModeSetByUser != RenderStereoMode::OFF)
    m_lastStereoModeSetByUser = m_stereoModeSetByUser;

  m_stereoModeSetByUser = RenderStereoMode::UNDEFINED;
}
