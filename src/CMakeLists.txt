# KissFFT library (third-party, warnings disabled)
add_library(kissfft STATIC
  ${PROJECT_SOURCE_DIR}/third_party/kissfft/kiss_fft.c
  ${PROJECT_SOURCE_DIR}/third_party/kissfft/kiss_fftr.c
)
set_target_properties(kissfft PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_include_directories(kissfft SYSTEM PUBLIC ${PROJECT_SOURCE_DIR}/third_party/kissfft)
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
  util/tunable.cpp
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
  # The base layer owns the shared coefficient designers: filters/ and metering/
  # call them just as the realtime layer does, and both files are pure
  # design-time coefficient/table computation over util/ only. Compiling them
  # with sonare_rt instead would make sonare_core depend upward on sonare_rt,
  # which fails to link once the feature libraries that happen to pull in both
  # are switched off. The headers stay under rt/, and sonare_rt links
  # sonare_core, so every existing include keeps resolving.
  rt/biquad_design.cpp
  rt/true_peak_fir.cpp
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
  effects/spectral_edit.cpp
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
  metering/stereo.cpp
  metering/true_peak.cpp
  metering/waveform.cpp
  mixing/api/scene_json.cpp
  analysis/key_profiles.cpp
  analysis/chord_templates.cpp
  analysis/progression_patterns.cpp
  analysis/acoustic_analyzer.cpp
  analysis/acoustic/common.cpp
  analysis/acoustic/decay.cpp
  analysis/acoustic/ir.cpp
  analysis/acoustic/subband.cpp
  analysis/bpm_analyzer.cpp
  analysis/key_analyzer.cpp
  analysis/onset_analyzer.cpp
  analysis/dynamics_analyzer.cpp
  analysis/beat_analyzer.cpp
  analysis/meter_analyzer.cpp
  analysis/tempo_curve.cpp
  analysis/chord_analyzer.cpp
  analysis/chord_hmm.cpp
  analysis/downbeat_analyzer.cpp
  analysis/boundary_detector.cpp
  analysis/rhythm_analyzer.cpp
  analysis/timbre_analyzer.cpp
  analysis/melody_analyzer.cpp
  analysis/section_analyzer.cpp
  analysis/music_analyzer.cpp
  analysis/analysis_json.cpp
  streaming/stream_analyzer.cpp
  streaming/stream_analyzer_features.cpp
  streaming/stream_analyzer_output.cpp
  streaming/stream_analyzer_processing.cpp
  streaming/stream_analyzer_progression.cpp
  streaming/stream_analyzer_utils.cpp
  streaming/stream_resampler.cpp
  quick.cpp
  c_api/sonare_c.cpp
  # Compiled here rather than with the mastering module: eq_band_json.cpp below
  # is always compiled (sonare_c_engine_mixing.cpp needs it in every shared
  # build) and calls these parsers, so leaving them behind a feature gate makes
  # -DBUILD_MASTERING=OFF -DBUILD_MIXING=OFF fail at link. The file is a pure
  # string<->enum mapping over a header-only enum, with no mastering runtime
  # behind it.
  mastering/eq/band_strings.cpp
  # Compiled here rather than with the mixing module for the same reason:
  # core/audio_io.cpp is always compiled and folds every multichannel source to
  # mono through downmix(), so leaving it behind a feature gate makes
  # -DBUILD_MIXING=OFF fail at link. The file is a pure stateless coefficient
  # matrix over a header-only layout enum, with no mixing runtime behind it.
  mixing/downmix.cpp
  c_api/eq_band_json.cpp
  c_api/core_analysis.cpp
  c_api/core_audio.cpp
  c_api/core_common.cpp
  c_api/core_cqt.cpp
  c_api/core_quick.cpp
  c_api/sonare_c_effects.cpp
  c_api/sonare_c_features.cpp
  c_api/features_common.cpp
  c_api/features_inverse.cpp
  c_api/features_music.cpp
  c_api/features_spectrogram_mel.cpp
  c_api/features_spectral_pitch.cpp
  c_api/features_segment.cpp
  c_api/features_streaming.cpp
  c_api/sonare_c_compat.cpp
  c_api/sonare_c_editing.cpp
  c_api/sonare_c_internal.cpp
  c_api/sonare_c_memory.cpp
)

set(SONARE_C_MIXING_SOURCES
  c_api/sonare_c_mixing.cpp
  c_api/mixing_assistant.cpp
  c_api/mixing_graph.cpp
  c_api/mixing_mixer.cpp
  c_api/mixing_strip.cpp
)

set(SONARE_C_PROJECT_SOURCES
  c_api/sonare_c_project.cpp
  c_api/project_bounce.cpp
  c_api/project_core.cpp
  c_api/project_edit_track.cpp
  c_api/project_edit_clip.cpp
  c_api/project_external_stems.cpp
  c_api/project_midi.cpp
  c_api/project_mir.cpp
  c_api/project_soundfont.cpp
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
  ${CMAKE_CURRENT_SOURCE_DIR} ${PROJECT_SOURCE_DIR}/include
)
target_include_directories(sonare_core_objects SYSTEM PUBLIC
  ${PROJECT_SOURCE_DIR}/third_party/kissfft
  ${PROJECT_SOURCE_DIR}/third_party/dr_libs
  ${PROJECT_SOURCE_DIR}/third_party/minimp3
  ${PROJECT_SOURCE_DIR}/third_party/r8brain
)
target_link_libraries(sonare_core_objects PUBLIC Eigen3::Eigen)
target_compile_options(sonare_core_objects PRIVATE ${SONARE_WARNING_FLAGS})
target_compile_definitions(sonare_core_objects PRIVATE
  SONARE_BUILD_MASTERING=$<IF:$<BOOL:${BUILD_MASTERING}>,1,0>
  SONARE_BUILD_MIXING=$<IF:$<BOOL:${BUILD_MIXING}>,1,0>
  SONARE_BUILD_MIXING_ASSISTANT=$<IF:$<BOOL:${BUILD_MIXING_ASSISTANT}>,1,0>
  SONARE_BUILD_FX=$<IF:$<BOOL:${BUILD_FX}>,1,0>
  # The capability descriptor lives here, so it needs the arrangement flag even
  # though this target does not link the arrangement/MIDI layer (that dependency
  # runs the other way).
  SONARE_BUILD_ARRANGEMENT=$<IF:$<BOOL:${BUILD_ARRANGEMENT}>,1,0>
)

add_library(sonare_core STATIC $<TARGET_OBJECTS:sonare_core_objects>)

target_include_directories(sonare_core PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR} ${PROJECT_SOURCE_DIR}/include
)
target_include_directories(sonare_core SYSTEM PUBLIC
  ${PROJECT_SOURCE_DIR}/third_party/kissfft
  ${PROJECT_SOURCE_DIR}/third_party/dr_libs
  ${PROJECT_SOURCE_DIR}/third_party/minimp3
  ${PROJECT_SOURCE_DIR}/third_party/r8brain
)

target_link_libraries(sonare_core PUBLIC Eigen3::Eigen kissfft)

