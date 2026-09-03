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

void CMultiviewFramePairer::SetInputs(AVFilterContext* base, AVFilterContext* dependent)
{
  av_frame_free(&m_held);
  m_base = base;
  m_dependent = dependent;
}

void CMultiviewFramePairer::Reset()
{
  av_frame_free(&m_held);
  m_baseViewId = -1;
}

int CMultiviewFramePairer::AddFrame(AVFrame* frame, std::string& stereoMode)
{
#if !FFMPEG_HAVE_MULTIVIEW
  return av_buffersrc_add_frame(m_base, frame);
#else
  const AVFrameSideData* sd = av_frame_get_side_data(frame, AV_FRAME_DATA_VIEW_ID);
  const bool tagged = sd && sd->size >= sizeof(int);
  const int viewId = tagged ? *reinterpret_cast<const int*>(sd->data) : -1;

  if (tagged && m_baseViewId < 0)
  {
    m_baseViewId = viewId;

    // The first view out of the decoder goes in the left half. If the bitstream says that
    // view is the right eye, tell the renderer the halves are swapped rather than reorder
    // the graph inputs.
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
      return 0;
    }

    AVFrame* held = m_held;
    m_held = nullptr;
    return FeedPair(held, frame);
  }

  // Whatever was held from last time has been overtaken, so it is never getting a partner.
  int ret = FlushHeldFrame();
  if (ret < 0)
  {
    av_frame_unref(frame);
    return ret;
  }

  m_held = av_frame_alloc();
  if (!m_held)
  {
    av_frame_unref(frame);
    return AVERROR(ENOMEM);
  }

  av_frame_move_ref(m_held, frame);
  return 0;
#endif
}

int CMultiviewFramePairer::FeedPair(AVFrame* base, AVFrame* dependent)
{
  int ret = av_buffersrc_add_frame(m_base, base);
  av_frame_free(&base);
  if (ret < 0)
  {
    av_frame_unref(dependent);
    return ret;
  }

  return av_buffersrc_add_frame(m_dependent, dependent);
}

int CMultiviewFramePairer::FlushHeldFrame()
{
  if (!m_held)
    return 0;

  AVFrame* held = m_held;
  m_held = nullptr;

  if (!m_base || !m_dependent)
  {
    av_frame_free(&held);
    return 0;
  }

  AVFrame* copy = av_frame_clone(held);

  int ret = av_buffersrc_add_frame(m_base, held);
  av_frame_free(&held);
  if (ret < 0)
  {
    av_frame_free(&copy);
    return ret;
  }

  if (!copy)
    return AVERROR(ENOMEM);

  ret = av_buffersrc_add_frame(m_dependent, copy);
  av_frame_free(&copy);
  return ret;
}

int CMultiviewFramePairer::Drain()
{
  int ret = FlushHeldFrame();
  if (ret < 0)
    return ret;

  if (!m_base || !m_dependent)
    return 0;

  // Both inputs have to see the end of the stream, or the sink never reports it.
  ret = av_buffersrc_add_frame(m_dependent, nullptr);
  if (ret < 0)
    return ret;

  return av_buffersrc_add_frame(m_base, nullptr);
}
