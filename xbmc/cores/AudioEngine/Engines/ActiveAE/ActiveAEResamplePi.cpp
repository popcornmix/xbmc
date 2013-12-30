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

#include "system.h"

#if defined(TARGET_RASPBERRY_PI)

#include "ActiveAEResample.h"
#include "linux/RBP.h"
#include "settings/Settings.h"
#include "utils/log.h"

extern "C" {
#include "libavutil/channel_layout.h"
#include "libavutil/opt.h"
#include "libswresample/swresample.h"
}

//#define DEBUG_VERBOSE

#define CLASSNAME "CActiveAEResamplePi"

#define BUFFERSIZE (32*1024*2*8)

//#define BENCHMARKING
#ifdef BENCHMARKING
#define LOGTIMEINIT(f) \
  struct timespec now; \
  uint64_t  Start, End; \
  clock_gettime(CLOCK_MONOTONIC, &now); \
  Start = ((int64_t)now.tv_sec * 1000000000L) + now.tv_nsec; \
  const char *_filename = f;

#define LOGTIME(n) \
  clock_gettime(CLOCK_MONOTONIC, &now); \
  End = ((int64_t)now.tv_sec * 1000000000L) + now.tv_nsec; \
  CLog::Log(LOGNOTICE, "ActiveAE::%s %d - resample %s took %.0fms", __FUNCTION__, n, _filename, (End-Start)*1e-6); \
  Start=End;
#else
#define LOGTIMEINIT(f)
#define LOGTIME(n)
#endif

using namespace ActiveAE;

CActiveAEResample::CActiveAEResample()
{
  CLog::Log(LOGINFO, "%s::%s", CLASSNAME, __func__);

  m_Initialized = false;
  m_last_src_fmt = AV_SAMPLE_FMT_NONE;
  m_last_dst_fmt = AV_SAMPLE_FMT_NONE;
  m_last_src_channels = 0;
  m_last_dst_channels = 0;
  m_encoded_buffer = NULL;
}

CActiveAEResample::~CActiveAEResample()
{
  CLog::Log(LOGINFO, "%s::%s", CLASSNAME, __func__);
  DeInit();
}

void CActiveAEResample::DeInit()
{
  CLog::Log(LOGDEBUG, "%s:%s", CLASSNAME, __func__);
  if (m_Initialized)
  {
    m_omx_mixer.FlushAll();
    m_omx_mixer.Deinitialize();
    m_Initialized = false;
  }
}

static int format_to_bits(AVSampleFormat fmt)
{
  switch (fmt)
  {
  case AV_SAMPLE_FMT_U8:
  case AV_SAMPLE_FMT_U8P:
    return 8;
  case AV_SAMPLE_FMT_S16:
  case AV_SAMPLE_FMT_S16P:
    return 16;
  case AV_SAMPLE_FMT_S32:
  case AV_SAMPLE_FMT_S32P:
  case AV_SAMPLE_FMT_FLT:
  case AV_SAMPLE_FMT_FLTP:
    return 32;
  default:
    assert(0);
  }
  return 0;
}

