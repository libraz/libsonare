/// @file c_refusal_subjects_test.cpp
/// @brief C entries refuse their own arguments under the entry's name without `sonare_`.

#include <sonare/sonare_c.h>

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>
#include <cstdint>
#include <functional>
#include <limits>
#include <map>
#include <string>
#include <string_view>
#include <vector>

#include "../../bindings/node/src/addon/field_spelling.h"
#include "wasm/field_spelling.h"

namespace {

std::string last_message() {
  const char* message = sonare_last_error_message();
  return message != nullptr ? message : "";
}

}  // namespace

TEST_CASE("the interleaved LUFS entries name themselves and the refused argument",
          "[c_api][metering]") {
  std::vector<float> stereo(2 * 4800, 0.1f);
  SonareLufsResult result{};
  CHECK(sonare_lufs_interleaved(stereo.data(), 4800, 0, 48000, &result) ==
        SONARE_ERROR_INVALID_PARAMETER);
  CHECK_THAT(last_message(),
             Catch::Matchers::Equals("num_channels must be an integer in [1, 2147483647]"));

  stereo[7] = std::numeric_limits<float>::quiet_NaN();
  CHECK(sonare_lufs_interleaved(stereo.data(), 4800, 2, 48000, &result) ==
        SONARE_ERROR_INVALID_PARAMETER);
  CHECK_THAT(last_message(), Catch::Matchers::StartsWith("samples "));

  stereo[7] = 0.1f;
  float* momentary = nullptr;
  CHECK(sonare_lufs_series_interleaved(stereo.data(), 4800, 2, 48000, &momentary, nullptr, nullptr,
                                       nullptr) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK_THAT(last_message(), Catch::Matchers::StartsWith("out_momentary and out_momentary_length"));
  float* short_term = nullptr;
  size_t momentary_length = 0;
  size_t short_term_length = 0;
  stereo[7] = std::numeric_limits<float>::quiet_NaN();
  CHECK(sonare_lufs_series_interleaved(stereo.data(), 4800, 2, 48000, &momentary, &momentary_length,
                                       &short_term,
                                       &short_term_length) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK_THAT(last_message(), Catch::Matchers::StartsWith("samples "));
}

TEST_CASE("the true-peak entries refuse an unsupported oversample factor by its parameter name",
          "[c_api][metering]") {
  const std::vector<float> samples(4800, 0.25f);
  float db = 0.0f;
  CHECK(sonare_metering_true_peak_dbtp(samples.data(), samples.size(), 48000, 3, &db) ==
        SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == "oversample_factor must be one of 1, 2, 4, 8, 16");
  REQUIRE(sonare_metering_true_peak_dbtp(samples.data(), samples.size(), 48000, 0, &db) ==
          SONARE_OK);
  float explicit_db = 0.0f;
  REQUIRE(sonare_metering_true_peak_dbtp(samples.data(), samples.size(), 48000, 4, &explicit_db) ==
          SONARE_OK);
  CHECK(db == explicit_db);

  SonareAudio* audio = nullptr;
  REQUIRE(sonare_audio_from_buffer(samples.data(), samples.size(), 48000, &audio) == SONARE_OK);
  CHECK(sonare_audio_true_peak_dbtp(audio, -2, &db) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == "oversample_factor must be one of 1, 2, 4, 8, 16");
  sonare_audio_destroy(audio);
}

#if defined(SONARE_WITH_ARRANGEMENT)
TEST_CASE("take alignment names the buffer it refuses", "[c_api][project]") {
  std::vector<float> reference(22050, 0.1f);
  std::vector<float> take(22050, 0.1f);
  SonareProjectWarpAnchor* anchors = nullptr;
  size_t count = 0;
  SonareTakeAlignment alignment{};

  take[100] = std::numeric_limits<float>::infinity();
  CHECK(sonare_align_take_to_reference_ex(reference.data(), reference.size(), 22050, take.data(),
                                          take.size(), 22050, nullptr, &anchors, &count,
                                          &alignment) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK_THAT(last_message(), Catch::Matchers::StartsWith("take "));

  reference[5] = std::numeric_limits<float>::quiet_NaN();
  CHECK(sonare_align_take_to_reference_ex(reference.data(), reference.size(), 22050, take.data(),
                                          take.size(), 22050, nullptr, &anchors, &count,
                                          &alignment) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK_THAT(last_message(), Catch::Matchers::StartsWith("reference "));

  reference[5] = 0.0f;
  take[100] = 0.0f;
  CHECK(sonare_align_take_to_reference_ex(reference.data(), reference.size(), 22050, take.data(),
                                          take.size(), 22050, nullptr, nullptr, &count,
                                          &alignment) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == "out_anchors must not be NULL");

  SonareTakeAlignConfig config{};
  config.bins_per_octave = 18;
  CHECK(sonare_align_take_to_reference_ex(reference.data(), reference.size(), 22050, take.data(),
                                          take.size(), 22050, &config, nullptr, &count,
                                          &alignment) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == "config.bins_per_octave must be a multiple of 12");
  config.bins_per_octave = -12;
  CHECK(sonare_align_take_to_reference_ex(reference.data(), reference.size(), 22050, take.data(),
                                          take.size(), 22050, &config, &anchors, &count,
                                          &alignment) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == "config.bins_per_octave must be an integer in [0, 2147483647]");
  config = SonareTakeAlignConfig{};
  for (const int hop : {-256, 65537}) {
    config.hop_length = hop;
    CHECK(sonare_align_take_to_reference_ex(reference.data(), reference.size(), 22050, take.data(),
                                            take.size(), 22050, &config, &anchors, &count,
                                            &alignment) == SONARE_ERROR_INVALID_PARAMETER);
    CHECK(last_message() == "config.hop_length must be an integer in [0, 65536]");
  }
  // The CQT's fmin and n_bins are derived from these two, so its Nyquist refusal names them.
  config = SonareTakeAlignConfig{};
  config.bins_per_octave = 24;
  CHECK(sonare_align_take_to_reference_ex(reference.data(), reference.size(), 8000, take.data(),
                                          take.size(), 8000, &config, &anchors, &count,
                                          &alignment) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() ==
        "config.bins_per_octave puts the top chroma bin at or above half of "
        "reference_sample_rate");
}
#endif  // defined(SONARE_WITH_ARRANGEMENT)

TEST_CASE("the CQT-family entries refuse each member by its C parameter name", "[c_api][feature]") {
  const std::vector<float> samples(22050, 0.1f);
  SonareCqtResult out{};
  const auto refused = [&](int hop, float fmin, int n_bins, int bpo, float filter_scale = 1.0f) {
    CHECK(sonare_cqt(samples.data(), samples.size(), 22050, hop, fmin, n_bins, bpo, filter_scale,
                     &out) == SONARE_ERROR_INVALID_PARAMETER);
    return last_message();
  };
  CHECK(refused(0, 32.7f, 24, 12) == "hop_length must be an integer in [1, 65536]");
  CHECK(refused(512, 0.0f, 24, 12) == "fmin_hz must be finite and greater than 0");
  CHECK(refused(512, 32.7f, 0, 12) == "n_bins must be an integer in [1, 2147483647]");
  CHECK(refused(512, 32.7f, 24, 0) == "bins_per_octave must be an integer in [1, 2147483647]");
  CHECK(refused(512, 32.7f, 24, 12, 0.0f) == "filter_scale must be finite and greater than 0");

  CHECK(sonare_pseudo_cqt(samples.data(), samples.size(), 22050, 512, 32.7f, 0, 12, 1.0f, &out) ==
        SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == "n_bins must be an integer in [1, 2147483647]");
  CHECK(sonare_vqt(samples.data(), samples.size(), 22050, 0, 32.7f, 24, 12, 1.0f, 0.0f, &out) ==
        SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == "hop_length must be an integer in [1, 65536]");
  for (const float bad_gamma : {std::numeric_limits<float>::infinity(), -1.0f}) {
    CHECK(sonare_vqt(samples.data(), samples.size(), 22050, 512, 32.7f, 24, 12, 1.0f, bad_gamma,
                     &out) == SONARE_ERROR_INVALID_PARAMETER);
    CHECK(last_message() == "gamma must be finite and at least 0");
  }
}

TEST_CASE("the CQT inverse refuses each member by its core name", "[c_api][feature]") {
  const std::vector<float> magnitude(24 * 4, 1.0f);
  float* out = nullptr;
  size_t out_length = 0;
  const auto message = [&](int n_bins, int n_frames, int sample_rate, int hop, float fmin, int bpo,
                           int n_iter) {
    CHECK(sonare_cqt_to_audio_checked(magnitude.data(), magnitude.size(), n_bins, n_frames,
                                      sample_rate, hop, fmin, bpo, n_iter, &out,
                                      &out_length) == SONARE_ERROR_INVALID_PARAMETER);
    return last_message();
  };
  CHECK(message(0, 4, 22050, 512, 32.7f, 12, 4) == "n_bins must be an integer in [1, 2147483647]");
  CHECK(message(24, 0, 22050, 512, 32.7f, 12, 4) ==
        "n_frames must be an integer in [1, 2147483647]");
  CHECK(message(24, 4, 100, 512, 32.7f, 12, 4) ==
        "sample_rate must be an integer in [8000, 384000]");
  CHECK(message(24, 4, 22050, 0, 32.7f, 12, 4) == "hop_length must be an integer in [1, 65536]");
  CHECK(message(24, 4, 22050, 512, 0.0f, 12, 4) == "fmin_hz must be finite and greater than 0");
  CHECK(message(24, 4, 22050, 512, 32.7f, 0, 4) ==
        "bins_per_octave must be an integer in [1, 2147483647]");
  CHECK(message(24, 4, 22050, 512, 32.7f, 12, 0) == "n_iter must be an integer in [1, 256]");
  CHECK(message(24, 4, 22050, 512, 32.7f, 12, 257) == "n_iter must be an integer in [1, 256]");
  CHECK(message(24, 4, 22050, 512, 20000.0f, 12, 4) ==
        "fmin_hz * 2^((n_bins - 1) / bins_per_octave) must be less than sample_rate / 2");
  // The filter scale is fixed at 1, so the filter-length relation names only what a caller moves.
  CHECK(message(24, 4, 22050, 512, 0.01f, 12, 4) ==
        "fmin_hz and bins_per_octave must keep every filter at most 131072 samples long at "
        "sample_rate");
  CHECK(sonare_vqt_to_audio_checked(magnitude.data(), magnitude.size(), 24, 4, 22050, 512, 0.01f,
                                    12, 0.0f, 4, &out,
                                    &out_length) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() ==
        "fmin_hz, bins_per_octave and gamma must keep every filter at most 131072 samples long at "
        "sample_rate");
}

TEST_CASE("the music feature entries refuse their own members", "[c_api][feature]") {
  const std::vector<float> samples(22050, 0.1f);
  SonareChromaResult chroma{};
  CHECK(sonare_bass_chroma(samples.data(), samples.size(), 22050, 0, 12, &chroma) ==
        SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == "hop_length must be an integer in [1, 65536]");
  CHECK(sonare_bass_chroma(samples.data(), samples.size(), 22050, 512, 0, &chroma) ==
        SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == "n_chroma must be an integer in [1, 2147483647]");
  CHECK(sonare_bass_chroma(samples.data(), samples.size(), 22050, 512, 5, &chroma) ==
        SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == "bins_per_octave must be a multiple of n_chroma");

  float* env = nullptr;
  size_t env_length = 0;
  int n_frames = 0;
  CHECK(sonare_onset_strength_multi(samples.data(), samples.size(), 22050, 2048, 512, 128, 0, &env,
                                    &env_length, &n_frames) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == "n_bands must be an integer in [1, 2147483647]");

  const std::vector<float> freqs{440.0f, 450.0f};
  float tuning = 0.0f;
  CHECK(sonare_pitch_tuning(freqs.data(), freqs.size(), 1.0f, 12, &tuning) ==
        SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == "resolution must be finite and in (0, 1)");
  CHECK(sonare_pitch_tuning(freqs.data(), freqs.size(), 0.01f, 0, &tuning) ==
        SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == "bins_per_octave must be an integer in [1, 2147483647]");
  CHECK(sonare_estimate_tuning(samples.data(), samples.size(), 22050, 2048, 512, 0.0f, 12,
                               &tuning) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == "resolution must be finite and in (0, 1)");
  CHECK(sonare_estimate_tuning(samples.data(), samples.size(), 22050, 2048, 512, 0.01f, 0,
                               &tuning) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == "bins_per_octave must be an integer in [1, 2147483647]");

  SonareMelodyResult melody{};
  CHECK(sonare_analyze_melody(samples.data(), samples.size(), 22050, 0.0f, 2093.0f, 2048, 256, 0.1f,
                              &melody) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == "f0_min_hz must be finite and greater than 0");
  CHECK(sonare_analyze_melody_ex(samples.data(), samples.size(), 22050, 100.0f, 100.0f, 2048, 256,
                                 0.1f, 1, 1, &melody) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == "f0_max_hz must be finite and greater than f0_min_hz");

  SonarePitchResult pitch{};
  CHECK(sonare_pitch_yin(samples.data(), samples.size(), 22050, 2048, 512, 500.0f, 100.0f, 0.1f,
                         &pitch) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == "f0_max_hz must be finite and greater than f0_min_hz");
  CHECK(sonare_pitch_pyin(samples.data(), samples.size(), 22050, 2048, 512, 500.0f, 100.0f, 0.1f, 0,
                          &pitch) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == "f0_max_hz must be finite and greater than f0_min_hz");
}

TEST_CASE("the music and spectrogram entries refuse a NULL argument by its name",
          "[c_api][feature]") {
  const std::vector<float> samples(22050, 0.1f);
  CHECK(sonare_chroma(samples.data(), samples.size(), 22050, 2048, 512, nullptr) ==
        SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == "out must not be NULL");
  CHECK(sonare_bass_chroma(samples.data(), samples.size(), 22050, 512, 12, nullptr) ==
        SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == "out must not be NULL");
  float* env = nullptr;
  size_t env_length = 0;
  CHECK(sonare_onset_strength(samples.data(), samples.size(), 22050, 2048, 512, 128, &env,
                              nullptr) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == "out_length must not be NULL");
  CHECK(sonare_onset_strength_multi(samples.data(), samples.size(), 22050, 2048, 512, 128, 2, &env,
                                    &env_length, nullptr) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == "out_n_frames must not be NULL");

  CHECK(sonare_stft(samples.data(), samples.size(), 22050, 2048, 512, nullptr) ==
        SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == "out must not be NULL");
  int n_bins = 0;
  int n_frames = 0;
  CHECK(sonare_stft_db(samples.data(), samples.size(), 22050, 2048, 512, &n_bins, &n_frames,
                       nullptr) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == "out_db must not be NULL");

  const std::vector<float> features(4 * 9, 0.5f);
  float* delta = nullptr;
  CHECK(sonare_mel_delta(nullptr, 4, 9, 9, &delta) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == "features must not be NULL");
  CHECK(sonare_mel_delta(features.data(), 0, 9, 9, &delta) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == "n_features must be an integer in [1, 2147483647]");
  CHECK(sonare_mel_delta(features.data(), 4, 0, 9, &delta) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == "n_frames must be an integer in [1, 2147483647]");
  CHECK(sonare_mel_delta(features.data(), 1 << 16, 1 << 16, 9, &delta) ==
        SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == "n_features * n_frames must be at most 2147483647");
  CHECK(sonare_mel_delta(features.data(), 4, 9, 4, &delta) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == "width must be an odd integer in [3, 2147483647]");
}

TEST_CASE("the audio handle entries refuse a NULL argument by its name", "[c_api][audio_io]") {
  const std::vector<uint8_t> bytes(16, 0);
  SonareAudio* audio = nullptr;
  CHECK(sonare_audio_from_memory(nullptr, 1, &audio) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == "data must not be NULL when length is not 0");
  CHECK(sonare_audio_from_memory(bytes.data(), 0, &audio) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == "data must not be empty");
  CHECK(sonare_audio_from_memory(nullptr, 0, &audio) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == "data must not be empty");
  CHECK(sonare_audio_from_memory(bytes.data(), bytes.size(), nullptr) ==
        SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == "out must not be NULL");
#ifndef __EMSCRIPTEN__
  CHECK(sonare_audio_from_file(nullptr, &audio) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == "path must not be NULL");
  CHECK(sonare_audio_from_file_channel("x.wav", -1, &audio) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == "channel_index must be an integer in [0, 2147483647]");
  CHECK(sonare_audio_file_num_channels("x.wav", nullptr) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == "out_channels must not be NULL");
#endif

  const std::vector<float> samples(22050, 0.1f);
  REQUIRE(sonare_audio_from_buffer(samples.data(), samples.size(), 22050, &audio) == SONARE_OK);
  SonareKey key{};
  CHECK(sonare_audio_detect_key(nullptr, &key) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == "audio must not be NULL");
  CHECK(sonare_audio_detect_key(audio, nullptr) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == "out_key must not be NULL");
  CHECK(sonare_audio_detect_bpm(audio, 60.0f, 200.0f, nullptr) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == "out_bpm must not be NULL");
  float* times = nullptr;
  CHECK(sonare_audio_detect_onsets(audio, &times, nullptr) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == "out_count must not be NULL");
  CHECK(sonare_audio_analyze(audio, nullptr) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == "out must not be NULL");
  sonare_audio_destroy(audio);
}

TEST_CASE("the chord entries refuse an option member and a chord element by its name",
          "[c_api][feature]") {
  const std::vector<float> samples(22050, 0.1f);
  SonareChordDetectionOptions options;
  sonare_chord_detection_options_init(&options);
  options.threshold = 0.5f;
  options.n_fft = 2048;
  options.hop_length = 512;
  SonareChordAnalysisResult result{};
  CHECK(sonare_detect_chords_ex(samples.data(), samples.size(), 22050, nullptr, &result) ==
        SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == "options must not be NULL");
  const uint32_t options_size = options.struct_size;
  options.struct_size = 0;
  CHECK(sonare_detect_chords_ex(samples.data(), samples.size(), 22050, &options, &result) ==
        SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == "options.struct_size must be set by sonare_chord_detection_options_init");
  options.struct_size = options_size;
  options.use_key_context = 1;
  options.key_root = static_cast<SonarePitchClass>(12);
  CHECK(sonare_detect_chords_ex(samples.data(), samples.size(), 22050, &options, &result) ==
        SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == "options.key_root must be an integer in [0, 11]");
  options.key_root = SONARE_PITCH_C;
  options.key_mode = static_cast<SonareMode>(7);
  CHECK(sonare_detect_chords_ex(samples.data(), samples.size(), 22050, &options, &result) ==
        SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == "options.key_mode must be an integer in [0, 6]");
  options.key_mode = SONARE_MODE_MAJOR;
  options.chroma_method = static_cast<SonareChromaMethod>(3);
  CHECK(sonare_detect_chords_ex(samples.data(), samples.size(), 22050, &options, &result) ==
        SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == "options.chroma_method must be an integer in [0, 2]");
  options.chroma_method = SONARE_CHROMA_METHOD_STFT;

  SonareStringArray roman{};
  SonareStringArray functions{};
  CHECK(sonare_chord_functional_analysis(
            samples.data(), samples.size(), 22050, &options, static_cast<SonarePitchClass>(12),
            SONARE_MODE_MAJOR, &result, &roman, &functions) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == "key_root must be an integer in [0, 11]");
  CHECK(sonare_chord_functional_analysis(samples.data(), samples.size(), 22050, &options,
                                         SONARE_PITCH_C, SONARE_MODE_MAJOR, nullptr, &roman,
                                         &functions) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == "chords must not be NULL");

  const SonareChord chords[2] = {
      {SONARE_PITCH_C, SONARE_CHORD_MAJOR, 0.0f, 1.0f, 1.0f, SONARE_PITCH_C},
      {SONARE_PITCH_G, static_cast<SonareChordQuality>(25), 1.0f, 2.0f, 1.0f, SONARE_PITCH_G},
  };
  CHECK(sonare_chord_functions(chords, 1, SONARE_PITCH_C, SONARE_MODE_MAJOR, nullptr, &functions) ==
        SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == "roman must not be NULL");
  CHECK(sonare_chord_functions(nullptr, 2, SONARE_PITCH_C, SONARE_MODE_MAJOR, &roman, &functions) ==
        SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == "chords must not be NULL when count is not 0");
  CHECK(sonare_chord_functions(chords, std::numeric_limits<size_t>::max() / 2, SONARE_PITCH_C,
                               SONARE_MODE_MAJOR, &roman,
                               &functions) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == "count must be an integer in [0, 500000000]");
  CHECK(sonare_chord_functions(chords, 1, SONARE_PITCH_C, static_cast<SonareMode>(-1), &roman,
                               &functions) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == "key_mode must be an integer in [0, 6]");
  CHECK(sonare_chord_functions(chords, 2, SONARE_PITCH_C, SONARE_MODE_MAJOR, &roman, &functions) ==
        SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == "chords[1].quality must be an integer in [0, 24]");
}

TEST_CASE("the segment entries refuse each matrix argument by its name", "[c_api][feature]") {
  const std::vector<float> features(2 * 3, 0.5f);
  SonareSegmentMatrix matrix{};
  SonareSegmentIndices indices{};

  CHECK(sonare_segment_cross_similarity(features.data(), 2, 3, features.data(), 2, 3, 0, "cosine",
                                        "affinity", nullptr) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == "out must not be NULL");
  CHECK(sonare_segment_cross_similarity(nullptr, 2, 3, features.data(), 2, 3, 0, "cosine",
                                        "affinity", &matrix) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == "x must not be NULL");
  CHECK(sonare_segment_cross_similarity(features.data(), 0, 3, features.data(), 2, 3, 0, "cosine",
                                        "affinity", &matrix) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == "x_rows must be an integer in [1, 2147483647]");
  CHECK(sonare_segment_cross_similarity(features.data(), 2, 3, features.data(), 2, 0, 0, "cosine",
                                        "affinity", &matrix) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == "y_cols must be an integer in [1, 2147483647]");
  CHECK(sonare_segment_cross_similarity(features.data(), 2, 3, features.data(), 1, 3, 0, "cosine",
                                        "affinity", &matrix) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == "y_rows must equal x_rows");
  CHECK(sonare_segment_cross_similarity(features.data(), 2, 3, features.data(), 2, 3, -1, "cosine",
                                        "affinity", &matrix) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == "k must be an integer in [0, 2147483647]");
  CHECK(sonare_segment_cross_similarity(features.data(), 2, 3, features.data(), 2, 3, 0, nullptr,
                                        "affinity", &matrix) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == "metric must not be NULL");
  CHECK(sonare_segment_cross_similarity(features.data(), 2, 3, features.data(), 2, 3, 0, "cosine",
                                        nullptr, &matrix) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == "mode must not be NULL");

  CHECK(sonare_segment_recurrence_matrix(features.data(), 1 << 16, 1 << 16, 0, 1, 1, "euclidean",
                                         "connectivity",
                                         &matrix) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == "rows * cols must be at most 500000000");
  CHECK(sonare_segment_recurrence_matrix(features.data(), 2, 3, 0, -1, 1, "euclidean",
                                         "connectivity",
                                         &matrix) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == "width must be an integer in [0, 2147483647]");
  CHECK(sonare_segment_recurrence_matrix(features.data(), 2, 3, 0, 1, 2, "euclidean",
                                         "connectivity",
                                         &matrix) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == "sym must be one of 0, 1");

  CHECK(sonare_segment_recurrence_to_lag(nullptr, 3, 1, &matrix) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == "recurrence must not be NULL");
  CHECK(sonare_segment_recurrence_to_lag(features.data(), 2, 2, &matrix) ==
        SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == "pad must be one of 0, 1");
  CHECK(sonare_segment_lag_to_recurrence(features.data(), 0, 3, &matrix) ==
        SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == "n_rows must be an integer in [1, 2147483647]");

  const std::vector<int> boundaries = {0, 3};
  CHECK(sonare_segment_subsegment(features.data(), 2, 3, nullptr, 2, 2, &indices) ==
        SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == "boundaries must not be NULL when boundary_count is not 0");
  CHECK(sonare_segment_subsegment(features.data(), 2, 3, boundaries.data(), boundaries.size(), 0,
                                  &indices) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == "n_segments must be an integer in [1, 2147483647]");
  CHECK(sonare_segment_agglomerative(features.data(), 2, 3, 0, "average", &indices) ==
        SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == "k must be an integer in [1, 2147483647]");
  CHECK(sonare_segment_agglomerative(features.data(), 2, 3, 2, nullptr, &indices) ==
        SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == "linkage must not be NULL");
  CHECK(sonare_segment_path_enhance(features.data(), 2, 1, 0, 0, 7, &matrix) ==
        SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == "max_ratio must be an integer in [1, 2147483647]");
  CHECK(sonare_segment_path_enhance(features.data(), 2, 1, 2, -1, 7, &matrix) ==
        SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == "min_ratio must be an integer in [0, 2147483647]");
}

TEST_CASE("the second-set entries name the first refused argument in header order",
          "[c_api][feature]") {
  const std::vector<float> samples(22050, 0.1f);
  const std::vector<float> features(4 * 9, 0.5f);
  CHECK(sonare_chroma_cqt(samples.data(), samples.size(), 22050, 0, 12, nullptr) ==
        SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == "hop_length must be an integer in [1, 65536]");
  CHECK(sonare_mel_delta(nullptr, 4, 9, 9, nullptr) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == "features must not be NULL");
  CHECK(sonare_segment_cross_similarity(nullptr, 2, 3, features.data(), 2, 3, 0, "cosine",
                                        "affinity", nullptr) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == "x must not be NULL");
  CHECK(sonare_audio_from_memory(nullptr, 1, nullptr) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == "data must not be NULL when length is not 0");
  CHECK(sonare_detect_chords_ex(samples.data(), samples.size(), 22050, nullptr, nullptr) ==
        SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == "options must not be NULL");
  SonareStreamConfig config;
  sonare_stream_config_init(&config);
  config.sample_rate = 0;
  CHECK(sonare_stream_analyzer_create(&config, nullptr) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == "config.sample_rate must be an integer in [1, 2147483647]");
  SonareStreamConfig unset{};
  CHECK(sonare_stream_analyzer_create(&unset, nullptr) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == "config.struct_size must be set by sonare_stream_config_init");
  CHECK(sonare_stream_analyzer_read_frames(nullptr, 1, nullptr) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == "analyzer must not be NULL");
}

TEST_CASE("the effects and metering entries refuse their own members", "[c_api][effects]") {
  const std::vector<float> samples(22050, 0.1f);
  float* out = nullptr;
  size_t out_length = 0;
  CHECK(sonare_phase_vocoder(samples.data(), samples.size(), 22050, 0.0f, 2048, 512, &out,
                             &out_length) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == "rate must be finite and greater than 0");

  const std::vector<float> s(8, 1.0f);
  float *w = nullptr, *h = nullptr;
  size_t w_length = 0, h_length = 0;
  const auto decompose = [&](int n_features, int n_frames, int n_components, int n_iter) {
    CHECK(sonare_decompose_with_init(s.data(), n_features, n_frames, n_components, n_iter, 2.0f,
                                     "random", &w, &w_length, &h,
                                     &h_length) == SONARE_ERROR_INVALID_PARAMETER);
    return last_message();
  };
  CHECK(decompose(0, 4, 2, 5) == "n_features must be an integer in [1, 2147483647]");
  CHECK(decompose(2, 0, 2, 5) == "n_frames must be an integer in [1, 2147483647]");
  CHECK(decompose(2, 4, 0, 5) == "n_components must be an integer in [1, 2147483647]");
  CHECK(decompose(2, 4, 2, -1) == "n_iter must be an integer in [0, 2147483647]");

  SonareWaveformPeaksResult peaks{};
  CHECK(sonare_waveform_peaks(samples.data(), samples.size(), 1, 0, &peaks) ==
        SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == "samples_per_bucket must be an integer of at least 1");
  const size_t levels[] = {64, 0};
  SonareWaveformPeakPyramidResult pyramid{};
  CHECK(sonare_waveform_peak_pyramid(samples.data(), samples.size(), 1, levels, 2, &pyramid) ==
        SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == "samples_per_bucket must be an integer of at least 1");
}

namespace {
SonareMidiRouteConfig route_config(int group, int channel, int remap) {
  SonareMidiRouteConfig config;
  sonare_midi_route_config_init(&config);
  config.filter_group = group;
  config.filter_channel = channel;
  config.remap_channel = remap;
  return config;
}
}  // namespace

TEST_CASE("the project MIDI entries name a route field and a bank outside their domains",
          "[c_api][project]") {
  size_t count = 0;
  for (const SonareMidiRouteConfig& config :
       {route_config(16, SONARE_MIDI_ROUTE_ANY, SONARE_MIDI_ROUTE_NO_REMAP),
        route_config(SONARE_MIDI_ROUTE_ANY, -2, SONARE_MIDI_ROUTE_NO_REMAP),
        route_config(SONARE_MIDI_ROUTE_ANY, SONARE_MIDI_ROUTE_ANY, 16)}) {
    CHECK(sonare_midi_route_events(nullptr, 0, &config, nullptr, 0, &count, nullptr, nullptr) ==
          SONARE_ERROR_INVALID_PARAMETER);
    CHECK_THAT(last_message(),
               Catch::Matchers::ContainsSubstring("must be -1 or an integer in [0, 15]"));
  }
  const SonareMidiRouteConfig remap =
      route_config(SONARE_MIDI_ROUTE_ANY, SONARE_MIDI_ROUTE_ANY, 16);
  CHECK(sonare_midi_route_events(nullptr, 0, &remap, nullptr, 0, &count, nullptr, nullptr) ==
        SONARE_ERROR_INVALID_PARAMETER);
  CHECK_THAT(last_message(), Catch::Matchers::ContainsSubstring("config.remap_channel must be"));
  SonareMidiRouteConfig unsized = remap;
  unsized.struct_size = sizeof(unsized) - 1;
  CHECK(sonare_midi_route_events(nullptr, 0, &unsized, nullptr, 0, &count, nullptr, nullptr) ==
        SONARE_ERROR_INVALID_PARAMETER);
  CHECK_THAT(
      last_message(),
      Catch::Matchers::Equals("config.struct_size must be set by sonare_midi_route_config_init"));

  SonareProject* project = nullptr;
  REQUIRE(sonare_project_create(&project) == SONARE_OK);
  uint32_t track = 0;
  uint32_t clip = 0;
  REQUIRE(sonare_project_add_midi_clip(project, 0.0, 2.0, &track, &clip) == SONARE_OK);
  for (const int bank : {-2, 16384}) {
    CHECK(sonare_project_set_program_on_channel(project, clip, 0, 0, 0, bank) ==
          SONARE_ERROR_INVALID_PARAMETER);
    CHECK_THAT(last_message(),
               Catch::Matchers::ContainsSubstring("bank must be -1 or an integer in [0, 16383]"));
  }
  CHECK(sonare_project_set_program_on_channel(project, clip, 0, 0, 0, -1) == SONARE_OK);
  sonare_project_destroy(project);
}

namespace {

/// Drives one C entry to a refusal naming the subject its spelling-table row renames; null for
/// an entry that refuses no value of that subject.
const std::map<std::string_view, std::function<void()>>& spelling_row_drivers() {
  static const std::map<std::string_view, std::function<void()>> drivers = {
      {"align_take_to_reference",
       [] {
         // 8000 Hz carries only 12 bins per octave, so 60 is refused against the reference rate.
         std::vector<float> audio(8000, 0.1f);
         SonareTakeAlignConfig config{};
         config.bins_per_octave = 60;
         SonareProjectWarpAnchor* anchors = nullptr;
         size_t count = 0;
         sonare_align_take_to_reference_ex(audio.data(), audio.size(), 8000, audio.data(),
                                           audio.size(), 8000, &config, &anchors, &count, nullptr);
       }},
      {"downmix",
       [] {
         const float samples[2] = {0.1f, 0.1f};
         float* out = nullptr;
         sonare_downmix(samples, 2, 0, 0, nullptr, &out);
       }},
      {"mel_to_audio",
       [] {
         float* out = nullptr;
         size_t out_length = 0;
         sonare_mel_to_audio(nullptr, 2, 2, 22050, 2048, 512, 0.0f, 0.0f, 1, &out, &out_length);
       }},
      {"mel_to_stft",
       [] {
         SonareInverseResult out{};
         sonare_mel_to_stft(nullptr, 2, 2, 22050, 2048, 0.0f, 0.0f, &out);
       }},
      // Every offset is accepted (one past the end reads silence), so no refusal names it.
      {"metering_spectrum_frame", nullptr},
      // A null buffer is refused as an unreadable document, never as a parameter.
      {"note_targets_from_smf", nullptr},
      // An unknown preset is refused as a document the chain loader cannot read, never as a
      // parameter, and a null one selects the default.
      {"voice_change_realtime", nullptr},
      {"waveform_peaks",
       [] {
         std::vector<float> samples(1024, 0.1f);
         SonareWaveformPeaksResult out{};
         sonare_waveform_peaks(samples.data(), samples.size(), 0, 512, &out);
       }},
      {"waveform_peak_pyramid",
       [] {
         std::vector<float> samples(1024, 0.1f);
         const size_t levels[] = {512};
         SonareWaveformPeakPyramidResult out{};
         sonare_waveform_peak_pyramid(samples.data(), samples.size(), 0, levels, 1, &out);
       }},
  };
  return drivers;
}

std::vector<std::string> last_refusal_subjects() {
  std::vector<std::string> subjects;
  for (size_t i = 0; i < sonare_last_refusal_subject_count(); ++i) {
    subjects.emplace_back(sonare_last_refusal_subject(i));
  }
  return subjects;
}

template <typename Overrides, typename Spell>
void check_spelling_rows(const Overrides& rows, const Spell& spelled_last_refusal) {
  for (const auto& row : rows) {
    const std::string entry(row.entry);
    CAPTURE(entry, std::string(row.subject), std::string(row.spelling));
    const auto driver = spelling_row_drivers().find(row.entry);
    REQUIRE(driver != spelling_row_drivers().end());
    if (!driver->second) continue;
    driver->second();
    const char* refused = sonare_last_refusal_entry();
    REQUIRE(refused != nullptr);
    CHECK(std::string(refused) == entry);
    const std::vector<std::string> subjects = last_refusal_subjects();
    REQUIRE(std::find(subjects.begin(), subjects.end(), std::string(row.subject)) !=
            subjects.end());
    const auto shown = spelled_last_refusal();
    REQUIRE(shown.has_value());
    CHECK_THAT(std::string(shown->what()),
               Catch::Matchers::ContainsSubstring(std::string(row.spelling)));
  }
}

}  // namespace

TEST_CASE("every renamed refusal subject reaches the caller under the facade's spelling",
          "[c_api][field_spelling]") {
  SECTION("node") {
    check_spelling_rows(sonare_field_spelling::node::kOverrides,
                        [] { return sonare_field_spelling::node::SpelledLastRefusal(); });
  }
  SECTION("wasm") {
    check_spelling_rows(sonare_field_spelling::wasm::kOverrides,
                        [] { return sonare_field_spelling::wasm::SpelledLastRefusal(); });
  }
}
