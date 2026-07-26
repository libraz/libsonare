/// @file pitch_test.cpp
/// @brief Reference compatibility tests for YIN pitch detection.
/// @details Reference values from: tests/librosa/reference/yin.json

#include "feature/pitch.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

#include "util/json_reader.h"
#include "util/math_utils.h"

using namespace sonare;
using namespace sonare::constants;
using namespace sonare::test;
using Catch::Matchers::WithinRel;

namespace {

double cents_error(double detected, double reference) {
  if (detected <= 0.0 || reference <= 0.0 || !std::isfinite(detected) ||
      !std::isfinite(reference)) {
    return std::numeric_limits<double>::infinity();
  }
  return 1200.0 * std::abs(std::log2(detected / reference));
}

}  // namespace

TEST_CASE("YIN pitch reference compatibility", "[pitch][reference]") {
  auto json = JsonReader::parse_file("tests/librosa/reference/yin.json");
  const auto& data = json["data"].as_array();

  for (const auto& entry : data) {
    std::string signal_name = entry["signal"].as_string();
    int sr = entry["sr"].as_int();
    float fmin = entry["fmin"].as_float();
    float fmax = entry["fmax"].as_float();
    int frame_length = entry["frame_length"].as_int();
    int hop_length = entry["hop_length"].as_int();
    bool center = entry.contains("center") ? entry["center"].as_bool() : true;
    const auto& ref_f0 = entry["f0"].as_array();

    if (signal_name == "440Hz_tone") {
      SECTION("440Hz stable pitch") {
        // Create 440Hz tone, 1.0s
        size_t n_samples = static_cast<size_t>(sr);
        std::vector<float> samples(n_samples);
        for (size_t i = 0; i < n_samples; ++i) {
          float t = static_cast<float>(i) / static_cast<float>(sr);
          samples[i] = std::sin(kTwoPi * 440.0f * t);
        }
        Audio audio = Audio::from_buffer(samples.data(), n_samples, sr);

        PitchConfig config;
        config.frame_length = frame_length;
        config.hop_length = hop_length;
        config.fmin = fmin;
        config.fmax = fmax;
        config.center = center;

        PitchResult result = yin_track(audio, config);

        // Implementations may produce different frame counts due to
        // different framing/padding strategies. Compare overlapping frames.
        int our_frames = result.n_frames();
        int ref_frames = static_cast<int>(ref_f0.size());
        int compare_frames = std::min(our_frames, ref_frames);
        CAPTURE(our_frames, ref_frames);
        REQUIRE(compare_frames > 4);

        // Compare detected f0 per frame, allowing mismatch at boundaries
        int boundary = 2;
        for (int i = boundary; i < compare_frames - boundary; ++i) {
          float ref = ref_f0[i].as_float();
          float det = result.f0[i];

          // Both should be voiced (> 0)
          if (ref > 0.0f && det > 0.0f) {
            REQUIRE_THAT(static_cast<double>(det), WithinRel(static_cast<double>(ref), 1e-2));
          }
        }
      }
    } else if (signal_name == "chirp_200_800Hz") {
      SECTION("chirp pitch tracking") {
        // Create linear chirp from 200Hz to 800Hz over 1.0s
        float chirp_fmin = 200.0f;
        float chirp_fmax = 800.0f;
        float duration = 1.0f;
        size_t n_samples = static_cast<size_t>(sr);
        std::vector<float> samples(n_samples);
        for (size_t i = 0; i < n_samples; ++i) {
          float t = static_cast<float>(i) / static_cast<float>(sr);
          float phase =
              kTwoPi * (chirp_fmin * t + 0.5f * (chirp_fmax - chirp_fmin) * t * t / duration);
          samples[i] = std::sin(phase);
        }
        Audio audio = Audio::from_buffer(samples.data(), n_samples, sr);

        PitchConfig config;
        config.frame_length = frame_length;
        config.hop_length = hop_length;
        config.fmin = fmin;
        config.fmax = fmax;
        config.center = center;

        PitchResult result = yin_track(audio, config);

        // Implementations may produce different frame counts due to
        // different framing/padding strategies. Compare overlapping frames.
        int our_frames = result.n_frames();
        int ref_frames = static_cast<int>(ref_f0.size());
        int n_frames = std::min(our_frames, ref_frames);
        CAPTURE(our_frames, ref_frames);
        REQUIRE(n_frames > 4);

        // Both f0 vectors should show increasing frequency.
        // Compute Pearson correlation between our f0 and reference f0.
        // Use only interior frames to avoid boundary effects.
        int boundary = 2;
        int count = n_frames - 2 * boundary;
        REQUIRE(count > 0);

        std::vector<float> our_f0(count);
        std::vector<float> ref_vec(count);
        for (int i = 0; i < count; ++i) {
          our_f0[i] = result.f0[i + boundary];
          ref_vec[i] = ref_f0[i + boundary].as_float();
        }

        float corr = pearson_correlation(our_f0.data(), ref_vec.data(), static_cast<size_t>(count));
        REQUIRE(corr > 0.9f);
      }
    }
  }
}

