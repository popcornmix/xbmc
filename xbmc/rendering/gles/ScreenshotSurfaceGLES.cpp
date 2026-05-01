/*
 *  Copyright (C) 2005-2018 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#include "ScreenshotSurfaceGLES.h"

#include "ServiceBroker.h"
#include "guilib/GUIComponent.h"
#include "guilib/GUIWindowManager.h"
#include "utils/Screenshot.h"
#include "utils/log.h"
#include "windowing/GraphicContext.h"
#include "windowing/WinSystem.h"

#include <memory>
#include <mutex>
#include <vector>

#include "system_gl.h"

void CScreenshotSurfaceGLES::Register()
{
  CScreenShot::Register(CScreenshotSurfaceGLES::CreateSurface);
}

std::unique_ptr<IScreenshotSurface> CScreenshotSurfaceGLES::CreateSurface()
{
  return std::make_unique<CScreenshotSurfaceGLES>();
}

bool CScreenshotSurfaceGLES::Capture()
{
  CWinSystemBase* winsystem = CServiceBroker::GetWinSystem();
  if (!winsystem)
    return false;

  CGUIComponent* gui = CServiceBroker::GetGUI();
  if (!gui)
    return false;

  std::unique_lock lock(winsystem->GetGfxContext());
  gui->GetWindowManager().Render();

  //get current viewport
  GLint viewport[4];
  glGetIntegerv(GL_VIEWPORT, viewport);

  m_width = viewport[2] - viewport[0];
  m_height = viewport[3] - viewport[1];

  GLint redBits = 8;
  glGetIntegerv(GL_RED_BITS, &redBits);

  // GLES only guarantees GL_RGBA/GL_UNSIGNED_BYTE for glReadPixels.
  // Any other format must be queried via GL_IMPLEMENTATION_COLOR_READ_TYPE.
  // For >8-bit surfaces, the driver may support:
  //   GL_UNSIGNED_INT_2_10_10_10_REV -- packed 10:10:10:2 (10-bit surfaces)
  //   GL_UNSIGNED_SHORT -- 16-bit per channel (12-bit or 16-bit surfaces)
  enum class ReadbackMode { Uint8, Packed1010102, Uint16 };
  ReadbackMode readbackMode = ReadbackMode::Uint8;

#if HAS_GLES == 3
  if (redBits > 8)
  {
    GLint readFormat = GL_RGBA;
    GLint readType = GL_UNSIGNED_BYTE;
    glGetIntegerv(GL_IMPLEMENTATION_COLOR_READ_FORMAT, &readFormat);
    glGetIntegerv(GL_IMPLEMENTATION_COLOR_READ_TYPE, &readType);
    if (readFormat == GL_RGBA)
    {
      if (readType == GL_UNSIGNED_INT_2_10_10_10_REV)
        readbackMode = ReadbackMode::Packed1010102;
      else if (readType == GL_UNSIGNED_SHORT)
        readbackMode = ReadbackMode::Uint16;
    }
  }
#endif

  m_bitDepth = (readbackMode != ReadbackMode::Uint8) ? redBits : 8;

  if (readbackMode == ReadbackMode::Packed1010102)
  {
    // 10-bit surface: read packed 10:10:10:2 RGBA and unpack to 16-bit per channel
    std::vector<uint32_t> packed(m_width * m_height);
    glReadPixels(viewport[0], viewport[1], viewport[2], viewport[3],
                 GL_RGBA, GL_UNSIGNED_INT_2_10_10_10_REV,
                 static_cast<GLvoid*>(packed.data()));

    if (glGetError() != GL_NO_ERROR)
    {
      CLog::Log(LOGERROR, "CScreenshotSurfaceGLES: glReadPixels 10-bit failed, falling back to 8-bit");
      readbackMode = ReadbackMode::Uint8;
      m_bitDepth = 8;
    }
    else
    {
      m_stride = m_width * 8; // 4 channels x 2 bytes
      m_buffer = new unsigned char[m_stride * m_height];
      for (int y = 0; y < m_height; y++)
      {
        uint16_t* dst = reinterpret_cast<uint16_t*>(m_buffer + y * m_stride);
        uint32_t* src = packed.data() + (m_height - 1 - y) * m_width;
        for (int x = 0; x < m_width; x++)
        {
          uint32_t p = src[x];
          dst[x * 4 + 0] = static_cast<uint16_t>(((p >> 0) & 0x3FF) << 6);  // R
          dst[x * 4 + 1] = static_cast<uint16_t>(((p >> 10) & 0x3FF) << 6); // G
          dst[x * 4 + 2] = static_cast<uint16_t>(((p >> 20) & 0x3FF) << 6); // B
          dst[x * 4 + 3] = 0xFFFF;                                           // A
        }
      }
    }
  }
  else if (readbackMode == ReadbackMode::Uint16)
  {
    // 12-bit or 16-bit surface: read 16-bit per channel directly
    m_stride = m_width * 8; // 4 channels x 2 bytes
    std::vector<uint16_t> raw(m_width * m_height * 4);
    glReadPixels(viewport[0], viewport[1], viewport[2], viewport[3],
                 GL_RGBA, GL_UNSIGNED_SHORT,
                 static_cast<GLvoid*>(raw.data()));

    if (glGetError() != GL_NO_ERROR)
    {
      CLog::Log(LOGERROR, "CScreenshotSurfaceGLES: glReadPixels 16-bit failed, falling back to 8-bit");
      readbackMode = ReadbackMode::Uint8;
      m_bitDepth = 8;
    }
    else
    {
      m_buffer = new unsigned char[m_stride * m_height];
      for (int y = 0; y < m_height; y++)
      {
        uint16_t* dst = reinterpret_cast<uint16_t*>(m_buffer + y * m_stride);
        uint16_t* src = raw.data() + (m_height - 1 - y) * m_width * 4;
        memcpy(dst, src, m_stride); // already RGBA16, just Y-flip
      }
    }
  }

  if (readbackMode == ReadbackMode::Uint8)
  {
    // 8-bit readback (default, or fallback from failed high-bit readback)
    m_stride = m_width * 4;
    std::vector<uint8_t> surface(m_stride * m_height);

    glReadPixels(viewport[0], viewport[1], viewport[2], viewport[3],
                 GL_RGBA, GL_UNSIGNED_BYTE, static_cast<GLvoid*>(surface.data()));

    m_buffer = new unsigned char[m_stride * m_height];
    for (int y = 0; y < m_height; y++)
    {
      // we need to save in BGRA order so XOR Swap RGBA -> BGRA
      unsigned char* swap_pixels = surface.data() + (m_height - y - 1) * m_stride;
      for (int x = 0; x < m_width; x++, swap_pixels += 4)
      {
        std::swap(swap_pixels[0], swap_pixels[2]);
      }

      memcpy(m_buffer + y * m_stride, surface.data() + (m_height - y - 1) * m_stride, m_stride);
    }
  }

  return m_buffer != nullptr;
}
