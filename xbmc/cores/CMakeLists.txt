set(SOURCES DataCacheCore.cpp
            FFmpeg.cpp
            VideoSettings.cpp
            VideoSourceSideData.cpp)

set(HEADERS DataCacheCore.h
            EdlEdit.h
            FFmpeg.h
            GameSettings.h
            IPlayer.h
            IPlayerCallback.h
            MenuType.h
            VideoSettings.h
            VideoSourceSideData.h)

core_add_library(cores)
