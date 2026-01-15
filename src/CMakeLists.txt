# KissFFT library (third-party, warnings disabled)
add_library(kissfft STATIC
  ${CMAKE_SOURCE_DIR}/third_party/kissfft/kiss_fft.c
  ${CMAKE_SOURCE_DIR}/third_party/kissfft/kiss_fftr.c
)
target_include_directories(kissfft PUBLIC ${CMAKE_SOURCE_DIR}/third_party/kissfft)
target_compile_definitions(kissfft PRIVATE KISS_FFT_FIXED=0)
# Disable warnings for third-party code
if(MSVC)
  target_compile_options(kissfft PRIVATE /W0)
else()
  target_compile_options(kissfft PRIVATE -w)
endif()

# Source files
set(SONARE_SOURCES
  util/math_utils.cpp
  core/convert.cpp
  core/window.cpp
  core/fft.cpp
  core/audio_io.cpp
  core/audio.cpp
  core/resample.cpp
  core/spectrum.cpp
  filters/mel.cpp
  filters/chroma.cpp
  filters/dct.cpp
  filters/iir.cpp
  feature/mel_spectrogram.cpp
  feature/chroma.cpp
  feature/spectral.cpp
  feature/onset.cpp
  feature/pitch.cpp
  feature/cqt.cpp
  feature/vqt.cpp
  effects/hpss.cpp
  effects/phase_vocoder.cpp
  effects/time_stretch.cpp
  effects/pitch_shift.cpp
  effects/normalize.cpp
  analysis/key_profiles.cpp
  analysis/chord_templates.cpp
  analysis/bpm_analyzer.cpp
  analysis/key_analyzer.cpp
  analysis/onset_analyzer.cpp
  analysis/dynamics_analyzer.cpp
  analysis/beat_analyzer.cpp
  analysis/chord_analyzer.cpp
  analysis/boundary_detector.cpp
  analysis/rhythm_analyzer.cpp
  analysis/timbre_analyzer.cpp
  analysis/melody_analyzer.cpp
  analysis/section_analyzer.cpp
  analysis/music_analyzer.cpp
  streaming/stream_analyzer.cpp
  quick.cpp
  sonare_c.cpp
)

# Library
add_library(sonare_core STATIC ${SONARE_SOURCES})

target_include_directories(sonare_core PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}
  ${CMAKE_SOURCE_DIR}/third_party/kissfft
  ${CMAKE_SOURCE_DIR}/third_party/dr_libs
  ${CMAKE_SOURCE_DIR}/third_party/minimp3
  ${CMAKE_SOURCE_DIR}/third_party/r8brain
)

target_link_libraries(sonare_core PUBLIC Eigen3::Eigen kissfft)

# WASM build
if(BUILD_WASM)
  add_executable(sonare wasm/bindings.cpp)
  target_link_libraries(sonare PRIVATE sonare_core)
  target_link_options(sonare PRIVATE
    -sEXPORT_NAME='Sonare'
    -sEXPORTED_RUNTIME_METHODS=['UTF8ToString']
    -sMODULARIZE=1
    -sEXPORT_ES6=1
    -sALLOW_MEMORY_GROWTH=1
    -sSTACK_SIZE=8388608
    --bind
  )

  # Copy output to dist directory
  add_custom_command(TARGET sonare POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_SOURCE_DIR}/dist
    COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_FILE:sonare> ${CMAKE_SOURCE_DIR}/dist/
    COMMAND ${CMAKE_COMMAND} -E copy ${CMAKE_BINARY_DIR}/bin/sonare.wasm ${CMAKE_SOURCE_DIR}/dist/
    COMMENT "Copying WASM files to dist/"
  )
endif()
