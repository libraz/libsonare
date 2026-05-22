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
  util/frame.cpp
  util/nnls.cpp
  util/padding.cpp
  util/peak.cpp
  util/sequence.cpp
  util/vector_normalize.cpp
  core/convert.cpp
  core/window.cpp
  core/fft.cpp
  core/audio_io.cpp
  core/audio.cpp
  core/db_convert.cpp
  core/iirt.cpp
  core/pcen.cpp
  core/resample.cpp
  core/spectrum.cpp
  core/weighting.cpp
  core/synthesis.cpp
  core/audio_ops.cpp
  core/harmonic.cpp
  filters/mel.cpp
  filters/chroma.cpp
  filters/dct.cpp
  filters/iir.cpp
  filters/wavelet.cpp
  feature/mel_spectrogram.cpp
  feature/chroma.cpp
  feature/spectral.cpp
  feature/onset.cpp
  feature/pitch.cpp
  feature/cqt.cpp
  feature/inverse.cpp
  feature/tonnetz.cpp
  feature/segment.cpp
  feature/vqt.cpp
  feature/rhythm.cpp
  effects/hpss.cpp
  effects/phase_vocoder.cpp
  effects/time_stretch.cpp
  effects/pitch_shift.cpp
  effects/normalize.cpp
  effects/decompose.cpp
  effects/preemphasis.cpp
  effects/remix.cpp
  effects/silence.cpp
  analysis/meter/basic.cpp
  analysis/meter/clipping.cpp
  analysis/meter/dynamic_range.cpp
  analysis/meter/lufs.cpp
  analysis/meter/phase_scope.cpp
  analysis/meter/spectrum.cpp
  analysis/meter/spectrogram.cpp
  analysis/meter/stereo.cpp
  analysis/meter/true_peak.cpp
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
  streaming/stream_analyzer_output.cpp
  streaming/stream_analyzer_utils.cpp
  quick.cpp
  sonare_c.cpp
  sonare_c_effects.cpp
  sonare_c_features.cpp
  sonare_c_compat.cpp
  sonare_c_internal.cpp
  sonare_c_memory.cpp
)

# Decide whether to enable FFmpeg integration.
#   ON   -> required (FATAL_ERROR if missing)
#   OFF  -> forced off
#   AUTO -> probe pkg-config; enable if found, else WAV/MP3 only
set(_sonare_ffmpeg_mode "${SONARE_WITH_FFMPEG}")
string(TOUPPER "${_sonare_ffmpeg_mode}" _sonare_ffmpeg_mode)
# Normalize TRUE/FALSE/1/0/YES/NO to ON/OFF
if(_sonare_ffmpeg_mode MATCHES "^(TRUE|YES|1)$")
  set(_sonare_ffmpeg_mode "ON")
elseif(_sonare_ffmpeg_mode MATCHES "^(FALSE|NO|0)$")
  set(_sonare_ffmpeg_mode "OFF")
endif()

set(SONARE_FFMPEG_ENABLED OFF CACHE INTERNAL "Whether FFmpeg decoding is linked")

if(NOT BUILD_WASM AND NOT _sonare_ffmpeg_mode STREQUAL "OFF")
  find_package(PkgConfig QUIET)
  set(_sonare_ffmpeg_found OFF)
  if(PkgConfig_FOUND)
    pkg_check_modules(FFMPEG QUIET IMPORTED_TARGET
      libavformat
      libavcodec
      libavutil
      libswresample
    )
    if(FFMPEG_FOUND)
      set(_sonare_ffmpeg_found ON)
    endif()
  endif()

  if(_sonare_ffmpeg_found)
    list(APPEND SONARE_SOURCES core/audio_io_ffmpeg.cpp)
    set(SONARE_FFMPEG_ENABLED ON CACHE INTERNAL "" FORCE)
    message(STATUS "libsonare: FFmpeg detected (libavformat ${FFMPEG_libavformat_VERSION}) "
                   "-> M4A/AAC/FLAC/OGG enabled")
  elseif(_sonare_ffmpeg_mode STREQUAL "ON")
    message(FATAL_ERROR
      "SONARE_WITH_FFMPEG=ON but FFmpeg development libraries were not found via pkg-config.\n"
      "Install them, e.g.:\n"
      "  macOS:         brew install ffmpeg\n"
      "  Debian/Ubuntu: sudo apt install libavformat-dev libavcodec-dev libavutil-dev libswresample-dev\n"
      "  Fedora:        sudo dnf install ffmpeg-devel\n"
      "If FFmpeg is in a non-standard prefix, set PKG_CONFIG_PATH to its .pc directory, "
      "or pass -DSONARE_WITH_FFMPEG=OFF to opt out.")
  else()
    message(STATUS "libsonare: FFmpeg not found -> WAV/MP3 only "
                   "(pass -DSONARE_WITH_FFMPEG=ON to require, or install FFmpeg dev libs to enable)")
  endif()
