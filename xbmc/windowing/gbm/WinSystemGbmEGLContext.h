/*
 *  Copyright (C) 2005-2018 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#pragma once

#include "WinSystemGbm.h"
#include "utils/EGLFence.h"
#include "utils/EGLUtils.h"
#include "windowing/linux/WinSystemEGL.h"

#include <memory>
#ifdef HAVE_DVBRIDGE
#include "DVWindowRetirement.h"
#endif

namespace KODI
{
namespace WINDOWING
{
namespace GBM
{

class CVaapiProxy;

class CWinSystemGbmEGLContext : public KODI::WINDOWING::LINUX::CWinSystemEGL, public CWinSystemGbm
{
public:
  ~CWinSystemGbmEGLContext() override;

  bool DestroyWindowSystem() override;
  bool CreateNewWindow(const std::string& name, bool fullScreen, RESOLUTION_INFO& res) override;
  bool DestroyWindow() override;
  void SetDirtyRegions(const CDirtyRegionList& dirtyRegions) override
  {
    m_eglContext.SetDamagedRegions(dirtyRegions);
  }
  int GetBufferAge() override { return m_eglContext.GetBufferAge(); }
  int GetOutputBitDepth() const override { return m_eglContext.GetConfigAttrib(EGL_RED_SIZE); }

  bool BindTextureUploadContext() override;
  bool UnbindTextureUploadContext() override;
  bool HasContext() override;

  bool SetVideoOutput(const VideoPicture* videoPicture) override;

protected:
  CWinSystemGbmEGLContext(EGLenum platform, std::string const& platformExtension)
    : CWinSystemEGL{platform, platformExtension}
  {
  }

  /**
   * Inheriting classes should override InitWindowSystem() without parameters
   * and call this function there with appropriate parameters
   */
  bool InitWindowSystemEGL(EGLint renderableType, EGLint apiType);
  bool ChooseEGLConfig(EGLint renderableType, int bitDepth = 8);
  virtual bool CreateContext() = 0;
  virtual int VideoOutputBitDepth(const VideoPicture* picture) const;
  // Opt-in only for DV frame transactions. Ordinary output keeps native semantics.
  void UseDVOutputSurfaceTransaction(bool enabled) { m_dvOutputSurface = enabled; }
  bool CompleteDVOutputSurface(bool submitted);
  virtual bool RejectDVOutputSurface() { return CompleteDVOutputSurface(false); }
  bool HasPendingDVOutputSurface() const { return m_dvSurfacePending; }

  EGLint m_renderableType{0};
  bool m_dvOutputSurface{}, m_dvSurfacePending{};
  CDRMPlane* m_priorOutputPlane{};
  uint32_t m_priorOutputFormat{};
  uint64_t m_priorOutputModifier{};
  bool m_priorOutputVideo{};
#ifdef HAVE_DVBRIDGE
  std::shared_ptr<CDVBridgeGLES::Context> m_nativeDVRetirement;
  std::shared_ptr<void> m_nativeDVProperties;
#endif

  std::unique_ptr<KODI::UTILS::EGL::CEGLFence> m_eglFence;

  struct delete_CVaapiProxy
  {
    void operator()(CVaapiProxy* p) const;
  };
  std::unique_ptr<CVaapiProxy, delete_CVaapiProxy> m_vaapiProxy;
};

} // namespace GBM
} // namespace WINDOWING
} // namespace KODI
