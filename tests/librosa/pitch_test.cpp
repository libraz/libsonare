/// @file pitch_test.cpp
/// @brief librosa compatibility tests for YIN pitch detection.
/// @details Reference values from: tests/librosa/reference/yin.json

#include "feature/pitch.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <cmath>
#include <string>
#include <vector>

#include "util/json_reader.h"
#include "util/math_utils.h"

using namespace sonare;
using namespace sonare::test;
using Catch::Matchers::WithinRel;

TEST_CASE("YIN pitch librosa compatibility", "[pitch][librosa]") {
  auto json = JsonReader::parse_file("tests/librosa/reference/yin.json");
  const auto& data = json["data"].as_array();

  for (const auto& entry : data) {
    std::string signal_name = entry["signal"].as_string();
    int sr = entry["sr"].as_int();
    float fmin = entry["fmin"].as_float();
    float fmax = entry["fmax"].as_float();
    int frame_length = entry["frame_length"].as_int();
    int hop_length = entry["hop_length"].as_int();
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

        PitchResult result = yin_track(audio, config);

        // libsonare and librosa may produce different frame counts due to
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

        PitchResult result = yin_track(audio, config);

        // libsonare and librosa may produce different frame counts due to
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
