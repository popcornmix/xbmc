/*
 *  Copyright (C) 2005-2018 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#pragma once

#include "DVDVideoCodec.h"
#include "DVDVideoPP.h"
#include "cores/VideoPlayer/DVDCodecs/DVDCodecs.h"
#include "cores/VideoPlayer/DVDStreamInfo.h"

#include <map>
#include <memory>
#include <string>
#include <vector>
#ifdef HAVE_DVBRIDGE
#include <deque>
struct dvbridge_fel;
#endif

extern "C"
{
#include <libavcodec/avcodec.h>
#include <libavfilter/avfilter.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
#include <libswscale/swscale.h>
}

class CVideoBufferPoolFFmpeg;

class CDVDVideoCodecFFmpeg : public CDVDVideoCodec, public ICallbackHWAccel
{
public:
  explicit CDVDVideoCodecFFmpeg(CProcessInfo &processInfo);
  ~CDVDVideoCodecFFmpeg() override;
  bool Open(CDVDStreamInfo &hints, CDVDCodecOptions &options) override;
  bool AddData(const DemuxPacket &packet) override;
  void Reset() override;
  void Reopen() override;
  CDVDVideoCodec::VCReturn GetPicture(VideoPicture* pVideoPicture) override;
  const char* GetName() override { return m_name.c_str(); }; // m_name is never changed after open
  unsigned GetConvergeCount() override;
  unsigned GetAllowedReferences() override;
  bool GetCodecStats(double &pts, int &droppedFrames, int &skippedPics) override;
  void SetCodecControl(int flags) override;

  IHardwareDecoder* GetHWAccel() override;
  bool GetPictureCommon(VideoPicture* pVideoPicture) override;
#ifdef CB1_HAS_HDR10_AI
  unsigned GetHDR10AIBufferCount() const override;
  void PrepareHDR10AIReopen() override;
  void BindHDR10AISource(VideoPicture& picture);
#endif
#ifdef HAVE_DVBRIDGE
  bool NeedsDVEnhancement() const
  {
    return m_dvNativeOutput && m_hints.dovi.dv_profile == 7 && m_hints.dovi.el_present_flag;
  }
#endif

protected:
  CDVDVideoCodec::VCReturn GetBasePicture(VideoPicture* picture);
#ifdef HAVE_DVBRIDGE
  std::shared_ptr<dvbridge_fel> m_dvFel;
  std::deque<std::shared_ptr<VideoPicture>> m_dvPending;
  bool m_dvFailed{false};
  bool m_dvNativeOutput{false};
  DVBRIDGE::SourceOwner m_dvSourceOwner;
  // Last in-band HDR10 static metadata of this stream (see GetPictureCommon).
  bool m_dvHasStreamDisplayMetadata{false};
  AVMasteringDisplayMetadata m_dvStreamDisplayMetadata{};
  bool m_dvHasStreamLightMetadata{false};
  AVContentLightMetadata m_dvStreamLightMetadata{};
#endif
  void Dispose();
  CDVDVideoCodec::VCReturn HandleHwFatal();
  static enum AVPixelFormat GetFormat(struct AVCodecContext * avctx, const AVPixelFormat * fmt);

  int  FilterOpen(const std::string& filters, bool scale);
  void FilterClose();
  CDVDVideoCodec::VCReturn FilterProcess(AVFrame* frame);
  void SetFilters();
  void UpdateName();
  bool SetPictureParams(VideoPicture* pVideoPicture);

  bool HasHardware() { return m_pHardware != nullptr; }
  void SetHardware(IHardwareDecoder *hardware);

  AVFrame* m_pFrame = nullptr;;
  AVFrame* m_pDecodedFrame = nullptr;;
  AVCodecContext* m_pCodecContext = nullptr;;
  std::shared_ptr<CVideoBufferPoolFFmpeg> m_videoBufferPool;

  std::string m_filters;
  std::string m_filters_next;
  AVFilterGraph* m_pFilterGraph = nullptr;
  AVFilterContext* m_pFilterIn = nullptr;
  AVFilterContext* m_pFilterOut = nullptr;;
  AVFrame* m_pFilterFrame = nullptr;;
  bool m_filterEof = false;
  bool m_eof = false;

  std::unique_ptr<IDVDVideoPP> m_postProc;

  int m_iPictureWidth = 0;
  int m_iPictureHeight = 0;
  int m_iScreenWidth = 0;
  int m_iScreenHeight = 0;
  int m_iOrientation = 0;// orientation of the video in degrees counter clockwise

  std::string m_name;
  int m_decoderState;
  int m_hwFailedCount = 0;
  IHardwareDecoder *m_pHardware = nullptr;
  int m_iLastKeyframe = 0;
  double m_dts = DVD_NOPTS_VALUE;
  bool m_started = false;
  bool m_startedInput = false;
  std::vector<AVPixelFormat> m_formats;
  double m_decoderPts = DVD_NOPTS_VALUE;
  int m_skippedDeint = 0;
  int m_droppedFrames = 0;
  bool m_requestSkipDeint = false;
  int m_codecControlFlags = 0;
  bool m_interlaced = false;
  double m_DAR = 1.0;
  CDVDStreamInfo m_hints;
  CDVDCodecOptions m_options;
  bool m_exportDoviRpu = false;
  std::map<int64_t, std::vector<uint8_t>> m_av1DoviRpus; // by packet pts

  struct CDropControl
  {
    CDropControl();
    void Reset(bool init);
    void Process(int64_t pts, bool drop);

    int64_t m_lastPTS;
    int64_t m_diffPTS;
    int m_count;
    enum
    {
      INIT,
      VALID
    } m_state;
  } m_dropCtrl;
};
