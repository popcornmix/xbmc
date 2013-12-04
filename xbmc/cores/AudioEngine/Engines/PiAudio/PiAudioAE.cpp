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

#include "PiAudioAE.h"

using namespace PiAudioAE;
#include "Utils/AEUtil.h"

#include "settings/Settings.h"
#include "settings/AdvancedSettings.h"
#include "windowing/WindowingFactory.h"

#if defined(TARGET_RASPBERRY_PI)
#include "linux/RBP.h"
#endif

#define printf if (1) {} else printf

CPiAudioAE::CPiAudioAE()
: CThread("CPiAudio")
{
  printf("%s: %p\n", __func__, this);
  m_initialized = false;
  m_mode = AE_SOUND_OFF;
  m_playing = false;
  m_playing_passthrough = false;
  m_stream = NULL;
}

CPiAudioAE::~CPiAudioAE()
{
  printf("%s: %p\n", __func__, this);
}

bool CPiAudioAE::Initialize()
{
  printf("%s\n", __func__);

  UpdateStreamSilence();
  Create();
  return true;
}

void CPiAudioAE::Process()
{
  while(!m_bStop)
  {
    /* thread just currently checks once a second if it's time to disable streamsilence */
    Sleep(1000);

    if (m_extSilenceTimer.IsTimePast())
    {
      UpdateStreamSilence(false);
      m_extSilenceTimer.Set(XbmcThreads::EndTime::InfiniteValue);
    }
  }
}

void CPiAudioAE::UpdateStreamSilence()
{
  if (CSettings::Get().GetInt("audiooutput.streamsilence") > 0)
    m_extSilenceTimeout = CSettings::Get().GetInt("audiooutput.streamsilence") * 60000;
  else
    m_extSilenceTimeout = XbmcThreads::EndTime::InfiniteValue;
  m_extSilenceTimer.Set(m_extSilenceTimeout);
  UpdateStreamSilence(CSettings::Get().GetString("audiooutput.audiodevice") == "HDMI" &&
              CSettings::Get().GetInt("audiooutput.streamsilence") != 0);
}

void CPiAudioAE::UpdateStreamSilence(bool enable)
{
#if defined(TARGET_RASPBERRY_PI)
  char response[80] = "";
  char command[80] = "";
  sprintf(command, "force_audio hdmi %d", enable);
  vc_gencmd(response, sizeof response, command);
#endif
}

bool CPiAudioAE::Suspend()
{
  return true;
}

bool CPiAudioAE::Resume()
{
  return true;
}

float CPiAudioAE::GetVolume()
{
  return m_aeVolume;
}

void CPiAudioAE::SetVolume(const float volume)
{
  printf("%s: %.2f\n", __func__, volume);

  m_aeVolume = std::max( 0.0f, std::min(1.0f, volume));
}

void CPiAudioAE::SetMute(const bool enabled)
{
  m_aeMuted = enabled;
}

bool CPiAudioAE::IsMuted()
{
  return m_aeMuted;
}

IAEStream *CPiAudioAE::MakeStream(enum AEDataFormat dataFormat, unsigned int sampleRate, unsigned int encodedSampleRate, CAEChannelInfo channelLayout, unsigned int options)
{
  CPiAudioAEStream *s = NULL;

  if (!sampleRate)
  {
    m_playing = true;
    m_playing_passthrough = options != 0;
    SetSoundMode(m_mode);
  }
  else
  {
    s = new CPiAudioAEStream(dataFormat, sampleRate, channelLayout, options);
  }
  printf("%s: %p,%d,%d,%d,%x,%x %p\n", __func__, m_stream, dataFormat, sampleRate, encodedSampleRate, 0, options, s);
  return s;
}

IAEStream *CPiAudioAE::FreeStream(IAEStream *stream)
{
  // will retrigger the streamsilence timer
  printf("%s: %p\n", __func__, stream);
  if (!stream)
  {
    m_playing = false;
    m_playing_passthrough = false;
    SetSoundMode(m_mode);
    UpdateStreamSilence();
  }
  delete static_cast<CPiAudioAEStream *>(stream);
  return NULL;
}

