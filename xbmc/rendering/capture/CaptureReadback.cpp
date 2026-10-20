/*
 *  Copyright (C) 2026 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#include "CaptureReadback.h"

#include "utils/log.h"

#include <cstddef>
#include <algorithm>
#include <cmath>

#if defined(HAS_GL) || defined(HAS_GLES)
#include "system_gl.h"
#endif

namespace KODI
{
namespace RENDERING
{
namespace CAPTURE
{

#if defined(HAS_GL) || HAS_GLES == 3

bool ReadFramebufferRegion(int x,
                           int y,
                           unsigned int width,
                           unsigned int height,
                           int bitDepth,
                           bool flipY,
                           ReadbackBuffer& out)
{
  if (width == 0 || height == 0)
    return false;

  while (glGetError() != GL_NO_ERROR)
    ;

  GLenum glFormat = GL_RGBA;
  GLenum glType = GL_UNSIGNED_BYTE;
  AVPixelFormat avFormat = AV_PIX_FMT_RGBA;
  unsigned int rowBytes = width * 4;

  if (bitDepth <= 8)
  {
#ifdef HAS_GL
    glFormat = GL_BGRA; // desktop GL reads BGRA directly
    avFormat = AV_PIX_FMT_BGRA;
#endif
  }
  else
  {
#ifdef HAS_GL
    // desktop GL normalizes any framebuffer depth to 16-bit per channel on read
    glType = GL_UNSIGNED_SHORT;
    avFormat = AV_PIX_FMT_RGBA64LE;
    rowBytes = width * 8;
#else
    // GLES guarantees only RGBA/UNSIGNED_BYTE; negotiate the deep readback type
    GLint readFormat = GL_RGBA;
    GLint readType = GL_UNSIGNED_BYTE;
    glGetIntegerv(GL_IMPLEMENTATION_COLOR_READ_FORMAT, &readFormat);
    glGetIntegerv(GL_IMPLEMENTATION_COLOR_READ_TYPE, &readType);
    if (readFormat == GL_RGBA && readType == GL_UNSIGNED_INT_2_10_10_10_REV)
    {
      glType = GL_UNSIGNED_INT_2_10_10_10_REV;
      avFormat = AV_PIX_FMT_X2BGR10LE; // R in the low field, as GL_RGBA/2_10_10_10_REV
    }
    else if (readFormat == GL_RGBA && readType == GL_UNSIGNED_SHORT)
    {
      glType = GL_UNSIGNED_SHORT;
      avFormat = AV_PIX_FMT_RGBA64LE;
      rowBytes = width * 8;
    }
    else if (readFormat == GL_RGBA && (readType == GL_FLOAT || readType == GL_HALF_FLOAT))
    {
      glType = GL_FLOAT;
      avFormat = AV_PIX_FMT_RGBA64LE;
      rowBytes = width * 8;
    }
    // else: no deep readback offered; fall through to the 8-bit RGBA defaults
#endif
  }

  out.pixels.reset(new uint8_t[static_cast<size_t>(rowBytes) * height]);
  const size_t samples = static_cast<size_t>(width) * height * 4;
  std::unique_ptr<float[]> floats;
  if (glType == GL_FLOAT)
    floats = std::make_unique<float[]>(samples);
  glReadPixels(x, y, static_cast<GLsizei>(width), static_cast<GLsizei>(height), glFormat, glType,
               floats ? static_cast<void*>(floats.get()) : out.pixels.get());
  if (glGetError() != GL_NO_ERROR)
  {
    CLog::LogF(LOGWARNING, "capture readback: glReadPixels failed");
    return false;
  }
  if (floats)
  {
    auto* values = reinterpret_cast<uint16_t*>(out.pixels.get());
    for (size_t i = 0; i < samples; ++i)
      values[i] = std::isfinite(floats[i]) ? static_cast<uint16_t>(
          std::lround(std::clamp(floats[i], 0.0f, 1.0f) * 65535.0f)) : 0;
  }

  out.width = width;
  out.height = height;
  out.stride = flipY ? -static_cast<int>(rowBytes) : static_cast<int>(rowBytes);
  out.format = avFormat;
  return true;
}

#elif HAS_GLES == 2

bool ReadFramebufferRegion(int x,
                           int y,
                           unsigned int width,
                           unsigned int height,
                           int /*bitDepth*/,
                           bool flipY,
                           ReadbackBuffer& out)
{
  // GLES2 guarantees no deep readback: always 8-bit RGBA, tonemap by tags
  if (width == 0 || height == 0)
    return false;

  const unsigned int rowBytes = width * 4;
  out.pixels.reset(new uint8_t[static_cast<size_t>(rowBytes) * height]);

  while (glGetError() != GL_NO_ERROR)
    ;
  glReadPixels(x, y, static_cast<GLsizei>(width), static_cast<GLsizei>(height), GL_RGBA,
               GL_UNSIGNED_BYTE, out.pixels.get());
  if (glGetError() != GL_NO_ERROR)
  {
    CLog::LogF(LOGWARNING, "capture readback: glReadPixels failed");
    return false;
  }

  out.width = width;
  out.height = height;
  out.stride = flipY ? -static_cast<int>(rowBytes) : static_cast<int>(rowBytes);
  out.format = AV_PIX_FMT_RGBA;
  return true;
}

#else

bool ReadFramebufferRegion(int, int, unsigned int, unsigned int, int, bool, ReadbackBuffer&)
{
  // no GL context (e.g. Direct3D): callers read through their own API
  return false;
}

#endif

} // namespace CAPTURE
} // namespace RENDERING
} // namespace KODI
