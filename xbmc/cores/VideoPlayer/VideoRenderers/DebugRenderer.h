/*
 *  Copyright (C) 2005-2018 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#pragma once

#include "OverlayRenderer.h"
#include <string>
#include <vector>

#define DEBUG_OVERLAY_COUNT_MAX 6

class CDVDOverlayText;

class CDebugRenderer
{
public:
  CDebugRenderer();
  virtual ~CDebugRenderer();
  void SetInfo(std::vector<std::string> &infos);
  void Render(CRect &src, CRect &dst, CRect &view);
  void Flush();

protected:

  class CRenderer : public OVERLAY::CRenderer
  {
  public:
    CRenderer();
    void Render(int idx) override;
  };

  std::string m_strDebug[DEBUG_OVERLAY_COUNT_MAX];
  CDVDOverlayText *m_overlay[DEBUG_OVERLAY_COUNT_MAX];
  CRenderer m_overlayRenderer;
};