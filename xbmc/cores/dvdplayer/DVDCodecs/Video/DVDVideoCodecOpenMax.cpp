/*
 *      Copyright (C) 2010-2013 Team XBMC
 *      http://xbmc.org
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
 *  along with XBMC; see the file COPYING.  If not, see
 *  <http://www.gnu.org/licenses/>.
 *
 */

#if (defined HAVE_CONFIG_H) && (!defined TARGET_WINDOWS)
  #include "config.h"
#elif defined(TARGET_WINDOWS)
#include "system.h"
#endif

#if defined(HAVE_LIBOPENMAX)
#include "DVDClock.h"
#include "DVDStreamInfo.h"
#include "DVDVideoCodecOpenMax.h"
#include "settings/Settings.h"
#include "utils/log.h"

#define CLASSNAME "CDVDVideoCodecOpenMax"
////////////////////////////////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////////////////////////////
CDVDVideoCodecOpenMax::CDVDVideoCodecOpenMax()
 : m_omx_decoder( new COpenMaxVideo )
{
  CLog::Log(LOGDEBUG, "%s::%s %p\n", CLASSNAME, __func__, this);
}

CDVDVideoCodecOpenMax::~CDVDVideoCodecOpenMax()
{
  CLog::Log(LOGDEBUG, "%s::%s %p\n", CLASSNAME, __func__, this);
  Dispose();
}

bool CDVDVideoCodecOpenMax::Open(CDVDStreamInfo &hints, CDVDCodecOptions &options)
{
  return m_omx_decoder->Open(hints, options, m_omx_decoder);
}

const char* CDVDVideoCodecOpenMax::GetName(void)
{
  return m_omx_decoder ? m_omx_decoder->GetName() : "omx-xxx";
}

void CDVDVideoCodecOpenMax::Dispose()
{
  m_omx_decoder->Dispose();
}

void CDVDVideoCodecOpenMax::SetDropState(bool bDrop)
{
  m_omx_decoder->SetDropState(bDrop);
}

int CDVDVideoCodecOpenMax::Decode(uint8_t* pData, int iSize, double dts, double pts)
{
  return m_omx_decoder->Decode(pData, iSize, dts, pts);
}

unsigned CDVDVideoCodecOpenMax::GetAllowedReferences()
{
  return m_omx_decoder->GetAllowedReferences();
}

void CDVDVideoCodecOpenMax::Reset(void)
{
  m_omx_decoder->Reset();
}

bool CDVDVideoCodecOpenMax::GetPicture(DVDVideoPicture* pDvdVideoPicture)
{
  return m_omx_decoder->GetPicture(pDvdVideoPicture);
}

bool CDVDVideoCodecOpenMax::ClearPicture(DVDVideoPicture* pDvdVideoPicture)
{
  return m_omx_decoder->ClearPicture(pDvdVideoPicture);
}

#endif
