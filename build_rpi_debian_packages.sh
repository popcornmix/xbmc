#!/bin/bash

ARCH=$(dpkg --print-architecture)
REPO_DIR=${REPO_DIR:-$(pwd)}
KODI_BUILD_DIR=${KODI_BUILD_DIR:-"${REPO_DIR}/build_${ARCH}"}
ADDONS_TO_BUILD=${ADDONS_TO_BUILD:-""}
ADDONS_BUILD_DIR=${ADDONS_BUILD_DIR:-"${KODI_BUILD_DIR}/addons_build/"}
ADDONS_BUILD_NUMBER=${ADDONS_BUILD_NUMBER:-"1"}
#CPU=${CPU:-"cortex-a7"}
BUILD_TYPE=${BUILD_TYPE:-"Release"}
DEB_ARCH=${DEB_ARCH:-${ARCH}}
DEB_PACK_VERSION=${DEB_PACK_VERSION:-"1"}
DEBUILD_OPTS=${DEBUILD_OPTS:-""}
BUILD_THREADS=$(( $(nproc)*3/2 ))

function usage {
    echo "$0: This script builds a Kodi debian package from a git repository optimized for Raspberry Pi 4.
              [-a]       ... Build binary addons only
              [--armv6]  ... Build for Raspberry Pi 0/1
              [--armv7]  ... Build for Raspberry Pi 2/3
              [-j]       ... set concurrency level
	"
}

function checkEnv {
    echo "#------ build environment ------#"
    echo "REPO_DIR: $REPO_DIR"
    echo "KODI_BUILD_DIR: $KODI_BUILD_DIR"
    echo "CPU: $CPU"
    echo "DEB_ARCH: $DEB_ARCH"
    echo "BUILD_TYPE: $BUILD_TYPE"
    echo "KODI_OPTS: $KODI_OPTS"
    echo "EXTRA_FLAGS: $EXTRA_FLAGS"
    echo "BUILD_THREADS: $BUILD_THREADS"
    [[ -n $ADDONS_TO_BUILD ]] && echo "ADDONS_TO_BUILD: $ADDONS_TO_BUILD"
    [[ -n $ADDONS_TO_BUILD ]] && echo "ADDONS_BUILD_DIR: $ADDONS_BUILD_DIR"
    [[ -n $ADDONS_TO_BUILD ]] && echo "DEBUILD_OPTS: $DEBUILD_OPTS"
   
    KODIPLATFORM=$(dpkg -l | grep libkodiplatform | wc -l)

    if [[ -n $ADDONS_TO_BUILD && ! $KODIPLATFORM ]];
    then
         echo "ERROR: libkodiplatform is not installed. Please compile and install before building binary addons"
         exit
    fi

    echo "#-------------------------------#"
}

function setEnv {

    echo "#------ preparing environment ------#"

    if [[ $BUILD_TYPE == 'Release' ]];
    then
        DEBIAN_PACKAGE_TYPE="stable"
    else
        DEBIAN_PACKAGE_TYPE="unstable"
    fi

KODI_OPTS="\
-DVERBOSE=1 \
-DCORE_SYSTEM_NAME=linux \
-DCORE_PLATFORM_NAME=gbm \
-DAPP_RENDER_SYSTEM=gles \
-DENABLE_VAAPI=OFF \
-DENABLE_VDPAU=OFF \
-DENABLE_OPENGL=OFF \
-DENABLE_DEBUGFISSION=OFF \
-DENABLE_INTERNAL_SPDLOG=ON \
-DCMAKE_CXX_STANDARD_LIBRARIES="-latomic" \
#-DWITH_CPU=${CPU} \
-DENABLE_OPENGLES=ON \
-DCMAKE_BUILD_TYPE=${BUILD_TYPE} \
-DCMAKE_INSTALL_PREFIX=/usr \
-DENABLE_AIRTUNES=ON \
-DENABLE_ALSA=ON \
-DENABLE_AVAHI=ON \
-DENABLE_BLURAY=ON \
-DENABLE_CEC=ON \
-DENABLE_DBUS=ON \
-DENABLE_DVDCSS=ON \
-DENABLE_EGL=ON \
-DENABLE_EVENTCLIENTS=ON \
-DENABLE_INTERNAL_FMT=ON \
-DENABLE_MICROHTTPD=ON \
-DENABLE_MYSQLCLIENT=ON \
-DENABLE_NFS=ON \
-DENABLE_OPENSSL=ON \
-DENABLE_OPTICAL=ON \
-DENABLE_PULSEAUDIO=ON \
-DENABLE_SMBCLIENT=ON \
-DENABLE_SSH=ON \
-DENABLE_UDEV=ON \
-DENABLE_UPNP=ON \
-DENABLE_XSLT=ON \
-DENABLE_LIRC=ON \
-DENABLE_APP_AUTONAME=OFF \
-DENABLE_LCMS2=OFF \
-DENABLE_SNDIO=OFF \
-DENABLE_MDNS=OFF \
-DCPACK_GENERATOR=DEB \
-DDEBIAN_PACKAGE_VERSION=${DEB_PACK_VERSION}~ \
-DDEB_PACKAGE_ARCHITECTURE=${DEB_ARCH} \
-DDEBIAN_PACKAGE_TYPE=${DEBIAN_PACKAGE_TYPE}
"

#EXTRA_FLAGS="-Os -fomit-frame-pointer -march=armv8-a+crc+simd -mfpu=neon-fp-armv8 -mfloat-abi=hard -mvectorize-with-neon-quad"

    echo "#-------------------------------#"
}

