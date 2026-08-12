/*
 *  Copyright (C) 2005-2026 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#pragma once

#include "BlurayStateSerializer.h"
#include "DVDInputStream.h"
#include "filesystem/bluray/PlaylistStructure.h"
#include "threads/CriticalSection.h"
#if defined(HAS_UDFREAD)
#include "filesystem/UDFContext.h"
#endif

#include <chrono>
#include <list>
#include <map>
#include <memory>
#include <optional>
#include <string>

extern "C"
{
#include <libbluray/bluray.h>
#include <libbluray/bluray-version.h>
#include <libbluray/keys.h>
#include <libbluray/overlay.h>
#include <libbluray/player_settings.h>
#include "DVDInputStreamFile.h"
}

#define MAX_PLAYLIST_ID 99999
#define MAX_CLIP_ID 99999
#define BD_EVENT_MENU_OVERLAY -1
#define BD_EVENT_MENU_ERROR   -2
#define BD_EVENT_ENC_ERROR    -3

#define HDMV_PID_VIDEO            0x1011
#define HDMV_PID_VIDEO_SS         0x1012
#define HDMV_PID_AUDIO_FIRST      0x1100
#define HDMV_PID_AUDIO_LAST       0x111f
#define HDMV_PID_PG_FIRST         0x1200
#define HDMV_PID_PG_LAST          0x121f
#define HDMV_PID_PG_HDR_FIRST     0x12a0
#define HDMV_PID_PG_HDR_LAST      0x12bf
#define HDMV_PID_IG_FIRST         0x1400
#define HDMV_PID_IG_LAST          0x141f

class CDVDInputStreamBlurayFile;
class CDVDOverlayImage;
class IVideoPlayer;

class CDVDInputStreamBluray
  : public CDVDInputStream
  , public CDVDInputStream::IDisplayTime
  , public CDVDInputStream::IChapter
  , public CDVDInputStream::IPosTime
  , public CDVDInputStream::IMenus
{
public:
  CDVDInputStreamBluray() = delete;
  CDVDInputStreamBluray(IVideoPlayer* player, const CFileItem& fileitem);
  ~CDVDInputStreamBluray() override;
  bool Open() override;
  void Close() override;
  int Read(uint8_t* buf, int buf_size) override;
  int ReadBlocks(uint8_t* buf, int lba, int num_blocks);
  int64_t Seek(int64_t offset, int whence) override;
  void Abort() override;
  bool IsEOF() override;
  int64_t GetLength() override;
  int GetBlockSize() override { return 6144; }
  ENextStream NextStream() override;


  /* IMenus */
  void ActivateButton() override { UserInput(BD_VK_ENTER); }
  void SelectButton(int iButton) override
  {
    if(iButton < 10)
      UserInput((bd_vk_key_e)(BD_VK_0 + iButton));
  }
  int  GetCurrentButton() override { return 0; }
  int  GetTotalButtons() override { return 0; }
  void OnUp() override  { UserInput(BD_VK_UP); }
  void OnDown() override  { UserInput(BD_VK_DOWN); }
  void OnLeft() override { UserInput(BD_VK_LEFT); }
  void OnRight() override { UserInput(BD_VK_RIGHT); }

  /*! \brief Open the Menu
  * \return true if the menu is successfully opened, false otherwise
  */
  bool OnMenu() override;
  void OnBack() override
  {
    if(IsInMenu())
      OnMenu();
  }
  void OnNext() override {}
  void OnPrevious() override {}

  /*!
   * \brief Get the supported menu type
   * \return The supported menu type
  */
  MenuType GetSupportedMenuType() override;

  bool IsInMenu() override;
  bool OnMouseMove(const CPoint &point) override  { return MouseMove(point); }
  bool OnMouseClick(const CPoint &point) override { return MouseClick(point); }
  void SkipStill() override;
  bool GetState(std::string& xmlstate) override;
  bool SetState(const std::string& xmlstate) override;
  bool CanSeek() override;


  void UserInput(bd_vk_key_e vk);
  bool MouseMove(const CPoint &point);
  bool MouseClick(const CPoint &point);

  int GetChapter() override;
  int GetChapterCount() override;
  void GetChapterName(std::string& name, int ch = -1) override;
  std::chrono::milliseconds GetChapterPos(int ch) override;
  bool SeekChapter(int ch) override;

  CDVDInputStream::IDisplayTime* GetIDisplayTime() override { return this; }
  int GetTotalTime() override;
  int GetTime() override;

  CDVDInputStream::IPosTime* GetIPosTime() override { return this; }
  bool PosTime(int ms) override;

  void GetStreamInfo(int pid, std::string &language);

  /*!
   * \brief Check whether a stream is the default of the playlist being played, ie. audio stream
   *        number 1 or presentation graphic stream number 1 of the current clip.
   * \param pid The packet identifier of the stream
   * \return True if the stream is the default audio or subtitle stream, false otherwise
   */
  bool IsDefaultStream(int pid) const;

  /*!
   * \brief Whether the playing title carries a stereoscopic (MVC) sub-path.
   *
   * The 3D extension sub-path is declared in the playlist extension data, which libbluray
   * parses but does not expose, so this comes from Kodi's own MPLS parser.
   *
   * \return true if the title has an MVC dependent view
   */
  bool IsStereoscopic() const;

  /*!
   * \brief Whether the disc carries 3D content at all, whatever is playing now.
   *
   * A playlist is only known once libbluray has run the disc far enough to choose one,
   * which in navigation mode is after the demuxer exists to read for it. The disc says
   * this up front, so it is what decides whether a second eye may need demuxing.
   *
   * \return true if the disc's application info declares 3D content
   */
  bool IsStereoscopicDisc() const;

  /*!
   * \brief Get the clip holding the MVC dependent view for the current play item.
   * \param clip filled with the dependent view clip number
   * \param codec filled with the clip codec id, which gives the file extension
   * \return true if a dependent view clip was found
   */
  bool GetStereoscopicClip(unsigned int& clip, std::string& codec) const;

  /*!
   * \brief Whether the base view is the right eye rather than the left.
   * \return true if the base view is the right eye
   */
  bool IsBaseViewRightEye() const;

  /*!
   * \brief Open a clip that libbluray is not itself playing.
   *
   * Goes through bd_open_file_dec() so AACS and BD+ are handled, and works for a disc
   * image, a BDMV folder and a physical disc alike.
   *
   * \param clip clip number
   * \param codec clip codec id, which gives the file extension
   * \return the opened stream, or nullptr on failure
   */
  std::shared_ptr<CDVDInputStreamBlurayFile> OpenClipStream(unsigned int clip,
                                                            const std::string& codec);

  void OverlayCallback(const BD_OVERLAY * const);
