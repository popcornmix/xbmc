/*
 *  Copyright (C) 2026 Team LibreELEC
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#include "DVSourceCapture.h"

#include "DVDStreamInfo.h"
#include "DVPlaybackPolicy.h"
#include "cores/VideoPlayer/DVDCodecs/Video/DVDVideoCodec.h"
#include "utils/DVBridgeDiagnosticState.h"
#include "utils/log.h"

#include "dvbridge_metadata_bounds.h"

extern "C"
{
#include <libavutil/dovi_meta.h>
#include <libavutil/frame.h>
}

using namespace DVBRIDGE;

void CSourceCapture::Open(const CDVDStreamInfo& hints)
{
  m_nativeOutput = RetainsDVSource(hints);
  m_owner.BeginStream();
  m_hasDisplayMetadata = false;
  m_hasLightMetadata = false;
}

void CSourceCapture::Seek()
{
  m_owner.Seek();
}

void CSourceCapture::Close()
{
  m_nativeOutput = false;
}

void CSourceCapture::Capture(VideoPicture* picture,
                             const AVFrame* frame,
                             const CDVDStreamInfo& hints,
                             uint64_t informationGeneration)
{
  if (!picture || !frame)
    return;

  // HDR10 static metadata latch, see CDVDVideoCodecFFmpeg::GetPictureCommon().
  if (av_frame_get_side_data(frame, AV_FRAME_DATA_MASTERING_DISPLAY_METADATA))
  {
    m_displayMetadata = picture->displayMetadata;
    m_hasDisplayMetadata = true;
  }
  else if (m_nativeOutput && m_hasDisplayMetadata)
  {
    picture->displayMetadata = m_displayMetadata;
    picture->hasDisplayMetadata = true;
  }
  if (av_frame_get_side_data(frame, AV_FRAME_DATA_CONTENT_LIGHT_LEVEL))
  {
    m_lightMetadata = picture->lightMetadata;
    m_hasLightMetadata = true;
  }
  else if (m_nativeOutput && m_hasLightMetadata)
  {
    picture->lightMetadata = m_lightMetadata;
    picture->hasLightMetadata = true;
  }

  picture->doviMetadata.Reset();
  picture->doviSourceBinding = {};
  if (picture->hdrType == StreamHdrType::HDR_TYPE_DOLBYVISION)
    picture->doviSourceBinding = m_owner.NextPicture();

  const auto& dovi = hints.dovi;
  picture->doviSourceInfo = {dovi.dv_profile,
                             dovi.dv_level,
                             dovi.dv_version_major,
                             dovi.dv_version_minor,
                             static_cast<unsigned>(picture->colorBits),
                             frame->format,
                             bool(dovi.bl_present_flag),
                             bool(dovi.el_present_flag),
                             bool(dovi.rpu_present_flag),
                             false,
                             picture->hasLightMetadata,
                             0.0,
                             0.0,
                             picture->hasLightMetadata ? picture->lightMetadata.MaxCLL : 0u,
                             picture->hasLightMetadata ? picture->lightMetadata.MaxFALL : 0u};
  auto& sourceInfo = picture->doviSourceInfo;
  const auto& mastering = picture->displayMetadata;
  sourceInfo.informationGeneration = informationGeneration;
  sourceInfo.compatibility = dovi.dv_bl_signal_compatibility_id;
  sourceInfo.mastering = picture->hasDisplayMetadata && mastering.has_luminance &&
                         mastering.max_luminance.den > 0 && mastering.min_luminance.den > 0;
  if (sourceInfo.mastering)
  {
    sourceInfo.maxLuma = av_q2d(mastering.max_luminance);
    sourceInfo.minLuma = av_q2d(mastering.min_luminance);
  }
  picture->doviSourcePts = frame->best_effort_timestamp;

  if (picture->hdrType == StreamHdrType::HDR_TYPE_DOLBYVISION)
  {
    const AVFrameSideData* sd = av_frame_get_side_data(frame, AV_FRAME_DATA_DOVI_METADATA);
    if (sd)
    {
      if (!picture->doviMetadata.Assign(sd->data, sd->size, picture->doviSourceBinding) ||
          !dvbridge_metadata_bounds(picture->doviMetadata.Data(), picture->doviMetadata.Size()))
      {
        picture->doviMetadata.Reset();
        static thread_local DiagnosticFailures failures;
        if (DiagnosticsEnabled() && failures.Record(DiagnosticNowMs(), "picture-metadata"))
          CLog::Log(LOGWARNING, "Unable to retain valid Dolby Vision picture metadata: failures={}",
                    failures.Count());
      }
      else
      {
        const auto* meta = reinterpret_cast<const AVDOVIMetadata*>(picture->doviMetadata.Data());
        const AVDOVIRpuDataHeader* hdr = av_dovi_get_header(meta);
        const AVDOVIDataMapping* mapping = av_dovi_get_mapping(meta);
        if (hdr && hdr->el_spatial_resampling_filter_flag == 1 && hdr->disable_residual_flag == 0)
        {
          picture->strDVELType = "MEL";
          for (int i = 0; i < 3; i++)
          {
            if (mapping && (mapping->nlq[i].nlq_offset != 0 ||
                            mapping->nlq[i].vdr_in_max != 8388608 ||
                            mapping->nlq[i].linear_deadzone_slope != 0 ||
                            mapping->nlq[i].linear_deadzone_threshold != 0))
            {
              picture->strDVELType = "FEL";
              break;
            }
          }
        }
      }
    }
  }
#ifdef CB1_HAS_HDR10_AI
  else if (picture->hdrType == StreamHdrType::HDR_TYPE_HDR10 && !sourceInfo.profile &&
           picture->doviSourcePts != AV_NOPTS_VALUE)
  {
    // HDR10 sources carry a binding for CB1's HDR10-to-DV modes.
    picture->doviSourceBinding = m_owner.NextPicture();
  }
#endif
}
