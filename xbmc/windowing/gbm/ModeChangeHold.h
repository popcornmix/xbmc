/*
 *  Copyright (C) 2026 Hades
 *  This file is part of LibreELEC Hades. It is not in Kodi as Team Kodi publishes it.
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#pragma once

#include <cstdint>
#include <utility>

namespace KODI
{
namespace WINDOWING
{
namespace GBM
{

/*!
 * \brief Whether a change of the output waits for the next one, so that the display changes once
 *
 * Every commit that changes the mode, the HDR signalling or the depth of the HDMI link makes the
 * TV set itself up again: some seconds without a picture, and on some TVs a click of the sound.
 * When a video starts with the refresh rate adjusted to it, Kodi sets the mode of the video
 * first (VideoPlayer, before the decoder is open), and the output of the video comes after it
 * with its first picture: the HDR signalling of an HDR10 or HLG video (SetHDR()), or the
 * transport of native Dolby Vision, which change the link again. When such a video stops, the
 * output loses its HDR signalling first, and Kodi sets the mode of the GUI after it. That is two
 * changes of the display each time, a second or less apart.
 *
 * START: the change of mode for a video that opens is held for the commit that sets up its
 * output: the mode is chosen, and goes out with the first commit after the renderer of the video
 * is configured, or with the first frame of native Dolby Vision. STOP: the commit that takes the
 * HDR signalling away is held for the change to the mode of the GUI that follows. (Native Dolby
 * Vision that ends waits for that change of mode on its own, the same way:
 * CWinSystemGbmGLESContext::EndDVBridgeAfterVideo().) Meanwhile nothing is committed: the display
 * keeps the picture it has. Each hold has a deadline, after which what was held goes out alone,
 * as it did before: never a frozen output.
 *
 * Only where it can be done: an atomic output that is on, a mode of the same window size (a new
 * window makes a new surface), no 3D output, and a mode that is not the one on the screen. A
 * video that sets nothing up after its mode (an SDR video) is not held: its mode is the only
 * change.
 *
 * This class only decides.
 */
class CModeChangeHold
{
public:
  //! What the output of a video that opens will be, as its stream says
  enum class Video
  {
    NONE, //!< no video opens: nothing to wait for
    ORDINARY, //!< without HDR signalling (SDR)
    HDR, //!< with HDR signalling: HDR10, HLG, or Dolby Vision that is not sent natively
    NATIVE_DV, //!< native Dolby Vision, where a transport mode can carry it
  };

  enum class Kind
  {
    NONE,
    START, //!< the mode of a video that opens waits for the output of that video
    STOP, //!< the end of the HDR signalling of a video waits for the mode of the GUI
  };

  //! What ended a hold
  enum class End
  {
    NONE,
    CONFIGURED, //!< (START) the output of the video was set up: the next commit carries all
    MODE, //!< (STOP) the change of mode came: its commit carries all
    DEADLINE, //!< nothing came in time: what was held goes out alone
    CANCELLED, //!< something else changed the output first
  };

  //! The change that is about to be made, as the window system sees it
  struct Change
  {
    bool atomic = false; //!< atomic modesetting: only its commits carry a mode and properties
    bool active = false; //!< the output is meant to be on (not blanked)
    bool sameWindow = false; //!< the window keeps its size (a new one makes a new surface)
    bool mode3D = false; //!< the mode is a 3D output
    //! the 3D output has anything to do with this change: the eyes are drawn split, the output
    //! is in a 3D layout or was refused one, or the driver has a vendor InfoFrame from Kodi
    bool stereoBusy = false;
    bool captured = false; //!< native Dolby Vision is captured: it waits its own way
    bool timingChanges = false; //!< the mode is another one than the mode on the screen
  };

  static bool Possible(const Change& c)
  {
    return c.atomic && c.active && c.sameWindow && !c.mode3D && !c.stereoBusy && !c.captured &&
           c.timingChanges;
  }

  /*!
   * \brief Native Dolby Vision that ended is restored in the commit of the mode that Kodi sets
   *        next, instead of in a commit of its own before it (CWinSystemGbm::DVModeChangeJoins())
   *
   * With every backend: nvidia-drm, and the kernel backends of amdgpu and i915, which make a full
   * modeset for the end of the transport anyway. The window keeps its size and the mode is no 3D
   * output (3D is never shown while native output has the output).
   */
  static bool DVRestoreJoins(const Change& c)
  {
    return c.atomic && c.active && c.captured && c.sameWindow && !c.mode3D && c.timingChanges;
  }

  //! START: a change of mode made for a video that opens is held for its output
  static bool StartHolds(Video video, const Change& c)
  {
    return (video == Video::HDR || video == Video::NATIVE_DV) && Possible(c);
  }

  //! STOP: the output of a video that ended, while the screen has HDR signalling and a change of
  //! mode back to the GUI mode follows (the change described is that one), is held for it
  static bool StopHolds(bool hdrOnScreen, bool modeChangeFollows, const Change& c)
  {
    return hdrOnScreen && modeChangeFollows && Possible(c);
  }