IAESound *CPiAudioAE::MakeSound(const std::string& file)
{
  if (!m_initialized)
  {
    CAEChannelInfo channelLayout;
    channelLayout.Reset();
    channelLayout += AE_CH_FL;
    channelLayout += AE_CH_FR;

    m_stream = static_cast<CPiAudioAEStream *>(MakeStream(AE_FMT_FLOAT, 48000, 48000, channelLayout, 0));
    if (!m_stream)
      CLog::Log(LOGERROR, "%s: Failed to makeStream", __func__);
    else
      m_initialized = true;
    SetSoundMode(m_mode);
  }

  CPiAudioAESound *s = new CPiAudioAESound(file, m_stream);
  if (!s->Initialize())
  {
  printf("%s: %s=%p FAILED\n", __func__, file.c_str(), s);
    delete s;
    return NULL;
  }
  printf("%s: %s=%p\n", __func__, file.c_str(), s);
  return s;
}

void CPiAudioAE::FreeSound(IAESound *sound)
{
  printf("%s: %p\n", __func__, sound);
  delete static_cast<CPiAudioAESound *>(sound);
}

bool CPiAudioAE::SupportsRaw(AEDataFormat format)
{
  bool supported = false;
#if defined(TARGET_RASPBERRY_PI)
  if (CSettings::Get().GetString("audiooutput.audiodevice") == "HDMI")
  {
    if (!CSettings::Get().GetBool("audiooutput.dualaudio"))
    {
      DllBcmHost m_DllBcmHost;
      m_DllBcmHost.Load();
      if (format == AE_FMT_AC3 && CSettings::Get().GetBool("audiooutput.ac3passthrough") &&
          m_DllBcmHost.vc_tv_hdmi_audio_supported(EDID_AudioFormat_eAC3, 2, EDID_AudioSampleRate_e44KHz, EDID_AudioSampleSize_16bit ) == 0)
        supported = true;
      if (format == AE_FMT_DTS && CSettings::Get().GetBool("audiooutput.dtspassthrough") &&
          m_DllBcmHost.vc_tv_hdmi_audio_supported(EDID_AudioFormat_eDTS, 2, EDID_AudioSampleRate_e44KHz, EDID_AudioSampleSize_16bit ) == 0)
        supported = true;
      m_DllBcmHost.Unload();
    }
  }
#endif
  return supported;
}

bool CPiAudioAE::SupportsSilenceTimeout()
{
  return true;
}

void CPiAudioAE::OnSettingsChange(const std::string& setting)
{
  if (setting == "audiooutput.streamsilence" || setting == "audiooutput.audiodevice")
    UpdateStreamSilence();
  if (setting == "audiooutput.audiodevice")
    SetSoundMode(m_mode);
}

void CPiAudioAE::EnumerateOutputDevices(AEDeviceList &devices, bool passthrough)
{
   if (!passthrough)
   {
     devices.push_back(AEDevice("Analogue", "Analogue"));
     devices.push_back(AEDevice("HDMI", "HDMI"));
   }
}

std::string CPiAudioAE::GetDefaultDevice(bool passthrough)
{
  return "HDMI";
}

void CPiAudioAE::SetSoundMode(const int mode)
{
   printf("%s: mode=%d\n", __func__, mode);
   m_mode = mode;
   if (!m_stream)
     return;
   if (m_playing_passthrough)
     m_stream->Pause();
   else if (m_mode == AE_SOUND_ALWAYS)
     m_stream->Resume();
   else if (m_mode == AE_SOUND_OFF)
     m_stream->Pause();
   else if (m_playing)
     m_stream->Pause();
   else
     m_stream->Resume();
}

bool CPiAudioAE::IsSettingVisible(const std::string &settingId)
{
  if (settingId == "audiooutput.samplerate")
    return true;

  if (CSettings::Get().GetString("audiooutput.audiodevice") == "HDMI")
  {
    if (settingId == "audiooutput.passthrough")
      return true;
    if (settingId == "audiooutput.dtspassthrough")
      return true;
    if (settingId == "audiooutput.ac3passthrough")
      return true;
    if (settingId == "audiooutput.channels")
      return true;
    if (settingId == "audiooutput.dualaudio")
      return true;
    if (settingId == "audiooutput.streamsilence")
      return true;
  }
  return false;
}
