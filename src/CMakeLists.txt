# KissFFT library (third-party, warnings disabled)
add_library(kissfft STATIC
  ${CMAKE_SOURCE_DIR}/third_party/kissfft/kiss_fft.c
  ${CMAKE_SOURCE_DIR}/third_party/kissfft/kiss_fftr.c
)
set_target_properties(kissfft PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_include_directories(kissfft SYSTEM PUBLIC ${CMAKE_SOURCE_DIR}/third_party/kissfft)
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
  util/fractional_octave.cpp
  util/lpc.cpp
  util/nnls.cpp
  util/padding.cpp
  util/peak.cpp
  util/sequence.cpp
  util/stereo_metrics.cpp
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
  feature/nnls_chroma.cpp
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
  effects/native_spectral_stretch.cpp
  effects/time_stretch.cpp
  effects/pitch_shift.cpp
  effects/normalize.cpp
  effects/decompose.cpp
  effects/preemphasis.cpp
  effects/remix.cpp
  effects/silence.cpp
  metering/basic.cpp
  metering/clipping.cpp
  metering/dynamic_range.cpp
  metering/lufs.cpp
  metering/phase_scope.cpp
  metering/spectrum.cpp
  metering/spectrogram.cpp
  metering/stereo.cpp
  metering/true_peak.cpp
  analysis/key_profiles.cpp
  analysis/chord_templates.cpp
  analysis/progression_patterns.cpp
  analysis/acoustic_analyzer.cpp
  analysis/bpm_analyzer.cpp
  analysis/key_analyzer.cpp
  analysis/onset_analyzer.cpp
  analysis/dynamics_analyzer.cpp
  analysis/beat_analyzer.cpp
  analysis/meter_analyzer.cpp
  analysis/chord_analyzer.cpp
  analysis/chord_hmm.cpp
  analysis/downbeat_analyzer.cpp
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
  sonare_c_editing.cpp
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

# Core object library shared by the static archive and the shared-language
# binding target, so BUILD_SHARED does not recompile the same core sources.
add_library(sonare_core_objects OBJECT ${SONARE_SOURCES})
set_target_properties(sonare_core_objects PROPERTIES POSITION_INDEPENDENT_CODE ON)

target_include_directories(sonare_core_objects PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}
)
target_include_directories(sonare_core_objects SYSTEM PUBLIC
  ${CMAKE_SOURCE_DIR}/third_party/kissfft
  ${CMAKE_SOURCE_DIR}/third_party/dr_libs
  ${CMAKE_SOURCE_DIR}/third_party/minimp3
  ${CMAKE_SOURCE_DIR}/third_party/r8brain
)
target_link_libraries(sonare_core_objects PUBLIC Eigen3::Eigen)
target_compile_options(sonare_core_objects PRIVATE ${SONARE_WARNING_FLAGS})

add_library(sonare_core STATIC $<TARGET_OBJECTS:sonare_core_objects>)

target_include_directories(sonare_core PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}
)
target_include_directories(sonare_core SYSTEM PUBLIC
  ${CMAKE_SOURCE_DIR}/third_party/kissfft
  ${CMAKE_SOURCE_DIR}/third_party/dr_libs
  ${CMAKE_SOURCE_DIR}/third_party/minimp3
  ${CMAKE_SOURCE_DIR}/third_party/r8brain
)

target_link_libraries(sonare_core PUBLIC Eigen3::Eigen kissfft)

# Threading support for parallel analysis on native builds
if(NOT BUILD_WASM)
  find_package(Threads REQUIRED)
  target_link_libraries(sonare_core PUBLIC Threads::Threads)
endif()

if(SONARE_FFMPEG_ENABLED)
  target_link_libraries(sonare_core PUBLIC PkgConfig::FFMPEG)
  target_compile_definitions(sonare_core_objects PUBLIC SONARE_WITH_FFMPEG=1)
  target_compile_definitions(sonare_core PUBLIC SONARE_WITH_FFMPEG=1)
endif()

