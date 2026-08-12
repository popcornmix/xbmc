/*
 *  Copyright (C) 2005-2018 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#include "DVDFactoryDemuxer.h"

#include "DVDDemuxCDDA.h"
#include "DVDDemuxClient.h"
#include "DVDDemuxFFmpeg.h"
#include "DVDInputStreams/DVDInputStream.h"
#include "DemuxMultiSource.h"
#include "utils/URIUtils.h"
#include "utils/log.h"

#ifdef HAVE_LIBBLURAY
#include "DVDDemuxBluray3D.h"
#include "DVDInputStreams/DVDInputStreamBluray.h"
#include "ServiceBroker.h"
#include "guilib/StereoscopicsManager.h"
#include "settings/Settings.h"
#include "settings/SettingsComponent.h"
#endif

CDVDDemux* CDVDFactoryDemuxer::CreateDemuxer(const std::shared_ptr<CDVDInputStream>& pInputStream,
                                             bool fileinfo)
{
  if (!pInputStream)
    return NULL;

  // Try to open CDDA demuxer
  if (pInputStream->IsStreamType(DVDSTREAM_TYPE_FILE) && pInputStream->GetContent().compare("application/octet-stream") == 0)
  {
    std::string filename = pInputStream->GetFileName();
    if (filename.substr(0, 7) == "cdda://")
    {
      CLog::Log(LOGDEBUG, "DVDFactoryDemuxer: Stream is probably CD audio. Creating CDDA demuxer.");

      std::unique_ptr<CDVDDemuxCDDA> demuxer(new CDVDDemuxCDDA());
      if (demuxer->Open(pInputStream))
      {
        return demuxer.release();
      }
    }
  }

  // Input stream handles demuxing
  if (pInputStream->GetIDemux())
  {
    std::unique_ptr<CDVDDemuxClient> demuxer(new CDVDDemuxClient());
    if(demuxer->Open(pInputStream))
      return demuxer.release();
    else
      return nullptr;
  }

  // Try to open the MultiFiles demuxer
  if (pInputStream->IsStreamType(DVDSTREAM_TYPE_MULTIFILES))
  {
    std::unique_ptr<CDemuxMultiSource> demuxer(new CDemuxMultiSource());
    if (demuxer->Open(pInputStream))
      return demuxer.release();
    else
      return NULL;
  }

#ifdef HAVE_LIBBLURAY
  // A 3D Blu-ray keeps its second eye in a clip of its own, which needs demuxing alongside
  // the one libbluray plays. Not worth it when only gathering stream details, or when the
  // user has asked for 2D.
  if (!fileinfo && pInputStream->IsStreamType(DVDSTREAM_TYPE_BLURAY))
  {
    const auto bluray{std::dynamic_pointer_cast<CDVDInputStreamBluray>(pInputStream)};
    const int playbackMode{CServiceBroker::GetSettingsComponent()->GetSettings()->GetInt(
        CSettings::SETTING_VIDEOPLAYER_STEREOSCOPICPLAYBACKMODE)};

    if (bluray && bluray->IsStereoscopicDisc() && playbackMode != STEREOSCOPIC_PLAYBACK_MODE_MONO)
    {
      CLog::Log(LOGDEBUG, "DVDFactoryDemuxer: Stereoscopic Blu-ray. Creating 3D demuxer.");

      std::unique_ptr<CDVDDemuxBluray3D> demuxer(new CDVDDemuxBluray3D());
      if (demuxer->Open(pInputStream))
        return demuxer.release();

      // Fall through and play the base view on its own rather than failing outright.
      CLog::Log(LOGWARNING,
                "DVDFactoryDemuxer: could not open the dependent view, playing in 2D.");
    }
  }
#endif

  std::unique_ptr<CDVDDemuxFFmpeg> demuxer(new CDVDDemuxFFmpeg());
  if (demuxer->Open(pInputStream, fileinfo))
    return demuxer.release();
  else
    return NULL;
}

