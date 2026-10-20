/*
 *  Copyright (C) 2005-2018 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#pragma once

#include "WinSystemGbmEGLContext.h"
#include "cores/VideoPlayer/VideoRenderers/FrameBufferObject.h"
#include "rendering/gles/RenderSystemGLES.h"
#include "utils/EGLUtils.h"

#include <memory>
#ifdef HAVE_DVBRIDGE
#include "cores/VideoPlayer/VideoRenderers/DVBridgeGLES.h"
#include "drm/DVBridgeState.h"
#endif

class CGuiCompositeShaderGLES;

class CVaapiProxy;

namespace KODI
{
namespace WINDOWING
{
namespace GBM
{

class CWinSystemGbmGLESContext : public CWinSystemGbmEGLContext, public CRenderSystemGLES
{
public:
  CWinSystemGbmGLESContext();
  ~CWinSystemGbmGLESContext() override = default;

  static void Register();
  using CWinSystemGbm::Register;
  static std::unique_ptr<CWinSystemBase> CreateWinSystem();

  // Implementation of CWinSystemBase via CWinSystemGbm
  CRenderSystemBase* GetRenderSystem() override { return this; }
  bool InitWindowSystem() override;
  bool SetFullScreen(bool fullScreen, RESOLUTION_INFO& res, bool blankOtherDisplays) override;
  void PresentRender(bool rendered, bool videoLayer) override;

  // GUI compositing for HDR
  bool SetGuiCompositing(int colorTransfer) override;
  bool BeginGuiComposite(bool guiWillRender) override;
  void EndGuiComposite() override;
  void CompositeGui() override;
  bool IsHdrComposite() const override { return m_guiCompositing; }
#ifdef HAVE_DVBRIDGE
  static bool DVBridgeRequested(const VideoPicture* picture);
  bool BeginDVBridge();
  void SetDVBridgeOverlays(bool visible) { m_dvOverlays = visible; }
  bool RenderDVBridge(const CDVBridgeGLES::Layer& layer, const AVFrame* enhancement,
                       const CDVMetadataBuffer& metadata,
                       dvbridge_geometry geometry);
  bool EndDVBridge();
  bool SetVideoOutput(const VideoPicture* picture) override;
  bool DestroyWindowSystem() override;
#endif

protected:
  void SetVSyncImpl(bool enable) override {}
  void PresentRenderImpl(bool rendered) override {};
  bool CreateContext() override;
#ifdef HAVE_DVBRIDGE
  int VideoOutputBitDepth(const VideoPicture* picture) const override;
#endif

private:
  void PresentFrame(bool rendered, bool videoLayer);
  bool m_guiCompositing{false};
  CFrameBufferObject m_guiFbo;
  int m_guiFboWidth{0};
  int m_guiFboHeight{0};
  // True when the GUI FBO is empty (no draws this frame); CompositeGui skips composite when true.
  bool m_guiFboClean{false};
  // Whether the GUI render pass will run this frame; set by BeginGuiComposite.
  bool m_guiWillRender{true};

  std::unique_ptr<CGuiCompositeShaderGLES> m_compositeShader;
#ifdef HAVE_DVBRIDGE
  std::unique_ptr<CDVBridgeGLES> m_dvBridge;
  CDVBridgeState m_dvState;
  bool m_dvFrame = false;
  bool m_dvOverlays = true;
#endif
};

} // namespace GBM
} // namespace WINDOWING
} // namespace KODI
