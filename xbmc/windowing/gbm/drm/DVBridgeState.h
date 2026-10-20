/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

#include "DRMObject.h"
#include <string>
#include <vector>

namespace KODI::WINDOWING::GBM
{
class CDRMAtomic;
class CDRMConnector;

class CDVBridgeState
{
public:
  enum class Backend { NONE, KERNEL, NVIDIA };
  static Backend GetBackend(CDRMConnector& connector);
  bool Capture(CDRMAtomic& drm, uint64_t ownVsif = 0);
  bool Stage(CDRMAtomic& drm, bool restore);
  void Restored() { m_saved.clear(); m_owner = nullptr; m_backend = Backend::NONE; }
  bool Captured() const { return !m_saved.empty(); }
  Backend CapturedBackend() const { return m_backend; }
  const char* CaptureFailure() const { return m_captureFailure; }

private:
  struct Property
  {
    CDRMObject* object;
    std::string name;
    uint64_t original;
    uint64_t transport;
    CDRMPropertyBlob blob;
    CDRMPropertyBlob transportBlob;
  };
  std::vector<Property> m_saved;
  CDRMAtomic* m_owner = nullptr;
  Backend m_backend = Backend::NONE;
  const char* m_captureFailure = "none";
};
}
