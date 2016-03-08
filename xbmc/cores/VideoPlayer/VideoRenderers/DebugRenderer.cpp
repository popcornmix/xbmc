/*
 *      Copyright (C) 2005-2016 Team XBMC
 *      http://xbmc.org
 *
 *  This Program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2, or (at your option)
 *  any later version.
 *
 *  This Program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with XBMC; see the file COPYING.  If not, see
 *  <http://www.gnu.org/licenses/>.
 *
 */


#include "DebugRenderer.h"
#include "OverlayRendererGUI.h"
#include "cores/VideoPlayer/DVDCodecs/Overlay/DVDOverlayText.h"
#include "guilib/GraphicContext.h"

using namespace OVERLAY;

CDebugRenderer::CDebugRenderer()
{
  for (int i = 0; i<DEBUG_OVERLAY_COUNT_MAX; i++)
  {
    m_overlay[i] = nullptr;
    m_strDebug[i] = " ";
  }
}

CDebugRenderer::~CDebugRenderer()
{
  for (int i = 0; i<DEBUG_OVERLAY_COUNT_MAX; i++)
  {
    if (m_overlay[i])
      m_overlay[i]->Release();
  }
}

void CDebugRenderer::SetInfo(std::vector<std::string> &infos)
{
  m_overlayRenderer.Release(0);

  for (size_t i = 0; i < std::min(infos.size(), (size_t)DEBUG_OVERLAY_COUNT_MAX); i++)
  {
    if (infos[i] != m_strDebug[i])
    {
      if (infos[i].empty())
        continue;
      m_strDebug[i] = infos[i];
      if (m_overlay[i])
        m_overlay[i]->Release();
      m_overlay[i] = new CDVDOverlayText();
      m_overlay[i]->AddElement(new CDVDOverlayText::CElementText(m_strDebug[i]));
    }

  }
  for (size_t i = 0; i < DEBUG_OVERLAY_COUNT_MAX; i++)
  {
    if (m_overlay[i])
      m_overlayRenderer.AddOverlay(m_overlay[i], 0, 0);
  }
}

void CDebugRenderer::Render(CRect &src, CRect &dst, CRect &view)
{
  m_overlayRenderer.SetVideoRect(src, dst, view);
  m_overlayRenderer.Render(0);
}

void CDebugRenderer::Flush()
{
  m_overlayRenderer.Flush();
}

CDebugRenderer::CRenderer::CRenderer() : OVERLAY::CRenderer()
{
  m_font = "__debug__";
  m_fontBorder = "__debugborder__";
}

void CDebugRenderer::CRenderer::Render(int idx)
{
  std::vector<COverlay*> render;
  std::vector<SElement>& list = m_buffers[idx];
  int posY = 0;
  for (std::vector<SElement>::iterator it = list.begin(); it != list.end(); ++it)
  {
    COverlay* o = nullptr;

    if (it->overlay_dvd)
      o = Convert(it->overlay_dvd, it->pts);

    if (!o)
      continue;

    COverlayText *text = dynamic_cast<COverlayText*>(o);
    if (text)
      text->PrepareRender("arial.ttf", 1, 12, 0, m_font, m_fontBorder);

    RESOLUTION_INFO res = g_graphicsContext.GetResInfo(g_graphicsContext.GetVideoResolution());

    o->m_pos = COverlay::POSITION_ABSOLUTE;
    o->m_align = COverlay::ALIGN_SCREEN;
    o->m_x = 10 + (o->m_width * m_rv.Width() / res.iWidth) / 2;
    o->m_y = posY + o->m_height;
    OVERLAY::CRenderer::Render(o, 0);

    posY = o->m_y;
  }

  ReleaseUnused();
}