#ifdef HAVE_LIBBLURAY_BDJ
  void OverlayCallbackARGB(const struct bd_argb_overlay_s * const);
#endif

  BLURAY_TITLE_INFO* GetTitleFromState(const std::string& xmlstate);
  BLURAY_TITLE_INFO* GetTitleLongest();
  BLURAY_TITLE_INFO* GetTitleFile(const std::string& name);

  void ProcessEvent();

  void SaveCurrentState(const CStreamDetails& details) override;
  UpdateState UpdateItemFromSavedStates(CFileItem& item, double time, bool& closed) override;
  void UpdateStack(CFileItem& item) override;

protected:
  struct SPlane;

  void OverlayFlush(int64_t pts);
  void OverlayClose();
  static void OverlayClear(SPlane& plane, int x, int y, int w, int h);
  static void OverlayInit (SPlane& plane, int w, int h);

  IVideoPlayer* m_player = nullptr;
  BLURAY* m_bd = nullptr;
  const BLURAY_TITLE* m_title = nullptr;
  BLURAY_TITLE_INFO* m_titleInfo = nullptr;
  uint32_t m_playlist = MAX_PLAYLIST_ID + 1;
  BLURAY_CLIP_INFO* m_clip = nullptr;

  //! The clip information of the play item being played, ie. every stream its m2ts carries (see
  //! GetStreamInfo). Owned, and only valid while m_clip refers to the same play item.
  struct clpi_cl* m_clipInfo = nullptr;
  uint32_t m_angle = 0;
  bool m_menu = false;
  bool m_isInMainMenu = false;
  bool m_hasOverlay = false;
  bool m_navmode = false;
  int m_dispTimeBeforeRead = 0;

  typedef std::shared_ptr<CDVDOverlayImage> SOverlay;
  typedef std::list<SOverlay> SOverlays;

  struct SPlane
  {
    SOverlays o;
    int w = 0;
    int h = 0;
  };

  SPlane m_planes[2];
  enum EHoldState {
    HOLD_NONE = 0,
    HOLD_HELD,
    HOLD_DATA,
    HOLD_STILL,
    HOLD_ERROR,
    HOLD_EXIT
  } m_hold = HOLD_NONE;
  BD_EVENT m_event;
