/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

#include <algorithm>

namespace DVBRIDGE
{
// Cap the visible list at six rows; longer lists retain native scrolling.
constexpr int PlaybackMenuRowsHeight(int count)
{
  return 50 * std::clamp(count, 1, 6);
}
}