endif()

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
target_compile_options(sonare_core PRIVATE ${SONARE_WARNING_FLAGS})

# Threading support for parallel analysis on native builds
if(NOT BUILD_WASM)
  find_package(Threads REQUIRED)
  target_link_libraries(sonare_core PUBLIC Threads::Threads)
endif()

if(SONARE_FFMPEG_ENABLED)
  target_link_libraries(sonare_core PUBLIC PkgConfig::FFMPEG)
  target_compile_definitions(sonare_core PUBLIC SONARE_WITH_FFMPEG=1)
endif()

if(BUILD_MASTERING)
  add_library(sonare_mastering STATIC
    mastering/api/named_processor.cpp
    mastering/common/biquad_design.cpp
    mastering/common/param_smoother.cpp
    mastering/common/envelope_follower.cpp
    mastering/common/hysteresis_ja.cpp
    mastering/common/oversampler.cpp
    mastering/common/partitioned_convolver.cpp
    mastering/common/delay_line.cpp
    mastering/common/lookahead_buffer.cpp
    mastering/common/lpc.cpp
    mastering/common/noise_tracker.cpp
    mastering/common/true_peak_filter.cpp
    mastering/dynamics/brickwall_limiter.cpp
    mastering/dynamics/compressor.cpp
    mastering/dynamics/deesser.cpp
    mastering/dynamics/expander.cpp
    mastering/dynamics/gate.cpp
    mastering/dynamics/limiter.cpp
    mastering/dynamics/parallel_comp.cpp
    mastering/dynamics/sidechain_router.cpp
    mastering/dynamics/transient_shaper.cpp
    mastering/dynamics/upward_compressor.cpp
    mastering/dynamics/upward_expander.cpp
    mastering/dynamics/vocal_rider.cpp
    mastering/final/bit_depth.cpp
    mastering/final/dither.cpp
    mastering/final/output_chain.cpp
    mastering/multiband/crossover.cpp
    mastering/multiband/multiband_compressor.cpp
    mastering/multiband/multiband_dynamic_eq.cpp
    mastering/multiband/multiband_expander.cpp
    mastering/multiband/multiband_imager.cpp
    mastering/multiband/multiband_limiter.cpp
    mastering/multiband/multiband_saturation.cpp
    mastering/maximizer/adaptive_release.cpp
    mastering/maximizer/loudness_optimize.cpp
    mastering/maximizer/maximizer.cpp
    mastering/maximizer/soft_knee_max.cpp
    mastering/maximizer/streaming_preview.cpp
    mastering/maximizer/true_peak_limiter.cpp
    mastering/match/ab_switcher.cpp
    mastering/match/match_eq.cpp
    mastering/match/reference_loudness.cpp
    mastering/match/reference_spectrum.cpp
    mastering/match/tonal_balance.cpp
    mastering/repair/declick.cpp
    mastering/repair/declip.cpp
    mastering/repair/decrackle.cpp
    mastering/repair/dehum.cpp
    mastering/repair/denoise_classical.cpp
    mastering/repair/dereverb_classical.cpp
    mastering/repair/trim_silence.cpp
    mastering/saturation/bitcrusher.cpp
    mastering/saturation/exciter.cpp
    mastering/saturation/hard_clipper.cpp
    mastering/saturation/multiband_exciter.cpp
    mastering/saturation/soft_clipper.cpp
    mastering/saturation/tape.cpp
    mastering/saturation/transformer.cpp
    mastering/saturation/tube.cpp
    mastering/saturation/waveshaper.cpp
    mastering/spectral/air_band.cpp
    mastering/spectral/low_end_focus.cpp
    mastering/spectral/presence_enhancer.cpp
    mastering/spectral/spectral_shaper.cpp
    mastering/stereo/auto_pan.cpp
    mastering/stereo/haas_enhancer.cpp
    mastering/stereo/imager.cpp
    mastering/stereo/mid_side.cpp
    mastering/stereo/mono_compat_check.cpp
    mastering/stereo/mono_maker.cpp
    mastering/stereo/phase_align.cpp
    mastering/stereo/stereo_balance.cpp
    mastering/eq/api_style.cpp
    mastering/eq/band_pass.cpp
    mastering/eq/cut_filter.cpp
    mastering/eq/dynamic_eq.cpp
    mastering/eq/graphic_eq.cpp
    mastering/eq/linear_phase.cpp
    mastering/eq/mid_side_eq.cpp
    mastering/eq/minimum_phase.cpp
    mastering/eq/parametric.cpp
    mastering/eq/pultec.cpp
    mastering/eq/shelving.cpp
    mastering/eq/tilt.cpp
  )
  target_include_directories(sonare_mastering PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/third_party/kissfft
    ${CMAKE_SOURCE_DIR}/third_party/dr_libs
    ${CMAKE_SOURCE_DIR}/third_party/minimp3
    ${CMAKE_SOURCE_DIR}/third_party/r8brain
  )
  target_link_libraries(sonare_mastering PUBLIC sonare_core)
  target_compile_options(sonare_mastering PRIVATE ${SONARE_WARNING_FLAGS})
  set_target_properties(sonare_mastering PROPERTIES POSITION_INDEPENDENT_CODE ON)
