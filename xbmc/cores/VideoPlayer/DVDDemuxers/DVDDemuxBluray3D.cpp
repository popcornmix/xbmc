/*
 *  Copyright (C) 2026 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#include "DVDDemuxBluray3D.h"

#include "DVDDemuxFFmpeg.h"
#include "DVDDemuxUtils.h"
#include "DVDInputStreams/DVDInputStreamBluray.h"
#include "DVDInputStreams/DVDInputStreamBlurayFile.h"
#include "cores/VideoPlayer/Interface/TimingConstants.h"
#include "utils/log.h"

#include <algorithm>
#include <cmath>
#include <mutex>

namespace
{
// Two access units belong together when their timestamps agree to within this much. Well
// below a frame interval, so neighbouring frames can never be confused, but loose enough
// to absorb rounding between the two demuxers.
constexpr double PTS_MATCH_TOLERANCE = DVD_MSEC_TO_TIME(10);

// After a seek the dependent view is placed this far before where it is wanted, so that it
// is always behind the base view and the matching loop can catch it up. Landing it too late
// would leave it permanently ahead, and the title would silently play in 2D.
constexpr double SEEK_BACK_OFF_MS = 3000.0;

// Largest gap the dependent view is allowed to skip forward over to reach the base view.
// A real gap is a few frames; anything beyond this is a bad timestamp rather than a stream
// position, and chasing it would run the dependent view to its end and leave the title in
// 2D for good. CDVDDemuxFFmpeg::ConvertTimestamp() produces exactly that on the first
// packet of a clip, which it hands over without subtracting the stream start time.
constexpr double MAX_CATCH_UP = DVD_MSEC_TO_TIME(10000);

// Base view access units allowed to go by unpaired before the dependent view is placed
// against the base view again. Bridging a clip change takes one or two; a run this long
// means the two are not going to meet on their own.
constexpr int MAX_UNMERGED = 12;

// How long, in base view time, to leave the dependent view alone after placing it again for
// want of a pair. Each placing costs a seek and a re-read of the clip.
constexpr double REALIGN_INTERVAL = DVD_SEC_TO_TIME(5);

/*!
 * \brief Length of a leading Annex-B access unit delimiter, or 0 if there is not one.
 *
 * Each view arrives as its own access unit, so the dependent view carries a delimiter of
 * its own. Left in place it would sit in the middle of the merged access unit and read as
 * the start of a new one.
 */
size_t LeadingAccessUnitDelimiter(const uint8_t* data, size_t size)
{
  size_t start{0};
  if (size >= 4 && data[0] == 0 && data[1] == 0 && data[2] == 0 && data[3] == 1)
    start = 4;
  else if (size >= 3 && data[0] == 0 && data[1] == 0 && data[2] == 1)
    start = 3;
  else
    return 0;

  // nal_unit_type 9 is the access unit delimiter
  if (start >= size || (data[start] & 0x1f) != 9)
    return 0;

  // Skip to the next start code, which is where the payload proper begins.
  for (size_t i = start + 1; i + 3 <= size; ++i)
  {
    if (data[i] == 0 && data[i + 1] == 0 && data[i + 2] == 1)
      return (i > 0 && data[i - 1] == 0) ? i - 1 : i;
  }

  return 0;
}
} // namespace

CDVDDemuxBluray3D::CDVDDemuxBluray3D() = default;

CDVDDemuxBluray3D::~CDVDDemuxBluray3D()
{
  if (m_pendingDependent)
    CDVDDemuxUtils::FreeDemuxPacket(m_pendingDependent);
}

bool CDVDDemuxBluray3D::Open(const std::shared_ptr<CDVDInputStream>& input)
{
  m_bluray = std::dynamic_pointer_cast<CDVDInputStreamBluray>(input);
  if (!m_bluray)
    return false;

  // An ffmpeg without H.264 MVC support would ignore the appended dependent view and
  // decode the base view alone - after the multiview flag has already cost the stream
  // its hardware decoder. Leave the title to the ordinary 2D path instead.
  if (!CDVDDemuxFFmpeg::SupportsMultiviewDecode(AV_CODEC_ID_H264))
  {
    CLog::LogF(LOGDEBUG, "no MVC decoder, playing the base view only");
    return false;
  }

  m_base = std::make_unique<CDVDDemuxFFmpeg>();
  if (!m_base->Open(input, false))
  {
    m_base.reset();
    return false;
  }

  if (!FindBaseVideoStream())
    return false;

  // A 3D disc plays plenty that is not 3D - idents, warnings, a menu - and in navigation
  // mode which of those is next is not known until libbluray has been read far enough to
  // choose it, which is what opening the base view above just did. So the dependent view
  // is looked for now and again whenever the streams change, and its absence is an
  // ordinary state rather than a reason to give up on the title.
  OpenDependentView();

  return true;
}