function configure {
    echo "#---------- configure ----------#"
    [ -d $KODI_BUILD_DIR ] || mkdir -p $KODI_BUILD_DIR || exit 1
    cd $KODI_BUILD_DIR || exit 1
    rm -rf $KODI_BUILD_DIR/CMakeCache.txt $KODI_BUILD_DIR/CMakeFiles $KODI_BUILD_DIR/CPackConfig.cmake $KODI_BUILD_DIR/CTestTestfile.cmake $KODI_BUILD_DIR/cmake_install.cmake > /dev/null
    CXXFLAGS=${EXTRA_FLAGS} CFLAGS=${EXTRA_FLAGS} cmake ${KODI_OPTS} ${REPO_DIR}/ |& tee build.log
    # CMAKE Doesn't have a return code for errors yet..
    #if [ $? -ne 0 ]; then
    #   echo "ERROR: configure step failed.. Bailing out."
    #   exit
    #fi
    echo "#-------------------------------#"
}

function compile {
    echo "#----------- compile -----------#"
    cd $KODI_BUILD_DIR &> /dev/null
    cmake --build . -- VERBOSE=1 -j${BUILD_THREADS} |& tee -a build.log
    # CMAKE Doesn't have a return code for errors yet..
    #if [ $? -ne 0 ]; then
    #   echo "ERROR: compile step failed.. Bailing out."
    #   exit
    #fi
    echo "#-------------------------------#"
}

function package {
    echo "#----------- package -----------#"
    cd $KODI_BUILD_DIR &> /dev/null
    cpack |& tee -a build.log
    cpack -G TXZ --config CPackSourceConfig.cmake |& tee -a build.log
    # CMAKE Doesn't have a return code for errors yet..
    #if [ $? -ne 0 ]; then
    #   echo "ERROR: package step failed.. Bailing out."
    #   exit
    #fi
    echo "#-------------------------------#"
}

function compileAddons {
   [ -d $ADDONS_BUILD_DIR ] || mkdir -p $ADDONS_BUILD_DIR || exit 1
   cd $ADDONS_BUILD_DIR || exit 1 
   echo "#------ Building ADDONS (${ADDONS_TO_BUILD}) ------#"
   if [[ $DEBUILD_OPTS != *"-nc"* ]]
   then
        cd  $ADDONS_BUILD_DIR && rm -rf *
   fi
   echo "#------ Configuring addons   ------#"
   cmake -DOVERRIDE_PATHS=1 -DBUILD_DIR=$(pwd) -DCORE_SOURCE_DIR="${REPO_DIR}" -DADDONS_TO_BUILD="${ADDONS_TO_BUILD}" -DADDON_DEPENDS_PATH="${KODI_BUILD_DIR}/build" $REPO_DIR/cmake/addons/ |& tee -a build_addons.log
   if [ $? -ne 0 ]; then
      echo "ADDONS ERROR: configure step failed.. Bailing out."
      exit
   fi
   echo "#------ ADDONS Build dir ($(pwd)) ------#"
   for D in $(ls . --ignore="*prefix"); do
	if [ -d "${D}/debian" ]; then
		cd ${D}
		echo "Building : ${D} -- $(pwd)" 
		VERSION_FILE="addon.xml.in"
		[[ ! -f "${D}/addon.xml.in" ]] && VERSION_FILE="addon.xml"
		ADDONS_PACK_VER=$(grep -oP "  version=\"(.*)\"" ./${D}/${VERSION_FILE} | awk -F'\"' '{print $2}')
		sed -e "s/#PACKAGEVERSION#/${ADDONS_PACK_VER}/g" -e "s/#TAGREV#/${ADDONS_BUILD_NUMBER}/g" -e "s/#DIST#/$(lsb_release -cs)/g" debian/changelog.in > debian/changelog
		if [[ $D == "pvr"* || $D == "audioencoder"* || $D == "visualization.waveform" ]]; then
			for F in $(ls debian/*.install); do
				echo "usr/lib" > ${F}
				echo "usr/share" >> ${F}
			done
		fi
		if [[ $D == "audioencoder"* || $D == "audiodecoder"* ]]; then
			sed -i "s/-DUSE_LTO=1//g" debian/rules
		fi

		dpkg-buildpackage $DEBUILD_OPTS -us -uc -b |& tee -a build_addons.log
		cd ..
	fi
   done
}

###
# main
###
ONLY_ADDONS=0
while :
do
  case $1 in
     -h | --help)
       usage
       exit
       ;;
    -a)
       ONLY_ADDONS=1
       shift
       ;;
    --armv6)
       CPU="arm1176jzf-s"
       shift
       ;;
    --armv7)
       CPU="cortex-a7"
       shift
       ;;
    -j)
       BUILD_THREADS=$2
       shift 2
       ;;
    *)
       break
       ;;
  esac
done

setEnv
checkEnv
if [[ $ONLY_ADDONS == 0 ]]
then
    configure
    compile
    package
fi

if [[ $ADDONS_TO_BUILD != "" ]]
then 
	compileAddons
fi
