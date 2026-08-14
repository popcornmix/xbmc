/*
 *  Copyright (C) 2005-2026 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#include "Resolution.h"

#include "GraphicContext.h"
#include "ServiceBroker.h"
#include "settings/AdvancedSettings.h"
#include "settings/DisplaySettings.h"
#include "settings/Settings.h"
#include "settings/SettingsComponent.h"
#include "utils/MathUtils.h"
#include "utils/Variant.h"
#include "utils/log.h"
#include "windowing/WinSystem.h"

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <utility>
#include <vector>

namespace
{

const char* SETTING_VIDEOSCREEN_WHITELIST_PULLDOWN{"videoscreen.whitelistpulldown"};
const char* SETTING_VIDEOSCREEN_WHITELIST_DOUBLEREFRESHRATE{
    "videoscreen.whitelistdoublerefreshrate"};

constexpr uint32_t STEREO_FLAGS{D3DPRESENTFLAG_MODE3DSBS | D3DPRESENTFLAG_MODE3DTB};

//! @brief The 3D layout a display mode has to carry for a split arrangement.
uint32_t StereoFlagFor(RenderStereoMode arrangement)
{
  return arrangement == RenderStereoMode::SPLIT_VERTICAL ? D3DPRESENTFLAG_MODE3DSBS
                                                         : D3DPRESENTFLAG_MODE3DTB;
}

//! @brief Reduce a stereoscopic frame to the size of one eye, which is what a display
//!        mode has to fit.
//!
//! Matching the packed size picks the wrong mode: a 3840x1080 full side-by-side frame
//! matches a 3840x2160 mode (same width) rather than the 1920x1080 one that actually
//! displays it. A full packing is wider (side-by-side) or taller (top-and-bottom)
//! than any single frame can be, so halve that axis. A half packing keeps a normal
//! frame shape and already describes one eye, so it is left alone.
void PerEyeSize(int& width, int& height)
{
  if (width <= 0 || height <= 0)
    return;

  const float frameShape = static_cast<float>(width) / static_cast<float>(height);
  if (frameShape > STEREO_FULL_SBS_MIN_ASPECT)
    width /= 2;
  else if (frameShape < STEREO_FULL_TAB_MAX_ASPECT)
    height /= 2;
}

//! @brief A mode as the display reports it. Unlike GetResInfo() this does not OR in
//!        the current GUI stereo mode's 3D flag.
const RESOLUTION_INFO& NativeModeInfo(const CVariant& mode)
{
  const CDisplaySettings& displaySettings = CDisplaySettings::GetInstance();
  return displaySettings.GetResolutionInfo(displaySettings.GetResFromString(mode.asString()));
}

//! @brief The 2D timing a mode carries. A half 3D mode transmits one eye inside an
//!        ordinary frame, so its geometry is already the 2D one. Frame packing sends
//!        both eyes plus an active space gap, so divide those out - only a
//!        frame-packing mode has iBlanking set.
std::pair<int, int> BaseGeometry(const RESOLUTION_INFO& info)
{
  if (info.iBlanking > 0)
    return {info.iScreenWidth, (info.iScreenHeight - info.iBlanking) / 2};

  return {info.iScreenWidth, info.iScreenHeight};
}

//! @brief True when two modes are the same 2D timing, however each transports it.
bool SameTiming(const RESOLUTION_INFO& a, const RESOLUTION_INFO& b)
{
  return BaseGeometry(a) == BaseGeometry(b) &&
         MathUtils::FloatEquals(a.fRefreshRate, b.fRefreshRate, 0.01f) &&
         (a.dwFlags & D3DPRESENTFLAG_INTERLACED) == (b.dwFlags & D3DPRESENTFLAG_INTERLACED);
}

//! @brief The natively-3D modes stereoscopic output may switch to.
//!
//! A 3D variant is not a mode anyone would choose in its own right: it is the same
//! video timing as the 2D mode - 1920x1080p23.98, its top-and-bottom variant and its
//! frame-packed variant all being 74.176 MHz with the same timings - transported
//! differently, and which transport is wanted follows from the stereo arrangement
//! rather than from taste. So a 3D mode is a candidate when its 2D timing is one the
//! whitelist already permits, and nobody has to whitelist 1920x2205 to reach frame
//! packing. Whitelisting a 3D mode outright licenses it too, since a mode is the same
//! timing as itself.
//!
//! Measure the default whitelist's "don't downgrade below the current mode" floor
//! against @p width x @p height, the size of one eye, rather than against the mode in
//! use. Every HDMI 3D mode is 1080 or 720 line based, so a 4K desktop otherwise
//! excludes the lot of them and there is nothing left to be stereoscopic in.
//!
//! And where one eye is larger than every 3D mode the display has - HDMI defines no
//! 3D timing above 1080 lines, so any 4K stereoscopic source - keep the modes the
//! floor turned down rather than none. Not downgrading is a sharpness heuristic, and
//! it has nothing to say when the alternative is showing 3D content flat: the eye is
//! scaled down to a mode either way, by the sink after it is told to expect 3D, or by
//! the renderer before it is not.
std::vector<RESOLUTION> Find3DCandidates(const std::vector<CVariant>& indexList,
                                         bool noWhiteList,
                                         int width,
                                         int height,
                                         const RESOLUTION_INFO& curr)
{
  std::vector<RESOLUTION> allowed;
  CServiceBroker::GetWinSystem()->GetGfxContext().GetAllowedResolutions(allowed);

  std::vector<RESOLUTION> candidates;
  std::vector<RESOLUTION> belowFloor;

  for (const RESOLUTION res : allowed)
  {
    const RESOLUTION_INFO& info = CDisplaySettings::GetInstance().GetResolutionInfo(res);
    if (!(info.dwFlags & STEREO_FLAGS))
      continue;

    if (noWhiteList)
    {
      // Scan type has to match the GUI, as it does for a 2D candidate, and the half
      // refresh rates left out there are no more playable in 3D.
      if ((info.dwFlags & D3DPRESENTFLAG_INTERLACED) !=
              (curr.dwFlags & D3DPRESENTFLAG_INTERLACED) ||
          (info.fRefreshRate <= 30 && !MathUtils::FloatEquals(info.fRefreshRate, 24.0f, 0.1f)))
        continue;

      const auto [baseWidth, baseHeight] = BaseGeometry(info);
      if (baseWidth < width || baseHeight < height)
      {
        belowFloor.push_back(res);
        continue;
      }
    }
    else if (std::ranges::none_of(indexList, [&info](const CVariant& mode)
                                  { return SameTiming(NativeModeInfo(mode), info); }))
      continue;

    candidates.push_back(res);
  }

  if (candidates.empty() && !belowFloor.empty())
  {
    CLog::Log(LOGDEBUG,
              "[WHITELIST] No 3D mode is as large as one eye ({}x{}), considering the smaller "
              "ones rather than losing 3D",
              width, height);
    return belowFloor;
  }

  return candidates;
}

//! @brief Whether the output puts a single image on the whole screen, so that a hardware
//!        3D mode is the wrong shape for it.
//!
//! Takes the requested stereo mode rather than the current one: SetStereoMode() is
//! deferred to the next flip, so a search triggered by the change itself - which is
//! exactly when the display mode has to be re-chosen - would otherwise still see the
//! mode being left behind.
bool Wants2DOutput(bool is3D)
{
  const RenderStereoMode stereoMode =
      CServiceBroker::GetWinSystem()->GetGfxContext().GetNextStereoMode();

  return !is3D || (stereoMode != RenderStereoMode::SPLIT_VERTICAL &&
                   stereoMode != RenderStereoMode::SPLIT_HORIZONTAL);
}

//! @brief The refresh rates to try for a native 3D mode, best first: the content rate,
//!        then double it, then a 3:2 pulldown rate, then the rate the GUI is running at.
//!
//! Unlike the 2D searches these are not gated on the whitelist's double refresh rate and
//! pulldown settings. A 3D EDID carries far sparser rate coverage - a sink offering
//! side-by-side at 50, 59.94 and 60Hz and nothing below them is ordinary - so for 3D the
//! alternative to a multiple of the content rate is rarely the content rate itself, it is
//! no 3D mode at all. The last tier changes the refresh rate whatever those settings say
//! too, so gating the multiples would only ever turn down the better of the two: it left
//! 30fps content in a 50Hz mode, juddering 5:3, while a judder-free 60Hz side-by-side
//! mode went unexamined.
//!
//! Which makes the tiers a fixed list, so StereoTierName() can name them by index.
std::vector<float> StereoRefreshTiers(float fps)
{
  return {fps, fps * 2, fps * 2.5f,
          CServiceBroker::GetWinSystem()
              ->GetGfxContext()
              .GetResInfo(CDisplaySettings::GetInstance().GetCurrentResolution())
              .fRefreshRate};
}

//! @brief What matching at @p tier of StereoRefreshTiers() means, for the log - the tier
//!        a mode came from being the difference between a considered choice and a last
//!        resort, and not otherwise visible in the mode it settled on.
const char* StereoTierName(size_t tier)
{
  switch (tier)
  {
    case 0:
      return "the content refresh rate";
    case 1:
      return "double the content refresh rate";
    case 2:
      return "a 3:2 pulldown refresh rate";
    default:
      return "the desktop refresh rate";
  }
}

//! @brief How well the display's best mode of one 3D layout fits. Refresh tier ranks
//!        above size, since a rate the content does not divide into judders.
struct StereoMatch
{
  RESOLUTION resolution{RES_INVALID};
  size_t tier{0};
  int area{0};
};

//! @brief Whether frame packing is what a 3D mode should be picked for. Off exercises
//!        the half modes, which are otherwise unreachable on a display offering frame
//!        packing at every timing that has one.
bool PreferFramePacking()
{
  return CServiceBroker::GetSettingsComponent()
      ->GetAdvancedSettings()
      ->m_stereoscopicPreferFramePacking;
}

//! @brief True when @p a is the better of two modes: matching at an earlier refresh
//!        tier, or at the same tier being the size preferred - frame packing carrying a
//!        whole frame per eye, or a half mode if that is what is being exercised. Never
//!        true of a layout the display has no mode for, and never for a tie, so the
//!        arrangement already wanted keeps it.
bool BetterMatch(const StereoMatch& a, const StereoMatch& b)
{
  if (a.resolution == RES_INVALID)
    return false;

  if (b.resolution == RES_INVALID)
    return true;

  if (a.tier != b.tier)
    return a.tier < b.tier;

  return PreferFramePacking() ? a.area > b.area : a.area < b.area;
}

//! @brief The best candidate natively flagged @p stereoFlag: the earliest @p tiers
//!        entry any of them matches, and the largest at that tier, so frame packing
//!        (1920x2205) wins over a half-resolution 3D mode - unless the
//!        stereoscopicpreferframepacking advanced setting turns that around, which is
//!        the only way to reach a half mode on a display offering frame packing at
//!        every timing that has one.
StereoMatch FindNative3DMode(const std::vector<RESOLUTION>& candidates,
                             uint32_t stereoFlag,
                             const std::vector<float>& tiers)
{
  const bool preferFramePacking = PreferFramePacking();
  StereoMatch match;

  for (size_t tier = 0; tier < tiers.size(); tier++)
  {
    for (const RESOLUTION i : candidates)
    {
      const RESOLUTION_INFO& info = CDisplaySettings::GetInstance().GetResolutionInfo(i);
      if (!(info.dwFlags & stereoFlag))
        continue;

      if (!MathUtils::FloatEquals(info.fRefreshRate, tiers[tier], 0.01f))
        continue;

      const int area = info.iScreenWidth * info.iScreenHeight;
      if (match.resolution == RES_INVALID ||
          (preferFramePacking ? area > match.area : area < match.area))
      {
        match.resolution = i;
        match.tier = tier;
        match.area = area;
      }
    }

    if (match.resolution != RES_INVALID)
      break;
  }

  return match;
}

} // namespace

EdgeInsets::EdgeInsets(float l, float t, float r, float b) : left(l), top(t), right(r), bottom(b)
{
}

RESOLUTION_INFO::RESOLUTION_INFO(int width, int height, float aspect, const std::string &mode) :
  strMode(mode)
{
  iWidth = width;
  iHeight = height;
  iBlanking = 0;
  iScreenWidth = width;
  iScreenHeight = height;
  fPixelRatio = aspect ? ((float)width)/height / aspect : 1.0f;
  bFullScreen = true;
  fRefreshRate = 0;
  dwFlags = iSubtitles = 0;
}

float RESOLUTION_INFO::DisplayRatio() const
{
  return iWidth * fPixelRatio / iHeight;
}

std::string RESOLUTION_INFO::StereoLayoutTag() const
{
  if (dwFlags & D3DPRESENTFLAG_MODE3DSBS)
    return " 3D sbs";

  if (dwFlags & D3DPRESENTFLAG_MODE3DTB)
    return iBlanking > 0 ? " 3D fp" : " 3D tab";

  return "";
}

RESOLUTION CResolutionUtils::ChooseBestResolution(float fps, int width, int height, bool is3D)
{
  RESOLUTION res = CServiceBroker::GetWinSystem()->GetGfxContext().GetVideoResolution();
  float weight = 0.0f;

  // A stereoscopic source packs both eyes into one frame, but a display mode has
  // to fit one eye.
  if (is3D)
  {
    PerEyeSize(width, height);

    CLog::Log(LOGDEBUG, "[WHITELIST] Stereoscopic source, matching modes against one eye: {}x{}",
              width, height);
  }

  if (!FindResolutionFromOverride(fps, width, is3D, res, weight, false)) //find a refreshrate from overrides
  {
    if (!FindResolutionFromOverride(fps, width, is3D, res, weight, true)) //if that fails find it from a fallback
    {
      FindResolutionFromWhitelist(fps, width, height, is3D, res); //find a refreshrate from whitelist
    }
  }

  // Matching nothing leaves the mode that was already set, which after a 3D title is a
  // hardware 3D one. Keeping it for 2D output means the sink goes on splitting the frame
  // and showing half of it to each eye. Nothing about it was chosen for this content, so
  // rather than keep it, fall back to the desktop mode - the one mode known to be meant
  // for a single image, and what playing the same content from a cold start would have
  // left in place.
  if (Wants2DOutput(is3D) &&
      (CDisplaySettings::GetInstance().GetResolutionInfo(res).dwFlags & STEREO_FLAGS))
  {
    const RESOLUTION desktop = CDisplaySettings::GetInstance().GetCurrentResolution();

    if ((CDisplaySettings::GetInstance().GetResolutionInfo(desktop).dwFlags & STEREO_FLAGS) == 0)
    {
      CLog::Log(LOGDEBUG, "[WHITELIST] No 2D mode was chosen, falling back to the desktop mode");
      res = desktop;
    }
  }

  const RESOLUTION_INFO chosen = CServiceBroker::GetWinSystem()->GetGfxContext().GetResInfo(res);
  CLog::Log(LOGINFO, "Display resolution ADJUST : {}{} ({}) (weight: {:.3f})", chosen.strMode,
            chosen.StereoLayoutTag(), res, weight);
  return res;
}

RenderStereoMode CResolutionUtils::ChooseStereoArrangement(RenderStereoMode wanted,
                                                           float fps,
                                                           int width,
                                                           int height)
{
  if (wanted != RenderStereoMode::SPLIT_VERTICAL && wanted != RenderStereoMode::SPLIT_HORIZONTAL)
    return wanted;

  PerEyeSize(width, height);

  const std::vector<CVariant> indexList =
      CServiceBroker::GetSettingsComponent()->GetSettings()->GetList(
          CSettings::SETTING_VIDEOSCREEN_WHITELIST);
  const bool noWhiteList = indexList.empty();

  CGraphicContext& gfxContext = CServiceBroker::GetWinSystem()->GetGfxContext();
  const std::vector<RESOLUTION> candidates =
      Find3DCandidates(indexList, noWhiteList, width, height,
                       gfxContext.GetResInfo(gfxContext.GetVideoResolution()));
  const std::vector<float> tiers = StereoRefreshTiers(fps);

  const RenderStereoMode other = wanted == RenderStereoMode::SPLIT_VERTICAL
                                     ? RenderStereoMode::SPLIT_HORIZONTAL
                                     : RenderStereoMode::SPLIT_VERTICAL;

  const StereoMatch wantedMatch = FindNative3DMode(candidates, StereoFlagFor(wanted), tiers);
  const StereoMatch otherMatch = FindNative3DMode(candidates, StereoFlagFor(other), tiers);

  // Only change the arrangement for a better mode. Neither arrangement having one
  // leaves the choice alone, which keeps a display with no native 3D mode for this
  // content behaving as it does today: it is one the viewer switches into 3D by hand,
  // and which half of the frame each eye lives in is then theirs to decide.
  if (BetterMatch(otherMatch, wantedMatch))
  {
    const RESOLUTION_INFO& mode =
        CDisplaySettings::GetInstance().GetResolutionInfo(otherMatch.resolution);
    CLog::Log(LOGDEBUG,
              "Stereoscopic 3D: changing the output arrangement to suit display mode {}{} at {}",
              mode.strMode, mode.StereoLayoutTag(), StereoTierName(otherMatch.tier));
    return other;
  }

  return wanted;
}

void CResolutionUtils::FindResolutionFromWhitelist(float fps, int width, int height, bool is3D, RESOLUTION &resolution)
{
  RESOLUTION_INFO curr = CServiceBroker::GetWinSystem()->GetGfxContext().GetResInfo(resolution);
  CLog::Log(LOGINFO,
            "[WHITELIST] Searching the whitelist for: width: {}, height: {}, fps: {:0.3f}, 3D: {}",
            width, height, fps, is3D ? "true" : "false");

  // Stereoscopic output means the GUI splits the screen between the eyes:
  // SPLIT_VERTICAL is side-by-side, SPLIT_HORIZONTAL is top-and-bottom (which
  // covers frame packing). Every other mode puts a single image on the whole
  // screen and so needs a 2D display mode, is3D or not.
  //
  // "Play as 2D" (MONO) arrives here with is3D still true - the source is
  // stereoscopic, and the per-eye size derivation in ChooseBestResolution()
  // depends on that - but only one eye is shown, full screen, so a 3D mode is as
  // wrong as it is for a 2D source: the sink keeps splitting the frame, and a half
  // 3D mode's doubled fPixelRatio is no longer compensated for by GetResInfo()
  // once the GUI leaves the split mode, squeezing that eye to half the width.
  //
  // The requested mode, not the current one - see Wants2DOutput().
  const RenderStereoMode stereoMode =
      CServiceBroker::GetWinSystem()->GetGfxContext().GetNextStereoMode();
  const bool wants2D = Wants2DOutput(is3D);

  // Flags a candidate has to share with the mode it is compared against below. For
  // 2D output the 3D layout bits are dropped from both sides: GetResInfo() OR-s the
  // stereo mode still in effect onto every mode it returns, so which side carries a
  // bit natively - and whether the other has had it OR-ed in - depends on whether
  // the deferred mode change has flipped yet. Masking both makes the comparison
  // independent of that, and the natively-3D candidates are erased below anyway.
  const uint32_t modeMask =
      wants2D ? D3DPRESENTFLAG_MODEMASK & ~STEREO_FLAGS : D3DPRESENTFLAG_MODEMASK;

  // Building the default candidate list, ignore the 3D bits in both directions: a
  // mode is a candidate on its size and scan type, and which 3D bit it appears to
  // carry here says nothing about it - GetResInfo() decides that from the stereo
  // mode still in effect. Comparing them only ever ties by accident, and a mode
  // wrongly excluded is invisible to every search below. Which layout is actually
  // wanted is settled after the list is built: the natively-3D modes are erased for
  // 2D output, and FindNative3DMode() demands the flag for stereo output.
  constexpr uint32_t candidateMask = D3DPRESENTFLAG_MODEMASK & ~STEREO_FLAGS;

  // A natively-3D mode's geometry is no use as the "don't downgrade below the
  // current mode" floor of the default whitelist below once 2D output is wanted: a
  // frame-packing mode reports its packed scanout (1920x2205), which no 2D mode can
  // reach, so the list would come out empty. Measure against the desktop mode, the
  // 2D baseline, instead.
  if (wants2D &&
      (CDisplaySettings::GetInstance().GetResolutionInfo(resolution).dwFlags & STEREO_FLAGS))
  {
    const RESOLUTION_INFO desktop = CDisplaySettings::GetInstance().GetResolutionInfo(
        CDisplaySettings::GetInstance().GetCurrentResolution());
    curr.iScreenWidth = desktop.iScreenWidth;
    curr.iScreenHeight = desktop.iScreenHeight;
  }

  std::vector<CVariant> indexList = CServiceBroker::GetSettingsComponent()->GetSettings()->GetList(CSettings::SETTING_VIDEOSCREEN_WHITELIST);

  bool noWhiteList = indexList.empty();

  if (noWhiteList)
  {
    CLog::Log(LOGDEBUG,
              "[WHITELIST] Using the default whitelist because the user whitelist is empty");
    std::vector<RESOLUTION> candidates;
    RESOLUTION_INFO info;
    CServiceBroker::GetWinSystem()->GetGfxContext().GetAllowedResolutions(candidates);
    for (const auto& c : candidates)
    {
      info = CServiceBroker::GetWinSystem()->GetGfxContext().GetResInfo(c);
      if (info.iScreenHeight >= curr.iScreenHeight && info.iScreenWidth >= curr.iScreenWidth &&
          (info.dwFlags & candidateMask) == (curr.dwFlags & candidateMask))
      {
        // do not add half refreshrates (25, 29.97 by default) as kodi cannot cope with
        // them on playback start. Especially interlaced content is not properly detected
        // and this causes ugly double switching.
        // This won't allow 25p / 30p playback on native refreshrate by default
        if ((info.fRefreshRate > 30) || (MathUtils::FloatEquals(info.fRefreshRate, 24.0f, 0.1f)))
          indexList.push_back(CDisplaySettings::GetInstance().GetStringFromRes(c));
      }
    }
  }

  // A hardware 3D mode must never be used for 2D output. The MODEMASK filter above
  // and below cannot be relied on to exclude one, because GetResInfo() OR-s the
  // stereo mode still in effect onto every candidate.
  if (wants2D)
  {
    std::erase_if(indexList, [](const CVariant& mode)
                  { return (NativeModeInfo(mode).dwFlags & STEREO_FLAGS) != 0; });
  }

  // For stereoscopic output prefer a mode natively flagged with the matching 3D
  // layout, so the sink switches to 3D. The arrangement itself was settled before
  // this point, by ChooseStereoArrangement() against the layouts the display has;
  // here it is taken as given, and only the refresh rate and that layout matter,
  // never the video's pixel size - the renderer crops the correct eye whatever the
  // source layout is.
  if (!wants2D)
  {
    // Search the 3D modes the whitelisted timings permit rather than the whitelist
    // itself, which holds one entry per timing and so never names a 3D variant.
    const std::vector<RESOLUTION> candidates3D =
        Find3DCandidates(indexList, noWhiteList, width, height, curr);

    const StereoMatch match =
        FindNative3DMode(candidates3D, StereoFlagFor(stereoMode), StereoRefreshTiers(fps));

    if (match.resolution != RES_INVALID)
    {
      resolution = match.resolution;
      const RESOLUTION_INFO matched =
          CServiceBroker::GetWinSystem()->GetGfxContext().GetResInfo(resolution);
      CLog::Log(LOGDEBUG, "[WHITELIST] Matched a native 3D mode {}{} ({}) at {}", matched.strMode,
                matched.StereoLayoutTag(), resolution, StereoTierName(match.tier));
      return;
    }

    CLog::Log(LOGDEBUG, "[WHITELIST] No native 3D mode matched, continuing the standard search");
  }

  CLog::Log(LOGDEBUG, "[WHITELIST] Searching for an exact resolution with an exact refresh rate");

  unsigned int penalty = std::numeric_limits<unsigned int>::max();
  bool found = false;

  for (const auto& mode : indexList)
  {
    auto i = CDisplaySettings::GetInstance().GetResFromString(mode.asString());
    const RESOLUTION_INFO info = CServiceBroker::GetWinSystem()->GetGfxContext().GetResInfo(i);

    // allow resolutions that are exact and have the correct refresh rate
    // allow hardware decoder surface padding due to codec block alignment and GPU requirements
    // note: height has greater tolerance due to 32/64px boundaries e.g. 1080→1088 or 2160→2176
    if (((height == info.iScreenHeight && width <= info.iScreenWidth + 8) ||
         (width == info.iScreenWidth && height <= info.iScreenHeight + 32)) &&
        (info.dwFlags & modeMask) == (curr.dwFlags & modeMask) &&
        MathUtils::FloatEquals(info.fRefreshRate, fps, 0.01f))
    {
      CLog::Log(LOGDEBUG,
                "[WHITELIST] Matched an exact resolution with an exact refresh rate {} ({})",
                info.strMode, i);
      unsigned int pen = abs(info.iScreenHeight - height) + abs(info.iScreenWidth - width);
      if (pen < penalty)
      {
        resolution = i;
        found = true;
        penalty = pen;
      }
    }
  }

  if (!found)
    CLog::Log(LOGDEBUG, "[WHITELIST] No match for an exact resolution with an exact refresh rate");

  if (noWhiteList || CServiceBroker::GetSettingsComponent()->GetSettings()->GetBool(
          SETTING_VIDEOSCREEN_WHITELIST_DOUBLEREFRESHRATE))
  {
    CLog::Log(LOGDEBUG,
              "[WHITELIST] Searching for an exact resolution with double the refresh rate");

    for (const auto& mode : indexList)
    {
      auto i = CDisplaySettings::GetInstance().GetResFromString(mode.asString());
      const RESOLUTION_INFO info = CServiceBroker::GetWinSystem()->GetGfxContext().GetResInfo(i);

      // allow resolutions that are exact and have double the refresh rate
      // allow hardware decoder surface padding due to codec block alignment and GPU requirements
      // note: height has greater tolerance due to 32/64px boundaries e.g. 1080→1088 or 2160→2176
      if (((height == info.iScreenHeight && width <= info.iScreenWidth + 8) ||
           (width == info.iScreenWidth && height <= info.iScreenHeight + 32)) &&
          (info.dwFlags & modeMask) == (curr.dwFlags & modeMask) &&
          MathUtils::FloatEquals(info.fRefreshRate, fps * 2, 0.01f))
      {
        CLog::Log(LOGDEBUG,
                  "[WHITELIST] Matched an exact resolution with double the refresh rate {} ({})",
                  info.strMode, i);
        unsigned int pen = abs(info.iScreenHeight - height) + abs(info.iScreenWidth - width);
        if (pen < penalty)
        {
          resolution = i;
          found = true;
          penalty = pen;
        }
      }
    }
    if (found)
      return;

    CLog::Log(LOGDEBUG,
              "[WHITELIST] No match for an exact resolution with double the refresh rate");
  }
  else if (found)
    return;

  if (noWhiteList || CServiceBroker::GetSettingsComponent()->GetSettings()->GetBool(
          SETTING_VIDEOSCREEN_WHITELIST_PULLDOWN))
  {
    CLog::Log(LOGDEBUG,
              "[WHITELIST] Searching for an exact resolution with a 3:2 pulldown refresh rate");

    for (const auto& mode : indexList)
    {
      auto i = CDisplaySettings::GetInstance().GetResFromString(mode.asString());
      const RESOLUTION_INFO info = CServiceBroker::GetWinSystem()->GetGfxContext().GetResInfo(i);

      // allow resolutions that are exact and have 2.5 times the refresh rate
      // allow hardware decoder surface padding due to codec block alignment and GPU requirements
      // note: height has greater tolerance due to 32/64px boundaries e.g. 1080→1088 or 2160→2176
      if (((height == info.iScreenHeight && width <= info.iScreenWidth + 8) ||
           (width == info.iScreenWidth && height <= info.iScreenHeight + 32)) &&
          (info.dwFlags & modeMask) == (curr.dwFlags & modeMask) &&
          MathUtils::FloatEquals(info.fRefreshRate, fps * 2.5f, 0.01f))
      {
        CLog::Log(
            LOGDEBUG,
            "[WHITELIST] Matched an exact resolution with a 3:2 pulldown refresh rate {} ({})",
            info.strMode, i);
        unsigned int pen = abs(info.iScreenHeight - height) + abs(info.iScreenWidth - width);
        if (pen < penalty)
        {
          resolution = i;
          found = true;
          penalty = pen;
        }
      }
    }
    if (found)
      return;

    CLog::Log(LOGDEBUG, "[WHITELIST] No match for a resolution with a 3:2 pulldown refresh rate");
  }


  CLog::Log(LOGDEBUG, "[WHITELIST] Searching for a desktop resolution with an exact refresh rate");

  const RESOLUTION_INFO desktop_info = CServiceBroker::GetWinSystem()->GetGfxContext().GetResInfo(CDisplaySettings::GetInstance().GetCurrentResolution());

  for (const auto& mode : indexList)
  {
    auto i = CDisplaySettings::GetInstance().GetResFromString(mode.asString());
    const RESOLUTION_INFO info = CServiceBroker::GetWinSystem()->GetGfxContext().GetResInfo(i);

    // allow resolutions that are desktop resolution but have the correct refresh rate
    if (info.iScreenWidth == desktop_info.iScreenWidth &&
        (info.dwFlags & modeMask) == (desktop_info.dwFlags & modeMask) &&
        MathUtils::FloatEquals(info.fRefreshRate, fps, 0.01f))
    {
      CLog::Log(LOGDEBUG,
                "[WHITELIST] Matched a desktop resolution with an exact refresh rate {} ({})",
                info.strMode, i);
      resolution = i;
      return;
    }
  }

  CLog::Log(LOGDEBUG, "[WHITELIST] No match for a desktop resolution with an exact refresh rate");

  if (noWhiteList || CServiceBroker::GetSettingsComponent()->GetSettings()->GetBool(
          SETTING_VIDEOSCREEN_WHITELIST_DOUBLEREFRESHRATE))
  {
    CLog::Log(LOGDEBUG,
              "[WHITELIST] Searching for a desktop resolution with double the refresh rate");

    for (const auto& mode : indexList)
    {
      auto i = CDisplaySettings::GetInstance().GetResFromString(mode.asString());
      const RESOLUTION_INFO info = CServiceBroker::GetWinSystem()->GetGfxContext().GetResInfo(i);

      // allow resolutions that are desktop resolution but have double the refresh rate
      if (info.iScreenWidth == desktop_info.iScreenWidth &&
          (info.dwFlags & modeMask) == (desktop_info.dwFlags & modeMask) &&
          MathUtils::FloatEquals(info.fRefreshRate, fps * 2, 0.01f))
      {
        CLog::Log(LOGDEBUG,
                  "[WHITELIST] Matched a desktop resolution with double the refresh rate {} ({})",
                  info.strMode, i);
        resolution = i;
        return;
      }
    }

    CLog::Log(LOGDEBUG,
              "[WHITELIST] No match for a desktop resolution with double the refresh rate");
  }

  if (noWhiteList || CServiceBroker::GetSettingsComponent()->GetSettings()->GetBool(
          SETTING_VIDEOSCREEN_WHITELIST_PULLDOWN))
  {
    CLog::Log(LOGDEBUG,
              "[WHITELIST] Searching for a desktop resolution with a 3:2 pulldown refresh rate");

    for (const auto& mode : indexList)
    {
      auto i = CDisplaySettings::GetInstance().GetResFromString(mode.asString());
      const RESOLUTION_INFO info = CServiceBroker::GetWinSystem()->GetGfxContext().GetResInfo(i);

      // allow resolutions that are desktop resolution but have 2.5 times the refresh rate
      if (info.iScreenWidth == desktop_info.iScreenWidth &&
          (info.dwFlags & modeMask) == (desktop_info.dwFlags & modeMask) &&
          MathUtils::FloatEquals(info.fRefreshRate, fps * 2.5f, 0.01f))
      {
        CLog::Log(
            LOGDEBUG,
            "[WHITELIST] Matched a desktop resolution with a 3:2 pulldown refresh rate {} ({})",
            info.strMode, i);
        resolution = i;
        return;
      }
    }

    CLog::Log(LOGDEBUG,
              "[WHITELIST] No match for a desktop resolution with a 3:2 pulldown refresh rate");
  }

  CLog::Log(LOGDEBUG, "[WHITELIST] No resolution matched");
}

bool CResolutionUtils::FindResolutionFromOverride(float fps, int width, bool is3D, RESOLUTION &resolution, float& weight, bool fallback)
{
  RESOLUTION_INFO curr = CServiceBroker::GetWinSystem()->GetGfxContext().GetResInfo(resolution);

  //try to find a refreshrate from the override
  for (int i = 0; i < (int)CServiceBroker::GetSettingsComponent()->GetAdvancedSettings()->m_videoAdjustRefreshOverrides.size(); i++)
  {
    RefreshOverride& override = CServiceBroker::GetSettingsComponent()->GetAdvancedSettings()->m_videoAdjustRefreshOverrides[i];

    if (override.fallback != fallback)
      continue;

    //if we're checking for overrides, check if the fps matches
    if (!fallback && (fps < override.fpsmin || fps > override.fpsmax))
      continue;

    for (size_t j = (int)RES_DESKTOP; j < CDisplaySettings::GetInstance().ResolutionInfoSize(); j++)
    {
      RESOLUTION_INFO info = CServiceBroker::GetWinSystem()->GetGfxContext().GetResInfo((RESOLUTION)j);

      if (info.iScreenWidth  == curr.iScreenWidth &&
          info.iScreenHeight == curr.iScreenHeight &&
          (info.dwFlags & D3DPRESENTFLAG_MODEMASK) == (curr.dwFlags & D3DPRESENTFLAG_MODEMASK))
      {
        if (info.fRefreshRate <= override.refreshmax &&
            info.fRefreshRate >= override.refreshmin)
        {
          resolution = (RESOLUTION)j;

          if (fallback)
          {
            CLog::Log(
                LOGDEBUG,
                "Found Resolution {} ({}) from fallback (refreshmin:{:.3f} refreshmax:{:.3f})",
                info.strMode, resolution, override.refreshmin, override.refreshmax);
          }
          else
          {
            CLog::Log(LOGDEBUG,
                      "Found Resolution {} ({}) from override of fps {:.3f} (fpsmin:{:.3f} "
                      "fpsmax:{:.3f} refreshmin:{:.3f} refreshmax:{:.3f})",
                      info.strMode, resolution, fps, override.fpsmin, override.fpsmax,
                      override.refreshmin, override.refreshmax);
          }

          weight = RefreshWeight(info.fRefreshRate, fps);

          return true; //fps and refresh match with this override, use this resolution
        }
      }
    }
  }

  return false; //no override found
}

//distance of refresh to the closest multiple of fps (multiple is 1 or higher), as a multiplier of fps
float CResolutionUtils::RefreshWeight(float refresh, float fps)
{
  float div   = refresh / fps;
  int round = MathUtils::round_int(static_cast<double>(div));

  float weight = 0.0f;

  if (round < 1)
    weight = (fps - refresh) / fps;
  else
    weight = fabs(div / round - 1.0f);

  // punish higher refreshrates and prefer better matching
  // e.g. 30 fps content at 60 hz is better than
  // 30 fps at 120 hz - as we sometimes don't know if
  // the content is interlaced at the start, only
  // punish when refreshrate > 60 hz to not have to switch
  // twice for 30i content
  if (refresh > 60 && round > 1)
    weight += round / 10000.0f;

  return weight;
}

bool CResolutionUtils::HasWhitelist()
{
  std::vector<CVariant> indexList = CServiceBroker::GetSettingsComponent()->GetSettings()->GetList(CSettings::SETTING_VIDEOSCREEN_WHITELIST);
  return !indexList.empty();
}

void CResolutionUtils::PrintWhitelist()
{
  std::string modeStr;
  auto indexList = CServiceBroker::GetSettingsComponent()->GetSettings()->GetList(
      CSettings::SETTING_VIDEOSCREEN_WHITELIST);
  if (!indexList.empty())
  {
    for (const auto& mode : indexList)
    {
      auto i = CDisplaySettings::GetInstance().GetResFromString(mode.asString());
      const RESOLUTION_INFO info = CServiceBroker::GetWinSystem()->GetGfxContext().GetResInfo(i);
      modeStr.append("\n" + info.strMode);
    }

    CLog::Log(LOGDEBUG, "[WHITELIST] whitelisted modes:{}", modeStr);
  }
}

void CResolutionUtils::GetMaxAllowedScreenResolution(unsigned int& width, unsigned int& height)
{
  if (!CServiceBroker::GetWinSystem()->GetGfxContext().IsFullScreenRoot())
    return;

  std::vector<RESOLUTION_INFO> resList;

  auto indexList = CServiceBroker::GetSettingsComponent()->GetSettings()->GetList(
      CSettings::SETTING_VIDEOSCREEN_WHITELIST);

  unsigned int maxWidth{0};
  unsigned int maxHeight{0};

  if (!indexList.empty())
  {
    for (const auto& mode : indexList)
    {
      RESOLUTION res = CDisplaySettings::GetInstance().GetResFromString(mode.asString());
      RESOLUTION_INFO resInfo{CServiceBroker::GetWinSystem()->GetGfxContext().GetResInfo(res)};
      if (static_cast<unsigned int>(resInfo.iScreenWidth) > maxWidth &&
          static_cast<unsigned int>(resInfo.iScreenHeight) > maxHeight)
      {
        maxWidth = static_cast<unsigned int>(resInfo.iScreenWidth);
        maxHeight = static_cast<unsigned int>(resInfo.iScreenHeight);
      }
    }
  }
  else
  {
    std::vector<RESOLUTION> resList;
    CServiceBroker::GetWinSystem()->GetGfxContext().GetAllowedResolutions(resList);

    for (const auto& res : resList)
    {
      RESOLUTION_INFO resInfo{CServiceBroker::GetWinSystem()->GetGfxContext().GetResInfo(res)};
      if (static_cast<unsigned int>(resInfo.iScreenWidth) > maxWidth &&
          static_cast<unsigned int>(resInfo.iScreenHeight) > maxHeight)
      {
        maxWidth = static_cast<unsigned int>(resInfo.iScreenWidth);
        maxHeight = static_cast<unsigned int>(resInfo.iScreenHeight);
      }
    }
  }

  width = maxWidth;
  height = maxHeight;
}
