// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "utils/DVBridgeKernelLog.h"
#include "utils/log.h"

#include <array>
#include <cerrno>
#include <chrono>
#include <mutex>
#include <fcntl.h>
#include <time.h>
#include <unistd.h>

namespace DVBRIDGE
{
// Used only by a Kodi background job after a rate-limited DV output failure.
// Read-only, nonblocking and bounded. No external process, new service or GPU work.
class KernelLogReader
{
public:
  ~KernelLogReader()
  {
    if (m_fd >= 0)
      close(m_fd);
  }

  // True means the independent cursor still has work; callers may run a small
  // bounded continuation after yielding. Never blocks the render thread.
  bool Collect()
  {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_fd < 0)
    {
      m_fd = open("/dev/kmsg", O_RDONLY | O_NONBLOCK | O_CLOEXEC);
      if (m_fd < 0)
      {
        if (!m_warned)
          CLog::Log(LOGWARNING, "DVBridge kernel: cannot read /dev/kmsg errno={}; kernel reason unavailable", errno);
        m_warned = true;
        return false;
      }
    }
    timespec now{};
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0)
    {
      if (!m_warned)
        CLog::Log(LOGWARNING, "DVBridge kernel: monotonic clock unavailable; evidence incomplete");
      m_warned = true;
      return false;
    }
    const uint64_t nowUs = uint64_t(now.tv_sec) * 1000000 + now.tv_nsec / 1000;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(50);
    std::array<char, 16384> buffer{};
    // The output budget spans continuation batches, not just one read call.
    if (!m_window || nowUs - m_window >= 30000000)
    {
      m_window = nowUs;
      m_copied = 0;
      m_notices = 0;
    }
    unsigned int suppressed = 0, overruns = 0;
    bool incomplete = true;
    for (unsigned int records = 0; records < 4096; ++records)
    {
      if (std::chrono::steady_clock::now() >= deadline)
        break;
      const auto count = read(m_fd, buffer.data(), buffer.size());
      if (count < 0)
      {
        if (errno == EPIPE) { ++overruns; continue; }
        if (errno == EINTR) continue;
        if (errno != EAGAIN && !m_warned)
        {
          CLog::Log(LOGWARNING, "DVBridge kernel: read failed errno={}; kernel reason unavailable", errno);
          m_warned = true;
        }
        incomplete = false;
        break;
      }
      if (!count) { incomplete = false; break; }
      const auto message = m_filter.Accept({buffer.data(), static_cast<size_t>(count)}, nowUs);
      if (!message)
        continue;
      if (m_copied < 8)
      {
        CLog::Log(LOGERROR, "DVBridge kernel: {}", *message);
        ++m_copied;
      }
      else
        ++suppressed;
    }
    if ((suppressed || overruns || incomplete || m_filter.Incomplete()) && m_notices++ < 2)
      CLog::Log(LOGWARNING,
          "DVBridge kernel: evidence incomplete={} ring_overruns={} suppressed={} (bounded collection)",
          incomplete || m_filter.Incomplete(), overruns, suppressed);
    return incomplete;
  }

private:
  std::mutex m_mutex;
  KernelLogFilter m_filter;
  int m_fd{-1};
  bool m_warned{false};
  uint64_t m_window{0};
  unsigned m_copied{0}, m_notices{0};
};
}
