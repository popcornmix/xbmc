/*
 *  Copyright (C) 2005-2018 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#pragma once

#include "rendering/RenderSystemTypes.h"

#include <stdint.h>
#include <string>

typedef int DisplayMode;
#define DM_WINDOWED     -1
#define DM_FULLSCREEN    0

enum RESOLUTION
{
  RES_INVALID        = -1,
  RES_WINDOW         = 15,
  RES_DESKTOP        = 16,          // Desktop resolution
  RES_CUSTOM         = 16 + 1,      // First additional resolution
};

struct OVERSCAN
{
  int left;
  int top;
  int right;
  int bottom;
public:
  OVERSCAN()
  {
    left = top = right = bottom = 0;
  }
  OVERSCAN(const OVERSCAN& os)
  {
    left = os.left; top = os.top;
    right = os.right; bottom = os.bottom;
  }
  OVERSCAN& operator=(const OVERSCAN&) = default;

  bool operator==(const OVERSCAN& other)
  {
    return left == other.left && right == other.right && top == other.top && bottom == other.bottom;
  }
  bool operator!=(const OVERSCAN& other)
  {
    return left != other.left || right != other.right || top != other.top || bottom != other.bottom;
  }
};

struct EdgeInsets
{
  float left = 0.0f;
  float top = 0.0f;
  float right = 0.0f;
  float bottom = 0.0f;

  EdgeInsets() = default;
  EdgeInsets(float l, float t, float r, float b);
};

//! @brief Provide info of a resolution
struct RESOLUTION_INFO
{
  //!< Screen overscan boundary
  OVERSCAN Overscan;

  //!< Edge insets to scale the GUI to prevent the display notch from hiding a part of the GUI
  EdgeInsets guiInsets;

  //!< Specify if it is a fullscreen resolution, otherwise windowed
  bool bFullScreen;

  //!< Width GUI resolution (pixels), may differ from the screen value if GUI resolution limit, 3D is set or in HiDPI screens
  int iWidth;

  //!< Height GUI resolution (pixels), may differ from the screen value if GUI resolution limit, 3D is set or in HiDPI screens
  int iHeight;

  //!< Number of pixels of padding between stereoscopic frames
  int iBlanking;

  //!< Screen width (logical width in pixels)
  int iScreenWidth;

  //!< Screen height (logical height in pixels)
  int iScreenHeight;

  //!< The vertical subtitle baseline position, may be changed by Video calibration
  int iSubtitles;

  //!< Properties of the resolution e.g. interlaced mode
  uint32_t dwFlags;

  //!< Pixel aspect ratio
  float fPixelRatio;

  //!< Refresh rate
  float fRefreshRate;

  //!< Resolution mode description
  std::string strMode;

  //!< Resolution output description
  std::string strOutput;

  //!< Resolution ID
  std::string strId;

public:
  RESOLUTION_INFO(int width = 1280, int height = 720, float aspect = 0, const std::string &mode = "");
  float DisplayRatio() const;

  /*!
   * \brief The stereoscopic layout the mode transmits, to append to a mode in a log
   *
   * " 3D sbs", " 3D tab" or " 3D fp", and empty for a 2D mode - the separator included so
   * that a 2D mode's line reads as it always did.
   *
   * strMode carries the geometry, the scan type and the refresh rate, none of which
   * distinguishes a half 3D mode from the 2D mode of the same timing - they print
   * identically - so a log naming only that cannot say whether 3D was signalled at all.
   * Frame packing is told apart from the half top-and-bottom layout it is presented as by
   * iBlanking, which the packed scanout needs and no other mode has.
   */
  std::string StereoLayoutTag() const;
};

class CResolutionUtils
{
public:
  static RESOLUTION ChooseBestResolution(float fps, int width, int height, bool is3D);

  /*!
   * \brief The stereoscopic output arrangement the display can present
   *
   * A split arrangement has to be one the sink can receive, because it is what the
   * sink is signalled: the renderer crops the correct eye either way, so the layout
   * the source happens to use says nothing about which of the two to output. Returns
   * the arrangement whose native 3D mode is the better fit for @p fps content of
   * @p width x @p height, preferring @p wanted when nothing beats it - including when
   * the display has no native 3D mode at all, which is a display the viewer switches
   * into 3D by hand.
   */
  static RenderStereoMode ChooseStereoArrangement(RenderStereoMode wanted,
                                                  float fps,
                                                  int width,
                                                  int height);
  static bool HasWhitelist();
  static void PrintWhitelist();

  /*!
   * \brief Get the max allowed screen resolution, if fullscreen
   * \param width [OUT] Max width resolution
   * \param height [OUT] Max height resolution
   */
  static void GetMaxAllowedScreenResolution(unsigned int& width, unsigned int& height);

protected:
  static void FindResolutionFromWhitelist(float fps, int width, int height, bool is3D, RESOLUTION &resolution);
  static bool FindResolutionFromOverride(float fps, int width, bool is3D, RESOLUTION &resolution, float& weight, bool fallback);
  static float RefreshWeight(float refresh, float fps);
};
