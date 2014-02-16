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

#if (defined HAVE_CONFIG_H) && (!defined TARGET_WINDOWS)
  #include "config.h"
#elif defined(TARGET_WINDOWS)
#include "system.h"
#endif

#include "OpenMaxVideo.h"

#include "DVDClock.h"
#include "DVDStreamInfo.h"
#include "windowing/WindowingFactory.h"
#include "DVDVideoCodec.h"
#include "utils/log.h"
#include "utils/TimeUtils.h"
#include "settings/Settings.h"
#include "settings/MediaSettings.h"
#include "ApplicationMessenger.h"
#include "Application.h"
#include "threads/Atomics.h"

#include <IL/OMX_Core.h>
#include <IL/OMX_Component.h>
#include <IL/OMX_Index.h>
#include <IL/OMX_Image.h>

#include "cores/omxplayer/OMXImage.h"
#include "linux/RBP.h"

#define DTS_QUEUE

#define DEFAULT_TIMEOUT 1000
#ifdef _DEBUG
#define OMX_DEBUG_VERBOSE
#endif

#define CLASSNAME "COpenMaxVideo"

#define OMX_BUFFERFLAG_PTS_INVALID (1<<28)
#define OMX_BUFFERFLAG_DROPPED     (1<<29)

COpenMaxVideoBuffer::COpenMaxVideoBuffer(COpenMaxVideo *omv)
    : m_omv(omv), m_refs(0)
{
  CLog::Log(LOGDEBUG, "%s::%s %p", CLASSNAME, __func__, this);
  omx_buffer = NULL;
  width = 0;
  height = 0;
  index = 0;
  egl_image = 0;
  texture_id = 0;
  m_aspect_ratio = 0.0f;
}

COpenMaxVideoBuffer::~COpenMaxVideoBuffer()
{
  CLog::Log(LOGDEBUG, "%s::%s %p", CLASSNAME, __func__, this);
}


// DecoderFillBufferDone -- OpenMax output buffer has been filled
static OMX_ERRORTYPE DecoderFillBufferDoneCallback(
  OMX_HANDLETYPE hComponent,
  OMX_PTR pAppData,
  OMX_BUFFERHEADERTYPE* pBuffer)
{
  COpenMaxVideoBuffer *pic = static_cast<COpenMaxVideoBuffer*>(pBuffer->pAppPrivate);
  COpenMaxVideo *ctx = pic->m_omv;
  return ctx->DecoderFillBufferDone(hComponent, pBuffer);
}


COpenMaxVideoBuffer* COpenMaxVideoBuffer::Acquire()
{
  long count = AtomicIncrement(&m_refs);
  #if defined(OMX_DEBUG_VERBOSE)
  CLog::Log(LOGDEBUG, "%s::%s %p ref:%ld", CLASSNAME, __func__, this, count);
  #endif
  (void)count;
  return this;
}

long COpenMaxVideoBuffer::Release()
{
  long count = AtomicDecrement(&m_refs);
  if (count == 0)
  {
    m_omv->ReleaseOpenMaxBuffer(this);
  }

  #if defined(OMX_DEBUG_VERBOSE)
  CLog::Log(LOGDEBUG, "%s::%s %p ref:%ld", CLASSNAME, __func__, this, count);
  #endif
  return count;
}

void COpenMaxVideoBuffer::Sync()
{
  #if defined(OMX_DEBUG_VERBOSE)
  CLog::Log(LOGDEBUG, "%s::%s %p ref:%ld", CLASSNAME, __func__, this, m_refs);
  #endif
  Release();
}

COpenMaxVideo::COpenMaxVideo()
{
  #if defined(OMX_DEBUG_VERBOSE)
  CLog::Log(LOGDEBUG, "%s::%s %p", CLASSNAME, __func__, this);
  #endif
  pthread_mutex_init(&m_omx_output_mutex, NULL);

  m_drop_state = false;
  m_decoded_width = 0;
  m_decoded_height = 0;
  m_egl_buffer_count = 0;

  m_port_settings_changed = false;
  m_finished = false;
  m_pFormatName = "omx-xxxx";

  m_deinterlace = false;
  m_deinterlace_request = VS_DEINTERLACEMODE_OFF;
  m_deinterlace_second_field = false;
  m_startframe = false;
}

COpenMaxVideo::~COpenMaxVideo()
{
  #if defined(OMX_DEBUG_VERBOSE)
  CLog::Log(LOGDEBUG, "%s::%s %p", CLASSNAME, __func__, this);
  #endif
  assert(m_finished);
  if (m_omx_decoder.IsInitialized())
  {
    if (m_omx_tunnel_decoder.IsInitialized())
      m_omx_tunnel_decoder.Deestablish();
    if (m_omx_tunnel_image_fx.IsInitialized())
      m_omx_tunnel_image_fx.Deestablish();

    StopDecoder();

    if (m_omx_egl_render.IsInitialized())
      m_omx_egl_render.Deinitialize();
    if (m_omx_image_fx.IsInitialized())
      m_omx_image_fx.Deinitialize();
    if (m_omx_decoder.IsInitialized())
      m_omx_decoder.Deinitialize();
  }
  pthread_mutex_destroy(&m_omx_output_mutex);
}

