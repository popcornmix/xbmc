#pragma once
/*
 *      Copyright (C) 2005-2015 Team Kodi
 *      http://kodi.tv
 *
 *  This Program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2, or (at your option)
 *  any later version.
 *
 *  This Program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with Kodi; see the file COPYING.  If not, see
 *  <http://www.gnu.org/licenses/>.
 *
 */

#include "xbmc_addon_dll.h"
#include "xbmc_scr_types.h"

extern "C"
{

  // Functions that your visualisation must implement
  void Start(KODI_HANDLE addonHandle);
  void Stop(KODI_HANDLE addonHandle);
  void Render(KODI_HANDLE addonHandle);

  // No more needed, stays here to prevent load faults as long function is needed by other types!
  void __declspec(dllexport) get_addon(void* ptr) { }
};