#ifdef HAVE_LIBBLURAY_BDJ
  struct bd_argb_buffer_s m_argb;
#endif

  private:
    bool OpenStream(CFileItem &item);
    void SetupPlayerSettings();
    void FreeTitleInfo();
    void FreeClipInfo();

    /*!
     * \brief Read the clip information of a play item of the playlist being played.
     * \param playItem The index of the play item, as BD_EVENT_PLAYITEM reports it
     */
    void UpdateClipInfo(unsigned int playItem);

    /*!
     * \brief Find the language of a stream in the stream number table of the current play item.
     * \param pid The packet identifier of the stream
     * \param language Filled in with the language of the stream, if it is found
     * \return True if the playlist presents the stream, false otherwise
     */
    bool GetPlaylistStreamLanguage(int pid, std::string& language) const;

    /*!
     * \brief Find the language of a stream in the clip information of the current play item.
     * \param pid The packet identifier of the stream
     * \param language Filled in with the language of the stream, if it is found
     * \return True if the clip carries the stream, false otherwise
     */
    bool GetClipStreamLanguage(int pid, std::string& language) const;

    //! \brief Re-read the playlist with Kodi's MPLS parser to pick up the 3D extension data.
    void UpdatePlaylistInformation();

    std::unique_ptr<CDVDInputStreamFile> m_pstream;
    std::string m_rootPath;

#if defined(HAS_UDFREAD)
    // Keeps a disc image's UDF volume mounted for as long as the disc is open
    std::optional<XFILE::CUDFMount> m_udfMount;
#endif

    /*! Disc root as a VFS path, e.g. udf://<image>/ for a disc image. Used to read the
        playlist directly, since libbluray does not expose the 3D extension data. */
    std::string m_vfsRoot;

    /*! Playlist as parsed by Kodi, which unlike BLURAY_TITLE_INFO includes the
        stereoscopic extension sub-paths. Only valid when m_playlistInfoValid. */
    XFILE::BlurayPlaylistInformation m_playlistInfo;
    std::map<unsigned int, XFILE::ClipInformation> m_clipCache;
    bool m_playlistInfoValid{false};

    /*! Whether the disc declares 3D content, from its application info. Known as soon as
        the disc is opened, unlike the playlist. */
    bool m_stereoscopicDisc{false};

    /*! Index of the current play item, tracked so the matching stereoscopic sub-play item
        can be found. */
    uint32_t m_playItem{0};

    /*! Bluray state serializer handler */
    CBlurayStateSerializer m_blurayStateSerializer;

    /* used during bd_open_stream read block*/
    CCriticalSection m_readBlocksLock;

    std::chrono::steady_clock::time_point m_startWatchTime{};
    std::vector<PlaylistInformation> m_playedPlaylists;
    CCriticalSection m_statesLock;
};
