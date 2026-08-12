/*
 *  Copyright (C) 2017-2018 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#pragma once

enum class RenderStereoView
{
  OFF,
  LEFT,
  RIGHT,
};

enum class RenderStereoMode
{
  OFF,
  SPLIT_HORIZONTAL,
  SPLIT_VERTICAL,
  ANAGLYPH_RED_CYAN,
  ANAGLYPH_GREEN_MAGENTA,
  ANAGLYPH_YELLOW_BLUE,
  INTERLACED,
  CHECKERBOARD,
  HARDWAREBASED,
  MONO,
  COUNT,

  // Pseudo modes
  AUTO = 100,
  UNDEFINED = 999,
};

//! Frame aspect ratios beyond which a stereoscopic frame has to be a "full" packing - two
//! full-size eyes beside or above each other - rather than a "half" packing that squeezes
//! each eye into a normal-looking frame: wider than Cinemascope for side by side, taller
//! than 4:3 for top and bottom. Nothing else in the picture tells the two apart.
constexpr float STEREO_FULL_SBS_MIN_ASPECT{2.4f};
constexpr float STEREO_FULL_TAB_MAX_ASPECT{1.3f};