bool CDVDDemuxBluray3D::FindBaseVideoStream()
{
  m_baseVideoStreamId = -1;

  for (const CDemuxStream* stream : m_base->GetStreams())
  {
    if (stream && stream->type == StreamType::VIDEO)
    {
      m_baseVideoStreamId = stream->uniqueId;
      if (stream->dvdNavId == HDMV_PID_VIDEO)
        break;
    }
  }

  if (m_baseVideoStreamId < 0)
    CLog::LogF(LOGERROR, "no video stream in the base view");

  return m_baseVideoStreamId >= 0;
}

bool CDVDDemuxBluray3D::OpenDependentView()
{
  CloseDependentView();

  if (m_aborted)
    return false;

  unsigned int clip{0};
  std::string codec;
  if (!m_bluray->GetStereoscopicClip(clip, codec))
    return false;

  // A clip that will not open is not going to open on the next access unit either, and
  // finding that out again costs a read of it and a probe. Some of a 3D disc's play items
  // name a dependent view that holds nothing decodable - a menu's, on one retail disc -
  // and those play for as long as the viewer is in the menu, so remember it.
  m_unopenableClip = static_cast<int>(clip);

  m_dependentInput = m_bluray->OpenClipStream(clip, codec);
  if (!m_dependentInput)
    return false;

  m_dependentClip = static_cast<int>(clip);

  {
    std::unique_lock lock(m_dependentSection);
    m_dependent = std::make_unique<CDVDDemuxFFmpeg>();
  }

  if (!m_dependent->Open(m_dependentInput, false))
  {
    CLog::LogF(LOGERROR, "could not demux the dependent view clip");
    CloseDependentView();
    return false;
  }

  for (const CDemuxStream* stream : m_dependent->GetStreams())
  {
    if (stream && stream->type == StreamType::VIDEO)
    {
      m_dependentVideoStreamId = stream->uniqueId;
      if (stream->dvdNavId == HDMV_PID_VIDEO_SS)
        break;
    }
  }

  if (m_dependentVideoStreamId < 0)
  {
    CLog::LogF(LOGERROR, "no video stream in the dependent view clip");
    CloseDependentView();
    return false;
  }

  m_unopenableClip = -1;

  // The clip starts at its beginning and the play item need not, so place the dependent
  // view against the first base view timestamp rather than leave it to catch up.
  m_resyncPending = true;

  MarkBaseViewStereoscopic();
  CalculatePtsOffset();

  CLog::Log(LOGDEBUG, "CDVDDemuxBluray3D - playing clip {:05} as the dependent view", clip);

  return true;
}

void CDVDDemuxBluray3D::CloseDependentView()
{
  if (m_pendingDependent)
  {
    CDVDDemuxUtils::FreeDemuxPacket(m_pendingDependent);
    m_pendingDependent = nullptr;
  }

  {
    std::unique_lock lock(m_dependentSection);
    m_dependent.reset();
  }

  m_dependentInput.reset();
  m_dependentVideoStreamId = -1;
  m_dependentClip = -1;
  m_dependentEnded = false;
  m_ptsOffset = 0.0;
  m_unmergedBase = 0;
  m_lastRealignPts = DVD_NOPTS_VALUE;
}

void CDVDDemuxBluray3D::MarkBaseViewStereoscopic()
{
  auto* stream{dynamic_cast<CDemuxStreamVideo*>(m_base->GetStream(m_baseVideoStreamId))};
  if (!stream)
    return;

  // The base view is the left eye unless the playlist says the eyes are swapped.
  const bool baseViewIsRightEye{m_bluray->IsBaseViewRightEye()};
  stream->stereo_mode = baseViewIsRightEye ? "right_left" : "left_right";
  stream->multiview = true;

  CLog::Log(LOGDEBUG, "CDVDDemuxBluray3D - the playlist says the base view is the {} eye ({})",
            baseViewIsRightEye ? "right" : "left", stream->stereo_mode);
}

