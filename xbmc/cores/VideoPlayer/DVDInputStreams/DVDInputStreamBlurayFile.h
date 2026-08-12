/*
 *  Copyright (C) 2026 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#pragma once

#include "DVDInputStream.h"

#include <memory>
#include <vector>

extern "C"
{
#include <libbluray/filesystem.h>
}

/*!
 * \brief Byte stream over a single file on a Blu-ray disc, read through libbluray.
 *
 * The handle comes from bd_open_file_dec(), so AACS and BD+ decryption are applied
 * transparently and the same code works for a disc image, a BDMV folder and a physical
 * disc. This is used to read a clip that libbluray is not itself playing - currently the
 * MVC dependent view of a 3D title, which lives in its own clip alongside the base view.
 *
 * Unlike CDVDInputStreamBluray this stream is byte seekable, so CDVDDemuxFFmpeg can seek
 * it directly.
 *
 * Reads are buffered. The dependent view sits far away from the base view on the disc, so
 * without a read ahead every packet would make the image seek back and forth.
 *
 * All access to the underlying handle is in whole 6144 byte aligned units, because that is
 * the only size libbluray's descrambling reader accepts, and because a unit is what it
 * decrypts at a time.
 */
class CDVDInputStreamBlurayFile : public CDVDInputStream
{
public:
  CDVDInputStreamBlurayFile() = delete;
  CDVDInputStreamBlurayFile(const CFileItem& fileitem, BD_FILE_H* file);
  ~CDVDInputStreamBlurayFile() override;

  bool Open() override;
  void Close() override;
  int Read(uint8_t* buf, int buf_size) override;
  int64_t Seek(int64_t offset, int whence) override;
  bool IsEOF() override;
  int64_t GetLength() override;
  int GetBlockSize() override { return 6144; }
  bool CanSeek() override { return true; }

private:
  //! \brief Refill the read ahead buffer from the current file position.
  //! \return true if there is anything to read afterwards
  bool FillBuffer();

  BD_FILE_H* m_file{nullptr};

  std::vector<uint8_t> m_buffer;
  int64_t m_bufferStart{0}; //!< file offset of m_buffer[0], always unit aligned
  size_t m_bufferPos{0}; //!< read offset within m_buffer
  size_t m_bufferLen{0}; //!< valid bytes in m_buffer

  int64_t m_readOffset{0}; //!< file offset the next refill reads from, always unit aligned
  size_t m_pendingSkip{0}; //!< bytes to drop from the next refill, to land on an unaligned seek

  int64_t m_position{0}; //!< logical position, i.e. what the caller sees
  int64_t m_length{-1};
  bool m_eof{false};
};
