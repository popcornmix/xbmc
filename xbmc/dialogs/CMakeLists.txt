set(SOURCES GUIDialogBoxBase.cpp
            GUIDialogBusy.cpp
            GUIDialogBusyNoCancel.cpp
            GUIDialogButtonMenu.cpp
            GUIDialogCache.cpp
            GUIDialogColorPicker.cpp
            GUIDialogContextMenu.cpp
            GUIDialogExtendedProgressBar.cpp
            GUIDialogFileBrowser.cpp
            GUIDialogGamepad.cpp
            GUIDialogKaiToast.cpp
            GUIDialogKeyboardGeneric.cpp
            GUIDialogKeyboardTouch.cpp
            GUIDialogMediaFilter.cpp
            GUIDialogMediaSource.cpp
            GUIDialogNumeric.cpp
            GUIDialogOK.cpp
            GUIDialogPlayerControls.cpp
            GUIDialogPlayerProcessInfo.cpp
            GUIDialogProgress.cpp
            GUIDialogSeekBar.cpp
            GUIDialogSelect.cpp
            GUIDialogSimpleMenu.cpp
            GUIDialogSlider.cpp
            GUIDialogSmartPlaylistEditor.cpp
            GUIDialogSmartPlaylistRule.cpp
            GUIDialogSubMenu.cpp
            GUIDialogTextViewer.cpp
            GUIDialogVolumeBar.cpp
            GUIDialogYesNo.cpp)

set(HEADERS GUIDialogBoxBase.h
            GUIDialogBusy.h
            GUIDialogBusyNoCancel.h
            GUIDialogButtonMenu.h
            GUIDialogCache.h
            GUIDialogColorPicker.h
            GUIDialogContextMenu.h
            GUIDialogExtendedProgressBar.h
            GUIDialogFileBrowser.h
            GUIDialogGamepad.h
            GUIDialogKaiToast.h
            GUIDialogKeyboardGeneric.h
            GUIDialogKeyboardTouch.h
            GUIDialogMediaFilter.h
            GUIDialogMediaSource.h
            GUIDialogNumeric.h
            GUIDialogOK.h
            GUIDialogPlayerControls.h
            GUIDialogPlayerProcessInfo.h
            GUIDialogProgress.h
            GUIDialogSeekBar.h
            GUIDialogSelect.h
            GUIDialogSimpleMenu.h
            GUIDialogSlider.h
            GUIDialogSmartPlaylistEditor.h
            GUIDialogSmartPlaylistRule.h
            GUIDialogSubMenu.h
            GUIDialogTextViewer.h
            GUIDialogVolumeBar.h
            GUIDialogYesNo.h
            IGUIVolumeBarCallback.h)

if(ENABLE_OPTICAL)
  list(APPEND SOURCES GUIDialogPlayEject.cpp)
  list(APPEND HEADERS GUIDialogPlayEject.h)
endif()

if(ENABLE_DVBRIDGE)
  list(APPEND SOURCES DVPlaybackModes.cpp)
  list(APPEND HEADERS DVPlaybackModes.h)
endif()
core_add_library(dialogs)
if(ENABLE_DVBRIDGE)
  target_link_libraries(${CORE_LIBRARY} PRIVATE dvbridge_core)
endif()
