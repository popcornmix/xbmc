/*
 *  Copyright (C) 2017-2018 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#pragma once

#include "DropControl.h"
#include "MultiviewFramePairer.h"
#include "cores/VideoPlayer/Buffers/VideoBuffer.h"
#include "cores/VideoPlayer/DVDCodecs/Video/DVDVideoCodec.h"
#include "cores/VideoPlayer/DVDStreamInfo.h"
#include "cores/VideoPlayer/Interface/TimingConstants.h"
#include "threads/CriticalSection.h"

#include <map>
#include <memory>
#include <utility>

extern "C"
{
#include <libavfilter/avfilter.h>
}

class CVideoBufferPoolDMA;
class CVideoBufferPoolDRMPRIMEFFmpeg;

class CDVDVideoCodecDRMPRIME : public CDVDVideoCodec
{
public:
  explicit CDVDVideoCodecDRMPRIME(CProcessInfo& processInfo);
  ~CDVDVideoCodecDRMPRIME() override;

  static std::unique_ptr<CDVDVideoCodec> Create(CProcessInfo& processInfo);
  static void Register();

  bool Open(CDVDStreamInfo& hints, CDVDCodecOptions& options) override;
  bool AddData(const DemuxPacket& packet) override;
  void Reset() override;
  CDVDVideoCodec::VCReturn GetPicture(VideoPicture* pVideoPicture) override;
  const char* GetName() override { return m_name.c_str(); }
  unsigned GetAllowedReferences() override { return 5; }
  bool GetCodecStats(double& pts, int& droppedFrames, int& skippedPics) override;
  void SetCodecControl(int flags) override;

protected:
  void Drain();
  bool SetPictureParams(VideoPicture* pVideoPicture);
  void UpdateProcessInfo(struct AVCodecContext* avctx, const enum AVPixelFormat fmt);
  CDVDVideoCodec::VCReturn ProcessFilterIn();
  CDVDVideoCodec::VCReturn ProcessFilterOut();
  static enum AVPixelFormat GetFormat(struct AVCodecContext* avctx, const enum AVPixelFormat* fmt);
  static int GetBuffer(struct AVCodecContext* avctx, AVFrame* frame, int flags);
  static AVFrame *alloc_filter_frame(AVFilterContext * ctx, void * v, int w, int h);
  bool FilterOpen(const std::string& filters, bool test);
  void FilterClose();
  void FilterTest();
  std::string GetFilterChain(bool interlaced);

  std::string m_name;
  std::string m_deintFilterName;
  std::string m_filters;
  int m_codecControlFlags = 0;
  CDVDStreamInfo m_hints;
  double m_DAR = 1.0;
  bool m_checkedDeinterlace = false;
  AVCodecContext* m_pCodecContext = nullptr;
  AVFrame* m_pFrame = nullptr;
  AVFrame* m_pFilterFrame = nullptr;
  AVFilterGraph* m_pFilterGraph = nullptr;
  AVFilterContext* m_pFilterIn = nullptr;
  AVFilterContext* m_pFilterIn2 = nullptr; //!< second view of a multiview stream
  AVFilterContext* m_pFilterOut = nullptr;

  //! Eyes are coded as separate views and are packed side by side by the filter graph.
  bool m_multiview = false;
  CMultiviewFramePairer m_multiviewPairer; //!< routes the views to the graph's two inputs
  std::string m_stereoMode; //!< mode the packed frame is in, empty when not stereoscopic

  std::shared_ptr<CVideoBufferPoolDRMPRIMEFFmpeg> m_hwVideoBufferPool;

  //! A pool only ever serves the size it was configured with, and GetBuffer() is
  //! reached with more than one: the decoder's own frames arrive through
  //! get_buffer2(), the filter graph's output through alloc_filter_frame(), and
  //! for a packed multiview frame the latter is twice as wide. Keep one pool per
  //! size rather than reconfiguring, which would reallocate on every frame.
  std::map<std::pair<AVPixelFormat, int>, std::shared_ptr<CVideoBufferPoolDMA>>
      m_swVideoBufferPools;
  CCriticalSection m_swVideoBufferPoolsSection;
  bool m_started = false;
  bool m_startedInput = false;
  int m_iLastKeyframe = 0;

  CDropControl m_dropCtrl;
  double m_decoderPts = DVD_NOPTS_VALUE; //!< pts of the last picture out of the codec
  int m_droppedFrames = 0; //!< pictures dropped since the player last read the stats

  AVBufferRef *m_hw_device_ref = nullptr;
  AVBufferRef *m_hw_frames_ref = nullptr;
};