add_library(sonare_rt STATIC
  rt/biquad_design.cpp
  rt/delay_line.cpp
  rt/envelope_follower.cpp
  rt/lookahead_buffer.cpp
  rt/oversampler.cpp
  rt/param_smoother.cpp
  rt/partitioned_convolver.cpp
  rt/true_peak_filter.cpp
  transport/marker.cpp
  transport/musical_time.cpp
  transport/tempo_map.cpp
  transport/transport.cpp
)
target_include_directories(sonare_rt PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}
)
target_include_directories(sonare_rt SYSTEM PUBLIC
  ${CMAKE_SOURCE_DIR}/third_party/kissfft
  ${CMAKE_SOURCE_DIR}/third_party/dr_libs
  ${CMAKE_SOURCE_DIR}/third_party/minimp3
  ${CMAKE_SOURCE_DIR}/third_party/r8brain
)
target_link_libraries(sonare_rt PUBLIC sonare_core)
target_compile_options(sonare_rt PRIVATE ${SONARE_WARNING_FLAGS})
set_target_properties(sonare_rt PROPERTIES POSITION_INDEPENDENT_CODE ON)

add_library(sonare_automation STATIC
  automation/automation_engine.cpp
  automation/automation_lane.cpp
  automation/parameter.cpp
)
target_include_directories(sonare_automation PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}
)
target_include_directories(sonare_automation SYSTEM PUBLIC
  ${CMAKE_SOURCE_DIR}/third_party/kissfft
  ${CMAKE_SOURCE_DIR}/third_party/dr_libs
  ${CMAKE_SOURCE_DIR}/third_party/minimp3
  ${CMAKE_SOURCE_DIR}/third_party/r8brain
)
target_link_libraries(sonare_automation PUBLIC sonare_rt)
target_compile_options(sonare_automation PRIVATE ${SONARE_WARNING_FLAGS})
set_target_properties(sonare_automation PROPERTIES POSITION_INDEPENDENT_CODE ON)

if(BUILD_MASTERING)
  add_library(sonare_mastering STATIC
    mastering/api/chain.cpp
    mastering/api/chain_json.cpp
    mastering/api/chain_params.cpp
    mastering/api/chain_streaming.cpp
    mastering/api/insert_factory.cpp
    mastering/api/named_processor.cpp
    mastering/api/named_processor_registry.cpp
    mastering/api/presets.cpp
    mastering/assistant/audio_profile.cpp
    mastering/assistant/suggester.cpp
    mastering/common/hysteresis_ja.cpp
    mastering/common/noise_tracker.cpp
    mastering/dynamics/brickwall_limiter.cpp
    mastering/dynamics/compressor.cpp
    mastering/dynamics/deesser.cpp
    mastering/dynamics/ducking_processor.cpp
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
    mastering/eq/equalizer.cpp
    mastering/eq/equalizer_dynamic.cpp
    mastering/eq/equalizer_routing.cpp
    mastering/eq/equalizer_spectrum.cpp
    mastering/eq/equalizer_validation.cpp
    mastering/eq/graphic_eq.cpp
    mastering/eq/linear_phase.cpp
    mastering/eq/mid_side_eq.cpp
    mastering/eq/minimum_phase.cpp
    mastering/eq/parametric.cpp
    mastering/eq/spectrum_engine.cpp
    mastering/eq/spectrum_registry.cpp
    mastering/eq/pultec.cpp
    mastering/eq/shelving.cpp
    mastering/eq/tilt.cpp
  )
  target_include_directories(sonare_mastering PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
  )
  target_include_directories(sonare_mastering SYSTEM PUBLIC
    ${CMAKE_SOURCE_DIR}/third_party/kissfft
    ${CMAKE_SOURCE_DIR}/third_party/dr_libs
    ${CMAKE_SOURCE_DIR}/third_party/minimp3
    ${CMAKE_SOURCE_DIR}/third_party/r8brain
  )
  target_link_libraries(sonare_mastering PUBLIC sonare_rt)
  if(BUILD_FX)
    # Streaming insert factory can build effects.reverb.* processors from sonare_fx.
    target_link_libraries(sonare_mastering PUBLIC sonare_fx)
    target_compile_definitions(sonare_mastering PRIVATE SONARE_HAVE_FX)
  endif()
  target_compile_options(sonare_mastering PRIVATE ${SONARE_WARNING_FLAGS})
  set_target_properties(sonare_mastering PROPERTIES POSITION_INDEPENDENT_CODE ON)
endif()

