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
  ~CWinSystemGbmEGLContext() override = default;

  bool DestroyWindowSystem() override;
  bool CreateNewWindow(const std::string& name, bool fullScreen, RESOLUTION_INFO& res) override;
  bool DestroyWindow() override;
  void SetDirtyRegions(const CDirtyRegionList& dirtyRegions) override
  {
    m_eglContext.SetDamagedRegions(dirtyRegions);
  }
  int GetBufferAge() override { return m_eglContext.GetBufferAge(); }

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

  /**
   * @brief Insert the GPU-side KMS fence wait before eglSwapBuffers.
   *
   * Deferred by one frame: waits on the out-fence of the frame *before* last
   * rather than the most recent flip, so the GPU may render one frame ahead of
   * the display (triple-buffered pipelining). Call only on a rendered, async
   * (non video-layer) frame. Does not block the CPU.
   */
  void FencePreSwap();

  /**
   * @brief Hand the GPU render fence to KMS as IN_FENCE_FD after eglSwapBuffers.
   *
   * Pairs with FencePreSwap(); call under the same conditions.
   */
  void FencePostSwap();

  EGLint m_renderableType{0};

  std::unique_ptr<KODI::UTILS::EGL::CEGLFence> m_eglFence;

  /**
   * @brief A dup of the previous frame's KMS out-fence, held one frame over.
   *
   * Used as the GPU-side render gate (the frame-before-last flip) so the GPU
   * may render ahead while the previous flip is still pending, while the CPU
   * back-pressure waits on the more recent flip. -1 when none is pending.
   * Consumed (and closed) by WaitSyncGPU() on the next frame, or by
   * DestroyWindow() if no further frame runs.
   */
  int m_kmsFenceFd{-1};

  struct delete_CVaapiProxy
  {
    void operator()(CVaapiProxy* p) const;
  };
  std::unique_ptr<CVaapiProxy, delete_CVaapiProxy> m_vaapiProxy;
};

} // namespace GBM
} // namespace WINDOWING
} // namespace KODI
