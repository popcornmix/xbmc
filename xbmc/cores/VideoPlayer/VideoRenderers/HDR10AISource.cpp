/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "HDR10AISource.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <limits>

HDR10AISource::~HDR10AISource() { cb1_hdr10_ai_destroy(&m_context); }

void HDR10AISource::ReleaseRetired()
{
  m_job = m_active = m_fallback = m_resetPending = false;
  m_lease.reset();
  m_prepared.reset();m_committed.reset();
  m_activation.Stop();
}

bool HDR10AISource::Matches(DVBRIDGE::SourceBinding binding) const
{
  return m_active && binding.Valid() && binding.stream == m_binding.stream &&
      binding.revision == m_binding.revision;
}

cb1_ai_identity HDR10AISource::Identity(DVBRIDGE::SourceBinding binding) const
{
  return {binding.stream,binding.picture,m_revision};
}

cb1_ai_status HDR10AISource::Collect()
{
  if (!m_job) return CB1_AI_READY;
  const auto status = cb1_hdr10_ai_submit(m_context,nullptr,m_jobIdentity,m_jobPts);
  if (status == CB1_AI_READY)
  {
    m_job = false;
    m_lastSubmitted = m_jobIdentity.picture;
    m_lease.reset();
  }
  return RecordStatus("analysis-collect",status);
}

cb1_ai_status HDR10AISource::Begin(pl_gpu gpu, const char* bundle,
    const DVBRIDGE::OutputPreferences& preferences, DVBRIDGE::SourceBinding binding,
    double pts, uint64_t now)
{
  if (!binding.Valid() || !std::isfinite(pts) || !gpu)
    return RecordStatus("analysis-begin",CB1_AI_INVALID);
  if (m_gpu && m_gpu != gpu)
    return RecordStatus("analysis-begin",CB1_AI_INCOMPATIBLE);
  if (!m_resetPending || !Current(binding,preferences))
  {
    m_diagnosticStage = "none";
    m_diagnosticStatus = CB1_AI_READY;
    m_diagnosticFailure = false;
    m_preferences = preferences;
    m_binding = binding;
    m_origin = pts;
    m_revision = DVBRIDGE::NextRevisionAfter(std::max(preferences.revision,binding.revision));
    m_prepared.reset();m_committed.reset();m_preparedBinding = {};
    m_fallback = false;
    m_active = preferences.hdr10Source != DVBRIDGE::HDR10SourceMode::Native;
    m_resetPending = true;
    if (m_active)
    {
      m_activation.Begin(binding.stream,m_revision,now);
      m_activation.SetPaused(m_paused,now);
    }
    else m_activation.Stop();
  }
  if (m_fallback) return CB1_AI_INCOMPATIBLE;
  // A policy/seek reset must not release a decoder surface still read by the GPU.
  if (m_job)
  {
    const auto status = Collect();
    if (status != CB1_AI_READY)
    {
      if (m_active && m_activation.Observe(status,Identity(binding),now) == Decision::Native)
      {
        m_fallback = true;
        return CB1_AI_INCOMPATIBLE;
      }
      return CB1_AI_PENDING;
    }
  }
  m_resetPending = false;
  m_job = false;
  m_lastSubmitted = 0;
  if (!m_active)
  {
    cb1_hdr10_ai_destroy(&m_context);
    m_gpu = nullptr;
    m_activation.Stop();
    return CB1_AI_READY;
  }
  auto status = CB1_AI_READY;
  if (!m_context)
    status = cb1_hdr10_ai_create(gpu,bundle,&m_context);
  RecordStatus("ai-create",status);
  if (status != CB1_AI_READY)
  {
    m_activation.Observe(CB1_AI_INCOMPATIBLE,Identity(binding),now);
    m_fallback = true;
    return status;
  }
  m_gpu = gpu;
  cb1_hdr10_ai_reset(m_context,binding.stream,m_revision);
  if (!cb1_hdr10_ai_set_coalesced_bounds(m_context,true))
    return RecordStatus("analysis-configuration",CB1_AI_INVALID);
  return CB1_AI_READY;
}