if(BUILD_MIXING)
  add_library(sonare_mixing STATIC
    mixing/alignment_delay.cpp
    mixing/api/presets.cpp
    mixing/api/scene_json.cpp
    mixing/automation_lane.cpp
    mixing/bus.cpp
    mixing/channel_strip.cpp
    mixing/fx_bus.cpp
    mixing/gain.cpp
    mixing/meter.cpp
    mixing/mixer_controller.cpp
    mixing/panner.cpp
    mixing/send.cpp
    mixing/stereo_width.cpp
    mixing/vca_group.cpp
  )
  target_include_directories(sonare_mixing PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
  )
  target_include_directories(sonare_mixing SYSTEM PUBLIC
    ${CMAKE_SOURCE_DIR}/third_party/kissfft
    ${CMAKE_SOURCE_DIR}/third_party/dr_libs
    ${CMAKE_SOURCE_DIR}/third_party/minimp3
    ${CMAKE_SOURCE_DIR}/third_party/r8brain
  )
  target_link_libraries(sonare_mixing PUBLIC sonare_rt sonare_mastering)
  target_compile_options(sonare_mixing PRIVATE ${SONARE_WARNING_FLAGS})
  set_target_properties(sonare_mixing PROPERTIES POSITION_INDEPENDENT_CODE ON)
endif()

if(BUILD_GRAPH)
  add_library(sonare_graph STATIC
    graph/graph.cpp
    graph/node.cpp
  )
  target_include_directories(sonare_graph PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
  )
  target_include_directories(sonare_graph SYSTEM PUBLIC
    ${CMAKE_SOURCE_DIR}/third_party/kissfft
    ${CMAKE_SOURCE_DIR}/third_party/dr_libs
    ${CMAKE_SOURCE_DIR}/third_party/minimp3
    ${CMAKE_SOURCE_DIR}/third_party/r8brain
  )
  target_link_libraries(sonare_graph PUBLIC sonare_rt)
  target_compile_options(sonare_graph PRIVATE ${SONARE_WARNING_FLAGS})
  set_target_properties(sonare_graph PROPERTIES POSITION_INDEPENDENT_CODE ON)
endif()

set(SONARE_ENGINE_SOURCES
  engine/boundary_splitter.cpp
  engine/capture.cpp
  engine/clip_player.cpp
  engine/engine_controller.cpp
  engine/metronome.cpp
  engine/realtime_engine.cpp
  engine/tempo_sync.cpp
)
if(BUILD_GRAPH)
  list(APPEND SONARE_ENGINE_SOURCES engine/graph_runtime.cpp)
endif()
if(BUILD_MIXING)
  list(APPEND SONARE_ENGINE_SOURCES
    engine/meter_telemetry.cpp
    engine/mixing_runtime.cpp
    engine/monitor_runtime.cpp
  )
endif()
add_library(sonare_engine STATIC ${SONARE_ENGINE_SOURCES})
target_include_directories(sonare_engine PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}
)
target_include_directories(sonare_engine SYSTEM PUBLIC
  ${CMAKE_SOURCE_DIR}/third_party/kissfft
  ${CMAKE_SOURCE_DIR}/third_party/dr_libs
  ${CMAKE_SOURCE_DIR}/third_party/minimp3
  ${CMAKE_SOURCE_DIR}/third_party/r8brain
)
target_link_libraries(sonare_engine PUBLIC sonare_rt sonare_automation)
if(BUILD_GRAPH)
  target_link_libraries(sonare_engine PUBLIC sonare_graph)
  target_compile_definitions(sonare_engine PUBLIC SONARE_WITH_GRAPH=1)
endif()
if(BUILD_MIXING)
  target_link_libraries(sonare_engine PUBLIC sonare_mixing)
  target_compile_definitions(sonare_engine PUBLIC SONARE_WITH_MIXING=1)
endif()
target_compile_options(sonare_engine PRIVATE ${SONARE_WARNING_FLAGS})
set_target_properties(sonare_engine PROPERTIES POSITION_INDEPENDENT_CODE ON)

