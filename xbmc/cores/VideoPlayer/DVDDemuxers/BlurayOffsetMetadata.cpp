/*
 *  Copyright (C) 2026 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#include "BlurayOffsetMetadata.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <mutex>
#include <ranges>

namespace KODI::VIDEO::BLURAY
{
namespace
{

constexpr uint8_t NAL_TYPE_MASK{0x1f};
constexpr uint8_t NAL_TYPE_SEI{6};

//! The metadata is a user_data_unregistered SEI message carrying this UUID, then "OFMD".
constexpr std::array<uint8_t, 16> METADATA_UUID{0x17, 0xee, 0x8c, 0x60, 0xf8, 0x4d, 0x11, 0xd9,
                                                0x8c, 0xd6, 0x08, 0x00, 0x20, 0x0c, 0x9a, 0x66};
constexpr std::array<uint8_t, 4> METADATA_MARKER{'O', 'F', 'M', 'D'};

//! Bytes of header between the marker and the offsets themselves.
constexpr size_t HEADER_SIZE{10};

//! Where in that header the two fields that matter are.
constexpr size_t HEADER_SEQUENCES{6};
constexpr size_t HEADER_FRAMES{7};

//! The sequence count shares its byte with two marker bits.
constexpr uint8_t SEQUENCES_MASK{0x3f};

//! An offset is a direction bit and a magnitude, not a signed byte: an unused sequence
//! reads 0x80, which as a magnitude of zero is the same offset either way.
constexpr uint8_t OFFSET_DIRECTION{0x80};
constexpr uint8_t OFFSET_MAGNITUDE{0x7f};

//! No SEI worth reading is anywhere near this large.
constexpr size_t MAX_SEI_SIZE{1 << 16};

//! \brief Undo the emulation prevention bytes of a NAL unit payload.
std::vector<uint8_t> Unescape(const uint8_t* data, size_t size)
{
  std::vector<uint8_t> rbsp;
  rbsp.reserve(size);

  for (size_t i = 0; i < size; ++i)
  {
    if (i + 2 < size && data[i] == 0 && data[i + 1] == 0 && data[i + 2] == 3)
    {
      rbsp.push_back(0);
      rbsp.push_back(0);
      i += 2;
      continue;
    }
    rbsp.push_back(data[i]);
  }

  return rbsp;
}

/*!
 * \brief Read the metadata out of an SEI NAL unit's unescaped payload.
 *
 * Found by its UUID rather than by walking the SEI messages, because how the message is
 * wrapped varies: an encoder is free to nest it in mvc_scalable_nesting under either form
 * of that message's header, and both forms are in the wild. A 16 byte UUID is its own
 * evidence, and this only ever looks inside an SEI NAL of the dependent view.
 */
bool ParseSei(const std::vector<uint8_t>& rbsp, OffsetMetadata& metadata)
{
  const auto uuid =
      std::search(rbsp.begin(), rbsp.end(), METADATA_UUID.begin(), METADATA_UUID.end());
  if (uuid == rbsp.end())
    return false;

  auto pos = uuid + METADATA_UUID.size();
  if (static_cast<size_t>(rbsp.end() - pos) < METADATA_MARKER.size() + HEADER_SIZE)
    return false;

  if (!std::equal(METADATA_MARKER.begin(), METADATA_MARKER.end(), pos))
    return false;

  pos += METADATA_MARKER.size();

  const unsigned int sequences{static_cast<unsigned int>(pos[HEADER_SEQUENCES] & SEQUENCES_MASK)};
  const unsigned int frames{pos[HEADER_FRAMES]};
  if (sequences == 0 || sequences > MAX_OFFSET_SEQUENCES || frames == 0)
    return false;

  pos += HEADER_SIZE;

  const size_t count{static_cast<size_t>(sequences) * frames};
  if (static_cast<size_t>(rbsp.end() - pos) < count)
    return false;

  metadata.sequences = sequences;
  metadata.frames = frames;
  metadata.offsets.resize(count);
  std::ranges::transform(pos, pos + count, metadata.offsets.begin(),
                         [](uint8_t value)
                         {
                           const int magnitude{value & OFFSET_MAGNITUDE};
                           return static_cast<int8_t>(value & OFFSET_DIRECTION ? -magnitude
                                                                               : magnitude);
                         });

  return true;
}

} // unnamed namespace

int OffsetMetadata::Offset(unsigned int sequence, unsigned int frame) const
{
  if (sequence >= sequences || frame >= frames)
    return 0;

  return offsets[static_cast<size_t>(sequence) * frames + frame];
}

bool ParseOffsetMetadata(const uint8_t* data, size_t size, OffsetMetadata& metadata)
{
  if (!data)
    return false;

  // Walk the access unit's NAL units, looking only inside the SEI ones.
  for (size_t i = 0; i + 3 <= size;)
  {
    if (data[i] != 0 || data[i + 1] != 0 || data[i + 2] != 1)
    {
      ++i;
      continue;
    }

    const size_t start{i + 3};
    size_t end{size};
    for (size_t j = start; j + 3 <= size; ++j)
    {
      if (data[j] == 0 && data[j + 1] == 0 && data[j + 2] == 1)
      {
        end = (j > start && data[j - 1] == 0) ? j - 1 : j;
        break;
      }
    }

    if (start < end && (data[start] & NAL_TYPE_MASK) == NAL_TYPE_SEI && end - start <= MAX_SEI_SIZE)
    {
      if (ParseSei(Unescape(data + start + 1, end - start - 1), metadata))
        return true;
    }

    i = end;
  }

  return false;
}

void COffsetMetadataStore::Add(double startPts, double frameDuration, OffsetMetadata&& metadata)
{
  if (frameDuration <= 0.0)
    return;

  std::unique_lock lock(m_section);

  m_entries.push_back({startPts, frameDuration, std::move(metadata)});
  while (m_entries.size() > MAX_ENTRIES)
    m_entries.pop_front();
}

int COffsetMetadataStore::GetOffset(double pts, unsigned int sequence) const
{
  std::unique_lock lock(m_section);

  // Most recent first: after a loop or a stream change the newer entry is the wanted one.
  for (const SEntry& entry : m_entries | std::views::reverse)
  {
    const long frame{std::lround((pts - entry.startPts) / entry.frameDuration)};
    if (frame >= 0 && frame < static_cast<long>(entry.metadata.frames))
      return entry.metadata.Offset(sequence, static_cast<unsigned int>(frame));
  }

  return 0;
}

void COffsetMetadataStore::Flush()
{
  std::unique_lock lock(m_section);

  m_entries.clear();
}

} // namespace KODI::VIDEO::BLURAY
