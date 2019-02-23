#.rst:
# FindLibgcrypt
# -------
# Finds the Libgcrypt library
#
# This will define the following variables::
#
# LIBGCRYPT_FOUND - system has Libgcrypt
# LIBGCRYPT_INCLUDE_DIRS - the Libgcrypt include directory
# LIBGCRYPT_LIBRARIES - the Libgcrypt libraries
# LIBGCRYPT_DEFINITIONS - the Libgcrypt compile definitions

find_path(LIBGCRYPT_INCLUDE_DIR gcrypt.h)
find_library(LIBGCRYPT_LIBRARY NAMES gcrypt)

include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(Libgcrypt REQUIRED_VARS LIBGCRYPT_LIBRARY LIBGCRYPT_INCLUDE_DIR)

if(LIBGCRYPT_FOUND)
  set(LIBGCRYPT_LIBRARIES ${LIBGCRYPT_LIBRARY})
  set(LIBGCRYPT_INCLUDE_DIRS ${LIBGCRYPT_INCLUDE_DIR})
  set(LIBGCRYPT_DEFINITIONS -DHAVE_GCRYPT=1)
endif()

mark_as_advanced(LIBGCRYPT_LIBRARY LIBGCRYPT_INCLUDE_DIR)