void CDVDDemuxBluray3D::CalculatePtsOffset()
{
  // Both clips give the same access unit the same timestamp, but each demuxer rebases on
  // the start time of its own stream before handing the packet over. Work out the constant
  // difference between the two, rather than trying to observe it, because after a seek
  // there is no known good pair to measure it from.
  //
  // This mirrors what CDVDDemuxFFmpeg::ConvertTimestamp() subtracts. The transport stream
  // branch there cannot apply to either of these: the base view is a Blu-ray, and the
  // dependent view is opened unparsed, both of which are excluded from it.
  double baseStart{0.0};
  if (m_bluray->GetSupportedMenuType() != MenuType::NATIVE && m_base->m_pFormatContext &&
      m_base->m_pFormatContext->start_time != static_cast<int64_t>(AV_NOPTS_VALUE))
  {
    baseStart = static_cast<double>(m_base->m_pFormatContext->start_time) / AV_TIME_BASE;
  }

  double dependentStart{0.0};
  if (m_dependent->m_pFormatContext &&
      m_dependent->m_pFormatContext->start_time != static_cast<int64_t>(AV_NOPTS_VALUE))
  {
    dependentStart = static_cast<double>(m_dependent->m_pFormatContext->start_time) / AV_TIME_BASE;
  }

  m_ptsOffset = (dependentStart - baseStart) * DVD_TIME_BASE;

  CLog::Log(LOGDEBUG, "CDVDDemuxBluray3D - dependent view timestamps offset by {}ms",
            DVD_TIME_TO_MSEC(m_ptsOffset));
}

DemuxPacket* CDVDDemuxBluray3D::ReadDependent()
{
  if (!m_dependent || m_dependentEnded)
    return nullptr;

  // Skip anything that is not the dependent view itself. The clip should hold nothing
  // else, but it costs little to be sure.
  while (true)
  {
    DemuxPacket* packet{m_dependent->Read()};
    if (!packet)
    {
      m_dependentEnded = true;
      return nullptr;
    }

    // Without a timestamp an access unit cannot be paired with anything, so there is no
    // point keeping it.
    if (packet->iStreamId == m_dependentVideoStreamId && packet->iSize > 0 &&
        packet->pts != DVD_NOPTS_VALUE)
    {
      return packet;
    }

    CDVDDemuxUtils::FreeDemuxPacket(packet);
  }
}

DemuxPacket* CDVDDemuxBluray3D::MergeViews(DemuxPacket* base, DemuxPacket* dependent)
{
  const size_t skip{LeadingAccessUnitDelimiter(dependent->pData, dependent->iSize)};

  // Grown in place, so that every other field of the base view packet goes through as it
  // is - including any DemuxPacket gains later.
  if (!CDVDDemuxUtils::AppendData(base, dependent->pData + skip,
                                  static_cast<int>(dependent->iSize - skip)))
    CLog::LogF(LOGERROR, "could not append the dependent view to the base view access unit");

  CDVDDemuxUtils::FreeDemuxPacket(dependent);

  return base;
}