# Threading support for parallel analysis on native builds
if(NOT BUILD_WASM)
  find_package(Threads REQUIRED)
  target_link_libraries(sonare_core PUBLIC Threads::Threads)
endif()

if(SONARE_FFMPEG_ENABLED)
  target_link_libraries(sonare_core_objects PUBLIC PkgConfig::FFMPEG)
  target_link_libraries(sonare_core PUBLIC PkgConfig::FFMPEG)
  target_compile_definitions(sonare_core_objects PUBLIC SONARE_WITH_FFMPEG=1)
  target_compile_definitions(sonare_core PUBLIC SONARE_WITH_FFMPEG=1)
endif()

add_library(sonare_rt STATIC
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
  ${CMAKE_CURRENT_SOURCE_DIR} ${PROJECT_SOURCE_DIR}/include
)
target_include_directories(sonare_rt SYSTEM PUBLIC
  ${PROJECT_SOURCE_DIR}/third_party/kissfft
  ${PROJECT_SOURCE_DIR}/third_party/dr_libs
  ${PROJECT_SOURCE_DIR}/third_party/minimp3
  ${PROJECT_SOURCE_DIR}/third_party/r8brain
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
  ${CMAKE_CURRENT_SOURCE_DIR} ${PROJECT_SOURCE_DIR}/include
)
target_include_directories(sonare_automation SYSTEM PUBLIC
  ${PROJECT_SOURCE_DIR}/third_party/kissfft
  ${PROJECT_SOURCE_DIR}/third_party/dr_libs
  ${PROJECT_SOURCE_DIR}/third_party/minimp3
  ${PROJECT_SOURCE_DIR}/third_party/r8brain
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
    mastering/common/loudness_measure.cpp
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
    mastering/saturation/amp_physics.cpp
    mastering/saturation/amp_presets.cpp
    mastering/saturation/amp_sim.cpp
    mastering/saturation/cab_ir.cpp
    mastering/saturation/cab_voicing.cpp
    mastering/saturation/bitcrusher.cpp
    mastering/saturation/exciter.cpp
    mastering/saturation/hard_clipper.cpp
    mastering/saturation/multiband_exciter.cpp
    mastering/saturation/soft_clipper.cpp
    mastering/saturation/tape.cpp
    mastering/saturation/tone_stack.cpp
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
    # band_strings.cpp is in the always-compiled core list; see the note there.
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
    ${CMAKE_CURRENT_SOURCE_DIR} ${PROJECT_SOURCE_DIR}/include
  )
  target_include_directories(sonare_mastering SYSTEM PUBLIC
    ${PROJECT_SOURCE_DIR}/third_party/kissfft
    ${PROJECT_SOURCE_DIR}/third_party/dr_libs
    ${PROJECT_SOURCE_DIR}/third_party/minimp3
    ${PROJECT_SOURCE_DIR}/third_party/r8brain
  )
  target_link_libraries(sonare_mastering PUBLIC sonare_rt)
  if(BUILD_FX)
    # Streaming insert factory can build effects.reverb.* processors from sonare_fx.
    target_link_libraries(sonare_mastering PUBLIC sonare_fx)
    target_compile_definitions(sonare_mastering PRIVATE SONARE_HAVE_FX)
    if(BUILD_ACOUSTIC_SIM)
      # The geometry-driven effects.reverb.room insert pulls in the acoustic
      # synthesizer (via sonare_fx's RoomReverb).
      target_link_libraries(sonare_mastering PUBLIC sonare_acoustic)
      target_compile_definitions(sonare_mastering PRIVATE SONARE_HAVE_ACOUSTIC)
    endif()
  endif()
  target_compile_options(sonare_mastering PRIVATE ${SONARE_WARNING_FLAGS})
  set_target_properties(sonare_mastering PROPERTIES POSITION_INDEPENDENT_CODE ON)
endif()

if(BUILD_MIXING)
  add_library(sonare_mixing STATIC
    mixing/alignment_delay.cpp
    mixing/api/presets.cpp
    mixing/automation_lane.cpp
    mixing/bus.cpp
    mixing/channel_strip.cpp
    mixing/fx_bus.cpp
    mixing/gain.cpp
    mixing/meter.cpp
    mixing/panner.cpp
    mixing/send.cpp
    mixing/stereo_width.cpp
    mixing/surround_panner.cpp
  )
  target_include_directories(sonare_mixing PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR} ${PROJECT_SOURCE_DIR}/include
  )
  target_include_directories(sonare_mixing SYSTEM PUBLIC
    ${PROJECT_SOURCE_DIR}/third_party/kissfft
    ${PROJECT_SOURCE_DIR}/third_party/dr_libs
    ${PROJECT_SOURCE_DIR}/third_party/minimp3
    ${PROJECT_SOURCE_DIR}/third_party/r8brain
  )
  target_link_libraries(sonare_mixing PUBLIC sonare_rt sonare_mastering)
  target_compile_options(sonare_mixing PRIVATE ${SONARE_WARNING_FLAGS})
  set_target_properties(sonare_mixing PROPERTIES POSITION_INDEPENDENT_CODE ON)
endif()

# Offline mixing assistant. A separate target rather than more sources in
# sonare_mixing, so the one-way dependency is structural: the assistant reads
# the mixing and mastering types, and nothing in the runtime reaches back into
# it. Dropping BUILD_MIXING_ASSISTANT therefore removes the whole subsystem
# without leaving a dangling reference anywhere else.
if(BUILD_MIXING_ASSISTANT)
  add_library(sonare_mixing_assistant STATIC
    mixing/assistant/decide_balance.cpp
    mixing/assistant/decide_dynamics.cpp
    mixing/assistant/decide_eq.cpp
    mixing/assistant/decide_image.cpp
    mixing/assistant/decide_structure.cpp
    mixing/assistant/gain_staging.cpp
    mixing/assistant/image_occupancy.cpp
    mixing/assistant/masking.cpp
    mixing/assistant/phase_alignment.cpp
    mixing/assistant/scene_delta.cpp
    mixing/assistant/source_class.cpp
    mixing/assistant/source_classifier.cpp
    mixing/assistant/suggester.cpp
    mixing/assistant/track_profile.cpp
  )
  target_include_directories(sonare_mixing_assistant PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR} ${PROJECT_SOURCE_DIR}/include
  )
  target_include_directories(sonare_mixing_assistant SYSTEM PUBLIC
    ${PROJECT_SOURCE_DIR}/third_party/kissfft
    ${PROJECT_SOURCE_DIR}/third_party/dr_libs
    ${PROJECT_SOURCE_DIR}/third_party/minimp3
    ${PROJECT_SOURCE_DIR}/third_party/r8brain
  )
  target_link_libraries(sonare_mixing_assistant PUBLIC sonare_mixing)
  target_compile_options(sonare_mixing_assistant PRIVATE ${SONARE_WARNING_FLAGS})
  # decide_structure.cpp guards its effect-bus section on this. The definition
  # does not travel with the sources — it is set per target — so without this
  # block the guard reads as absent, the reverb and delay returns are compiled
  # away, and every surface links a suggester that silently never proposes an
  # effect bus. Nothing reports that: the section vanishes at preprocessing, so
  # there is no warning, no link error and no failing case, only two tests that
  # pass over an empty scene.
  target_compile_definitions(sonare_mixing_assistant PRIVATE
    SONARE_BUILD_FX=$<IF:$<BOOL:${BUILD_FX}>,1,0>
  )
  set_target_properties(sonare_mixing_assistant PROPERTIES POSITION_INDEPENDENT_CODE ON)
