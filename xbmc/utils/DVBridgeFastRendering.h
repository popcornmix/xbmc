/* SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once
#include <cstdint>
#include <algorithm>
#include <optional>
#include <cctype>
#include <cmath>
#include <string>
#include <string_view>

namespace DVBRIDGE
{
inline bool FastRenderingEnabled(int mode, bool automatic)
{
  return mode == 1 || (mode == 2 && automatic);
}

inline bool ContinuousFastRenderingPicture(double currentUs, double previousUs)
{
  // Kodi picture timestamps use microseconds; seeks and repeats are excluded.
  return std::isfinite(currentUs) && std::isfinite(previousUs) &&
      currentUs > previousUs && currentUs - previousUs < 250000;
}

inline std::optional<int> LegacyFastRendering(std::string_view value)
{
  std::string text(value);
  std::transform(text.begin(), text.end(), text.begin(),
      [](unsigned char c) { return std::tolower(c); });
  if (text == "false") return 0;
  if (text == "true") return 1;
  return std::nullopt;
}

inline std::optional<int64_t> PresentationHeadroom(uint64_t previousNs, uint64_t readyNs,
    uint64_t submittedNs, uint64_t presentedNs, uint64_t periodNs)
{
  if (!previousNs || !readyNs || !submittedNs || periodNs < 1000000 || periodNs > 100000000 ||
      presentedNs <= previousNs || readyNs > presentedNs || submittedNs > presentedNs ||
      presentedNs - previousNs > 4 * periodNs)
    return std::nullopt;
  // A delayed actual presentation must not move the missed deadline forward.
  return static_cast<int64_t>(previousNs) + static_cast<int64_t>(periodNs) -
      static_cast<int64_t>(std::max(readyNs, submittedNs));
}

// Two low-margin samples within 250 ms; latched for the current playback.
class FastRendering
{
public:
  bool Enabled() const { return m_enabled; }
  void Reset() { *this = {}; }
  bool Observe(uint64_t nowNs, int64_t marginNs, bool eligible)
  {
    if (m_enabled) return false;
    if (!m_startNs && nowNs) m_startNs = nowNs;
    if (!eligible || !nowNs || nowNs <= m_lastNs)
    {
      m_warmFrames = 0;
      m_alertNs = 0;
      m_lastNs = nowNs;
      return false;
    }
    m_lastNs = nowNs;
    if (m_warmFrames < 8)
    {
      ++m_warmFrames;
      return false;
    }
    // Ignore startup timing while Kodi establishes the playback cadence.
    if (nowNs - m_startNs < 8000000000ULL) return false;
    if (marginNs >= 2500000) return false;
    if (m_alertNs && nowNs - m_alertNs <= 250000000)
    {
      m_enabled = true;
      return true;
    }
    m_alertNs = nowNs;
    return false;
  }
private:
  bool m_enabled{};
  unsigned m_warmFrames{};
  uint64_t m_startNs{}, m_lastNs{}, m_alertNs{};
};
}
