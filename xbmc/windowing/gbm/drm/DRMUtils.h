/*
 *  Copyright (C) 2005-2018 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#pragma once

#include "DRMConnector.h"
#include "DRMCrtc.h"
#include "DRMEncoder.h"
#include "DRMPlane.h"
#include "windowing/Resolution.h"
#include "windowing/gbm/GBMUtils.h"

#include <cstdint>
#include <deque>
#include <utility>
#include <vector>

#include <gbm.h>
#include <xf86drm.h>

namespace KODI
{
namespace WINDOWING
{
namespace GBM
{
// https://github.com/torvalds/linux/blob/7d0a66e4bb9081d75c82ec4957c50034cb0ea449/drivers/
// gpu/drm/amd/display/amdgpu_dm/amdgpu_dm_crtc.c#L683-L692
//
// amdgpu drm driver needs at least 1 primary plane to active when doing a modeset
inline constexpr int QUIRK_NEEDSPRIMARY = 1 << 0;

struct drm_fb
{
  struct gbm_bo* bo = nullptr;
  uint32_t fb_id;
  uint32_t format;
};

struct outputformat
{
  uint32_t drm;
  uint8_t bpp;
  uint8_t alpha;
  bool active;
  std::vector<uint64_t> modifiers;
};

class CDRMUtils
{
public:
  CDRMUtils() = default;
  virtual ~CDRMUtils();
  virtual void FlipPage(struct gbm_bo* bo, bool rendered, bool videoLayer, bool async) {}
  virtual bool SetVideoMode(const RESOLUTION_INFO& res, struct gbm_bo* bo) { return false; }
  virtual bool SetActive(bool active) { return false; }
  virtual bool InitDrm();
  virtual void DestroyDrm();
  virtual bool SupportsFencing() { return false; }

  int GetFileDescriptor() const { return m_fd; }
  int GetRenderNodeFileDescriptor() const { return m_renderFd; }
  const char* GetRenderDevicePath() const { return m_renderDevicePath; }
  CDRMPlane* GetVideoPlane() const { return m_video_plane; }
  CDRMPlane* GetVideoPlane2() const { return m_video_plane2; }
  CDRMPlane* GetGuiPlane() const { return m_gui_plane; }
  CDRMCrtc* GetCrtc() const { return m_crtc; }
  CDRMConnector* GetConnector() const { return m_connector; }
  std::vector<outputformat>& GetOutputFormats() { return m_output_formats; }
  bool FindGuiPlane(uint32_t format, uint64_t modifier);
  bool FindVideoPlane(uint32_t format, uint64_t modifier);
  bool FindVideoAndGuiPlane(uint32_t format, uint64_t modifier, uint64_t width, uint64_t height);
  // FindGuiPlane never releases a D2P video plane reservation; a caller that
  // means "video is over" states it here before revalidating the gui plane.
  void ReleaseVideoPlane() { m_video_plane = nullptr; }

  /*!
   * \brief Claim a second video plane below the gui plane, so stereoscopic
   *        playback can scan out one eye per plane. Optional: on failure the
   *        caller keeps using the single video plane.
   */
  bool FindSecondVideoPlane(uint32_t format, uint64_t modifier, uint64_t width, uint64_t height);

  bool HasQuirk(int quirk) const { return m_drm_quirks & quirk; }

  std::vector<std::string> GetConnectedConnectorNames();

  virtual RESOLUTION_INFO GetCurrentMode();
  virtual std::vector<RESOLUTION_INFO> GetModes();
  virtual bool SetMode(const RESOLUTION_INFO& res);

  void SetInFenceFd(int fd) { m_inFenceFd = fd; }
  int TakeOutFenceFd()
  {
    int fd{-1};
    return std::exchange(m_outFenceFd, fd);
  }

  //! \brief Whether the flip of a given atomic commit has happened yet.
  enum class CommitState
  {
    COMPLETE, //!< the flip landed, so whatever it replaced is off screen
    PENDING, //!< not yet issued, or issued with its flip still unsignalled
    UNKNOWN, //!< that commit carried no out-fence, so it cannot be told
  };

  /*!
   * \brief Sequence the next commit will carry - the commit that will present
   *        the properties being added to the request now.
   */
  uint64_t GetNextCommitSequence() const { return m_commitSeq + 1; }

  /*!
   * \brief State of the commit with the given sequence.
   *
   * Lets a caller that has handed a buffer to a plane tell whether the commit
   * meant to replace it on screen has actually landed. Non-blocking, and for
   * the presenting thread: the sequence is not synchronised.
   */
  CommitState GetCommitState(uint64_t sequence);

protected:
  bool OpenDrm(bool needConnector);
  drm_fb* DrmFbGetFromBo(struct gbm_bo* bo);
  //! \brief Record an issued commit and, when it carried one, its out-fence.
  void NoteCommit(int outFenceFd);

  int m_fd{-1};
  CDRMConnector* m_connector{nullptr};
  CDRMEncoder* m_encoder{nullptr};
  CDRMCrtc* m_crtc{nullptr};
  CDRMCrtc* m_old_crtc{nullptr};
  CDRMCrtc* m_orig_crtc{nullptr};
  CDRMPlane* m_video_plane{nullptr};
  //! Second video plane, only held while scanning out stereoscopic content.
  CDRMPlane* m_video_plane2{nullptr};
  CDRMPlane* m_gui_plane{nullptr};
  drmModeModeInfo* m_mode = nullptr;

  int m_width = 0;
  int m_height = 0;

  int m_inFenceFd{-1};
  int m_outFenceFd{-1};

  uint64_t m_commitSeq{0};
  //! Sequence and a dup of the out-fence of recent commits, oldest first.
  std::deque<std::pair<uint64_t, int>> m_commitFences;

  std::vector<std::unique_ptr<CDRMPlane>> m_planes;

private:
  bool FindConnector();
  bool FindEncoder();
  bool FindCrtc();
  bool FindPreferredMode();
  bool RestoreOriginalMode();
  RESOLUTION_INFO GetResolutionInfo(drmModeModeInfoPtr mode);
  void PrintDrmDeviceInfo(drmDevicePtr device);

  int m_renderFd{-1};
  const char* m_renderDevicePath{nullptr};

  int m_drm_quirks{0};
  std::vector<std::unique_ptr<CDRMConnector>> m_connectors;
  std::vector<std::unique_ptr<CDRMEncoder>> m_encoders;
  std::vector<std::unique_ptr<CDRMCrtc>> m_crtcs;
  // 8-bit formats first: Direct-to-Plane needs full 8-bit alpha for GUI overlay.
  // 10-bit formats are available for single-plane HDR rendering when requested.
  std::vector<outputformat> m_output_formats = {{DRM_FORMAT_ARGB8888, 8, 8, false, {}},
                                                {DRM_FORMAT_XRGB8888, 8, 0, false, {}},
                                                {DRM_FORMAT_ARGB2101010, 10, 2, false, {}},
                                                {DRM_FORMAT_XRGB2101010, 10, 0, false, {}}};
};

} // namespace GBM
} // namespace WINDOWING
} // namespace KODI
