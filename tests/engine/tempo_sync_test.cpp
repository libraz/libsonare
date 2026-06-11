#include "engine/tempo_sync.h"

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <cmath>
#include <complex>
#include <vector>

using Catch::Matchers::WithinAbs;

namespace {

constexpr double kPi = 3.14159265358979323846;

std::complex<double> project_tone(const std::vector<float>& samples, double sample_rate,
                                  double frequency) {
  std::complex<double> acc{};
  for (size_t i = 0; i < samples.size(); ++i) {
    const double phase = -2.0 * kPi * frequency * static_cast<double>(i) / sample_rate;
    acc += static_cast<double>(samples[i]) * std::complex<double>(std::cos(phase), std::sin(phase));
  }
  return acc;
}

double wrapped_phase_delta(double a, double b) { return std::remainder(a - b, 2.0 * kPi); }

}  // namespace

TEST_CASE("tempo sync converts musical values to PPQ", "[engine][tempo_sync]") {
  REQUIRE_THAT(sonare::engine::tempo_sync_ppq({4, sonare::transport::NoteModifier::kStraight}),
               WithinAbs(1.0, 1.0e-9));
  REQUIRE_THAT(sonare::engine::tempo_sync_ppq({8, sonare::transport::NoteModifier::kDotted}),
               WithinAbs(0.75, 1.0e-9));
  REQUIRE_THAT(sonare::engine::tempo_sync_ppq({4, sonare::transport::NoteModifier::kTriplet}),
               WithinAbs(2.0 / 3.0, 1.0e-9));
}

TEST_CASE("tempo sync follows TransportState bpm and sample rate", "[engine][tempo_sync]") {
  sonare::transport::TransportState state{};
  state.bpm = 120.0;
  state.sample_rate = 48000.0;
  REQUIRE(sonare::engine::tempo_sync_samples({4, sonare::transport::NoteModifier::kStraight},
                                             state) == 24000);

  state.bpm = 60.0;
  REQUIRE(sonare::engine::tempo_sync_samples({4, sonare::transport::NoteModifier::kStraight},
                                             state) == 48000);
}

TEST_CASE("tempo sync warp bake smooths segment joins without changing target length",
          "[engine][tempo_sync]") {
  constexpr int kSamplesPerSegment = 2048;
  std::vector<float> source(static_cast<size_t>(kSamplesPerSegment * 2));
  std::fill(source.begin(), source.begin() + kSamplesPerSegment, 0.75f);
  std::fill(source.begin() + kSamplesPerSegment, source.end(), -0.75f);

  const std::vector<sonare::engine::TempoSyncWarpSegment> segments{
      {0, kSamplesPerSegment, kSamplesPerSegment},
      {kSamplesPerSegment, kSamplesPerSegment, kSamplesPerSegment},
  };
  sonare::engine::TempoSyncWarpBakeConfig config;
  config.sample_rate = 48000;
  config.n_fft = 512;
  config.hop_length = 128;

  config.join_crossfade_samples = 0;
  const std::vector<float> hard =
      sonare::engine::bake_tempo_sync_warp_channel(source.data(), source.size(), segments, config);

  config.join_crossfade_samples = 64;
  const std::vector<float> smoothed =
      sonare::engine::bake_tempo_sync_warp_channel(source.data(), source.size(), segments, config);

  REQUIRE(hard.size() == source.size());
  REQUIRE(smoothed.size() == source.size());
  const size_t boundary = static_cast<size_t>(kSamplesPerSegment);
  const float hard_jump = std::abs(hard[boundary] - hard[boundary - 1]);
  const float smoothed_jump = std::abs(smoothed[boundary] - smoothed[boundary - 1]);
  REQUIRE(smoothed_jump < hard_jump);
}

TEST_CASE("tempo sync stereo warp shares phase rotation across channels", "[engine][tempo_sync]") {
  constexpr double kSampleRate = 48000.0;
  constexpr double kFrequency = 750.0;
  constexpr double kPhaseOffset = 0.73;
  constexpr size_t kSourceSamples = 8192;
  constexpr size_t kTargetSamples = 12288;

  std::vector<float> left(kSourceSamples);
  std::vector<float> right(kSourceSamples);
  for (size_t i = 0; i < kSourceSamples; ++i) {
    const double t = static_cast<double>(i) / kSampleRate;
    left[i] = static_cast<float>(0.75 * std::sin(2.0 * kPi * kFrequency * t));
    right[i] = static_cast<float>(0.62 * std::sin(2.0 * kPi * kFrequency * t + kPhaseOffset) +
                                  0.025 * std::sin(2.0 * kPi * 1320.0 * t));
  }

  const std::vector<sonare::engine::TempoSyncWarpSegment> segments{
      {0, kSourceSamples, kTargetSamples},
  };
  sonare::engine::TempoSyncWarpBakeConfig config;
  config.sample_rate = static_cast<int>(kSampleRate);
  config.n_fft = 1024;
  config.hop_length = 256;
  config.join_crossfade_samples = 0;

  const std::vector<const float*> sources{left.data(), right.data()};
  const std::vector<std::vector<float>> stretched =
      sonare::engine::bake_tempo_sync_warp_channels(sources, kSourceSamples, segments, config);

  REQUIRE(stretched.size() == 2);
  REQUIRE(stretched[0].size() == kTargetSamples);
  REQUIRE(stretched[1].size() == kTargetSamples);

  const double original_delta = std::arg(project_tone(right, kSampleRate, kFrequency)) -
                                std::arg(project_tone(left, kSampleRate, kFrequency));
  const double stretched_frequency =
      kFrequency * static_cast<double>(kSourceSamples) / static_cast<double>(kTargetSamples);
  const double stretched_delta =
      std::arg(project_tone(stretched[1], kSampleRate, stretched_frequency)) -
      std::arg(project_tone(stretched[0], kSampleRate, stretched_frequency));
  REQUIRE(std::abs(wrapped_phase_delta(stretched_delta, original_delta)) < 0.18);
}
