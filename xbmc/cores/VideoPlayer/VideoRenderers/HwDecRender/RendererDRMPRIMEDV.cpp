/*
 *  Copyright (C) 2026 Team LibreELEC
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#include "RendererDRMPRIMEDV.h"

#include "ServiceBroker.h"
#include "cores/VideoPlayer/Buffers/VideoBufferDRMPRIME.h"
#include "cores/VideoPlayer/DVDCodecs/Video/DVDVideoCodec.h"
#include "cores/VideoPlayer/VideoRenderers/RenderFactory.h"
#include "settings/Settings.h"
#include "settings/SettingsComponent.h"
#include "utils/EGLFence.h"
#include "utils/EGLUtils.h"
#include "utils/GLUtils.h"
#include "utils/log.h"
#include "windowing/GraphicContext.h"
#include "windowing/gbm/WinSystemGbmGLESContext.h"

#include <cmath>
#include <cstring>

#include <drm_fourcc.h>

extern "C"
{
#include <libavutil/hwcontext_drm.h>
#include <libavutil/rational.h>
}

#ifndef GL_R16_EXT
#define GL_R16_EXT 0x822A
#endif
#ifndef GL_RG16_EXT
#define GL_RG16_EXT 0x822C
#endif

using namespace KODI::UTILS::EGL;

namespace
{
// NV15: 4:2:0, 10-bit samples packed LSB-first, 4 samples in 5 bytes, with no
// padding between samples. The raw plane is imported as R8 and each output
// sample is assembled from the two bytes that hold its 10 bits. The result is
// written MSB-aligned into UNORM16 (value << 6), i.e. exactly the P010 texel
// a VA-API import would have produced, so CB1 sees an ordinary P010 picture.
constexpr const char* UNPACK_VERTEX = R"(#version 300 es
void main()
{
  // Full-screen triangle; no vertex attributes needed.
  vec2 pos = vec2(float((gl_VertexID << 1) & 2), float(gl_VertexID & 2));
  gl_Position = vec4(pos * 2.0 - 1.0, 0.0, 1.0);
}
)";

constexpr const char* UNPACK_FRAGMENT = R"(#version 300 es
precision highp float;
precision highp int;
uniform highp sampler2D u_source;
uniform int u_chroma;
out highp vec4 o_colour;

uint byteAt(int x, int y)
{
  return uint(texelFetch(u_source, ivec2(x, y), 0).r * 255.0 + 0.5);
}

float sampleAt(int index, int y)
{
  int bit = index * 10;
  int b = bit >> 3;
  uint word = byteAt(b, y) | (byteAt(b + 1, y) << 8u);
  uint value = (word >> uint(bit & 7)) & 1023u;
  return float(value << 6u) / 65535.0;
}

void main()
{
  ivec2 p = ivec2(gl_FragCoord.xy);
  if (u_chroma == 0)
    o_colour = vec4(sampleAt(p.x, p.y), 0.0, 0.0, 1.0);
  else
    o_colour = vec4(sampleAt(2 * p.x, p.y), sampleAt(2 * p.x + 1, p.y), 0.0, 1.0);
}
)";

GLuint CompileShader(GLenum type, const char* source)
{
  GLuint shader = glCreateShader(type);
  glShaderSource(shader, 1, &source, nullptr);
  glCompileShader(shader);
  GLint ok = GL_FALSE;
  glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
  if (ok != GL_TRUE)
  {
    char log[1024]{};
    glGetShaderInfoLog(shader, sizeof(log), nullptr, log);
    CLog::Log(LOGERROR, "CRendererDRMPRIMEDV: NV15 unpack shader compile failed: {}", log);
    glDeleteShader(shader);
    return 0;
  }
  return shader;
}

bool HasGLExtension(const char* name)
{
  const auto* extensions = reinterpret_cast<const char*>(glGetString(GL_EXTENSIONS));
  if (!extensions)
    return false;
  const size_t length = std::strlen(name);
  for (const char* p = std::strstr(extensions, name); p; p = std::strstr(p + length, name))
  {
    if ((p == extensions || p[-1] == ' ') && (p[length] == ' ' || p[length] == '\0'))
      return true;
  }
  return false;
}
} // namespace

CRendererDRMPRIMEDV::SourceTextures::SourceTextures(EGLDisplay eglDisplay,
                                                    PFNEGLDESTROYIMAGEKHRPROC destroyImage)
  : display(eglDisplay), eglDestroyImageKHR(destroyImage)
{
}

CRendererDRMPRIMEDV::SourceTextures::~SourceTextures()
{
  for (auto& texture : textures)
  {
    if (texture)
      glDeleteTextures(1, &texture);
    texture = 0;
  }
  ReleaseImages();
}

void CRendererDRMPRIMEDV::SourceTextures::ReleaseImages()
{
  for (auto& image : images)
  {
    if (image != EGL_NO_IMAGE_KHR && eglDestroyImageKHR)
      eglDestroyImageKHR(display, image);
    image = EGL_NO_IMAGE_KHR;
  }
}

CRendererDRMPRIMEDV::CRendererDRMPRIMEDV() = default;

CRendererDRMPRIMEDV::~CRendererDRMPRIMEDV()
{
  for (int i = 0; i < MAX_RENDER_BUFFERS; ++i)
    DeleteTexture(i);
  m_unpackPool.clear();
  ReleaseUnpack();
}

CRendererDRMPRIMEDV::Layout CRendererDRMPRIMEDV::LayoutFor(const AVDRMFrameDescriptor* desc)
{
  if (!desc || desc->nb_layers < 1 || desc->nb_objects < 1)
    return Layout::NONE;

  // Either one layer with two planes (V4L2 request, rkvdec) or two
  // single-plane layers (VA-API style exports).
  uint32_t format = desc->layers[0].format;
  if (desc->nb_layers == 2 && desc->layers[0].nb_planes == 1 && desc->layers[1].nb_planes == 1)
  {
    if (format == DRM_FORMAT_R8 && desc->layers[1].format == DRM_FORMAT_GR88)
      return Layout::NV12;
    if (format == DRM_FORMAT_R16 && desc->layers[1].format == DRM_FORMAT_GR1616)
      return Layout::P010;
    return Layout::NONE;
  }
  if (desc->nb_layers != 1 || desc->layers[0].nb_planes != 2)
    return Layout::NONE;

  switch (format)
  {
    case DRM_FORMAT_NV12:
      return Layout::NV12;
    case DRM_FORMAT_P010:
      return Layout::P010;
    case DRM_FORMAT_NV15:
      return Layout::NV15;
    default:
      return Layout::NONE;
  }
}

bool CRendererDRMPRIMEDV::LayoutUsable(const AVDRMFrameDescriptor* desc,
                                       Layout layout,
                                       int width,
                                       int height)
{
  if (layout == Layout::NONE || width < 2 || height < 2 || width > 4096 || height > 2304)
    return false;
  for (int i = 0; i < desc->nb_objects; ++i)
  {
    // The NV15 unpack addresses bytes directly, so tiled or compressed layouts are out.
    const uint64_t modifier = desc->objects[i].format_modifier;
    if (modifier != DRM_FORMAT_MOD_LINEAR && modifier != DRM_FORMAT_MOD_INVALID)
      return false;
    if (desc->objects[i].fd < 0)
      return false;
  }
  return true;
}

CBaseRenderer* CRendererDRMPRIMEDV::Create(CVideoBuffer* buffer)
{
  auto* buf = dynamic_cast<CVideoBufferDRMPRIME*>(buffer);
  if (!buf)
    return nullptr;

  auto settings = CServiceBroker::GetSettingsComponent()->GetSettings();
  if (!settings->GetBool(CSettings::SETTING_VIDEOPLAYER_USEPRIMEDECODER))
    return nullptr;

  // Same decision CLinuxRendererGLES::ConfigureVideoOutput() makes; anything the
  // bridge will not process keeps the ordinary DRM PRIME renderers.
  if (!KODI::WINDOWING::GBM::CWinSystemGbmGLESContext::DVBridgeRequested(&buf->GetPicture()))
    return nullptr;

  if (!buf->AcquireDescriptor())
    return nullptr;
  const AVDRMFrameDescriptor* desc = buf->GetDescriptor();
  const Layout layout = LayoutFor(desc);
  const bool usable = desc && LayoutUsable(desc, layout, static_cast<int>(buf->GetWidth()),
                                           static_cast<int>(buf->GetHeight()));
  buf->ReleaseDescriptor();
  if (!usable)
  {
    CLog::Log(LOGWARNING, "CRendererDRMPRIMEDV: unsupported DRM PRIME layout for Dolby Vision");
    return nullptr;
  }

  return new CRendererDRMPRIMEDV();
}

void CRendererDRMPRIMEDV::Register()
{
  // Renderers are tried in id order: this id sorts ahead of "drm_prime" and
  // "drm_prime_gles" so DV pictures reach the bridge first.
  VIDEOPLAYER::CRendererFactory::RegisterRenderer("dovi_drm_prime", CRendererDRMPRIMEDV::Create);
}

bool CRendererDRMPRIMEDV::InitInterop()
{
  if (m_eglCreateImageKHR)
    return true;

  auto* window = dynamic_cast<KODI::WINDOWING::GBM::CWinSystemGbmGLESContext*>(
      CServiceBroker::GetWinSystem());
  if (!window)
    return false;

  m_eglDisplay = window->GetEGLDisplay();
  m_eglCreateImageKHR =
      CEGLUtils::GetRequiredProcAddress<PFNEGLCREATEIMAGEKHRPROC>("eglCreateImageKHR");
  m_eglDestroyImageKHR =
      CEGLUtils::GetRequiredProcAddress<PFNEGLDESTROYIMAGEKHRPROC>("eglDestroyImageKHR");
  m_glEGLImageTargetTexture2DOES =
      CEGLUtils::GetRequiredProcAddress<PFNGLEGLIMAGETARGETTEXTURE2DOESPROC>(
          "glEGLImageTargetTexture2DOES");
  m_hasModifiers =
      CEGLUtils::HasExtension(m_eglDisplay, "EGL_EXT_image_dma_buf_import_modifiers");
  return m_eglDisplay != EGL_NO_DISPLAY && m_eglCreateImageKHR && m_eglDestroyImageKHR &&
         m_glEGLImageTargetTexture2DOES;
}

bool CRendererDRMPRIMEDV::Configure(const VideoPicture& picture,
                                    float fps,
                                    unsigned int orientation)
{
  auto* buf = dynamic_cast<CVideoBufferDRMPRIME*>(picture.videoBuffer);
  if (!buf || !InitInterop())
    return false;

  if (!buf->AcquireDescriptor())
    return false;
  m_layout = LayoutFor(buf->GetDescriptor());
  buf->ReleaseDescriptor();
  if (m_layout == Layout::NONE)
    return false;

  if (m_layout == Layout::NV15 && !InitUnpack())
    return false;

  for (int i = 0; i < MAX_RENDER_BUFFERS; ++i)
  {
    m_sources[i].reset();
    m_fences[i] = std::make_unique<CEGLFence>(m_eglDisplay);
  }

  CLog::Log(LOGINFO, "CRendererDRMPRIMEDV: configured {}x{} {}", picture.iWidth, picture.iHeight,
            m_layout == Layout::NV15  ? "NV15 (shader unpack to P010)"
            : m_layout == Layout::P010 ? "P010"
                                       : "NV12");

  return CLinuxRendererGLES::Configure(picture, fps, orientation);
}

bool CRendererDRMPRIMEDV::ConfigChanged(const VideoPicture& picture)
{
  auto* buf = dynamic_cast<CVideoBufferDRMPRIME*>(picture.videoBuffer);
  if (!buf)
    return true;
  if (!buf->AcquireDescriptor())
    return true;
  const Layout layout = LayoutFor(buf->GetDescriptor());
  buf->ReleaseDescriptor();
  return layout != m_layout || CLinuxRendererGLES::ConfigChanged(picture);
}

bool CRendererDRMPRIMEDV::CreateTexture(int index)
{
  CPictureBuffer& buf = m_buffers[index];
  YuvImage& im = buf.image;
  CYuvPlane(&planes)[YuvImage::MAX_PLANES] = buf.fields[0];

  DeleteTexture(index);

  im = {};
  std::fill(std::begin(planes), std::end(planes), CYuvPlane{});
  im.height = m_sourceHeight;
  im.width = m_sourceWidth;
  im.cshift_x = 1;
  im.cshift_y = 1;

  // Textures are supplied per picture by UploadTexture().
  planes[0].id = 1;
  return true;
}

void CRendererDRMPRIMEDV::DeleteTexture(int index)
{
  m_sources[index].reset();
  CPictureBuffer& buf = m_buffers[index];
  buf.fields[FIELD_FULL][0].id = 0;
  buf.fields[FIELD_FULL][1].id = 0;
  buf.fields[FIELD_FULL][2].id = 0;
}

bool CRendererDRMPRIMEDV::ImportPlane(const AVDRMFrameDescriptor* desc,
                                      int layerIndex,
                                      uint32_t fourcc,
                                      int width,
                                      int height,
                                      SourceTextures& target,
                                      int slot)
{
  // Plane `slot` of a single two-plane layer, or plane 0 of layer `slot`.
  const bool split = desc->nb_layers == 2;
  const AVDRMLayerDescriptor& layer = desc->layers[split ? slot : layerIndex];
  const AVDRMPlaneDescriptor& plane = layer.planes[split ? 0 : slot];
  if (plane.object_index < 0 || plane.object_index >= desc->nb_objects)
    return false;
  const AVDRMObjectDescriptor& object = desc->objects[plane.object_index];

  CEGLAttributes<8> attribs;
  attribs.Add({{EGL_LINUX_DRM_FOURCC_EXT, static_cast<EGLint>(fourcc)},
               {EGL_WIDTH, width},
               {EGL_HEIGHT, height},
               {EGL_DMA_BUF_PLANE0_FD_EXT, object.fd},
               {EGL_DMA_BUF_PLANE0_OFFSET_EXT, static_cast<EGLint>(plane.offset)},
               {EGL_DMA_BUF_PLANE0_PITCH_EXT, static_cast<EGLint>(plane.pitch)}});
  if (m_hasModifiers && object.format_modifier != DRM_FORMAT_MOD_INVALID)
  {
    attribs.Add(
        {{EGL_DMA_BUF_PLANE0_MODIFIER_LO_EXT, static_cast<EGLint>(object.format_modifier)},
         {EGL_DMA_BUF_PLANE0_MODIFIER_HI_EXT, static_cast<EGLint>(object.format_modifier >> 32)}});
  }

  EGLImageKHR image = m_eglCreateImageKHR(m_eglDisplay, EGL_NO_CONTEXT, EGL_LINUX_DMA_BUF_EXT,
                                          nullptr, attribs.Get());
  if (image == EGL_NO_IMAGE_KHR)
  {
    CLog::Log(LOGERROR, "CRendererDRMPRIMEDV: eglCreateImageKHR failed for plane {} ({:#x})",
              slot, eglGetError());
    return false;
  }

  GLuint texture = 0;
  glGenTextures(1, &texture);
  glBindTexture(GL_TEXTURE_2D, texture);
  m_glEGLImageTargetTexture2DOES(GL_TEXTURE_2D, image);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
  glBindTexture(GL_TEXTURE_2D, 0);

  target.images[slot] = image;
  target.textures[slot] = texture;
  target.widths[slot] = width;
  target.heights[slot] = height;
  return glGetError() == GL_NO_ERROR;
}

std::shared_ptr<CRendererDRMPRIMEDV::SourceTextures> CRendererDRMPRIMEDV::ImportDirect(
    const AVDRMFrameDescriptor* desc)
{
  const bool p010 = m_layout == Layout::P010;
  const int chromaWidth = (static_cast<int>(m_sourceWidth) + 1) >> 1;
  const int chromaHeight = (static_cast<int>(m_sourceHeight) + 1) >> 1;
  auto source = std::make_shared<SourceTextures>(m_eglDisplay, m_eglDestroyImageKHR);
  if (!ImportPlane(desc, 0, p010 ? DRM_FORMAT_R16 : DRM_FORMAT_R8,
                   static_cast<int>(m_sourceWidth), static_cast<int>(m_sourceHeight), *source,
                   0) ||
      !ImportPlane(desc, 0, p010 ? DRM_FORMAT_GR1616 : DRM_FORMAT_GR88, chromaWidth,
                   chromaHeight, *source, 1))
    return nullptr;
  return source;
}

bool CRendererDRMPRIMEDV::InitUnpack()
{
  if (m_unpackProgram)
    return true;

  // UNORM16 render targets are required to reproduce P010 exactly.
  if (!HasGLExtension("GL_EXT_texture_norm16"))
  {
    CLog::Log(LOGERROR, "CRendererDRMPRIMEDV: NV15 needs GL_EXT_texture_norm16");
    return false;
  }

  GLuint vertex = CompileShader(GL_VERTEX_SHADER, UNPACK_VERTEX);
  GLuint fragment = CompileShader(GL_FRAGMENT_SHADER, UNPACK_FRAGMENT);
  if (!vertex || !fragment)
  {
    glDeleteShader(vertex);
    glDeleteShader(fragment);
    return false;
  }

  m_unpackProgram = glCreateProgram();
  glAttachShader(m_unpackProgram, vertex);
  glAttachShader(m_unpackProgram, fragment);
  glLinkProgram(m_unpackProgram);
  glDeleteShader(vertex);
  glDeleteShader(fragment);

  GLint linked = GL_FALSE;
  glGetProgramiv(m_unpackProgram, GL_LINK_STATUS, &linked);
  if (linked != GL_TRUE)
  {
    CLog::Log(LOGERROR, "CRendererDRMPRIMEDV: NV15 unpack program link failed");
    ReleaseUnpack();
    return false;
  }
  m_unpackSourceLoc = glGetUniformLocation(m_unpackProgram, "u_source");
  m_unpackChromaLoc = glGetUniformLocation(m_unpackProgram, "u_chroma");
  glGenFramebuffers(1, &m_unpackFramebuffer);
  return m_unpackFramebuffer != 0;
}

void CRendererDRMPRIMEDV::ReleaseUnpack()
{
  if (m_unpackFramebuffer)
    glDeleteFramebuffers(1, &m_unpackFramebuffer);
  if (m_unpackProgram)
    glDeleteProgram(m_unpackProgram);
  m_unpackFramebuffer = 0;
  m_unpackProgram = 0;
}

std::shared_ptr<CRendererDRMPRIMEDV::SourceTextures> CRendererDRMPRIMEDV::AcquireUnpackTarget()
{
  const int width = static_cast<int>(m_sourceWidth);
  const int height = static_cast<int>(m_sourceHeight);

  // Reuse a target nobody else holds: not a buffer slot, not a CB1 lease.
  for (auto& target : m_unpackPool)
  {
    if (target.use_count() == 1 && target->widths[0] == width && target->heights[0] == height)
      return target;
  }

  auto target = std::make_shared<SourceTextures>(m_eglDisplay, m_eglDestroyImageKHR);
  const GLenum formats[2] = {GL_R16_EXT, GL_RG16_EXT};
  for (int p = 0; p < 2; ++p)
  {
    target->widths[p] = p ? (width + 1) >> 1 : width;
    target->heights[p] = p ? (height + 1) >> 1 : height;
    glGenTextures(1, &target->textures[p]);
    glBindTexture(GL_TEXTURE_2D, target->textures[p]);
    glTexStorage2D(GL_TEXTURE_2D, 1, formats[p], target->widths[p], target->heights[p]);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
  }
  glBindTexture(GL_TEXTURE_2D, 0);
  if (glGetError() != GL_NO_ERROR)
    return nullptr;

  // Bounded: decoder slots plus the frames CB1 may still be reading.
  if (m_unpackPool.size() < static_cast<size_t>(MAX_RENDER_BUFFERS) + 4)
    m_unpackPool.push_back(target);
  return target;
}

std::shared_ptr<CRendererDRMPRIMEDV::SourceTextures> CRendererDRMPRIMEDV::ImportNV15(
    const AVDRMFrameDescriptor* desc)
{
  const int width = static_cast<int>(m_sourceWidth);
  const int height = static_cast<int>(m_sourceHeight);
  const int chromaHeight = (height + 1) >> 1;
  // Both planes hold width samples per row (luma: Y, chroma: Cb/Cr pairs) of 10 bits.
  const int rowBytes = (width * 10 + 7) / 8;

  // The last sample of a row ends in byte rowBytes - 1, so the shader never reads past it.
  SourceTextures raw(m_eglDisplay, m_eglDestroyImageKHR);
  if (!ImportPlane(desc, 0, DRM_FORMAT_R8, rowBytes, height, raw, 0) ||
      !ImportPlane(desc, 0, DRM_FORMAT_R8, rowBytes, chromaHeight, raw, 1))
    return nullptr;

  auto target = AcquireUnpackTarget();
  if (!target)
    return nullptr;

  GLint previousFramebuffer = 0;
  GLint previousProgram = 0;
  GLint previousViewport[4]{};
  GLint previousTexture = 0;
  GLint previousActive = 0;
  glGetIntegerv(GL_FRAMEBUFFER_BINDING, &previousFramebuffer);
  glGetIntegerv(GL_CURRENT_PROGRAM, &previousProgram);
  glGetIntegerv(GL_VIEWPORT, previousViewport);
  glGetIntegerv(GL_ACTIVE_TEXTURE, &previousActive);
  glActiveTexture(GL_TEXTURE0);
  glGetIntegerv(GL_TEXTURE_BINDING_2D, &previousTexture);
  const GLboolean blend = glIsEnabled(GL_BLEND);
  const GLboolean scissor = glIsEnabled(GL_SCISSOR_TEST);
  const GLboolean depth = glIsEnabled(GL_DEPTH_TEST);
  glDisable(GL_BLEND);
  glDisable(GL_SCISSOR_TEST);
  glDisable(GL_DEPTH_TEST);

  glUseProgram(m_unpackProgram);
  glUniform1i(m_unpackSourceLoc, 0);
  glBindFramebuffer(GL_FRAMEBUFFER, m_unpackFramebuffer);
  bool ok = true;
  for (int p = 0; p < 2 && ok; ++p)
  {
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
                           target->textures[p], 0);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
    {
      CLog::Log(LOGERROR, "CRendererDRMPRIMEDV: UNORM16 target {} is not renderable", p);
      ok = false;
      break;
    }
    glViewport(0, 0, target->widths[p], target->heights[p]);
    glBindTexture(GL_TEXTURE_2D, raw.textures[p]);
    glUniform1i(m_unpackChromaLoc, p);
    glDrawArrays(GL_TRIANGLES, 0, 3);
  }
  glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, 0, 0);

  glBindFramebuffer(GL_FRAMEBUFFER, previousFramebuffer);
  glUseProgram(previousProgram);
  glViewport(previousViewport[0], previousViewport[1], previousViewport[2], previousViewport[3]);
  glBindTexture(GL_TEXTURE_2D, previousTexture);
  glActiveTexture(previousActive);
  if (blend)
    glEnable(GL_BLEND);
  if (scissor)
    glEnable(GL_SCISSOR_TEST);
  if (depth)
    glEnable(GL_DEPTH_TEST);

  // The raw imports are only read by the draws above; GL keeps them alive
  // until those complete, so they can be released now.
  return ok && glGetError() == GL_NO_ERROR ? target : nullptr;
}

bool CRendererDRMPRIMEDV::UploadTexture(int index)
{
  auto* window = dynamic_cast<KODI::WINDOWING::GBM::CWinSystemGbmGLESContext*>(
      CServiceBroker::GetWinSystem());
  if (window)
    window->ObserveDVImport({}, {}, false);

  CPictureBuffer& buf = m_buffers[index];
  auto* video = dynamic_cast<CVideoBufferDRMPRIME*>(buf.videoBuffer);
  if (!video || !video->IsValid() || !video->AcquireDescriptor())
    return false;

  const AVDRMFrameDescriptor* desc = video->GetDescriptor();
  std::shared_ptr<SourceTextures> source;
  if (desc && LayoutFor(desc) == m_layout &&
      LayoutUsable(desc, m_layout, static_cast<int>(m_sourceWidth),
                   static_cast<int>(m_sourceHeight)))
    source = m_layout == Layout::NV15 ? ImportNV15(desc) : ImportDirect(desc);
  video->ReleaseDescriptor();
  if (!source)
    return false;

  m_sources[index] = source;

  CYuvPlane(&planes)[YuvImage::MAX_PLANES] = buf.fields[0];
  for (int p = 0; p < 3; ++p)
  {
    const int s = p ? 1 : 0;
    planes[p].id = source->textures[s];
    planes[p].texwidth = static_cast<unsigned>(source->widths[s]);
    planes[p].texheight = static_cast<unsigned>(source->heights[s]);
    planes[p].pixpertex_x = 1;
    planes[p].pixpertex_y = 1;
  }
  for (int p = 0; p < 2; ++p)
  {
    glBindTexture(m_textureTarget, planes[p].id);
    glTexParameteri(m_textureTarget, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(m_textureTarget, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glBindTexture(m_textureTarget, 0);
  }
  CalculateTextureSourceRects(index, 3);

  if (window && buf.doviSourcePicture)
  {
    const auto binding = buf.doviSourcePicture->doviSourceBinding.Valid()
                             ? buf.doviSourcePicture->doviSourceBinding
                             : buf.doviMetadata.Binding();
    window->ObserveDVImport(binding, DVBridgeGeometry(),
                            DVBridgeOrientationSupported(m_renderOrientation));
  }
  return true;
}

void CRendererDRMPRIMEDV::AfterRenderHook(int index)
{
  if (m_fences[index])
  {
    m_fences[index]->DestroyFence();
    m_fences[index]->CreateFence();
  }
}

bool CRendererDRMPRIMEDV::NeedBuffer(int index)
{
  return m_fences[index] && !m_fences[index]->IsSignaled();
}

void CRendererDRMPRIMEDV::ReleaseBuffer(int index)
{
  if (m_fences[index])
    m_fences[index]->DestroyFence();
  m_sources[index].reset();
  CLinuxRendererGLES::ReleaseBuffer(index);
}

bool CRendererDRMPRIMEDV::DVBridgeOrientationSupported(unsigned int orientation) const
{
  // Native orientation includes the saved playback rotation; CB1 ignores only that part.
  return orientation == static_cast<unsigned int>(m_videoSettings.m_Orientation);
}

dvbridge_geometry CRendererDRMPRIMEDV::DVBridgeGeometry()
{
  const CRect view = CServiceBroker::GetWinSystem()->GetGfxContext().GetViewWindow();
  CRect dest;
  CalcDestRect(view.x1, view.y1, view.Width(), view.Height(), GetAspectRatio(), 1.0f, 0.0f, dest,
               false);
  return {static_cast<int>(m_sourceWidth),
          static_cast<int>(m_sourceHeight),
          static_cast<int>(std::lround(dest.x1)),
          static_cast<int>(std::lround(dest.y1)),
          static_cast<int>(std::lround(dest.Width())),
          static_cast<int>(std::lround(dest.Height()))};
}

bool CRendererDRMPRIMEDV::BuildDVBridgeLayer(int idx, CDVBridgeGLES::Layer& layer)
{
  // CB1 reads the full decoded texture, independent of Kodi's view-mode crop.
  if (m_textureTarget != GL_TEXTURE_2D || !DVBridgeOrientationSupported(m_renderOrientation))
    return false;
  auto& buffer = m_buffers[idx];
  const auto& source = m_sources[idx];
  if (!source || !buffer.doviSourcePicture)
    return false;
  if (!buffer.doviMetadata.Data() || buffer.doviPts == DVD_NOPTS_VALUE)
    return false;

  layer.layout = m_layout == Layout::NV12 ? DVBRIDGE_NV12 : DVBRIDGE_P010;
  layer.width = static_cast<int>(m_sourceWidth);
  layer.height = static_cast<int>(m_sourceHeight);
  layer.pts = buffer.doviPts;
  layer.binding = buffer.doviMetadata.Binding();
  if (!std::isfinite(layer.pts))
    return false;

  const auto& picture = *buffer.doviSourcePicture;
  layer.sourceInfo = picture.doviSourceInfo;
  layer.chromaLocation = picture.chroma_position;

  struct ForeignLease
  {
    std::shared_ptr<void> imported;
    std::shared_ptr<VideoPicture> decoded;
  };
  layer.lease = std::make_shared<ForeignLease>(source, buffer.doviSourcePicture);
  for (int p = 0; p < 2; ++p)
    layer.planes[p] = {source->textures[p], source->widths[p], source->heights[p]};
  layer.planes[2] = {};
  return true;
}

bool CRendererDRMPRIMEDV::RenderDVBridge(int idx)
{
  auto* window = dynamic_cast<KODI::WINDOWING::GBM::CWinSystemGbmGLESContext*>(
      CServiceBroker::GetWinSystem());
  CDVBridgeGLES::Layer layer{};
  if (!window || !BuildDVBridgeLayer(idx, layer))
    return false;
  auto& buffer = m_buffers[idx];
  // FEL is not yet carried on DRM PRIME: the enhancement frame stays empty and
  // CB1 renders the base layer with its RPU (profiles 5, 8.x and 7 MEL).
  return window->RenderDVBridge(layer, buffer.doviEnhancement, buffer.doviEnhancementBinding,
                                buffer.doviMetadata, DVBridgeGeometry(), nullptr);
}