bool COpenMaxVideo::Open(CDVDStreamInfo &hints, CDVDCodecOptions &options, OpenMaxVideoPtr myself)
{
  #if defined(OMX_DEBUG_VERBOSE)
  CLog::Log(LOGDEBUG, "%s::%s useomx:%d software:%d", CLASSNAME, __func__, CSettings::Get().GetBool("videoplayer.useomx"), hints.software);
  #endif

  // we always qualify even if DVDFactoryCodec does this too.
  if (!CSettings::Get().GetBool("videoplayer.useomx") || hints.software)
    return false;

  OMX_ERRORTYPE omx_err = OMX_ErrorNone;

  m_deinterlace_request = CMediaSettings::Get().GetCurrentVideoSettings().m_DeinterlaceMode;

  m_myself = myself;
  m_decoded_width  = hints.width;
  m_decoded_height = hints.height;
  m_forced_aspect_ratio = hints.forced_aspect;
  m_aspect_ratio = hints.aspect;

  m_egl_buffer_count = 4;

  m_codingType = OMX_VIDEO_CodingUnused;

  switch (hints.codec)
  {
    case AV_CODEC_ID_H264:
      // H.264
      m_codingType = OMX_VIDEO_CodingAVC;
      m_pFormatName = "omx-h264";
    break;
    case AV_CODEC_ID_H263:
    case AV_CODEC_ID_MPEG4:
      // MPEG-4, DivX 4/5 and Xvid compatible
      m_codingType = OMX_VIDEO_CodingMPEG4;
      m_pFormatName = "omx-mpeg4";
    break;
    case AV_CODEC_ID_MPEG1VIDEO:
    case AV_CODEC_ID_MPEG2VIDEO:
      // MPEG-2
      m_codingType = OMX_VIDEO_CodingMPEG2;
      m_pFormatName = "omx-mpeg2";
    break;
    case AV_CODEC_ID_VP6:
      // this form is encoded upside down
      // fall through
    case AV_CODEC_ID_VP6F:
    case AV_CODEC_ID_VP6A:
      // VP6
      m_codingType = OMX_VIDEO_CodingVP6;
      m_pFormatName = "omx-vp6";
    break;
    case AV_CODEC_ID_VP8:
      // VP8
      m_codingType = OMX_VIDEO_CodingVP8;
      m_pFormatName = "omx-vp8";
    break;
    case AV_CODEC_ID_THEORA:
      // theora
      m_codingType = OMX_VIDEO_CodingTheora;
      m_pFormatName = "omx-theora";
    break;
    case AV_CODEC_ID_MJPEG:
    case AV_CODEC_ID_MJPEGB:
      // mjpg
      m_codingType = OMX_VIDEO_CodingMJPEG;
      m_pFormatName = "omx-mjpg";
    break;
    case AV_CODEC_ID_VC1:
    case AV_CODEC_ID_WMV3:
      // VC-1, WMV9
      m_codingType = OMX_VIDEO_CodingWMV;
      m_pFormatName = "omx-vc1";
      break;
    default:
      CLog::Log(LOGERROR, "%s::%s : Video codec unknown: %x", CLASSNAME, __func__, hints.codec);
      return false;
    break;
  }

  if ( (m_codingType == OMX_VIDEO_CodingMPEG2 && !g_RBP.GetCodecMpg2() ) ||
       (m_codingType == OMX_VIDEO_CodingWMV   && !g_RBP.GetCodecWvc1() ) )
  {
    CLog::Log(LOGWARNING, "%s::%s Codec %s is not supported\n", CLASSNAME, __func__, m_pFormatName);
    return false;
  }

  // initialize OpenMAX.
  if (!m_omx_decoder.Initialize("OMX.broadcom.video_decode", OMX_IndexParamVideoInit))
  {
    CLog::Log(LOGERROR, "%s::%s error m_omx_decoder.Initialize", CLASSNAME, __func__);
    return false;
  }

  omx_err = m_omx_decoder.SetStateForComponent(OMX_StateIdle);
  if (omx_err != OMX_ErrorNone)
  {
    CLog::Log(LOGERROR, "%s::%s m_omx_decoder.SetStateForComponent omx_err(0x%08x)", CLASSNAME, __func__, omx_err);
    return false;
  }

  OMX_VIDEO_PARAM_PORTFORMATTYPE formatType;
  OMX_INIT_STRUCTURE(formatType);
  formatType.nPortIndex = m_omx_decoder.GetInputPort();
  formatType.eCompressionFormat = m_codingType;

  omx_err = m_omx_decoder.SetParameter(OMX_IndexParamVideoPortFormat, &formatType);
  if (omx_err != OMX_ErrorNone)
  {
    CLog::Log(LOGERROR, "%s::%s m_omx_decoder.SetParameter omx_err(0x%08x)", CLASSNAME, __func__, omx_err);
    return false;
  }
  OMX_PARAM_PORTDEFINITIONTYPE portParam;
  OMX_INIT_STRUCTURE(portParam);
  portParam.nPortIndex = m_omx_decoder.GetInputPort();

  omx_err = m_omx_decoder.GetParameter(OMX_IndexParamPortDefinition, &portParam);
  if (omx_err != OMX_ErrorNone)
  {
    CLog::Log(LOGERROR, "%s::%s error OMX_IndexParamPortDefinition omx_err(0x%08x)", CLASSNAME, __func__, omx_err);
    return false;
  }

  portParam.nPortIndex = m_omx_decoder.GetInputPort();
  portParam.nBufferCountActual = 20;
  portParam.format.video.nFrameWidth  = m_decoded_width;
  portParam.format.video.nFrameHeight = m_decoded_height;

  omx_err = m_omx_decoder.SetParameter(OMX_IndexParamPortDefinition, &portParam);
  if (omx_err != OMX_ErrorNone)
  {
    CLog::Log(LOGERROR, "%s::%s error OMX_IndexParamPortDefinition omx_err(0x%08x)", CLASSNAME, __func__, omx_err);
    return false;
  }

  // request portsettingschanged on aspect ratio change
  OMX_CONFIG_REQUESTCALLBACKTYPE notifications;
  OMX_INIT_STRUCTURE(notifications);
  notifications.nPortIndex = m_omx_decoder.GetOutputPort();
  notifications.nIndex = OMX_IndexParamBrcmPixelAspectRatio;
  notifications.bEnable = OMX_TRUE;

  omx_err = m_omx_decoder.SetParameter((OMX_INDEXTYPE)OMX_IndexConfigRequestCallback, &notifications);
  if (omx_err != OMX_ErrorNone)
  {
    CLog::Log(LOGERROR, "%s::%s OMX_IndexConfigRequestCallback error (0%08x)", CLASSNAME, __func__, omx_err);
    return false;
  }

  if (NaluFormatStartCodes(hints.codec, (uint8_t *)hints.extradata, hints.extrasize))
  {
    OMX_NALSTREAMFORMATTYPE nalStreamFormat;
    OMX_INIT_STRUCTURE(nalStreamFormat);
    nalStreamFormat.nPortIndex = m_omx_decoder.GetInputPort();
    nalStreamFormat.eNaluFormat = OMX_NaluFormatStartCodes;

    omx_err = m_omx_decoder.SetParameter((OMX_INDEXTYPE)OMX_IndexParamNalStreamFormatSelect, &nalStreamFormat);
    if (omx_err != OMX_ErrorNone)
    {
      CLog::Log(LOGERROR, "%s::%s OMX_IndexParamNalStreamFormatSelect error (0%08x)", CLASSNAME, __func__, omx_err);
      return false;
    }
  }

  // Alloc buffers for the omx input port.
  omx_err = m_omx_decoder.AllocInputBuffers();
  if (omx_err != OMX_ErrorNone)
  {
    CLog::Log(LOGERROR, "%s::%s AllocInputBuffers error (0%08x)", CLASSNAME, __func__, omx_err);
    return false;
  }

  omx_err = m_omx_decoder.SetStateForComponent(OMX_StateExecuting);
  if (omx_err != OMX_ErrorNone)
  {
    CLog::Log(LOGERROR, "%s::%s error m_omx_decoder.SetStateForComponent error (0%08x)", CLASSNAME, __func__, omx_err);
    return false;
  }

  if (!SendDecoderConfig((uint8_t *)hints.extradata, hints.extrasize))
    return false;

  m_drop_state = false;
  m_startframe = false;

  return true;
}

