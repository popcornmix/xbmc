// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "utils/DVBridgeKernelLog.h"
#include "utils/log.h"
#include "ServiceBroker.h"
#include "jobs/JobManager.h"

#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <mutex>
#include <thread>
#include <fcntl.h>
#include <time.h>
#include <unistd.h>

namespace DVBRIDGE
{
// Used only by a Kodi background job after a rate-limited DV output failure
// or selected-route change/initialization.
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
    m_filter.BeginCollection();
    if (m_fd < 0)
    {
      m_fd = open("/dev/kmsg", O_RDONLY | O_NONBLOCK | O_CLOEXEC);
      if (m_fd < 0)
      {
        const int error = errno;
        if (!m_warned)
          CLog::Log(LOGWARNING, "DVBridge kernel coverage: complete=false reason=open-error error={} interval=unavailable", error);
        m_warned = true;
        return false;
      }
    }
    timespec now{};
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0)
    {
      const int error = errno;
      if (!m_warned)
        CLog::Log(LOGWARNING, "DVBridge kernel coverage: complete=false reason=clock-error error={} interval=unavailable", error);
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
    unsigned int suppressed = 0, overruns = 0, matched = 0;
    bool continuation = true, complete = false;
    int error = 0;
    const char* reason = "scan-limit";
    for (unsigned int records = 0; records < 4096; ++records)
    {
      if (std::chrono::steady_clock::now() >= deadline)
        break;
      const auto count = read(m_fd, buffer.data(), buffer.size());
      if (count < 0)
      {
        const int readError = errno;
        if (readError == EPIPE) { ++overruns; continue; }
        if (readError == EINTR) continue;
        complete = readError == EAGAIN;
        reason = complete ? "cursor-drained" : "read-error";
        error = complete ? 0 : readError;
        continuation = false;
        break;
      }
      if (!count) { complete = true; continuation = false; reason = "cursor-eof"; break; }
      const auto message = m_filter.Accept({buffer.data(), static_cast<size_t>(count)}, nowUs);
      if (!message)
        continue;
      ++matched;
      if (m_copied < 8)
      {
        const auto level = message->find(" route_evidence=kernel-unscoped ") != std::string::npos
                               ? LOGINFO : LOGERROR;
        CLog::Log(level, "DVBridge kernel: {}", *message);
        ++m_copied;
      }
      else
        ++suppressed;
    }
    complete = complete && !overruns && !suppressed && !m_filter.Incomplete();
    if (m_notices++ < 2)
      CLog::Log(LOGWARNING,
          "DVBridge kernel coverage: complete={} reason={} error={} requested_from_boot_us={} to_boot_us={} matched={} ring_overruns={} suppressed={} filter_incomplete={} coverage_scope=retained-cursor history_before_cursor=unknown route_evidence_may_precede_interval=true",
          complete, reason, error ? std::to_string(error) : "unavailable",
          nowUs > 60000000 ? nowUs - 60000000 : 0, nowUs, matched, overruns, suppressed,
          m_filter.Incomplete());
    return continuation;
  }

private:
  std::mutex m_mutex;
  KernelLogFilter m_filter;
  int m_fd{-1};
  bool m_warned{false};
  uint64_t m_window{0};
  unsigned m_copied{0}, m_notices{0};
};

// One cursor/job/output budget shared by route initialization and failures.
// Keep all kmsg IO off Kodi's render thread, with the existing batch limits.
inline void CollectKernelLog()
{
  static std::atomic<bool> pending{false};
  auto jobs = CServiceBroker::GetJobManager();
  if (!jobs || pending.exchange(true))
    return;
  try
  {
    jobs->Submit([] {
      try
      {
        static KernelLogReader reader;
        for (unsigned pass = 0; pass < 3 && reader.Collect(); ++pass)
          std::this_thread::sleep_for(std::chrono::milliseconds(10));
      }
      catch (...) {}
      pending.store(false);
    });
  }
  catch (...) { pending.store(false); }
}
}
