# Install and find_package() export rules for the C++ library.
#
# Included from the top-level CMakeLists after every subdirectory has run, so
# SONARE_EXPORT_TARGETS already lists the targets this configuration produced.
# Guarded by SONARE_INSTALL, which defaults off for an add_subdirectory() build.

include(CMakePackageConfigHelpers)
include(GNUInstallDirs)

set(SONARE_CMAKE_CONFIG_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/sonare)

# --- Libraries -------------------------------------------------------------

install(TARGETS ${SONARE_EXPORT_TARGETS}
  EXPORT sonareTargets
  ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
  LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
  INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
)

# --- Headers ---------------------------------------------------------------

# The C ABI. Consumers include it namespaced, so the directory keeps its name.
install(DIRECTORY ${PROJECT_SOURCE_DIR}/include/sonare
  DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
  FILES_MATCHING PATTERN "*.h"
)

# The C++ API is the src/ header tree: sonare.h pulls in roughly four hundred
# headers that include each other by their path relative to src/, so the tree
# has to be installed with that structure intact for those includes to resolve.
# It lands under a subdirectory of its own, and the exported targets put that
# subdirectory on the consumer's include path, so `core/audio.h`-style paths and
# short names such as `sonare.h` resolve at the root of <prefix>/include/sonare/cpp.
# That exposes those generic roots to the consumer; it is the price of the
# installed headers including each other by src-relative quoted paths. The
# namespaced `<sonare/cpp/...>` spelling resolves through the include root.
#
# Excluded: the WASM embind wrappers and the macOS host backends. Neither is
# part of any distributed configuration, and the backends include OS SDK
# headers a consumer has no reason to be offered.
#
# Also excluded: c_api, whose public face is include/sonare and whose own
# headers are implementation detail. Nothing outside c_api and wasm includes
# one, so installing them only put internal headers — several with a
# file-scope `using namespace` — into the consumer's search space.
install(DIRECTORY ${PROJECT_SOURCE_DIR}/src/
  DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/sonare/cpp
  FILES_MATCHING
    PATTERN "*.h"
    PATTERN "c_api" EXCLUDE
    PATTERN "wasm" EXCLUDE
    PATTERN "host/backends" EXCLUDE
)

# --- Export set ------------------------------------------------------------

install(EXPORT sonareTargets
  FILE sonareTargets.cmake
  NAMESPACE sonare::
  DESTINATION ${SONARE_CMAKE_CONFIG_DESTINATION}
)

# Which optional subsystems this build contains. The exported targets already
# carry the matching SONARE_WITH_* compile definitions; these variables let a
# consumer branch before it writes a link line, and back the COMPONENTS form of
# find_package().
set(SONARE_CONFIG_BUILD_MASTERING ${BUILD_MASTERING})
set(SONARE_CONFIG_BUILD_MIXING ${BUILD_MIXING})
set(SONARE_CONFIG_BUILD_MIXING_ASSISTANT ${BUILD_MIXING_ASSISTANT})
set(SONARE_CONFIG_BUILD_GRAPH ${BUILD_GRAPH})
set(SONARE_CONFIG_BUILD_FX ${BUILD_FX})
set(SONARE_CONFIG_BUILD_ACOUSTIC_SIM ${BUILD_ACOUSTIC_SIM})
set(SONARE_CONFIG_BUILD_PITCH_EDITOR ${BUILD_PITCH_EDITOR})
set(SONARE_CONFIG_BUILD_VOICE_CHANGER ${BUILD_VOICE_CHANGER})
set(SONARE_CONFIG_BUILD_PLAYBACK ${BUILD_PLAYBACK})
set(SONARE_CONFIG_BUILD_ARRANGEMENT ${BUILD_ARRANGEMENT})
set(SONARE_CONFIG_BUILD_ASSIST ${BUILD_ASSIST})
set(SONARE_CONFIG_BUILD_SHARED ${BUILD_SHARED})
set(SONARE_CONFIG_WITH_FFMPEG ${SONARE_FFMPEG_ENABLED})