void COpenMaxVideo::Dispose()
{
  #if defined(OMX_DEBUG_VERBOSE)
  CLog::Log(LOGDEBUG, "%s::%s", CLASSNAME, __func__);
  #endif
  // we are happy to exit, but let last shared pointer being deleted trigger the destructor
  bool done = false;
  pthread_mutex_lock(&m_omx_output_mutex);
  if (m_omx_output_busy.empty())
    done = true;
  m_finished = true;
  pthread_mutex_unlock(&m_omx_output_mutex);
  if (done)
    m_myself.reset();
}

void COpenMaxVideo::SetDropState(bool bDrop)
{
#if defined(OMX_DEBUG_VERBOSE)
  if (m_drop_state != bDrop)
    CLog::Log(LOGDEBUG, "%s::%s - m_drop_state(%d)",
      CLASSNAME, __func__, bDrop);
#endif
  m_drop_state = bDrop;
  if (m_drop_state)
  {
    while (1)
    {
      COpenMaxVideoBuffer *buffer = NULL;
      pthread_mutex_lock(&m_omx_output_mutex);
      // fetch a output buffer and pop it off the ready list
      if (!m_omx_output_ready.empty())
      {
        buffer = m_omx_output_ready.front();
        m_omx_output_ready.pop();
      }
      pthread_mutex_unlock(&m_omx_output_mutex);
      if (buffer)
        ReturnOpenMaxBuffer(buffer);
      else
        break;
    }
  }
}

bool COpenMaxVideo::SendDecoderConfig(uint8_t *extradata, int extrasize)
{
  OMX_ERRORTYPE omx_err   = OMX_ErrorNone;

  /* send decoder config */
  if (extrasize > 0 && extradata != NULL)
  {
    OMX_BUFFERHEADERTYPE *omx_buffer = m_omx_decoder.GetInputBuffer();

    if (omx_buffer == NULL)
    {
      CLog::Log(LOGERROR, "%s::%s - buffer error 0x%08x", CLASSNAME, __func__, omx_err);
      return false;
    }

    omx_buffer->nOffset = 0;
    omx_buffer->nFilledLen = extrasize;
    if (omx_buffer->nFilledLen > omx_buffer->nAllocLen)
    {
      CLog::Log(LOGERROR, "%s::%s - omx_buffer->nFilledLen > omx_buffer->nAllocLen", CLASSNAME, __func__);
      return false;
    }

    memcpy((unsigned char *)omx_buffer->pBuffer, extradata, omx_buffer->nFilledLen);
    omx_buffer->nFlags = OMX_BUFFERFLAG_CODECCONFIG | OMX_BUFFERFLAG_ENDOFFRAME;

//CLog::Log(LOGINFO, "%s::%s - Empty(%d,%x)", CLASSNAME, __func__, omx_buffer->nFilledLen, omx_buffer->nFlags); CLog::MemDump((char *)omx_buffer->pBuffer, std::min(64U, omx_buffer->nFilledLen));
    omx_err = m_omx_decoder.EmptyThisBuffer(omx_buffer);
    if (omx_err != OMX_ErrorNone)
    {
      CLog::Log(LOGERROR, "%s::%s - OMX_EmptyThisBuffer() failed with result(0x%x)", CLASSNAME, __func__, omx_err);
      return false;
    }
  }
  return true;
}

bool COpenMaxVideo::NaluFormatStartCodes(enum AVCodecID codec, uint8_t *extradata, int extrasize)
{
  switch(codec)
  {
    case AV_CODEC_ID_H264:
      if (extrasize < 7 || extradata == NULL)
        return true;
      // valid avcC atom data always starts with the value 1 (version), otherwise annexb
      else if ( *extradata != 1 )
        return true;
    default: break;
  }
  return false;
}

