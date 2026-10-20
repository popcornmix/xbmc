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

#include <array>
#include <memory>
#include <optional>
#include <mutex>
#ifdef HAVE_DVBRIDGE
#include "cores/VideoPlayer/VideoRenderers/DVBridgeGLES.h"
#include "cores/VideoPlayer/DVDCodecs/Video/DVOutputSelection.h"
#include "drm/DVBridgeState.h"
#include "ModeChangeHold.h"
#include "DVPresentStall.h"
#endif

class CGuiCompositeShaderGLES;

class CVaapiProxy;
class VideoSourceSideData;

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
  void StageVideoSource(std::shared_ptr<const VideoSourceSideData> source)
  {
    m_videoSourceSideData = std::move(source);
  }
#ifdef HAVE_DVBRIDGE
  bool BeginRender() override;
  bool Hide() override;
  bool Show(bool raise = true) override;
  float GetDisplayLatency() override;
#endif

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
  DVBRIDGE::OutputCapabilities GetDVOutputCapabilities(const VideoPicture* picture,
      bool allowModeSwitch = false) const;
  bool CanRetainDVSource() const;
  bool CompleteDVModeSwitch(const VideoPicture& picture);
  bool DVModeSwitchDeferred()
  {
    return !m_dvRollbackFailed && m_dvShutdownFence && m_dvContext &&
        m_dvContext->Poll() != DVBRIDGE_DV_FAILED;
  }
  void ObserveDVImport(DVBRIDGE::SourceBinding binding, dvbridge_geometry geometry, bool usable);
  DVBRIDGE::OutputSelection SelectedDVOutput(const VideoPicture* picture) const;
  DVBRIDGE::OutputReason ConsumeDVPlaybackFailure(DVBRIDGE::SourceBinding binding);
  bool HasPendingDVBridgeRestore() const
  {
    return m_dvStopPending || m_failedHDR10SetupRestore || m_hdrDeepColorPrevious || m_hdrConversion ||
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
  bool SupportsHDR10Conversion(bool requireCurrentMode = true);
  void SetDVBridgeOverlays(bool visible) { m_dvOverlays = visible; }
  bool CB1FramePending() const { return m_cb1FramePending; }
  bool CB1AsyncPlayback() const
  {
    return CB1AsyncAvailable() && m_dvBridge->AsyncPreparationActive();
  }
  bool CB1NativeAsyncPlayback() const
  {
    return m_DRM && m_DRM->HasQuirk(QUIRK_CB1_NATIVE_EDGE) && CB1AsyncPlayback() &&
        !m_hdrConversion && !m_dvBridge->Advanced() &&
        (m_dvSelection.policy.mode == DVBRIDGE_MODE_STANDARD ||
         m_dvSelection.policy.mode == DVBRIDGE_MODE_MASTER_REFERENCE);
  }
  std::optional<double> CB1PresentationAge() const;
  void RecordDVSubtitle(const CRect& rect, unsigned texture);
  bool BeginDVPQSubtitle();
  void EndDVPQSubtitle();
  bool DVPQSubtitlesReady() const { return m_dvPQSubtitleDraws != 0; }
  bool HasDVBridgeScreenshot() const { return m_dvState.Captured() || m_hdrConversion; }
  bool ReadDVBridgeScreenshot(KODI::RENDERING::CAPTURE::ReadbackBuffer& out)
  {
    return m_dvFrame && m_dvBridge && m_dvBridge->ReadScreenshot(out);
  }
  bool RenderDVBridge(const CDVBridgeGLES::Layer& layer, std::shared_ptr<AVFrame> enhancement,
                       DVBRIDGE::SourceBinding enhancementBinding,
                       const CDVMetadataBuffer& metadata,
                       dvbridge_geometry geometry,
                       const CDVBridgeGLES::FELSuccessor* successor = nullptr);
  bool EndDVBridge();
  void DiscardDVBridgeFrame();
  bool StopDVBridgePlayback();
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
#ifdef HAVE_DVBRIDGE
  CModeChangeHold m_modeHold;
  CHeldModeAnnouncement m_modeAnnouncement;
  CDVPresentStall m_presentStall;
  bool CB1ModeCanJoin(const RESOLUTION_INFO& res) const;
  bool HoldCB1Mode(RESOLUTION_INFO& res);
  bool HoldCB1Stop();
  bool PrepareNvidiaModeRestore(const RESOLUTION_INFO& res);
  void AnnounceCB1Mode(bool transport);
  void CompleteCB1Mode(bool accepted, bool transport);
#endif
  void PublishVideoSource(bool accepted, const std::shared_ptr<const VideoSourceSideData>& source);
  std::shared_ptr<const VideoSourceSideData> m_videoSourceSideData;
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
      const DVBRIDGE::OutputPreferences& preferences, bool allowModeSwitch = false) const;
  DVBRIDGE::OutputSelection SelectedDVOutput(const VideoPicture* picture,
      const DVBRIDGE::OutputPreferences& preferences, DVBRIDGE::OutputCapabilities& capabilities) const;
  std::unique_ptr<CDVBridgeGLES> m_dvBridge;
  std::shared_ptr<CDVBridgeGLES::Context> m_dvContext;
  bool RetireDVContext();
  bool CB1AsyncAvailable() const;
  bool CreateCB1GPUFence(const CDVBridgeGLES::Submission& submission);
  void ObserveCB1Headroom();
  DVBRIDGE::FastRendering m_fastRendering;
  bool m_fastRenderEligible{}, m_fastSubmittedEligible{};
  uint64_t m_fastObservedPresentation{}, m_fastSourceStream{};
  double m_fastSourcePts{};
  uint64_t m_fastPreparedStream{};
  double m_fastPreparedPts{};
  bool m_dvShutdownFence{};
  bool m_dvRejectedSurface{}, m_dvRollbackFailed{};
  bool PollRejectedDVSurface();
  bool StageOrdinaryDVOutput(const VideoPicture* picture);
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
  bool m_dvAvailableNativeDV{}, m_dvAvailableHDR10{};
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
  bool m_cb1FramePending{};
  bool m_dvEndPending{};
  bool m_dvStopPending{};
  bool m_hdrConversion = false;
  std::optional<dvbridge_hdr10_session> m_hdr10Session;
  std::unique_ptr<VideoPicture> m_hdr10BasicSignal;
  std::optional<std::array<unsigned, 5>> m_hdr10Signal;
  uint64_t m_hdr10SessionStream{};
  std::optional<uint64_t> m_hdrDeepColorPrevious;
  bool m_failedHDR10SetupRestore = false;
  bool m_dvOverlays = true;
  CRect m_dvSubtitleBounds;
  unsigned m_dvSubtitleDraws{};
  uint64_t m_dvSubtitleContent{};
  uint64_t m_dvGUIContentGeneration{};
  CFrameBufferObject m_dvPQSubtitleFbo;
  int m_dvPQSubtitleWidth{}, m_dvPQSubtitleHeight{}, m_dvPQPreviousFbo{};
  unsigned m_dvPQSubtitleDraws{};
  bool m_dvPQSubtitleCleared{}, m_dvPQSubtitleRendering{};
  bool m_dvNativeGui{};
#endif
};

} // namespace GBM
} // namespace WINDOWING
} // namespace KODI