endif()

if(BUILD_GRAPH)
  add_library(sonare_graph STATIC
    graph/graph.cpp
    graph/node.cpp
  )
  target_include_directories(sonare_graph PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR} ${PROJECT_SOURCE_DIR}/include
  )
  target_include_directories(sonare_graph SYSTEM PUBLIC
    ${PROJECT_SOURCE_DIR}/third_party/kissfft
    ${PROJECT_SOURCE_DIR}/third_party/dr_libs
    ${PROJECT_SOURCE_DIR}/third_party/minimp3
    ${PROJECT_SOURCE_DIR}/third_party/r8brain
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
  engine/realtime_engine_lifecycle.cpp
  engine/realtime_engine_commands.cpp
  engine/realtime_engine_instruments.cpp
  engine/realtime_engine_midi.cpp
  engine/realtime_engine_mixing.cpp
  engine/tempo_sync.cpp
  engine/warp_stretch.cpp
)
if(BUILD_GRAPH)
  list(APPEND SONARE_ENGINE_SOURCES engine/graph_runtime.cpp)
endif()
if(BUILD_MIXING)
  list(APPEND SONARE_ENGINE_SOURCES
    engine/meter_telemetry.cpp
    engine/scope_telemetry.cpp
    engine/mixing_runtime.cpp
    engine/monitor_runtime.cpp
    engine/track_mixer.cpp
    engine/track_mixer_params.cpp
    engine/track_mixer_render.cpp
  )
endif()
add_library(sonare_engine STATIC ${SONARE_ENGINE_SOURCES})
target_include_directories(sonare_engine PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR} ${PROJECT_SOURCE_DIR}/include
)
target_include_directories(sonare_engine SYSTEM PUBLIC
  ${PROJECT_SOURCE_DIR}/third_party/kissfft
  ${PROJECT_SOURCE_DIR}/third_party/dr_libs
  ${PROJECT_SOURCE_DIR}/third_party/minimp3
  ${PROJECT_SOURCE_DIR}/third_party/r8brain
)
target_link_libraries(sonare_engine PUBLIC sonare_core sonare_rt sonare_automation)
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
    effects/modulation/auto_wah.cpp
    effects/modulation/chorus.cpp
    effects/modulation/ensemble.cpp
    effects/modulation/flanger.cpp
    effects/modulation/mod_delay_line.cpp
    effects/modulation/phaser.cpp
    effects/modulation/pitch_shifter.cpp
    effects/modulation/ring_modulator.cpp
    effects/modulation/rotary.cpp
    effects/modulation/wah.cpp
    effects/reverb/convolution_reverb.cpp
    effects/reverb/dattorro_reverb.cpp
    effects/reverb/fdn_reverb.cpp
    effects/reverb/velvet_reverb.cpp
  )
  target_include_directories(sonare_fx PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR} ${PROJECT_SOURCE_DIR}/include
  )
  target_include_directories(sonare_fx SYSTEM PUBLIC
    ${PROJECT_SOURCE_DIR}/third_party/kissfft
    ${PROJECT_SOURCE_DIR}/third_party/dr_libs
    ${PROJECT_SOURCE_DIR}/third_party/minimp3
    ${PROJECT_SOURCE_DIR}/third_party/r8brain
  )
  target_link_libraries(sonare_fx PUBLIC sonare_rt)
  if(BUILD_ACOUSTIC_SIM)
    # The geometry-driven RoomReverb synthesizes its RIR via the acoustic module
    # and convolves it with the existing convolution-reverb path; RoomMorph adds
    # source-tail suppression in front of a target-room convolution.
    target_sources(sonare_fx PRIVATE
      effects/reverb/room_reverb.cpp
      effects/acoustic/room_morph.cpp
    )
    target_link_libraries(sonare_fx PUBLIC sonare_acoustic)
    target_compile_definitions(sonare_fx PRIVATE SONARE_HAVE_ACOUSTIC)
  endif()
  target_compile_options(sonare_fx PRIVATE ${SONARE_WARNING_FLAGS})
  set_target_properties(sonare_fx PROPERTIES POSITION_INDEPENDENT_CODE ON)
endif()

if(BUILD_ACOUSTIC_SIM)
  # Geometric room-acoustics: 3D room model -> image-source early reflections +
  # statistical late-reverberation tail -> synthesized RIR (the 5th reverb
  # engine), plus blind room estimation and the room-morph creative FX. The
  # convolution path is pulled in where the synthesized RIR is applied.
  add_library(sonare_acoustic STATIC
    acoustic/room_types.cpp
    acoustic/material.cpp
    acoustic/room_model.cpp
    acoustic/image_source.cpp
    acoustic/late_reverb.cpp
    acoustic/rir_synthesizer.cpp
    analysis/room_estimator.cpp
  )
  target_include_directories(sonare_acoustic PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR} ${PROJECT_SOURCE_DIR}/include
  )
  target_include_directories(sonare_acoustic SYSTEM PUBLIC
    ${PROJECT_SOURCE_DIR}/third_party/kissfft
    ${PROJECT_SOURCE_DIR}/third_party/dr_libs
    ${PROJECT_SOURCE_DIR}/third_party/minimp3
    ${PROJECT_SOURCE_DIR}/third_party/r8brain
  )
  target_link_libraries(sonare_acoustic PUBLIC sonare_rt)
  target_compile_options(sonare_acoustic PRIVATE ${SONARE_WARNING_FLAGS})
  set_target_properties(sonare_acoustic PROPERTIES POSITION_INDEPENDENT_CODE ON)
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
    ${CMAKE_CURRENT_SOURCE_DIR} ${PROJECT_SOURCE_DIR}/include
  )
  target_include_directories(sonare_pitch_editor SYSTEM PUBLIC
    ${PROJECT_SOURCE_DIR}/third_party/kissfft
    ${PROJECT_SOURCE_DIR}/third_party/dr_libs
    ${PROJECT_SOURCE_DIR}/third_party/minimp3
    ${PROJECT_SOURCE_DIR}/third_party/r8brain
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
    editing/voice_changer/realtime/config.cpp
    editing/voice_changer/realtime/processing.cpp
    editing/voice_changer/streaming_formant.cpp
    editing/voice_changer/streaming_retune.cpp
    editing/voice_changer/streaming_reverb.cpp
    editing/voice_changer/voice_changer.cpp
  )
  target_include_directories(sonare_voice_changer PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR} ${PROJECT_SOURCE_DIR}/include
  )
  target_include_directories(sonare_voice_changer SYSTEM PUBLIC
    ${PROJECT_SOURCE_DIR}/third_party/kissfft
    ${PROJECT_SOURCE_DIR}/third_party/dr_libs
    ${PROJECT_SOURCE_DIR}/third_party/minimp3
    ${PROJECT_SOURCE_DIR}/third_party/r8brain
  )
  target_link_libraries(sonare_voice_changer PUBLIC sonare_core sonare_pitch_editor)
  target_compile_options(sonare_voice_changer PRIVATE ${SONARE_WARNING_FLAGS})
  set_target_properties(sonare_voice_changer PROPERTIES POSITION_INDEPENDENT_CODE ON)