bool COpenMaxVideo::PortSettingsChanged()
{
  OMX_ERRORTYPE omx_err   = OMX_ErrorNone;

  if (m_port_settings_changed)
  {
    m_omx_decoder.DisablePort(m_omx_decoder.GetOutputPort(), true);
  }

  OMX_PARAM_PORTDEFINITIONTYPE port_def;
  OMX_INIT_STRUCTURE(port_def);
  port_def.nPortIndex = m_omx_decoder.GetOutputPort();
  omx_err = m_omx_decoder.GetParameter(OMX_IndexParamPortDefinition, &port_def);
  if (omx_err != OMX_ErrorNone)
  {
    CLog::Log(LOGERROR, "%s::%s - error m_omx_decoder.GetParameter(OMX_IndexParamPortDefinition) omx_err(0x%08x)", CLASSNAME, __func__, omx_err);
    return false;
  }

  OMX_CONFIG_POINTTYPE pixel_aspect;
  OMX_INIT_STRUCTURE(pixel_aspect);
  pixel_aspect.nPortIndex = m_omx_decoder.GetOutputPort();
  omx_err = m_omx_decoder.GetParameter(OMX_IndexParamBrcmPixelAspectRatio, &pixel_aspect);
  if (omx_err != OMX_ErrorNone)
  {
    CLog::Log(LOGERROR, "%s::%s - error m_omx_decoder.GetParameter(OMX_IndexParamBrcmPixelAspectRatio) omx_err(0x%08x)", CLASSNAME, __func__, omx_err);
    return false;
  }
  if (!m_forced_aspect_ratio && pixel_aspect.nX && pixel_aspect.nY)
    m_aspect_ratio = (float)pixel_aspect.nX * port_def.format.video.nFrameWidth /
      ((float)pixel_aspect.nY * port_def.format.video.nFrameHeight);

  if (m_port_settings_changed)
  {
    m_omx_decoder.EnablePort(m_omx_decoder.GetOutputPort(), true);
    return true;
  }

  // convert in stripes
  port_def.format.video.nSliceHeight = 16;
  port_def.format.video.nStride = 0;

  omx_err = m_omx_decoder.SetParameter(OMX_IndexParamPortDefinition, &port_def);
  if (omx_err != OMX_ErrorNone)
  {
    CLog::Log(LOGERROR, "%s::%s m_omx_decoder.SetParameter result(0x%x)", CLASSNAME, __func__, omx_err);
    return false;
  }

  OMX_CONFIG_INTERLACETYPE interlace;
  OMX_INIT_STRUCTURE(interlace);
  interlace.nPortIndex = m_omx_decoder.GetOutputPort();
  omx_err = m_omx_decoder.GetConfig(OMX_IndexConfigCommonInterlace, &interlace);

  if (m_deinterlace_request == VS_DEINTERLACEMODE_FORCE)
    m_deinterlace = true;
  else if (m_deinterlace_request == VS_DEINTERLACEMODE_OFF)
    m_deinterlace = false;
  else
    m_deinterlace = interlace.eMode != OMX_InterlaceProgressive;

  CLog::Log(LOGDEBUG, "%s::%s - %dx%d@%.2f interlace:%d deinterlace:%d",
  CLASSNAME, __func__, port_def.format.video.nFrameWidth, port_def.format.video.nFrameHeight, port_def.format.video.xFramerate / (float) (1 << 16),
      interlace.eMode, m_deinterlace);

  if (m_deinterlace)
  {
    if (!m_omx_image_fx.Initialize("OMX.broadcom.image_fx", OMX_IndexParamImageInit))
    {
      CLog::Log(LOGERROR, "%s::%s error m_omx_image_fx.Initialize", CLASSNAME, __func__);
      return false;
    }
  }

  if (m_deinterlace)
  {
    OMX_CONFIG_IMAGEFILTERPARAMSTYPE image_filter;
    OMX_INIT_STRUCTURE(image_filter);

    image_filter.nPortIndex = m_omx_image_fx.GetOutputPort();
    image_filter.nNumParams = 1;
    image_filter.nParams[0] = 3;
    image_filter.eImageFilter = OMX_ImageFilterDeInterlaceAdvanced;

    omx_err = m_omx_image_fx.SetConfig(OMX_IndexConfigCommonImageFilterParameters, &image_filter);
    if (omx_err != OMX_ErrorNone)
    {
      CLog::Log(LOGERROR, "%s::%s - OMX_IndexConfigCommonImageFilterParameters omx_err(0x%08x)", CLASSNAME, __func__, omx_err);
      return false;
    }
  }

  OMX_CALLBACKTYPE callbacks = { NULL, NULL, DecoderFillBufferDoneCallback };
  if (!m_omx_egl_render.Initialize("OMX.broadcom.egl_render", OMX_IndexParamVideoInit, &callbacks))
  {
    CLog::Log(LOGERROR, "%s::%s error m_omx_egl_render.Initialize", CLASSNAME, __func__);
    return false;
  }

  OMX_CONFIG_PORTBOOLEANTYPE discardMode;
  OMX_INIT_STRUCTURE(discardMode);
  discardMode.nPortIndex = m_omx_egl_render.GetInputPort();
  discardMode.bEnabled = OMX_FALSE;
  omx_err = m_omx_egl_render.SetParameter(OMX_IndexParamBrcmVideoEGLRenderDiscardMode, &discardMode);
  if (omx_err != OMX_ErrorNone)
  {
    CLog::Log(LOGERROR, "%s::%s - error m_omx_egl_render.SetParameter(OMX_IndexParamBrcmVideoEGLRenderDiscardMode) omx_err(0x%08x)", CLASSNAME, __func__, omx_err);
    return false;
  }

  m_omx_egl_render.ResetEos();

  if (m_deinterlace)
  {
    m_omx_tunnel_decoder.Initialize(&m_omx_decoder, m_omx_decoder.GetOutputPort(), &m_omx_image_fx, m_omx_image_fx.GetInputPort());
    m_omx_tunnel_image_fx.Initialize(&m_omx_image_fx, m_omx_image_fx.GetOutputPort(), &m_omx_egl_render, m_omx_egl_render.GetInputPort());
  }
  else
  {
    m_omx_tunnel_decoder.Initialize(&m_omx_decoder, m_omx_decoder.GetOutputPort(), &m_omx_egl_render, m_omx_egl_render.GetInputPort());
  }

  omx_err = m_omx_tunnel_decoder.Establish();
  if (omx_err != OMX_ErrorNone)
  {
    CLog::Log(LOGERROR, "%s::%s - m_omx_tunnel_decoder.Establish omx_err(0x%08x)", CLASSNAME, __func__, omx_err);
    return false;
  }

  if (m_deinterlace)
  {
    omx_err = m_omx_tunnel_image_fx.Establish();
    if (omx_err != OMX_ErrorNone)
    {
      CLog::Log(LOGERROR, "%s::%s - m_omx_tunnel_image_fx.Establish omx_err(0x%08x)", CLASSNAME, __func__, omx_err);
      return false;
    }
    omx_err = m_omx_image_fx.SetStateForComponent(OMX_StateExecuting);
    if (omx_err != OMX_ErrorNone)
    {
      CLog::Log(LOGERROR, "%s::%s - m_omx_image_fx.SetStateForComponent omx_err(0x%08x)",
      CLASSNAME, __func__, omx_err);
      return false;
    }
  }

  // Obtain the information about the output port.
  OMX_PARAM_PORTDEFINITIONTYPE port_format;
  OMX_INIT_STRUCTURE(port_format);
  port_format.nPortIndex = m_omx_egl_render.GetOutputPort();
  omx_err = m_omx_egl_render.GetParameter(OMX_IndexParamPortDefinition, &port_format);
  if (omx_err != OMX_ErrorNone)
    CLog::Log(LOGERROR, "%s::%s - m_omx_egl_render.GetParameter OMX_IndexParamPortDefinition omx_err(0x%08x)", CLASSNAME, __func__, omx_err);

  port_format.nBufferCountActual = m_egl_buffer_count;
  omx_err = m_omx_egl_render.SetParameter(OMX_IndexParamPortDefinition, &port_format);
  if (omx_err != OMX_ErrorNone)
    CLog::Log(LOGERROR, "%s::%s - m_omx_egl_render.SetParameter OMX_IndexParamPortDefinition omx_err(0x%08x)", CLASSNAME, __func__, omx_err);

  #if defined(OMX_DEBUG_VERBOSE)
  CLog::Log(LOGDEBUG,
    "%s::%s (1) - oport(%d), nFrameWidth(%u), nFrameHeight(%u), nStride(%x), nBufferCountMin(%u), nBufferCountActual(%u), nBufferSize(%u)",
    CLASSNAME, __func__, m_omx_egl_render.GetOutputPort(),
    port_format.format.video.nFrameWidth, port_format.format.video.nFrameHeight,port_format.format.video.nStride,
    port_format.nBufferCountMin, port_format.nBufferCountActual, port_format.nBufferSize);
  #endif


  omx_err =  m_omx_egl_render.EnablePort(m_omx_egl_render.GetOutputPort(), false);
  if (omx_err != OMX_ErrorNone)
  {
    CLog::Log(LOGERROR, "%s::%s - m_omx_egl_render.EnablePort omx_err(0x%08x)", CLASSNAME, __func__, omx_err);
    return false;
  }

  if (!AllocOMXOutputBuffers())
  {
    CLog::Log(LOGERROR, "%s::%s - AllocOMXOutputBuffers failed", CLASSNAME, __func__);
    return false;
  }

  omx_err = m_omx_egl_render.WaitForCommand(OMX_CommandPortEnable, m_omx_egl_render.GetOutputPort());
  if (omx_err != OMX_ErrorNone)
  {
    CLog::Log(LOGERROR, "%s::%s m_omx_egl_render.WaitForCommand(OMX_CommandPortEnable) omx_err(0x%08x)", CLASSNAME, __func__, omx_err);
    return false;
  }

  assert(m_omx_output_busy.empty());
  assert(m_omx_output_ready.empty());

  omx_err = m_omx_egl_render.SetStateForComponent(OMX_StateExecuting);
  if (omx_err != OMX_ErrorNone)
  {
    CLog::Log(LOGERROR, "%s::%s - m_omx_egl_render.SetStateForComponent omx_err(0x%08x)", CLASSNAME, __func__, omx_err);
    return false;
  }

  omx_err = PrimeFillBuffers();
  if (omx_err != OMX_ErrorNone)
  {
    CLog::Log(LOGERROR, "%s::%s - m_omx_egl_render.PrimeFillBuffers omx_err(0x%08x)", CLASSNAME, __func__, omx_err);
    return false;
  }

  m_port_settings_changed = true;
  return true;
}


