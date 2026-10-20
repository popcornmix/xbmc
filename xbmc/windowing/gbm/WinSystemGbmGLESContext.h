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
#include <optional>
#include <mutex>
#ifdef HAVE_DVBRIDGE
#include "cores/VideoPlayer/VideoRenderers/DVBridgeGLES.h"
#include "cores/VideoPlayer/DVDCodecs/Video/DVOutputSelection.h"
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
  ~CWinSystemGbmGLESContext() override;

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
  bool CreateNewWindow(const std::string& name, bool fullScreen, RESOLUTION_INFO& res) override;
  void RefreshDVOutputCapabilities();
  DVBRIDGE::OutputCapabilities GetDVOutputCapabilities(const VideoPicture* picture) const;
  bool CanRetainDVSource() const;
  void ObserveDVImport(DVBRIDGE::SourceBinding binding, dvbridge_geometry geometry, bool usable);
  DVBRIDGE::OutputSelection SelectedDVOutput(const VideoPicture* picture) const;
  DVBRIDGE::OutputReason ConsumeDVPlaybackFailure(DVBRIDGE::SourceBinding binding);
  bool HasPendingDVBridgeRestore() const
  {
    return m_failedHDR10SetupRestore || m_hdrDeepColorPrevious || m_hdrConversion ||
        m_dvState.Captured();
  }
  bool BeginDVBridge();
  bool PrepareDVOutputFrame(const VideoPicture& picture);
#ifdef CB1_HAS_HDR10_AI
  bool HDR10AILookaheadActive() const;
  bool HDR10AIFrameCommitted() const
  { return m_dvBridge && m_dvBridge->CommittedHDR10AI().has_value(); }
  bool PrepareHDR10AIQueue(const VideoPicture& picture,
      std::span<const CDVBridgeGLES::Layer> queue, bool end, bool paused,
      uint64_t presentationDeadline = 0);
#endif
  bool RestoreFailedHDR10Setup();
  bool ConfigureHDR10Conversion(const VideoPicture& picture);
  bool SupportsHDR10Conversion();
  void SetDVBridgeOverlays(bool visible) { m_dvOverlays = visible; }
  bool RenderDVBridge(const CDVBridgeGLES::Layer& layer, std::shared_ptr<AVFrame> enhancement,
                       DVBRIDGE::SourceBinding enhancementBinding,
                       const CDVMetadataBuffer& metadata,
                       dvbridge_geometry geometry);
  bool EndDVBridge();
  bool SetVideoOutput(const VideoPicture* picture) override;
  bool DestroyWindowSystem() override;
  bool DestroyWindow() override;
#endif

protected:
  void SetVSyncImpl(bool enable) override {}
  void PresentRenderImpl(bool rendered) override {};
  bool CreateContext() override;
#ifdef HAVE_DVBRIDGE
  int VideoOutputBitDepth(const VideoPicture* picture) const override;
  bool RejectDVOutputSurface() override;
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
  DVBRIDGE::OutputCapabilities GetDVOutputCapabilities(const VideoPicture* picture,
      const DVBRIDGE::OutputPreferences& preferences) const;
  DVBRIDGE::OutputSelection SelectedDVOutput(const VideoPicture* picture,
      const DVBRIDGE::OutputPreferences& preferences, DVBRIDGE::OutputCapabilities& capabilities) const;
  std::unique_ptr<CDVBridgeGLES> m_dvBridge;
  std::shared_ptr<CDVBridgeGLES::Context> m_dvContext;
  bool RetireDVContext();
  bool m_dvShutdownFence{};
  bool m_dvRejectedSurface{}, m_dvRollbackFailed{};
  bool PollRejectedDVSurface();
  bool RecoverDVActivation();
  void ResetDVActivation(const VideoPicture* picture);
  bool m_dvRecoveryPending{}, m_dvNativeFailed{}, m_dvHDR10Failed{}, m_dvFallbackActive{};
  bool m_dvActivating{};
  uint64_t m_dvActivationStream{}, m_dvActivationPolicy{};
  unsigned m_dvAttemptFailures{};
  DVBRIDGE::OutputRoute m_dvRejectedRoute{DVBRIDGE::OutputRoute::Vanilla};
  DVBRIDGE::OutputReason m_dvActivationReason{DVBRIDGE::OutputReason::None};
  std::optional<dvbridge_mode> m_dvConfiguredOutput;
  std::optional<dvbridge_mode> m_dvPriorConfiguredOutput;
  DVBRIDGE::OutputSelection m_dvSelection;
  // Also owns the coherent activation stream/policy, failure flags and reason
  // shared by video-thread selection. GPU/DRM operations remain render-owned.
  mutable std::mutex m_dvCapabilityMutex;
#ifdef CB1_HAS_HDR10_AI
  DVBRIDGE::SourceBinding m_aiActivation{};
  uint64_t m_aiPolicy{};
  bool m_aiReady{}, m_aiFallback{}, m_aiActive{};
#endif
  DVBRIDGE::OutputCapabilities m_dvCapabilities;
  DVBRIDGE::SourceBinding m_dvImportBinding{};
  dvbridge_geometry m_dvImportGeometry{};
  bool m_dvImportUsable{};
  uint64_t m_dvImportGeneration{};
  DVBRIDGE::SourceBinding m_dvFatalBinding{};
  DVBRIDGE::OutputReason m_dvFatalReason{DVBRIDGE::OutputReason::None};
  bool m_dvFatalConsumed{};
  bool m_dvPriorHDRConversion{}, m_dvPriorGuiCompositing{};
  uint64_t m_dvPendingStream{}, m_dvPendingPolicy{}, m_dvFailedStream{}, m_dvFailedPolicy{};
  bool m_dvOutputRetiring{}, m_dvOrdinaryFrame{};
  CDVBridgeState m_dvState;
  DVBRIDGE::DiagnosticFailures m_dvSetupFailures;
  bool m_dvFrame = false;
  bool m_hdrConversion = false;
  std::optional<dvbridge_hdr10_session> m_hdr10Session;
  uint64_t m_hdr10SessionStream{};
  std::optional<uint64_t> m_hdrDeepColorPrevious;
  bool m_failedHDR10SetupRestore = false;
  bool m_dvOverlays = true;
#endif
};

} // namespace GBM
} // namespace WINDOWING
} // namespace KODI
