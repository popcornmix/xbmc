/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include "DVBridgeGLES.h"

class CDVBridgeVAScaler
{
public:
  CDVBridgeVAScaler();
  ~CDVBridgeVAScaler();
  CDVBridgeGLES::FrameStatus Scale(const std::shared_ptr<AVFrame>& source,
      DVBRIDGE::SourceBinding binding, int width, int height, CDVBridgeGLES::Layer& out);
  bool Close();
private:
  struct State;
  std::unique_ptr<State> m_state;
};
