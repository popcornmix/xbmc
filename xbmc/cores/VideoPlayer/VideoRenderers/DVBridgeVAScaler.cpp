/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "DVBridgeVAScaler.h"
struct CDVBridgeVAScaler::State {};
CDVBridgeVAScaler::CDVBridgeVAScaler() : m_state(std::make_unique<State>()) {}
CDVBridgeVAScaler::~CDVBridgeVAScaler() = default;
bool CDVBridgeVAScaler::Close() { return true; }
CDVBridgeGLES::FrameStatus CDVBridgeVAScaler::Scale(const std::shared_ptr<AVFrame>&,
    DVBRIDGE::SourceBinding, int, int, CDVBridgeGLES::Layer&)
{
  return CDVBridgeGLES::FrameStatus::Unsupported;
}