DemuxPacket* CDVDDemuxBluray3D::Read()
{
  if (!m_base)
    return nullptr;

  DemuxPacket* packet{m_base->Read()};
  if (!packet)
    return nullptr;

  if (packet->iStreamId == DMX_SPECIALID_STREAMCHANGE)
  {
    // The stream list was rebuilt, so find the base view in it again and put the multiview
    // flag back if the play item still has a second eye. The play item may have changed as
    // well, and CheckDependentView() reopens the clip only if it has.
    FindBaseVideoStream();
    CheckDependentView();
    if (m_dependent)
      MarkBaseViewStereoscopic();
    return packet;
  }

  if (packet->iStreamId != m_baseVideoStreamId)
    return packet;

  CheckDependentView();

  if (!m_dependent || packet->pts == DVD_NOPTS_VALUE)
    return packet;

  if (m_resyncPending)
    AlignDependent(packet->pts);

  if (!m_pendingDependent)
    m_pendingDependent = ReadDependent();

  // Catch the dependent view up with the base view, dropping whatever it left behind. An
  // implausibly large gap means the base timestamp is wrong rather than the dependent view
  // being behind, so leave the dependent view where it is and let the next access unit sort
  // it out.
  while (m_pendingDependent)
  {
    const double gap{packet->pts - (m_pendingDependent->pts + m_ptsOffset)};
    if (gap <= PTS_MATCH_TOLERANCE || gap > MAX_CATCH_UP)
      break;

    CDVDDemuxUtils::FreeDemuxPacket(m_pendingDependent);
    m_pendingDependent = ReadDependent();
  }

  if (m_pendingDependent &&
      std::abs(m_pendingDependent->pts + m_ptsOffset - packet->pts) <= PTS_MATCH_TOLERANCE)
  {
    DemuxPacket* dependent{m_pendingDependent};
    m_pendingDependent = nullptr;
    m_unmergedBase = 0;

    return MergeViews(packet, dependent);
  }

  // The dependent view is ahead and the base view has yet to catch up, which never lasts
  // long in a title that is playing properly, so a run of unpaired access units means it has
  // lost its place - a clip joined without continuous timestamps, say. Put it back where the
  // base view is, but not more than once in a while, and not when it has simply run out:
  // placing it again then only re-reads the clip to its end.
  if (!m_dependentEnded && ++m_unmergedBase > MAX_UNMERGED &&
      (m_lastRealignPts == DVD_NOPTS_VALUE ||
       std::abs(packet->pts - m_lastRealignPts) > REALIGN_INTERVAL))
  {
    m_lastRealignPts = packet->pts;
    m_resyncPending = true;
  }

  return packet;
}

void CDVDDemuxBluray3D::CheckDependentView()
{
  unsigned int clip{0};
  std::string codec;
  if (!m_bluray->GetStereoscopicClip(clip, codec))
  {
    // The play item now being read has no second eye of its own.
    if (m_dependent)
      CloseDependentView();
    m_unopenableClip = -1;
    return;
  }

  if (m_dependent && static_cast<int>(clip) == m_dependentClip)
    return;

  if (static_cast<int>(clip) == m_unopenableClip)
    return;

  OpenDependentView();
}

void CDVDDemuxBluray3D::FlushDependent()
{
  if (m_pendingDependent)
  {
    CDVDDemuxUtils::FreeDemuxPacket(m_pendingDependent);
    m_pendingDependent = nullptr;
  }

  m_dependentEnded = false;

  if (m_dependent)
    m_dependent->Flush();

  // Where the base view has landed is not known until it hands over a packet - a seek moves
  // the play item as well as the position, and the clip the dependent view has to come from
  // moves with it. Place the dependent view once there is a base view timestamp to place it
  // against.
  m_resyncPending = true;
  m_lastRealignPts = DVD_NOPTS_VALUE;
  m_unopenableClip = -1;
}

void CDVDDemuxBluray3D::AlignDependent(double basePts)
{
  m_resyncPending = false;

  if (m_pendingDependent)
  {
    CDVDDemuxUtils::FreeDemuxPacket(m_pendingDependent);
    m_pendingDependent = nullptr;
  }

  m_dependentEnded = false;
  m_unmergedBase = 0;

  if (!m_dependent)
    return;

  m_dependent->Flush();

  // Convert the base view timestamp to the dependent view's own timeline, then land
  // deliberately early. Being behind is recoverable by dropping packets, being ahead is not.
  const double target{
      std::max(0.0, DVD_TIME_TO_MSEC(basePts - m_ptsOffset) - SEEK_BACK_OFF_MS)};

  CLog::Log(LOGDEBUG, "CDVDDemuxBluray3D - placing the dependent view at {}ms for a base view "
                      "at {}ms",
            static_cast<int64_t>(target), static_cast<int64_t>(DVD_TIME_TO_MSEC(basePts)));

  m_dependent->SeekTime(target, true, nullptr);
}

bool CDVDDemuxBluray3D::SeekTime(double time, bool backwards, double* startpts)
{
  if (!m_base)
    return false;

  if (!m_base->SeekTime(time, backwards, startpts))
    return false;

  FlushDependent();

  return true;
}

bool CDVDDemuxBluray3D::SeekChapter(int chapter, double* startpts)
{
  if (!m_base)
    return false;

  if (!m_base->SeekChapter(chapter, startpts))
    return false;

  FlushDependent();

  return true;
}

void CDVDDemuxBluray3D::Flush()
{
  if (m_base)
    m_base->Flush();

  FlushDependent();
}

