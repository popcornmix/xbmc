/*
 *  Copyright (C) 2005-2018 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#include "GBMUtils.h"

#include "utils/log.h"

#include <mutex>

using namespace KODI::WINDOWING::GBM;

namespace
{
std::once_flag flag;
}

bool CGBMUtils::CreateDevice(int fd)
{
  auto device = gbm_create_device(fd);
  if (!device)
  {
    CLog::Log(LOGERROR, "CGBMUtils::{} - failed to create device: {}", __FUNCTION__,
              strerror(errno));
    return false;
  }

  m_device.reset(new CGBMDevice(device));

  return true;
}

CGBMUtils::CGBMDevice::CGBMDevice(gbm_device* device) : m_device(device)
{
}

bool CGBMUtils::CGBMDevice::CreateSurface(
    int width, int height, uint32_t format, const uint64_t* modifiers, const int modifiers_count,
    bool staged)
{
  if (m_priorSurface)
    return false;
  gbm_surface* surface{nullptr};
#if defined(HAS_GBM_MODIFIERS)
  if (modifiers)
  {
    surface = gbm_surface_create_with_modifiers(m_device, width, height, format, modifiers,
                                                modifiers_count);
  }
#endif
  if (!surface)
  {
    surface = gbm_surface_create(m_device, width, height, format,
                                 GBM_BO_USE_SCANOUT | GBM_BO_USE_RENDERING);
  }

  if (!surface)
  {
    CLog::Log(LOGERROR, "CGBMUtils::{} - failed to create surface: {}", __FUNCTION__,
              strerror(errno));
    return false;
  }

  CLog::Log(LOGDEBUG, "CGBMUtils::{} - created surface with size {}x{}", __FUNCTION__, width,
            height);

  if (staged)
  {
    m_surface.get_deleter().releaseBuffers = true;
    m_priorSurface = std::move(m_surface);
    m_surface = decltype(m_surface)(new CGBMSurface(surface), CGBMSurfaceDeleter{true});
  }
  else
    m_surface.reset(new CGBMSurface(surface));

  return true;
}

void CGBMUtils::CGBMDevice::CompleteOutputSurface(bool presented)
{
  if (!m_priorSurface)
    return;
  if (!presented)
    m_surface.swap(m_priorSurface);
  m_priorSurface.reset();
}

CGBMUtils::CGBMDevice::CGBMSurface::CGBMSurface(gbm_surface* surface) : m_surface(surface)
{
}

CGBMUtils::CGBMDevice::CGBMSurface::CGBMSurfaceBuffer& CGBMUtils::CGBMDevice::CGBMSurface::
    LockFrontBuffer()
{
  m_buffers.emplace(std::make_unique<CGBMSurfaceBuffer>(m_surface));

  if (!static_cast<bool>(gbm_surface_has_free_buffers(m_surface)))
  {
    /*
     * We want to use call_once here because we want it to be logged the first time that
     * we have to release buffers. This means that the maximum amount of buffers had been reached.
     * For mesa this should be 4 buffers but it may vary across other implementations.
     */
    std::call_once(flag, [this]()
                   { CLog::Log(LOGDEBUG, "CGBMUtils - using {} buffers", m_buffers.size()); });

    m_buffers.pop();
  }

  return *m_buffers.back();
}

CGBMUtils::CGBMDevice::CGBMSurface::CGBMSurfaceBuffer::CGBMSurfaceBuffer(gbm_surface* surface)
  : m_surface(surface), m_buffer(gbm_surface_lock_front_buffer(surface))
{
}

gbm_bo* CGBMUtils::CGBMDevice::CGBMSurface::LockTransactionalBuffer()
{
  if (m_pendingBuffer)
    return nullptr;
  auto buffer = std::make_unique<CGBMSurfaceBuffer>(m_surface);
  if (!buffer->Get())
    return nullptr;
  m_pendingBuffer = std::move(buffer);
  return m_pendingBuffer->Get();
}

void CGBMUtils::CGBMDevice::CGBMSurface::CompleteTransactionalBuffer(bool presented)
{
  if (!m_pendingBuffer)
    return;
  if (presented)
  {
    // The blocking commit has replaced every legacy queued scanout buffer.
    while (!m_buffers.empty())
      m_buffers.pop();
    m_scanoutBuffer = std::move(m_pendingBuffer);
  }
  else
    m_pendingBuffer.reset();
}

void CGBMUtils::CGBMDevice::CGBMSurface::EndTransactionalMode()
{
  // Transfer the restored scanout to the ordinary queue without unlocking it.
  if (!m_pendingBuffer && m_scanoutBuffer)
    m_buffers.push(std::move(m_scanoutBuffer));
}

CGBMUtils::CGBMDevice::CGBMSurface::CGBMSurfaceBuffer::~CGBMSurfaceBuffer()
{
  if (m_surface && m_buffer)
    gbm_surface_release_buffer(m_surface, m_buffer);
}