endif()

# Shared library for language bindings (Python ctypes, etc.)
if(BUILD_SHARED)
  set(SONARE_SHARED_SOURCES ${SONARE_SOURCES})
  if(BUILD_MASTERING)
    list(APPEND SONARE_SHARED_SOURCES sonare_c_mastering.cpp)
  endif()
  add_library(sonare_shared SHARED ${SONARE_SHARED_SOURCES})
  set_target_properties(sonare_shared PROPERTIES
    OUTPUT_NAME sonare
    VERSION ${PROJECT_VERSION}
    SOVERSION 1
    POSITION_INDEPENDENT_CODE ON
  )
  target_include_directories(sonare_shared PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/third_party/kissfft
    ${CMAKE_SOURCE_DIR}/third_party/dr_libs
    ${CMAKE_SOURCE_DIR}/third_party/minimp3
    ${CMAKE_SOURCE_DIR}/third_party/r8brain
  )
  target_link_libraries(sonare_shared PUBLIC Eigen3::Eigen kissfft)
  if(BUILD_MASTERING)
    target_link_libraries(sonare_shared PUBLIC sonare_mastering)
    target_compile_definitions(sonare_shared PUBLIC SONARE_WITH_MASTERING=1)
  endif()
  target_compile_options(sonare_shared PRIVATE ${SONARE_WARNING_FLAGS})
  if(NOT BUILD_WASM)
    target_link_libraries(sonare_shared PUBLIC Threads::Threads)
  endif()
  if(SONARE_FFMPEG_ENABLED)
    target_link_libraries(sonare_shared PUBLIC PkgConfig::FFMPEG)
    target_compile_definitions(sonare_shared PUBLIC SONARE_WITH_FFMPEG=1)
  endif()
endif()

# WASM build
if(BUILD_WASM)
  add_executable(sonare wasm/bindings.cpp)
  target_link_libraries(sonare PRIVATE sonare_core)
  if(BUILD_MASTERING)
    target_link_libraries(sonare PRIVATE sonare_mastering)
    target_compile_definitions(sonare PRIVATE SONARE_WITH_MASTERING=1)
  else()
    target_sources(sonare PRIVATE mastering/maximizer/loudness_optimize.cpp)
  endif()
  target_link_options(sonare PRIVATE
    -sEXPORT_NAME='Sonare'
    -sEXPORTED_RUNTIME_METHODS=['UTF8ToString']
    -sMODULARIZE=1
    -sEXPORT_ES6=1
    -sALLOW_MEMORY_GROWTH=1
    -sSTACK_SIZE=8388608
    --bind
  )

  # Copy output to bindings/wasm/dist directory
  add_custom_command(TARGET sonare POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_SOURCE_DIR}/bindings/wasm/dist
    COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_FILE:sonare> ${CMAKE_SOURCE_DIR}/bindings/wasm/dist/
    COMMAND ${CMAKE_COMMAND} -E copy ${CMAKE_BINARY_DIR}/bin/sonare.wasm ${CMAKE_SOURCE_DIR}/bindings/wasm/dist/
    COMMENT "Copying WASM files to bindings/wasm/dist/"
  )
endif()
