/*
 *  Copyright (C) 2026 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#include "DVDInputStreamBlurayFile.h"

#include "FileItem.h"
#include "utils/log.h"

#include <algorithm>
#include <cstring>

namespace
{
// An aligned unit, and the only read size libbluray's descrambling reader accepts.
constexpr int64_t UNIT_SIZE = 6144;

// The base and dependent views sit far apart on the disc, so every refill costs a seek of
// the whole image. Around 3 MiB is roughly a second of dependent view, which keeps the seek
// rate low without adding a noticeable delay after a jump.
constexpr size_t UNITS_PER_FILL = 512;
} // namespace

CDVDInputStreamBlurayFile::CDVDInputStreamBlurayFile(const CFileItem& fileitem, BD_FILE_H* file)
  : CDVDInputStream(DVDSTREAM_TYPE_FILE, fileitem),
    m_file(file)
{
  m_content = "video/x-mpegts";
}

CDVDInputStreamBlurayFile::~CDVDInputStreamBlurayFile()
{
  CDVDInputStreamBlurayFile::Close();
}

bool CDVDInputStreamBlurayFile::Open()
{
  if (!m_file)
    return false;

  m_length = m_file->seek(m_file, 0, SEEK_END);
  if (m_length < 0)
  {
    CLog::LogF(LOGERROR, "failed to determine clip length");
    return false;
  }

  if (m_file->seek(m_file, 0, SEEK_SET) < 0)
  {
    CLog::LogF(LOGERROR, "failed to rewind clip");
    return false;
  }

  m_buffer.resize(UNITS_PER_FILL * UNIT_SIZE);
  m_bufferStart = 0;
  m_bufferPos = 0;
  m_bufferLen = 0;
  m_readOffset = 0;
  m_pendingSkip = 0;
  m_position = 0;
  m_eof = false;

  return true;
}

void CDVDInputStreamBlurayFile::Close()
{
  if (m_file)
  {
    m_file->close(m_file);
    m_file = nullptr;
  }

  m_buffer.clear();
  m_buffer.shrink_to_fit();
  m_bufferPos = 0;
  m_bufferLen = 0;
}

bool CDVDInputStreamBlurayFile::FillBuffer()
{
  if (!m_file || m_buffer.empty())
    return false;

  m_bufferStart = m_readOffset;
  m_bufferPos = 0;
  m_bufferLen = 0;

  // One unit per call. Anything else is rejected outright when the clip is scrambled.
  while (m_bufferLen + UNIT_SIZE <= m_buffer.size())
  {
    const int64_t read{m_file->read(m_file, m_buffer.data() + m_bufferLen, UNIT_SIZE)};
    if (read <= 0)
      break;

    m_bufferLen += static_cast<size_t>(read);

    if (read < UNIT_SIZE)
      break;
  }

  m_readOffset += static_cast<int64_t>(m_bufferLen);

  if (m_bufferLen == 0)
  {
    m_eof = true;
    return false;
  }

  // Drop the part of the leading unit that a seek landed past.
  m_bufferPos = std::min(m_pendingSkip, m_bufferLen);
  m_pendingSkip = 0;

  return m_bufferPos < m_bufferLen;
}

int CDVDInputStreamBlurayFile::Read(uint8_t* buf, int buf_size)
{
  if (!m_file || buf_size <= 0)
    return -1;

  int total{0};
  while (total < buf_size)
  {
    if (m_bufferPos >= m_bufferLen && !FillBuffer())
      break;

    const size_t available{m_bufferLen - m_bufferPos};
    const size_t chunk{std::min(available, static_cast<size_t>(buf_size - total))};
    std::memcpy(buf + total, m_buffer.data() + m_bufferPos, chunk);

    m_bufferPos += chunk;
    total += static_cast<int>(chunk);
  }

  if (total == 0)
    return m_eof ? 0 : -1;

  m_position += total;

  return total;
}

int64_t CDVDInputStreamBlurayFile::Seek(int64_t offset, int whence)
{
  if (whence == DVDSTREAM_SEEK_POSSIBLE)
    return 1;

  if (!m_file)
    return -1;

  int64_t target{offset};
  switch (whence)
  {
    case SEEK_SET:
      break;
    case SEEK_CUR:
      target = m_position + offset;
      break;
    case SEEK_END:
      if (m_length < 0)
        return -1;
      target = m_length + offset;
      break;
    default:
      return -1;
  }

  if (target < 0)
    return -1;

  // Serve the seek from the read ahead buffer when the target is still inside it. Seeking
  // the underlying handle would throw away data we have already paid a disc seek for, and
  // the demuxer does a lot of short steps while scanning.
  if (target >= m_bufferStart && target < m_bufferStart + static_cast<int64_t>(m_bufferLen))
  {
    m_bufferPos = static_cast<size_t>(target - m_bufferStart);
    m_position = target;
    m_eof = false;
    return m_position;
  }

  // Descrambling works a unit at a time, so land on a unit boundary and skip the remainder
  // once it has been read.
  const int64_t aligned{target - (target % UNIT_SIZE)};
  if (m_file->seek(m_file, aligned, SEEK_SET) < 0)
    return -1;

  m_bufferStart = aligned;
  m_bufferPos = 0;
  m_bufferLen = 0;
  m_readOffset = aligned;
  m_pendingSkip = static_cast<size_t>(target - aligned);
  m_position = target;
  m_eof = false;

  return m_position;
}

bool CDVDInputStreamBlurayFile::IsEOF()
{
  return m_eof && m_bufferPos >= m_bufferLen;
}

int64_t CDVDInputStreamBlurayFile::GetLength()
{
  return m_length;
}