endif()

if(BUILD_ARRANGEMENT)
  # Headless arrangement / DAW project model (control-thread-only, value data).
  # Depends one-way on transport/ + automation/ (via sonare_automation, which
  # transitively links sonare_rt -> sonare_core) and includes the pure-data
  # mixing::api::Scene struct header. The Scene STRUCT is header-only data, so
  # sonare_mixing is NOT linked here; mixer JSON helpers stay optional, and the include
  # path resolves through sonare_core's src/ include dir even when BUILD_MIXING
  # is OFF. No core/engine/rt target depends on sonare_arrangement (one-way).
  # MIDI core (UMP / clip / sequencer). RT-layer subsystem: depends ONLY on
  # rt/ + transport/ (both compiled into sonare_rt). engine/ may include this
  # (RealtimeEngine gains a flag-gated MidiSequencer member); arrangement/
  # compiles to midi::MidiClipSchedule. One-way: midi does NOT depend on
  # engine/ or arrangement/, so the link graph stays acyclic
  # (rt/transport <- midi <- engine <- arrangement; arrangement -> midi too).
  add_library(sonare_midi STATIC
    midi/ump.cpp
    midi/midi_clip.cpp
    midi/sequencer.cpp
    midi/smf.cpp
    midi/smf2.cpp
    midi/midi_fx.cpp
    midi/program_map.cpp
    midi/sound_destination.cpp
    midi/routing.cpp
    midi/cc_map.cpp
    midi/clock_sync.cpp
    midi/capture.cpp
    midi/builtin_synth.cpp
    midi/synth/envelope.cpp
    midi/synth/oscillator.cpp
    midi/synth/filter_models.cpp
    midi/synth/mod_matrix.cpp
    midi/synth/patch_tuning.cpp
    midi/synth/fm_voice.cpp
    midi/synth/ks_voice.cpp
    midi/synth/modal_voice.cpp
    midi/synth/additive_voice.cpp
    midi/synth/percussion_voice.cpp
    midi/synth/piano_voice.cpp
    midi/synth/piano_resonance.cpp
    midi/synth/pipe_organ_voice.cpp
    midi/synth/bowed_string_voice.cpp
    midi/synth/reed_voice.cpp
    midi/synth/brass_voice.cpp
    midi/synth/flute_voice.cpp
    midi/synth/harpsichord_voice.cpp
    midi/synth/plucked_string_voice.cpp
    midi/synth/vocal_voice.cpp
    midi/synth/free_reed_voice.cpp
    midi/synth/native_synth_patch.cpp
    midi/synth/native_synth_params.cpp
    midi/synth/native_synth_voice.cpp
    midi/synth/native_synth_live_control.cpp
    midi/synth/native_synth.cpp
    midi/synth/gm_fallback_map.cpp
    midi/synth/gm_fallback_families.cpp
    midi/synth/gm_fallback_programs.cpp
    midi/synth/gm_fallback_drums.cpp
    midi/synth/synth_presets.cpp
    midi/synth/sf2_file.cpp
    midi/synth/sf2_voice.cpp
    midi/synth/sf2_player.cpp
    midi/synth/sf2_player_control.cpp
    midi/synth/sf2_player_midi.cpp
    midi/synth/sf2_player_render.cpp
    midi/synth/gs_layer.cpp
  )
  target_include_directories(sonare_midi PUBLIC ${CMAKE_CURRENT_SOURCE_DIR} ${PROJECT_SOURCE_DIR}/include)
  target_link_libraries(sonare_midi PUBLIC sonare_rt sonare_core sonare_automation)
  # The GM fallback tables are constant-initialised so they land in the data
  # section instead of being assembled by start-up code. Clamping a couple of
  # hundred patches field by field runs past the default constant-evaluation
  # budget, which is a front-end resource ceiling rather than a language limit:
  # raising it changes only how long the compiler is allowed to fold.
  set(SONARE_CONSTEXPR_TABLE_SOURCES
    midi/synth/gm_fallback_families.cpp
    midi/synth/gm_fallback_programs.cpp
    midi/synth/gm_fallback_drums.cpp
  )
  if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    set_source_files_properties(${SONARE_CONSTEXPR_TABLE_SOURCES}
      PROPERTIES COMPILE_OPTIONS "-fconstexpr-steps=100000000")
  elseif(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    set_source_files_properties(${SONARE_CONSTEXPR_TABLE_SOURCES}
      PROPERTIES COMPILE_OPTIONS "-fconstexpr-ops-limit=1000000000")
  elseif(MSVC)
    set_source_files_properties(${SONARE_CONSTEXPR_TABLE_SOURCES}
      PROPERTIES COMPILE_OPTIONS "/constexpr:steps100000000")
  endif()
  if(BUILD_FX)
    # GS system effects (reverb/chorus/delay send-returns) reuse the FX suite;
    # without BUILD_FX the SF2 player renders dry (sends become no-ops).
    target_sources(sonare_midi PRIVATE midi/synth/gs_effects.cpp)
    target_link_libraries(sonare_midi PUBLIC sonare_fx)
    target_compile_definitions(sonare_midi PUBLIC SONARE_MIDI_WITH_FX=1)
  endif()
  target_compile_options(sonare_midi PRIVATE ${SONARE_WARNING_FLAGS})
  set_target_properties(sonare_midi PROPERTIES POSITION_INDEPENDENT_CODE ON)
  target_compile_definitions(sonare_midi PUBLIC SONARE_WITH_ARRANGEMENT=1)

  # RealtimeEngine gains a flag-gated MidiSequencer member + set_midi_clips when
  # the arrangement subsystem is built. Wire the dependency + the
  # SONARE_WITH_ARRANGEMENT define onto the already-created sonare_engine target
  # (PUBLIC so every consumer that links engine sees the matching layout).
  target_link_libraries(sonare_engine PUBLIC sonare_midi)
  target_compile_definitions(sonare_engine PUBLIC SONARE_WITH_ARRANGEMENT=1)

  add_library(sonare_arrangement STATIC
    arrangement/edit_model.cpp
    arrangement/external_stems.cpp
    arrangement/edit_command_track.cpp
    arrangement/edit_command_clip.cpp
    arrangement/edit_command_source_timeline.cpp
    arrangement/edit_command_midi_assist_internal.cpp
    arrangement/retained_bytes.cpp
    arrangement/edit_history.cpp
    arrangement/edit_compiler.cpp
  )
  target_include_directories(sonare_arrangement PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR} ${PROJECT_SOURCE_DIR}/include
  )
  target_include_directories(sonare_arrangement SYSTEM PUBLIC
    ${PROJECT_SOURCE_DIR}/third_party/kissfft
    ${PROJECT_SOURCE_DIR}/third_party/dr_libs
    ${PROJECT_SOURCE_DIR}/third_party/minimp3
    ${PROJECT_SOURCE_DIR}/third_party/r8brain
  )
  # compiler links engine (engine::ClipSchedule / RealtimeEngine target
  # types) and core (the fixed sonare::resample for source sample-rate
  # conversion). engine PUBLIC-propagates SONARE_WITH_GRAPH / SONARE_WITH_MIXING,
  # so the compiler's flag guards match the build. This is a ONE-WAY dependency:
  # no core/engine/rt target links sonare_arrangement.
  target_link_libraries(sonare_arrangement PUBLIC sonare_automation sonare_engine sonare_midi sonare_core)
  target_compile_options(sonare_arrangement PRIVATE ${SONARE_WARNING_FLAGS})
  set_target_properties(sonare_arrangement PROPERTIES POSITION_INDEPENDENT_CODE ON)
  target_compile_definitions(sonare_arrangement PUBLIC SONARE_WITH_ARRANGEMENT=1)

  # MIR tempo / grid: OFFLINE, control-plane-only bridge from existing
  # beat/downbeat/BPM/meter analysis to transport::TempoSegment candidates plus
  # pure PPQ grid-snap helpers. Depends one-way on analysis/feature/effects (in
  # sonare_core), transport (in sonare_rt) and the arrangement value model
  # (sonare_arrangement). NOTHING in rt/engine/midi/arrangement depends on mir;
  # the RT-critical CI guard forbids rt/ and engine/ from including mir/.
  add_library(sonare_mir STATIC
    mir/tempo_estimator_bridge.cpp
    mir/grid_snap.cpp
    mir/warp.cpp
    mir/key_context.cpp
  )
  target_include_directories(sonare_mir PUBLIC ${CMAKE_CURRENT_SOURCE_DIR} ${PROJECT_SOURCE_DIR}/include)
  target_link_libraries(sonare_mir PUBLIC sonare_core sonare_rt sonare_arrangement)
  target_compile_options(sonare_mir PRIVATE ${SONARE_WARNING_FLAGS})
  set_target_properties(sonare_mir PROPERTIES POSITION_INDEPENDENT_CODE ON)
  target_compile_definitions(sonare_mir PUBLIC SONARE_WITH_ARRANGEMENT=1)

  # project serializer: OFFLINE / control-plane-only deterministic JSON
  # round-trip of the arrangement Project + MidiContentStore. Depends one-way on
  # arrangement/ + midi/ + mir/ (and core for util::json). NOTHING in
  # rt/engine/midi/arrangement/mir depends on serialize; the RT-critical CI guard
  # forbids rt/ and engine/ from including serialize/. The mixer Scene is embedded
  # through the canonical scene_to_json/scene_from_json helpers in sonare_core,
  # so mixing-ON and mixing-OFF builds use the same schema walker.
  add_library(sonare_serialize STATIC
    serialize/project_serializer.cpp
    serialize/project_serializer_encode.cpp
    serialize/project_serializer_decode.cpp
    serialize/project_serializer_helpers.cpp
  )
  target_include_directories(sonare_serialize PUBLIC ${CMAKE_CURRENT_SOURCE_DIR} ${PROJECT_SOURCE_DIR}/include)
  target_link_libraries(sonare_serialize PUBLIC
    sonare_arrangement sonare_midi sonare_mir sonare_core)
  if(BUILD_MIXING)
    target_link_libraries(sonare_serialize PUBLIC sonare_mixing)
    target_compile_definitions(sonare_serialize PUBLIC SONARE_WITH_MIXING=1)
  endif()
  target_compile_options(sonare_serialize PRIVATE ${SONARE_WARNING_FLAGS})
  set_target_properties(sonare_serialize PROPERTIES POSITION_INDEPENDENT_CODE ON)
  target_compile_definitions(sonare_serialize PUBLIC SONARE_WITH_ARRANGEMENT=1)

  # composition-assist injection seam: OFFLINE / CONTROL-THREAD ONLY. It is a
  # same-build C++ link seam (NOT a stable ABI): a future composition-theory /
  # generation engine implements the abstract module interfaces and is linked in
  # later WITHOUT changing core/runtime. Depends one-way on midi/ + arrangement/
  # + mir/. NOTHING in rt/engine/midi/arrangement/mir depends on assist; the
  # RT-critical guard forbids rt/ and engine/ from including midi/assist/. Gated
  # by BUILD_ASSIST AND BUILD_ARRANGEMENT; defines SONARE_WITH_ASSIST=1 so a
  # no-assist build omits these symbols entirely and the runtime is
  # unaffected.
  if(BUILD_ASSIST)
    add_library(sonare_assist STATIC
      midi/assist/assist_registry.cpp
      midi/assist/composition_assist.cpp
    )
    target_include_directories(sonare_assist PUBLIC ${CMAKE_CURRENT_SOURCE_DIR} ${PROJECT_SOURCE_DIR}/include)
    target_link_libraries(sonare_assist PUBLIC sonare_arrangement sonare_midi sonare_mir)
    target_compile_options(sonare_assist PRIVATE ${SONARE_WARNING_FLAGS})
    set_target_properties(sonare_assist PROPERTIES POSITION_INDEPENDENT_CODE ON)
    target_compile_definitions(sonare_assist PUBLIC SONARE_WITH_ARRANGEMENT=1 SONARE_WITH_ASSIST=1)
  endif()