bool CActiveAEResample::Init(uint64_t dst_chan_layout, int dst_channels, int dst_rate, AVSampleFormat dst_fmt, int dst_bits, int dst_dither, uint64_t src_chan_layout, int src_channels, int src_rate, AVSampleFormat src_fmt, int src_bits, int src_dither, bool upmix, bool normalize, CAEChannelInfo *remapLayout, AEQuality quality)
{
  LOGTIMEINIT("x");

  CLog::Log(LOGINFO, "%s::%s remap:%p chan:%d->%d rate:%d->%d format:%d->%d bits:%d->%d norm:%d upmix:%d", CLASSNAME, __func__, remapLayout, src_channels, dst_channels, src_rate, dst_rate, src_fmt, dst_fmt, src_bits, dst_bits, normalize, upmix);

  // replace passed in number of bits with correct ones
  src_bits = format_to_bits(src_fmt);
  dst_bits = format_to_bits(dst_fmt);

  m_dst_chan_layout = dst_chan_layout;
  m_dst_channels = dst_channels;
  m_dst_rate = dst_rate;
  m_dst_fmt = dst_fmt;
  m_dst_bits = dst_bits;
  m_src_chan_layout = src_chan_layout;
  m_src_channels = src_channels;
  m_src_rate = src_rate;
  m_src_fmt = src_fmt;
  m_src_bits = src_bits;

  if (m_dst_chan_layout == 0)
    m_dst_chan_layout = av_get_default_channel_layout(m_dst_channels);
  if (m_src_chan_layout == 0)
    m_src_chan_layout = av_get_default_channel_layout(m_src_channels);

  OMX_CONFIG_BRCMAUDIODOWNMIXCOEFFICIENTS8x8 mix;
  OMX_INIT_STRUCTURE(mix);

  assert(sizeof(mix.coeff)/sizeof(mix.coeff[0]) == 64);

  LOGTIME(1);
// this code is just uses ffmpeg to produce the 8x8 mixing matrix
{
  // dummy sample rate and format, as we only care about channel mapping
  SwrContext *m_pContext = swr_alloc_set_opts(NULL, m_dst_chan_layout, AV_SAMPLE_FMT_FLT, 48000,
                                                        m_src_chan_layout, AV_SAMPLE_FMT_FLT, 48000, 0, NULL);
  if (!m_pContext)
  {
    CLog::Log(LOGERROR, "CActiveAEResample::Init - create context failed");
    return false;
  }
  // tell resampler to clamp float values
  // not required for sink stage (remapLayout == true)
  if (!remapLayout && normalize)
  {
    av_opt_set_double(m_pContext, "rematrix_maxval", 1.0, 0);
  }

  if (remapLayout)
  {
    // one-to-one mapping of channels
    // remapLayout is the layout of the sink, if the channel is in our src layout
    // the channel is mapped by setting coef 1.0
    double m_rematrix[AE_CH_MAX][AE_CH_MAX];
    memset(m_rematrix, 0, sizeof(m_rematrix));
    m_dst_chan_layout = 0;
    for (unsigned int out=0; out<remapLayout->Count(); out++)
    {
      m_dst_chan_layout += (uint64_t) (1 << out);
      int idx = GetAVChannelIndex((*remapLayout)[out], m_src_chan_layout);
      if (idx >= 0)
      {
        m_rematrix[out][idx] = 1.0;
      }
    }

    av_opt_set_int(m_pContext, "out_channel_count", m_dst_channels, 0);
    av_opt_set_int(m_pContext, "out_channel_layout", m_dst_chan_layout, 0);

    if (swr_set_matrix(m_pContext, (const double*)m_rematrix, AE_CH_MAX) < 0)
    {
      CLog::Log(LOGERROR, "CActiveAEResample::Init - setting channel matrix failed");
      return false;
    }
  }
  // stereo upmix
  else if (upmix && m_src_channels == 2 && m_dst_channels > 2)
  {
    double m_rematrix[AE_CH_MAX][AE_CH_MAX];
    memset(m_rematrix, 0, sizeof(m_rematrix));
    for (int out=0; out<m_dst_channels; out++)
    {
      uint64_t out_chan = av_channel_layout_extract_channel(m_dst_chan_layout, out);
      switch(out_chan)
      {
        case AV_CH_FRONT_LEFT:
        case AV_CH_BACK_LEFT:
        case AV_CH_SIDE_LEFT:
          m_rematrix[out][0] = 1.0;
          break;
        case AV_CH_FRONT_RIGHT:
        case AV_CH_BACK_RIGHT:
        case AV_CH_SIDE_RIGHT:
          m_rematrix[out][1] = 1.0;
          break;
        case AV_CH_FRONT_CENTER:
          m_rematrix[out][0] = 0.5;
          m_rematrix[out][1] = 0.5;
          break;
        case AV_CH_LOW_FREQUENCY:
          m_rematrix[out][0] = 0.5;
          m_rematrix[out][1] = 0.5;
          break;
        default:
          break;
      }
    }

    if (swr_set_matrix(m_pContext, (const double*)m_rematrix, AE_CH_MAX) < 0)
    {
      CLog::Log(LOGERROR, "CActiveAEResample::Init - setting channel matrix failed");
      return false;
    }
  }

  if (swr_init(m_pContext) < 0)
  {
    CLog::Log(LOGERROR, "CActiveAEResample::Init - init resampler failed");
    return false;
  }

  const int samples = 8;
  uint8_t *output, *input;
  av_samples_alloc(&output, NULL, m_dst_channels, samples, AV_SAMPLE_FMT_FLT, 1);
  av_samples_alloc(&input , NULL, m_src_channels, samples, AV_SAMPLE_FMT_FLT, 1);

  // Produce "identity" samples
  float *f = (float *)input;
  for (int j=0; j < samples; j++)
    for (int i=0; i < m_src_channels; i++)
      *f++ = i == j ? 1.0f : 0.0f;

  int ret = swr_convert(m_pContext, &output, samples, (const uint8_t **)&input, samples);
  if (ret < 0)
    CLog::Log(LOGERROR, "CActiveAEResample::Resample - resample failed");

  f = (float *)output;
  for (int j=0; j < samples; j++)
    for (int i=0; i < m_dst_channels; i++)
      mix.coeff[8*i+j] = *f++ * (1<<16);

  for (int j=0; j < 8; j++)
  {
    char s[128] = {}, *t=s;
    for (int i=0; i < 8; i++)
      t += sprintf(t, "% 6.2f ", mix.coeff[j*8+i] * (1.0/0x10000));
    CLog::Log(LOGINFO, "%s::%s  %s", CLASSNAME, __func__, s);
  }
  av_freep(&input);
  av_freep(&output);
  swr_free(&m_pContext);
}
  LOGTIME(2);

  // This may be called before Application calls g_RBP.Initialise, so call it here too
  g_RBP.Initialize();

  OMX_ERRORTYPE omx_err   = OMX_ErrorNone;

  if (!m_omx_mixer.Initialize("OMX.broadcom.audio_mixer", OMX_IndexParamAudioInit))
    CLog::Log(LOGERROR, "%s::%s - m_omx_mixer.Initialize omx_err(0x%08x)", CLASSNAME, __func__, omx_err);

  LOGTIME(3);

  // audio_mixer only supports up to 192kHz, however as long as ratio of samplerates remains the same we can lie
  while (src_rate > 192000 || dst_rate > 192000)
    src_rate >>= 1, dst_rate >>= 1;

  OMX_INIT_STRUCTURE(m_pcm_input);
  m_pcm_input.nPortIndex            = m_omx_mixer.GetInputPort();
  m_pcm_input.eNumData              = OMX_NumericalDataSigned;
  m_pcm_input.eEndian               = OMX_EndianLittle;
  m_pcm_input.bInterleaved          = OMX_TRUE;
  m_pcm_input.nBitPerSample         = m_src_bits;
  // 0x8000 = float, 0x10000 = planar
  uint32_t flags = 0;
  if (m_src_fmt == AV_SAMPLE_FMT_FLT || m_src_fmt == AV_SAMPLE_FMT_FLTP)
   flags |= 0x8000;
  if (m_src_fmt >= AV_SAMPLE_FMT_U8P)
   flags |= 0x10000;
  m_pcm_input.ePCMMode              = flags == 0 ? OMX_AUDIO_PCMModeLinear : (OMX_AUDIO_PCMMODETYPE)flags;
  m_pcm_input.nChannels             = src_channels;
  m_pcm_input.nSamplingRate         = src_rate;

  omx_err = m_omx_mixer.SetParameter(OMX_IndexParamAudioPcm, &m_pcm_input);
  if (omx_err != OMX_ErrorNone)
    CLog::Log(LOGERROR, "%s::%s - error m_omx_mixer in SetParameter omx_err(0x%08x)", CLASSNAME, __func__, omx_err);

  OMX_INIT_STRUCTURE(m_pcm_output);
  m_pcm_output.nPortIndex            = m_omx_mixer.GetOutputPort();
  m_pcm_output.eNumData              = OMX_NumericalDataSigned;
  m_pcm_output.eEndian               = OMX_EndianLittle;
  m_pcm_output.bInterleaved          = OMX_TRUE;
  m_pcm_output.nBitPerSample         = m_dst_bits;
  flags = 0;
  if (m_dst_fmt == AV_SAMPLE_FMT_FLT || m_dst_fmt == AV_SAMPLE_FMT_FLTP)
   flags |= 0x8000;
  if (m_dst_fmt >= AV_SAMPLE_FMT_U8P)
   flags |= 0x10000;
  m_pcm_output.ePCMMode              = flags == 0 ? OMX_AUDIO_PCMModeLinear : (OMX_AUDIO_PCMMODETYPE)flags;
  m_pcm_output.nChannels             = dst_channels;
  m_pcm_output.nSamplingRate         = dst_rate;

  omx_err = m_omx_mixer.SetParameter(OMX_IndexParamAudioPcm, &m_pcm_output);
  if (omx_err != OMX_ErrorNone)
    CLog::Log(LOGERROR, "%s::%s - error m_omx_mixer out SetParameter omx_err(0x%08x)", CLASSNAME, __func__, omx_err);

  LOGTIME(4);

  mix.nPortIndex = m_omx_mixer.GetInputPort();
  omx_err = m_omx_mixer.SetConfig(OMX_IndexConfigBrcmAudioDownmixCoefficients8x8, &mix);
  if (omx_err != OMX_ErrorNone)
  {
    CLog::Log(LOGERROR, "%s::%s - error setting mixer OMX_IndexConfigBrcmAudioDownmixCoefficients, error 0x%08x\n",
              CLASSNAME, __func__, omx_err);
    return false;
  }

  // set up the number/size of buffers for decoder input
  OMX_PARAM_PORTDEFINITIONTYPE port_param;
  OMX_INIT_STRUCTURE(port_param);
  port_param.nPortIndex = m_omx_mixer.GetInputPort();

  omx_err = m_omx_mixer.GetParameter(OMX_IndexParamPortDefinition, &port_param);
  if (omx_err != OMX_ErrorNone)
    CLog::Log(LOGERROR, "%s:%s - error get OMX_IndexParamPortDefinition (input) omx_err(0x%08x)", CLASSNAME, __func__, omx_err);

  port_param.nBufferCountActual = std::max((unsigned int)port_param.nBufferCountMin, (unsigned int)1);
  port_param.nBufferSize = BUFFERSIZE;

  omx_err = m_omx_mixer.SetParameter(OMX_IndexParamPortDefinition, &port_param);
  if (omx_err != OMX_ErrorNone)
    CLog::Log(LOGERROR, "%s:%s - error set OMX_IndexParamPortDefinition (input) omx_err(0x%08x)", CLASSNAME, __func__, omx_err);

  LOGTIME(5);

  omx_err = m_omx_mixer.AllocInputBuffers();
  if (omx_err != OMX_ErrorNone)
    CLog::Log(LOGERROR, "%s:%s - Error alloc buffers 0x%08x", CLASSNAME, __func__, omx_err);

  LOGTIME(6);

  // set up the number/size of buffers for decoder output
  OMX_INIT_STRUCTURE(port_param);
  port_param.nPortIndex = m_omx_mixer.GetOutputPort();

  omx_err = m_omx_mixer.GetParameter(OMX_IndexParamPortDefinition, &port_param);
  if (omx_err != OMX_ErrorNone)
    CLog::Log(LOGERROR, "%s:%s - error get OMX_IndexParamPortDefinition (input) omx_err(0x%08x)", CLASSNAME, __func__, omx_err);

  port_param.nBufferCountActual = std::max((unsigned int)port_param.nBufferCountMin, (unsigned int)1);
  port_param.nBufferSize = BUFFERSIZE;

  omx_err = m_omx_mixer.SetParameter(OMX_IndexParamPortDefinition, &port_param);
  if (omx_err != OMX_ErrorNone)
    CLog::Log(LOGERROR, "%s:%s - error set OMX_IndexParamPortDefinition (input) omx_err(0x%08x)", CLASSNAME, __func__, omx_err);

  LOGTIME(7);

  omx_err = m_omx_mixer.AllocOutputBuffers();
  if (omx_err != OMX_ErrorNone)
    CLog::Log(LOGERROR, "%s:%s - Error alloc buffers 0x%08x", CLASSNAME, __func__, omx_err);

  LOGTIME(8);

  omx_err = m_omx_mixer.SetStateForComponent(OMX_StateExecuting);
  if (omx_err != OMX_ErrorNone)
    CLog::Log(LOGERROR, "%s:%s - m_omx_mixer OMX_StateExecuting omx_err(0x%08x)", CLASSNAME, __func__, omx_err);

  LOGTIME(9);

  m_Initialized = true;

  return true;
}