int COpenMaxVideo::Decode(uint8_t* pData, int iSize, double dts, double pts)
{
  #if defined(OMX_DEBUG_VERBOSE)
  CLog::Log(LOGDEBUG, "%s::%s - %-8p %-6d dts:%.3f pts:%.3f demux_queue(%d) dts_queue(%d) ready_queue(%d) busy_queue(%d)",
     CLASSNAME, __func__, pData, iSize, dts == DVD_NOPTS_VALUE ? 0.0 : dts*1e-6, pts == DVD_NOPTS_VALUE ? 0.0 : pts*1e-6, m_demux_queue.size(), m_dts_queue.size(), m_omx_output_ready.size(), m_omx_output_busy.size());
  #endif

  OMX_ERRORTYPE omx_err;
  unsigned int demuxer_bytes = 0;
  uint8_t *demuxer_content = NULL;

  // we need to queue then de-queue the demux packet, seems silly but
  // omx might not have a omx input buffer available when we are called
  // and we must store the demuxer packet and try again later.
  if (pData && m_demux_queue.empty() && m_omx_decoder.GetInputBufferSpace() >= (unsigned int)iSize)
  {
    demuxer_bytes = iSize;
    demuxer_content = pData;
  }
  else if (pData && iSize)
  {
    omx_demux_packet demux_packet;
    demux_packet.dts = dts;
    demux_packet.pts = pts;
    demux_packet.size = iSize;
    demux_packet.buff = new OMX_U8[iSize];
    memcpy(demux_packet.buff, pData, iSize);
    m_demux_queue.push(demux_packet);
  }

  OMX_U8 *buffer_to_free = NULL;
  while (1)
  {
    // try to send any/all demux packets to omx decoder.
    if (!demuxer_bytes && !m_demux_queue.empty())
    {
      omx_demux_packet &demux_packet = m_demux_queue.front();
      if (m_omx_decoder.GetInputBufferSpace() >= (unsigned int)demux_packet.size)
      {
        // need to lock here to retrieve an input buffer and pop the queue
        m_demux_queue.pop();
        demuxer_bytes = (unsigned int)demux_packet.size;
        demuxer_content = demux_packet.buff;
        buffer_to_free = demux_packet.buff;
        dts = demux_packet.dts;
        pts = demux_packet.pts;
      }
    }

    if (demuxer_content)
    {
      // 500ms timeout
      OMX_BUFFERHEADERTYPE *omx_buffer = m_omx_decoder.GetInputBuffer(500);
      if (omx_buffer == NULL)
      {
        CLog::Log(LOGERROR, "%s::%s - m_omx_decoder.GetInputBuffer timeout", CLASSNAME, __func__);
        return VC_ERROR;
      }
      #if defined(OMX_DEBUG_VERBOSE)
      //CLog::Log(LOGDEBUG, "%s::%s - omx_buffer=%p", CLASSNAME, __func__, omx_buffer);
      #endif
      omx_buffer->nFlags  = 0;
      omx_buffer->nOffset = 0;

      omx_buffer->nFilledLen = (demuxer_bytes > omx_buffer->nAllocLen) ? omx_buffer->nAllocLen : demuxer_bytes;
      omx_buffer->nTimeStamp = ToOMXTime((uint64_t)(pts == DVD_NOPTS_VALUE) ? 0 : pts);
      omx_buffer->pAppPrivate = omx_buffer;
      memcpy(omx_buffer->pBuffer, demuxer_content, omx_buffer->nFilledLen);

      demuxer_bytes -= omx_buffer->nFilledLen;
      demuxer_content += omx_buffer->nFilledLen;

      if (demuxer_bytes == 0)
        omx_buffer->nFlags |= OMX_BUFFERFLAG_ENDOFFRAME;
      // openmax doesn't like an unknown timestamp as first frame
      if (pts == DVD_NOPTS_VALUE && m_startframe)
        omx_buffer->nFlags |= OMX_BUFFERFLAG_TIME_UNKNOWN;
      if (pts == DVD_NOPTS_VALUE) // hijack an omx flag to indicate there wasn't a real timestamp - it will be returned with the picture (but otherwise ignored)
        omx_buffer->nFlags |= OMX_BUFFERFLAG_PTS_INVALID;
      if (m_drop_state) // hijack an omx flag to signal this frame to be dropped - it will be returned with the picture (but otherwise ignored)
        omx_buffer->nFlags |= OMX_BUFFERFLAG_DECODEONLY | OMX_BUFFERFLAG_DROPPED;

#if defined(OMX_DEBUG_VERBOSE)
      CLog::Log(LOGDEBUG, "%s::%s - %-6d dts:%.3f pts:%.3f flags:%x",
        CLASSNAME, __func__, omx_buffer->nFilledLen, dts == DVD_NOPTS_VALUE ? 0.0 : dts*1e-6, pts == DVD_NOPTS_VALUE ? 0.0 : pts*1e-6, omx_buffer->nFlags);
#endif

      omx_err = m_omx_decoder.EmptyThisBuffer(omx_buffer);
      if (omx_err != OMX_ErrorNone)
      {
        CLog::Log(LOGERROR, "%s::%s - OMX_EmptyThisBuffer() failed with result(0x%x)", CLASSNAME, __func__, omx_err);
        return VC_ERROR;
      }
      if (demuxer_bytes == 0)
      {
        m_startframe = true;
#ifdef DTS_QUEUE
        if (!m_drop_state)
        {
          // only push if we are successful with feeding OMX_EmptyThisBuffer
          m_dts_queue.push(dts);
          assert(m_dts_queue.size() < 32);
        }
#endif
        if (buffer_to_free)
        {
          delete [] buffer_to_free;
          buffer_to_free = NULL;
          demuxer_content = NULL;
          continue;
        }
      }
    }
    omx_err = m_omx_decoder.WaitForEvent(OMX_EventPortSettingsChanged, 0);
    if (omx_err == OMX_ErrorNone)
    {
      if (!PortSettingsChanged())
      {
        CLog::Log(LOGERROR, "%s::%s - error PortSettingsChanged omx_err(0x%08x)", CLASSNAME, __func__, omx_err);
        return VC_ERROR;
      }
    }
    else if (omx_err != OMX_ErrorTimeout)
    {
      CLog::Log(LOGERROR, "%s::%s - video not supported omx_err(0x%08x)", CLASSNAME, __func__, omx_err);
      return VC_ERROR;
    }
    omx_err = m_omx_decoder.WaitForEvent(OMX_EventParamOrConfigChanged, 0);
    if (omx_err == OMX_ErrorNone)
    {
      if (!PortSettingsChanged())
      {
        CLog::Log(LOGERROR, "%s::%s - error PortSettingsChanged (EventParamOrConfigChanged) omx_err(0x%08x)", CLASSNAME, __func__, omx_err);
        return VC_ERROR;
      }
    }
    else if (omx_err == OMX_ErrorStreamCorrupt)
    {
      CLog::Log(LOGERROR, "%s::%s - video not supported 2 omx_err(0x%08x)", CLASSNAME, __func__, omx_err);
      return VC_ERROR;
    }
    if (!demuxer_bytes)
      break;
  }

#if defined(OMX_DEBUG_VERBOSE)
  if (!m_omx_decoder.GetInputBufferSpace())
    CLog::Log(LOGDEBUG,
      "%s::%s - buffering demux, m_demux_queue_size(%d), demuxer_bytes(%d) m_dts_queue.size(%d)",
      CLASSNAME, __func__, m_demux_queue.size(), demuxer_bytes, m_dts_queue.size());
  #endif

  if (m_omx_output_ready.empty())
  {
    //CLog::Log(LOGDEBUG, "%s::%s - empty: buffers:%d", CLASSNAME, __func__, m_omx_output_ready.size());
    return VC_BUFFER;
  }

  //CLog::Log(LOGDEBUG, "%s::%s -  full: buffers:%d", CLASSNAME, __func__, m_omx_output_ready.size());
  return VC_PICTURE;
}