bool CDVDDemuxBluray3D::Reset()
{
  if (!m_base)
    return false;

  if (!m_base->Reset())
    return false;

  // The base demuxer has been opened afresh and its stream list with it.
  FindBaseVideoStream();

  // A dependent view demuxer cannot be reset in place: resetting re-probes the clip from
  // wherever its input stream was left rather than from the start, which fails and leaves
  // the demuxer holding an unopened format context that crashes inside libavformat on the
  // next seek. Let it go and open the clip the play item names afresh - now, not on the next
  // base view access unit. The navigator resets the demuxer at every play item of a menu, and
  // on a still the probe that follows consumes the one video access unit there is, so a
  // reopen left to that packet never happens.
  CloseDependentView();
  FlushDependent();
  CheckDependentView();

  return true;
}

void CDVDDemuxBluray3D::Abort()
{
  m_aborted = true;

  if (m_base)
    m_base->Abort();

  std::unique_lock lock(m_dependentSection);
  if (m_dependent)
    m_dependent->Abort();
}

int CDVDDemuxBluray3D::GetChapterCount()
{
  return m_base ? m_base->GetChapterCount() : 0;
}

int CDVDDemuxBluray3D::GetChapter()
{
  return m_base ? m_base->GetChapter() : 0;
}

void CDVDDemuxBluray3D::GetChapterName(std::string& strChapterName, int chapterIdx)
{
  if (m_base)
    m_base->GetChapterName(strChapterName, chapterIdx);
}

std::chrono::milliseconds CDVDDemuxBluray3D::GetChapterPos(int chapterIdx)
{
  return m_base ? m_base->GetChapterPos(chapterIdx) : std::chrono::milliseconds(0);
}

void CDVDDemuxBluray3D::SetSpeed(int iSpeed)
{
  if (m_base)
    m_base->SetSpeed(iSpeed);
}

void CDVDDemuxBluray3D::FillBuffer(bool mode)
{
  if (m_base)
    m_base->FillBuffer(mode);
}

int CDVDDemuxBluray3D::GetStreamLength()
{
  return m_base ? m_base->GetStreamLength() : 0;
}

CDemuxStream* CDVDDemuxBluray3D::GetStream(int64_t demuxerId, int iStreamId) const
{
  // Through the base class, as the single argument override in CDVDDemuxFFmpeg hides this.
  const CDVDDemux* base{m_base.get()};
  return base ? base->GetStream(demuxerId, iStreamId) : nullptr;
}

CDemuxStream* CDVDDemuxBluray3D::GetStream(int iStreamId) const
{
  return m_base ? m_base->GetStream(iStreamId) : nullptr;
}

std::vector<CDemuxStream*> CDVDDemuxBluray3D::GetStreams() const
{
  return m_base ? m_base->GetStreams() : std::vector<CDemuxStream*>{};
}

int CDVDDemuxBluray3D::GetNrOfStreams() const
{
  return m_base ? m_base->GetNrOfStreams() : 0;
}

int CDVDDemuxBluray3D::GetPrograms(std::vector<ProgramInfo>& programs)
{
  return m_base ? m_base->GetPrograms(programs) : 0;
}

void CDVDDemuxBluray3D::SetProgram(int progId)
{
  if (m_base)
    m_base->SetProgram(progId);
}

std::string CDVDDemuxBluray3D::GetFileName()
{
  return m_base ? m_base->GetFileName() : std::string{};
}

std::string CDVDDemuxBluray3D::GetStreamCodecName(int64_t demuxerId, int iStreamId)
{
  CDVDDemux* base{m_base.get()};
  return base ? base->GetStreamCodecName(demuxerId, iStreamId) : std::string{};
}

std::string CDVDDemuxBluray3D::GetStreamCodecName(int iStreamId)
{
  return m_base ? m_base->GetStreamCodecName(iStreamId) : std::string{};
}

void CDVDDemuxBluray3D::EnableStream(int64_t demuxerId, int id, bool enable)
{
  if (m_base)
    m_base->EnableStream(demuxerId, id, enable);
}

void CDVDDemuxBluray3D::OpenStream(int64_t demuxerId, int id)
{
  if (m_base)
    m_base->OpenStream(demuxerId, id);
}

void CDVDDemuxBluray3D::SetVideoResolution(unsigned int width, unsigned int height)
{
  if (m_base)
    m_base->SetVideoResolution(width, height);
}
