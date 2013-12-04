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

#ifdef TARGET_RASPBERRY_PI

#include "Interfaces/AEStream.h"
#include "threads/Thread.h"
#include "cores/omxplayer/OMXAudio.h"
#include "linux/OMXClock.h"

class CPiAudioAEStream : public IAEStream
{
public:
  /* this should NEVER be called directly, use AE.GetStream */
  CPiAudioAEStream(enum AEDataFormat format, unsigned int sampleRate, CAEChannelInfo channelLayout, unsigned int options);
  virtual ~CPiAudioAEStream();

  virtual void Destroy();

  virtual unsigned int GetSpace();
  virtual unsigned int AddData(void *data, unsigned int size);
  virtual double GetDelay();
  virtual double GetCacheTime ();
  virtual double GetCacheTotal();

  virtual bool IsPaused     ();
  virtual bool IsDraining   ();
  virtual bool IsDrained    ();
  virtual bool IsDestroyed  ();
  virtual bool IsBuffering() { return false; }

  virtual void Pause   ();
  virtual void Resume  ();
  virtual void Drain   (bool wait);
  virtual void Flush   ();

  virtual float GetVolume    ();
  virtual float GetReplayGain();
  virtual float GetAmplification() { return 1.0f; }
  virtual void  SetVolume    (float volume);
  virtual void  SetReplayGain(float factor);
  virtual void  SetAmplification(float amplify){}
  void SetMute(const bool muted);

  virtual const unsigned int      GetFrameSize   () const;
  virtual const unsigned int      GetChannelCount() const;
  virtual const unsigned int      GetSampleRate  () const;
  virtual const enum AEDataFormat GetDataFormat  () const;
  virtual const unsigned int GetEncodedSampleRate() const { return GetSampleRate(); }

  /* for dynamic sample rate changes (smoothvideo) */
  virtual double GetResampleRatio();
  virtual bool   SetResampleRatio(double ratio);

  /* vizualization callback register function */
  virtual void RegisterAudioCallback(IAudioCallback* pCallback);
  virtual void UnRegisterAudioCallback();

  virtual void FadeVolume(float from, float target, unsigned int time);
  virtual bool IsFading();

  /* trigger the stream to update its volume relative to AE */
  void UpdateVolume(float max);

  /* used to prepare a stream for resume */
  void SetDrained() { m_ResumeCallback = true; };

  /* Process the Resume of streams */
  void ProcessCallbacks();

  virtual void RegisterSlave(IAEStream *stream);
private:
  void SetAudioDest();
  CAEChannelInfo    m_channelLayout;
  bool m_ResumeCallback;
  IAudioCallback* m_AudioCallback;
  bool m_Initialized;
  AEAudioFormat             m_format;
  float       m_MaxVolume;
  float       m_Volume;
  bool        m_paused;

  OMX_AUDIO_PARAM_PCMMODETYPE m_pcm_input;
  COMXCoreComponent m_omx_render;
};

#endif