void COpenMaxVideo::Reset(void)
{
  #if defined(OMX_DEBUG_VERBOSE)
  CLog::Log(LOGDEBUG, "%s::%s", CLASSNAME, __func__);
  #endif
  if (m_omx_egl_render.IsInitialized())
    m_omx_egl_render.FlushAll();
  if (m_omx_image_fx.IsInitialized())
    m_omx_image_fx.FlushAll();
  if (m_omx_decoder.IsInitialized())
    m_omx_decoder.FlushAll();
  // blow all ready video frames
  SetDropState(true);
  SetDropState(false);
#ifdef DTS_QUEUE
  while (!m_dts_queue.empty())
    m_dts_queue.pop();
#endif

  while (!m_demux_queue.empty())
    m_demux_queue.pop();
  m_startframe = false;
}


OMX_ERRORTYPE COpenMaxVideo::ReturnOpenMaxBuffer(COpenMaxVideoBuffer *buffer)
{
  OMX_ERRORTYPE omx_err = OMX_ErrorNone;
#if defined(OMX_DEBUG_VERBOSE)
  CLog::Log(LOGDEBUG, "%s::%s %p (%d)", CLASSNAME, __func__, buffer, m_omx_output_busy.size());
#endif
  bool done = buffer->omx_buffer->nFlags & OMX_BUFFERFLAG_EOS;
  if (!done)
  {
    // return the omx buffer back to OpenMax to fill.
    buffer->omx_buffer->nFlags = 0;
    buffer->omx_buffer->nFilledLen = 0;

    assert(buffer->omx_buffer->nOutputPortIndex == m_omx_egl_render.GetOutputPort());
#if defined(OMX_DEBUG_VERBOSE)
    CLog::Log(LOGDEBUG, "%s::%s FillThisBuffer(%p) %p->%ld", CLASSNAME, __func__, buffer, buffer->omx_buffer, buffer->m_refs);
#endif
    OMX_ERRORTYPE omx_err = m_omx_egl_render.FillThisBuffer(buffer->omx_buffer);

    if (omx_err)
      CLog::Log(LOGERROR, "%s::%s - OMX_FillThisBuffer, omx_err(0x%x)", CLASSNAME, __func__, omx_err);
  }
  return omx_err;
}

void COpenMaxVideo::ReleaseOpenMaxBuffer(COpenMaxVideoBuffer *buffer)
{
  // remove from busy list
  pthread_mutex_lock(&m_omx_output_mutex);
  m_omx_output_busy.erase(std::remove(m_omx_output_busy.begin(), m_omx_output_busy.end(), buffer), m_omx_output_busy.end());
  pthread_mutex_unlock(&m_omx_output_mutex);
  ReturnOpenMaxBuffer(buffer);
  bool done = false;
  pthread_mutex_lock(&m_omx_output_mutex);
  if (m_finished && m_omx_output_busy.empty())
    done = true;
  pthread_mutex_unlock(&m_omx_output_mutex);
  if (done)
    m_myself.reset();
}

