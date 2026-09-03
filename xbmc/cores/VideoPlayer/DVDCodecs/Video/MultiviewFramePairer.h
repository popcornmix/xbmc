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
 * \brief Feed the two views of a multiview access unit to the two inputs of the filter graph
 *        that packs them side by side.
 *
 * A decoder asked for every view hands the views of an access unit back as separate frames
 * sharing a pts, each tagged with its view id. hstack pairs its inputs frame for frame and
 * emits nothing until both have arrived, so the graph must never be left with a frame on one
 * input and nothing coming on the other: the decoder tags base view frames with view 0
 * whether or not a dependent view follows, and when the second eye stops - a 2D play item
 * of a 3D disc, a dependent clip that would not open - frames queue behind the filter until
 * memory runs out.
 *
 * Whether a frame has a partner is not knowable until the next frame turns up, so each base
 * view frame is held back and the frame after it decides: a dependent view frame completes
 * the pair, anything else means the held frame is on its own and it goes to both inputs as
 * its own partner. One held frame always yields exactly one pair, so the eyes stay paired
 * across the transition either way. Costs one frame of latency in the filter stage.
 */
class CMultiviewFramePairer
{
public:
  ~CMultiviewFramePairer();

  /*!
   * \brief The graph inputs to feed. Drops any held frame, which belonged to the old graph.
   * \param base the buffer source for the view that goes in the left half
   * \param dependent the buffer source for the other view
   */
  void SetInputs(AVFilterContext* base, AVFilterContext* dependent);

  //! \brief Forget which view is the base view and drop any held frame, for a decoder reset.
  void Reset();

  /*!
   * \brief Route a decoded frame to its input. Takes the frame's reference, leaving it empty.
   * \param frame the decoded frame, left blank on return whether or not this succeeds
   * \param stereoMode overwritten with the packed layout the bitstream's own view
   *        information gives ("left_right" or "right_left") once it is known, otherwise
   *        left as it is
   * \return 0 or an AVERROR
   */
  int AddFrame(AVFrame* frame, std::string& stereoMode);

  //! \brief Feed a held frame to both inputs as its own partner. 0 when nothing is held.
  int FlushHeldFrame();

  bool HasHeldFrame() const { return m_held != nullptr; }

  //! \brief End the stream on both inputs, after flushing any held frame.
  int Drain();

private:
  //! \brief Feed a pair: \p base is freed, \p dependent is left empty.
  int FeedPair(AVFrame* base, AVFrame* dependent);

  AVFilterContext* m_base{nullptr};
  AVFilterContext* m_dependent{nullptr};

  //! View id of the view that goes in the left half, -1 until a tagged frame has been seen.
  int m_baseViewId{-1};

  //! Base view frame waiting to be told whether it has a partner.
  AVFrame* m_held{nullptr};
};
