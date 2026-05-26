cmake_minimum_required(VERSION 3.16)
project(sonare-node LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# node-addon-api include path
execute_process(
  COMMAND node -p "require('node-addon-api').include"
  WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
  OUTPUT_VARIABLE NAPI_INCLUDE_DIR
  OUTPUT_STRIP_TRAILING_WHITESPACE
)
# Strip quotes from the output
string(REPLACE "\"" "" NAPI_INCLUDE_DIR ${NAPI_INCLUDE_DIR})

# Root of the libsonare source tree
set(SONARE_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../..)

# Optional: enable FFmpeg-backed decoding (M4A/AAC/FLAC/OGG/Opus/...). Forwarded
# from the npm build via SONARE_FFMPEG (handled in scripts/build-native.mjs):
#   unset / AUTO -> probe via pkg-config; enable if found, else WAV/MP3 only
#   1 / ON       -> require FFmpeg (FATAL_ERROR if missing)
#   0 / OFF      -> force OFF
set(SONARE_WITH_FFMPEG "AUTO" CACHE STRING
    "FFmpeg-based decoding for M4A/AAC/FLAC/OGG: ON (require), OFF (disable), AUTO (detect via pkg-config, default)")
set_property(CACHE SONARE_WITH_FFMPEG PROPERTY STRINGS ON OFF AUTO)

# Core library sources (built inline as static)
set(SONARE_SOURCES
  ${SONARE_ROOT}/src/util/math_utils.cpp
  ${SONARE_ROOT}/src/util/nnls.cpp
  ${SONARE_ROOT}/src/core/convert.cpp
  ${SONARE_ROOT}/src/core/window.cpp
  ${SONARE_ROOT}/src/core/fft.cpp
  ${SONARE_ROOT}/src/core/audio_io.cpp
  ${SONARE_ROOT}/src/core/audio.cpp
  ${SONARE_ROOT}/src/core/resample.cpp
  ${SONARE_ROOT}/src/core/spectrum.cpp
  ${SONARE_ROOT}/src/core/db_convert.cpp
  ${SONARE_ROOT}/src/core/iirt.cpp
  ${SONARE_ROOT}/src/core/pcen.cpp
  ${SONARE_ROOT}/src/core/weighting.cpp
  ${SONARE_ROOT}/src/core/synthesis.cpp
  ${SONARE_ROOT}/src/core/audio_ops.cpp
  ${SONARE_ROOT}/src/core/harmonic.cpp
  ${SONARE_ROOT}/src/filters/mel.cpp
  ${SONARE_ROOT}/src/filters/chroma.cpp
  ${SONARE_ROOT}/src/filters/dct.cpp
  ${SONARE_ROOT}/src/filters/iir.cpp
  ${SONARE_ROOT}/src/filters/wavelet.cpp
  ${SONARE_ROOT}/src/feature/mel_spectrogram.cpp
  ${SONARE_ROOT}/src/feature/chroma.cpp
  ${SONARE_ROOT}/src/feature/nnls_chroma.cpp
  ${SONARE_ROOT}/src/feature/spectral.cpp
  ${SONARE_ROOT}/src/feature/onset.cpp
  ${SONARE_ROOT}/src/feature/pitch.cpp
  ${SONARE_ROOT}/src/feature/cqt.cpp
  ${SONARE_ROOT}/src/feature/inverse.cpp
  ${SONARE_ROOT}/src/feature/vqt.cpp
  ${SONARE_ROOT}/src/feature/rhythm.cpp
  ${SONARE_ROOT}/src/feature/tonnetz.cpp
  ${SONARE_ROOT}/src/feature/segment.cpp
  ${SONARE_ROOT}/src/effects/hpss.cpp
  ${SONARE_ROOT}/src/effects/phase_vocoder.cpp
  ${SONARE_ROOT}/src/effects/native_spectral_stretch.cpp
  ${SONARE_ROOT}/src/effects/time_stretch.cpp
  ${SONARE_ROOT}/src/effects/pitch_shift.cpp
  ${SONARE_ROOT}/src/effects/normalize.cpp
  ${SONARE_ROOT}/src/effects/decompose.cpp
  ${SONARE_ROOT}/src/effects/preemphasis.cpp
  ${SONARE_ROOT}/src/effects/remix.cpp
  ${SONARE_ROOT}/src/effects/silence.cpp
  ${SONARE_ROOT}/src/util/frame.cpp
  ${SONARE_ROOT}/src/util/padding.cpp
  ${SONARE_ROOT}/src/util/peak.cpp
  ${SONARE_ROOT}/src/util/sequence.cpp
  ${SONARE_ROOT}/src/util/vector_normalize.cpp
  ${SONARE_ROOT}/src/mastering/maximizer/loudness_optimize.cpp
  ${SONARE_ROOT}/src/metering/basic.cpp
  ${SONARE_ROOT}/src/metering/clipping.cpp
  ${SONARE_ROOT}/src/metering/dynamic_range.cpp
  ${SONARE_ROOT}/src/metering/lufs.cpp
  ${SONARE_ROOT}/src/metering/phase_scope.cpp
  ${SONARE_ROOT}/src/metering/spectrum.cpp
  ${SONARE_ROOT}/src/metering/spectrogram.cpp
  ${SONARE_ROOT}/src/metering/stereo.cpp
  ${SONARE_ROOT}/src/metering/true_peak.cpp
  ${SONARE_ROOT}/src/analysis/key_profiles.cpp
  ${SONARE_ROOT}/src/analysis/chord_templates.cpp
  ${SONARE_ROOT}/src/analysis/acoustic_analyzer.cpp
  ${SONARE_ROOT}/src/analysis/bpm_analyzer.cpp
  ${SONARE_ROOT}/src/analysis/key_analyzer.cpp
  ${SONARE_ROOT}/src/analysis/onset_analyzer.cpp
  ${SONARE_ROOT}/src/analysis/dynamics_analyzer.cpp
  ${SONARE_ROOT}/src/analysis/beat_analyzer.cpp
  ${SONARE_ROOT}/src/analysis/meter_analyzer.cpp
  ${SONARE_ROOT}/src/analysis/chord_analyzer.cpp
  ${SONARE_ROOT}/src/analysis/chord_hmm.cpp
  ${SONARE_ROOT}/src/analysis/downbeat_analyzer.cpp
  ${SONARE_ROOT}/src/analysis/boundary_detector.cpp
  ${SONARE_ROOT}/src/analysis/rhythm_analyzer.cpp
  ${SONARE_ROOT}/src/analysis/timbre_analyzer.cpp
  ${SONARE_ROOT}/src/analysis/melody_analyzer.cpp
  ${SONARE_ROOT}/src/analysis/section_analyzer.cpp
  ${SONARE_ROOT}/src/analysis/music_analyzer.cpp
  ${SONARE_ROOT}/src/streaming/stream_analyzer.cpp
  ${SONARE_ROOT}/src/streaming/stream_analyzer_output.cpp
  ${SONARE_ROOT}/src/streaming/stream_analyzer_utils.cpp
  ${SONARE_ROOT}/src/quick.cpp
  ${SONARE_ROOT}/src/sonare_c.cpp
  ${SONARE_ROOT}/src/sonare_c_effects.cpp
  ${SONARE_ROOT}/src/sonare_c_features.cpp
  ${SONARE_ROOT}/src/sonare_c_compat.cpp
  ${SONARE_ROOT}/src/sonare_c_internal.cpp
  ${SONARE_ROOT}/src/sonare_c_memory.cpp
)

file(GLOB_RECURSE SONARE_MASTERING_SOURCES CONFIGURE_DEPENDS
  ${SONARE_ROOT}/src/mastering/*.cpp
)
list(APPEND SONARE_SOURCES ${SONARE_MASTERING_SOURCES})
# Pitch editing and voice changing power the editing-oriented effects
# (pitchCorrectToMidi, noteStretch, voiceChange).
file(GLOB_RECURSE SONARE_PITCH_EDITOR_SOURCES CONFIGURE_DEPENDS
  ${SONARE_ROOT}/src/editing/pitch_editor/*.cpp
)
list(APPEND SONARE_SOURCES ${SONARE_PITCH_EDITOR_SOURCES})
file(GLOB_RECURSE SONARE_VOICE_CHANGER_SOURCES CONFIGURE_DEPENDS
  ${SONARE_ROOT}/src/editing/voice_changer/*.cpp
)
list(APPEND SONARE_SOURCES ${SONARE_VOICE_CHANGER_SOURCES})
file(GLOB_RECURSE SONARE_MIXING_SOURCES CONFIGURE_DEPENDS
  ${SONARE_ROOT}/src/mixing/*.cpp
)
list(APPEND SONARE_SOURCES ${SONARE_MIXING_SOURCES})
file(GLOB_RECURSE SONARE_RT_SOURCES CONFIGURE_DEPENDS
  ${SONARE_ROOT}/src/rt/*.cpp
)
list(APPEND SONARE_SOURCES ${SONARE_RT_SOURCES})
file(GLOB_RECURSE SONARE_TRANSPORT_SOURCES CONFIGURE_DEPENDS
  ${SONARE_ROOT}/src/transport/*.cpp
)
list(APPEND SONARE_SOURCES ${SONARE_TRANSPORT_SOURCES})
file(GLOB_RECURSE SONARE_AUTOMATION_SOURCES CONFIGURE_DEPENDS
  ${SONARE_ROOT}/src/automation/*.cpp
)
list(APPEND SONARE_SOURCES ${SONARE_AUTOMATION_SOURCES})
file(GLOB_RECURSE SONARE_ENGINE_SOURCES CONFIGURE_DEPENDS
  ${SONARE_ROOT}/src/engine/*.cpp
)
list(APPEND SONARE_SOURCES ${SONARE_ENGINE_SOURCES})
list(APPEND SONARE_SOURCES ${SONARE_ROOT}/src/sonare_c_daw.cpp)
list(APPEND SONARE_SOURCES ${SONARE_ROOT}/src/sonare_c_mixing.cpp)
# sonare_c_mixing.cpp routes the mixer through the routing graph.
list(APPEND SONARE_SOURCES
  ${SONARE_ROOT}/src/graph/graph.cpp
  ${SONARE_ROOT}/src/graph/node.cpp
)
# The channel-strip insert factory (mastering/api/insert_factory.cpp, pulled in
# via the mastering GLOB above) can instantiate effects.reverb.* inserts when
# SONARE_HAVE_FX is defined. Compile the reverb sources it links against so the
# addon resolves those symbols, matching the native sonare_fx target.
list(APPEND SONARE_SOURCES
  ${SONARE_ROOT}/src/effects/reverb/dattorro_reverb.cpp
  ${SONARE_ROOT}/src/effects/reverb/fdn_reverb.cpp
  ${SONARE_ROOT}/src/effects/reverb/velvet_reverb.cpp
  ${SONARE_ROOT}/src/effects/reverb/convolution_reverb.cpp
  # dc_blocker.cpp is a transitive dependency of the FDN/convolution reverbs.
  ${SONARE_ROOT}/src/effects/common/dc_blocker.cpp
)
list(REMOVE_DUPLICATES SONARE_SOURCES)

# KissFFT sources
set(KISSFFT_SOURCES
  ${SONARE_ROOT}/third_party/kissfft/kiss_fft.c
  ${SONARE_ROOT}/third_party/kissfft/kiss_fftr.c
)

# Addon sources
set(ADDON_SOURCES
  src/addon/addon.cpp
  src/addon/sonare_wrap_engine.cpp
  src/addon/sonare_wrap.cpp
  src/addon/sonare_wrap_analysis.cpp
  src/addon/sonare_wrap_effects.cpp
  src/addon/sonare_wrap_features.cpp
  src/addon/sonare_wrap_mixer.cpp
  src/addon/sonare_wrap_streaming.cpp
  src/addon/sonare_wrap_utils.cpp
)

# Find Eigen3
find_package(Eigen3 3.3 REQUIRED NO_MODULE)

# Optional FFmpeg discovery (mirrors src/CMakeLists.txt).
#   ON   -> required (FATAL_ERROR if missing)
#   OFF  -> forced off
#   AUTO -> probe pkg-config; enable if found, else WAV/MP3 only
set(_node_ffmpeg_mode "${SONARE_WITH_FFMPEG}")
string(TOUPPER "${_node_ffmpeg_mode}" _node_ffmpeg_mode)
if(_node_ffmpeg_mode MATCHES "^(TRUE|YES|1)$")
  set(_node_ffmpeg_mode "ON")
elseif(_node_ffmpeg_mode MATCHES "^(FALSE|NO|0)$")
  set(_node_ffmpeg_mode "OFF")
endif()

set(_node_ffmpeg_enabled OFF)

if(NOT _node_ffmpeg_mode STREQUAL "OFF")
  find_package(PkgConfig QUIET)
  set(_node_ffmpeg_found OFF)
  if(PkgConfig_FOUND)
    pkg_check_modules(FFMPEG QUIET IMPORTED_TARGET
      libavformat
      libavcodec
      libavutil
      libswresample
    )
    if(FFMPEG_FOUND)
      set(_node_ffmpeg_found ON)
    endif()
  endif()

  if(_node_ffmpeg_found)
    list(APPEND SONARE_SOURCES ${SONARE_ROOT}/src/core/audio_io_ffmpeg.cpp)
    set(_node_ffmpeg_enabled ON)
    message(STATUS "sonare-node: FFmpeg detected (libavformat ${FFMPEG_libavformat_VERSION}) "
                   "-> M4A/AAC/FLAC/OGG enabled")
  elseif(_node_ffmpeg_mode STREQUAL "ON")
    message(FATAL_ERROR
      "SONARE_WITH_FFMPEG=ON but FFmpeg development libraries were not found via pkg-config.\n"
      "Install them, e.g.:\n"
      "  macOS:         brew install ffmpeg\n"
      "  Debian/Ubuntu: sudo apt install libavformat-dev libavcodec-dev libavutil-dev libswresample-dev\n"
      "  Fedora:        sudo dnf install ffmpeg-devel\n"
      "If FFmpeg is in a non-standard prefix, set PKG_CONFIG_PATH to its .pc directory, "
      "or pass -DSONARE_WITH_FFMPEG=OFF to opt out.")
  else()
    message(STATUS "sonare-node: FFmpeg not found -> WAV/MP3 only "
                   "(pass -DSONARE_WITH_FFMPEG=ON to require, or install FFmpeg dev libs to enable)")
  endif()
endif()

# Build the addon
add_library(${PROJECT_NAME} SHARED
  ${SONARE_SOURCES}
  ${KISSFFT_SOURCES}
  ${ADDON_SOURCES}
  ${CMAKE_JS_SRC}
)

set_target_properties(${PROJECT_NAME} PROPERTIES
  PREFIX ""
  SUFFIX ".node"
)

target_include_directories(${PROJECT_NAME} PRIVATE
  ${CMAKE_JS_INC}
  ${NAPI_INCLUDE_DIR}
  ${SONARE_ROOT}/src
  ${SONARE_ROOT}/third_party/kissfft
  ${SONARE_ROOT}/third_party/dr_libs
  ${SONARE_ROOT}/third_party/minimp3
  ${SONARE_ROOT}/third_party/r8brain
)

target_link_libraries(${PROJECT_NAME} PRIVATE
  Eigen3::Eigen
  ${CMAKE_JS_LIB}
)

if(_node_ffmpeg_enabled)
  target_link_libraries(${PROJECT_NAME} PRIVATE PkgConfig::FFMPEG)
  target_compile_definitions(${PROJECT_NAME} PRIVATE SONARE_WITH_FFMPEG=1)
endif()

target_compile_definitions(${PROJECT_NAME} PRIVATE
  NAPI_DISABLE_CPP_EXCEPTIONS
  KISS_FFT_FIXED=0
  SONARE_WITH_GRAPH=1
  SONARE_WITH_PITCH_EDITOR=1
  SONARE_WITH_VOICE_CHANGER=1
  # Enable the effects.reverb.* branch of the channel-strip insert factory
  # (mirrors src/CMakeLists.txt's sonare_mastering + sonare_fx wiring).
  SONARE_HAVE_FX
)

target_compile_options(${PROJECT_NAME} PRIVATE
  -Wall
  -Wextra
)
