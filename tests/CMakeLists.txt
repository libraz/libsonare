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
  feature/spectral_test.cpp
  feature/onset_test.cpp
  feature/pitch_test.cpp
  feature/cqt_test.cpp
  feature/vqt_test.cpp
  effects/hpss_test.cpp
  effects/time_stretch_test.cpp
  effects/pitch_shift_test.cpp
  effects/normalize_test.cpp
  analysis/key_profiles_test.cpp
  analysis/chord_templates_test.cpp
  analysis/bpm_analyzer_test.cpp
  analysis/key_analyzer_test.cpp
  analysis/onset_analyzer_test.cpp
  analysis/dynamics_analyzer_test.cpp
  analysis/beat_analyzer_test.cpp
  analysis/chord_analyzer_test.cpp
  analysis/boundary_detector_test.cpp
  analysis/meter_basic_test.cpp
  analysis/rhythm_analyzer_test.cpp
  analysis/timbre_analyzer_test.cpp
  analysis/melody_analyzer_test.cpp
  analysis/section_analyzer_test.cpp
  analysis/music_analyzer_test.cpp
  streaming/stream_analyzer_test.cpp
  api/quick_test.cpp
  api/sonare_c_test.cpp
  librosa/convert_test.cpp
  librosa/mel_test.cpp
  librosa/mfcc_test.cpp
  librosa/onset_test.cpp
  librosa/tempo_test.cpp
  librosa/beat_test.cpp
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
  cli/cli_test.cpp
)

target_link_libraries(sonare_tests PRIVATE sonare_core Catch2::Catch2WithMain)
target_compile_options(sonare_tests PRIVATE ${SONARE_WARNING_FLAGS})
target_include_directories(sonare_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

if(BUILD_MASTERING)
  target_sources(sonare_tests PRIVATE
    mastering/chain_test.cpp
    mastering/common_test.cpp
    mastering/preset_test.cpp
    mastering/dynamics_test.cpp
    mastering/eq_test.cpp
    mastering/final_test.cpp
    mastering/match_test.cpp
    mastering/maximizer_test.cpp
    mastering/multiband_test.cpp
    mastering/repair_test.cpp
    mastering/saturation_test.cpp
    mastering/spectral_test.cpp
    mastering/stereo_test.cpp
  )
  target_link_libraries(sonare_tests PRIVATE sonare_mastering)
  target_sources(sonare_tests PRIVATE ${CMAKE_SOURCE_DIR}/src/sonare_c_mastering.cpp)
  target_compile_definitions(sonare_tests PRIVATE SONARE_WITH_MASTERING=1)
endif()

include(CTest)
include(Catch)
catch_discover_tests(sonare_tests WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
