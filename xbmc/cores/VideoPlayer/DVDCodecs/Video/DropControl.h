/*
 *  Copyright (C) 2005-2018 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#pragma once

extern "C"
{
#include <libavutil/avutil.h>
}

#include <cstdint>

class CDropControl
{
public:
  CDropControl();
  void Reset(bool init);
  void Process(int64_t pts, bool drop);

  /*!
   * \brief Whether \p pts belongs to a picture this has not been given yet.
   *
   * The views of a multiview access unit come out of the decoder as separate frames
   * sharing one pts. Everything here counts and measures pictures, so only the first
   * of them may be handed over; the rest would halve the measured interval and make
   * every other delta look like a discontinuity.
   */
  bool IsNewPicture(int64_t pts) const { return pts == AV_NOPTS_VALUE || pts != m_lastPTS; }

  int64_t m_lastPTS;
  int64_t m_diffPTS;
  int m_count;
  enum
  {
    INIT,
    VALID
  } m_state;
};
