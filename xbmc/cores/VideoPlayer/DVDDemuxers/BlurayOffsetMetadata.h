/*
 *  Copyright (C) 2026 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#pragma once

#include "threads/CriticalSection.h"

#include <cstddef>
#include <cstdint>
#include <deque>
#include <vector>

namespace KODI::VIDEO::BLURAY
{

//! Most offset sequences a Blu-ray 3D title can carry.
inline constexpr unsigned int MAX_OFFSET_SEQUENCES{32};

//! The playlist's way of saying a stream follows no offset sequence.
inline constexpr unsigned int NO_OFFSET_SEQUENCE{0xff};

/*!
 * \brief The plane offsets one GOP of a Blu-ray 3D dependent view carries.
 *
 * A "1 plane + offset" title sends one monoscopic subtitle plane and asks the player to
 * place it in depth by shifting it horizontally, by its own amount each frame. The amounts
 * are carried in the dependent view as up to 32 independent sequences, one of which a
 * subtitle stream follows; the playlist says which (StreamInformation::offsetSequenceId).
 */
struct OffsetMetadata
{
  unsigned int sequences{0};
  unsigned int frames{0};

  //! sequences * frames entries in plane pixels, sequence-major.
  std::vector<int8_t> offsets;

  //! \brief The offset of one frame of one sequence, or 0 for anything out of range.
  int Offset(unsigned int sequence, unsigned int frame) const;
};

/*!
 * \brief Read the offset metadata of one dependent view access unit.
 *
 * \param data an Annex B access unit of the MVC dependent view
 * \param size its length
 * \param metadata filled in when the access unit carries offset metadata
 * \return true when it does, which is once per GOP
 */
bool ParseOffsetMetadata(const uint8_t* data, size_t size, OffsetMetadata& metadata);

/*!
 * \brief The offset metadata of the GOPs around the current position.
 *
 * Written by the demuxer as the dependent view is read and read by the render thread a few
 * seconds later, so it holds its own lock; the demuxer runs ahead by no more than the demux
 * buffer, which is why so few entries need keeping.
 */
class COffsetMetadataStore
{
public:
  /*!
   * \brief Keep the metadata of one GOP.
   * \param startPts presentation time of the first frame the metadata describes
   * \param frameDuration a frame's worth of presentation time
   */
  void Add(double startPts, double frameDuration, OffsetMetadata&& metadata);

  //! \brief The offset a sequence asks for at \p pts, or 0 when nothing describes it.
  int GetOffset(double pts, unsigned int sequence) const;

  void Flush();

private:
  //! Enough to cover the demux buffer several times over; a GOP is around a second.
  static constexpr size_t MAX_ENTRIES{32};

  struct SEntry
  {
    double startPts{0.0};
    double frameDuration{0.0};
    OffsetMetadata metadata;
  };

  mutable CCriticalSection m_section;
  std::deque<SEntry> m_entries;
};

} // namespace KODI::VIDEO::BLURAY
