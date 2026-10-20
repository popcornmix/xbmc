/*
 *  Copyright (C) 2005-2018 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#include "DRMAtomic.h"

#include "ServiceBroker.h"
#include "application/ApplicationComponents.h"
#include "application/ApplicationPlayer.h"
#include "guilib/GUIComponent.h"
#include "guilib/GUIWindowManager.h"
#include "settings/Settings.h"
#include "utils/log.h"
#include "utils/DVBridgeDiagnosticLog.h"
#include "DVBridgeKernelLogReader.h"
#include "utils/DVBridgeEdid.h"
#include "utils/DVBridgeFastRendering.h"
#ifdef HAVE_DVBRIDGE
#include "cores/VideoPlayer/DVDCodecs/Video/DVStreamTelemetry.h"
#endif

#include <chrono>
#include <algorithm>
#include <errno.h>
#include <string.h>
#include <thread>

#include <drm_fourcc.h>
#include <drm_mode.h>
#include <fcntl.h>
#include <linux/sync_file.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <unistd.h>

using namespace KODI::WINDOWING::GBM;

namespace
{
uint64_t DVNowMs()
{
  return std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::steady_clock::now().time_since_epoch()).count();
}

uint64_t DVNowNs()
{
  timespec now{};
  return clock_gettime(CLOCK_MONOTONIC, &now) == 0 ?
      uint64_t(now.tv_sec) * 1000000000 + now.tv_nsec : 0;
}

void CollectDVKernelLog()
{
  DVBRIDGE::CollectKernelLog();
}
}

void CDRMAtomic::ReportDVFailure(const char* stage, int error) noexcept try
{
#ifdef HAVE_DVBRIDGE
  if (m_dvRestoring)
    DVBRIDGE::StreamTelemetry::NativeFailure(DVBRIDGE::OutputReason::RestoreFailed,
        error,m_dvRetry.Count(),false);
#endif
  if (!DVBRIDGE::DiagnosticsEnabled()) return;
  const bool changed = m_dvFailures.Record(DVNowMs(), stage, error);
  DVBRIDGE::DiagnosticFailureLog(stage, changed, LOGERROR,
      "DVBridge output: result=rejected stage={} error={} failures={} activation_failures={} connector={} crtc={} mode={}x{} clock_khz={} requested_dv={} property_id={} restoring={}",
      stage, error ? std::to_string(error) : "unavailable", m_dvFailures.Count(), m_dvRetry.Count(),
      m_connector ? m_connector->GetId() : 0, m_crtc ? m_crtc->GetId() : 0,
      m_mode ? m_mode->hdisplay : 0, m_mode ? m_mode->vdisplay : 0,
      m_mode ? m_mode->clock : 0,
      m_req && m_req->requestedDV ? std::to_string(*m_req->requestedDV) : "unavailable",
      m_connector ? m_connector->GetPropertyId("DVBRIDGE_DV_STANDARD_LAB") : 0,m_dvRestoring);
  if (!changed) return;
  // One bounded snapshot per connection identity. Never dump each frame's
  // framebuffer or synchronization objects; they obscure the actual rejection.
  const auto blobId = m_dvLinkKnown ? m_dvLinkBlob :
      (m_connector ? m_connector->GetPropertyValue("EDID").value_or(0) : 0);
  const auto connectorId = m_connector ? m_connector->GetId() : 0;
  if (!m_dvSnapshot || m_dvSnapshotBlob != blobId || m_dvSnapshotConnector != connectorId)
  {
    m_dvSnapshot = true;
    m_dvSnapshotBlob = blobId;
    m_dvSnapshotConnector = connectorId;
    std::vector<uint8_t> edid;
    if (auto* blob = drmModeGetPropertyBlob(m_fd, blobId))
    {
      if (blob->length <= 32768 && blob->data)
        edid.assign(static_cast<uint8_t*>(blob->data),
                    static_cast<uint8_t*>(blob->data) + blob->length);
      drmModeFreePropertyBlob(blob);
    }
    const auto caps = dvbridge_parse_edid(edid.data(), edid.size());
    DVBRIDGE::DiagnosticLog(LOGINFO, "DVBridge sink: edid_bytes={} status={} version={} legacy_vsif={} 2160p60={}",
        edid.size(), dvbridge_edid_reason(caps.status), caps.version,
        caps.legacy_vsif, caps.supports_2160p60);
    if (m_req)
      m_req->LogDVProperties();
  }
  CollectDVKernelLog();
}
catch (...) {}

void CDRMAtomic::RefreshDVLink()
{
  if (!m_connector)
    return;
  // Current is a cached DRM query, not a blocking forced EDID probe. It is
  // used only during failed output, at most once per second.
  auto* current = drmModeGetConnectorCurrent(m_fd, m_connector->GetId());
  if (!current)
    return;
  const auto property = m_connector->GetPropertyId("EDID");
  uint32_t blob = 0;
  for (int i = 0; i < current->count_props; ++i)
    if (current->props[i] == property)
      blob = current->prop_values[i];
  m_dvLinkKnown = true;
  m_dvLinkConnector = current->connector_id;
  m_dvLinkBlob = blob;
  m_dvLinkStatus = current->connection;
  drmModeFreeConnector(current);
}

void CDRMAtomic::ReportDVCommit(const char* stage) noexcept try
{
  DVBRIDGE::DiagnosticLog(LOGINFO,
      "DVBridge activation: stage={} result=accepted requested_dv={} restoring={}", stage,
      m_req && m_req->requestedDV ? std::to_string(*m_req->requestedDV) : "unavailable",m_dvRestoring);
}
catch (...) {}

bool CDRMAtomic::DrmAtomicCommit(int fb_id, int flags, bool rendered, bool videoLayer, bool strict)
{
  CDRMCrtc* oldCrtc = m_old_crtc;
  const auto reject = [&](const char* stage, int error) {
    if (strict)
    {
      if (m_dvActivating && !m_dvRestoring) m_dvRetry.Failed(error);
      ReportDVFailure(stage, error);
    }
    m_old_crtc = oldCrtc;
    FinishRequest(false);
    return false;
  };
  // Declared at function scope so the blob outlives drmModeAtomicCommit.
  // DRM requires the blob to remain alive for the duration of the commit;
  // destroying it before the commit returns leaves the atomic request
  // referencing an invalid id and the kernel rejects with EINVAL.
  CDRMPropertyBlob modeBlob;

  if (m_old_crtc != nullptr)
  {
    if (m_old_crtc->GetId() != m_crtc->GetId())
    {
      AddProperty(m_old_crtc, "ACTIVE", 0);
      AddProperty(m_old_crtc, "MODE_ID", 0);
      flags |= DRM_MODE_ATOMIC_ALLOW_MODESET;
    }

    for (const auto& plane : m_planes)
    {
      if (m_gui_plane != nullptr && m_gui_plane->GetId() == plane->GetId())
        continue;
      if (m_video_plane != nullptr && m_video_plane->GetId() == plane->GetId())
        continue;

      uint64_t planeid = plane->GetPropertyValue("CRTC_ID").value_or(0);
      if (planeid == m_crtc->GetId() || planeid == m_old_crtc->GetId())
      {
        AddProperty(plane.get(), "CRTC_ID", 0);
        AddProperty(plane.get(), "FB_ID", 0);
      }

      // below disables the planes which are not in our crtcs, in other words
      // crts attached to other connectors (ie: 2nd monitor), amdgpu requires at least
      // one primary plane to enable crtcs, if we disable rest of the planes in amdgpu
      // atomic commit will fail
      if (!(HasQuirk(QUIRK_NEEDSPRIMARY)))
      {
        AddProperty(plane.get(), "CRTC_ID", 0);
        AddProperty(plane.get(), "FB_ID", 0);
      }
    }
    m_old_crtc = nullptr;
  }

  if (flags & DRM_MODE_ATOMIC_ALLOW_MODESET)
  {
    if (!AddProperty(m_connector, "CRTC_ID", m_crtc->GetCrtcId()))
      return reject("connector-crtc", EINVAL);

    modeBlob = CDRMPropertyBlob(m_fd, m_mode, sizeof(*m_mode));
    if (!modeBlob.IsValid())
      return reject("mode-blob", 0);

    if (!AddProperty(m_crtc, "MODE_ID", modeBlob.Get()))
      return reject("mode-property", EINVAL);
    // Follow the request actually accepted, including ordinary old-mode fallback.
    // Transport admission consumes this receipt even when diagnostics are disabled.
    m_req->signalMode = *m_mode;

    if (!AddProperty(m_crtc, "ACTIVE", m_active ? 1 : 0))
      return reject("active-property", EINVAL);
  }

  // In Direct-To-Plane (dual plane) mode m_gui_plane is the output
  // (gui overlay on top of m_video_plane). In single-plane flip-flop mode
  // FindVideoPlane has made m_video_plane the output and nulled m_gui_plane.
  // Pick whichever is live.
  CDRMPlane* outputPlane = m_gui_plane ? m_gui_plane : m_video_plane;

  if (rendered)
  {
    AddProperty(outputPlane, "FB_ID", fb_id);
    AddProperty(outputPlane, "CRTC_ID", m_crtc->GetCrtcId());
    AddProperty(outputPlane, "SRC_X", 0);
    AddProperty(outputPlane, "SRC_Y", 0);
    AddProperty(outputPlane, "SRC_W", m_width << 16);
    AddProperty(outputPlane, "SRC_H", m_height << 16);
    // Frame packing scans out (vdisplay + vtotal) lines - the kernel doubles the
    // CRTC timing - and the framebuffer is sized to match, so cover all of it.
    uint32_t crtcHeight = m_mode->vdisplay;
    if ((m_mode->flags & DRM_MODE_FLAG_3D_MASK) == DRM_MODE_FLAG_3D_FRAME_PACKING)
      crtcHeight += m_mode->vtotal;

    AddProperty(outputPlane, "CRTC_X", 0);
    AddProperty(outputPlane, "CRTC_Y", 0);
    AddProperty(outputPlane, "CRTC_W", m_mode->hdisplay);
    AddProperty(outputPlane, "CRTC_H", crtcHeight);

    if (m_inFenceFd != -1)
    {
      AddProperty(m_crtc, "OUT_FENCE_PTR", reinterpret_cast<uint64_t>(&m_outFenceFd));
      AddProperty(outputPlane, "IN_FENCE_FD", m_inFenceFd);
    }
  }
  //! @todo Reaching out to the window manager and application player
  //! singletons from inside the DRM layer is a layering violation. The
  //! "should the GUI plane be attached this frame" decision is GUI/player
  //! policy and should be computed at the WinSystem caller and passed in
  //! as a parameter on FlipPage. Until that refactor lands, do the lookups
  //! in place to match the surrounding master code pattern.
  else if (m_gui_plane && m_video_plane &&
           !CServiceBroker::GetGUI()->GetWindowManager().HasVisibleControls() &&
           !CServiceBroker::GetAppComponents()
                .GetComponent<CApplicationPlayer>()
                ->HasVisibleOverlay() &&
           !HasQuirk(QUIRK_NEEDSPRIMARY))
  {
    AddProperty(m_gui_plane, "FB_ID", 0);
    AddProperty(m_gui_plane, "CRTC_ID", 0);
  }

  if (CServiceBroker::GetLogging().CanLogComponent(LOGWINDOWING))
    m_req->LogAtomicRequest();

  if (strict && m_propertyFailure)
  {
    return reject("properties", EINVAL);
  }

  const bool auditCommit = strict && DVBRIDGE::DiagnosticsEnabled() &&
      (!m_dvCommitReported || m_dvLastCommitRestore != m_dvRestoring);
  auto ret = drmModeAtomicCommit(m_fd, m_req->Get(), flags | DRM_MODE_ATOMIC_TEST_ONLY, nullptr);
  if (ret < 0)
  {
    const int testError = errno;
    if (strict || (m_mode->flags & DRM_MODE_FLAG_3D_MASK))
    {
      return reject("atomic-test", testError);
    }
    CLog::LogF(LOGERROR,
               "test commit failed: ({}) - falling back to last successful atomic "
               "request",
               strerror(testError));
    if (DVBRIDGE::DiagnosticsEnabled() && m_dvFailures.Record(DVNowMs(), "native-atomic-test", testError))
      CollectDVKernelLog();

    auto oldRequest = m_atomicRequestQueue.front().get();
    CDRMAtomicRequest::LogAtomicDiff(m_req, oldRequest);
    m_req = oldRequest;

    // update the old atomic request with the new fb id to avoid tearing
    if (rendered)
      AddProperty(outputPlane, "FB_ID", fb_id);
  }

  if (auditCommit)
    ReportDVCommit("atomic-test");
  if (strict && m_cb1TimingInput >= 0) m_cb1SubmittedNs = DVNowNs();
  ret = drmModeAtomicCommit(m_fd, m_req->Get(), flags, nullptr);
  if (ret < 0)
  {
    const int error = errno;
    if (!strict)
      CLog::LogF(LOGERROR, "atomic commit failed: {}", strerror(errno));
    if (!strict && DVBRIDGE::DiagnosticsEnabled() &&
        m_dvFailures.Record(DVNowMs(), "native-atomic-commit", error))
      CollectDVKernelLog();
    return reject("atomic-commit", error);
  }
  if (strict)
  {
    if (auditCommit)
    {
      m_dvCommitReported = true;
      m_dvLastCommitRestore = m_dvRestoring;
      ReportDVCommit("atomic-commit");
    }
    if (DVBRIDGE::DiagnosticsEnabled() && m_dvFailures.Recovered(DVNowMs()))
      DVBRIDGE::DiagnosticLog(LOGINFO, "DVBridge output recovered: failures={} recoveries={}",
          m_dvFailures.Count(), m_dvFailures.Recoveries());
  }
  FinishRequest(true);
  return true;
}

void CDRMAtomic::FinishRequest(bool accepted)
{
  if (accepted)
  {
    // Sync the property cache with values the kernel accepted.
    // This must happen after a successful commit so that
    // GetPropertyValue() returns current state (e.g. CRTC_ID=0
    // after Disable()). Without this, stale cached values cause
    // incorrect plane cleanup on subsequent video playback.
    m_req->CacheProperties();
    if (m_req->signalMode)
      m_committedMode = *m_req->signalMode;
    if (m_signalDirty && DVBRIDGE::DiagnosticsEnabled())
      ReportSignalState();

    if (m_atomicRequestQueue.size() > 1)
      m_atomicRequestQueue.pop_front();
  }
  else if (!m_atomicRequestQueue.empty())
    m_atomicRequestQueue.pop_back();

  if (m_inFenceFd != -1)
  {
    close(m_inFenceFd);
    m_inFenceFd = -1;
  }

  m_atomicRequestQueue.emplace_back(std::make_unique<CDRMAtomicRequest>());
  m_req = m_atomicRequestQueue.back().get();
  m_propertyFailure = false;
  m_signalDirty = false;
}

bool CDRMAtomic::IsSelectedModeCommitted() const
{
  return m_mode && m_committedMode.clock == m_mode->clock &&
      m_committedMode.hdisplay == m_mode->hdisplay &&
      m_committedMode.hsync_start == m_mode->hsync_start &&
      m_committedMode.hsync_end == m_mode->hsync_end &&
      m_committedMode.htotal == m_mode->htotal && m_committedMode.hskew == m_mode->hskew &&
      m_committedMode.vdisplay == m_mode->vdisplay &&
      m_committedMode.vsync_start == m_mode->vsync_start &&
      m_committedMode.vsync_end == m_mode->vsync_end &&
      m_committedMode.vtotal == m_mode->vtotal && m_committedMode.vscan == m_mode->vscan &&
      m_committedMode.vrefresh == m_mode->vrefresh && m_committedMode.flags == m_mode->flags;
}

CDRMAtomic::~CDRMAtomic()
{
  ResetCB1Timing();
}

void CDRMAtomic::ResetCB1Timing()
{
  if (m_cb1TimingInput >= 0) close(m_cb1TimingInput);
  m_cb1TimingInput = -1;
  m_cb1SubmittedNs = 0;
  m_cb1Headroom.reset();
}

void CDRMAtomic::SetCB1TimingPeriod(uint64_t periodNs)
{
  if (!periodNs || periodNs != m_cb1TimingPeriodNs) ResetCB1Timing();
  m_cb1TimingPeriodNs = periodNs;
}

std::optional<int64_t> CDRMAtomic::TakeCB1Headroom()
{
  const auto value = m_cb1Headroom;
  m_cb1Headroom.reset();
  return value;
}

bool CDRMAtomic::CompleteCB1Page(int timeoutMs)
{
  if (!m_cb1Pending) return true;
  if (m_cb1Uncertain) return false;
  pollfd fence{m_cb1OutFence, POLLIN, 0};
  const int ready = poll(&fence, 1, timeoutMs);
  if (!ready || (ready < 0 && errno == EINTR)) return false;
  sync_file_info info{};
  if (ready < 0 || !(fence.revents & POLLIN) ||
      ioctl(m_cb1OutFence, SYNC_IOC_FILE_INFO, &info) < 0 || info.status <= 0)
  {
    m_cb1Uncertain = true;
    ResetCB1Timing();
    ResetCB1PresentationTime();
    ReportDVFailure("display-fence", EIO);
    return false;
  }
  const uint64_t previous = m_cb1CompletedNs;
  ResetCB1PresentationTime();
  if (info.num_fences == 1)
  {
    sync_fence_info detail{};
    info.sync_fence_info = reinterpret_cast<uintptr_t>(&detail);
    if (ioctl(m_cb1OutFence, SYNC_IOC_FILE_INFO, &info) == 0 &&
        info.num_fences == 1 && detail.status > 0 && detail.timestamp_ns > previous)
    {
      m_cb1CompletedNs = detail.timestamp_ns;
      m_cb1PreviousCompletedNs = previous;
    }
  }
  m_cb1Headroom.reset();
  if (m_cb1TimingInput >= 0 && m_cb1TimingPeriodNs && m_cb1CompletedNs)
  {
    sync_file_info input{};
    std::array<sync_fence_info, 16> details{};
    uint64_t completed = 0;
    if (ioctl(m_cb1TimingInput, SYNC_IOC_FILE_INFO, &input) == 0 && input.status > 0 &&
        input.num_fences && input.num_fences <= details.size())
    {
      input.sync_fence_info = reinterpret_cast<uintptr_t>(details.data());
      if (ioctl(m_cb1TimingInput, SYNC_IOC_FILE_INFO, &input) == 0 &&
          input.num_fences && input.num_fences <= details.size())
        for (unsigned i = 0; i < input.num_fences; ++i)
        {
          if (details[i].status <= 0 || !details[i].timestamp_ns ||
              !strcmp(details[i].driver_name, "dma_fence")) { completed = 0; break; }
          completed = std::max(completed, uint64_t(details[i].timestamp_ns));
        }
    }
    m_cb1Headroom = DVBRIDGE::PresentationHeadroom(previous, completed, m_cb1SubmittedNs,
        m_cb1CompletedNs, m_cb1TimingPeriodNs);
  }
  if (m_cb1TimingInput >= 0) close(m_cb1TimingInput);
  m_cb1TimingInput = -1;
  m_cb1SubmittedNs = 0;
  close(m_cb1OutFence);
  m_cb1OutFence = -1;
  m_cb1Pending = false;
  return true;
}

bool CDRMAtomic::FlipPageTransactional(struct gbm_bo* bo, bool restore, bool async)
{
  if (m_cb1Pending) return false;
  // The window owns the bounded retry decision after coherent restoration.
  m_dvRestoring = restore;
  if (!bo)
  {
    ResetCB1PresentationTime();
    if (m_dvActivating && !restore) m_dvRetry.Failed(EINVAL);
    ReportDVFailure("missing-buffer", EINVAL);
    FinishRequest(false);
    return false;
  }
  const uint32_t fb = DrmOpaqueFbGetFromBo(bo);
  if (!fb)
  {
    ResetCB1PresentationTime();
    if (m_dvActivating && !restore) m_dvRetry.Failed(EINVAL);
    ReportDVFailure("framebuffer-import", 0);
    FinishRequest(false);
    return false;
  }
  // Stable scanout keeps the accepted mode. Transitions still validate it fully.
  const bool asynchronous = async && !restore && CanAsyncDVBridge() && m_inFenceFd >= 0;
  ResetCB1Timing();
  if (asynchronous && m_cb1TimingPeriodNs)
    m_cb1TimingInput = fcntl(m_inFenceFd, F_DUPFD_CLOEXEC, 3);
  const int flags = (m_need_modeset || m_dvActivating || restore)
                        ? DRM_MODE_ATOMIC_ALLOW_MODESET :
                            asynchronous ? DRM_MODE_ATOMIC_NONBLOCK : 0;
  const bool accepted = DrmAtomicCommit(fb, flags, true, false, true);
  if (!accepted || !asynchronous)
  {
    ResetCB1PresentationTime();
    ResetCB1Timing();
  }
  if (accepted)
  {
    m_need_modeset = false;
    if (asynchronous)
    {
      m_cb1Pending = true;
      m_cb1OutFence = TakeOutFenceFd();
      m_cb1Uncertain = m_cb1OutFence < 0;
      if (m_cb1Uncertain) ResetCB1PresentationTime();
      if (m_cb1Uncertain) ReportDVFailure("missing-display-fence", EIO);
    }
  }
  else if (asynchronous)
  {
    const int fd = TakeOutFenceFd();
    if (fd >= 0) close(fd);
  }
  return accepted;
}

bool CDRMAtomic::TestFramebuffer(struct gbm_bo* bo)
{
  CDRMPlane* plane = m_gui_plane ? m_gui_plane : m_video_plane;
  struct drm_fb* fb = bo ? DrmFbGetFromBo(bo) : nullptr;
  if (!fb || !plane || !m_crtc || !m_mode) return false;

  // Probe a new surface independently without consuming the queued signal.
  CDRMAtomicRequest request;
  if (!request.AddProperty(plane, "FB_ID", fb->fb_id) ||
      !request.AddProperty(plane, "CRTC_ID", m_crtc->GetCrtcId()) ||
      !request.AddProperty(plane, "SRC_X", 0) || !request.AddProperty(plane, "SRC_Y", 0) ||
      !request.AddProperty(plane, "SRC_W", static_cast<uint64_t>(m_width) << 16) ||
      !request.AddProperty(plane, "SRC_H", static_cast<uint64_t>(m_height) << 16) ||
      !request.AddProperty(plane, "CRTC_X", 0) || !request.AddProperty(plane, "CRTC_Y", 0) ||
      !request.AddProperty(plane, "CRTC_W", m_mode->hdisplay) ||
      !request.AddProperty(plane, "CRTC_H", m_mode->vdisplay)) return false;

  return drmModeAtomicCommit(m_fd, request.Get(),
      DRM_MODE_ATOMIC_TEST_ONLY | DRM_MODE_ATOMIC_ALLOW_MODESET, nullptr) == 0;
}

void CDRMAtomic::FlipPage(struct gbm_bo* bo, bool rendered, bool videoLayer, bool async)
{
  ResetCB1PresentationTime();
  m_lastPageAccepted=false;
  struct drm_fb* drm_fb = nullptr;
  uint32_t flags = 0;

  if (rendered)
  {
    drm_fb = CDRMUtils::DrmFbGetFromBo(bo);
    if (!drm_fb)
    {
      CLog::LogF(LOGERROR, "Failed to get a new FBO");
      return;
    }

    if (async && !m_need_modeset)
      flags |= DRM_MODE_ATOMIC_NONBLOCK;
  }

  if (m_need_modeset)
  {
    flags |= DRM_MODE_ATOMIC_ALLOW_MODESET;
    m_need_modeset = false;
    CLog::LogF(LOGDEBUG, "Execute modeset at next commit");
  }

  m_lastPageAccepted=DrmAtomicCommit(!drm_fb ? 0 : drm_fb->fb_id, flags, rendered, videoLayer);
}

bool CDRMAtomic::InitDrm()
{
  bool opened = CDRMUtils::OpenDrm(true);

  // At cold boot a connector's EDID/HPD probe may not have completed, so
  // OpenDrm(true) transiently fails with no connector even though an
  // atomic-capable display is attached. Retry a few times with a short delay,
  // but only when the hardware is atomic capable so a not-yet-ready display is
  // given time to appear. Genuinely non-atomic hardware skips the retry and
  // returns immediately, so a legacy fall-through is never delayed.
  if (!opened && SupportsAtomicModesetting())
  {
    constexpr int maxPasses = 3;
    constexpr auto retryDelay = std::chrono::milliseconds(500);

    for (int pass = 1; !opened && pass < maxPasses; ++pass)
    {
      CLog::LogF(LOGWARNING,
                 "atomic-capable device present but no connector ready, re-scanning ({}/{})", pass,
                 maxPasses);
      std::this_thread::sleep_for(retryDelay);
      opened = CDRMUtils::OpenDrm(true);
    }
  }

  if (!opened)
    return false;

  auto ret = drmSetClientCap(m_fd, DRM_CLIENT_CAP_ATOMIC, 1);
  if (ret)
  {
    CLog::LogF(LOGERROR, "no atomic modesetting support: {}", strerror(errno));
    return false;
  }

  m_atomicRequestQueue.emplace_back(std::make_unique<CDRMAtomicRequest>());
  m_req = m_atomicRequestQueue.back().get();

  if (!CDRMUtils::InitDrm())
    return false;

  CLog::LogF(LOGDEBUG, "initialized atomic DRM");

  return true;
}

void CDRMAtomic::DestroyDrm()
{
  ResetCB1PresentationTime();
  CDRMUtils::DestroyDrm();
}

bool CDRMAtomic::SupportsAtomicModesetting()
{
  int numDevices = drmGetDevices2(0, nullptr, 0);
  if (numDevices <= 0)
    return false;

  std::vector<drmDevicePtr> devices(numDevices);
  if (drmGetDevices2(0, devices.data(), devices.size()) < 0)
    return false;

  bool atomic = false;
  for (const auto device : devices)
  {
    if (!(device->available_nodes & 1 << DRM_NODE_PRIMARY))
      continue;

    int fd = open(device->nodes[DRM_NODE_PRIMARY], O_RDWR | O_CLOEXEC);
    if (fd < 0)
      continue;

    // The atomic client cap is a device property gated on the driver's atomic
    // support, not on any connector, so testing it needs only the fd.
    if (drmSetClientCap(fd, DRM_CLIENT_CAP_ATOMIC, 1) == 0)
      atomic = true;

    close(fd);

    if (atomic)
      break;
  }

  drmFreeDevices(devices.data(), devices.size());
  return atomic;
}

bool CDRMAtomic::SetVideoMode(const RESOLUTION_INFO& res, struct gbm_bo* bo)
{
  ResetCB1PresentationTime();
  m_need_modeset = true;

  return true;
}

bool CDRMAtomic::SetActive(bool active)
{
  ResetCB1PresentationTime();
  m_need_modeset = true;
  m_active = active;

  return true;
}

bool CDRMAtomic::AddProperty(CDRMObject* object, const char* name, uint64_t value)
{
  const bool added = object && m_req && m_req->AddProperty(object, name, value);
  if (added && DVBRIDGE::DiagnosticsEnabled() &&
      (strcmp(name, "MODE_ID") == 0 || strcmp(name, "ACTIVE") == 0 ||
       strcmp(name, "DVBRIDGE_DV_STANDARD_LAB") == 0 || strcmp(name, "HDR_OUTPUT_METADATA") == 0 ||
       strcmp(name, "Colorspace") == 0 || strcmp(name, "max bpc") == 0 ||
       strcmp(name, "Broadcast RGB") == 0 || strcmp(name, "NV_HDMI_VSIF_METADATA") == 0 ||
       strcmp(name, "dithering mode") == 0))
    m_signalDirty = true;
  m_propertyFailure |= !added;
  return added;
}

void CDRMAtomic::ReportSignalState() noexcept try
{
  // Cached values are updated only after an accepted real commit. No DRM query
  // or per-frame log. Presence belongs in the key: absent != present at zero.
  const auto get = [](const auto* object, const char* name) {
    const bool known = object && object->HasPropertyInventory();
    return DVBRIDGE::DiagnosticProperty(known, known ? object->GetPropertyValue(name) : std::nullopt);
  };
  const auto dv = get(m_connector, "DVBRIDGE_DV_STANDARD_LAB");
  const auto hdr = get(m_connector, "HDR_OUTPUT_METADATA");
  const auto color = get(m_connector, "Colorspace");
  const auto bpc = get(m_connector, "max bpc");
  const auto rgb = get(m_connector, "Broadcast RGB");
  const auto active = get(m_crtc, "ACTIVE");
  const auto nv = get(m_connector, "NV_HDMI_VSIF_METADATA");
  const auto dither = get(m_connector, "dithering mode");
  const auto& mode = m_committedMode;
  const std::array<uint64_t, 25> state{m_connector ? m_connector->GetId() : 0,
      m_crtc ? m_crtc->GetId() : 0, dv.value.value_or(0), hdr.value.value_or(0),
      color.value.value_or(0), bpc.value.value_or(0), rgb.value.value_or(0),
      mode.hdisplay, mode.vdisplay, mode.clock, mode.htotal, mode.vtotal, mode.flags,
      mode.vscan, active.value.value_or(0), dv.Code(),hdr.Code(),color.Code(),bpc.Code(),rgb.Code(),active.Code(),
      nv.value.value_or(0),nv.Code(),dither.value.value_or(0),dither.Code()};
  if (m_signalKnown && state == m_signalState)
    return;
  const auto error = m_connector ? m_connector->PropertyInventoryError() : std::nullopt;
  if (DVBRIDGE::DiagnosticLog(LOGINFO,
      "DVBridge signal: origin=accepted-commit-cache connector={} crtc={} dv_property={} dv_value={} hdr_property={} hdr_value={} colorspace_property={} colorspace_value={} max_bpc_property={} max_bpc_ceiling={} rgb_property={} rgb_value={} active_property={} active_value={} mode={}x{} clock_khz={} inventory_error={} nv_vsif_property={} nv_vsif_value={} dither_property={} dither_value={}",
      state[0], state[1], dv.Presence(),dv.Value(),hdr.Presence(),hdr.Value(),
      color.Presence(),color.Value(),bpc.Presence(),bpc.Value(),rgb.Presence(),rgb.Value(),
      active.Presence(),active.Value(),mode.hdisplay,mode.vdisplay,mode.clock,
      error ? std::to_string(*error) : "unavailable",nv.Presence(),nv.Value(),dither.Presence(),dither.Value()))
  {
    m_signalKnown = true;
    m_signalState = state;
  }
}

catch (...) {}

CDRMAtomic::CDRMAtomicRequest::CDRMAtomicRequest() : m_atomicRequest(drmModeAtomicAlloc())
{
}

bool CDRMAtomic::CDRMAtomicRequest::AddProperty(CDRMObject* object,
                                                const char* name,
                                                uint64_t value)
{
  uint32_t propertyId = object->GetPropertyId(name);
  if (propertyId == 0)
    return false;

  int ret = drmModeAtomicAddProperty(m_atomicRequest.get(), object->GetId(), propertyId, value);
  if (ret < 0)
    return false;

  m_atomicRequestItems[object][propertyId] = value;
  if (std::strcmp(name,"DVBRIDGE_DV_STANDARD_LAB") == 0) requestedDV = value;
  return true;
}

void CDRMAtomic::CDRMAtomicRequest::CacheProperties()
{
  for (const auto& [object, properties] : m_atomicRequestItems)
  {
    for (const auto& [propertyId, value] : properties)
    {
      object->CachePropertyValue(propertyId, value);
    }
  }
}

void CDRMAtomic::CDRMAtomicRequest::LogAtomicDiff(CDRMAtomicRequest* current,
                                                  CDRMAtomicRequest* old)
{
  std::map<CDRMObject*, std::map<uint32_t, uint64_t>> atomicDiff;

  for (const auto& object : current->m_atomicRequestItems)
  {
    auto sameObject = old->m_atomicRequestItems.find(object.first);
    if (sameObject != old->m_atomicRequestItems.end())
    {
      std::map<uint32_t, uint64_t> propertyDiff;

      std::ranges::set_difference(current->m_atomicRequestItems[object.first],
                                  old->m_atomicRequestItems[object.first],
                                  std::inserter(propertyDiff, propertyDiff.begin()));

      atomicDiff[object.first] = propertyDiff;
    }
    else
    {
      atomicDiff[object.first] = current->m_atomicRequestItems[object.first];
    }
  }

  CLog::LogF(LOGDEBUG, "DRM Atomic Request Diff:");

  LogAtomicRequest(LOGERROR, atomicDiff);
}

void CDRMAtomic::CDRMAtomicRequest::LogAtomicRequest()
{
  CLog::LogF(LOGDEBUG, "DRM Atomic Request:");
  LogAtomicRequest(LOGDEBUG, m_atomicRequestItems);
}

void CDRMAtomic::CDRMAtomicRequest::LogDVProperties()
{
  std::string message;
  unsigned count = 0;
  for (const auto& object : m_atomicRequestItems)
    for (const auto& property : object.second)
    {
      const auto name = object.first->GetPropertyName(property.first);
      if (name != "DVBRIDGE_DV_STANDARD_LAB" && name != "HDR_OUTPUT_METADATA" &&
          name != "max bpc" && name != "Broadcast RGB" && name != "Colorspace" &&
          name != "CRTC_ID" && name != "ACTIVE" && name != "VRR_ENABLED" &&
          name != "CTM" && name != "GAMMA_LUT" && name != "DEGAMMA_LUT" &&
          name != "rotation" && name != "alpha" && name != "COLOR_PIPELINE" &&
          name != "NV_HDMI_VSIF_METADATA" && name != "dithering mode" &&
          !name.starts_with("NV_PLANE_") && !name.starts_with("NV_CRTC_") &&
          name != "NV_INPUT_COLORSPACE" && name != "NV_HDR_STATIC_METADATA")
        continue;
      if (count++ < 48)
        message += " " + std::to_string(object.first->GetId()) + ":" + name + "=" +
                   std::to_string(property.second);
    }
  DVBRIDGE::DiagnosticLog(LOGINFO, "DVBridge requested properties (48 max):{}", message);
}

void CDRMAtomic::CDRMAtomicRequest::LogAtomicRequest(
    uint8_t logLevel, std::map<CDRMObject*, std::map<uint32_t, uint64_t>>& atomicRequestItems)
{
  std::string message;
  for (const auto& object : atomicRequestItems)
  {
    message.append("\nObject: " + object.first->GetTypeName() +
                   "\tID: " + std::to_string(object.first->GetId()));
    for (const auto& property : object.second)
      message.append("\n  Property: " + object.first->GetPropertyName(property.first) +
                     "\tID: " + std::to_string(property.first) +
                     "\tValue: " + std::to_string(property.second));
  }

  CLog::LogF(logLevel, "{}", message);
}

void CDRMAtomic::CDRMAtomicRequest::DrmModeAtomicReqDeleter::operator()(drmModeAtomicReqPtr p) const
{
  drmModeAtomicFree(p);
};
