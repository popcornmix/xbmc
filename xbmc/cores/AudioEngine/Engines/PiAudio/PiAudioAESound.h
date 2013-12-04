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

#include "system.h"
#ifdef TARGET_RASPBERRY_PI

#include "Interfaces/AESound.h"
//#include "Interfaces/AEStream.h"
#include "PiAudioAEStream.h"
#include "Utils/AEWAVLoader.h"

class CPiAudioAESound : public IAESound
{
public:
  /* this should NEVER be called directly, use AE.GetSound */
  CPiAudioAESound(const std::string &filename, CPiAudioAEStream *stream);
  virtual ~CPiAudioAESound();

  virtual void DeInitialize();
  virtual bool Initialize();

  virtual void Play();
  virtual void Stop();
  virtual bool IsPlaying();

  virtual void  SetVolume(float volume);
  virtual float GetVolume();
private:
  bool InitializeInternal();
  bool GuiSoundEnabled();

  std::string    m_pulseName;
  std::string    m_filename;
  CPiAudioAEStream     *m_stream;
  CAEWAVLoader   m_wavLoader;
  int16_t       *m_samples;
  int            m_sampleCount;
  bool           m_initialized;
  bool           m_enabled;

  float m_maxVolume, m_volume;
};

#endif
