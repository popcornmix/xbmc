/*
 *  Copyright (C) 2026 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#include "MultiviewFramePairer.h"

#include "cores/FFmpeg.h"
#include "utils/log.h"

extern "C"
{
#include <libavfilter/buffersrc.h>
#include <libavutil/error.h>
#include <libavutil/stereo3d.h>
}

CMultiviewFramePairer::~CMultiviewFramePairer()
{
  av_frame_free(&m_held);
}

void CMultiviewFramePairer::Reset()
{
  av_frame_free(&m_held);
  m_baseViewId = -1;
}

void CMultiviewFramePairer::DropHeldFrame()
{
  av_frame_free(&m_held);
}

bool CMultiviewFramePairer::AddFrame(AVFrame* frame,
                                     std::string& stereoMode,
                                     AVFrame* base,
                                     AVFrame* dependent)
{
  int viewId = -1;
  bool tagged = false;
#if FFMPEG_HAVE_MULTIVIEW
  const AVFrameSideData* sd = av_frame_get_side_data(frame, AV_FRAME_DATA_VIEW_ID);
  tagged = sd && sd->size >= sizeof(int);
  if (tagged)
    viewId = *reinterpret_cast<const int*>(sd->data);
#endif

  if (tagged && m_baseViewId < 0)
  {
    m_baseViewId = viewId;

    // The first view out of the decoder leads. If the bitstream says that view is the
    // right eye, tell the renderer the eyes are swapped rather than reorder the views.
    const AVFrameSideData* stereo = av_frame_get_side_data(frame, AV_FRAME_DATA_STEREO3D);
    if (stereo && stereo->size >= sizeof(AVStereo3D))
    {
      switch (reinterpret_cast<const AVStereo3D*>(stereo->data)->view)
      {
        case AV_STEREO3D_VIEW_LEFT:
          stereoMode = "left_right";
          break;
        case AV_STEREO3D_VIEW_RIGHT:
          stereoMode = "right_left";
          break;
        default:
          break;
      }
    }

    CLog::Log(LOGDEBUG, "CMultiviewFramePairer::{} - base view {}, stereo mode {}",
              __FUNCTION__, viewId, stereoMode);
  }

  if (tagged && viewId != m_baseViewId)
  {
    if (!m_held)
    {
      // A dependent view frame with no base view frame to go with it.
      av_frame_unref(frame);
      return false;
    }

    av_frame_move_ref(base, m_held);
    av_frame_free(&m_held);
    av_frame_move_ref(dependent, frame);
    return true;
  }

  // Whatever was held from last time has been overtaken, so it is never getting a partner.
  const bool orphan = FlushHeldFrame(base, dependent);

  m_held = av_frame_alloc();
  if (!m_held)
  {
    av_frame_unref(frame);
    return orphan;
  }

  av_frame_move_ref(m_held, frame);
  return orphan;
}

bool CMultiviewFramePairer::FlushHeldFrame(AVFrame* base, AVFrame* dependent)
{
  if (!m_held)
    return false;

  AVFrame* copy = av_frame_clone(m_held);
  if (!copy)
  {
    av_frame_free(&m_held);
    return false;
  }

  av_frame_move_ref(base, m_held);
  av_frame_free(&m_held);
  av_frame_move_ref(dependent, copy);
  av_frame_free(&copy);
  return true;
}

int FeedMultiviewPair(AVFilterContext* base,
                      AVFilterContext* dependent,
                      AVFrame* baseFrame,
                      AVFrame* dependentFrame)
{
  if (!base || !dependent)
  {
    av_frame_unref(baseFrame);
    av_frame_unref(dependentFrame);
    return 0;
  }

  int ret = av_buffersrc_add_frame(base, baseFrame);
  if (ret < 0)
  {
    av_frame_unref(baseFrame);
    av_frame_unref(dependentFrame);
    return ret;
  }

  return av_buffersrc_add_frame(dependent, dependentFrame);
}

int DrainMultiviewInputs(AVFilterContext* base, AVFilterContext* dependent)
{
  if (!base || !dependent)
    return 0;

  int ret = av_buffersrc_add_frame(dependent, nullptr);
  if (ret < 0)
    return ret;

  return av_buffersrc_add_frame(base, nullptr);
}
