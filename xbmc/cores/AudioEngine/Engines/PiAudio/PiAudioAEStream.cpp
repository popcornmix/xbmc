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
#ifdef TARGET_RASPBERRY_PI

#include "PiAudioAEStream.h"
#include "AEFactory.h"
#include "Utils/AEUtil.h"
#include "utils/log.h"
#include "utils/MathUtils.h"
#include "threads/SingleLock.h"
#include "settings/Settings.h"

#define CLASSNAME "CPiAudioAEStream"

#define printf if (1) {} else printf

void CPiAudioAEStream::SetAudioDest()
{
  OMX_ERRORTYPE omx_err   = OMX_ErrorNone;
  OMX_CONFIG_BRCMAUDIODESTINATIONTYPE audioDest;
  OMX_INIT_STRUCTURE(audioDest);
  if (CSettings::Get().GetString("audiooutput.audiodevice") == "Analogue")
    strncpy((char *)audioDest.sName, "local", strlen("local"));
  else
    strncpy((char *)audioDest.sName, "hdmi", strlen("hdmi"));
  omx_err = m_omx_render.SetConfig(OMX_IndexConfigBrcmAudioDestination, &audioDest);
  if (omx_err != OMX_ErrorNone)
    CLog::Log(LOGERROR, "%s::%s - m_omx_render.SetConfig omx_err(0x%08x)", CLASSNAME, __func__, omx_err);
}

CPiAudioAEStream::CPiAudioAEStream(enum AEDataFormat format, unsigned int sampleRate, CAEChannelInfo channelLayout, unsigned int options)
{
  printf("%s\n", __func__);
  ASSERT(channelLayout.Count());
  m_Initialized = false;
  m_paused = true;

  OMX_ERRORTYPE omx_err   = OMX_ErrorNone;
  if(!m_omx_render.Initialize("OMX.broadcom.audio_render", OMX_IndexParamAudioInit))
    CLog::Log(LOGERROR, "%s::%s - m_omx_render.Initialize omx_err(0x%08x)", CLASSNAME, __func__, omx_err);

  OMX_INIT_STRUCTURE(m_pcm_input);
  m_pcm_input.nPortIndex = m_omx_render.GetInputPort();
  m_pcm_input.eNumData              = OMX_NumericalDataSigned;
  m_pcm_input.eEndian               = OMX_EndianLittle;
  m_pcm_input.bInterleaved          = OMX_TRUE;
  m_pcm_input.nBitPerSample         = 16;
  m_pcm_input.ePCMMode              = OMX_AUDIO_PCMModeLinear;
  m_pcm_input.nChannels             = 2;
  m_pcm_input.nSamplingRate         = sampleRate;
  m_pcm_input.eChannelMapping[0] = OMX_AUDIO_ChannelLF;
  m_pcm_input.eChannelMapping[1] = OMX_AUDIO_ChannelRF;
  m_pcm_input.eChannelMapping[2] = OMX_AUDIO_ChannelMax;

  omx_err = m_omx_render.SetParameter(OMX_IndexParamAudioPcm, &m_pcm_input);
  if(omx_err != OMX_ErrorNone)
    CLog::Log(LOGERROR, "%s::%s - error m_omx_render SetParameter omx_err(0x%08x)", CLASSNAME, __func__, omx_err);

  m_omx_render.ResetEos();

  SetAudioDest();

  // set up the number/size of buffers for decoder input
  OMX_PARAM_PORTDEFINITIONTYPE port_param;
  OMX_INIT_STRUCTURE(port_param);
  port_param.nPortIndex = m_omx_render.GetInputPort();

  omx_err = m_omx_render.GetParameter(OMX_IndexParamPortDefinition, &port_param);
  if(omx_err != OMX_ErrorNone)
  {
    CLog::Log(LOGERROR, "COMXAudio::Initialize error get OMX_IndexParamPortDefinition (input) omx_err(0x%08x)\n", omx_err);
  }
  printf("port_param.nBufferSize = %d, port_param.nBufferCountActual = %d\n", port_param.nBufferSize, port_param.nBufferCountActual);
  port_param.nBufferCountActual = std::max((unsigned int)port_param.nBufferCountMin, 16U);

  port_param.nBufferSize = 128*1024;
  port_param.nBufferCountActual = 1;

  omx_err = m_omx_render.SetParameter(OMX_IndexParamPortDefinition, &port_param);
  if(omx_err != OMX_ErrorNone)
  {
    CLog::Log(LOGERROR, "COMXAudio::Initialize error set OMX_IndexParamPortDefinition (intput) omx_err(0x%08x)\n", omx_err);
  }

  omx_err = m_omx_render.AllocInputBuffers();
  if(omx_err != OMX_ErrorNone)
    CLog::Log(LOGERROR, "COMXAudio::Initialize - Error alloc buffers 0x%08x", omx_err);

    omx_err = m_omx_render.SetStateForComponent(OMX_StateExecuting);
    if(omx_err != OMX_ErrorNone)
      CLog::Log(LOGERROR, "%s::%s - m_omx_render OMX_StateExecuting omx_err(0x%08x)", CLASSNAME, __func__, omx_err);

  m_Initialized = true;

  m_MaxVolume     = CAEFactory::GetEngine()->GetVolume();
  m_Volume        = 1.0f;
  SetVolume(m_Volume);
}

