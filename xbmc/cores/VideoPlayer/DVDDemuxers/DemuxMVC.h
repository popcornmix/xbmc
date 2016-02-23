#pragma once

/*
*      Copyright (C) 2005-2013 Team XBMC
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

#include "DVDDemux.h"

extern "C" {
#include "libavformat/avformat.h"
}

class CDemuxMVC : public CDVDDemux
{
public:
  CDemuxMVC();
  virtual ~CDemuxMVC();
  bool Open(CDVDInputStream* pInput);
  virtual void Reset();
  virtual void Abort();
  virtual void Flush();
  virtual DemuxPacket* Read();
  virtual bool SeekTime(int time, bool backwords = false, double* startpts = nullptr);
  virtual void SetSpeed(int iSpeed) { };
  virtual int GetStreamLength() { return 0; };
  virtual CDemuxStream* GetStream(int iStreamId) const override { return nullptr; };
  virtual std::vector<CDemuxStream*> GetStreams() const override;
  virtual int GetNrOfStreams() const override { return 1; };
  virtual std::string GetFileName();

  AVStream* GetAVStream();
  CDVDInputStream*    m_pInput;

private:
  void Dispose();
  double ConvertTimestamp(int64_t pts, int den, int num);

  AVIOContext        *m_ioContext = nullptr;
  AVFormatContext    *m_pFormatContext = nullptr;
  int                 m_nStreamIndex = -1;
};