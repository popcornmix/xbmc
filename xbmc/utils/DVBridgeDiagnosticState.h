/* SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <array>
#include <optional>
#include <string>
#include <string_view>

namespace DVBRIDGE
{
// Enabled without Kodi's global debug mode. Developers can disable the entire
// supplement in one place; it must never change output or retry decisions.
inline bool DiagnosticsEnabled()
{
  static const bool enabled = [] {
    const char* setting = std::getenv("DVBRIDGE_DIAGNOSTICS");
    return !setting || setting[0] != '0' || setting[1] != '\0';
  }();
  return enabled;
}

inline uint64_t DiagnosticNowMs()
{
  return std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::steady_clock::now().time_since_epoch()).count();
}

struct DiagnosticProperty
{
  bool inventory;
  std::optional<uint64_t> value;
  DiagnosticProperty(bool known, std::optional<uint64_t> cached) : inventory(known),
      value(known ? cached : std::nullopt) {}
  const char* Presence() const { return !inventory ? "unknown" : value ? "present" : "absent"; }
  uint64_t Code() const { return !inventory ? 0 : value ? 2 : 1; }
  std::string Value() const { return value ? std::to_string(*value) : "unavailable"; }
  bool operator==(const DiagnosticProperty&) const = default;
};

enum class DiagnosticEvent { Normal, Failure, Final, Connection };

// One active native stream, shared by every producer. Pure observation: none of
// these facts are consumed by output selection, retry, or GPU synchronization.
// Callers serialize through DiagnosticLog's private diagnostic mutex.
class DiagnosticSession
{
public:
  void Open() { *this = {}; }
  void Candidate(uint64_t now)
  {
    if (!m_requested && !m_stream && !m_ended)
    { m_requested = true; m_waitStart = now; m_stage = "stream-candidate"; }
  }
  void Observe(uint64_t now, uint64_t stream, uint64_t picture, uint64_t revision,
               uint64_t policy, const char* stage, bool requested, bool paused)
  {
    if (!stream || stream < m_stream || (stream == m_stream && m_ended)) return;
    if (stream != m_stream)
    {
      // Binding the first picture identifies the candidate already observed by
      // this playback. It must not grant a second budget or failure exemption.
      if (m_stream) *this = {};
      m_stream = stream; m_waitStart = now;
    }
    if (revision != m_revision || policy != m_policy)
    {
      // A seek/policy generation may clear its reason, never the stream's line
      // budget or first-failure exemption (oscillation cannot refill either).
      m_failure = nullptr; m_waitStart = now; m_waited = false;
    }
    if (!requested || paused || !m_requested || m_paused) m_waitStart = now;
    m_requested = requested; m_paused = paused;
    m_picture = picture; m_revision = revision; m_policy = policy; m_stage = stage;
  }
  bool Admit(uint64_t now, DiagnosticEvent event)
  {
    if (event == DiagnosticEvent::Final)
    {
      if ((!m_stream && !m_requested) || m_ended) return false;
      m_ended = true;
      return true;
    }
    if (event == DiagnosticEvent::Failure && !m_firstFailure)
    { m_firstFailure = true; return true; }
    if (event == DiagnosticEvent::Connection) return true;
    // Clamp backwards observations: no unsigned wrap can refill the budget.
    if (now < m_lastNow) now = m_lastNow;
    m_lastNow = now;
    while (m_size && now >= m_lines[m_head] && now - m_lines[m_head] >= 30000)
    { m_head = (m_head + 1) % m_lines.size(); --m_size; }
    if (m_size == m_lines.size()) { ++m_suppressed; return false; }
    m_lines[(m_head + m_size) % m_lines.size()] = now;
    ++m_size;
    return true;
  }
  bool WaitDue(uint64_t now)
  {
    if (m_ended || !m_requested || m_paused || m_presented ||
        now < m_waitStart || now - m_waitStart < 5000 ||
        (m_waited && (now < m_lastWait || now - m_lastWait < 30000))) return false;
    m_waited = true; m_lastWait = now; return true;
  }
  void Presented(uint64_t stream) { if (stream == m_stream) m_presented = true; }
  void Paused(uint64_t now, bool paused)
  {
    if (m_paused != paused) { m_waitStart = now; m_waited = false; }
    m_paused = paused;
  }
  void Failure(const char* stage) { if (!m_failure) m_failure = stage; m_stage = stage; }
  void Stage(const char* stage) { m_stage = stage; }
  void DisableRequest() { m_requested = false; m_stage = "not-requested"; }
  void Truncated() { ++m_truncated; }
  void Suppress() { ++m_suppressed; }
  bool FailureReported() const { return m_firstFailure; }
  uint64_t Stream() const { return m_stream; }
  uint64_t Picture() const { return m_picture; }
  uint64_t Revision() const { return m_revision; }
  uint64_t Policy() const { return m_policy; }
  uint64_t Suppressed() const { return m_suppressed; }
  uint64_t Truncations() const { return m_truncated; }
  bool FirstPresented() const { return m_presented; }
  const char* Stage() const { return m_stage ? m_stage : "unavailable"; }
  const char* FirstFailure() const { return m_failure ? m_failure : "none"; }
private:
  std::array<uint64_t, 24> m_lines{};
  size_t m_head{}, m_size{};
  uint64_t m_lastNow{}, m_suppressed{}, m_truncated{};
  uint64_t m_stream{}, m_picture{}, m_revision{}, m_policy{};
  uint64_t m_waitStart{}, m_lastWait{};
  const char* m_stage{}, *m_failure{}; // Static labels only.
  bool m_requested{}, m_paused{}, m_presented{}, m_firstFailure{}, m_ended{}, m_waited{};
};

class DiagnosticInterval
{
public:
  bool Due(uint64_t now)
  {
    if (m_reported && (now<m_last || now-m_last<5000)) return false;
    m_reported=true;m_last=now;return true;
  }
private:
  uint64_t m_last{};
  bool m_reported{};
};

// A bounded first-event/change budget plus a 30-second summary. Alternating
// failures/recoveries cannot reset the budget and turn this into per-frame IO.
class DiagnosticFailures
{
public:
  bool Record(uint64_t now, const char* stage = "failure", int error = 0)
  {
    ++m_count;
    if (!m_consecutive) m_firstConsecutive=now;
    ++m_consecutive;
    const bool changed = stage != m_stage || error != m_error;
    m_stage = stage;
    m_error = error;
    if (!m_reported || now - m_window >= 30000)
    {
      m_window = now;
      m_reports = 0;
    }
    if ((!m_reported || changed || now - m_last >= 30000) && m_reports < 4)
    {
      m_reported = true;
      ++m_reports;
      m_last = now;
      return true;
    }
    return false;
  }
  bool Recovered(uint64_t now)
  {
    if (!m_consecutive)
      return false;
    m_consecutive = 0;
    ++m_recoveries;
    if (m_recoveries == 1 || now - m_lastRecovery >= 30000)
    {
      m_lastRecovery = now;
      return true;
    }
    return false;
  }
  uint64_t Count() const { return m_count; }
  uint64_t Recoveries() const { return m_recoveries; }
  bool ReferenceDue(uint64_t now,bool eligible)
  {
    if (!eligible || !m_consecutive) { m_firstConsecutive=now; return false; }
    if (m_referenceWarned || m_consecutive<30 || now<m_firstConsecutive ||
        now-m_firstConsecutive<5000) return false;
    m_referenceWarned=true;
    return true;
  }
  void Reset() { *this = {}; }

private:
  uint64_t m_count{0}, m_consecutive{0}, m_recoveries{0};
  uint64_t m_window{0}, m_last{0}, m_lastRecovery{0};
  unsigned m_reports{0};
  const char* m_stage{nullptr}; // Callers pass static stage labels.
  int m_error{0};
  bool m_reported{false};
  uint64_t m_firstConsecutive{};
  bool m_referenceWarned{};
};
} // namespace DVBRIDGE
