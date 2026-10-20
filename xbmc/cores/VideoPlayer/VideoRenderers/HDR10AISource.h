/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

#include "cores/VideoPlayer/DVDCodecs/Video/DVOutputSettings.h"
#include "cores/VideoPlayer/DVDCodecs/Video/HDR10AIActivation.h"
extern "C"
{
#include "cb1_hdr10_ai.h"
}
#include <optional>

// Render-thread confined. The owning GL context must retire GPU use before destruction.
class HDR10AISource
{
public:
  using Decision = DVBRIDGE::HDR10AIActivation::Decision;
  HDR10AISource() = default;
  HDR10AISource(const HDR10AISource&) = delete;
  HDR10AISource& operator=(const HDR10AISource&) = delete;
  ~HDR10AISource();
  // Called only after the owning context's retirement fence has completed.
  void ReleaseRetired();
  cb1_ai_status Begin(pl_gpu gpu, const char* bundle, const DVBRIDGE::OutputPreferences& preferences,
      DVBRIDGE::SourceBinding binding, double pts, uint64_t now);
  cb1_ai_status Submit(const pl_frame* frame, DVBRIDGE::SourceBinding binding, double pts,
      double headPts, std::shared_ptr<void> lease);
  cb1_ai_status Seal(double pts);
  void End();
  Decision Prediction(DVBRIDGE::SourceBinding binding,
      const DVBRIDGE::OutputPreferences& preferences, uint64_t now, cb1_ai_result* result,
      bool required = true);
  bool Presented(DVBRIDGE::SourceBinding binding, bool success);
  void DiscardBefore(DVBRIDGE::SourceBinding binding);
  Decision Preparation(DVBRIDGE::SourceBinding binding, cb1_ai_status status, uint64_t now);
  bool FallbackEvent() { return m_activation.TakeFallbackEvent(); }
  bool Active() const { return m_active && !m_fallback; }
  bool ResetPending() const { return m_resetPending; }
  bool Current(DVBRIDGE::SourceBinding binding, const DVBRIDGE::OutputPreferences& preferences) const
  {
    return Matches(binding) && preferences.revision == m_preferences.revision &&
        preferences.SameValues(m_preferences);
  }
  uint64_t LastSubmitted() const { return m_lastSubmitted; }
  void SetPresentationDeadline(uint64_t deadline) { m_activation.SetPresentationDeadline(deadline); }
  void SetPaused(bool paused, uint64_t now)
  {
    m_paused = paused;
    m_activation.SetPaused(paused,now);
  }
  const std::optional<cb1_ai_result>& Committed() const { return m_committed; }
  bool AwaitingPresentation() const
  {
    return m_prepared && (!m_committed || m_prepared->id.picture != m_committed->id.picture);
  }
  struct cb1_hdr10_ai_metrics Metrics() const;
  cb1_ai_status Collect();

private:
  cb1_ai_identity Identity(DVBRIDGE::SourceBinding binding) const;
  bool Matches(DVBRIDGE::SourceBinding binding) const;
  cb1_hdr10_ai* m_context{};
  pl_gpu m_gpu{};
  DVBRIDGE::HDR10AIActivation m_activation;
  DVBRIDGE::OutputPreferences m_preferences;
  DVBRIDGE::SourceBinding m_binding{}, m_preparedBinding{};
  cb1_ai_identity m_jobIdentity{};
  std::shared_ptr<void> m_lease;
  std::optional<cb1_ai_result> m_prepared, m_committed;
  uint64_t m_revision{};
  uint64_t m_lastSubmitted{};
  double m_origin{}, m_jobPts{};
  bool m_active{}, m_job{}, m_fallback{}, m_resetPending{}, m_paused{};
};
