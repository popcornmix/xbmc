/*
 *  Copyright (C) 2005-2018 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#pragma once

#include "guilib/GUIDialog.h"
#ifdef HAVE_DVBRIDGE
#include "utils/DVRenderUsage.h"
#endif

class CGUIDialogPlayerProcessInfo : public CGUIDialog
{
public:
  CGUIDialogPlayerProcessInfo(void);
  ~CGUIDialogPlayerProcessInfo(void) override;

  bool OnAction(const CAction &action) override;
  void OnInitWindow() override;
  void OnDeinitWindow(int nextWindowID) override;
  void Process(unsigned int currentTime, CDirtyRegionList& dirtyregions) override;

private:
#ifdef HAVE_DVBRIDGE
  void RefreshInfo();
  DVBRIDGE::RenderUsage m_renderUsage;
#endif
  unsigned int m_nextRefresh{0};
};