CPiAudioAEStream::~CPiAudioAEStream()
{
  printf("%s\n", __func__);
  Destroy();
}

/*
  this method may be called inside the pulse main loop,
  so be VERY careful with locking
*/
void CPiAudioAEStream::Destroy()
{
  printf("%s\n", __func__);
}

unsigned int CPiAudioAEStream::GetSpace()
{
  printf("%s\n", __func__);
  return 0;
}

unsigned int CPiAudioAEStream::AddData(void *data, unsigned int size)
{
  printf("%s %p,%d\n", __func__, data, size);
  unsigned int sent = 0;

  if (!m_Initialized)
    return size;

  OMX_ERRORTYPE omx_err   = OMX_ErrorNone;
  OMX_BUFFERHEADERTYPE *omx_buffer = NULL;
  while(sent < size)
  {
    // 200ms timeout
    omx_buffer = m_omx_render.GetInputBuffer(200);

    if(omx_buffer == NULL)
    {
      CLog::Log(LOGERROR, "COMXAudio::Decode timeout\n");
      break;
    }

    omx_buffer->nFilledLen = std::min(omx_buffer->nAllocLen, size-sent);
    omx_buffer->nTimeStamp = ToOMXTime(0);
    omx_buffer->nFlags = 0;
    memcpy(omx_buffer->pBuffer, (uint8_t *)data + sent, omx_buffer->nFilledLen);
    sent += omx_buffer->nFilledLen;

    if (sent == size)
      omx_buffer->nFlags |= OMX_BUFFERFLAG_ENDOFFRAME;

    omx_err = m_omx_render.EmptyThisBuffer(omx_buffer);
    if (omx_err != OMX_ErrorNone)
    {
      CLog::Log(LOGERROR, "%s: size=%d err=%x", __func__, size, omx_err);
    }
  }
  printf("%s %p,%d=%d\n", __func__, data, size, sent);
  return 0;
}

double CPiAudioAEStream::GetDelay()
{
  OMX_PARAM_U32TYPE param;
  OMX_INIT_STRUCTURE(param);

  if (!m_Initialized)
    return 0.0;

  param.nPortIndex = m_omx_render.GetInputPort();

  OMX_ERRORTYPE omx_err = m_omx_render.GetConfig(OMX_IndexConfigAudioRenderingLatency, &param);

  if(omx_err != OMX_ErrorNone)
  {
    CLog::Log(LOGERROR, "%s::%s - error getting OMX_IndexConfigAudioRenderingLatency error 0x%08x\n",
      CLASSNAME, __func__, omx_err);
  }
  double delay = (double)param.nU32 / (double)m_pcm_input.nSamplingRate;
  printf("%s %.2f\n", __func__, delay);
  return delay;
}

double CPiAudioAEStream::GetCacheTime()
{
  printf("%s\n", __func__);
  return 0;
}

double CPiAudioAEStream::GetCacheTotal()
{
  printf("%s\n", __func__);
  return 0;
}

bool CPiAudioAEStream::IsPaused()
{
  printf("%s\n", __func__);
  return m_paused;
}

