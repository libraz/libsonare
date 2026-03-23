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

# Core library sources (built inline as static)
set(SONARE_SOURCES
  ${SONARE_ROOT}/src/util/math_utils.cpp
  ${SONARE_ROOT}/src/core/convert.cpp
  ${SONARE_ROOT}/src/core/window.cpp
  ${SONARE_ROOT}/src/core/fft.cpp
  ${SONARE_ROOT}/src/core/audio_io.cpp
  ${SONARE_ROOT}/src/core/audio.cpp
  ${SONARE_ROOT}/src/core/resample.cpp
  ${SONARE_ROOT}/src/core/spectrum.cpp
  ${SONARE_ROOT}/src/filters/mel.cpp
  ${SONARE_ROOT}/src/filters/chroma.cpp
  ${SONARE_ROOT}/src/filters/dct.cpp
  ${SONARE_ROOT}/src/filters/iir.cpp
  ${SONARE_ROOT}/src/feature/mel_spectrogram.cpp
  ${SONARE_ROOT}/src/feature/chroma.cpp
  ${SONARE_ROOT}/src/feature/spectral.cpp
  ${SONARE_ROOT}/src/feature/onset.cpp
  ${SONARE_ROOT}/src/feature/pitch.cpp
  ${SONARE_ROOT}/src/feature/cqt.cpp
  ${SONARE_ROOT}/src/feature/vqt.cpp
  ${SONARE_ROOT}/src/effects/hpss.cpp
  ${SONARE_ROOT}/src/effects/phase_vocoder.cpp
  ${SONARE_ROOT}/src/effects/time_stretch.cpp
  ${SONARE_ROOT}/src/effects/pitch_shift.cpp
  ${SONARE_ROOT}/src/effects/normalize.cpp
  ${SONARE_ROOT}/src/analysis/key_profiles.cpp
  ${SONARE_ROOT}/src/analysis/chord_templates.cpp
  ${SONARE_ROOT}/src/analysis/bpm_analyzer.cpp
  ${SONARE_ROOT}/src/analysis/key_analyzer.cpp
  ${SONARE_ROOT}/src/analysis/onset_analyzer.cpp
  ${SONARE_ROOT}/src/analysis/dynamics_analyzer.cpp
  ${SONARE_ROOT}/src/analysis/beat_analyzer.cpp
  ${SONARE_ROOT}/src/analysis/chord_analyzer.cpp
  ${SONARE_ROOT}/src/analysis/boundary_detector.cpp
  ${SONARE_ROOT}/src/analysis/rhythm_analyzer.cpp
  ${SONARE_ROOT}/src/analysis/timbre_analyzer.cpp
  ${SONARE_ROOT}/src/analysis/melody_analyzer.cpp
  ${SONARE_ROOT}/src/analysis/section_analyzer.cpp
  ${SONARE_ROOT}/src/analysis/music_analyzer.cpp
  ${SONARE_ROOT}/src/streaming/stream_analyzer.cpp
  ${SONARE_ROOT}/src/quick.cpp
  ${SONARE_ROOT}/src/sonare_c.cpp
)

# KissFFT sources
set(KISSFFT_SOURCES
  ${SONARE_ROOT}/third_party/kissfft/kiss_fft.c
  ${SONARE_ROOT}/third_party/kissfft/kiss_fftr.c
)

# Addon sources
set(ADDON_SOURCES
  src/addon/addon.cpp
  src/addon/sonare_wrap.cpp
)

# Find Eigen3
find_package(Eigen3 3.3 REQUIRED NO_MODULE)

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

target_compile_definitions(${PROJECT_NAME} PRIVATE
  NAPI_DISABLE_CPP_EXCEPTIONS
  KISS_FFT_FIXED=0
)

target_compile_options(${PROJECT_NAME} PRIVATE
  -Wall
  -Wextra
)