static void copy_planes(uint8_t **dst_buffer, int d_pitch, int d_planes, int d_samplesize, int offset, uint8_t *src_buffer, int src_samples)
{
  int planesize = src_samples * d_samplesize / d_planes;
  for (int i=0; i < d_planes; i++)
    memcpy(dst_buffer[i] + offset * d_pitch, src_buffer + i * planesize, planesize);
}

int CActiveAEResample::Resample(uint8_t **dst_buffer, int dst_samples, uint8_t **src_buffer, int src_samples, double ratio)
{
  #ifdef DEBUG_VERBOSE
  CLog::Log(LOGINFO, "%s::%s samples:%d->%d (%.2f)", CLASSNAME, __func__, src_samples, dst_samples, ratio);
  #endif
  if (!m_Initialized)
    return 0;
  OMX_ERRORTYPE omx_err   = OMX_ErrorNone;

  const int s_planes = m_src_fmt >= AV_SAMPLE_FMT_U8P ? m_src_channels : 1;
  const int d_planes = m_dst_fmt >= AV_SAMPLE_FMT_U8P ? m_dst_channels : 1;
  const int s_chans  = m_src_fmt >= AV_SAMPLE_FMT_U8P ? 1 : m_src_channels;
  const int d_chans  = m_dst_fmt >= AV_SAMPLE_FMT_U8P ? 1 : m_dst_channels;
  const int s_pitch = s_chans * m_src_bits >> 3;
  const int d_pitch = d_chans * m_dst_bits >> 3;

  const int s_samplesize = m_src_channels * m_src_bits >> 3;
  const int d_samplesize = m_dst_channels * m_dst_bits >> 3;
  const int max_src_samples = BUFFERSIZE / s_samplesize;
  const int max_dst_samples = (long long)(BUFFERSIZE / d_samplesize) * m_src_rate / (m_dst_rate + m_src_rate-1);

  int sent = 0;
  int received = 0;

  if (m_encoded_buffer && m_encoded_buffer->nFilledLen)
  {
    int samples_available = m_encoded_buffer->nFilledLen / d_samplesize - m_encoded_buffer->nOffset;
    int samples = std::min(samples_available, dst_samples - received);
    copy_planes(dst_buffer, d_pitch, d_planes, d_samplesize, received, (uint8_t *)m_encoded_buffer->pBuffer + m_encoded_buffer->nOffset * d_pitch, samples);
    received += samples;
    samples_available -= samples;
    if (samples_available > 0)
      m_encoded_buffer->nOffset += samples;
    else
      m_encoded_buffer = NULL;
  }
  assert(!m_encoded_buffer);
  while (sent < src_samples)
  {
    OMX_BUFFERHEADERTYPE *omx_buffer = NULL;

    omx_buffer = m_omx_mixer.GetInputBuffer(1000);
    if (omx_buffer == NULL)
      return false;

    int send = std::min(std::min(max_dst_samples, max_src_samples), src_samples - sent);

    omx_buffer->nOffset = 0;
    omx_buffer->nFlags = OMX_BUFFERFLAG_EOS;
    omx_buffer->nFilledLen = send * s_samplesize;

    assert(omx_buffer->nFilledLen > 0 && omx_buffer->nFilledLen <= omx_buffer->nAllocLen);

    if (omx_buffer->nFilledLen)
    {
      int planesize = omx_buffer->nFilledLen / s_planes;
      for (int i=0; i < s_planes; i++)
        memcpy((uint8_t *)omx_buffer->pBuffer + i * planesize, src_buffer[i] + sent * s_pitch, planesize);
      sent += send;
    }

    omx_err = m_omx_mixer.EmptyThisBuffer(omx_buffer);
    if (omx_err != OMX_ErrorNone)
    {
      CLog::Log(LOGERROR, "%s::%s OMX_EmptyThisBuffer() failed with result(0x%x)", CLASSNAME, __func__, omx_err);
      return false;
    }

    m_encoded_buffer = m_omx_mixer.GetOutputBuffer();

    if (!m_encoded_buffer)
    {
      CLog::Log(LOGERROR, "%s::%s no output buffer", CLASSNAME, __func__);
      return false;
    }

    omx_err = m_omx_mixer.FillThisBuffer(m_encoded_buffer);
    if (omx_err != OMX_ErrorNone)
      return false;

    omx_err = m_omx_mixer.WaitForOutputDone(1000);
    if (omx_err != OMX_ErrorNone)
    {
      CLog::Log(LOGERROR, "%s::%s m_omx_mixer.WaitForOutputDone result(0x%x)", CLASSNAME, __func__, omx_err);
      return false;
    }
    assert(m_encoded_buffer->nFilledLen > 0 && m_encoded_buffer->nFilledLen <= m_encoded_buffer->nAllocLen);

    if (m_omx_mixer.BadState())
    {
      CLog::Log(LOGERROR, "%s::%s m_omx_mixer.BadState", CLASSNAME, __func__);
      return false;
    }

    if (m_encoded_buffer->nFilledLen)
    {
      int samples_available = m_encoded_buffer->nFilledLen / d_samplesize;
      int samples = std::min(samples_available, dst_samples - received);
      copy_planes(dst_buffer, d_pitch, d_planes, d_samplesize, received, (uint8_t *)m_encoded_buffer->pBuffer, samples);
      received += samples;
      samples_available -= samples;
      if (samples_available > 0)
        m_encoded_buffer->nOffset += samples;
      else
        m_encoded_buffer = NULL;
    }
  }
  #ifdef DEBUG_VERBOSE
  CLog::Log(LOGINFO, "%s::%s format:%d->%d rate:%d->%d chan:%d->%d samples %d->%d (%f) %d =%d", CLASSNAME, __func__,
    (int)m_src_fmt, (int)m_dst_fmt, m_src_rate, m_dst_rate, m_src_channels, m_dst_channels, src_samples, dst_samples, ratio, m_Initialized, received);
  #endif
  assert(received <= dst_samples);
  return received;
}