if(BUILD_FX)
  add_library(sonare_fx STATIC
    effects/common/dc_blocker.cpp
    effects/delay/stereo_delay.cpp
    effects/modulation/chorus.cpp
    effects/modulation/flanger.cpp
    effects/modulation/mod_delay_line.cpp
    effects/modulation/phaser.cpp
    effects/reverb/convolution_reverb.cpp
    effects/reverb/dattorro_reverb.cpp
    effects/reverb/fdn_reverb.cpp
    effects/reverb/velvet_reverb.cpp
  )
  target_include_directories(sonare_fx PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
  )
  target_include_directories(sonare_fx SYSTEM PUBLIC
    ${CMAKE_SOURCE_DIR}/third_party/kissfft
    ${CMAKE_SOURCE_DIR}/third_party/dr_libs
    ${CMAKE_SOURCE_DIR}/third_party/minimp3
    ${CMAKE_SOURCE_DIR}/third_party/r8brain
  )
  target_link_libraries(sonare_fx PUBLIC sonare_rt)
  target_compile_options(sonare_fx PRIVATE ${SONARE_WARNING_FLAGS})
  set_target_properties(sonare_fx PROPERTIES POSITION_INDEPENDENT_CODE ON)
endif()

if(BUILD_PITCH_EDITOR)
  add_library(sonare_pitch_editor STATIC
    editing/pitch_editor/f0_provider.cpp
    editing/pitch_editor/note_editor.cpp
    editing/pitch_editor/note_segmenter.cpp
    editing/pitch_editor/pitch_corrector.cpp
    editing/pitch_editor/scale_quantizer.cpp
  )
  target_include_directories(sonare_pitch_editor PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
  )
  target_include_directories(sonare_pitch_editor SYSTEM PUBLIC
    ${CMAKE_SOURCE_DIR}/third_party/kissfft
    ${CMAKE_SOURCE_DIR}/third_party/dr_libs
    ${CMAKE_SOURCE_DIR}/third_party/minimp3
    ${CMAKE_SOURCE_DIR}/third_party/r8brain
  )
  target_link_libraries(sonare_pitch_editor PUBLIC sonare_core)
  target_compile_options(sonare_pitch_editor PRIVATE ${SONARE_WARNING_FLAGS})
  set_target_properties(sonare_pitch_editor PROPERTIES POSITION_INDEPENDENT_CODE ON)
endif()

if(BUILD_VOICE_CHANGER AND NOT BUILD_PITCH_EDITOR)
  message(FATAL_ERROR "BUILD_VOICE_CHANGER requires BUILD_PITCH_EDITOR for C API helpers")
endif()

if(BUILD_VOICE_CHANGER)
  add_library(sonare_voice_changer STATIC
    editing/voice_changer/formant_warp.cpp
    editing/voice_changer/isp_limiter.cpp
    editing/voice_changer/realtime_voice_changer.cpp
    editing/voice_changer/streaming_formant.cpp
    editing/voice_changer/streaming_retune.cpp
    editing/voice_changer/streaming_reverb.cpp
    editing/voice_changer/voice_changer.cpp
  )
  target_include_directories(sonare_voice_changer PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
  )
  target_include_directories(sonare_voice_changer SYSTEM PUBLIC
    ${CMAKE_SOURCE_DIR}/third_party/kissfft
    ${CMAKE_SOURCE_DIR}/third_party/dr_libs
    ${CMAKE_SOURCE_DIR}/third_party/minimp3
    ${CMAKE_SOURCE_DIR}/third_party/r8brain
  )
  target_link_libraries(sonare_voice_changer PUBLIC sonare_core sonare_pitch_editor)
  target_compile_options(sonare_voice_changer PRIVATE ${SONARE_WARNING_FLAGS})
  set_target_properties(sonare_voice_changer PROPERTIES POSITION_INDEPENDENT_CODE ON)
endif()