bool COpenMaxVideo::GetPicture(DVDVideoPicture* pDvdVideoPicture)
{
  //CLog::Log(LOGDEBUG, "%s::%s - m_omx_output_busy.size()=%d m_omx_output_ready.size()=%d", CLASSNAME, __func__, m_omx_output_busy.size(), m_omx_output_ready.size());
  //CLog::Log(LOGDEBUG, "%s::%s -  full: buffers:%d", CLASSNAME, __func__, m_omx_output_ready.size());

  if (!m_omx_output_ready.empty())
  {
    COpenMaxVideoBuffer *buffer;
    // fetch a output buffer and pop it off the ready list
    pthread_mutex_lock(&m_omx_output_mutex);
    buffer = m_omx_output_ready.front();
    m_omx_output_ready.pop();
    m_omx_output_busy.push_back(buffer);
    pthread_mutex_unlock(&m_omx_output_mutex);

    memset(pDvdVideoPicture, 0, sizeof *pDvdVideoPicture);
    pDvdVideoPicture->dts = DVD_NOPTS_VALUE;
    pDvdVideoPicture->pts = DVD_NOPTS_VALUE;
    pDvdVideoPicture->format = RENDER_FMT_OMXEGL;
    pDvdVideoPicture->openMaxBuffer = buffer;
    pDvdVideoPicture->color_range  = 0;
    pDvdVideoPicture->color_matrix = 4;
    pDvdVideoPicture->iWidth  = m_decoded_width;
    pDvdVideoPicture->iHeight = m_decoded_height;
    pDvdVideoPicture->iDisplayWidth  = m_decoded_width;
    pDvdVideoPicture->iDisplayHeight = m_decoded_height;

    if (buffer->m_aspect_ratio > 0.0 && !m_forced_aspect_ratio)
    {
      pDvdVideoPicture->iDisplayWidth  = ((int)lrint(pDvdVideoPicture->iHeight * buffer->m_aspect_ratio)) & -3;
      if (pDvdVideoPicture->iDisplayWidth > pDvdVideoPicture->iWidth)
      {
        pDvdVideoPicture->iDisplayWidth  = pDvdVideoPicture->iWidth;
        pDvdVideoPicture->iDisplayHeight = ((int)lrint(pDvdVideoPicture->iWidth / buffer->m_aspect_ratio)) & -3;
      }
    }

#ifdef DTS_QUEUE
    if (!m_deinterlace_second_field)
    {
      assert(!m_dts_queue.empty());
      pDvdVideoPicture->dts = m_dts_queue.front();
      m_dts_queue.pop();
    }
    if (m_deinterlace)
      m_deinterlace_second_field = !m_deinterlace_second_field;
#endif
    // nTimeStamp is in microseconds
    pDvdVideoPicture->pts = FromOMXTime(buffer->omx_buffer->nTimeStamp);
    pDvdVideoPicture->openMaxBuffer->Acquire();
    pDvdVideoPicture->iFlags  = DVP_FLAG_ALLOCATED;
    if (buffer->omx_buffer->nFlags & OMX_BUFFERFLAG_PTS_INVALID)
      pDvdVideoPicture->pts = DVD_NOPTS_VALUE;
#if defined(OMX_DEBUG_VERBOSE)
    CLog::Log(LOGINFO, "%s::%s dts:%.3f pts:%.3f flags:%x:%x openMaxBuffer:%p omx_buffer:%p egl_image:%p texture_id:%x", CLASSNAME, __func__,
        pDvdVideoPicture->dts == DVD_NOPTS_VALUE ? 0.0 : pDvdVideoPicture->dts*1e-6, pDvdVideoPicture->pts == DVD_NOPTS_VALUE ? 0.0 : pDvdVideoPicture->pts*1e-6,
        pDvdVideoPicture->iFlags, buffer->omx_buffer->nFlags, pDvdVideoPicture->openMaxBuffer, pDvdVideoPicture->openMaxBuffer->omx_buffer, pDvdVideoPicture->openMaxBuffer->egl_image, pDvdVideoPicture->openMaxBuffer->texture_id);
#endif
    assert(!(buffer->omx_buffer->nFlags & (OMX_BUFFERFLAG_DECODEONLY | OMX_BUFFERFLAG_DROPPED)));
  }
  else
  {
    CLog::Log(LOGERROR, "%s::%s - called but m_omx_output_ready is empty", CLASSNAME, __func__);
    return false;
  }
  return true;
}

bool COpenMaxVideo::ClearPicture(DVDVideoPicture* pDvdVideoPicture)
{
#if defined(OMX_DEBUG_VERBOSE)
  CLog::Log(LOGDEBUG, "%s::%s - %p", CLASSNAME, __func__, pDvdVideoPicture->openMaxBuffer);
#endif
  if (pDvdVideoPicture->format == RENDER_FMT_OMXEGL)
    pDvdVideoPicture->openMaxBuffer->Release();
  memset(pDvdVideoPicture, 0, sizeof *pDvdVideoPicture);
  return true;
}

  // DecoderFillBufferDone -- OpenMax output buffer has been filled
OMX_ERRORTYPE COpenMaxVideo::DecoderFillBufferDone(
  OMX_HANDLETYPE hComponent,
  OMX_BUFFERHEADERTYPE* pBuffer)
{
  COpenMaxVideoBuffer *buffer = (COpenMaxVideoBuffer*)pBuffer->pAppPrivate;

  #if defined(OMX_DEBUG_VERBOSE)
  CLog::Log(LOGDEBUG, "%s::%s - %p (%p,%p) buffer_size(%u), pts:%.3f flags:%x",
    CLASSNAME, __func__, buffer, pBuffer, buffer->omx_buffer, pBuffer->nFilledLen, (double)FromOMXTime(buffer->omx_buffer->nTimeStamp)*1e-6, buffer->omx_buffer->nFlags);
  #endif

  assert(!(buffer->omx_buffer->nFlags & (OMX_BUFFERFLAG_DECODEONLY | OMX_BUFFERFLAG_DROPPED)));
  // queue output omx buffer to ready list.
  pthread_mutex_lock(&m_omx_output_mutex);
  buffer->m_aspect_ratio = m_aspect_ratio;
  m_omx_output_ready.push(buffer);
  pthread_mutex_unlock(&m_omx_output_mutex);

  return OMX_ErrorNone;
}

OMX_ERRORTYPE COpenMaxVideo::PrimeFillBuffers(void)
{
  OMX_ERRORTYPE omx_err = OMX_ErrorNone;
  COpenMaxVideoBuffer *buffer;

  #if defined(OMX_DEBUG_VERBOSE)
  CLog::Log(LOGDEBUG, "%s::%s", CLASSNAME, __func__);
  #endif
  // tell OpenMax to start filling output buffers
  for (size_t i = 0; i < m_omx_output_buffers.size(); i++)
  {
    buffer = m_omx_output_buffers[i];
    // always set the port index.
    buffer->omx_buffer->nOutputPortIndex = m_omx_egl_render.GetOutputPort();
    buffer->omx_buffer->pAppPrivate = buffer;
    omx_err = ReturnOpenMaxBuffer(buffer);
  }
  return omx_err;
}

OMX_ERRORTYPE COpenMaxVideo::FreeOMXInputBuffers(void)
{
  OMX_ERRORTYPE omx_err = OMX_ErrorNone;

  // empty input buffer queue. not decoding so don't need lock/unlock.
  while (!m_demux_queue.empty())
    m_demux_queue.pop();
#ifdef DTS_QUEUE
  while (!m_dts_queue.empty())
    m_dts_queue.pop();
#endif
  return(omx_err);
}

bool COpenMaxVideo::CallbackAllocOMXEGLTextures(EGLDisplay egl_display, EGLContext egl_context, void *userdata)
{
  COpenMaxVideo *omx = static_cast<COpenMaxVideo*>(userdata);
  return omx->AllocOMXOutputEGLTextures(egl_display, egl_context) == OMX_ErrorNone;
}

bool COpenMaxVideo::CallbackFreeOMXEGLTextures(EGLDisplay egl_display, EGLContext egl_context, void *userdata)
{
  COpenMaxVideo *omx = static_cast<COpenMaxVideo*>(userdata);
  return omx->FreeOMXOutputEGLTextures(egl_display, egl_context) == OMX_ErrorNone;
}

