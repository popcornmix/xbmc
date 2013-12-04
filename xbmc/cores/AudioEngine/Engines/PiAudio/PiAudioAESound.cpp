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

#include "PiAudioAESound.h"
#include "AEFactory.h"
#include "utils/log.h"
#include "MathUtils.h"
#include "StringUtils.h"
#include "settings/Settings.h"

#define printf if (1) {} else printf

CPiAudioAESound::CPiAudioAESound(const std::string &filename, CPiAudioAEStream *stream) :
  IAESound         (filename),
  m_pulseName      (StringUtils::CreateUUID()),
  m_filename       (filename),
  m_stream         (stream  ),
  m_samples        (NULL    ),
  m_sampleCount    (0       ),
  m_initialized    (false   ),
  m_maxVolume      (0.0f    ),
  m_volume         (0.0f    )
{
}

CPiAudioAESound::~CPiAudioAESound()
{
  DeInitialize();
}

bool CPiAudioAESound::GuiSoundEnabled()
{
  // we abuse the paused flag of stream here
  return !m_stream->IsPaused();
}

bool CPiAudioAESound::InitializeInternal()
{
  m_wavLoader.Load(m_filename);

  CAEChannelInfo channelLayout;
  channelLayout.Reset();
  channelLayout += AE_CH_FL;
  channelLayout += AE_CH_FR;

  if (!m_wavLoader.Initialize(48000, channelLayout))
    return false;

  m_sampleCount = m_wavLoader.GetSampleCount();
  m_samples = (int16_t *)_aligned_malloc(sizeof(int16_t) * m_sampleCount, 16);
  float *samples = m_wavLoader.GetSamples();
  float scale = (1<<15)-1;
  for (int i=0; i<m_sampleCount; i++)
    m_samples[i] = samples[i] * scale;
  m_wavLoader.DeInitialize();
  return true;
}

bool CPiAudioAESound::Initialize()
{
  if (GuiSoundEnabled() && !m_initialized)
    m_initialized = InitializeInternal();

  m_maxVolume     = CAEFactory::GetEngine()->GetVolume();
  m_volume        = 1.0f;
  printf("%s %p %d\n", __func__, m_stream, m_sampleCount);

  return true;
}

void CPiAudioAESound::DeInitialize()
{
  _aligned_free(m_samples);
  m_samples = NULL;
  m_initialized = false;
}

void CPiAudioAESound::Play()
{
  printf("%s %s\n", __func__, m_filename.c_str());

  if (!GuiSoundEnabled())
    return;

  if (!m_initialized)
    m_initialized = InitializeInternal();

  if (!m_initialized || !m_stream || !m_samples)
    return;
  if (0 && IsPlaying())
    m_stream->Flush();
  m_stream->AddData((void *)m_samples, m_sampleCount * sizeof(int16_t));
}

void CPiAudioAESound::Stop()
{
  m_stream->Flush();
  printf("%s\n", __func__);
}

bool CPiAudioAESound::IsPlaying()
{
  printf("%s\n", __func__);
  return m_stream->GetDelay() > 0.0;
}

void CPiAudioAESound::SetVolume(float volume)
{
  printf("%s\n", __func__);
}

float CPiAudioAESound::GetVolume()
{
  printf("%s\n", __func__);
  return 1.0f;
}

#endif
