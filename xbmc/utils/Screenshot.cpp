/*
 *  Copyright (C) 2005-2018 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#include "Screenshot.h"

#include "ServiceBroker.h"
#include "URL.h"
#include "Util.h"
#include "filesystem/File.h"
#include "jobs/JobManager.h"
#include "pictures/Picture.h"
#include "resources/LocalizeStrings.h"
#include "resources/ResourcesComponent.h"
#include "settings/SettingPath.h"
#include "settings/Settings.h"
#include "settings/SettingsComponent.h"
#include "settings/windows/GUIControlSettings.h"
#include "utils/StringUtils.h"
#include "utils/URIUtils.h"
#include "utils/log.h"
#include "windowing/WinSystem.h"

using namespace XFILE;

std::vector<std::function<std::unique_ptr<IScreenshotSurface>()>> CScreenShot::m_screenShotSurfaces;

void CScreenShot::Register(const std::function<std::unique_ptr<IScreenshotSurface>()>& createFunc)
{
  m_screenShotSurfaces.emplace_back(createFunc);
}

void CScreenShot::TakeScreenshot(const std::string& filename, bool sync)
{
  auto surface = m_screenShotSurfaces.back()();

  if (!surface)
  {
    CLog::Log(LOGERROR, "failed to create screenshot surface");
    return;
  }

  if (!surface->Capture())
  {
    CLog::Log(LOGERROR, "Screenshot {} failed", CURL::GetRedacted(filename));
    return;
  }

  surface->CaptureVideo(true);

  CLog::Log(LOGDEBUG, "Saving screenshot {}", CURL::GetRedacted(filename));

  unsigned int format = XB_FMT_A8R8G8B8;
  if (surface->GetBitDepth() > 8)
  {
    // 10-bit capture: buffer is already RGBA16 with correct alpha, no swap needed
    format = XB_FMT_RGBA16;
  }
  else
  {
    //set alpha byte to 0xFF
    for (int y = 0; y < surface->GetHeight(); y++)
    {
      unsigned char* alphaptr = surface->GetBuffer() - 1 + y * surface->GetStride();
      for (int x = 0; x < surface->GetWidth(); x++)
        *(alphaptr += 4) = 0xFF;
    }
  }

  //if sync is true, the png file needs to be completely written when this function returns
  if (sync)
  {
    if (!CPicture::CreateThumbnailFromSurface(surface->GetBuffer(), surface->GetWidth(), surface->GetHeight(), surface->GetStride(), filename, format))
      CLog::Log(LOGERROR, "Unable to write screenshot {}", CURL::GetRedacted(filename));

    surface->ReleaseBuffer();
  }
  else
  {
    //make sure the file exists to avoid concurrency issues
    XFILE::CFile file;
    if (file.OpenForWrite(filename))
      file.Close();
    else
      CLog::Log(LOGERROR, "Unable to create file {}", CURL::GetRedacted(filename));

    //write .png file asynchronous with CThumbnailWriter, prevents stalling of the render thread
    //buffer is deleted from CThumbnailWriter
    CThumbnailWriter* thumbnailwriter = new CThumbnailWriter(surface->GetBuffer(), surface->GetWidth(), surface->GetHeight(), surface->GetStride(), filename, format);
    CServiceBroker::GetJobManager()->AddJob(thumbnailwriter, nullptr);
  }
}

void CScreenShot::TakeScreenshot()
{
  std::shared_ptr<CSettingPath> screenshotSetting = std::static_pointer_cast<CSettingPath>(CServiceBroker::GetSettingsComponent()->GetSettings()->GetSetting(CSettings::SETTING_DEBUG_SCREENSHOTPATH));
  if (!screenshotSetting)
    return;

  std::string strDir = screenshotSetting->GetValue();
  if (strDir.empty())
  {
    if (!CGUIControlButtonSetting::GetPath(
            screenshotSetting, &CServiceBroker::GetResourcesComponent().GetLocalizeStrings()))
      return;

    strDir = screenshotSetting->GetValue();
  }

  URIUtils::RemoveSlashAtEnd(strDir);

  if (!strDir.empty())
  {
    // Compute colorspace+EOTF tag from current scanout state.
    // BT.709 + SDR is the default assumption; only deviations are tagged.
    // Tag goes BEFORE the index, e.g. screenshot_bt2020_pq_00000.png.
    std::string tag;
    using KODI::UTILS::Eotf;
    using KODI::UTILS::Colorimetry;
    if (auto* winSystem = CServiceBroker::GetWinSystem())
    {
      Colorimetry colorimetry = winSystem->GetColorimetry();
      Eotf eotf = winSystem->GetEotf();

      if (colorimetry == Colorimetry::BT2020_YCC ||
          colorimetry == Colorimetry::BT2020_RGB ||
          colorimetry == Colorimetry::BT2020_CYCC)
        tag += "_bt2020";

      if (eotf == Eotf::PQ)
        tag += "_pq";
      else if (eotf == Eotf::HLG)
        tag += "_hlg";
    }

    const std::string templateFile = tag.empty()
        ? "screenshot{:05}.png"
        : "screenshot" + tag + "_{:05}.png";

    std::string file =
        CUtil::GetNextFilename(URIUtils::AddFileToFolder(strDir, templateFile), 65535);

    if (!file.empty())
    {
      TakeScreenshot(file, false);
    }
    else
    {
      CLog::Log(LOGWARNING, "Too many screen shots or invalid folder");
    }
  }
}
