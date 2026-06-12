# Catch2
Include(FetchContent)
FetchContent_Declare(
  Catch2
  GIT_REPOSITORY https://github.com/catchorg/Catch2.git
  GIT_TAG v3.4.0
)
FetchContent_MakeAvailable(Catch2)

# Test executable
add_executable(sonare_tests
  test_main.cpp
  util/math_utils_test.cpp
  util/lru_cache_test.cpp
  util/json_test.cpp
  util/frame_test.cpp
  util/padding_test.cpp
  util/peak_test.cpp
  util/vector_normalize_test.cpp
  util/poly_features_test.cpp
  util/onset_backtrack_test.cpp
  util/chroma_cqt_test.cpp
  util/pitch_utilities_test.cpp
  util/plp_test.cpp
  util/iirt_test.cpp
  util/inverse_features_test.cpp
  util/zero_crossings_test.cpp
  util/weighting_test.cpp
  util/synthesis_test.cpp
  util/remix_test.cpp
  util/audio_ops_test.cpp
  util/nnls_test.cpp
  util/harmonic_test.cpp
  util/wavelet_test.cpp
  util/segment_test.cpp
  util/sequence_test.cpp
  util/decompose_test.cpp
  util/reassigned_test.cpp
  core/convert_test.cpp
  core/window_test.cpp
  core/fft_test.cpp
  core/audio_io_test.cpp
  core/audio_test.cpp
  core/resample_test.cpp
  core/spectrum_test.cpp
  filters/mel_test.cpp
  filters/chroma_test.cpp
  filters/dct_test.cpp
  filters/iir_test.cpp
  feature/mel_spectrogram_test.cpp
  feature/chroma_test.cpp
  feature/nnls_chroma_test.cpp
  feature/spectral_test.cpp
  feature/onset_test.cpp
  feature/pitch_test.cpp
  feature/cqt_test.cpp
  feature/vqt_test.cpp
  effects/hpss_test.cpp
  effects/time_stretch_test.cpp
  effects/pitch_shift_test.cpp
  effects/phase_vocoder_test.cpp
  effects/normalize_test.cpp
  effects/fx_reachability_test.cpp
  analysis/key_profiles_test.cpp
  analysis/chord_templates_test.cpp
  analysis/acoustic_analyzer_test.cpp
  analysis/bpm_analyzer_test.cpp
  analysis/key_analyzer_test.cpp
  analysis/key_synthetic_matrix_test.cpp
  analysis/onset_analyzer_test.cpp
  analysis/dynamics_analyzer_test.cpp
  analysis/beat_analyzer_test.cpp
  analysis/meter_analyzer_test.cpp
  analysis/downbeat_analyzer_test.cpp
  analysis/chord_analyzer_test.cpp
  analysis/chord_synthetic_matrix_test.cpp
  analysis/boundary_detector_test.cpp
  metering/meter_basic_test.cpp
  analysis/rhythm_analyzer_test.cpp
  analysis/timbre_analyzer_test.cpp
  analysis/melody_analyzer_test.cpp
  analysis/section_analyzer_test.cpp
  analysis/music_analyzer_test.cpp
  streaming/stream_analyzer_core_test.cpp
  streaming/stream_analyzer_quantized_test.cpp
  streaming/stream_analyzer_integration_test.cpp
  streaming/stream_analyzer_long_chord_test.cpp
  rt/adaa2_test.cpp
  rt/biquad_design_test.cpp
  rt/k_weighting_freq_response_test.cpp
  rt/spsc_queue_test.cpp
  rt/partitioned_convolver_test.cpp
  rt/true_peak_filter_test.cpp
  rt/rt_publisher_test.cpp
  rt/seqlock_cell_test.cpp
  rt/overflow_counter_test.cpp
  automation/automation_test.cpp
  engine/boundary_splitter_test.cpp
  engine/capture_test.cpp
  engine/clip_player_test.cpp
  engine/metronome_test.cpp
  engine/realtime_engine_test.cpp
  engine/realtime_engine_concurrency_test.cpp
  engine/telemetry_test.cpp
  engine/tempo_sync_test.cpp
  transport/marker_test.cpp
  transport/transport_test.cpp
  api/quick_test.cpp
  api/sonare_c_core_test.cpp
  api/sonare_c_music_test.cpp
  api/sonare_c_mastering_test.cpp
  api/sonare_c_mastering_repair_test.cpp
  api/sonare_c_features_metering_test.cpp
  api/sonare_c_abi_version_test.cpp
  api/sonare_c_engine_voice_test.cpp
  api/sonare_c_mixing_features_test.cpp
  api/sonare_c_features_extra_test.cpp
  api/sonare_c_effects_extra_test.cpp
  api/sonare_c_metering_extra_test.cpp
  librosa/convert_test.cpp
  librosa/mel_test.cpp
  librosa/mfcc_test.cpp
  librosa/onset_test.cpp
  librosa/tempo_test.cpp
  librosa/beat_test.cpp
  librosa/high_level/bpm_parity_test.cpp
  librosa/high_level/beat_tracking_test.cpp
  librosa/stft_test.cpp
  librosa/power_to_db_test.cpp
  librosa/zcr_test.cpp
  librosa/spectral_test.cpp
  librosa/chroma_test.cpp
  librosa/cqt_test.cpp
  librosa/pitch_test.cpp
  librosa/hpss_test.cpp
  librosa/db_convert_test.cpp
  librosa/frames_samples_test.cpp
  librosa/magphase_test.cpp
  librosa/preemphasis_test.cpp
  librosa/silence_test.cpp
  librosa/tempogram_test.cpp
  librosa/poly_features_test.cpp
  librosa/tonnetz_test.cpp
  librosa/pcen_test.cpp
  librosa/pitch_utilities_test.cpp
  librosa/zero_crossings_test.cpp
  librosa/weighting_test.cpp
  librosa/remix_test.cpp
  librosa/audio_ops_test.cpp
  librosa/nnls_test.cpp
  librosa/harmonic_test.cpp
  librosa/wavelet_filters_test.cpp
  librosa/segment_test.cpp
  librosa/sequence_test.cpp
  librosa/decompose_test.cpp
  librosa/reassigned_test.cpp
)

