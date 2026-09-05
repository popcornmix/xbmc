/*
 *  Copyright (C) 2026 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#pragma once

#include <string>

extern "C"
{
#include <libavfilter/avfilter.h>
#include <libavutil/frame.h>
}

/*!
 * \brief Pair up the two views of a multiview access unit as they come out of the decoder.
 *
 * A decoder asked for every view hands the views of an access unit back as separate frames
 * sharing a pts, each tagged with its view id. Consumers want them a pair at a time - a
 * renderer taking an eye per buffer, or a filter graph packing them side by side, which
 * emits nothing until both of its inputs have a frame.
 *
 * Whether a frame has a partner is not knowable until the next frame turns up: the decoder
 * tags base view frames with view 0 whether or not a dependent view follows, and the second
 * eye can stop at any time - a 2D play item of a 3D disc, a dependent clip that would not
 * open. So each base view frame is held back and the frame after it decides: a dependent
 * view frame completes the pair, anything else means the held frame is on its own and it
 * becomes its own partner. One held frame always yields exactly one pair, so the eyes stay
 * paired across the transition either way. Costs one frame of latency.
 */
class CMultiviewFramePairer
{
public:
  ~CMultiviewFramePairer();

  //! \brief Forget which view is the base view and drop any held frame, for a decoder reset.
  void Reset();

  //! \brief Drop any held frame, for a decode chain that is being rebuilt under it.
  void DropHeldFrame();

  /*!
   * \brief Offer a decoded frame. Takes the frame's reference, leaving it empty.
   * \param frame the decoded frame, left blank on return whether or not this pairs
   * \param stereoMode overwritten with the layout the bitstream's own view information
   *        gives ("left_right" or "right_left") once it is known, otherwise left as it is
   * \param base receives the view that leads, when a pair completes
   * \param dependent receives the other view, when a pair completes
   * \return true when \p base and \p dependent hold a pair, which is then the caller's
   */
  bool AddFrame(AVFrame* frame, std::string& stereoMode, AVFrame* base, AVFrame* dependent);

  /*!
   * \brief Release a held frame as its own partner, for the end of a stream.
   * \return true when one was held, and \p base and \p dependent are the caller's
   */
  bool FlushHeldFrame(AVFrame* base, AVFrame* dependent);

private:
  //! View id of the view that leads, -1 until a tagged frame has been seen.
  int m_baseViewId{-1};

  //! Base view frame waiting to be told whether it has a partner.
  AVFrame* m_held{nullptr};
};

/*!
 * \brief Feed a completed pair to the two buffer sources of a graph that packs the views.
 * \param baseFrame the view for the graph's first input, left empty whatever happens
 * \param dependentFrame the view for its second input, left empty whatever happens
 * \return 0 or an AVERROR
 */
int FeedMultiviewPair(AVFilterContext* base,
                      AVFilterContext* dependent,
                      AVFrame* baseFrame,
                      AVFrame* dependentFrame);

//! \brief End the stream on both buffer sources, or the sink never reports it.
int DrainMultiviewInputs(AVFilterContext* base, AVFilterContext* dependent);