bool CPiAudioAEStream::IsDraining()
{
  printf("%s\n", __func__);
  return GetDelay() > 0.0;
}

bool CPiAudioAEStream::IsDrained()
{
  printf("%s\n", __func__);
  return !IsDraining();
}

bool CPiAudioAEStream::IsDestroyed()
{
  printf("%s\n", __func__);
  return false;
}

void CPiAudioAEStream::Pause()
{
  OMX_ERRORTYPE omx_err   = OMX_ErrorNone;
  m_paused = true;

  omx_err = m_omx_render.DisablePort(m_omx_render.GetInputPort(), true);
  if(omx_err != OMX_ErrorNone)
    CLog::Log(LOGERROR, "%s::%s - error m_omx_render DisablePort omx_err(0x%08x)", CLASSNAME, __func__, omx_err);

  SetAudioDest();
  printf("%s\n", __func__);
}

void CPiAudioAEStream::Resume()
{
  OMX_ERRORTYPE omx_err   = OMX_ErrorNone;
  SetAudioDest();

  omx_err = m_omx_render.EnablePort(m_omx_render.GetInputPort(), true);
  if(omx_err != OMX_ErrorNone)
    CLog::Log(LOGERROR, "%s::%s - error m_omx_render EnablePort omx_err(0x%08x)", CLASSNAME, __func__, omx_err);

  m_paused = false;
  printf("%s\n", __func__);
}

void CPiAudioAEStream::Drain(bool wait)
{
  printf("%s\n", __func__);
  if (wait)
    while (GetDelay() > 0.0)
      Sleep(10);
}

void CPiAudioAEStream::Flush()
{
  printf("%s\n", __func__);

  if (!m_Initialized)
    return;
  m_omx_render.FlushAll();
}

float CPiAudioAEStream::GetVolume()
{
  printf("%s\n", __func__);
  return m_Volume;
}

float CPiAudioAEStream::GetReplayGain()
{
  printf("%s\n", __func__);
  return 0.0f;
}

void CPiAudioAEStream::SetVolume(float volume)
{
  if (!m_Initialized)
    return;

  m_Volume = volume;
}

void CPiAudioAEStream::UpdateVolume(float max)
{
  if (!m_Initialized)
    return;

  m_MaxVolume = max;
  SetVolume(m_Volume);
}

void CPiAudioAEStream::SetMute(const bool mute)
{
  printf("%s\n", __func__);
  if (mute)
    SetVolume(0.0f);
  else
    SetVolume(m_Volume);
}

void CPiAudioAEStream::SetReplayGain(float factor)
{
  printf("%s\n", __func__);
}

const unsigned int CPiAudioAEStream::GetFrameSize() const
{
  printf("%s\n", __func__);
  return 0;
}

const unsigned int CPiAudioAEStream::GetChannelCount() const
{
  printf("%s\n", __func__);
  return m_channelLayout.Count();
}

const unsigned int CPiAudioAEStream::GetSampleRate() const
{
  printf("%s\n", __func__);
  return m_format.m_sampleRate;
}

const enum AEDataFormat CPiAudioAEStream::GetDataFormat() const
{
  printf("%s\n", __func__);
  return m_format.m_dataFormat;
}

double CPiAudioAEStream::GetResampleRatio()
{
  printf("%s\n", __func__);
  return 1.0;
}

bool CPiAudioAEStream::SetResampleRatio(double ratio)
{
  printf("%s\n", __func__);
  return false;
}

void CPiAudioAEStream::RegisterAudioCallback(IAudioCallback* pCallback)
{
  printf("%s\n", __func__);
  m_AudioCallback = pCallback;
}

void CPiAudioAEStream::UnRegisterAudioCallback()
{
  printf("%s\n", __func__);
  m_AudioCallback = NULL;
}

void CPiAudioAEStream::FadeVolume(float from, float target, unsigned int time)
{
  printf("%s\n", __func__);
}

bool CPiAudioAEStream::IsFading()
{
  printf("%s\n", __func__);
  return false;
}

void CPiAudioAEStream::ProcessCallbacks()
{
  printf("%s\n", __func__);
}

void CPiAudioAEStream::RegisterSlave(IAEStream *stream)
{
  printf("%s\n", __func__);
}

#endif