int64_t CActiveAEResample::GetDelay(int64_t base)
{
  int ret = m_dst_rate ? 1000 * GetBufferedSamples() / m_dst_rate : 0;
  #ifdef DEBUG_VERBOSE
  CLog::Log(LOGINFO, "%s::%s = %d", CLASSNAME, __func__, ret);
  #endif
  return ret;
}

int CActiveAEResample::GetBufferedSamples()
{
  int samples = 0;
  if (m_encoded_buffer)
  {
    const int d_samplesize = m_dst_channels * m_dst_bits >> 3;
    samples = m_encoded_buffer->nFilledLen / d_samplesize - m_encoded_buffer->nOffset;
  }
  #ifdef DEBUG_VERBOSE
  CLog::Log(LOGINFO, "%s::%s = %d", CLASSNAME, __func__, samples);
  #endif
  return samples;
}

int CActiveAEResample::CalcDstSampleCount(int src_samples, int dst_rate, int src_rate)
{
  int ret = ((long long)src_samples * dst_rate + src_rate-1) / src_rate;
  #ifdef DEBUG_VERBOSE
  CLog::Log(LOGINFO, "%s::%s = %d", CLASSNAME, __func__, ret);
  #endif
  return ret;
}

int CActiveAEResample::GetSrcBufferSize(int samples)
{
  int ret = 0;
  #ifdef DEBUG_VERBOSE
  CLog::Log(LOGINFO, "%s::%s = %d", CLASSNAME, __func__, ret);
  #endif
  return ret;
}

