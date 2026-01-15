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
  cli/cli_test.cpp
)

target_link_libraries(sonare_tests PRIVATE sonare_core Catch2::Catch2WithMain)
target_include_directories(sonare_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

include(CTest)
include(Catch)
catch_discover_tests(sonare_tests WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
