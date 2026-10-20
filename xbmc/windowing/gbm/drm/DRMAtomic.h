/*
 *  Copyright (C) 2005-2018 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#pragma once

#include "DRMUtils.h"
#include "utils/log.h"
#include "utils/DVBridgeOutputRetry.h"

#include <cstdint>
#include <deque>
#include <map>
#include <memory>

namespace KODI
{
namespace WINDOWING
{
namespace GBM
{

class CDRMAtomic : public CDRMUtils
{
public:
  CDRMAtomic() = default;
  ~CDRMAtomic() override = default;
  void FlipPage(struct gbm_bo* bo, bool rendered, bool videoLayer, bool async) override;
  bool SetVideoMode(const RESOLUTION_INFO& res, struct gbm_bo* bo) override;
  bool SetActive(bool active) override;
  bool InitDrm() override;
  void DestroyDrm() override;
  bool SupportsFencing() override { return true; }
  bool AddProperty(CDRMObject* object, const char* name, uint64_t value);
  // Blocking commit: rejected properties must not be paired with a new framebuffer.
  bool FlipPageTransactional(struct gbm_bo* bo, bool restore = false);
  void ResetDVActivation() { m_dvRetry.Reset(); }
  void SetDVActivationPending(bool pending) { m_dvActivating = pending; }
  void FailDVActivation(int error) { m_dvRetry.Failed(error); }
  bool CanRetryDVActivation(bool restored) const
  { return m_dvRetry.CanRetry(m_dvRetry.Error(), restored); }
  int DVActivationError() const { return m_dvRetry.Error(); }
  unsigned DVActivationFailures() const { return m_dvRetry.Count(); }

  // Probe whether any DRM device supports atomic modesetting, with no
  // connector required. Used to decide atomic-vs-legacy before falling back.
  static bool SupportsAtomicModesetting();

private:
  bool DrmAtomicCommit(int fb_id, int flags, bool rendered, bool videoLayer,
                       bool strict = false);
  void FinishRequest(bool accepted);
  DVBRIDGE::OutputRetry m_dvRetry;
  bool m_dvLinkKnown{false};
  uint32_t m_dvLinkConnector{0}, m_dvLinkBlob{0}, m_dvLinkStatus{0};
  void RefreshDVLink();

  bool m_need_modeset{true};
  bool m_active = true;
  bool m_propertyFailure = false;
  bool m_dvRestoring = false;
  bool m_dvActivating = true;

  class CDRMAtomicRequest
  {
  public:
    CDRMAtomicRequest();
    ~CDRMAtomicRequest() = default;
    CDRMAtomicRequest(const CDRMAtomicRequest& right) = delete;

    drmModeAtomicReqPtr Get() const { return m_atomicRequest.get(); }

    bool AddProperty(CDRMObject* object, const char* name, uint64_t value);
    void CacheProperties();
    void LogAtomicRequest();

    static void LogAtomicDiff(CDRMAtomicRequest* current, CDRMAtomicRequest* old);

  private:
    static void LogAtomicRequest(
        uint8_t logLevel, std::map<CDRMObject*, std::map<uint32_t, uint64_t>>& atomicRequestItems);

    std::map<CDRMObject*, std::map<uint32_t, uint64_t>> m_atomicRequestItems;

    struct DrmModeAtomicReqDeleter
    {
      void operator()(drmModeAtomicReqPtr p) const;
    };

    std::unique_ptr<drmModeAtomicReq, DrmModeAtomicReqDeleter> m_atomicRequest;
  };

  CDRMAtomicRequest* m_req = nullptr;
  std::deque<std::unique_ptr<CDRMAtomicRequest>> m_atomicRequestQueue;
};

} // namespace GBM
} // namespace WINDOWING
} // namespace KODI
