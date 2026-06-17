# macOS host backends — concrete sonare::host seam implementations that include
# OS / plugin-SDK headers. Kept out of core (invariant 6): each is an
# independent leaf static library that links the core/engine targets but is
# NEVER reverse-depended by them, is default OFF, and is excluded from the
# default distribution and every language binding. This directory is only added
# on APPLE with at least one backend enabled (see the top-level CMakeLists.txt).

# The .mm translation units are Objective-C++. No other part of the tree uses
# Objective-C++, so the language is enabled locally here rather than at project
# scope.
enable_language(OBJCXX)

# Frameworks shared by the audio backends.
find_library(COREFOUNDATION_FRAMEWORK CoreFoundation REQUIRED)

if(BUILD_COREAUDIO)
  find_library(COREAUDIO_FRAMEWORK CoreAudio REQUIRED)
  find_library(AUDIOTOOLBOX_FRAMEWORK AudioToolbox REQUIRED)
  find_library(AUDIOUNIT_FRAMEWORK AudioUnit REQUIRED)

  add_library(sonare_host_coreaudio STATIC coreaudio/coreaudio_device.mm)
  target_link_libraries(sonare_host_coreaudio PUBLIC sonare_engine)
  target_link_libraries(sonare_host_coreaudio PRIVATE
    ${COREAUDIO_FRAMEWORK} ${AUDIOTOOLBOX_FRAMEWORK} ${AUDIOUNIT_FRAMEWORK}
    ${COREFOUNDATION_FRAMEWORK})
  set_target_properties(sonare_host_coreaudio PROPERTIES POSITION_INDEPENDENT_CODE ON)
  message(STATUS "libsonare: building CoreAudio host backend (sonare_host_coreaudio)")
endif()

if(BUILD_COREMIDI)
  find_library(COREMIDI_FRAMEWORK CoreMIDI REQUIRED)

  add_library(sonare_host_coremidi STATIC coremidi/coremidi_io.mm)
  target_link_libraries(sonare_host_coremidi PUBLIC sonare_engine)
  target_link_libraries(sonare_host_coremidi PRIVATE
    ${COREMIDI_FRAMEWORK} ${COREFOUNDATION_FRAMEWORK})
  set_target_properties(sonare_host_coremidi PROPERTIES POSITION_INDEPENDENT_CODE ON)
  message(STATUS "libsonare: building CoreMIDI host backend (sonare_host_coremidi)")
endif()

if(BUILD_AU_HOST)
  find_library(AUDIOTOOLBOX_FRAMEWORK AudioToolbox REQUIRED)
  find_library(AUDIOUNIT_FRAMEWORK AudioUnit REQUIRED)
  find_library(AVFOUNDATION_FRAMEWORK AVFoundation REQUIRED)
  find_library(COREAUDIO_FRAMEWORK CoreAudio REQUIRED)

  add_library(sonare_host_plughost STATIC plughost/au_instrument_provider.mm)
  target_link_libraries(sonare_host_plughost PUBLIC sonare_engine)
  target_link_libraries(sonare_host_plughost PRIVATE
    ${AUDIOTOOLBOX_FRAMEWORK} ${AUDIOUNIT_FRAMEWORK} ${AVFOUNDATION_FRAMEWORK}
    ${COREAUDIO_FRAMEWORK} ${COREFOUNDATION_FRAMEWORK})
  set_target_properties(sonare_host_plughost PROPERTIES POSITION_INDEPENDENT_CODE ON)
  target_compile_definitions(sonare_host_plughost PRIVATE SONARE_HOST_WITH_AU=1)
  message(STATUS "libsonare: building Audio Unit plugin host (sonare_host_plughost)")
endif()