int CActiveAEResample::GetDstBufferSize(int samples)
{
  int ret = CalcDstSampleCount(samples, m_dst_rate, m_src_rate);
  #ifdef DEBUG_VERBOSE
  CLog::Log(LOGINFO, "%s::%s = %d", CLASSNAME, __func__, ret);
  #endif
  return ret;
}

uint64_t CActiveAEResample::GetAVChannelLayout(CAEChannelInfo &info)
{
  #ifdef DEBUG_VERBOSE
  CLog::Log(LOGINFO, "%s::%s", CLASSNAME, __func__);
  #endif
  uint64_t channelLayout = 0;
  if (info.HasChannel(AE_CH_FL))   channelLayout |= AV_CH_FRONT_LEFT;
  if (info.HasChannel(AE_CH_FR))   channelLayout |= AV_CH_FRONT_RIGHT;
  if (info.HasChannel(AE_CH_FC))   channelLayout |= AV_CH_FRONT_CENTER;
  if (info.HasChannel(AE_CH_LFE))  channelLayout |= AV_CH_LOW_FREQUENCY;
  if (info.HasChannel(AE_CH_BL))   channelLayout |= AV_CH_BACK_LEFT;
  if (info.HasChannel(AE_CH_BR))   channelLayout |= AV_CH_BACK_RIGHT;
  if (info.HasChannel(AE_CH_FLOC)) channelLayout |= AV_CH_FRONT_LEFT_OF_CENTER;
  if (info.HasChannel(AE_CH_FROC)) channelLayout |= AV_CH_FRONT_RIGHT_OF_CENTER;
  if (info.HasChannel(AE_CH_BC))   channelLayout |= AV_CH_BACK_CENTER;
  if (info.HasChannel(AE_CH_SL))   channelLayout |= AV_CH_SIDE_LEFT;
  if (info.HasChannel(AE_CH_SR))   channelLayout |= AV_CH_SIDE_RIGHT;
  if (info.HasChannel(AE_CH_TC))   channelLayout |= AV_CH_TOP_CENTER;
  if (info.HasChannel(AE_CH_TFL))  channelLayout |= AV_CH_TOP_FRONT_LEFT;
  if (info.HasChannel(AE_CH_TFC))  channelLayout |= AV_CH_TOP_FRONT_CENTER;
  if (info.HasChannel(AE_CH_TFR))  channelLayout |= AV_CH_TOP_FRONT_RIGHT;
  if (info.HasChannel(AE_CH_TBL))   channelLayout |= AV_CH_TOP_BACK_LEFT;
  if (info.HasChannel(AE_CH_TBC))   channelLayout |= AV_CH_TOP_BACK_CENTER;
  if (info.HasChannel(AE_CH_TBR))   channelLayout |= AV_CH_TOP_BACK_RIGHT;

  return channelLayout;
}

