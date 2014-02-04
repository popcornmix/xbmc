#pragma once
/*
 *      Copyright (C) 2010-2013 Team XBMC
 *      http://xbmc.org
 *
 *  This Program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2, or (at your option)
 *  any later version.
 *
 *  This Program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with XBMC; see the file COPYING.  If not, see
 *  <http://www.gnu.org/licenses/>.
 *
 */

#if defined(HAVE_LIBOPENMAX)

#include "system_gl.h"
#include <EGL/egl.h>
#include <EGL/eglext.h>

#include "linux/OMXCore.h"
#include "linux/OMXClock.h"

#include "cores/dvdplayer/DVDStreamInfo.h"
#include "DVDVideoCodec.h"
#include "threads/Event.h"
#include "xbmc/settings/VideoSettings.h"

#include <queue>
#include <semaphore.h>

typedef struct omx_demux_packet {
  OMX_U8 *buff;
  int size;
  double dts;
  double pts;
} omx_demux_packet;

class COpenMaxVideo;
typedef boost::shared_ptr<COpenMaxVideo> OpenMaxVideoPtr;
// an omx egl video frame
class COpenMaxVideoBuffer
{
public:
  COpenMaxVideoBuffer(COpenMaxVideo *omv);
  virtual ~COpenMaxVideoBuffer();

  OMX_BUFFERHEADERTYPE *omx_buffer;
  int width;
  int height;
  float m_aspect_ratio;
  int index;

  // used for egl based rendering if active
  EGLImageKHR egl_image;
  GLuint texture_id;
  // reference counting
  COpenMaxVideoBuffer* Acquire();
  long                 Release();
  void                 Sync();

  COpenMaxVideo *m_omv;
  long m_refs;
private:
};

class COpenMaxVideo
{
public:
  COpenMaxVideo();
  virtual ~COpenMaxVideo();

  // Required overrides
  virtual bool Open(CDVDStreamInfo &hints, CDVDCodecOptions &options, OpenMaxVideoPtr myself);
  virtual void Dispose(void);
  virtual int  Decode(uint8_t *pData, int iSize, double dts, double pts);
  virtual void Reset(void);
  virtual bool GetPicture(DVDVideoPicture *pDvdVideoPicture);
  virtual bool ClearPicture(DVDVideoPicture* pDvdVideoPicture);
  virtual unsigned GetAllowedReferences() { return 2; }
  virtual void SetDropState(bool bDrop);
  virtual const char* GetName(void) { return (const char*)m_pFormatName; }

  // OpenMax decoder callback routines.
  OMX_ERRORTYPE DecoderFillBufferDone(OMX_HANDLETYPE hComponent, OMX_BUFFERHEADERTYPE* pBuffer);
  void ReleaseOpenMaxBuffer(COpenMaxVideoBuffer *buffer);

protected:
  void QueryCodec(void);
  OMX_ERRORTYPE PrimeFillBuffers(void);
  OMX_ERRORTYPE AllocOMXInputBuffers(void);
  OMX_ERRORTYPE FreeOMXInputBuffers(void);
  bool AllocOMXOutputBuffers(void);
  bool FreeOMXOutputBuffers(void);
  static bool CallbackAllocOMXEGLTextures(void*);
  OMX_ERRORTYPE AllocOMXOutputEGLTextures(void);
  static bool CallbackFreeOMXEGLTextures(void*);
  OMX_ERRORTYPE FreeOMXOutputEGLTextures(void);
  OMX_ERRORTYPE StopDecoder(void);
  OMX_ERRORTYPE ReturnOpenMaxBuffer(COpenMaxVideoBuffer *buffer);

  // EGL Resources
  EGLDisplay        m_egl_display;
  EGLContext        m_egl_context;

  // Video format
  bool              m_drop_state;
  int               m_decoded_width;
  int               m_decoded_height;
  unsigned int      m_egl_buffer_count;
  bool              m_finished;
  float             m_aspect_ratio;
  bool              m_forced_aspect_ratio;

  bool m_port_settings_changed;
  const char        *m_pFormatName;
  OpenMaxVideoPtr   m_myself;

  std::queue<double> m_dts_queue;
  std::queue<omx_demux_packet> m_demux_queue;

  // OpenMax output buffers (video frames)
  pthread_mutex_t   m_omx_output_mutex;
  std::vector<COpenMaxVideoBuffer*> m_omx_output_busy;
  std::queue<COpenMaxVideoBuffer*> m_omx_output_ready;
  std::vector<COpenMaxVideoBuffer*> m_omx_output_buffers;

  // initialize OpenMax and get decoder component
  bool Initialize( const CStdString &decoder_name);

  // Components
  COMXCoreComponent m_omx_decoder;
  COMXCoreComponent m_omx_image_fx;
  COMXCoreComponent m_omx_egl_render;

  COMXCoreTunel     m_omx_tunnel_decoder;
  COMXCoreTunel     m_omx_tunnel_image_fx;
  OMX_VIDEO_CODINGTYPE m_codingType;

  bool              m_deinterlace;
  EDEINTERLACEMODE  m_deinterlace_request;
  bool              m_deinterlace_second_field;

  bool PortSettingsChanged();
  bool SendDecoderConfig(uint8_t *extradata, int extrasize);
  bool NaluFormatStartCodes(enum AVCodecID codec, uint8_t *extradata, int extrasize);
};

// defined(HAVE_LIBOPENMAX)
#endif
