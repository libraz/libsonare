cmake_minimum_required(VERSION 3.16)
project(sonare-node LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Root of the libsonare source tree.
set(SONARE_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../..)
get_filename_component(SONARE_ROOT ${SONARE_ROOT} ABSOLUTE)

# node-addon-api include path.
execute_process(
  COMMAND node -p "require('node-addon-api').include"
  WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
  OUTPUT_VARIABLE NAPI_INCLUDE_DIR
  OUTPUT_STRIP_TRAILING_WHITESPACE
)
string(REPLACE "\"" "" NAPI_INCLUDE_DIR ${NAPI_INCLUDE_DIR})

# Reuse the canonical libsonare CMake graph instead of maintaining a parallel
# source list in this binding. The Node addon only compiles its N-API wrappers
# plus the C API translation units that are not part of sonare_core.
set(BUILD_TESTING OFF CACHE BOOL "" FORCE)
set(BUILD_WASM OFF CACHE BOOL "" FORCE)
set(BUILD_SHARED OFF CACHE BOOL "" FORCE)
set(BUILD_CLI OFF CACHE BOOL "" FORCE)
set(BUILD_BENCH OFF CACHE BOOL "" FORCE)
set(BUILD_MASTERING ON CACHE BOOL "" FORCE)
set(BUILD_MIXING ON CACHE BOOL "" FORCE)
set(BUILD_GRAPH ON CACHE BOOL "" FORCE)
set(BUILD_FX ON CACHE BOOL "" FORCE)
set(BUILD_PITCH_EDITOR ON CACHE BOOL "" FORCE)
set(BUILD_VOICE_CHANGER ON CACHE BOOL "" FORCE)
set(BUILD_ACOUSTIC_SIM ON CACHE BOOL "" FORCE)

# Optional: enable FFmpeg-backed decoding (M4A/AAC/FLAC/OGG/Opus/...). Forwarded
# from the npm build via SONARE_FFMPEG (handled in scripts/build-native.mjs):
#   unset / AUTO -> probe via pkg-config; enable if found, else WAV/MP3 only
#   1 / ON       -> require FFmpeg (FATAL_ERROR if missing)
#   0 / OFF      -> force OFF
set(SONARE_WITH_FFMPEG "AUTO" CACHE STRING
    "FFmpeg-based decoding for M4A/AAC/FLAC/OGG: ON (require), OFF (disable), AUTO (detect via pkg-config, default)")
set_property(CACHE SONARE_WITH_FFMPEG PROPERTY STRINGS ON OFF AUTO)

add_subdirectory(${SONARE_ROOT} ${CMAKE_BINARY_DIR}/libsonare)

set(ADDON_SOURCES
  src/addon/addon.cpp
  src/addon/sonare_wrap_engine.cpp
  src/addon/sonare_wrap.cpp
  src/addon/sonare_wrap_acoustic.cpp
  src/addon/sonare_wrap_analysis.cpp
  src/addon/sonare_wrap_effects.cpp
  src/addon/sonare_wrap_features.cpp
  src/addon/sonare_wrap_mixer.cpp
  src/addon/sonare_wrap_streaming.cpp
  src/addon/sonare_wrap_utils.cpp
)

set(SONARE_NODE_C_API_SOURCES
  ${SONARE_ROOT}/src/sonare_c_mastering_apply.cpp
  ${SONARE_ROOT}/src/sonare_c_mastering_chain.cpp
  ${SONARE_ROOT}/src/sonare_c_mastering_dynamics.cpp
  ${SONARE_ROOT}/src/sonare_c_mastering_eq.cpp
  ${SONARE_ROOT}/src/sonare_c_mastering_preset.cpp
  ${SONARE_ROOT}/src/sonare_c_mastering_repair.cpp
  ${SONARE_ROOT}/src/sonare_c_mastering_streaming.cpp
  ${SONARE_ROOT}/src/sonare_c_mixing.cpp
  ${SONARE_ROOT}/src/sonare_c_daw.cpp
  ${SONARE_ROOT}/src/sonare_c_engine.cpp
  ${SONARE_ROOT}/src/sonare_c_voice_changer.cpp
)

add_library(${PROJECT_NAME} SHARED
  ${ADDON_SOURCES}
  ${SONARE_NODE_C_API_SOURCES}
  ${CMAKE_JS_SRC}
)

set_target_properties(${PROJECT_NAME} PROPERTIES
  PREFIX ""
  SUFFIX ".node"
)

target_include_directories(${PROJECT_NAME} PRIVATE
  ${CMAKE_JS_INC}
  ${NAPI_INCLUDE_DIR}
  ${CMAKE_CURRENT_SOURCE_DIR}/src/addon
  ${SONARE_ROOT}/src
)

target_link_libraries(${PROJECT_NAME} PRIVATE
  sonare_engine
  sonare_voice_changer
  sonare_acoustic
  ${CMAKE_JS_LIB}
)

target_compile_definitions(${PROJECT_NAME} PRIVATE
  NAPI_DISABLE_CPP_EXCEPTIONS
  SONARE_WITH_MASTERING=1
  SONARE_WITH_GRAPH=1
  SONARE_WITH_MIXING=1
  SONARE_WITH_PITCH_EDITOR=1
  SONARE_WITH_VOICE_CHANGER=1
  SONARE_WITH_ACOUSTIC_SIM=1
)

if(MSVC)
  target_compile_options(${PROJECT_NAME} PRIVATE /W4)
else()
  target_compile_options(${PROJECT_NAME} PRIVATE -Wall -Wextra)
endif()
