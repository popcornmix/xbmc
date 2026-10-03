/*
 *  Copyright (C) 2005-2018 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#pragma once

#include <cstdint>

class CDropControl
{
public:
  CDropControl();
  void Reset(bool init);
  void Process(int64_t pts, bool drop);

  int64_t m_lastPTS;
  int64_t m_diffPTS;
  int m_count;
  enum
  {
    INIT,
    VALID
  } m_state;
};