cb1_ai_status HDR10AISource::Submit(const pl_frame* frame, DVBRIDGE::SourceBinding binding,
    double pts, double headPts, std::shared_ptr<void> lease)
{
  if (!Matches(binding) || m_fallback || !m_context || !std::isfinite(pts) ||
      !std::isfinite(headPts) || pts < m_origin || pts < headPts || pts > headPts + 1.0 + 1e-9)
    return RecordStatus("analysis-submit",CB1_AI_INVALID);
  const auto collected = Collect();
  if (collected != CB1_AI_READY) return collected;
  const auto submissions = Metrics().gpu_submissions;
  const auto status = cb1_hdr10_ai_submit(m_context,frame,Identity(binding),pts-m_origin);
  // Failed partial batches still read the decoder surface until retirement.
  if (status == CB1_AI_PENDING || Metrics().gpu_submissions != submissions)
  {
    m_job = true;m_jobIdentity = Identity(binding);m_jobPts = pts-m_origin;
    m_lease = std::move(lease);
  }
  else if (status == CB1_AI_READY) m_lastSubmitted = binding.picture;
  return RecordStatus("analysis-submit",status);
}

cb1_ai_status HDR10AISource::Seal(double pts)
{
  if (!m_context || !Active() || !std::isfinite(pts) || pts < m_origin)
    return RecordStatus("temporal-seal",CB1_AI_INVALID);
  const auto status = Collect();
  return status == CB1_AI_READY ?
      RecordStatus("temporal-seal",cb1_hdr10_ai_advance(m_context,pts-m_origin)) : status;
}

void HDR10AISource::End()
{
  if (m_context && Active()) cb1_hdr10_ai_end(m_context);
}

HDR10AISource::Decision HDR10AISource::Prediction(DVBRIDGE::SourceBinding binding,
    const DVBRIDGE::OutputPreferences& preferences, uint64_t now, cb1_ai_result* result,
    bool required)
{
  if (!result || !Matches(binding) || preferences.revision != m_preferences.revision ||
      !preferences.SameValues(m_preferences))
    return Decision::Ignore;
  if (m_fallback) return Decision::Native;
  if (m_committed && m_committed->id.picture == binding.picture)
  {
    *result = *m_committed;
    return Decision::Prepare;
  }
  if (m_prepared && binding == m_preparedBinding)
  {
    *result = *m_prepared;
    return m_activation.Observe(CB1_AI_READY,Identity(binding),now);
  }
  // Speculative preparation must not replace an image awaiting presentation.
  if (!required && m_prepared &&
      (!m_committed || m_prepared->id.picture != m_committed->id.picture))
    return Decision::Wait;
  const auto started=std::chrono::steady_clock::now();
  cb1_ai_result prediction{};
  const auto collected = Collect();
  const auto status = collected == CB1_AI_READY || collected == CB1_AI_PENDING ?
      cb1_hdr10_ai_poll(m_context,Identity(binding),&prediction) : collected;
  RecordStatus("prediction",status,false);
  const auto elapsed=static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::steady_clock::now()-started).count());
  const auto completed=now+std::min(elapsed,std::numeric_limits<uint64_t>::max()-now);
  const auto decision = m_activation.Observe(status,Identity(binding),completed,required);
  if (decision == Decision::Prepare)
  {
    m_prepared = prediction;m_preparedBinding = binding;*result = prediction;
  }
  if (decision == Decision::Native) m_fallback = true;
  return decision;
}

bool HDR10AISource::Presented(DVBRIDGE::SourceBinding binding, bool success)
{
  if (Matches(binding) && m_committed && m_committed->id.picture == binding.picture)
  {
    if (!success) m_fallback = true;
    return success;
  }
  if (binding != m_preparedBinding || !m_prepared ||
      !m_activation.Presented(m_prepared->id,success))
  {
    if (binding == m_preparedBinding && !success) m_fallback = true;
    return false;
  }
  m_committed = m_prepared;
  return true;
}

void HDR10AISource::DiscardBefore(DVBRIDGE::SourceBinding binding, uint64_t now)
{
  if (Matches(binding) && AwaitingPresentation() &&
      binding.picture > m_preparedBinding.picture)
  {
    if (m_activation.CancelPrepared(m_prepared->id, now) == Decision::Native)
      m_fallback = true;
    // Kodi discarded this image; no presentation state is committed.
    m_prepared.reset();
    m_preparedBinding = {};
  }
}

HDR10AISource::Decision HDR10AISource::Preparation(DVBRIDGE::SourceBinding binding,
    cb1_ai_status status, uint64_t now)
{
  if (!Matches(binding) || binding != m_preparedBinding || !m_prepared)
    return Decision::Ignore;
  const auto decision = m_activation.Observe(status, m_prepared->id, now);
  if (decision == Decision::Native) m_fallback = true;
  return decision;
}

struct cb1_hdr10_ai_metrics HDR10AISource::Metrics() const
{
  struct cb1_hdr10_ai_metrics metrics{};
  if (m_context) cb1_hdr10_ai_metrics(m_context,&metrics);
  return metrics;
}