configure_package_config_file(
  ${PROJECT_SOURCE_DIR}/cmake/sonareConfig.cmake.in
  ${CMAKE_CURRENT_BINARY_DIR}/sonareConfig.cmake
  INSTALL_DESTINATION ${SONARE_CMAKE_CONFIG_DESTINATION}
)

# The archives are rebuilt from source by whoever installs them, so a consumer
# asking for an older version gets a newer one without complaint; what breaks
# compatibility here is a C++ API change, which SemVer major covers.
write_basic_package_version_file(
  ${CMAKE_CURRENT_BINARY_DIR}/sonareConfigVersion.cmake
  VERSION ${PROJECT_VERSION}
  COMPATIBILITY SameMajorVersion
)

install(FILES
  ${CMAKE_CURRENT_BINARY_DIR}/sonareConfig.cmake
  ${CMAKE_CURRENT_BINARY_DIR}/sonareConfigVersion.cmake
  DESTINATION ${SONARE_CMAKE_CONFIG_DESTINATION}
)

# --- pkg-config ------------------------------------------------------------

# Only for the shared build. pkg-config describes one library, and the static
# configuration is a dependency-ordered set of nineteen archives whose contents
# depend on the BUILD_* options -- a `.pc` for that would be a second, silently
# drifting copy of the link graph CMake already exports correctly. The shared
# library is a single artifact exposing the C ABI, which is exactly what a
# pkg-config consumer wants.
if(BUILD_SHARED)
  if(IS_ABSOLUTE "${CMAKE_INSTALL_LIBDIR}")
    set(SONARE_PC_LIBDIR "${CMAKE_INSTALL_LIBDIR}")
  else()
    set(SONARE_PC_LIBDIR "\${exec_prefix}/${CMAKE_INSTALL_LIBDIR}")
  endif()
  if(IS_ABSOLUTE "${CMAKE_INSTALL_INCLUDEDIR}")
    set(SONARE_PC_INCLUDEDIR "${CMAKE_INSTALL_INCLUDEDIR}")
  else()
    set(SONARE_PC_INCLUDEDIR "\${prefix}/${CMAKE_INSTALL_INCLUDEDIR}")
  endif()

  # Relative to the .pc directory so a relocated or re-prefixed install resolves.
  if(IS_ABSOLUTE "${CMAKE_INSTALL_LIBDIR}")
    set(SONARE_PC_PREFIX "${CMAKE_INSTALL_PREFIX}")
  else()
    file(RELATIVE_PATH _sonare_pc_rel
      "${CMAKE_INSTALL_PREFIX}/${CMAKE_INSTALL_LIBDIR}/pkgconfig" "${CMAKE_INSTALL_PREFIX}")
    string(REGEX REPLACE "/+$" "" _sonare_pc_rel "${_sonare_pc_rel}")
    set(SONARE_PC_PREFIX "\${pcfiledir}/${_sonare_pc_rel}")
  endif()

  set(SONARE_PC_REQUIRES_PRIVATE "")
  if(SONARE_FFMPEG_ENABLED)
    set(SONARE_PC_REQUIRES_PRIVATE "libavformat libavcodec libavutil libswresample")
  endif()
  set(SONARE_PC_LIBS_PRIVATE "${CMAKE_THREAD_LIBS_INIT}")

  configure_file(
    ${PROJECT_SOURCE_DIR}/cmake/sonare.pc.in
    ${CMAKE_CURRENT_BINARY_DIR}/sonare.pc
    @ONLY
  )
  install(FILES ${CMAKE_CURRENT_BINARY_DIR}/sonare.pc
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/pkgconfig)
else()
  message(STATUS "libsonare: pkg-config file needs a single shared artifact; "
                 "pass -DBUILD_SHARED=ON to install sonare.pc")
endif()