AVSampleFormat CActiveAEResample::GetAVSampleFormat(AEDataFormat format)
{
  #ifdef DEBUG_VERBOSE
  CLog::Log(LOGINFO, "%s::%s", CLASSNAME, __func__);
  #endif
  if      (format == AE_FMT_U8)     return AV_SAMPLE_FMT_U8;
  else if (format == AE_FMT_S16NE)  return AV_SAMPLE_FMT_S16;
  else if (format == AE_FMT_S32NE)  return AV_SAMPLE_FMT_S32;
  else if (format == AE_FMT_S24NE4) return AV_SAMPLE_FMT_S32;
  else if (format == AE_FMT_S24NE4MSB)return AV_SAMPLE_FMT_S32;
  else if (format == AE_FMT_FLOAT)  return AV_SAMPLE_FMT_FLT;
  else if (format == AE_FMT_DOUBLE) return AV_SAMPLE_FMT_DBL;

  else if (format == AE_FMT_U8P)     return AV_SAMPLE_FMT_U8P;
  else if (format == AE_FMT_S16NEP)  return AV_SAMPLE_FMT_S16P;
  else if (format == AE_FMT_S32NEP)  return AV_SAMPLE_FMT_S32P;
  else if (format == AE_FMT_S24NE4P) return AV_SAMPLE_FMT_S32P;
  else if (format == AE_FMT_S24NE4MSB)return AV_SAMPLE_FMT_S32;
  else if (format == AE_FMT_FLOATP)  return AV_SAMPLE_FMT_FLTP;
  else if (format == AE_FMT_DOUBLEP) return AV_SAMPLE_FMT_DBLP;

  if (AE_IS_PLANAR(format))
    return AV_SAMPLE_FMT_FLTP;
  else
    return AV_SAMPLE_FMT_FLT;
}