# Shared library for language bindings (Python ctypes, etc.)
if(BUILD_SHARED)
  set(SONARE_SHARED_SOURCES $<TARGET_OBJECTS:sonare_core_objects>)
  if(BUILD_MASTERING)
    list(APPEND SONARE_SHARED_SOURCES sonare_c_mastering.cpp sonare_c_mastering_repair.cpp
                                      sonare_c_mastering_dynamics.cpp)
  endif()
  if(BUILD_MIXING)
    list(APPEND SONARE_SHARED_SOURCES sonare_c_mixing.cpp)
  endif()
  list(APPEND SONARE_SHARED_SOURCES sonare_c_daw.cpp)
  add_library(sonare_shared SHARED ${SONARE_SHARED_SOURCES})
  set_target_properties(sonare_shared PROPERTIES
    OUTPUT_NAME sonare
    VERSION ${PROJECT_VERSION}
    SOVERSION 1
    POSITION_INDEPENDENT_CODE ON
  )
  target_include_directories(sonare_shared PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
  )
  target_include_directories(sonare_shared SYSTEM PUBLIC
    ${CMAKE_SOURCE_DIR}/third_party/kissfft
    ${CMAKE_SOURCE_DIR}/third_party/dr_libs
    ${CMAKE_SOURCE_DIR}/third_party/minimp3
    ${CMAKE_SOURCE_DIR}/third_party/r8brain
  )
  target_link_libraries(sonare_shared PUBLIC Eigen3::Eigen)
  if(BUILD_MASTERING AND NOT BUILD_MIXING)
    target_link_libraries(sonare_shared PUBLIC sonare_mastering)
  endif()
  if(BUILD_MASTERING)
    target_compile_definitions(sonare_shared PUBLIC SONARE_WITH_MASTERING=1)
  endif()
  target_link_libraries(sonare_shared PUBLIC sonare_engine)
  if(BUILD_PITCH_EDITOR AND NOT BUILD_VOICE_CHANGER)
    target_link_libraries(sonare_shared PUBLIC sonare_pitch_editor)
  endif()
  if(BUILD_PITCH_EDITOR)
    target_compile_definitions(sonare_shared PUBLIC SONARE_WITH_PITCH_EDITOR=1)
  endif()
  if(BUILD_VOICE_CHANGER)
    target_link_libraries(sonare_shared PUBLIC sonare_voice_changer)
    target_compile_definitions(sonare_shared PUBLIC SONARE_WITH_VOICE_CHANGER=1)
  endif()
  if(BUILD_MIXING)
    target_compile_definitions(sonare_shared PUBLIC SONARE_WITH_MIXING=1)
    if(BUILD_GRAPH)
      # sonare_c_mixing.cpp routes the mixer through the routing graph.
      target_compile_definitions(sonare_shared PUBLIC SONARE_WITH_GRAPH=1)
    endif()
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
  # Enable WebAssembly SIMD128 across the whole WASM build (core, mastering,
  # binding shim). -msimd128 must be set both on compile and link for Eigen
  # vectorization to actually map onto v128 ops in the emitted .wasm.
  target_compile_options(sonare_core PUBLIC -msimd128)
  target_compile_options(sonare_rt PUBLIC -msimd128)
  target_compile_options(sonare_automation PUBLIC -msimd128)
  target_compile_options(sonare_engine PUBLIC -msimd128)
  if(BUILD_MASTERING)
    target_compile_options(sonare_mastering PUBLIC -msimd128)
  endif()
  if(BUILD_MIXING)
    target_compile_options(sonare_mixing PUBLIC -msimd128)
  endif()
  if(BUILD_GRAPH)
    target_compile_options(sonare_graph PUBLIC -msimd128)
  endif()
  if(BUILD_FX)
    target_compile_options(sonare_fx PUBLIC -msimd128)
  endif()
  if(BUILD_PITCH_EDITOR)
    target_compile_options(sonare_pitch_editor PUBLIC -msimd128)
  endif()
  if(BUILD_VOICE_CHANGER)
    target_compile_options(sonare_voice_changer PUBLIC -msimd128)
  endif()

  target_compile_options(sonare_core PUBLIC -matomics -mbulk-memory)
  target_compile_options(sonare_rt PUBLIC -matomics -mbulk-memory)
  target_compile_options(sonare_automation PUBLIC -matomics -mbulk-memory)
  target_compile_options(sonare_engine PUBLIC -matomics -mbulk-memory)
  if(BUILD_MASTERING)
    target_compile_options(sonare_mastering PUBLIC -matomics -mbulk-memory)
  endif()
  if(BUILD_MIXING)
    target_compile_options(sonare_mixing PUBLIC -matomics -mbulk-memory)
  endif()
  if(BUILD_GRAPH)
    target_compile_options(sonare_graph PUBLIC -matomics -mbulk-memory)
  endif()
  if(BUILD_FX)
    target_compile_options(sonare_fx PUBLIC -matomics -mbulk-memory)
  endif()
  if(BUILD_PITCH_EDITOR)
    target_compile_options(sonare_pitch_editor PUBLIC -matomics -mbulk-memory)
  endif()
  if(BUILD_VOICE_CHANGER)
    target_compile_options(sonare_voice_changer PUBLIC -matomics -mbulk-memory)
  endif()

  add_executable(sonare wasm/bindings.cpp)
  target_link_libraries(sonare PRIVATE sonare_core sonare_engine)
  if(BUILD_PITCH_EDITOR)
    target_link_libraries(sonare PRIVATE sonare_pitch_editor)
  endif()
  if(BUILD_VOICE_CHANGER)
    target_link_libraries(sonare PRIVATE sonare_voice_changer)
  endif()
  if(BUILD_MASTERING)
    target_link_libraries(sonare PRIVATE sonare_mastering)
    target_compile_definitions(sonare PRIVATE SONARE_WITH_MASTERING=1)
  else()
    target_sources(sonare PRIVATE mastering/maximizer/loudness_optimize.cpp)
  endif()
  if(BUILD_MIXING)
    target_link_libraries(sonare PRIVATE sonare_mixing)
    target_compile_definitions(sonare PRIVATE SONARE_WITH_MIXING=1)
    if(BUILD_GRAPH)
      # The mixer C API (sonare_c_mixing.cpp) routes strips through the routing
      # graph, so the WASM MixerWasm binding needs sonare_graph linked and the
      # C API translation unit compiled in.
      target_link_libraries(sonare PRIVATE sonare_graph)
      target_compile_definitions(sonare PRIVATE SONARE_WITH_GRAPH=1)
      target_sources(sonare PRIVATE sonare_c_mixing.cpp)
    endif()
  endif()
  target_compile_options(sonare PRIVATE -msimd128)
  target_link_options(sonare PRIVATE
    -msimd128
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

  add_executable(sonare-rt wasm/rt_bindings.cpp)
  target_link_libraries(sonare-rt PRIVATE sonare_engine)
  if(BUILD_GRAPH)
    target_link_libraries(sonare-rt PRIVATE sonare_graph)
    target_compile_definitions(sonare-rt PRIVATE SONARE_WITH_GRAPH=1)
  endif()
  if(BUILD_MIXING)
    target_link_libraries(sonare-rt PRIVATE sonare_mixing)
    target_compile_definitions(sonare-rt PRIVATE SONARE_WITH_MIXING=1)
  endif()
  target_compile_options(sonare-rt PRIVATE -msimd128)
  target_link_options(sonare-rt PRIVATE
    -msimd128
    -sEXPORT_NAME='SonareRt'
    -sEXPORTED_FUNCTIONS=['_malloc','_free','_sonare_rt_engine_abi_version','_sonare_rt_engine_create','_sonare_rt_engine_destroy','_sonare_rt_engine_prepare','_sonare_rt_engine_play','_sonare_rt_engine_stop','_sonare_rt_engine_seek_sample','_sonare_rt_engine_seek_ppq','_sonare_rt_engine_set_tempo','_sonare_rt_engine_set_loop','_sonare_rt_engine_seek_marker','_sonare_rt_engine_set_metronome_enabled','_sonare_rt_engine_set_capture_armed','_sonare_rt_engine_set_capture_punch','_sonare_rt_engine_process','_sonare_rt_engine_drain_telemetry']
    -sMODULARIZE=1
    -sEXPORT_ES6=1
    -sALLOW_MEMORY_GROWTH=0
    -sINITIAL_MEMORY=67108864
    -sSTACK_SIZE=1048576
    -sWASM_WORKERS=1
    -sAUDIO_WORKLET=1
  )
  add_custom_command(TARGET sonare-rt POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_SOURCE_DIR}/bindings/wasm/dist
    COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_FILE:sonare-rt> ${CMAKE_SOURCE_DIR}/bindings/wasm/dist/
    COMMAND ${CMAKE_COMMAND} -E copy ${CMAKE_BINARY_DIR}/bin/sonare-rt.wasm ${CMAKE_SOURCE_DIR}/bindings/wasm/dist/
    COMMENT "Copying realtime WASM files to bindings/wasm/dist/"
  )
endif()
