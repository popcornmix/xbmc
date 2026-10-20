/* SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once
#include "DVBridgeDiagnosticState.h"
#include "utils/log.h"
#include <algorithm>
#include <mutex>

namespace DVBRIDGE
{
inline std::mutex diagnosticMutex;
inline DiagnosticSession diagnosticSession;
inline bool diagnosticPaused{};

inline void DiagnosticOpen() noexcept
{
  try { std::lock_guard lock(diagnosticMutex); diagnosticSession.Open(); diagnosticPaused = false; }
  catch (...) {}
}

inline void DiagnosticCandidate() noexcept
{
  if (!DiagnosticsEnabled()) return;
  try { std::lock_guard lock(diagnosticMutex); diagnosticSession.Candidate(DiagnosticNowMs()); }
  catch (...) {}
}

// 896 bytes leaves 128 bytes for Kodi's normal timestamp/thread/severity prefix.
// format_to_n bounds allocation even for a backend's arbitrarily long message.
template<class... Args>
bool WriteDiagnosticLocked(DiagnosticEvent event, int level,
                           fmt::format_string<Args...> format, Args&&... args)
{
  if (!diagnosticSession.Admit(DiagnosticNowMs(), event)) return false;
  std::array<char, 896> buffer{};
  const auto formatted = fmt::format_to_n(buffer.data(), 640, format, std::forward<Args>(args)...);
  const bool truncated = formatted.size > 640;
  if (truncated) diagnosticSession.Truncated();
  std::string message(buffer.data(), std::min<size_t>(formatted.size, 640));
  // A producer cannot inject extra physical lines into the shared line budget.
  for (char& ch : message) if (ch == '\n' || ch == '\r' || ch == '\0') ch = ' ';
  if (truncated)
  {
    // Discard a partial UTF-8 tail before appending the ASCII marker.
    while (!message.empty() && (static_cast<unsigned char>(message.back()) & 0xc0) == 0x80)
      message.pop_back();
    if (!message.empty() && static_cast<unsigned char>(message.back()) >= 0xc0) message.pop_back();
    message += " [truncated]";
  }
  message += fmt::format(" stream={} picture={} revision={} policy={} suppressed={} truncations={}",
      diagnosticSession.Stream(), diagnosticSession.Picture(), diagnosticSession.Revision(),
      diagnosticSession.Policy(), diagnosticSession.Suppressed(), diagnosticSession.Truncations());
  CLog::Log(level, "{}", message);
  return true;
}

template<class... Args>
bool DiagnosticLog(int level, fmt::format_string<Args...> format, Args&&... args) noexcept
{
  if (!DiagnosticsEnabled()) return false;
  try
  {
    std::lock_guard lock(diagnosticMutex);
    return WriteDiagnosticLocked(DiagnosticEvent::Normal, level, format, std::forward<Args>(args)...);
  }
  catch (...) { return false; } // Diagnostics cannot escape into a C callback or playback.
}

template<class... Args>
void DiagnosticFailureLog(const char* stage, bool changed, int level,
                          fmt::format_string<Args...> format, Args&&... args) noexcept
{
  if (!DiagnosticsEnabled()) return;
  try
  {
    std::lock_guard lock(diagnosticMutex);
    diagnosticSession.Failure(stage);
    if (!changed && diagnosticSession.FailureReported()) { diagnosticSession.Suppress(); return; }
    WriteDiagnosticLocked(DiagnosticEvent::Failure, level, format, std::forward<Args>(args)...);
  }
  catch (...) {}
}

inline void ObserveDiagnostic(uint64_t stream, uint64_t picture, uint64_t revision,
                              uint64_t policy, const char* stage, bool requested, bool paused) noexcept
{
  if (!DiagnosticsEnabled()) return;
  try
  {
    std::lock_guard lock(diagnosticMutex);
    const auto now = DiagnosticNowMs();
    const bool same = stream == diagnosticSession.Stream() && revision == diagnosticSession.Revision() &&
        policy == diagnosticSession.Policy();
    const char* previous = diagnosticSession.Stage();
    diagnosticSession.Observe(now,stream,picture,revision,policy,stage,requested,paused || diagnosticPaused);
    if (diagnosticSession.WaitDue(now))
      WriteDiagnosticLocked(DiagnosticEvent::Normal, LOGWARNING,
          "DVBridge waiting: first_present=false last_stage={} first_failure={}",
          same ? previous : diagnosticSession.Stage(), diagnosticSession.FirstFailure());
  }
  catch (...) {}
}

inline void DiagnosticPresented(uint64_t stream) noexcept
{
  if (!DiagnosticsEnabled()) return;
  try { std::lock_guard lock(diagnosticMutex); diagnosticSession.Presented(stream); }
  catch (...) {}
}

inline void DiagnosticPause(bool paused) noexcept
{
  if (!DiagnosticsEnabled()) return;
  try
  {
    std::lock_guard lock(diagnosticMutex);
    diagnosticPaused = paused;
    diagnosticSession.Paused(DiagnosticNowMs(), paused);
  }
  catch (...) {}
}

inline void DiagnosticStage(const char* stage) noexcept
{
  if (!DiagnosticsEnabled()) return;
  try { std::lock_guard lock(diagnosticMutex); diagnosticSession.Stage(stage); }
  catch (...) {}
}

// Called by the existing player loop, including starts with no decoded picture.
inline void DiagnosticTick(bool requested) noexcept
{
  if (!DiagnosticsEnabled()) return;
  try
  {
    std::lock_guard lock(diagnosticMutex);
    if (!requested) diagnosticSession.DisableRequest();
    if (diagnosticSession.WaitDue(DiagnosticNowMs()))
      WriteDiagnosticLocked(DiagnosticEvent::Normal, LOGWARNING,
          "DVBridge waiting: first_present=false last_stage={} first_failure={}",
          diagnosticSession.Stage(),diagnosticSession.FirstFailure());
  }
  catch (...) {}
}

inline void DiagnosticFinal(const char* stop) noexcept
{
  if (!DiagnosticsEnabled()) return;
  try
  {
    std::lock_guard lock(diagnosticMutex);
    WriteDiagnosticLocked(DiagnosticEvent::Final, LOGINFO,
        "DVBridge playback end: stop={} first_present={} last_stage={} first_failure={}", stop,
        diagnosticSession.FirstPresented(), diagnosticSession.Stage(), diagnosticSession.FirstFailure());
  }
  catch (...) {}
}
}
