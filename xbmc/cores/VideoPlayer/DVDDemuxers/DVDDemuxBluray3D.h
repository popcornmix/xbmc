/*
 *  Copyright (C) 2026 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#pragma once

#include "DVDDemux.h"
#include "cores/VideoPlayer/Interface/TimingConstants.h"
#include "threads/CriticalSection.h"

#include <atomic>
#include <chrono>
#include <memory>
#include <string>
#include <vector>

class CDVDDemuxFFmpeg;
class CDVDInputStream;
class CDVDInputStreamBluray;

/*!
 * \brief Demuxer for a stereoscopic (MVC) Blu-ray title.
 *
 * A 3D Blu-ray codes the two eyes as two clips. The base view is the ordinary clip that
 * libbluray plays and that a 2D player sees; the MVC dependent view sits in a clip of its
 * own, referenced from the stereoscopic sub-path of the playlist.
 *
 * The H.264 decoder wants both views of an access unit in one packet, so this demuxer runs
 * a second demuxer over the dependent view clip and appends its access units to the
 * matching base view packets. Everything else - stream list, chapters, seeking, programs -
 * is the base demuxer's, so from the player's point of view this behaves like the plain
 * Blu-ray path with one extra flag set on the video stream.
 */
class CDVDDemuxBluray3D : public CDVDDemux
{
public:
  CDVDDemuxBluray3D();
  ~CDVDDemuxBluray3D() override;

  /*!
   * \brief Open the base view and, if the current play item has one, the dependent view.
   * \param input the Blu-ray input stream
   * \return true if the base view was opened
   */
  bool Open(const std::shared_ptr<CDVDInputStream>& input);

  bool Reset() override;
  void Abort() override;
  void Flush() override;
  DemuxPacket* Read() override;
  bool SeekTime(double time, bool backwards = false, double* startpts = nullptr) override;
  bool SeekChapter(int chapter, double* startpts = nullptr) override;
  int GetChapterCount() override;
  int GetChapter() override;
  void GetChapterName(std::string& strChapterName, int chapterIdx = -1) override;
  std::chrono::milliseconds GetChapterPos(int chapterIdx = -1) override;
  void SetSpeed(int iSpeed) override;
  void FillBuffer(bool mode) override;
  int GetStreamLength() override;
  CDemuxStream* GetStream(int64_t demuxerId, int iStreamId) const override;
  std::vector<CDemuxStream*> GetStreams() const override;
  int GetNrOfStreams() const override;
  int GetPrograms(std::vector<ProgramInfo>& programs) override;
  void SetProgram(int progId) override;
  std::string GetFileName() override;
  std::string GetStreamCodecName(int64_t demuxerId, int iStreamId) override;
  void EnableStream(int64_t demuxerId, int id, bool enable) override;
  void OpenStream(int64_t demuxerId, int id) override;
  void SetVideoResolution(unsigned int width, unsigned int height) override;

protected:
  CDemuxStream* GetStream(int iStreamId) const override;
  std::string GetStreamCodecName(int iStreamId) override;

private:
  /*!
   * \brief Open the dependent view of the current play item, replacing any previous one.
   * \return true if a dependent view was opened
   */
  bool OpenDependentView();

  //! \brief Let go of the dependent view and anything buffered from it.
  void CloseDependentView();

  //! \brief Find the base view's video stream in the base demuxer's stream list.
  bool FindBaseVideoStream();

  //! \brief Tell the decoder the base view stream carries a second view to unpack.
  void MarkBaseViewStereoscopic();

  //! \brief Work out the fixed difference between the two views' timestamps.
  void CalculatePtsOffset();

  //! \brief Note how long a frame lasts, which is what indexes the plane offsets.
  void CalculateFrameDuration();

  /*!
   * \brief Keep the plane offsets a dependent view access unit carries, if it has any.
   *
   * A "1 plane + offset" title places its subtitles in depth by shifting the plane, and
   * says by how much once per GOP, in the dependent view. \p pts is the base view's, being
   * the time the player will present the first of the frames described.
   */
  void ReadOffsetMetadata(const DemuxPacket& dependent, double pts);

  //! \brief Read the next access unit of the dependent view, or nullptr at its end.
  DemuxPacket* ReadDependent();

  /*!
   * \brief Open the dependent view of the play item now being read, if it is not already.
   *
   * A feature is usually spread over many play items, each with a dependent view clip of
   * its own, and moving between them changes nothing else the demuxer would notice.
   */
  void CheckDependentView();

  //! \brief Discard buffered dependent view data and place it again once the base view lands.
  void FlushDependent();

  //! \brief Seek the dependent view to just before the given base view timestamp.
  void AlignDependent(double basePts);

  /*!
   * \brief Append the dependent view access unit to the base view one.
   *
   * Frees the dependent packet and returns the base packet, grown in place.
   */
  DemuxPacket* MergeViews(DemuxPacket* base, DemuxPacket* dependent);

  std::shared_ptr<CDVDInputStreamBluray> m_bluray;

  //! Base view, over the Blu-ray input stream. Owns the stream list the player sees.
  std::unique_ptr<CDVDDemuxFFmpeg> m_base;

  //! Dependent view, over its own clip. Its streams are never exposed.
  std::unique_ptr<CDVDDemuxFFmpeg> m_dependent;
  std::shared_ptr<CDVDInputStream> m_dependentInput;

  //! Guards m_dependent against Abort(), which arrives from another thread while the demux
  //! thread may be replacing the dependent view as the play item changes.
  CCriticalSection m_dependentSection;

  //! Set by Abort() and never cleared, an aborted demuxer being on its way out; stops a
  //! dependent view being opened after it.
  std::atomic<bool> m_aborted{false};

  int m_baseVideoStreamId{-1};
  int m_dependentVideoStreamId{-1};

  //! Clip the dependent view is open on, or -1 when there is none.
  int m_dependentClip{-1};

  //! Clip that would not open, and so is not to be tried again, or -1 when there is none.
  int m_unopenableClip{-1};

  //! Base view access units handed over unmerged since the last pair.
  int m_unmergedBase{0};

  //! Base view time at which the dependent view was last placed again for want of a pair,
  //! or DVD_NOPTS_VALUE. Each placing costs a seek and a re-read, so they are rate limited.
  double m_lastRealignPts{DVD_NOPTS_VALUE};

  //! A frame's worth of presentation time, taken from the base view.
  double m_frameDuration{0.0};

  //! Whether the title has been said to carry plane offsets, which is said once.
  bool m_loggedOffsetMetadata{false};

  //! Dependent view packet held back because it is ahead of the base view.
  DemuxPacket* m_pendingDependent{nullptr};

  //! Added to a dependent view timestamp to compare it with a base view one. The two
  //! clips are timestamped alike, but each demuxer subtracts its own start time.
  double m_ptsOffset{0.0};

  bool m_dependentEnded{false};

  //! Whether the dependent view still has to be placed against the base view.
  bool m_resyncPending{false};
};
