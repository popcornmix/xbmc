/*
 *  Copyright (C) 2026 Hades
 *  This file is part of LibreELEC Hades. It is not in Kodi as Team Kodi publishes it.
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#pragma once

namespace KODI
{
namespace WINDOWING
{
namespace GBM
{

/*!
 * \brief Whether the frames of native Dolby Vision output still reach the display
 *
 * A frame of native output that the kernel refuses is not shown, and counts towards the return to
 * the ordinary output (CWinSystemGbmGLESContext::DVBridgeFrameShown()). A frame whose commit is
 * taken, but whose flip does not come, was counted as shown: the commit returns all the same,
 * once the driver gives up waiting for the flip (the kernel's helpers, which amdgpu and i915 use,
 * after 10 s; nvidia-drm after 3 s, "Flip event timeout"). Such an output shows nothing new, and
 * Kodi waits that long for every frame: the GUI does not move, the sound goes on, and the player
 * finds no free picture buffer ("OutputPicture - timeout waiting for buffer"). This tells it.
 *
 * Only a frame that changes nothing but the picture counts: the first frame of a native output,
 * or any frame whose commit carries a change of mode or properties that Kodi set since, may take
 * seconds (the HDMI link is set up again). Of the frames that count, one that took longer than
 * STALL_MS stalled; two in a row, or one that took longer than GIVE_UP_MS (the 10 s of the
 * kernel's helpers), end the native output. A frame in time between them starts again.
 *
 * This class only decides.
 */
class CDVPresentStall
{
public:
  enum class Verdict
  {
    NONE, //!< in time, or a frame that does not count
    STALLED, //!< it stalled: told, the native output goes on
    GIVE_UP, //!< the native output ends: its frames do not reach the display
  };

  //! What a frame whose commit was taken that took this long (the presentation, ms) means.
  //! plain: the commit changed nothing but the picture (the output was carrying the transport,
  //! and Kodi had asked for nothing else since the frame before).
  Verdict Presented(bool plain, double ms)
  {
    if (!plain)
      return Verdict::NONE;
    if (!(ms >= STALL_MS))
    {
      m_stalls = 0;
      return Verdict::NONE;
    }
    ++m_stalls;
    if (ms >= GIVE_UP_MS || m_stalls >= STALLS)
    {
      m_stalls = 0;
      return Verdict::GIVE_UP;
    }
    return Verdict::STALLED;
  }

  //! Frames that stalled in a row, until the one that gave up
  unsigned int Stalls() const { return m_stalls; }

  //! A native output starts or ends: what was found is of another one
  void Reset() { m_stalls = 0; }

  //! More than the 3 s after which nvidia-drm gives up waiting for a flip ends nothing alone; well
  //! over any frame in time (a refresh is 16.7 to 41.7 ms; a frame is drawn in less than one)
  static constexpr double STALL_MS = 2000.0;
  //! Only a driver that waited for the flip with the 10 s of the kernel's helpers takes this long
  static constexpr double GIVE_UP_MS = 5000.0;
  static constexpr unsigned int STALLS = 2;

private:
  unsigned int m_stalls = 0;
};

} // namespace GBM
} // namespace WINDOWING
} // namespace KODI