TEST_CASE("pYIN pitch reference compatibility", "[pitch][pyin][reference]") {
  auto json = JsonReader::parse_file("tests/librosa/reference/pyin.json");
  const auto& data = json["data"].as_array();

  for (const auto& entry : data) {
    std::string signal_name = entry["signal"].as_string();
    int sr = entry["sr"].as_int();
    float fmin = entry["fmin"].as_float();
    float fmax = entry["fmax"].as_float();
    int frame_length = entry["frame_length"].as_int();
    int hop_length = entry["hop_length"].as_int();
    bool center = entry.contains("center") ? entry["center"].as_bool() : true;
    const auto& ref_f0 = entry["f0"].as_array();
    const auto& ref_voiced = entry["voiced_flag"].as_array();
    const auto& ref_voiced_prob = entry["voiced_prob"].as_array();
    const double cents_tolerance = entry["acceptance"]["f0_cents_tolerance"].as_number();
    const double max_mismatch_ratio =
        entry["acceptance"]["max_voiced_flag_mismatch_ratio"].as_number();
    const double max_probability_error =
        entry["acceptance"]["max_voiced_prob_mean_abs_error"].as_number();
    const double min_probability_correlation =
        entry["acceptance"]["min_voiced_prob_correlation"].as_number();

    std::vector<float> samples(static_cast<size_t>(sr));
    if (signal_name == "440Hz_tone") {
      for (size_t i = 0; i < samples.size(); ++i) {
        float t = static_cast<float>(i) / static_cast<float>(sr);
        samples[i] = std::sin(kTwoPi * 440.0f * t);
      }
    } else if (signal_name == "chirp_200_800Hz" || signal_name == "noisy_chirp_200_800Hz") {
      float chirp_fmin = 200.0f;
      float chirp_fmax = 800.0f;
      float duration = 1.0f;
      uint32_t noise_state = 0x12345678U;
      for (size_t i = 0; i < samples.size(); ++i) {
        float t = static_cast<float>(i) / static_cast<float>(sr);
        float phase =
            kTwoPi * (chirp_fmin * t + 0.5f * (chirp_fmax - chirp_fmin) * t * t / duration);
        samples[i] = std::sin(phase);
        if (signal_name == "noisy_chirp_200_800Hz") {
          noise_state = 1664525U * noise_state + 1013904223U;
          const float noise =
              (static_cast<float>(noise_state >> 8U) / static_cast<float>(1U << 24U) * 2.0f -
               1.0f) *
              0.05f;
          samples[i] = 0.8f * samples[i] + noise;
        }
      }
    } else {
      FAIL("unknown pYIN reference signal: " << signal_name);
    }

    Audio audio = Audio::from_buffer(samples.data(), samples.size(), sr);
    PitchConfig config;
    config.frame_length = frame_length;
    config.hop_length = hop_length;
    config.fmin = fmin;
    config.fmax = fmax;
    config.fill_na = true;
    config.center = center;

    PitchResult result = pyin(audio, config);
    int compare_frames = std::min(result.n_frames(), static_cast<int>(ref_f0.size()));
    CAPTURE(signal_name, result.n_frames(), ref_f0.size());
    REQUIRE(compare_frames > 0);

    int voiced_compared = 0;
    int f0_within_tolerance = 0;
    int voiced_mismatches = 0;
    double probability_abs_error = 0.0;
    double probability_sum = 0.0;
    double reference_probability_sum = 0.0;
    for (int i = 0; i < compare_frames; ++i) {
      const bool expected_voiced = ref_voiced[static_cast<size_t>(i)].as_bool();
      if (result.voiced_flag[static_cast<size_t>(i)] != expected_voiced) {
        ++voiced_mismatches;
      }
      if (expected_voiced && result.voiced_flag[static_cast<size_t>(i)]) {
        ++voiced_compared;
        const double error = cents_error(result.f0[static_cast<size_t>(i)],
                                         ref_f0[static_cast<size_t>(i)].as_number());
        if (error <= cents_tolerance) {
          ++f0_within_tolerance;
        }
      }
      probability_abs_error +=
          std::abs(static_cast<double>(result.voiced_prob[static_cast<size_t>(i)]) -
                   ref_voiced_prob[static_cast<size_t>(i)].as_number());
      probability_sum += result.voiced_prob[static_cast<size_t>(i)];
      reference_probability_sum += ref_voiced_prob[static_cast<size_t>(i)].as_number();
    }

    REQUIRE(static_cast<double>(voiced_mismatches) / compare_frames <= max_mismatch_ratio);
    REQUIRE(voiced_compared > 0);
    REQUIRE(static_cast<double>(f0_within_tolerance) / voiced_compared >= 0.99);
    REQUIRE(probability_abs_error / compare_frames <= max_probability_error);
    const double probability_mean = probability_sum / compare_frames;
    const double reference_probability_mean = reference_probability_sum / compare_frames;
    double covariance = 0.0;
    double probability_variance = 0.0;
    double reference_probability_variance = 0.0;
    for (int i = 0; i < compare_frames; ++i) {
      const double observed = result.voiced_prob[static_cast<size_t>(i)] - probability_mean;
      const double expected =
          ref_voiced_prob[static_cast<size_t>(i)].as_number() - reference_probability_mean;
      covariance += observed * expected;
      probability_variance += observed * observed;
      reference_probability_variance += expected * expected;
    }
    const double probability_correlation =
        covariance / std::sqrt(probability_variance * reference_probability_variance);
    REQUIRE(probability_correlation >= min_probability_correlation);
  }
}