bool COpenMaxVideo::AllocOMXOutputBuffers(void)
{
  pthread_mutex_lock(&m_omx_output_mutex);
  for (size_t i = 0; i < m_egl_buffer_count; i++)
  {
    COpenMaxVideoBuffer *egl_buffer = new COpenMaxVideoBuffer(this);
    egl_buffer->width  = m_decoded_width;
    egl_buffer->height = m_decoded_height;
    egl_buffer->index = i;
    m_omx_output_buffers.push_back(egl_buffer);
  }
  bool ret = g_OMXImage.SendMessage(CallbackAllocOMXEGLTextures, (void *)this);
  pthread_mutex_unlock(&m_omx_output_mutex);
  return ret;
}

bool COpenMaxVideo::FreeOMXOutputBuffers(void)
{
  pthread_mutex_lock(&m_omx_output_mutex);
  bool ret = g_OMXImage.SendMessage(CallbackFreeOMXEGLTextures, (void *)this);

  for (size_t i = 0; i < m_omx_output_buffers.size(); i++)
  {
    COpenMaxVideoBuffer *egl_buffer = m_omx_output_buffers[i];
    delete egl_buffer;
  }

  m_omx_output_buffers.clear();
  pthread_mutex_unlock(&m_omx_output_mutex);
  return ret;
}

OMX_ERRORTYPE COpenMaxVideo::AllocOMXOutputEGLTextures(EGLDisplay egl_display, EGLContext egl_context)
{
  OMX_ERRORTYPE omx_err = OMX_ErrorNone;
  EGLint attrib = EGL_NONE;

  glActiveTexture(GL_TEXTURE0);

  for (size_t i = 0; i < m_egl_buffer_count; i++)
  {
    COpenMaxVideoBuffer *egl_buffer = m_omx_output_buffers[i];

    glGenTextures(1, &egl_buffer->texture_id);
    glBindTexture(GL_TEXTURE_2D, egl_buffer->texture_id);

    // no mipmaps
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);

    // create space for buffer with a texture
    glTexImage2D(
      GL_TEXTURE_2D,      // target
      0,                  // level
      GL_RGBA,            // internal format
      m_decoded_width,    // width
      m_decoded_height,   // height
      0,                  // border
      GL_RGBA,            // format
      GL_UNSIGNED_BYTE,   // type
      NULL);              // pixels -- will be provided later

    // create EGLImage from texture
    egl_buffer->egl_image = eglCreateImageKHR(
      egl_display,
      egl_context,
      EGL_GL_TEXTURE_2D_KHR,
      (EGLClientBuffer)(egl_buffer->texture_id),
      &attrib);
    if (!egl_buffer->egl_image)
    {
      CLog::Log(LOGERROR, "%s::%s - ERROR creating EglImage", CLASSNAME, __func__);
      return(OMX_ErrorUndefined);
    }

    // tell decoder output port that it will be using EGLImage
    omx_err = m_omx_egl_render.UseEGLImage(
      &egl_buffer->omx_buffer, m_omx_egl_render.GetOutputPort(), egl_buffer, egl_buffer->egl_image);
    if (omx_err)
    {
      CLog::Log(LOGERROR, "%s::%s - OMX_UseEGLImage failed with omx_err(0x%x)",
        CLASSNAME, __func__, omx_err);
      return(omx_err);
    }

    CLog::Log(LOGDEBUG, "%s::%s - Texture %p Width %d Height %d",
      CLASSNAME, __func__, egl_buffer->egl_image, egl_buffer->width, egl_buffer->height);
  }
  return omx_err;
}

OMX_ERRORTYPE COpenMaxVideo::FreeOMXOutputEGLTextures(EGLDisplay egl_display, EGLContext egl_context)
{
  OMX_ERRORTYPE omx_err = OMX_ErrorNone;

  for (size_t i = 0; i < m_omx_output_buffers.size(); i++)
  {
    COpenMaxVideoBuffer *egl_buffer = m_omx_output_buffers[i];
    // tell decoder output port to stop using the EGLImage
    omx_err = m_omx_egl_render.FreeOutputBuffer(egl_buffer->omx_buffer);
    if (omx_err != OMX_ErrorNone)
      CLog::Log(LOGERROR, "%s::%s m_omx_egl_render.FreeOutputBuffer(%p) omx_err(0x%08x)", CLASSNAME, __func__, egl_buffer->omx_buffer, omx_err);
    // destroy egl_image
    eglDestroyImageKHR(egl_display, egl_buffer->egl_image);
    // free texture
    glDeleteTextures(1, &egl_buffer->texture_id);
  }
  return omx_err;
}


// StopPlayback -- Stop video playback
OMX_ERRORTYPE COpenMaxVideo::StopDecoder(void)
{
  OMX_ERRORTYPE omx_err = OMX_ErrorNone;

  #if defined(OMX_DEBUG_VERBOSE)
  CLog::Log(LOGDEBUG, "%s::%s", CLASSNAME, __func__);
  #endif

  // transition decoder component from executing to idle
  if (m_omx_decoder.IsInitialized())
  {
    omx_err = m_omx_decoder.SetStateForComponent(OMX_StateIdle);
    if (omx_err)
      CLog::Log(LOGERROR, "%s::%s - setting OMX_StateIdle failed with omx_err(0x%x)",
        CLASSNAME, __func__, omx_err);
  }

  // we can free our allocated port buffers in OMX_StateIdle state.
  // free OpenMax input buffers.
  FreeOMXInputBuffers();

  if (m_omx_egl_render.IsInitialized())
  {
      omx_err = m_omx_egl_render.SetStateForComponent(OMX_StateIdle);
      if (omx_err)
        CLog::Log(LOGERROR, "%s::%s - setting egl OMX_StateIdle failed with omx_err(0x%x)",
          CLASSNAME, __func__, omx_err);
      // free OpenMax output buffers.
      omx_err = m_omx_egl_render.DisablePort(m_omx_egl_render.GetOutputPort(), false);
      if (omx_err != OMX_ErrorNone)
        CLog::Log(LOGERROR, "%s::%s m_omx_egl_render.DisablePort(%d) omx_err(0x%08x)", CLASSNAME, __func__, m_omx_egl_render.GetOutputPort(), omx_err);

      FreeOMXOutputBuffers();

      omx_err = m_omx_egl_render.WaitForCommand(OMX_CommandPortDisable, m_omx_egl_render.GetOutputPort());
      if (omx_err != OMX_ErrorNone)
        CLog::Log(LOGERROR, "%s::%s WaitForCommand:OMX_CommandPortDisable omx_err(0x%08x)", CLASSNAME, __func__, omx_err);
  }
  return omx_err;
}