uint64_t CActiveAEResample::GetAVChannel(enum AEChannel aechannel)
{
  #ifdef DEBUG_VERBOSE
  CLog::Log(LOGINFO, "%s::%s", CLASSNAME, __func__);
  #endif
  switch (aechannel)
  {
  case AE_CH_FL:   return AV_CH_FRONT_LEFT;
  case AE_CH_FR:   return AV_CH_FRONT_RIGHT;
  case AE_CH_FC:   return AV_CH_FRONT_CENTER;
  case AE_CH_LFE:  return AV_CH_LOW_FREQUENCY;
  case AE_CH_BL:   return AV_CH_BACK_LEFT;
  case AE_CH_BR:   return AV_CH_BACK_RIGHT;
  case AE_CH_FLOC: return AV_CH_FRONT_LEFT_OF_CENTER;
  case AE_CH_FROC: return AV_CH_FRONT_RIGHT_OF_CENTER;
  case AE_CH_BC:   return AV_CH_BACK_CENTER;
  case AE_CH_SL:   return AV_CH_SIDE_LEFT;
  case AE_CH_SR:   return AV_CH_SIDE_RIGHT;
  case AE_CH_TC:   return AV_CH_TOP_CENTER;
  case AE_CH_TFL:  return AV_CH_TOP_FRONT_LEFT;
  case AE_CH_TFC:  return AV_CH_TOP_FRONT_CENTER;
  case AE_CH_TFR:  return AV_CH_TOP_FRONT_RIGHT;
  case AE_CH_TBL:  return AV_CH_TOP_BACK_LEFT;
  case AE_CH_TBC:  return AV_CH_TOP_BACK_CENTER;
  case AE_CH_TBR:  return AV_CH_TOP_BACK_RIGHT;
  default:
    return 0;
  }
}

int CActiveAEResample::GetAVChannelIndex(enum AEChannel aechannel, uint64_t layout)
{
  #ifdef DEBUG_VERBOSE
  CLog::Log(LOGINFO, "%s::%s", CLASSNAME, __func__);
  #endif
  return av_get_channel_layout_channel_index(layout, GetAVChannel(aechannel));
}

#endif
