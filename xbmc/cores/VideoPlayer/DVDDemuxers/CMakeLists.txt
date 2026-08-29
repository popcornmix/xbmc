set(SOURCES BlurayOffsetMetadata.cpp
            DemuxMultiSource.cpp
            DVDDemux.cpp
            DVDDemuxCC.cpp
            DVDDemuxCC/CCBitstreamParserFactory.cpp
            DVDDemuxCC/H264AnnexBBitstreamParser.cpp
            DVDDemuxCC/H264AVCCBitstreamParser.cpp
            DVDDemuxCC/H264CCBitstreamParser.cpp
            DVDDemuxCC/MPEG2CCBitstreamParser.cpp
            DVDDemuxCDDA.cpp
            DVDDemuxClient.cpp
            DVDDemuxFFmpeg.cpp
            DVDDemuxUtils.cpp
            DVDDemuxVobsub.cpp
            DVDFactoryDemuxer.cpp)

if(TARGET ${APP_NAME_LC}::Bluray)
  list(APPEND SOURCES DVDDemuxBluray3D.cpp)
endif()

set(HEADERS BlurayOffsetMetadata.h
            DemuxMultiSource.h
            DVDDemux.h
            DVDDemuxCC.h
            DVDDemuxCC/Bitstream.h
            DVDDemuxCC/CaptionBlock.h
            DVDDemuxCC/CCBitstreamParserFactory.h
            DVDDemuxCC/H264AnnexBBitstreamParser.h
            DVDDemuxCC/H264AVCCBitstreamParser.h
            DVDDemuxCC/H264CCBitstreamParser.h
            DVDDemuxCC/ICCBitstreamParser.h
            DVDDemuxCC/MPEG2CCBitstreamParser.h
            DVDDemuxCDDA.h
            DVDDemuxClient.h
            DVDDemuxFFmpeg.h
            DVDDemuxUtils.h
            DVDDemuxVobsub.h
            DVDFactoryDemuxer.h)

if(TARGET ${APP_NAME_LC}::Bluray)
  list(APPEND HEADERS DVDDemuxBluray3D.h)
endif()

core_add_library(dvddemuxers)
