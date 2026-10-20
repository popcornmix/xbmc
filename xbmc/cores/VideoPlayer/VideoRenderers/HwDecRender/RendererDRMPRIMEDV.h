/*
 *  Copyright (C) 2026 Team LibreELEC
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#pragma once

#include "cores/VideoPlayer/VideoRenderers/DVBridgeGLES.h"
#include "cores/VideoPlayer/VideoRenderers/LinuxRendererGLES.h"

#include <array>
#include <memory>
#include <vector>

#include <EGL/egl.h>
#include <EGL/eglext.h>

class CVideoBufferDRMPRIME;
struct AVDRMFrameDescriptor;

namespace KODI::UTILS::EGL
{
class CEGLFence;
}

/*!
 * \brief CB1 Dolby Vision renderer for DRM PRIME (V4L2 stateless) decoders.
 *
 * The VA-API counterpart is CRendererVAAPIGLES. CB1 consumes decoded pictures
 * as per-plane GL textures in NV12 (R8 + GR88) or P010 (R16 + GR1616) layout.
 * DRM PRIME frames in NV12 or P010 are imported directly from their dmabufs.
 * NV15 (packed 10-bit 4:2:0, as produced by rkvdec) has no GL equivalent, so
 * its planes are imported as raw bytes and unpacked by a bit-exact shader into
 * P010-equivalent textures (MSB-aligned 10-bit in UNORM16).
 *
 * Selected only for pictures the DV bridge will process; everything else keeps
 * the ordinary DRM PRIME plane and EGL renderers.
 */
class CRendererDRMPRIMEDV : public CLinuxRendererGLES
{
public:
  CRendererDRMPRIMEDV();
  ~CRendererDRMPRIMEDV() override;

  static CBaseRenderer* Create(CVideoBuffer* buffer);
  static void Register();

  bool Configure(const VideoPicture& picture, float fps, unsigned int orientation) override;
  bool ConfigChanged(const VideoPicture& picture) override;
  void ReleaseBuffer(int idx) override;
  bool NeedBuffer(int idx) override;

protected:
  EShaderFormat GetShaderFormat() override { return SHADER_NV12_RRG; }
  bool LoadShadersHook() override { return false; }
  bool RenderHook(int idx) override { return false; }
  void AfterRenderHook(int idx) override;

  bool CreateTexture(int index) override;
  void DeleteTexture(int index) override;
  bool UploadTexture(int index) override;

  bool RenderDVBridge(int idx) override;

private:
  enum class Layout
  {
    NONE,
    NV12,
    P010,
    NV15,
  };

  //! GL textures for one decoded picture, shared with CB1 for as long as it reads them.
  struct SourceTextures
  {
    SourceTextures(EGLDisplay display, PFNEGLDESTROYIMAGEKHRPROC destroyImage);
    ~SourceTextures();
    void ReleaseImages();

    EGLDisplay display{EGL_NO_DISPLAY};
    PFNEGLDESTROYIMAGEKHRPROC eglDestroyImageKHR{nullptr};
    std::array<EGLImageKHR, 2> images{EGL_NO_IMAGE_KHR, EGL_NO_IMAGE_KHR};
    std::array<GLuint, 2> textures{};
    std::array<int, 2> widths{};
    std::array<int, 2> heights{};
  };

  static Layout LayoutFor(const AVDRMFrameDescriptor* desc);
  static bool LayoutUsable(const AVDRMFrameDescriptor* desc, Layout layout, int width, int height);

  bool InitInterop();
  bool ImportPlane(const AVDRMFrameDescriptor* desc,
                   int layerIndex,
                   uint32_t fourcc,
                   int width,
                   int height,
                   SourceTextures& target,
                   int slot);
  std::shared_ptr<SourceTextures> ImportDirect(const AVDRMFrameDescriptor* desc);
  std::shared_ptr<SourceTextures> ImportNV15(const AVDRMFrameDescriptor* desc);
  std::shared_ptr<SourceTextures> AcquireUnpackTarget();
  bool InitUnpack();
  void ReleaseUnpack();

  bool BuildDVBridgeLayer(int idx, CDVBridgeGLES::Layer& layer);
  dvbridge_geometry DVBridgeGeometry();
  bool DVBridgeOrientationSupported(unsigned int orientation) const;

  Layout m_layout{Layout::NONE};
  std::array<std::shared_ptr<SourceTextures>, MAX_RENDER_BUFFERS> m_sources;
  std::array<std::unique_ptr<KODI::UTILS::EGL::CEGLFence>, MAX_RENDER_BUFFERS> m_fences;

  // NV15 unpack: a pool of UNORM16 targets recycled once CB1 drops its lease.
  std::vector<std::shared_ptr<SourceTextures>> m_unpackPool;
  GLuint m_unpackProgram{0};
  GLuint m_unpackFramebuffer{0};
  GLint m_unpackSourceLoc{-1};
  GLint m_unpackChromaLoc{-1};

  EGLDisplay m_eglDisplay{EGL_NO_DISPLAY};
  PFNEGLCREATEIMAGEKHRPROC m_eglCreateImageKHR{nullptr};
  PFNEGLDESTROYIMAGEKHRPROC m_eglDestroyImageKHR{nullptr};
  PFNGLEGLIMAGETARGETTEXTURE2DOESPROC m_glEGLImageTargetTexture2DOES{nullptr};
  bool m_hasModifiers{false};
};
