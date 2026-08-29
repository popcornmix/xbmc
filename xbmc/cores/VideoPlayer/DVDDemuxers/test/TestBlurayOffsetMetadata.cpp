/*
 *  Copyright (C) 2026 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#include "cores/VideoPlayer/DVDDemuxers/BlurayOffsetMetadata.h"

#include <algorithm>
#include <ranges>
#include <vector>

#include <gtest/gtest.h>

using namespace KODI::VIDEO::BLURAY;

namespace
{

constexpr double FRAME{1000.0};

std::vector<uint8_t> Uuid()
{
  return {0x17, 0xee, 0x8c, 0x60, 0xf8, 0x4d, 0x11, 0xd9,
          0x8c, 0xd6, 0x08, 0x00, 0x20, 0x0c, 0x9a, 0x66};
}

/*!
 * \brief An access unit holding one offset metadata SEI.
 *
 * \param nesting the mvc_scalable_nesting header the message is wrapped in, which differs
 *                between encoders - one byte where the message applies to all view
 *                components, two where it names an operation point
 */
std::vector<uint8_t> AccessUnit(const std::vector<uint8_t>& nesting,
                                unsigned int sequences,
                                unsigned int frames,
                                const std::vector<uint8_t>& offsets,
                                bool delimiter = true)
{
  std::vector<uint8_t> unit;

  if (delimiter)
  {
    // An access unit delimiter, as the dependent view carries one of its own.
    unit.insert(unit.end(), {0x00, 0x00, 0x00, 0x01, 0x09, 0x10});
  }

  std::vector<uint8_t> payload{nesting};
  payload.push_back(5); // user_data_unregistered
  const std::vector<uint8_t> uuid{Uuid()};
  const size_t size{uuid.size() + 4 + 10 + offsets.size()};
  for (size_t left = size; left >= 255; left -= 255)
    payload.push_back(0xff);
  payload.push_back(static_cast<uint8_t>(size % 255));
  payload.insert(payload.end(), uuid.begin(), uuid.end());
  payload.insert(payload.end(), {'O', 'F', 'M', 'D'});
  payload.insert(payload.end(),
                 {0x81, 0x00, 0x00, 0x00, 0x00, 0x00, static_cast<uint8_t>(0x80 | sequences),
                  static_cast<uint8_t>(frames), 0x80, 0x00});
  payload.insert(payload.end(), offsets.begin(), offsets.end());

  unit.insert(unit.end(), {0x00, 0x00, 0x00, 0x01, 0x06, 37});
  for (size_t left = payload.size(); left >= 255; left -= 255)
    unit.push_back(0xff);
  unit.push_back(static_cast<uint8_t>(payload.size() % 255));
  unit.insert(unit.end(), payload.begin(), payload.end());

  // A slice of the dependent view, which is what the rest of the access unit is.
  unit.insert(unit.end(), {0x00, 0x00, 0x00, 0x01, 0x14, 0x01, 0x02});

  return unit;
}

bool Parse(const std::vector<uint8_t>& unit, OffsetMetadata& metadata)
{
  return ParseOffsetMetadata(unit.data(), unit.size(), metadata);
}

} // unnamed namespace

TEST(TestBlurayOffsetMetadata, ReadsOffsetsInSequenceMajorOrder)
{
  OffsetMetadata metadata;
  ASSERT_TRUE(Parse(AccessUnit({0x40}, 2, 3, {1, 2, 3, 10, 11, 12}), metadata));

  EXPECT_EQ(metadata.sequences, 2U);
  EXPECT_EQ(metadata.frames, 3U);
  EXPECT_EQ(metadata.Offset(0, 0), 1);
  EXPECT_EQ(metadata.Offset(0, 2), 3);
  EXPECT_EQ(metadata.Offset(1, 0), 10);
  EXPECT_EQ(metadata.Offset(1, 2), 12);
}

TEST(TestBlurayOffsetMetadata, ReadsAMessageNamingAnOperationPoint)
{
  OffsetMetadata metadata;
  ASSERT_TRUE(Parse(AccessUnit({0xc0, 0x10}, 1, 2, {7, 8}), metadata));

  EXPECT_EQ(metadata.Offset(0, 0), 7);
  EXPECT_EQ(metadata.Offset(0, 1), 8);
}