endif()

# Shared library for language bindings (Python ctypes, etc.)
if(BUILD_SHARED)
  set(SONARE_SHARED_SOURCES $<TARGET_OBJECTS:sonare_core_objects>)
  if(BUILD_MASTERING)
    list(APPEND SONARE_SHARED_SOURCES
         c_api/sonare_c_mastering_apply.cpp
         c_api/sonare_c_mastering_chain.cpp
         c_api/sonare_c_mastering_dynamics.cpp
         c_api/sonare_c_mastering_eq.cpp
         c_api/sonare_c_mastering_preset.cpp
         c_api/sonare_c_mastering_repair.cpp
         c_api/sonare_c_mastering_streaming.cpp)
  endif()
  if(BUILD_MIXING)
    list(APPEND SONARE_SHARED_SOURCES ${SONARE_C_MIXING_SOURCES})
  endif()
  # Always compile the acoustic C ABI translation unit so the public header's
  # symbols exist in feature-off shared builds; it returns NOT_SUPPORTED unless
  # the acoustic + FX modules are both linked.
  list(APPEND SONARE_SHARED_SOURCES c_api/sonare_c_acoustic.cpp)
  list(APPEND SONARE_SHARED_SOURCES c_api/sonare_c_daw.cpp)
  list(APPEND SONARE_SHARED_SOURCES c_api/sonare_c_engine.cpp)
  list(APPEND SONARE_SHARED_SOURCES c_api/sonare_c_engine_midi.cpp)
  list(APPEND SONARE_SHARED_SOURCES c_api/sonare_c_engine_clips.cpp)
  list(APPEND SONARE_SHARED_SOURCES c_api/sonare_c_engine_mixing.cpp)
  list(APPEND SONARE_SHARED_SOURCES c_api/sonare_c_engine_capture.cpp)
  list(APPEND SONARE_SHARED_SOURCES c_api/sonare_c_engine_render.cpp)
  list(APPEND SONARE_SHARED_SOURCES c_api/sonare_c_voice_changer.cpp)
  list(APPEND SONARE_SHARED_SOURCES c_api/sonare_c_streaming_retune.cpp)
  # The project (arrangement) C ABI sources are always compiled so their
  # public symbols exist in feature-off shared builds; it returns NOT_SUPPORTED
  # unless the arrangement subsystem is linked (see the BUILD_ARRANGEMENT block).
  list(APPEND SONARE_SHARED_SOURCES ${SONARE_C_PROJECT_SOURCES})
  add_library(sonare_shared SHARED ${SONARE_SHARED_SOURCES})
  set_target_properties(sonare_shared PROPERTIES
    OUTPUT_NAME sonare
    VERSION ${PROJECT_VERSION}
    SOVERSION 1
    POSITION_INDEPENDENT_CODE ON
  )
  target_include_directories(sonare_shared PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR} ${PROJECT_SOURCE_DIR}/include
  )
  target_include_directories(sonare_shared SYSTEM PUBLIC
    ${PROJECT_SOURCE_DIR}/third_party/kissfft
    ${PROJECT_SOURCE_DIR}/third_party/dr_libs
    ${PROJECT_SOURCE_DIR}/third_party/minimp3
    ${PROJECT_SOURCE_DIR}/third_party/r8brain
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
  if(BUILD_MIXING_ASSISTANT)
    target_link_libraries(sonare_shared PUBLIC sonare_mixing_assistant)
    target_compile_definitions(sonare_shared PUBLIC SONARE_WITH_MIXING_ASSISTANT=1)
  endif()
  if(BUILD_MIXING)
    target_compile_definitions(sonare_shared PUBLIC SONARE_WITH_MIXING=1)
    if(BUILD_GRAPH)
      # The mixing C ABI routes the mixer through the routing graph.
      target_compile_definitions(sonare_shared PUBLIC SONARE_WITH_GRAPH=1)
    endif()
  endif()
  if(BUILD_ACOUSTIC_SIM AND BUILD_FX)
    # The offline acoustic C ABI synthesizes RIRs / estimates rooms / morphs.
    target_link_libraries(sonare_shared PUBLIC sonare_acoustic)
    target_compile_definitions(sonare_shared PUBLIC SONARE_WITH_ACOUSTIC_SIM=1)
  endif()
  if(BUILD_ARRANGEMENT)
    # The headless-project C ABI (sonare_c_project.cpp) wraps the arrangement
    # control plane: EditHistory, the compiler + offline bounce, the
    # serializer, and the MIR tempo/grid bridges. Link them and switch on
    # SONARE_WITH_ARRANGEMENT so the real implementation is compiled.
    target_link_libraries(sonare_shared PUBLIC
      sonare_arrangement sonare_serialize sonare_mir sonare_midi)
    target_compile_definitions(sonare_shared PUBLIC SONARE_WITH_ARRANGEMENT=1)
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
  target_compile_options(sonare_core_objects PUBLIC -msimd128)
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
  if(BUILD_ACOUSTIC_SIM)
    target_compile_options(sonare_acoustic PUBLIC -msimd128)
  endif()
  if(BUILD_PITCH_EDITOR)
    target_compile_options(sonare_pitch_editor PUBLIC -msimd128)
  endif()
  if(BUILD_VOICE_CHANGER)
    target_compile_options(sonare_voice_changer PUBLIC -msimd128)
  endif()

  target_compile_options(sonare_core_objects PUBLIC -matomics -mbulk-memory)
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
  if(BUILD_ACOUSTIC_SIM)
    target_compile_options(sonare_acoustic PUBLIC -matomics -mbulk-memory)
  endif()
  if(BUILD_PITCH_EDITOR)
    target_compile_options(sonare_pitch_editor PUBLIC -matomics -mbulk-memory)
  endif()
  if(BUILD_VOICE_CHANGER)
    target_compile_options(sonare_voice_changer PUBLIC -matomics -mbulk-memory)
  endif()

  if(SONARE_WASM_ANALYSIS_ONLY)
    if(BUILD_MASTERING OR BUILD_MIXING)
      message(FATAL_ERROR
        "SONARE_WASM_ANALYSIS_ONLY requires BUILD_MASTERING=OFF and BUILD_MIXING=OFF")
    endif()
    set(SONARE_WASM_BINDING_SOURCES
      wasm/bindings.cpp
      wasm/bindings/common/common.cpp
      wasm/bindings/analysis/features.cpp
      wasm/bindings/analysis/quick.cpp
      wasm/bindings/analysis/quick_detailed.cpp
      wasm/bindings/features/core.cpp
      wasm/bindings/features/music.cpp
      wasm/bindings/features/pitch.cpp
      wasm/bindings/features/spectral.cpp
      wasm/bindings/features/spectrogram.cpp
      wasm/bindings/metering/metering.cpp
    )
  else()
    set(SONARE_WASM_BINDING_SOURCES
      wasm/bindings.cpp
      wasm/bindings/common/common.cpp
      wasm/bindings/effects/audio.cpp
      c_api/sonare_c_voice_changer.cpp
      c_api/sonare_c_streaming_retune.cpp
      wasm/bindings/features/core.cpp
      wasm/bindings/features/music.cpp
      wasm/bindings/features/pitch.cpp
      wasm/bindings/features/spectral.cpp
      wasm/bindings/features/spectrogram.cpp
      wasm/bindings/metering/metering.cpp
      wasm/bindings/mixing/mixing.cpp
      wasm/bindings/mixing/mixing_assistant.cpp
      wasm/bindings/mixing/mixing_strip.cpp
      wasm/bindings/mixing/mixing_automation.cpp
      wasm/bindings/mixing/mixing_processing.cpp
      wasm/bindings/mixing/mixing_topology.cpp
      wasm/bindings/mastering/api.cpp
      wasm/bindings/mastering/chain.cpp
      wasm/bindings/offline.cpp
      wasm/bindings/effects/offline_dynamics_editing.cpp
      wasm/bindings/project/project.cpp
      wasm/bindings/project/project_arrange.cpp
      wasm/bindings/project/project_external_stems.cpp
      wasm/bindings/project/project_edit.cpp
      wasm/bindings/project/project_midi.cpp
      wasm/bindings/project/project_bounce.cpp
      wasm/bindings/project/project_meta.cpp
      wasm/bindings/analysis/quick.cpp
      wasm/bindings/analysis/quick_detailed.cpp
      wasm/bindings/effects/repair.cpp
      wasm/bindings/effects/realtime_voice_changer.cpp
      wasm/bindings/realtime/engine.cpp
      wasm/bindings/realtime/transport.cpp
      wasm/bindings/realtime/params.cpp
      wasm/bindings/realtime/midi.cpp
      wasm/bindings/realtime/mixer.cpp
      wasm/bindings/realtime/clips.cpp
      wasm/bindings/realtime/capture.cpp
      wasm/bindings/realtime/processing.cpp
      wasm/bindings/realtime/telemetry.cpp
      wasm/bindings/analysis/stream_analyzer.cpp
      wasm/bindings/mastering/streaming_equalizer.cpp
      wasm/bindings/mastering/streaming_chain.cpp
      wasm/bindings/streaming/retune.cpp
    )
  endif()

  add_executable(sonare ${SONARE_WASM_BINDING_SOURCES})
  set_target_properties(sonare PROPERTIES OUTPUT_NAME ${SONARE_WASM_OUTPUT_NAME})
  # sonare_rt supplies the realtime processor and transport translation units the
  # bindings reach for; the canonical K-weighting / RBJ coefficient designers
  # that core analysis and metering share live one layer down in sonare_core.
  # The analysis entry deliberately stops here: it does not link the realtime
  # engine, mixer, or arrangement layers.
  target_link_libraries(sonare PRIVATE sonare_core sonare_rt)
  target_compile_definitions(sonare PRIVATE
    SONARE_BUILD_MASTERING=$<IF:$<BOOL:${BUILD_MASTERING}>,1,0>
    SONARE_BUILD_MIXING=$<IF:$<BOOL:${BUILD_MIXING}>,1,0>
    SONARE_BUILD_MIXING_ASSISTANT=$<IF:$<BOOL:${BUILD_MIXING_ASSISTANT}>,1,0>
    SONARE_BUILD_FX=$<IF:$<BOOL:${BUILD_FX}>,1,0>
    SONARE_BUILD_ARRANGEMENT=$<IF:$<BOOL:${BUILD_ARRANGEMENT}>,1,0>
    $<$<BOOL:${SONARE_WASM_ANALYSIS_ONLY}>:SONARE_WASM_ANALYSIS_ONLY=1>
  )
  if(NOT SONARE_WASM_ANALYSIS_ONLY)
    target_link_libraries(sonare PRIVATE sonare_engine)
  endif()
  # `sonare_note_segments` is part of the public feature surface, including
  # the analysis-only WASM bundle. Its C-ABI implementation lives in
  # sonare_core but calls NoteSegmenter from this target, so both bundle shapes
  # must link the implementation library.
  if(BUILD_PITCH_EDITOR)
    target_link_libraries(sonare PRIVATE sonare_pitch_editor)
  endif()
  if(BUILD_VOICE_CHANGER AND NOT SONARE_WASM_ANALYSIS_ONLY)
    target_link_libraries(sonare PRIVATE sonare_voice_changer)
    target_compile_definitions(sonare PRIVATE SONARE_WITH_VOICE_CHANGER=1)
  endif()
  if(BUILD_MASTERING AND NOT SONARE_WASM_ANALYSIS_ONLY)
    target_link_libraries(sonare PRIVATE sonare_mastering)
    target_compile_definitions(sonare PRIVATE SONARE_WITH_MASTERING=1)
  elseif(NOT SONARE_WASM_ANALYSIS_ONLY)
    target_sources(sonare PRIVATE
      mastering/common/loudness_measure.cpp
      mastering/maximizer/loudness_optimize.cpp)
  endif()
  if(BUILD_MIXING AND NOT SONARE_WASM_ANALYSIS_ONLY)
    target_link_libraries(sonare PRIVATE sonare_mixing)
    target_compile_definitions(sonare PRIVATE SONARE_WITH_MIXING=1)
    if(BUILD_GRAPH)
      # The mixer C API routes strips through the routing
      # graph, so the WASM MixerWasm binding needs sonare_graph linked and the
      # C API translation unit compiled in.
      target_link_libraries(sonare PRIVATE sonare_graph)
      target_compile_definitions(sonare PRIVATE SONARE_WITH_GRAPH=1)
      target_sources(sonare PRIVATE ${SONARE_C_MIXING_SOURCES})
    endif()
    if(BUILD_MIXING_ASSISTANT)
      target_link_libraries(sonare PRIVATE sonare_mixing_assistant)
      target_compile_definitions(sonare PRIVATE SONARE_WITH_MIXING_ASSISTANT=1)
    endif()
  endif()
  if(BUILD_ACOUSTIC_SIM AND BUILD_FX AND NOT SONARE_WASM_ANALYSIS_ONLY)
    # RIR synthesis + room estimation (acoustic) and the offline morph (fx).
    target_link_libraries(sonare PRIVATE sonare_acoustic sonare_fx)
    target_compile_definitions(sonare PRIVATE SONARE_WITH_ACOUSTIC_SIM=1)
  endif()
  if(BUILD_ARRANGEMENT AND NOT SONARE_WASM_ANALYSIS_ONLY)
    # Headless DAW project. The WASM Project binding wraps the C-ABI project
    # sources, so compile them into the target and link the arrangement
    # control-plane libraries (serializer / MIR / MIDI / project model).
    # sonare_c_memory.cpp (sonare_free_string / _floats) is already compiled into
    # sonare_core via sonare_core_objects, so it is NOT added here.
    target_sources(sonare PRIVATE ${SONARE_C_PROJECT_SOURCES})
    target_link_libraries(sonare PRIVATE
      sonare_arrangement sonare_serialize sonare_midi sonare_mir)
    target_compile_definitions(sonare PRIVATE SONARE_WITH_ARRANGEMENT=1)
  endif()
  target_compile_options(sonare PRIVATE -msimd128)
  # -fexceptions must be on the COMPILE line, not only the link line: emscripten
  # defaults to DISABLE_EXCEPTION_CATCHING=1, which elides landing pads at
  # compile time, so enabling runtime support at link time cannot restore a
  # `catch` that was never emitted. Losing one is silent -- not a compile error,
  # not a link error, not a test failure. A C-ABI guard that promises an error
  # code delivers a raw exception instead, and a `noexcept` function whose
  # catch(...) was deleted calls std::terminate and takes the module with it.
  #
  # Every unit that catches gets the flag, across every target linked into the
  # module -- the sibling static libraries are compiled by their own target's
  # flags, so listing only this target's sources leaves the engine, mastering,
  # mixing, serializer and C-ABI units uncovered. Whole-target -fexceptions
  # needs no list at all, but emits landing pads in the 343 units that do not
  # catch, and measures at roughly twice the size for the same behaviour
  # (gzip +11.6% against +6.5% on the full module, +12.3% against +4.1% on the
  # analysis-only one).
  #
  # The per-target if(TARGET) guards are load-bearing: a feature-reduced
  # configuration does not create every target, and TARGET_DIRECTORY on a target
  # that does not exist is a hard configure error. That failure appears only in
  # the reduced configuration, so the full build cannot catch it.
  #
  # Kept in step with the sources by tests/conformance/check_wasm_exception_scope.py,
  # which resolves the linked object set from the module's link line, follows the
  # compiler's own dependency files so a catch reached through a header counts,
  # and fails on any unit that catches without the flag.
  if(TARGET sonare)
    set_source_files_properties(
      c_api/mixing_assistant.cpp
      c_api/mixing_graph.cpp
      c_api/mixing_mixer.cpp
      c_api/mixing_strip.cpp
      c_api/project_bounce.cpp
      c_api/project_core.cpp
      c_api/project_edit_clip.cpp
      c_api/project_edit_track.cpp
      c_api/project_external_stems.cpp
      c_api/project_midi.cpp
      c_api/project_mir.cpp
      c_api/project_soundfont.cpp
      c_api/sonare_c_mixing.cpp
      c_api/sonare_c_project.cpp
      c_api/sonare_c_streaming_retune.cpp
      c_api/sonare_c_voice_changer.cpp
      wasm/bindings.cpp
      wasm/bindings/analysis/quick.cpp
      wasm/bindings/analysis/quick_detailed.cpp
      wasm/bindings/analysis/stream_analyzer.cpp
      wasm/bindings/common/common.cpp
      wasm/bindings/effects/audio.cpp
      wasm/bindings/effects/offline_dynamics_editing.cpp
      wasm/bindings/effects/realtime_voice_changer.cpp
      wasm/bindings/effects/repair.cpp
      wasm/bindings/features/core.cpp
      wasm/bindings/features/music.cpp
      wasm/bindings/features/pitch.cpp
      wasm/bindings/features/spectral.cpp
      wasm/bindings/features/spectrogram.cpp
      wasm/bindings/mastering/api.cpp
      wasm/bindings/mastering/chain.cpp
      wasm/bindings/mastering/streaming_chain.cpp
      wasm/bindings/mastering/streaming_equalizer.cpp
      wasm/bindings/metering/metering.cpp
      wasm/bindings/mixing/mixing.cpp
      wasm/bindings/mixing/mixing_assistant.cpp
      wasm/bindings/mixing/mixing_automation.cpp
      wasm/bindings/mixing/mixing_processing.cpp
      wasm/bindings/mixing/mixing_strip.cpp
      wasm/bindings/mixing/mixing_topology.cpp
      wasm/bindings/offline.cpp
      wasm/bindings/project/project.cpp
      wasm/bindings/project/project_arrange.cpp
      wasm/bindings/project/project_bounce.cpp
      wasm/bindings/project/project_edit.cpp
      wasm/bindings/project/project_external_stems.cpp
      wasm/bindings/project/project_meta.cpp
      wasm/bindings/project/project_midi.cpp
      wasm/bindings/realtime/capture.cpp
      wasm/bindings/realtime/clips.cpp
      wasm/bindings/realtime/engine.cpp
      wasm/bindings/realtime/midi.cpp
      wasm/bindings/realtime/mixer.cpp
      wasm/bindings/realtime/params.cpp
      wasm/bindings/realtime/processing.cpp
      wasm/bindings/realtime/telemetry.cpp
      wasm/bindings/realtime/transport.cpp
      wasm/bindings/streaming/retune.cpp
      TARGET_DIRECTORY sonare
      PROPERTIES COMPILE_OPTIONS -fexceptions)
  endif()
  if(TARGET sonare_arrangement)
    set_source_files_properties(
      arrangement/edit_compiler.cpp
      arrangement/edit_history.cpp
      arrangement/external_stems.cpp
      arrangement/retained_bytes.cpp
      TARGET_DIRECTORY sonare_arrangement
      PROPERTIES COMPILE_OPTIONS -fexceptions)
  endif()
  if(TARGET sonare_core_objects)
    set_source_files_properties(
      c_api/core_analysis.cpp
      c_api/core_audio.cpp
      c_api/core_common.cpp
      c_api/core_cqt.cpp
      c_api/core_quick.cpp
      c_api/eq_band_json.cpp
      c_api/features_common.cpp
      c_api/features_inverse.cpp
      c_api/features_music.cpp
      c_api/features_segment.cpp
      c_api/features_spectral_pitch.cpp
      c_api/features_spectrogram_mel.cpp
      c_api/features_streaming.cpp
      c_api/sonare_c.cpp
      c_api/sonare_c_compat.cpp
      c_api/sonare_c_editing.cpp
      c_api/sonare_c_effects.cpp
      c_api/sonare_c_features.cpp
      c_api/sonare_c_internal.cpp
      c_api/sonare_c_memory.cpp
      core/convert.cpp
      TARGET_DIRECTORY sonare_core_objects
      PROPERTIES COMPILE_OPTIONS -fexceptions)
  endif()
  if(TARGET sonare_engine)
    set_source_files_properties(
      engine/engine_controller.cpp
      engine/realtime_engine.cpp
      engine/realtime_engine_commands.cpp
      engine/realtime_engine_instruments.cpp
      engine/realtime_engine_lifecycle.cpp
      engine/realtime_engine_midi.cpp
      engine/realtime_engine_mixing.cpp
      engine/track_mixer.cpp
      engine/track_mixer_params.cpp
      TARGET_DIRECTORY sonare_engine
      PROPERTIES COMPILE_OPTIONS -fexceptions)
  endif()
  if(TARGET sonare_mastering)
    set_source_files_properties(
      mastering/api/chain_json.cpp
      mastering/api/insert_factory.cpp
      mastering/api/named_processor_registry.cpp
      mastering/assistant/audio_profile.cpp
      mastering/eq/equalizer.cpp
      TARGET_DIRECTORY sonare_mastering
      PROPERTIES COMPILE_OPTIONS -fexceptions)
  endif()
  if(TARGET sonare_midi)
    set_source_files_properties(
      midi/sequencer.cpp
      TARGET_DIRECTORY sonare_midi
      PROPERTIES COMPILE_OPTIONS -fexceptions)
  endif()
  if(TARGET sonare_mixing)
    set_source_files_properties(
      mixing/alignment_delay.cpp
      mixing/channel_strip.cpp
      TARGET_DIRECTORY sonare_mixing
      PROPERTIES COMPILE_OPTIONS -fexceptions)
  endif()
  if(TARGET sonare_serialize)
    set_source_files_properties(
      serialize/project_serializer.cpp
      serialize/project_serializer_decode.cpp
      TARGET_DIRECTORY sonare_serialize
      PROPERTIES COMPILE_OPTIONS -fexceptions)
  endif()
  if(TARGET sonare_voice_changer)
    set_source_files_properties(
      editing/voice_changer/realtime/config.cpp
      TARGET_DIRECTORY sonare_voice_changer
      PROPERTIES COMPILE_OPTIONS -fexceptions)
  endif()
  target_link_options(sonare PRIVATE
    -msimd128
    # Enable classic (JS-based) C++ exception support so a native throw
    # propagates to JS as a catchable CppException carrying the exception
    # pointer, instead of aborting. emscripten disables exception catching by
    # default (DISABLE_EXCEPTION_CATCHING=1); without this, embind functions
    # that throw abort the module on platforms where the throw is not silently
    # turned into a pointer (e.g. Linux), which is how SonareError surfacing
    # breaks. The JS module wrapper decodes the pointer via sonareExceptionInfo.
    -fexceptions
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
    COMMAND ${CMAKE_COMMAND} -E make_directory ${PROJECT_SOURCE_DIR}/bindings/wasm/dist
    COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_FILE:sonare> ${PROJECT_SOURCE_DIR}/bindings/wasm/dist/
    COMMAND ${CMAKE_COMMAND} -E copy ${CMAKE_BINARY_DIR}/bin/${SONARE_WASM_OUTPUT_NAME}.wasm ${PROJECT_SOURCE_DIR}/bindings/wasm/dist/
    COMMENT "Copying WASM files to bindings/wasm/dist/"
  )

endif()
