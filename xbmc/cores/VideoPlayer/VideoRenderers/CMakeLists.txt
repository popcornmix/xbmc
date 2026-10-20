set(SOURCES BaseRenderer.cpp
            ColorManager.cpp
            OverlayRenderer.cpp
            OverlayRendererUtil.cpp
            RenderFactory.cpp
            RenderFlags.cpp
            RenderManager.cpp
            DebugRenderer.cpp)

set(HEADERS BaseRenderer.h
            ColorManager.h
            DebugInfo.h
            OverlayRenderer.h
            OverlayRendererUtil.h
            RenderFactory.h
            RenderFlags.h
            RenderInfo.h
            RenderManager.h
            DebugRenderer.h)

if(CORE_SYSTEM_NAME STREQUAL windows OR CORE_SYSTEM_NAME STREQUAL windowsstore)
  list(APPEND SOURCES WinRenderer.cpp
                      OverlayRendererDX.cpp)
  list(APPEND HEADERS WinRenderer.h
                      OverlayRendererDX.h)
endif()

if(TARGET ${APP_NAME_LC}::OpenGl OR TARGET ${APP_NAME_LC}::OpenGLES)
  list(APPEND SOURCES FrameBufferObject.cpp)
  list(APPEND HEADERS FrameBufferObject.h)
endif()

if(TARGET ${APP_NAME_LC}::OpenGl)
  list(APPEND SOURCES LinuxRendererGL.cpp
                      OverlayRendererGL.cpp)
  list(APPEND HEADERS LinuxRendererGL.h
                      OverlayRendererGL.h)
endif()

if(TARGET ${APP_NAME_LC}::OpenGLES AND ("android" IN_LIST CORE_PLATFORM_NAME_LC OR
                            "ios" IN_LIST CORE_PLATFORM_NAME_LC OR
                            "tvos" IN_LIST CORE_PLATFORM_NAME_LC OR
                            "gbm" IN_LIST CORE_PLATFORM_NAME_LC OR
                            "x11" IN_LIST CORE_PLATFORM_NAME_LC OR
                            "wayland" IN_LIST CORE_PLATFORM_NAME_LC))
  list(APPEND SOURCES LinuxRendererGLES.cpp
                      OverlayRendererGLES.cpp)
  list(APPEND HEADERS LinuxRendererGLES.h
                      OverlayRendererGLES.cpp)
endif()

option(ENABLE_DVBRIDGE "Build experimental native DV bridge (requires patched dependencies)" OFF)
if(ENABLE_DVBRIDGE)
  if(NOT TARGET ${APP_NAME_LC}::OpenGLES OR NOT "gbm" IN_LIST CORE_PLATFORM_NAME_LC OR
     NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
    message(FATAL_ERROR "DVBridge requires the GBM/OpenGL ES platform")
  endif()
  add_subdirectory("${DVBRIDGE_SOURCE_DIR}" "${CMAKE_CURRENT_BINARY_DIR}/dvbridge-core")
  file(READ "${DVBRIDGE_SOURCE_DIR}/pack_gles300.vert" DVBRIDGE_VERTEX_SOURCE)
  file(READ "${DVBRIDGE_SOURCE_DIR}/pack_gles300.frag" DVBRIDGE_FRAGMENT_SOURCE)
  configure_file(DVBridgeShaders.h.in DVBridgeShaders.h @ONLY)
  list(APPEND SOURCES DVBridgeGLES.cpp)
  list(APPEND HEADERS DVBridgeGLES.h)
  if(CB1_ENABLE_HDR10_AI)
    list(APPEND SOURCES HDR10AISource.cpp)
    list(APPEND HEADERS HDR10AISource.h)
  endif()
endif()

core_add_library(videorenderers)
if(ENABLE_DVBRIDGE)
  target_link_libraries(${CORE_LIBRARY} PRIVATE dvbridge_core)
  target_include_directories(${CORE_LIBRARY} PRIVATE "${CMAKE_CURRENT_BINARY_DIR}")
  target_compile_definitions(${CORE_LIBRARY} PRIVATE HAVE_DVBRIDGE=1)
endif()