TEST(TestBlurayOffsetMetadata, ReadsADirectionBitAsTheSign)
{
  OffsetMetadata metadata;
  ASSERT_TRUE(Parse(AccessUnit({0x40}, 2, 2, {0x05, 0x85, 0x80, 0x80}), metadata));

  EXPECT_EQ(metadata.Offset(0, 0), 5);
  EXPECT_EQ(metadata.Offset(0, 1), -5);

  // An unused sequence reads as a direction with no magnitude, which is no offset.
  EXPECT_EQ(metadata.Offset(1, 0), 0);
  EXPECT_EQ(metadata.Offset(1, 1), 0);
}

TEST(TestBlurayOffsetMetadata, ReadsThroughEmulationPreventionBytes)
{
  std::vector<uint8_t> unit{AccessUnit({0x40}, 1, 4, {0x01, 0x02, 0x03, 0x09})};

  // The header's run of zeroes is where an encoder has to escape, so put one there.
  const auto marker = std::ranges::search(unit, std::vector<uint8_t>{'O', 'F', 'M', 'D'});
  ASSERT_FALSE(marker.empty());
  unit.insert(marker.begin() + 8, 0x03);

  OffsetMetadata metadata;
  ASSERT_TRUE(Parse(unit, metadata));
  EXPECT_EQ(metadata.frames, 4U);
  EXPECT_EQ(metadata.Offset(0, 0), 1);
  EXPECT_EQ(metadata.Offset(0, 3), 9);
}

TEST(TestBlurayOffsetMetadata, IgnoresAnAccessUnitWithoutMetadata)
{
  const std::vector<uint8_t> unit{0x00, 0x00, 0x00, 0x01, 0x09, 0x10, 0x00,
                                  0x00, 0x00, 0x01, 0x14, 0x01, 0x02};

  OffsetMetadata metadata;
  EXPECT_FALSE(Parse(unit, metadata));
}

TEST(TestBlurayOffsetMetadata, RejectsMetadataThatDoesNotFit)
{
  std::vector<uint8_t> unit{AccessUnit({0x40}, 4, 8, {1, 2, 3})};

  OffsetMetadata metadata;
  EXPECT_FALSE(Parse(unit, metadata));
}

TEST(TestBlurayOffsetMetadata, RejectsAnImpossibleSequenceCount)
{
  // 63 sequences: the field's two top bits are markers, so it cannot exceed 32.
  OffsetMetadata metadata;
  EXPECT_FALSE(Parse(AccessUnit({0x40}, 63, 1, std::vector<uint8_t>(63, 1)), metadata));
}

TEST(TestBlurayOffsetMetadata, StoreFindsTheFrameAtATime)
{
  OffsetMetadata metadata;
  ASSERT_TRUE(Parse(AccessUnit({0x40}, 2, 3, {1, 2, 3, 10, 11, 12}), metadata));

  COffsetMetadataStore store;
  store.Add(5000.0, FRAME, std::move(metadata));

  EXPECT_EQ(store.GetOffset(5000.0, 0), 1);
  EXPECT_EQ(store.GetOffset(6000.0, 0), 2);
  EXPECT_EQ(store.GetOffset(7000.0, 1), 12);

  // Close enough to a frame is that frame; the presented time need not be exact.
  EXPECT_EQ(store.GetOffset(6100.0, 0), 2);

  // Outside what the block describes, and for a sequence it does not carry.
  EXPECT_EQ(store.GetOffset(4000.0, 0), 0);
  EXPECT_EQ(store.GetOffset(8000.0, 0), 0);
  EXPECT_EQ(store.GetOffset(5000.0, 5), 0);
}

TEST(TestBlurayOffsetMetadata, StoreForgetsWhatItIsToldTo)
{
  OffsetMetadata metadata;
  ASSERT_TRUE(Parse(AccessUnit({0x40}, 1, 1, {4}), metadata));

  COffsetMetadataStore store;
  store.Add(0.0, FRAME, std::move(metadata));
  EXPECT_EQ(store.GetOffset(0.0, 0), 4);

  store.Flush();
  EXPECT_EQ(store.GetOffset(0.0, 0), 0);
}

TEST(TestBlurayOffsetMetadata, StoreKeepsTheMostRecentBlocks)
{
  COffsetMetadataStore store;

  // More blocks than it keeps: the oldest go, the newest stay.
  for (int i = 0; i < 100; ++i)
  {
    OffsetMetadata metadata;
    ASSERT_TRUE(Parse(AccessUnit({0x40}, 1, 1, {static_cast<uint8_t>(1 + i % 9)}), metadata));
    store.Add(i * FRAME, FRAME, std::move(metadata));
  }

  EXPECT_EQ(store.GetOffset(99 * FRAME, 0), 1 + 99 % 9);
  EXPECT_EQ(store.GetOffset(0.0, 0), 0);
}
