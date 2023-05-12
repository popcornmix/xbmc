#!/bin/bash
set -e
ARCH=$(dpkg --print-architecture)

export ADDONS_BUILD_NUMBER=1
export DEBIAN_PACKAGE_VERSION=1
export DEBIAN_PACKAGE_EPOCH=3
export CORE_PLATFORM_NAME="gbm x11 wayland"
export CORE_PLATFORM_DIR=build_$(echo $(printf "%s\n" ${CORE_PLATFORM_NAME} | sort) | tr ' ' '_')_${ARCH}
#export ENABLE_INTERNAL_FFMPEG=ON

if [ "$1" == "compile" ]; then
 REPO_DIR=${REPO_DIR:-$(pwd)}
 KODI_BUILD_DIR=${KODI_BUILD_DIR:-"${REPO_DIR}/${CORE_PLATFORM_DIR}"}
 cd $KODI_BUILD_DIR &> /dev/null
 cmake --build . -- VERBOSE=1 -j${BUILD_THREADS} |& tee -a build.log
 exit
fi

if [ "$ADDONS_TO_BUILD" == "" ]; then
 ./build_rpi_debian_packages.sh
 cd ${CORE_PLATFORM_DIR}/packages/ && sudo dpkg -i kodi-bin_20*.deb kodi_20.*.deb kodi-addon-dev*.deb kodi-tools-texturepacker*.deb && cd -
fi
#ADDONS_TO_BUILD="inputstream.adaptive pvr.hts screensaver.shadertoy visualization.shadertoy" \
ADDONS_TO_BUILD=${ADDONS_TO_BUILD:-"all"} \
./build_rpi_debian_packages.sh -a
#sudo dpkg -i ${BUILD}/build/addons_build/*.deb
