/*
 *  Copyright (C) 2005-2018 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#pragma once

#include "DVDClock.h"
#include "DVDCodecs/Video/DVDVideoCodec.h"
#include "DVDMessageQueue.h"
#include "DVDOverlayContainer.h"
#include "DVDStreamInfo.h"
#include "IVideoPlayer.h"
#include "PTSTracker.h"
#include "cores/VideoPlayer/VideoRenderers/RenderManager.h"
#include "threads/SystemClock.h"
#include "threads/Thread.h"
#include "utils/BitstreamStats.h"

#include <atomic>

#define DROP_DROPPED 1
#define DROP_VERYLATE 2
#define DROP_BUFFER_LEVEL 4

class CDemuxStreamVideo;

class CDroppingStats
{
public:
  void Reset();
  void AddOutputDropGain(double pts, int frames);
  struct CGain
  {
    int frames;
    double pts;
  };
  std::deque<CGain> m_gain;
  int m_totalGain;
  double m_lastPts;
};

class CVideoPlayerVideo : public CThread, public IDVDStreamPlayerVideo
{
public:
  CVideoPlayerVideo(CDVDClock* pClock,
                    CDVDOverlayContainer* pOverlayContainer,
                    CDVDMessageQueue& parent,
                    CRenderManager& renderManager,
                    CProcessInfo& processInfo,
                    double messageQueueTimeSize);
  ~CVideoPlayerVideo() override;

  bool OpenStream(CDVDStreamInfo hint) override;
  void CloseStream(bool bWaitForBuffers) override;
  void Flush(bool sync) override;
  bool AcceptsData() const override;
  bool HasData() const override;
  bool IsInited() const override;
  void SendMessage(std::shared_ptr<CDVDMsg> pMsg, int priority = 0) override;
  void FlushMessages() override;

  void EnableSubtitle(bool bEnable) override { m_bRenderSubs = bEnable; }
  bool IsSubtitleEnabled() override { return m_bRenderSubs; }
  double GetSubtitleDelay() override { return m_iSubtitleDelay; }
  void SetSubtitleDelay(double delay) override { m_iSubtitleDelay = delay; }
  bool IsStalled() const override { return m_stalled; }
  bool IsRewindStalled() const override { return m_rewindStalled; }
  double GetCurrentPts() override;
  double GetOutputDelay() override; /* returns the expected delay, from that a packet is put in queue */
  std::string GetPlayerInfo() override;
  int GetVideoBitrate() override;
  void SetSpeed(int iSpeed) override;

  // classes
  CDVDOverlayContainer* m_pOverlayContainer;
  CDVDClock* m_pClock;

protected:

  enum EOutputState
  {
    OUTPUT_NORMAL,
    OUTPUT_ABORT,
    OUTPUT_DROPPED,
    OUTPUT_AGAIN
  };

  void OnExit() override;
  void Process() override;

  bool ProcessDecoderOutput(double &frametime, double &pts);
  void UpdatePlayerInfo();
  void SendMessageBack(const std::shared_ptr<CDVDMsg>& pMsg, int priority = 0);
  MsgQueueReturnCode GetMessage(std::shared_ptr<CDVDMsg>& pMsg,
                                std::chrono::milliseconds timeout,
                                int& priority);

  EOutputState OutputPicture(const VideoPicture* src);
  void ProcessOverlays(const VideoPicture* pSource, double pts);
  void OpenStream(CDVDStreamInfo& hint, std::unique_ptr<CDVDVideoCodec> codec);

  void ResetFrameRateCalc();
  void CalcFrameRate();
  int CalcDropRequirement(double pts);

  //! Total of every picture that failed to reach the renderer, what the osd shows as drop:
  int GetDroppedFrames() const
  {
    return m_iDroppedFramesDecoder + m_iSkippedPicturesDecoder + m_iDroppedFramesOutput;
  }

  //! Periodic snapshot of the frame loss counters to the log, cheap enough to call per packet
  void LogPlaybackStats();

  double m_iSubtitleDelay;

  int m_iLateFrames;
  int m_iDroppedRequest;

  int m_iDroppedFramesDecoder; //!< pictures the decoder discarded, as reported by the codec
  int m_iSkippedPicturesDecoder; //!< pictures lost to skipped post processing / deinterlacing
  int m_iDroppedFramesOutput; //!< pictures dropped by OutputPicture, renderer full or too late
  int m_iRenderLateFrames; //!< lateness in frames last read from the render manager

  XbmcThreads::EndTime<> m_statsLogTimer;
  bool m_statsLogged; //!< the first line is logged even with nothing lost, to show it is live
  int m_iLoggedDroppedFrames; //!< counter values at the previous log line, to log the deltas
  int m_iLoggedSkippedFrames;
  int m_iLoggedRepeatedFrames;

  double m_fFrameRate;       //framerate of the video currently playing
  double m_fStableFrameRate; //place to store calculated framerates
  int m_iFrameRateCount;     //how many calculated framerates we stored in m_fStableFrameRate
  bool m_bAllowDrop;         //we can't drop frames until we've calculated the framerate
  int m_iFrameRateErr;       //how many frames we couldn't calculate the framerate, we give up after a while
  int m_iFrameRateLength;    //how many seconds we should measure the framerate
                             //this is increased exponentially from CVideoPlayerVideo::CalcFrameRate()

  bool m_bFpsInvalid;        // needed to ignore fps (e.g. dvd stills)
  bool m_bRenderSubs;
  float m_fForcedAspectRatio;
  int m_speed;
  std::atomic_bool m_stalled = false;
  std::atomic_bool m_rewindStalled;
  bool m_paused;
  IDVDStreamPlayer::ESyncState m_syncState;
  std::atomic_bool m_bAbortOutput;

  BitstreamStats m_videoStats;

  CDVDMessageQueue m_messageQueue;
  CDVDMessageQueue& m_messageParent;
  CDVDStreamInfo m_hints;
  std::unique_ptr<CDVDVideoCodec> m_pVideoCodec;
  CPtsTracker m_ptsTracker;
  std::list<DVDMessageListItem> m_packets;
  CDroppingStats m_droppingStats;
  CRenderManager& m_renderManager;
  VideoPicture m_picture;

  EOutputState m_outputSate{OUTPUT_NORMAL};
};
