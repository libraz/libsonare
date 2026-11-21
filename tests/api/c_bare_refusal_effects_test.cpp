/// @file c_bare_refusal_effects_test.cpp
/// @brief The effects and polyphony C entries record a stated cause for every argument refusal.

#include <sonare/sonare_c.h>

#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <string>
#include <vector>

namespace {

std::string last_message() {
  const char* message = sonare_last_error_message();
  return message != nullptr ? message : "";
}

std::vector<float> tone(size_t length) {
  std::vector<float> samples(length);
  for (size_t i = 0; i < length; ++i) {
    samples[i] = 0.5f * std::sin(0.05f * static_cast<float>(i));
  }
  return samples;
}

constexpr int kRate = 22050;

}  // namespace

TEST_CASE("hpss_ex and trim_ex refuse a non-positive size by name", "[c_api][effects]") {
  const auto samples = tone(4096);
  SonareHpssResult result{};
  CHECK(sonare_hpss_ex(samples.data(), samples.size(), kRate, 31, 31, 0, 0, 1, 0, &result,
                       nullptr) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == "n_fft must be an even integer in [4, 524288]");

  float* out = nullptr;
  size_t out_length = 0;
  CHECK(sonare_trim_ex(samples.data(), samples.size(), kRate, -60.0f, 0, 512, &out, &out_length) ==
        SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == "frame_length must be an integer in [1, 2147483647]");
  CHECK(sonare_trim_ex(samples.data(), samples.size(), kRate, -60.0f, 2048, 0, &out, &out_length) ==
        SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == "hop_length must be an integer in [1, 2147483647]");
}

TEST_CASE("nn_filter refuses a non-positive shape by name", "[c_api][effects]") {
  const std::vector<float> s(16, 1.0f);
  float* out = nullptr;
  size_t out_length = 0;
  CHECK(sonare_nn_filter(s.data(), 0, 4, nullptr, -1, -1, &out, &out_length) ==
        SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == "n_features must be an integer in [1, 2147483647]");
  CHECK(sonare_nn_filter(s.data(), 4, 0, nullptr, -1, -1, &out, &out_length) ==
        SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == "n_frames must be an integer in [1, 2147483647]");
}

TEST_CASE("decompose_stems refuses an unpaired factor output by name", "[c_api][effects]") {
  const auto samples = tone(8192);
  SonareDecomposeStemsConfig config;
  sonare_decompose_stems_config_init(&config);
  float* out = nullptr;
  size_t count = 0;
  size_t component_length = 0;
  float* w = nullptr;
  float* h = nullptr;
  size_t h_length = 0;
  CHECK(sonare_decompose_stems(samples.data(), samples.size(), kRate, &config, &out, &count,
                               &component_length, &w, nullptr, nullptr,
                               nullptr) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == "out_w and out_w_length must be given together");
  CHECK(sonare_decompose_stems(samples.data(), samples.size(), kRate, &config, &out, &count,
                               &component_length, nullptr, nullptr, &h,
                               nullptr) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == "out_h and out_h_length must be given together");
  CHECK(sonare_decompose_stems(samples.data(), samples.size(), kRate, &config, &out, &count,
                               &component_length, nullptr, nullptr, nullptr,
                               &h_length) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == "out_h and out_h_length must be given together");
}

TEST_CASE("decompose_stems_linked refuses channels and num_channels by name", "[c_api][effects]") {
  const auto samples = tone(8192);
  const float* channels[1] = {samples.data()};
  float* out = nullptr;
  size_t count = 0;
  size_t num_channels = 0;
  size_t component_length = 0;
  CHECK(sonare_decompose_stems_linked(nullptr, 1, samples.size(), kRate, nullptr, &out, &count,
                                      &num_channels, &component_length, nullptr, nullptr, nullptr,
                                      nullptr) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == "channels must not be NULL");
  CHECK(sonare_decompose_stems_linked(channels, 0, samples.size(), kRate, nullptr, &out, &count,
                                      &num_channels, &component_length, nullptr, nullptr, nullptr,
                                      nullptr) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == "num_channels must be an integer in [1, 64]");
  CHECK(sonare_decompose_stems_linked(channels, 65, samples.size(), kRate, nullptr, &out, &count,
                                      &num_channels, &component_length, nullptr, nullptr, nullptr,
                                      nullptr) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == "num_channels must be an integer in [1, 64]");
}

TEST_CASE("spectral_edit refuses a window or mode out of range by name", "[c_api][effects]") {
  const auto samples = tone(4096);
  float* out = nullptr;
  size_t out_length = 0;

  SonareSpectralEditConfig config;
  sonare_spectral_edit_config_init(&config);
  config.window = 99;
  CHECK(sonare_spectral_edit(samples.data(), samples.size(), kRate, &config, nullptr, 0, &out,
                             &out_length) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == "config.window must be an integer in [0, 3]");

  SonareSpectralRegionOp op;
  sonare_spectral_region_op_init(&op);
  op.mode = 9;
  CHECK(sonare_spectral_edit(samples.data(), samples.size(), kRate, nullptr, &op, 1, &out,
                             &out_length) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == "ops[0].mode must be an integer in [0, 3]");
}

#if defined(SONARE_WITH_PITCH_EDITOR)

TEST_CASE("polyphonic accessors refuse a note or out past what they hold by name",
          "[c_api][polyphony]") {
  const auto samples = tone(22050);
  SonarePolyphonicAnalysis* analysis = nullptr;
  REQUIRE(sonare_polyphonic_analyze(samples.data(), samples.size(), kRate, nullptr, &analysis) ==
          SONARE_OK);
  size_t note_count = 0;
  REQUIRE(sonare_polyphonic_note_count(analysis, &note_count) == SONARE_OK);

  float curve[4] = {};
  size_t written = 0;
  CHECK(sonare_polyphonic_note_f0(analysis, note_count, curve, 4, &written) ==
        SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == "note must be less than the number of notes");
  CHECK(sonare_polyphonic_set_note_edit(analysis, note_count, nullptr, nullptr, 0) ==
        SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == "note must be less than the number of notes");

  CHECK(sonare_polyphonic_set_note_edit(nullptr, 0, nullptr, nullptr, 0) ==
        SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == "analysis must not be NULL");

  if (note_count > 0) {
    CHECK(sonare_polyphonic_set_note_edit(analysis, 0, nullptr, nullptr, 2) ==
          SONARE_ERROR_INVALID_PARAMETER);
    CHECK(last_message() == "envelope must not be NULL when envelope_count is not 0");
    size_t frames = 0;
    if (sonare_polyphonic_note_f0(analysis, 0, nullptr, 0, &frames) == SONARE_OK && frames > 0) {
      CHECK(sonare_polyphonic_note_f0(analysis, 0, nullptr, 4, &written) ==
            SONARE_ERROR_INVALID_PARAMETER);
      CHECK(last_message() == "out must not be NULL when capacity is not 0");
    }
  }

  int32_t counts[2] = {};
  CHECK(sonare_polyphonic_polyphony(nullptr, counts, 2, &written) ==
        SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == "analysis must not be NULL");

  sonare_polyphonic_analysis_destroy(analysis);
}

#endif
