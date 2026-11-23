/// @file c_refusal_subjects_test.cpp
/// @brief C entries refuse their own arguments under the entry's name without `sonare_`.

#include <sonare/sonare_c.h>

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>
#include <cmath>
#include <cstdint>
#include <functional>
#include <limits>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "../../bindings/node/src/addon/field_spelling.h"
#include "util/constants.h"
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
  CHECK(last_message() ==
        "out_momentary_lufs_length must not be NULL when out_momentary_lufs is not NULL");
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
  CHECK(sonare_align_take_to_reference(reference.data(), reference.size(), 22050, take.data(),
                                       take.size(), 22050, nullptr, &anchors, &count,
                                       &alignment) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK_THAT(last_message(), Catch::Matchers::StartsWith("take "));

  reference[5] = std::numeric_limits<float>::quiet_NaN();
  CHECK(sonare_align_take_to_reference(reference.data(), reference.size(), 22050, take.data(),
                                       take.size(), 22050, nullptr, &anchors, &count,
                                       &alignment) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK_THAT(last_message(), Catch::Matchers::StartsWith("reference "));

  reference[5] = 0.0f;
  take[100] = 0.0f;
  CHECK(sonare_align_take_to_reference(reference.data(), reference.size(), 22050, take.data(),
                                       take.size(), 22050, nullptr, nullptr, &count,
                                       &alignment) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == "out_anchors must not be NULL");

  SonareTakeAlignConfig config;
  sonare_take_align_config_init(&config);
  config.bins_per_octave = 18;
  CHECK(sonare_align_take_to_reference(reference.data(), reference.size(), 22050, take.data(),
                                       take.size(), 22050, &config, nullptr, &count,
                                       &alignment) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == "config.bins_per_octave must be a multiple of 12");
  config.bins_per_octave = -12;
  CHECK(sonare_align_take_to_reference(reference.data(), reference.size(), 22050, take.data(),
                                       take.size(), 22050, &config, &anchors, &count,
                                       &alignment) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == "config.bins_per_octave must be an integer in [1, 2147483647]");
  sonare_take_align_config_init(&config);
  for (const int hop : {0, -256, 65537}) {
    config.hop_length = hop;
    CHECK(sonare_align_take_to_reference(reference.data(), reference.size(), 22050, take.data(),
                                         take.size(), 22050, &config, &anchors, &count,
                                         &alignment) == SONARE_ERROR_INVALID_PARAMETER);
    CHECK(last_message() == "config.hop_length must be an integer in [1, 65536]");
  }
  // The CQT's fmin and n_bins are derived from these two, so its Nyquist refusal names them.
  sonare_take_align_config_init(&config);
  config.bins_per_octave = 24;
  CHECK(sonare_align_take_to_reference(reference.data(), reference.size(), 8000, take.data(),
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
  // The inverse reads gamma exactly as the forward transform does: NaN is automatic.
  const std::vector<float> magnitude(24 * 4, 0.1f);
  float* audio = nullptr;
  size_t audio_length = 0;
  for (const float bad_gamma : {std::numeric_limits<float>::infinity(), -1.0f}) {
    CHECK(sonare_vqt_to_audio(magnitude.data(), magnitude.size(), 24, 4, 22050, 512, 32.7f, 12,
                              bad_gamma, 1, &audio,
                              &audio_length) == SONARE_ERROR_INVALID_PARAMETER);
    CHECK(last_message() == "gamma must be finite and at least 0");
    CHECK(audio == nullptr);
  }
  REQUIRE(sonare_vqt_to_audio(magnitude.data(), magnitude.size(), 24, 4, 22050, 512, 32.7f, 12,
                              std::numeric_limits<float>::quiet_NaN(), 1, &audio,
                              &audio_length) == SONARE_OK);
  sonare_free_floats(audio);
}

TEST_CASE("the chroma-from-CQT entries restate the derived grid in their own arguments",
          "[c_api][feature]") {
  const std::vector<float> samples(22050, 0.1f);
  SonareChromaResult out{};
  const auto run = [&](bool cens, int sample_rate, int bins_per_octave) {
    if (cens) {
      SonareChromaCensOptions options;
      sonare_chroma_cens_options_init(&options);
      options.bins_per_octave = bins_per_octave;
      return sonare_chroma_cens(samples.data(), samples.size(), sample_rate, 512, 12, &options,
                                &out);
    }
    SonareChromaCqtOptions options;
    sonare_chroma_cqt_options_init(&options);
    options.bins_per_octave = bins_per_octave;
    return sonare_chroma_cqt(samples.data(), samples.size(), sample_rate, 512, 12, &options, &out);
  };
  for (const bool cens : {false, true}) {
    CHECK(run(cens, 8000, 36) == SONARE_ERROR_INVALID_PARAMETER);
    CHECK(last_message() ==
          "options.bins_per_octave puts the top chroma bin at or above half of sample_rate");
    CHECK(run(cens, 22050, 96) == SONARE_ERROR_INVALID_PARAMETER);
    CHECK(last_message() ==
          "options.bins_per_octave puts more bins in the CQT kernel than fit at sample_rate");
    CHECK(run(cens, 22050, 1200) == SONARE_ERROR_INVALID_PARAMETER);
    CHECK(last_message() ==
          "options.bins_per_octave must keep every filter at most 131072 samples long at "
          "sample_rate");
  }
}

TEST_CASE("the CQT inverse refuses each member by its core name", "[c_api][feature]") {
  const std::vector<float> magnitude(24 * 4, 1.0f);
  float* out = nullptr;
  size_t out_length = 0;
  const auto message = [&](int n_bins, int n_frames, int sample_rate, int hop, float fmin, int bpo,
                           int n_iter) {
    CHECK(sonare_cqt_to_audio(magnitude.data(), magnitude.size(), n_bins, n_frames, sample_rate,
                              hop, fmin, bpo, n_iter, &out,
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
  CHECK(sonare_vqt_to_audio(magnitude.data(), magnitude.size(), 24, 4, 22050, 512, 0.01f, 12, 0.0f,
                            4, &out, &out_length) == SONARE_ERROR_INVALID_PARAMETER);
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
                              nullptr, &melody) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == "f0_min_hz must be finite and greater than 0");
  SonareAnalyzeMelodyOptions melody_options;
  sonare_analyze_melody_options_init(&melody_options);
  melody_options.use_pyin = 1;
  CHECK(sonare_analyze_melody(samples.data(), samples.size(), 22050, 100.0f, 100.0f, 2048, 256,
                              0.1f, &melody_options, &melody) == SONARE_ERROR_INVALID_PARAMETER);
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

  CHECK(sonare_stft(samples.data(), samples.size(), 22050, 2048, 512, nullptr, nullptr) ==
        SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == "out must not be NULL");

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
  CHECK(sonare_audio_from_file(nullptr, nullptr, &audio) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == "path must not be NULL");
  SonareAudioFileOptions file_options;
  sonare_audio_file_options_init(&file_options);
  file_options.channel_index = -2;
  CHECK(sonare_audio_from_file("x.wav", &file_options, &audio) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == "options.channel_index must be an integer in [0, 2147483647]");
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
  CHECK(sonare_detect_chords(samples.data(), samples.size(), 22050, nullptr, &result) == SONARE_OK);
  sonare_free_chord_analysis_result(&result);
  const uint32_t options_size = options.struct_size;
  options.struct_size = 0;
  CHECK(sonare_detect_chords(samples.data(), samples.size(), 22050, &options, &result) ==
        SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == "options.struct_size must be set by sonare_chord_detection_options_init");
  options.struct_size = options_size;
  options.use_key_context = 1;
  options.key_root = static_cast<SonarePitchClass>(12);
  CHECK(sonare_detect_chords(samples.data(), samples.size(), 22050, &options, &result) ==
        SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == "options.key_root must be an integer in [0, 11]");
  options.key_root = SONARE_PITCH_C;
  options.key_mode = static_cast<SonareMode>(7);
  CHECK(sonare_detect_chords(samples.data(), samples.size(), 22050, &options, &result) ==
        SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == "options.key_mode must be an integer in [0, 6]");
  options.key_mode = SONARE_MODE_MAJOR;
  options.chroma_method = static_cast<SonareChromaMethod>(3);
  CHECK(sonare_detect_chords(samples.data(), samples.size(), 22050, &options, &result) ==
        SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == "options.chroma_method must be an integer in [0, 2]");
  options.chroma_method = SONARE_CHROMA_METHOD_STFT;
  options.min_duration_sec = -1.0f;
  CHECK(sonare_detect_chords(samples.data(), samples.size(), 22050, &options, &result) ==
        SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message().rfind("options.min_duration_sec ", 0) == 0);
  options.min_duration_sec = 0.0f;
  options.hmm_beam_width = -1;
  CHECK(sonare_detect_chords(samples.data(), samples.size(), 22050, &options, &result) ==
        SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message().rfind("options.hmm_beam_width ", 0) == 0);
  options.hmm_beam_width = 0;
  // The chord chroma runs at four times the hop, which has to stay a constant-Q hop.
  options.hop_length = 16385;
  CHECK(sonare_detect_chords(samples.data(), samples.size(), 22050, &options, &result) ==
        SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == "options.hop_length must be an integer in [1, 16384]");
  options.hop_length = 512;
  options.tuning_auto = 0;
  options.tuning_cents = 50.0f;
  CHECK(sonare_detect_chords(samples.data(), samples.size(), 22050, &options, &result) ==
        SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == "options.tuning_cents must be finite and in [-50, 50)");
  options.tuning_cents = 0.0f;

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

TEST_CASE("the key entries refuse their FFT, hop and high-pass options by their names",
          "[c_api][feature]") {
  const std::vector<float> samples(22050, 0.1f);
  SonareDetectKeyOptions options;
  sonare_detect_key_options_init(&options);
  SonareKey key{};
  for (const int n_fft : {1000000000, 2049}) {
    options.n_fft = n_fft;
    CHECK(sonare_detect_key(samples.data(), samples.size(), 22050, &options, &key, nullptr) ==
          SONARE_ERROR_INVALID_PARAMETER);
    CHECK(last_message() == "options.n_fft must be an even integer in [2, 524288]");
  }
  options.n_fft = 2048;
  options.hop_length = 16385;
  CHECK(sonare_detect_key(samples.data(), samples.size(), 22050, &options, &key, nullptr) ==
        SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == "options.hop_length must be an integer in [1, 16384]");
  options.hop_length = 512;
  options.chroma_high_pass_hz = 11025.0f;
  CHECK(sonare_detect_key(samples.data(), samples.size(), 22050, &options, &key, nullptr) ==
        SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == "options.chroma_high_pass_hz must be below half of sample_rate");
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
  CHECK(sonare_chroma_cqt(samples.data(), samples.size(), 22050, 0, 12, nullptr, nullptr) ==
        SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == "hop_length must be an integer in [1, 65536]");
  CHECK(sonare_mel_delta(nullptr, 4, 9, 9, nullptr) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == "features must not be NULL");
  CHECK(sonare_segment_cross_similarity(nullptr, 2, 3, features.data(), 2, 3, 0, "cosine",
                                        "affinity", nullptr) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == "x must not be NULL");
  CHECK(sonare_audio_from_memory(nullptr, 1, nullptr) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == "data must not be NULL when length is not 0");
  CHECK(sonare_detect_chords(samples.data(), samples.size(), 22050, nullptr, nullptr) ==
        SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == "out must not be NULL");
  SonareStreamConfig config;
  sonare_stream_config_init(&config);
  config.sample_rate = 0;
  CHECK(sonare_stream_analyzer_create(&config, nullptr) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == "config.sample_rate must be an integer in [8000, 384000]");
  SonareStreamConfig unset{};
  CHECK(sonare_stream_analyzer_create(&unset, nullptr) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == "config.struct_size must be set by sonare_stream_config_init");
  CHECK(sonare_stream_analyzer_read_frames(nullptr, 1, nullptr) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == "analyzer must not be NULL");
}

TEST_CASE("pitch shift names the refused semitone argument",
          "[c_api][effects][c_refusal_subjects]") {
  const float samples[] = {0.25f, -0.25f};
  for (float semitones : {std::numeric_limits<float>::infinity(), 1200.0f}) {
    CAPTURE(semitones);
    float* out = nullptr;
    size_t length = 0;
    REQUIRE(sonare_pitch_shift(samples, 2, 48000, semitones, nullptr, &out, &length) ==
            SONARE_ERROR_INVALID_PARAMETER);
    CHECK_THAT(last_message(), Catch::Matchers::StartsWith("pitch_semitones must "));
    CHECK(out == nullptr);
    CHECK(length == 0);
  }
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
    SonareDecomposeOptions options;
    sonare_decompose_options_init(&options);
    options.init = "random";
    CHECK(sonare_decompose(s.data(), n_features, n_frames, n_components, n_iter, 2.0f, &options, &w,
                           &w_length, &h, &h_length) == SONARE_ERROR_INVALID_PARAMETER);
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
    CHECK(sonare_project_set_program(project, clip, 0, 0, 0, bank) ==
          SONARE_ERROR_INVALID_PARAMETER);
    CHECK_THAT(last_message(),
               Catch::Matchers::ContainsSubstring("bank must be -1 or an integer in [0, 16383]"));
  }
  CHECK(sonare_project_set_program(project, clip, 0, 0, 0, -1) == SONARE_OK);
  sonare_project_destroy(project);
}

namespace {

using EnginePtr = std::unique_ptr<SonareEngine, decltype(&sonare_engine_destroy)>;
using MixerPtr = std::unique_ptr<SonareMixer, decltype(&sonare_mixer_destroy)>;
using ProjectPtr = std::unique_ptr<SonareProject, decltype(&sonare_project_destroy)>;
using MeterPtr =
    std::unique_ptr<SonarePlaybackLoudnessMeter, decltype(&sonare_playback_loudness_meter_destroy)>;

EnginePtr prepared_engine() {
  SonareEngine* raw = nullptr;
  REQUIRE(sonare_engine_create(&raw) == SONARE_OK);
  EnginePtr engine(raw, sonare_engine_destroy);
  REQUIRE(sonare_engine_prepare(raw, 48000, 256, 64, 64, 0) == SONARE_OK);
  return engine;
}

MixerPtr new_mixer() {
  SonareMixer* raw = nullptr;
  REQUIRE(sonare_mixer_create(48000, 0, &raw) == SONARE_OK);
  return MixerPtr(raw, sonare_mixer_destroy);
}

ProjectPtr new_project() {
  SonareProject* raw = nullptr;
  REQUIRE(sonare_project_create(&raw) == SONARE_OK);
  return ProjectPtr(raw, sonare_project_destroy);
}

/// One spelling-table row's driver: @p run makes the refusal, @p c_root is the prefix the C
/// subject carries over the row's (empty when it carries none).
struct SpellingDriver {
  std::function<void()> run;
  const char* c_root = "";
};

/// Drives one C entry to a refusal naming the subject its spelling-table row renames, keyed
/// "<entry>.<subject>"; null for a row no value of that subject is refused under.
const std::map<std::string, SpellingDriver>& spelling_row_drivers() {
  static const std::map<std::string, SpellingDriver> drivers = {
      {"align_take_to_reference.reference_sample_rate", {[] {
         // 8000 Hz carries only 12 bins per octave, so 60 is refused against the reference rate.
         std::vector<float> audio(8000, 0.1f);
         SonareTakeAlignConfig config;
         sonare_take_align_config_init(&config);
         config.bins_per_octave = 60;
         SonareProjectWarpAnchor* anchors = nullptr;
         size_t count = 0;
         sonare_align_take_to_reference(audio.data(), audio.size(), 8000, audio.data(),
                                        audio.size(), 8000, &config, &anchors, &count, nullptr);
       }}},
      // Every offset is accepted (one past the end reads silence), so no refusal names it.
      {"audio_spectrum_frame.frame_offset", {nullptr}},
      {"downmix.num_channels", {[] {
         const float samples[2] = {0.1f, 0.1f};
         float* out = nullptr;
         sonare_downmix(samples, 2, 0, 0, nullptr, &out);
       }}},
      {"encode_audio.num_channels", {[] {
         const float samples[2] = {0.1f, 0.1f};
         uint8_t* out = nullptr;
         size_t out_size = 0;
         sonare_encode_audio(samples, 2, 0, 44100, nullptr, &out, &out_size);
       }}},
      {"engine_drain_external_midi.max_events", {[] {
         // A buffer shorter than one lowered record is refused.
         const EnginePtr engine = prepared_engine();
         SonareExternalMidiEvent events[1] = {};
         size_t count = 0;
         sonare_engine_drain_external_midi(engine.get(), events, 1, &count);
       }}},
      // A realtime entry refuses a bad count without recording a subject.
      {"engine_process.num_channels", {nullptr}},
      {"engine_resolve_master_automation_id.param_name", {[] {
         const EnginePtr engine = prepared_engine();
         uint32_t id = 0;
         sonare_engine_resolve_master_automation_id(engine.get(), "unknown", &id);
       }}},
      {"engine_resolve_track_lane_automation_id.param_name", {[] {
         const EnginePtr engine = prepared_engine();
         uint32_t id = 0;
         sonare_engine_resolve_track_lane_automation_id(engine.get(), 1, "unknown", &id);
       }}},
      // A flag: every int is accepted.
      {"engine_set_controller_velocity_meaningful.meaningful", {nullptr}},
      // A flag: every int is accepted.
      {"engine_set_external_midi_clock_enabled.enabled", {nullptr}},
      // A flag: every int is accepted.
      {"engine_set_midi_destination_external.external", {nullptr}},
      {"engine_set_record_offset_samples.offset_samples", {[] {
         const EnginePtr engine = prepared_engine();
         REQUIRE(sonare_engine_set_loop(engine.get(), 0.0, 1.0, 1) == SONARE_OK);
         float samples[256] = {};
         float* channels[] = {samples};
         const SonareEngineCaptureBuffer buffer{channels, 1, 256};
         REQUIRE(sonare_engine_set_capture_buffer(engine.get(), &buffer) == SONARE_OK);
         REQUIRE(sonare_engine_set_capture_punch(engine.get(), 0, 256, 1) == SONARE_OK);
         REQUIRE(sonare_engine_arm_capture(engine.get(), 1) == SONARE_OK);
         REQUIRE(sonare_engine_set_record_offset_samples(engine.get(),
                                                         std::numeric_limits<int64_t>::max()) ==
                 SONARE_ERROR_INVALID_PARAMETER);
         CHECK_THAT(last_message(), Catch::Matchers::StartsWith("offset_samples must "));
       }}},
      {"engine_apply_track_strip_insert_param_by_name_now.track_id", {[] {
         const EnginePtr engine = prepared_engine();
         int applied = 0;
         sonare_engine_apply_track_strip_insert_param_by_name_now(engine.get(), 0, 0, "gain", 0.0f,
                                                                  &applied);
       }}},
      // A query: the answer comes back through out_refusal, so no track id is refused.
      {"engine_can_set_lane_sidechain.track_id", {nullptr}},
      {"engine_clear_track_insert_parameter_bases.track_id", {[] {
         const EnginePtr engine = prepared_engine();
         sonare_engine_clear_track_insert_parameter_bases(engine.get(), 99);
       }}},
      {"engine_resolve_track_insert_automation_id.track_id", {[] {
         const EnginePtr engine = prepared_engine();
         uint32_t id = 0;
         sonare_engine_resolve_track_insert_automation_id(engine.get(), 0, 0, "gain", &id);
       }}},
      {"engine_resolve_track_lane_automation_id.track_id", {[] {
         const EnginePtr engine = prepared_engine();
         uint32_t id = 0;
         sonare_engine_resolve_track_lane_automation_id(engine.get(), 0, "volume", &id);
       }}},
      {"engine_restore_track_strip_insert_param_by_name.track_id", {[] {
         const EnginePtr engine = prepared_engine();
         sonare_engine_restore_track_strip_insert_param_by_name(engine.get(), 0, 0, "gain", 0.0f);
       }}},
      {"engine_set_lane_sidechain.track_id", {[] {
         const EnginePtr engine = prepared_engine();
         sonare_engine_set_lane_sidechain(engine.get(), 0, 0, 98);
       }}},
      {"engine_set_track_strip_channel_delay_samples.track_id", {[] {
         const EnginePtr engine = prepared_engine();
         sonare_engine_set_track_strip_channel_delay_samples(engine.get(), 0, 0);
       }}},
      {"engine_set_track_strip_dual_pan.track_id", {[] {
         const EnginePtr engine = prepared_engine();
         sonare_engine_set_track_strip_dual_pan(engine.get(), 0, 0.0f, 0.0f);
       }}},
      {"engine_set_track_strip_eq_band_json.track_id", {[] {
         const EnginePtr engine = prepared_engine();
         sonare_engine_set_track_strip_eq_band_json(engine.get(), 0, 0, "{}");
       }}},
      {"engine_set_track_strip_insert_bypassed.track_id", {[] {
         const EnginePtr engine = prepared_engine();
         sonare_engine_set_track_strip_insert_bypassed(engine.get(), 0, 0, 1, 0);
       }}},
      {"engine_set_track_strip_insert_param_by_name.track_id", {[] {
         const EnginePtr engine = prepared_engine();
         sonare_engine_set_track_strip_insert_param_by_name(engine.get(), 0, 0, "gain", 0.0f);
       }}},
      {"engine_set_track_strip_json.track_id", {[] {
         const EnginePtr engine = prepared_engine();
         sonare_engine_set_track_strip_json(engine.get(), 0, "{\"id\":\"a\"}");
       }}},
      {"engine_set_track_strip_pan.track_id", {[] {
         const EnginePtr engine = prepared_engine();
         sonare_engine_set_track_strip_pan(engine.get(), 0, 0.0f);
       }}},
      {"engine_set_track_strip_pan_law.track_id", {[] {
         const EnginePtr engine = prepared_engine();
         sonare_engine_set_track_strip_pan_law(engine.get(), 0, 0);
       }}},
      {"engine_set_track_strip_pan_mode.track_id", {[] {
         const EnginePtr engine = prepared_engine();
         sonare_engine_set_track_strip_pan_mode(engine.get(), 0, 0);
       }}},
      {"engine_set_track_strip_surround_pan.track_id", {[] {
         const EnginePtr engine = prepared_engine();
         SonareSurroundPan pan{};
         sonare_engine_set_track_strip_surround_pan(engine.get(), 0, &pan);
       }}},
      {"engine_set_bus_strip_json.scene_json", {[] {
         const EnginePtr engine = prepared_engine();
         sonare_engine_set_bus_strip_json(engine.get(), 1, nullptr);
       }}},
      {"engine_set_master_strip_json.scene_json", {[] {
         const EnginePtr engine = prepared_engine();
         sonare_engine_set_master_strip_json(engine.get(), nullptr);
       }}},
      {"engine_set_part_rig.inserts_json", {[] {
         const EnginePtr engine = prepared_engine();
         sonare_engine_set_part_rig(engine.get(), 1, 0, SONARE_PART_RIG_CHAIN, nullptr);
       }}},
      {"engine_set_track_strip_json.scene_json", {[] {
         const EnginePtr engine = prepared_engine();
         sonare_engine_set_track_strip_json(engine.get(), 1, nullptr);
       }}},
      {"project_set_part_rig.inserts_json", {[] {
         const ProjectPtr project = new_project();
         sonare_project_set_part_rig(project.get(), 1, 0, SONARE_PART_RIG_CHAIN, nullptr);
       }}},
      {"engine_set_bus_strip_insert_param_by_name.param_name", {[] {
         const EnginePtr engine = prepared_engine();
         sonare_engine_set_bus_strip_insert_param_by_name(engine.get(), 1, 0, "", 0.0f);
       }}},
      {"engine_set_master_strip_insert_param_by_name.param_name", {[] {
         const EnginePtr engine = prepared_engine();
         sonare_engine_set_master_strip_insert_param_by_name(engine.get(), 0, "", 0.0f);
       }}},
      {"engine_set_track_strip_insert_param_by_name.param_name", {[] {
         const EnginePtr engine = prepared_engine();
         sonare_engine_set_track_strip_insert_param_by_name(engine.get(), 1, 0, "", 0.0f);
       }}},
      {"engine_restore_bus_strip_insert_param_by_name.param_name", {[] {
         const EnginePtr engine = prepared_engine();
         sonare_engine_restore_bus_strip_insert_param_by_name(engine.get(), 1, 0, "", 0.0f);
       }}},
      {"engine_restore_master_strip_insert_param_by_name.param_name", {[] {
         const EnginePtr engine = prepared_engine();
         sonare_engine_restore_master_strip_insert_param_by_name(engine.get(), 0, "", 0.0f);
       }}},
      {"engine_restore_track_strip_insert_param_by_name.param_name", {[] {
         const EnginePtr engine = prepared_engine();
         sonare_engine_restore_track_strip_insert_param_by_name(engine.get(), 1, 0, "", 0.0f);
       }}},
      {"engine_apply_bus_strip_insert_param_by_name_now.param_name", {[] {
         const EnginePtr engine = prepared_engine();
         int applied = 0;
         sonare_engine_apply_bus_strip_insert_param_by_name_now(engine.get(), 1, 0, "", 0.0f,
                                                                &applied);
       }}},
      {"engine_apply_master_strip_insert_param_by_name_now.param_name", {[] {
         const EnginePtr engine = prepared_engine();
         int applied = 0;
         sonare_engine_apply_master_strip_insert_param_by_name_now(engine.get(), 0, "", 0.0f,
                                                                   &applied);
       }}},
      {"engine_apply_track_strip_insert_param_by_name_now.param_name", {[] {
         const EnginePtr engine = prepared_engine();
         int applied = 0;
         sonare_engine_apply_track_strip_insert_param_by_name_now(engine.get(), 1, 0, "", 0.0f,
                                                                  &applied);
       }}},
      // The queries answer through out_refusal, so they refuse no source.
      {"engine_can_set_bus_sidechain.source_id", {nullptr}},
      {"engine_can_set_bus_sidechain.source_kind", {nullptr}},
      {"engine_can_set_lane_sidechain.source_track_id", {nullptr}},
      {"engine_can_set_master_sidechain.source_id", {nullptr}},
      {"engine_can_set_master_sidechain.source_kind", {nullptr}},
      {"engine_set_bus_sidechain.source_kind", {[] {
         const EnginePtr engine = prepared_engine();
         sonare_engine_set_bus_sidechain(engine.get(), 1, 0, 2, 1);
       }}},
      {"engine_set_master_sidechain.source_kind", {[] {
         const EnginePtr engine = prepared_engine();
         sonare_engine_set_master_sidechain(engine.get(), 0, 2, 1);
       }}},
      {"engine_set_bus_sidechain.source_id", {[] {
         const EnginePtr engine = prepared_engine();
         const SonareEngineBus bus[] = {{1, 0.0f, SONARE_CHANNEL_LAYOUT_STEREO, 0, nullptr, 0}};
         REQUIRE(sonare_engine_set_track_buses(engine.get(), bus, 1) == SONARE_OK);
         REQUIRE(sonare_engine_set_bus_strip_json(
                     engine.get(), 1,
                     "{\"id\":\"1\",\"inserts\":[{\"slot\":\"pre\","
                     "\"processor\":\"dynamics.duckingProcessor\",\"params\":{}}]}") == SONARE_OK);
         sonare_engine_set_bus_sidechain(engine.get(), 1, 0, SONARE_SIDECHAIN_SOURCE_TRACK, 99);
       }}},
      {"engine_set_lane_sidechain.source_track_id", {[] {
         const EnginePtr engine = prepared_engine();
         const SonareEngineTrackLane lane[] = {{1, nullptr, 0, 0, SONARE_CHANNEL_LAYOUT_STEREO}};
         REQUIRE(sonare_engine_set_track_lanes(engine.get(), lane, 1) == SONARE_OK);
         REQUIRE(sonare_engine_set_track_strip_json(
                     engine.get(), 1,
                     "{\"id\":\"track-1\",\"inserts\":[{\"slot\":\"pre\","
                     "\"processor\":\"dynamics.duckingProcessor\",\"params\":{}}]}") == SONARE_OK);
         sonare_engine_set_lane_sidechain(engine.get(), 1, 0, 99);
       }}},
      {"engine_set_master_sidechain.source_id", {[] {
         const EnginePtr engine = prepared_engine();
         REQUIRE(sonare_engine_set_master_strip_json(
                     engine.get(),
                     "{\"id\":\"master\",\"inserts\":[{\"slot\":\"pre\","
                     "\"processor\":\"dynamics.duckingProcessor\",\"params\":{}}]}") == SONARE_OK);
         sonare_engine_set_master_sidechain(engine.get(), 0, SONARE_SIDECHAIN_SOURCE_TRACK, 99);
       }}},
      {"engine_set_track_output_bus.output_bus_id", {[] {
         const EnginePtr engine = prepared_engine();
         const SonareEngineTrackLane lane[] = {{1, nullptr, 0, 0, SONARE_CHANNEL_LAYOUT_STEREO}};
         REQUIRE(sonare_engine_set_track_lanes(engine.get(), lane, 1) == SONARE_OK);
         sonare_engine_set_track_output_bus(engine.get(), 1, 99);
       }}},
      {"lufs_interleaved.num_channels", {[] {
         const float samples[2] = {0.1f, 0.1f};
         SonareLufsResult result{};
         sonare_lufs_interleaved(samples, 1, 0, 48000, &result);
       }}},
      {"lufs_series_interleaved.num_channels", {[] {
         const float samples[2] = {0.1f, 0.1f};
         float* momentary = nullptr;
         size_t momentary_length = 0;
         float* short_term = nullptr;
         size_t short_term_length = 0;
         sonare_lufs_series_interleaved(samples, 1, 0, 48000, &momentary, &momentary_length,
                                        &short_term, &short_term_length);
       }}},
      {"mel_to_audio.mel", {[] {
         float* out = nullptr;
         size_t out_length = 0;
         sonare_mel_to_audio(nullptr, 4, 2, 2, 22050, 2048, 512, 0.0f, 0.0f, 1, nullptr, &out,
                             &out_length);
       }}},
      {"mel_to_stft.mel", {[] {
         SonareInverseResult out{};
         sonare_mel_to_stft(nullptr, 4, 2, 2, 22050, 2048, 0.0f, 0.0f, nullptr, &out);
       }}},
      // The entry passes the three metering flags as constants; only the options struct takes them.
      {"mixer_add_strip.enable_metering", {nullptr}},
      {"mixer_add_strip.measure_lufs", {nullptr}},
      {"mixer_add_strip.measure_true_peak", {nullptr}},
      {"mixer_process_stereo.input_left", {[] {
         const MixerPtr mixer = new_mixer();
         sonare_mixer_process_stereo(mixer.get(), nullptr, nullptr, 1, nullptr, nullptr, 1);
       }}},
      {"mixer_process_stereo.input_right", {[] {
         const MixerPtr mixer = new_mixer();
         const float* plane = nullptr;
         sonare_mixer_process_stereo(mixer.get(), &plane, nullptr, 1, nullptr, nullptr, 1);
       }}},
      {"mixer_strip_at.index", {[] {
         // An empty mixer has no strip at index 0.
         const MixerPtr mixer = new_mixer();
         sonare_mixer_strip_at(mixer.get(), 0);
       }}},
      {"mixing_scene_preset_json.preset_name", {[] {
         char* json = nullptr;
         sonare_mixing_scene_preset_json(nullptr, &json);
       }}},
      // Every offset is accepted (one past the end reads silence), so no refusal names it.
      {"metering_spectrum_frame.frame_offset", {nullptr}},
      // A null buffer is refused as an unreadable document, never as a parameter.
      {"note_targets_from_smf.bytes", {nullptr}},
      {"playback_loudness_meter_push_interleaved.in", {[] {
         SonarePlaybackLoudnessMeter* raw = nullptr;
         REQUIRE(sonare_playback_loudness_meter_create(2, 48000, &raw) == SONARE_OK);
         const MeterPtr meter(raw, sonare_playback_loudness_meter_destroy);
         sonare_playback_loudness_meter_push_interleaved(meter.get(), nullptr, 1);
       }}},
      // A bad input is refused as plain text with no subject.
      {"playback_render_interleaved.in", {nullptr}},
      {"playback_render_interleaved.in_channels", {[] {
         const float sample = 0.0f;
         float* out = nullptr;
         size_t out_frames = 0;
         int out_channels = 0;
         sonare_playback_render_interleaved(&sample, 1, 0, 48000, "{}", nullptr, &out, &out_frames,
                                            &out_channels);
       }}},
      // A realtime entry refuses a bad input without recording a subject.
      {"playback_renderer_process_interleaved.in", {nullptr}},
      // A realtime entry refuses a bad input without recording a subject.
      {"playback_renderer_process_planar.in", {nullptr}},
      // A bad input is refused as plain text with no subject.
      {"playback_renderer_render_offline.in", {nullptr}},
      {"polyphonic_set_note_edit.amplitude_envelope", {[] {
         std::vector<float> tone(22050);
         for (size_t i = 0; i < tone.size(); ++i) {
           tone[i] = 0.5f * std::sin(sonare::constants::kTwoPi * 440.0f * static_cast<float>(i) /
                                     22050.0f);
         }
         SonarePolyphonicAnalysis* raw = nullptr;
         REQUIRE(sonare_polyphonic_analyze(tone.data(), tone.size(), 22050, nullptr, &raw) ==
                 SONARE_OK);
         const std::unique_ptr<SonarePolyphonicAnalysis,
                               decltype(&sonare_polyphonic_analysis_destroy)>
             analysis(raw, sonare_polyphonic_analysis_destroy);
         size_t note_count = 0;
         REQUIRE(sonare_polyphonic_note_count(analysis.get(), &note_count) == SONARE_OK);
         REQUIRE(note_count > 0);
         sonare_polyphonic_set_note_edit(analysis.get(), 0, nullptr, nullptr, 2);
       }}},
      {"project_apply_vocal_edit.sve1",
       {[] {
          const ProjectPtr project = new_project();
          const float rendered[1] = {0.0f};
          SonareProjectVocalEditApplyDesc desc;
          sonare_project_vocal_edit_apply_desc_init(&desc);
          desc.clip_id = 1;
          desc.expected_source_sample_rate = 48000;
          desc.expected_source_sample_count = 1;
          desc.expected_clip_length_ppq = 4.0;
          desc.rendered_mono = rendered;
          desc.rendered_sample_count = 1;
          desc.rendered_sample_rate = 48000;
          desc.sve1 = nullptr;
          SonareProjectVocalEditApplyResult result;
          sonare_project_vocal_edit_apply_result_init(&result);
          sonare_project_apply_vocal_edit(project.get(), &desc, &result);
        },
        "desc"}},
      {"project_import_clip_file.bytes", {[] {
         const ProjectPtr project = new_project();
         uint32_t first_clip_id = 0;
         sonare_project_import_clip_file(project.get(), nullptr, 1, &first_clip_id);
       }}},
      {"project_import_smf.bytes", {[] {
         const ProjectPtr project = new_project();
         uint32_t first_clip_id = 0;
         sonare_project_import_smf(project.get(), nullptr, 1, &first_clip_id);
       }}},
      {"project_move_clip.new_track_id", {[] {
         const ProjectPtr project = new_project();
         uint32_t track = 0;
         uint32_t clip = 0;
         REQUIRE(sonare_project_add_midi_clip(project.get(), 0.0, 2.0, &track, &clip) == SONARE_OK);
         sonare_project_move_clip(project.get(), clip, 0.0, track + 1000);
       }}},
      {"project_set_marker.marker.id", {[] {
         const ProjectPtr project = new_project();
         SonareProjectMarker marker{};
         marker.id = std::numeric_limits<uint32_t>::max();
         uint32_t marker_id = 0;
         sonare_project_set_marker(project.get(), &marker, nullptr, &marker_id);
       }}},
      {"project_set_overlap_policy.overlap_policy", {[] {
         const ProjectPtr project = new_project();
         sonare_project_set_overlap_policy(project.get(), 99);
       }}},
      // A realtime entry refuses a bad input without recording a subject.
      {"realtime_voice_changer_process_mono.input", {nullptr}},
      // A realtime entry refuses a bad input without recording a subject.
      {"realtime_voice_changer_process_mono_with_carrier.input", {nullptr}},
      // A misplaced offset is refused as plain text with no subject.
      {"stream_analyzer_process.sample_offset", {nullptr}},
      // Every base offset is accepted.
      {"stream_analyzer_reset.base_sample_offset", {nullptr}},
      {"strip_remove_send.index", {[] {
         const MixerPtr mixer = new_mixer();
         SonareStrip* strip = sonare_mixer_add_strip(mixer.get(), "strip", nullptr);
         REQUIRE(strip != nullptr);
         sonare_strip_remove_send(strip, 0);
       }}},
      {"strip_set_send_db.index", {[] {
         const MixerPtr mixer = new_mixer();
         SonareStrip* strip = sonare_mixer_add_strip(mixer.get(), "strip", nullptr);
         REQUIRE(strip != nullptr);
         sonare_strip_set_send_db(strip, 0, 0.0f);
       }}},
      // An unknown preset is refused as a document the chain loader cannot read, never as a
      // parameter, and a null one selects the default.
      {"voice_change_realtime.preset", {nullptr}},
      {"waveform_peaks.num_channels", {[] {
         std::vector<float> samples(1024, 0.1f);
         SonareWaveformPeaksResult out{};
         sonare_waveform_peaks(samples.data(), samples.size(), 0, 512, &out);
       }}},
      {"waveform_peak_pyramid.num_channels", {[] {
         std::vector<float> samples(1024, 0.1f);
         const size_t levels[] = {512};
         SonareWaveformPeakPyramidResult out{};
         sonare_waveform_peak_pyramid(samples.data(), samples.size(), 0, levels, 1, &out);
       }}},
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
    const auto driver =
        spelling_row_drivers().find(std::string(row.entry) + "." + std::string(row.subject));
    REQUIRE(driver != spelling_row_drivers().end());
    if (!driver->second.run) continue;
    driver->second.run();
    const std::string root = driver->second.c_root;
    const std::string c_subject =
        root.empty() ? std::string(row.subject) : root + "." + std::string(row.subject);
    const char* refused = sonare_last_refusal_entry();
    REQUIRE(refused != nullptr);
    CHECK(std::string(refused) == entry);
    const std::vector<std::string> subjects = last_refusal_subjects();
    REQUIRE(std::find(subjects.begin(), subjects.end(), c_subject) != subjects.end());
    const auto shown = spelled_last_refusal(root);
    REQUIRE(shown.has_value());
    CHECK_THAT(std::string(shown->what()),
               Catch::Matchers::ContainsSubstring(std::string(row.spelling)));
  }
}

}  // namespace

TEST_CASE("offline freeze names the refused frame count", "[c_api][c_refusal_subjects]") {
  const EnginePtr engine = prepared_engine();
  SonareEngineFreezeOptions options{};
  sonare_engine_freeze_options_init(&options);
  options.total_frames = -1;
  SonareEngineFreezeResult out{};
  REQUIRE(sonare_engine_freeze_offline(engine.get(), &options, &out) ==
          SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_refusal_subjects() == std::vector<std::string>{"options.total_frames"});
}

TEST_CASE("every renamed refusal subject reaches the caller under the facade's spelling",
          "[c_api][field_spelling]") {
  SECTION("node") {
    check_spelling_rows(sonare_field_spelling::node::kOverrides, [](std::string_view root) {
      return sonare_field_spelling::node::SpelledLastRefusal(root);
    });
  }
  SECTION("wasm") {
    check_spelling_rows(sonare_field_spelling::wasm::kOverrides, [](std::string_view root) {
      return sonare_field_spelling::wasm::SpelledLastRefusal(root);
    });
  }
}

namespace {

// One call per strip setter: the caller's bare object carrying an unknown key and, for the track
// strip, a value of the wrong kind.
void set_strip_object(const std::string& entry, const std::string& entry_json) {
  const EnginePtr engine = prepared_engine();
  if (entry == "engine_set_bus_strip_json") {
    const SonareEngineBus bus[] = {{1, 0.0f, SONARE_CHANNEL_LAYOUT_STEREO, 0, nullptr, 0}};
    REQUIRE(sonare_engine_set_track_buses(engine.get(), bus, 1) == SONARE_OK);
    CHECK(sonare_engine_set_bus_strip_json(engine.get(), 1, entry_json.c_str()) ==
          SONARE_ERROR_INVALID_PARAMETER);
    return;
  }
  if (entry == "engine_set_master_strip_json") {
    CHECK(sonare_engine_set_master_strip_json(engine.get(), entry_json.c_str()) ==
          SONARE_ERROR_INVALID_PARAMETER);
    return;
  }
  REQUIRE(entry == "engine_set_track_strip_json");
  CHECK(sonare_engine_set_track_strip_json(engine.get(), 1, entry_json.c_str()) ==
        SONARE_ERROR_INVALID_PARAMETER);
}

}  // namespace

TEST_CASE("a strip setter refusal names the caller's settings object", "[c_api][field_spelling]") {
  for (const char* entry : {"engine_set_bus_strip_json", "engine_set_master_strip_json",
                            "engine_set_track_strip_json"}) {
    CAPTURE(entry);
    set_strip_object(entry, "{\"id\":\"1\",\"faderDB\":0}");
    REQUIRE(sonare_last_refusal_entry() != nullptr);
    CHECK(std::string(sonare_last_refusal_entry()) == entry);
    REQUIRE(last_refusal_subjects() == std::vector<std::string>{"settings.faderDB"});
    CHECK(sonare_last_error_code() == SONARE_ERROR_INVALID_PARAMETER);
    CHECK(last_message().rfind("unknown key settings.faderDB (expected ", 0) == 0);

    set_strip_object(entry, "{\"id\":\"1\",\"pan\":\"left\"}");
    CHECK(last_message() == "settings.pan must be a number");
  }
}

namespace {

// One call per quotient row: a frame count the facade derives from an interleaved array's length.
// The realtime process entries record no refusal by contract, so they reach no spelling at all.
template <typename Quotients, typename Spell>
void check_quotient_rows(const Quotients& rows, const Spell& spelled_last_refusal) {
  REQUIRE(!rows.empty());
  for (const auto& row : rows) {
    const std::string entry(row.entry);
    CAPTURE(entry, row.subject, row.spelling, row.divisor);
    SonarePlaybackRenderer* raw = nullptr;
    REQUIRE(sonare_playback_renderer_create_json("{}", nullptr, 48000, 512, &raw) == SONARE_OK);
    const std::unique_ptr<SonarePlaybackRenderer, decltype(&sonare_playback_renderer_destroy)>
        renderer(raw, sonare_playback_renderer_destroy);
    const float sample = 0.0f;
    float out_sample = 0.0f;
    if (entry == "playback_renderer_process_interleaved") {
      CHECK(sonare_playback_renderer_process_interleaved(renderer.get(), &sample, 2, &out_sample, 2,
                                                         -1) == SONARE_ERROR_INVALID_PARAMETER);
      CHECK(sonare_last_refusal_subject_count() == 0);
      continue;
    }
    REQUIRE(entry == "playback_renderer_render_offline");
    float* out = nullptr;
    size_t out_frames = 0;
    int out_channels = 0;
    CHECK(sonare_playback_renderer_render_offline(renderer.get(), &sample, 0, 2, &out, &out_frames,
                                                  &out_channels) == SONARE_ERROR_INVALID_PARAMETER);
    REQUIRE(last_refusal_subjects() == std::vector<std::string>{std::string(row.subject)});
    const auto shown = spelled_last_refusal({});
    REQUIRE(shown.has_value());
    CHECK(std::string(shown->what()) == std::string(row.spelling) + ".length / " +
                                            std::string(row.divisor) +
                                            " must be an integer of at least 1");
  }
}

}  // namespace

TEST_CASE("an empty interleaved buffer names the array over its channel count",
          "[c_api][field_spelling]") {
  SECTION("node") {
    check_quotient_rows(sonare_field_spelling::node::kQuotients, [](std::string_view root) {
      return sonare_field_spelling::node::SpelledLastRefusal(root);
    });
  }
  SECTION("wasm") {
    check_quotient_rows(sonare_field_spelling::wasm::kQuotients, [](std::string_view root) {
      return sonare_field_spelling::wasm::SpelledLastRefusal(root);
    });
  }
  SECTION("C callers read the frame count") {
    float* out = nullptr;
    size_t out_frames = 0;
    int out_channels = 0;
    const float sample = 0.0f;
    CHECK(sonare_playback_render_interleaved(&sample, 0, 2, 48000, "{}", nullptr, &out, &out_frames,
                                             &out_channels) == SONARE_ERROR_INVALID_PARAMETER);
    CHECK(std::string(sonare_last_error_message()) == "frames must be an integer of at least 1");
  }
}

TEST_CASE("an empty SMF or MIDI Clip File buffer is refused as empty, not as left out",
          "[c_api][project]") {
  using ImportEntry = SonareError (*)(SonareProject*, const uint8_t*, size_t, uint32_t*);
  for (ImportEntry entry : {&sonare_project_import_smf, &sonare_project_import_clip_file}) {
    const ProjectPtr project = new_project();
    const uint8_t byte = 0;
    uint32_t first_clip_id = 0;
    CHECK(entry(project.get(), &byte, 0, &first_clip_id) == SONARE_ERROR_INVALID_PARAMETER);
    CHECK(last_message() == "bytes must not be empty");
  }
}

namespace {

constexpr int kSr = 22050;
constexpr const char* kElementLimit = "500000000";

std::vector<float> sine(size_t count) {
  std::vector<float> values(count);
  for (size_t i = 0; i < count; ++i) values[i] = std::sin(0.05f * static_cast<float>(i));
  return values;
}

/// @brief The refusal text "<subject> must keep <what> within <limit> elements".
std::string element_limit(const std::string& subject, const std::string& what,
                          const std::string& limit = kElementLimit) {
  return subject + " must keep " + what + " within " + limit + " elements";
}

/// @brief Checks that the last call was refused as an invalid parameter with exactly @p text.
void check_refused(SonareError rc, const std::string& text) {
  CHECK(rc == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == text);
}

constexpr int kKernelNBins = 33554432 / 2 + 1;
constexpr size_t kOutputSamples = 1000001;

}  // namespace

TEST_CASE("sonare_cqt refuses an n_bins and a hop_length past the work size limit",
          "[c_api][work_size_limit]") {
  const std::vector<float> small(4096, 0.0f);
  SonareCqtResult out{};
  check_refused(
      sonare_cqt(small.data(), small.size(), kSr, 512, 32.7f, kKernelNBins, 12, 1.0f, &out),
      element_limit("n_bins", "the filter kernel", "33554432"));
  const std::vector<float> long_input(kOutputSamples, 0.0f);
  check_refused(
      sonare_cqt(long_input.data(), long_input.size(), kSr, 1, kSr / 4.0f, 500, 500, 1.0f, &out),
      element_limit("hop_length", "the constant-Q output"));
}

TEST_CASE("sonare_pseudo_cqt refuses an n_bins past the work size limit",
          "[c_api][work_size_limit]") {
  const std::vector<float> small(4096, 0.0f);
  SonareCqtResult out{};
  check_refused(
      sonare_pseudo_cqt(small.data(), small.size(), kSr, 512, 32.7f, kKernelNBins, 12, 1.0f, &out),
      element_limit("n_bins", "the filter kernel", "33554432"));
}

TEST_CASE("sonare_hybrid_cqt refuses an n_bins and a hop_length past the work size limit",
          "[c_api][work_size_limit]") {
  const std::vector<float> small(4096, 0.0f);
  SonareCqtResult out{};
  check_refused(
      sonare_hybrid_cqt(small.data(), small.size(), kSr, 512, 32.7f, kKernelNBins, 12, 1.0f, &out),
      element_limit("n_bins", "the filter kernel", "33554432"));
  const std::vector<float> long_input(kOutputSamples, 0.0f);
  check_refused(sonare_hybrid_cqt(long_input.data(), long_input.size(), kSr, 1, kSr / 4.0f, 500,
                                  500, 1.0f, &out),
                element_limit("hop_length", "the constant-Q output"));
}

TEST_CASE("sonare_vqt refuses an n_bins and a hop_length past the work size limit",
          "[c_api][work_size_limit]") {
  const std::vector<float> small(4096, 0.0f);
  SonareCqtResult out{};
  const float automatic_gamma = std::numeric_limits<float>::quiet_NaN();
  check_refused(sonare_vqt(small.data(), small.size(), kSr, 512, 32.7f, kKernelNBins, 12, 1.0f,
                           automatic_gamma, &out),
                element_limit("n_bins", "the filter kernel", "33554432"));
  const std::vector<float> long_input(kOutputSamples, 0.0f);
  check_refused(sonare_vqt(long_input.data(), long_input.size(), kSr, 1, kSr / 4.0f, 500, 500, 1.0f,
                           automatic_gamma, &out),
                element_limit("hop_length", "the constant-Q output"));
}

TEST_CASE("sonare_cqt_to_audio refuses a hop_length and an n_frames past the work size limit",
          "[c_api][work_size_limit]") {
  const std::vector<float> magnitude(7631, 1.0f);
  float* out = nullptr;
  size_t out_length = 0;
  check_refused(sonare_cqt_to_audio(magnitude.data(), magnitude.size(), 1, 7631, kSr, 65536, 32.7f,
                                    12, 1, &out, &out_length),
                element_limit("hop_length", "the reconstructed signal"));
  check_refused(sonare_cqt_to_audio(magnitude.data(), magnitude.size(), 1, 7631, kSr, 1, 0.3f, 1, 1,
                                    &out, &out_length),
                element_limit("n_frames", "the STFT magnitude"));
}

TEST_CASE("sonare_vqt_to_audio refuses a hop_length and an n_frames past the work size limit",
          "[c_api][work_size_limit]") {
  const std::vector<float> magnitude(7631, 1.0f);
  float* out = nullptr;
  size_t out_length = 0;
  check_refused(
      sonare_vqt_to_audio(magnitude.data(), magnitude.size(), 1, 7631, kSr, 65536, 32.7f, 12,
                          std::numeric_limits<float>::quiet_NaN(), 1, &out, &out_length),
      element_limit("hop_length", "the reconstructed signal"));
  check_refused(sonare_vqt_to_audio(magnitude.data(), magnitude.size(), 1, 7631, kSr, 1, 0.3f, 1,
                                    1e-3f, 1, &out, &out_length),
                element_limit("n_frames", "the STFT magnitude"));
}

TEST_CASE("sonare_pitch_pyin refuses a hop_length past the work size limit",
          "[c_api][work_size_limit]") {
  const std::vector<float> samples(220000, 0.0f);
  SonarePitchResult out{};
  check_refused(sonare_pitch_pyin(samples.data(), samples.size(), kSr, 2048, 1, kSr / 2040.0f,
                                  kSr / 2.1f, 0.1f, 0, &out),
                element_limit("hop_length", "the observation matrix"));
}

TEST_CASE("sonare_analyze_melody refuses a hop_length past the work size limit",
          "[c_api][work_size_limit]") {
  const std::vector<float> samples(220000, 0.0f);
  SonareAnalyzeMelodyOptions options;
  sonare_analyze_melody_options_init(&options);
  options.use_pyin = 1;
  SonareMelodyResult out{};
  check_refused(sonare_analyze_melody(samples.data(), samples.size(), kSr, kSr / 2040.0f,
                                      kSr / 2.1f, 2048, 1, 0.1f, &options, &out),
                element_limit("hop_length", "the observation matrix"));
}

TEST_CASE("sonare_polyphonic_analyze refuses a cents_per_bin past the work size limit",
          "[c_api][work_size_limit]") {
  const std::vector<float> samples(20000, 0.0f);
  SonarePolyphonicConfig config;
  sonare_polyphonic_config_init(&config);
  config.n_fft = 64;
  config.hop_length = 1;
  config.cents_per_bin = 1.0f;
  config.cent_ref_hz = 1.0f;
  config.cent_max_hz = 1e8f;
  SonarePolyphonicAnalysis* out = nullptr;
  check_refused(sonare_polyphonic_analyze(samples.data(), samples.size(), 44100, &config, &out),
                element_limit("cents_per_bin", "the cent spectrum"));
  CHECK(out == nullptr);
}

TEST_CASE("sonare_decompose refuses an n_components past the work size limit",
          "[c_api][work_size_limit]") {
  const std::vector<float> single(1, 1.0f);
  const std::vector<float> row(5001, 1.0f);
  float* w = nullptr;
  float* h = nullptr;
  size_t w_length = 0;
  size_t h_length = 0;
  check_refused(sonare_decompose(single.data(), 1, 1, 500000001, 0, 2.0f, nullptr, &w, &w_length,
                                 &h, &h_length),
                element_limit("n_components", "the component weights"));
  check_refused(
      sonare_decompose(row.data(), 1, 5001, 100000, 0, 2.0f, nullptr, &w, &w_length, &h, &h_length),
      element_limit("n_components", "the component activations"));
}

TEST_CASE("sonare_decompose_stems refuses an n_components past the work size limit",
          "[c_api][work_size_limit]") {
  const std::vector<float> samples = sine(8192);
  SonareDecomposeStemsConfig config;
  sonare_decompose_stems_config_init(&config);
  config.n_fft = 4;
  config.hop_length = 1;
  float* out = nullptr;
  size_t out_frames = 0;
  int out_n_components = 0;
  config.n_components = 500000001;
  check_refused(
      sonare_decompose_stems(samples.data(), samples.size(), kSr, &config, &out, &out_frames,
                             &out_n_components, nullptr, nullptr, nullptr, nullptr),
      element_limit("n_components", "the component weights"));
  config.n_components = 100000;
  check_refused(
      sonare_decompose_stems(samples.data(), samples.size(), kSr, &config, &out, &out_frames,
                             &out_n_components, nullptr, nullptr, nullptr, nullptr),
      element_limit("n_components", "the component activations"));
}

TEST_CASE("sonare_decompose_stems_linked refuses an n_components past the work size limit",
          "[c_api][work_size_limit]") {
  const std::vector<float> samples = sine(8192);
  const float* channels[2] = {samples.data(), samples.data()};
  SonareDecomposeStemsConfig config;
  sonare_decompose_stems_config_init(&config);
  config.n_fft = 4;
  config.hop_length = 1;
  float* out = nullptr;
  size_t out_frames = 0;
  int out_n_components = 0;
  config.n_components = 500000001;
  check_refused(
      sonare_decompose_stems_linked(channels, 2, samples.size(), kSr, &config, &out, &out_frames,
                                    &out_n_components, nullptr, nullptr, nullptr, nullptr),
      element_limit("n_components", "the component weights"));
  config.n_components = 100000;
  check_refused(
      sonare_decompose_stems_linked(channels, 2, samples.size(), kSr, &config, &out, &out_frames,
                                    &out_n_components, nullptr, nullptr, nullptr, nullptr),
      element_limit("n_components", "the component activations"));
}

TEST_CASE("sonare_phase_vocoder refuses a speed_ratio past the work size limit",
          "[c_api][work_size_limit]") {
  const std::vector<float> samples = sine(8192);
  float* out = nullptr;
  size_t out_length = 0;
  check_refused(sonare_phase_vocoder(samples.data(), samples.size(), kSr, 1e-6f, 2048, 512, &out,
                                     &out_length),
                element_limit("speed_ratio", "the stretched spectrogram"));
}

TEST_CASE("sonare_note_stretch refuses a stretch_ratio past the work size limit",
          "[c_api][work_size_limit]") {
  const std::vector<float> samples = sine(8192);
  float* out = nullptr;
  size_t out_length = 0;
  check_refused(
      sonare_note_stretch(samples.data(), samples.size(), kSr, 1000, 6000, 1e6f, &out, &out_length),
      element_limit("stretch_ratio", "the stretched note", "2147483647"));
}

TEST_CASE("the tempogram entries refuse a win_length and an n_bins past the work size limit",
          "[c_api][work_size_limit]") {
  constexpr size_t kWindow = 12288;
  const std::vector<float> envelope(81400, 1.0f);
  const std::vector<float> short_envelope(40700, 1.0f);
  const std::vector<float> small(1000, 1.0f);
  const std::string tempogram = element_limit("win_length", "the tempogram");
  float* out = nullptr;
  size_t out_length = 0;
  int n_frames = 0;

  SECTION("sonare_fourier_tempogram") {
    check_refused(sonare_fourier_tempogram(envelope.data(), envelope.size(), kSr, 512, kWindow, 1,
                                           0, &out, &out_length, &n_frames),
                  tempogram);
  }
  SECTION("sonare_cyclic_tempogram") {
    check_refused(sonare_cyclic_tempogram(envelope.data(), envelope.size(), kSr, 512, kWindow,
                                          60.0f, 60, &out, &out_length, &n_frames),
                  tempogram);
    check_refused(sonare_cyclic_tempogram(small.data(), small.size(), kSr, 512, 8, 60.0f, 500001,
                                          &out, &out_length, &n_frames),
                  element_limit("n_bins", "the cyclic tempogram"));
  }
  SECTION("sonare_plp") {
    check_refused(sonare_plp(envelope.data(), envelope.size(), kSr, 512, 60.0f, 200.0f, kWindow,
                             &out, &out_length),
                  tempogram);
  }
  SECTION("sonare_tempogram") {
    check_refused(
        sonare_tempogram(short_envelope.data(), short_envelope.size(), kSr, 512, kWindow, 1, 1,
                         SONARE_TEMPOGRAM_AUTOCORRELATION, &out, &out_length, &n_frames),
        tempogram);
  }
}

TEST_CASE("sonare_segment_cross_similarity refuses an x_cols past the work size limit",
          "[c_api][work_size_limit]") {
  const std::vector<float> features(46341, 1.0f);
  SonareSegmentMatrix out{};
  check_refused(sonare_segment_cross_similarity(features.data(), 1, 46341, features.data(), 1,
                                                46341, 0, "cosine", "connectivity", &out),
                element_limit("x_cols", "the similarity matrix", "2147483647"));
}

TEST_CASE("sonare_frame_signal refuses a hop_length past the work size limit",
          "[c_api][work_size_limit]") {
  const std::vector<float> samples(70000, 0.0f);
  float* out = nullptr;
  size_t out_length = 0;
  int n_frames = 0;
  check_refused(
      sonare_frame_signal(samples.data(), samples.size(), 35000, 1, &out, &out_length, &n_frames),
      element_limit("hop_length", "the framed output"));
}

TEST_CASE("sonare_remix refuses intervals past the work size limit", "[c_api][work_size_limit]") {
  const std::vector<float> samples(1000000, 0.0f);
  std::vector<int> intervals;
  for (int i = 0; i < 501; ++i) {
    intervals.push_back(0);
    intervals.push_back(1000000);
  }
  float* out = nullptr;
  size_t out_length = 0;
  check_refused(sonare_remix(samples.data(), samples.size(), kSr, intervals.data(), 501, 0, &out,
                             &out_length),
                element_limit("intervals", "the remixed signal"));
}

TEST_CASE("sonare_pad_center refuses a target_size past the work size limit",
          "[c_api][work_size_limit]") {
  const float value = 1.0f;
  float* out = nullptr;
  size_t out_length = 0;
  check_refused(sonare_pad_center(&value, 1, 500000001, 0.0f, &out, &out_length),
                element_limit("target_size", "the padded output"));
}

TEST_CASE("sonare_fix_length refuses a target_size past the work size limit",
          "[c_api][work_size_limit]") {
  const float value = 1.0f;
  float* out = nullptr;
  size_t out_length = 0;
  check_refused(sonare_fix_length(&value, 1, 500000001, 0.0f, &out, &out_length),
                element_limit("target_size", "the padded output"));
}

TEST_CASE("sonare_mastering_repair_analyze refuses a num_channels past the work size limit",
          "[c_api][work_size_limit]") {
  constexpr size_t kLength = 65536;
  constexpr size_t kChannels = 7630;
  const std::vector<float> plane(kLength, 0.0f);
  const std::vector<const float*> channels(kChannels, plane.data());
  char* json = nullptr;
  check_refused(
      sonare_mastering_repair_analyze(channels.data(), kChannels, kLength, 48000, "{}", &json),
      element_limit("num_channels", "the interleaved audio"));
  CHECK(json == nullptr);
}

TEST_CASE("the playback render entries refuse an in_channels past the work size limit",
          "[c_api][work_size_limit]") {
  const float sample = 0.0f;
  float* out = nullptr;
  size_t out_frames = 0;
  int out_channels = 0;
  const std::string expected = element_limit("in_channels", "the input samples");

  SECTION("sonare_playback_render_interleaved") {
    check_refused(sonare_playback_render_interleaved(&sample, 1, 500000001, 48000, "{}", nullptr,
                                                     &out, &out_frames, &out_channels),
                  expected);
  }
  SECTION("sonare_playback_renderer_render_offline") {
    SonarePlaybackRenderer* raw = nullptr;
    REQUIRE(sonare_playback_renderer_create_json("{}", nullptr, 48000, 512, &raw) == SONARE_OK);
    const std::unique_ptr<SonarePlaybackRenderer, decltype(&sonare_playback_renderer_destroy)>
        renderer(raw, sonare_playback_renderer_destroy);
    check_refused(sonare_playback_renderer_render_offline(renderer.get(), &sample, 1, 500000001,
                                                          &out, &out_frames, &out_channels),
                  expected);
  }
}