target_link_libraries(sonare_tests PRIVATE sonare_engine Catch2::Catch2WithMain)
target_compile_options(sonare_tests PRIVATE ${SONARE_WARNING_FLAGS})
target_include_directories(sonare_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

if(SONARE_ENABLE_OPTIONAL_FIXTURE_TESTS)
  target_sources(sonare_tests PRIVATE
    analysis/acoustic_dataset_test.cpp
    analysis/music_dataset_test.cpp
  )
endif()

if(BUILD_MASTERING)
  target_sources(sonare_tests PRIVATE
    mastering/assistant_test.cpp
    mastering/chain_test.cpp
    mastering/chain_latency_test.cpp
    mastering/common_test.cpp
    mastering/property_test.cpp
    mastering/preset_test.cpp
    mastering/dynamics_compressor_limiter_test.cpp
    mastering/dynamics_expander_gate_test.cpp
    mastering/dynamics_processors_test.cpp
    mastering/dynamics_rt_safe_test.cpp
    mastering/eq_golden_test.cpp
    mastering/eq_parametric_test.cpp
    mastering/eq_processor_test.cpp
    mastering/eq_spectrum_test.cpp
    mastering/eq_phase_test.cpp
    mastering/eq_specialized_test.cpp
    mastering/eq_dynamic_detail_test.cpp
    mastering/final_test.cpp
    mastering/golden_hash_test.cpp
    mastering/amp_sim_test.cpp
    mastering/insert_factory_extra_test.cpp
    mastering/loudness_ceiling_test.cpp
    mastering/match_test.cpp
    mastering/maximizer_test.cpp
    mastering/multiband_test.cpp
    mastering/param_wiring_test.cpp
    mastering/repair_test.cpp
    mastering/saturation_test.cpp
    mastering/set_parameter_test.cpp
    mastering/tape_oversample_test.cpp
    mastering/spectral_test.cpp
    mastering/stereo_test.cpp
  )
  if(NOT BUILD_MIXING)
    target_link_libraries(sonare_tests PRIVATE sonare_mastering)
  endif()
  target_sources(sonare_tests PRIVATE
    ${PROJECT_SOURCE_DIR}/src/sonare_c_mastering_apply.cpp
    ${PROJECT_SOURCE_DIR}/src/sonare_c_mastering_chain.cpp
    ${PROJECT_SOURCE_DIR}/src/sonare_c_mastering_dynamics.cpp
    ${PROJECT_SOURCE_DIR}/src/sonare_c_mastering_eq.cpp
    ${PROJECT_SOURCE_DIR}/src/sonare_c_mastering_preset.cpp
    ${PROJECT_SOURCE_DIR}/src/sonare_c_mastering_repair.cpp
    ${PROJECT_SOURCE_DIR}/src/sonare_c_mastering_streaming.cpp
  )
  target_compile_definitions(sonare_tests PRIVATE SONARE_WITH_MASTERING=1)
  if(SONARE_ENABLE_OPTIONAL_FIXTURE_TESTS)
    target_sources(sonare_tests PRIVATE mastering/ebu_r128_compliance_test.cpp)
  endif()
endif()

if(BUILD_MIXING)
  target_sources(sonare_tests PRIVATE
    engine/meter_telemetry_test.cpp
    engine/mixing_runtime_test.cpp
    engine/monitor_runtime_test.cpp
    engine/track_mixer_test.cpp
    mixing/golden_hash_test.cpp
    mixing/insert_automation_c_api_test.cpp
    mixing/mixing_meter_panner_capi_test.cpp
    mixing/mixing_automation_scene_test.cpp
    mixing/mixing_channel_strip_test.cpp
    mixing/mixing_bus_control_test.cpp
    mixing/mixing_meter_lufs_integration_test.cpp
    mixing/no_alloc_allocators.cpp
    mixing/no_alloc_engine_mixing_test.cpp
    mixing/no_alloc_mastering_eq_test.cpp
    mixing/no_alloc_mastering_dynamics_test.cpp
    mixing/no_alloc_voice_reverb_test.cpp
    mixing/routing_latency_test.cpp
    mixing/routing_scene_test.cpp
    mixing/routing_capi_test.cpp
  )
  target_sources(sonare_tests PRIVATE
    ${PROJECT_SOURCE_DIR}/src/sonare_c_mixing.cpp
    ${PROJECT_SOURCE_DIR}/src/c_api/mixing_graph.cpp
    ${PROJECT_SOURCE_DIR}/src/c_api/mixing_mixer.cpp
    ${PROJECT_SOURCE_DIR}/src/c_api/mixing_strip.cpp
  )
  target_compile_definitions(sonare_tests PRIVATE SONARE_WITH_MIXING=1)
endif()

if(BUILD_GRAPH)
  target_sources(sonare_tests PRIVATE
    engine/graph_runtime_test.cpp
    graph/graph_test.cpp
  )
  target_compile_definitions(sonare_tests PRIVATE SONARE_WITH_GRAPH=1)
endif()

if(BUILD_FX)
  target_sources(sonare_tests PRIVATE effects/creative_fx_test.cpp)
  target_sources(sonare_tests PRIVATE effects/ensemble_test.cpp)
  target_sources(sonare_tests PRIVATE effects/fdn_reverb_test.cpp)
  if(NOT BUILD_MASTERING)
    target_link_libraries(sonare_tests PRIVATE sonare_fx)
  endif()
  target_compile_definitions(sonare_tests PRIVATE SONARE_WITH_FX=1)
endif()

if(BUILD_ACOUSTIC_SIM)
  target_sources(sonare_tests PRIVATE
    acoustic/room_types_test.cpp
    acoustic/material_test.cpp
    acoustic/room_model_test.cpp
    acoustic/image_source_test.cpp
    acoustic/late_reverb_test.cpp
    acoustic/rir_synthesizer_test.cpp
    acoustic/room_estimator_test.cpp
  )
  if(BUILD_FX)
    # The room-character morph lives in sonare_fx but needs the acoustic module.
    target_sources(sonare_tests PRIVATE effects/room_morph_test.cpp)
    target_sources(sonare_tests PRIVATE
      api/sonare_c_acoustic_test.cpp
      ${PROJECT_SOURCE_DIR}/src/sonare_c_acoustic.cpp
    )
  endif()
  if(SONARE_ENABLE_OPTIONAL_FIXTURE_TESTS)
    # Cross-engine ISM oracle vs a pyroomacoustics-generated golden; SKIPs when
    # the fixture is absent (see room_sim/gen_ism_golden.py).
    target_sources(sonare_tests PRIVATE acoustic/ism_golden_test.cpp)
  endif()
  target_link_libraries(sonare_tests PRIVATE sonare_acoustic)
  target_compile_definitions(sonare_tests PRIVATE SONARE_WITH_ACOUSTIC_SIM=1)
endif()

if(BUILD_PITCH_EDITOR)
  target_sources(sonare_tests PRIVATE editing/pitch_editor_test.cpp)
  if(NOT BUILD_VOICE_CHANGER)
    target_link_libraries(sonare_tests PRIVATE sonare_pitch_editor)
  endif()
  target_compile_definitions(sonare_tests PRIVATE SONARE_WITH_PITCH_EDITOR=1)
endif()

if(BUILD_VOICE_CHANGER)
  target_sources(sonare_tests PRIVATE
    editing/voice_changer_retune_formant_test.cpp
    editing/voice_changer_realtime_core_test.cpp
    editing/voice_changer_preset_capi_test.cpp
    editing/voice_changer_realtime_dsp_test.cpp
    editing/voice_changer_limiter_json_test.cpp
  )
  target_sources(sonare_tests PRIVATE engine/offline_bounce_golden_test.cpp)
  target_sources(sonare_tests PRIVATE ${PROJECT_SOURCE_DIR}/src/sonare_c_daw.cpp)
  target_sources(sonare_tests PRIVATE ${PROJECT_SOURCE_DIR}/src/sonare_c_engine.cpp)
  target_sources(sonare_tests PRIVATE ${PROJECT_SOURCE_DIR}/src/sonare_c_voice_changer.cpp)
  target_link_libraries(sonare_tests PRIVATE sonare_voice_changer)
  target_compile_definitions(sonare_tests PRIVATE
    SONARE_WITH_PITCH_EDITOR=1
    SONARE_WITH_VOICE_CHANGER=1
  )
endif()

if(BUILD_ARRANGEMENT)
  target_sources(sonare_tests PRIVATE
    arrangement/edit_model_test.cpp
    arrangement/edit_command_history_test.cpp
    arrangement/edit_compiler_golden_test.cpp
    arrangement/c_abi_edit_ops_test.cpp
    midi/midi_ump_test.cpp
    midi/builtin_synth_test.cpp
    midi/native_synth_test.cpp
    midi/filter_models_test.cpp
    midi/mod_matrix_test.cpp
    midi/fm_voice_test.cpp
    midi/ks_voice_test.cpp
    midi/modal_voice_test.cpp
    midi/piano_voice_test.cpp
    midi/voice_polish_test.cpp
    midi/synth_toolkit_test.cpp
    midi/sf2_file_test.cpp
    midi/sf2_player_test.cpp
    midi/sf2_fidelity_test.cpp
    midi/sf2_effects_test.cpp
    midi/sf2_gs_layer_test.cpp
    midi/midi_clip_sequencer_test.cpp
    midi/midi_no_alloc_test.cpp
    midi/smf_roundtrip_test.cpp
    midi/smf2_roundtrip_test.cpp
    midi/midi_fx_test.cpp
    midi/program_map_test.cpp
    midi/midi_routing_test.cpp
    mir/mir_tempo_grid_test.cpp
    mir/mir_warp_test.cpp
    mir/mir_key_chord_onset_test.cpp
    serialize/project_serializer_roundtrip_test.cpp
    serialize/project_deserialize_fuzz_test.cpp
    serialize/project_default_sr_and_uri_test.cpp
    host/host_seam_test.cpp
    engine/realtime_engine_midi_test.cpp
    bindings/binding_project_c_surface_test.cpp
    bindings/binding_project_bounce_test.cpp
    bindings/binding_project_editing_test.cpp
    bindings/binding_project_midi_test.cpp
    bindings/binding_project_soundfont_test.cpp
    bindings/binding_synth_patch_test.cpp
  )
  # The headless-project C ABI sources are compiled into the test binary so the
  # parity test drives the real C surface, like the engine/voice-changer TUs.
  target_sources(sonare_tests PRIVATE
    ${PROJECT_SOURCE_DIR}/src/sonare_c_project.cpp
    ${PROJECT_SOURCE_DIR}/src/c_api/project_bounce.cpp
    ${PROJECT_SOURCE_DIR}/src/c_api/project_core.cpp
    ${PROJECT_SOURCE_DIR}/src/c_api/project_edit.cpp
    ${PROJECT_SOURCE_DIR}/src/c_api/project_midi.cpp
    ${PROJECT_SOURCE_DIR}/src/c_api/project_mir.cpp
    ${PROJECT_SOURCE_DIR}/src/c_api/project_soundfont.cpp
  )
  target_link_libraries(sonare_tests PRIVATE sonare_arrangement sonare_mir sonare_serialize sonare_midi)
  target_compile_definitions(sonare_tests PRIVATE SONARE_WITH_ARRANGEMENT=1)

  # Composition-assist seam test. Exercises the seam end to end via a mock
  # module + a static layering guard (rt/ + engine/ must not include
  # midi/assist/). Gated by BUILD_ASSIST so a no-assist build compiles cleanly
  # with no assist symbols on the surface.
  if(BUILD_ASSIST)
    target_sources(sonare_tests PRIVATE assist/assist_seam_test.cpp)
    target_link_libraries(sonare_tests PRIVATE sonare_assist)
    target_compile_definitions(sonare_tests PRIVATE SONARE_WITH_ASSIST=1)
  endif()
endif()

# The CLI tests shell out to the built `sonare` binary, which only exists when
# the CLI is built. Register them only then so they do not fail with exit 127
# under -DBUILD_CLI=OFF (e.g. CI's native build matrix).
if(BUILD_CLI AND NOT BUILD_WASM)
  target_sources(sonare_tests PRIVATE cli/cli_test.cpp)
endif()

include(CTest)
include(Catch)
catch_discover_tests(sonare_tests WORKING_DIRECTORY ${PROJECT_SOURCE_DIR})
