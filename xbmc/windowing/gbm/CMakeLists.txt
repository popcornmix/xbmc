add_subdirectory(drm)

set(SOURCES OptionalsReg.cpp
            WinSystemGbm.cpp
            VideoSyncGbm.cpp
            GBMUtils.cpp
            WinSystemGbmEGLContext.cpp
            GBMDPMSSupport.cpp)

set(HEADERS OptionalsReg.h
            WinSystemGbm.h
            VideoSyncGbm.h
            GBMUtils.h
            WinSystemGbmEGLContext.h
            GBMDPMSSupport.h)

if(TARGET ${APP_NAME_LC}::OpenGl)
  list(APPEND SOURCES WinSystemGbmGLContext.cpp)
  list(APPEND HEADERS WinSystemGbmGLContext.h)
endif()
if(TARGET ${APP_NAME_LC}::OpenGLES)
  list(APPEND SOURCES WinSystemGbmGLESContext.cpp)
  list(APPEND HEADERS WinSystemGbmGLESContext.h)
endif()

core_add_library(windowing_gbm)
if(ENABLE_DVBRIDGE)
  target_link_libraries(${CORE_LIBRARY} PRIVATE dvbridge_core)
  target_compile_definitions(${CORE_LIBRARY} PRIVATE HAVE_DVBRIDGE=1)
endif()
