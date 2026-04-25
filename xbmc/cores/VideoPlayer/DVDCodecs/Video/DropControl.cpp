/*
 *  Copyright (C) 2005-2018 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#include "DropControl.h"

#include "utils/log.h"

#include <cstdlib>

extern "C"
{
#include <libavutil/avutil.h>
}

CDropControl::CDropControl()
{
  Reset(true);
}

void CDropControl::Reset(bool init)
{
  m_lastPTS = AV_NOPTS_VALUE;

  if (init || m_state != VALID)
  {
    m_count = 0;
    m_diffPTS = 0;
    m_state = INIT;
  }
}

void CDropControl::Process(int64_t pts, bool drop)
{
  if (m_state == INIT)
  {
    if (pts != AV_NOPTS_VALUE && m_lastPTS != AV_NOPTS_VALUE)
    {
      m_diffPTS += pts - m_lastPTS;
      m_count++;
    }
    if (m_count > 10)
    {
      m_diffPTS = m_diffPTS / m_count;
      if (m_diffPTS > 0)
      {
        CLog::Log(LOGINFO, "CDropControl: calculated diff time: {}", m_diffPTS);
        m_state = CDropControl::VALID;
        m_count = 0;
      }
    }
  }
  else if (m_state == VALID && !drop)
  {
    if (std::abs(pts - m_lastPTS - m_diffPTS) > m_diffPTS * 0.2)
    {
      m_count++;
      if (m_count > 5)
      {
        CLog::Log(LOGINFO, "CDropControl: lost diff");
        Reset(true);
      }
    }
    else
      m_count = 0;
  }
  m_lastPTS = pts;
}