  void Start(int64_t nowMs) { Begin(Kind::START, nowMs + START_MS); }
  void Stop(int64_t nowMs) { Begin(Kind::STOP, nowMs + STOP_MS); }

  Kind Holding() const { return m_kind; }

  //! A commit of the ordinary output would be made now: false while a hold lasts. At its deadline
  //! the hold ends (End::DEADLINE) and the commit goes.
  bool Commits(int64_t nowMs)
  {
    if (m_kind == Kind::NONE)
      return true;
    if (nowMs < m_until)
      return false;
    Finish(End::DEADLINE);
    return true;
  }

  //! The renderer of a video was configured (its output is set up): a START hold ends, and the
  //! next commit carries the mode with that output; a STOP hold too (a renderer made again
  //! within a video passes through the end of the one before: no change of mode follows)
  bool Configured()
  {
    if (m_kind == Kind::NONE)
      return false;
    Finish(m_kind == Kind::START ? End::CONFIGURED : End::CANCELLED);
    return true;
  }

  //! A change of mode is being made: a STOP hold ends with it (its commit carries what was held);
  //! a START hold is dropped, and the new change is decided on its own
  bool ModeChange()
  {
    if (m_kind == Kind::NONE)
      return false;
    Finish(m_kind == Kind::STOP ? End::MODE : End::CANCELLED);
    return true;
  }

  //! Something else commits the output (it is blanked, the window goes, native output is
  //! captured): the hold ends
  bool Cancel()
  {
    if (m_kind == Kind::NONE)
      return false;
    Finish(End::CANCELLED);
    return true;
  }

  //! How the last hold ended, and what it was, once
  std::pair<Kind, End> Take()
  {
    const auto ended = std::make_pair(m_endedKind, m_ended);
    m_endedKind = Kind::NONE;
    m_ended = End::NONE;
    return ended;
  }

  //! The time a START hold waits for the output of the video: its first picture, decoded and
  //! configured (well under a second, also for a 2160p stream)
  static constexpr int64_t START_MS = 3000;
  //! ... and a STOP hold for the change of mode (Kodi makes it within a second of the end of the
  //! video: 0.8 s in the tests of 2026-10-14), as native Dolby Vision that ends waits for it
  static constexpr int64_t STOP_MS = 2000;

private:
  void Begin(Kind kind, int64_t until)
  {
    if (m_kind != Kind::NONE)
      Finish(End::CANCELLED);
    m_kind = kind;
    m_until = until;
  }
  void Finish(End end)
  {
    m_endedKind = m_kind;
    m_ended = end;
    m_kind = Kind::NONE;
  }

  Kind m_kind = Kind::NONE;
  int64_t m_until = 0;
  Kind m_endedKind = Kind::NONE;
  End m_ended = End::NONE;
};

/*!
 * \brief When the other subsystems are told of a change of mode that was held (CModeChangeHold,
 *        START): around the commit that makes it, not when it was chosen
 *
 * A change of mode tells the display resources (VideoPlayer, the audio engine, the video clock,
 * the decoders) that the display is lost before its commit, and that it is back once that commit
 * is made (OnLostDevice(), then OnResetDisplay() after the delay that the user set). A change that
 * is held is chosen at once and committed later: told when it was chosen, the display was back for
 * them before it changed at all, and the change itself came unannounced (the audio engine, waiting
 * for the display in its error state, tried its sink before the change and again with the device
 * changes of the change). Told now before the first commit that carries it, of any kind (the first
 * frame of native output, or a commit of the ordinary output after the deadline): the display is
 * back after it. When that commit is a frame of native output that the driver refused, the mode
 * did not change: the commit that does change it later is not another frame of native output (a
 * frame is attempted every refresh, and is not told again) but the restoration of the ordinary
 * output, or a commit of it, which is told then.
 *
 * Only decides; the window system tells, and arms the reset, when Before() says so.
 */
class CHeldModeAnnouncement
{
public:
  enum class Step
  {
    NONE, //!< nothing to tell
    BEFORE_NEXT, //!< before the next commit, of any kind
    BEFORE_ORDINARY, //!< before the next commit that is not a frame of native output
    TOLD, //!< told for the commit that is being made
  };

  //! A change of mode was held: it is told with its commit
  void Held() { m_step = Step::BEFORE_NEXT; }
  //! Another change of mode is made, which tells of itself: this one is not told
  void Dropped() { m_step = Step::NONE; }
  //! A commit is about to be made (transport: a frame of native Dolby Vision output); true when
  //! the other subsystems are to be told now that the display is lost (and back after it)
  bool Before(bool transport)
  {
    if (m_step == Step::BEFORE_NEXT || (m_step == Step::BEFORE_ORDINARY && !transport))
    {
      m_step = Step::TOLD;
      return true;
    }
    return false;
  }
  //! ... it was made: accepted, or refused by the driver (only a frame of native output can be)
  void After(bool accepted, bool transport)
  {
    if (m_step == Step::TOLD)
      m_step = !accepted && transport ? Step::BEFORE_ORDINARY : Step::NONE;
  }
  Step Pending() const { return m_step; }

private:
  Step m_step = Step::NONE;
};

} // namespace GBM
} // namespace WINDOWING
} // namespace KODI
