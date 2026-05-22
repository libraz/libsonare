#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <vector>

#include "mastering/multiband/crossover.h"
#include "mastering/multiband/multiband_compressor.h"
#include "mastering/multiband/multiband_dynamic_eq.h"
#include "mastering/multiband/multiband_expander.h"
#include "mastering/multiband/multiband_imager.h"
#include "mastering/multiband/multiband_limiter.h"
#include "mastering/multiband/multiband_saturation.h"

using namespace sonare::mastering::multiband;

namespace {

constexpr double kPi = 3.14159265358979323846;

std::vector<float> sine(float frequency_hz, int sample_rate, int samples, float amplitude = 0.25f) {
  std::vector<float> out(static_cast<size_t>(samples));
  for (int i = 0; i < samples; ++i) {
    out[static_cast<size_t>(i)] =
        amplitude * static_cast<float>(std::sin(2.0 * kPi * frequency_hz * i / sample_rate));
  }
  return out;
}

std::vector<float> four_tone_signal(int sample_rate, int samples) {
  auto signal = sine(60.0f, sample_rate, samples, 0.12f);
  auto low_mid = sine(400.0f, sample_rate, samples, 0.10f);
  auto high_mid = sine(2500.0f, sample_rate, samples, 0.08f);
  auto high = sine(12000.0f, sample_rate, samples, 0.06f);
  for (size_t i = 0; i < signal.size(); ++i) {
    signal[i] += low_mid[i] + high_mid[i] + high[i];
  }
  return signal;
}

float rms_tail(const std::vector<float>& samples, size_t skip) {
  double sum = 0.0;
  size_t count = 0;
  for (size_t i = std::min(skip, samples.size()); i < samples.size(); ++i) {
    sum += static_cast<double>(samples[i]) * samples[i];
    ++count;
  }
  return count == 0 ? 0.0f : static_cast<float>(std::sqrt(sum / static_cast<double>(count)));
}

float projected_tone_amplitude(const std::vector<float>& samples, float frequency_hz,
                               int sample_rate, size_t skip) {
  double sin_sum = 0.0;
  double cos_sum = 0.0;
  size_t count = 0;
  for (size_t i = std::min(skip, samples.size()); i < samples.size(); ++i) {
    const double phase = 2.0 * kPi * frequency_hz * static_cast<double>(i) / sample_rate;
    sin_sum += samples[i] * std::sin(phase);
    cos_sum += samples[i] * std::cos(phase);
    ++count;
  }
  if (count == 0) {
    return 0.0f;
  }
  return static_cast<float>(2.0 * std::sqrt(sin_sum * sin_sum + cos_sum * cos_sum) /
                            static_cast<double>(count));
}

void process(sonare::mastering::common::ProcessorBase& processor, std::vector<float>& mono) {
  float* channels[] = {mono.data()};
  processor.process(channels, 1, static_cast<int>(mono.size()));
}

void process_stereo(sonare::mastering::common::ProcessorBase& processor, std::vector<float>& left,
                    std::vector<float>& right) {
  float* channels[] = {left.data(), right.data()};
  processor.process(channels, 2, static_cast<int>(left.size()));
}

std::vector<float> side_signal(const std::vector<float>& left, const std::vector<float>& right) {
  std::vector<float> side(left.size());
  for (size_t i = 0; i < left.size(); ++i) {
    side[i] = 0.5f * (left[i] - right[i]);
  }
  return side;
}

std::vector<float> reconstruct_mono(const CrossoverOutput& output) {
  std::vector<float> reconstructed(static_cast<size_t>(output.num_samples()), 0.0f);
  for (int band = 0; band < output.num_bands(); ++band) {
    const auto& samples = output.bands[static_cast<size_t>(band)][0];
    for (size_t i = 0; i < reconstructed.size(); ++i) {
      reconstructed[i] += samples[i];
    }
  }
  return reconstructed;
}

float max_abs_difference_tail(const std::vector<float>& a, const std::vector<float>& b,
                              size_t skip) {
  float max_error = 0.0f;
  for (size_t i = std::min(skip, a.size()); i < a.size() && i < b.size(); ++i) {
    max_error = std::max(max_error, std::abs(a[i] - b[i]));
  }
  return max_error;
}

void require_projected_amplitude_near(const std::vector<float>& samples, float frequency_hz,
                                      int sample_rate, float expected_amplitude,
                                      float tolerance_ratio) {
  const float amplitude = projected_tone_amplitude(samples, frequency_hz, sample_rate, 8192);
  REQUIRE(amplitude > expected_amplitude * (1.0f - tolerance_ratio));
  REQUIRE(amplitude < expected_amplitude * (1.0f + tolerance_ratio));
}

void require_projected_amplitude_near(const std::vector<float>& samples, float frequency_hz,
                                      float expected_amplitude, float tolerance_ratio) {
  require_projected_amplitude_near(samples, frequency_hz, 48000, expected_amplitude,
                                   tolerance_ratio);
}

void require_four_tone_amplitudes_near(const std::vector<float>& samples, float tolerance_ratio) {
  require_projected_amplitude_near(samples, 60.0f, 0.12f, tolerance_ratio);
  require_projected_amplitude_near(samples, 400.0f, 0.10f, tolerance_ratio);
  require_projected_amplitude_near(samples, 2500.0f, 0.08f, tolerance_ratio);
  require_projected_amplitude_near(samples, 12000.0f, 0.06f, tolerance_ratio);
}

}  // namespace

TEST_CASE("Crossover uses matched bands with stable recombined level", "[mastering][multiband]") {
  Crossover crossover({{200.0f, 2000.0f}, CrossoverSlope::LR4, CrossoverMode::LinkwitzRiley});
  crossover.prepare(48000.0, 1024);

  auto signal = sine(100.0f, 48000, 48000, 0.2f);
  auto high = sine(6000.0f, 48000, 48000, 0.1f);
  for (size_t i = 0; i < signal.size(); ++i) {
    signal[i] += high[i];
  }
  float* channels[] = {signal.data()};

  const auto output = crossover.split(channels, 1, static_cast<int>(signal.size()));
  const auto reconstructed = reconstruct_mono(output);

  REQUIRE(output.num_bands() == 3);
  REQUIRE(output.num_channels() == 1);
  REQUIRE(rms_tail(output.bands[0][0], 4096) > rms_tail(output.bands[2][0], 4096));
  REQUIRE(rms_tail(reconstructed, 4096) > rms_tail(signal, 4096) * 0.8f);
  REQUIRE(rms_tail(reconstructed, 4096) < rms_tail(signal, 4096) * 1.2f);
}

TEST_CASE("Crossover recombines three-way tones with flat projected amplitude",
          "[mastering][multiband]") {
  Crossover crossover({{200.0f, 2000.0f}, CrossoverSlope::LR4, CrossoverMode::LinkwitzRiley});
  crossover.prepare(48000.0, 65536);

  for (const float frequency_hz : {100.0f, 1000.0f, 8000.0f}) {
    auto signal = sine(frequency_hz, 48000, 65536, 0.25f);
    float* channels[] = {signal.data()};
    const auto output = crossover.split(channels, 1, static_cast<int>(signal.size()));
    const auto reconstructed = reconstruct_mono(output);
    const float gain = projected_tone_amplitude(reconstructed, frequency_hz, 48000, 8192) / 0.25f;

    REQUIRE(gain > 0.985f);
    REQUIRE(gain < 1.015f);
    crossover.reset();
  }
}

TEST_CASE("Crossover recombines four-way tones with flat projected amplitude",
          "[mastering][multiband]") {
  Crossover crossover(
      {{120.0f, 1000.0f, 6000.0f}, CrossoverSlope::LR4, CrossoverMode::LinkwitzRiley});
  crossover.prepare(48000.0, 65536);

  for (const float frequency_hz : {60.0f, 400.0f, 2500.0f, 12000.0f}) {
    auto signal = sine(frequency_hz, 48000, 65536, 0.25f);
    float* channels[] = {signal.data()};
    const auto output = crossover.split(channels, 1, static_cast<int>(signal.size()));
    const auto reconstructed = reconstruct_mono(output);
    const float gain = projected_tone_amplitude(reconstructed, frequency_hz, 48000, 8192) / 0.25f;

    REQUIRE(gain > 0.985f);
    REQUIRE(gain < 1.015f);
    crossover.reset();
  }
}

TEST_CASE("Crossover LR8 recombines three-way tones with flat projected amplitude",
          "[mastering][multiband]") {
  Crossover crossover({{200.0f, 2000.0f}, CrossoverSlope::LR8, CrossoverMode::LinkwitzRiley});
  crossover.prepare(48000.0, 65536);

  for (const float frequency_hz : {100.0f, 1000.0f, 8000.0f}) {
    auto signal = sine(frequency_hz, 48000, 65536, 0.25f);
    float* channels[] = {signal.data()};
    const auto output = crossover.split(channels, 1, static_cast<int>(signal.size()));
    const auto reconstructed = reconstruct_mono(output);
    const float gain = projected_tone_amplitude(reconstructed, frequency_hz, 48000, 8192) / 0.25f;

    REQUIRE(gain > 0.985f);
    REQUIRE(gain < 1.015f);
    crossover.reset();
  }
}

TEST_CASE("Crossover routes low and high tones to expected bands", "[mastering][multiband]") {
  Crossover crossover({{1000.0f}, CrossoverSlope::LR2, CrossoverMode::LinkwitzRiley});
  crossover.prepare(48000.0, 1024);

  auto low = sine(100.0f, 48000, 48000, 0.4f);
  auto high = sine(8000.0f, 48000, 48000, 0.4f);

  float* low_channels[] = {low.data()};
  const auto low_split = crossover.split(low_channels, 1, static_cast<int>(low.size()));
  crossover.reset();
  float* high_channels[] = {high.data()};
  const auto high_split = crossover.split(high_channels, 1, static_cast<int>(high.size()));

  REQUIRE(rms_tail(low_split.bands[0][0], 4096) > rms_tail(low_split.bands[1][0], 4096) * 4.0f);
  REQUIRE(rms_tail(high_split.bands[1][0], 4096) > rms_tail(high_split.bands[0][0], 4096) * 4.0f);
}

TEST_CASE("Crossover Butterworth and Bessel route low and high tones to expected bands",
          "[mastering][multiband]") {
  for (const CrossoverMode mode : {CrossoverMode::Butterworth, CrossoverMode::Bessel}) {
    Crossover crossover({{1000.0f}, CrossoverSlope::LR4, mode});
    crossover.prepare(48000.0, 1024);

    auto low = sine(100.0f, 48000, 48000, 0.4f);
    auto high = sine(8000.0f, 48000, 48000, 0.4f);

    float* low_channels[] = {low.data()};
    const auto low_split = crossover.split(low_channels, 1, static_cast<int>(low.size()));
    crossover.reset();
    float* high_channels[] = {high.data()};
    const auto high_split = crossover.split(high_channels, 1, static_cast<int>(high.size()));

    REQUIRE(rms_tail(low_split.bands[0][0], 4096) > rms_tail(low_split.bands[1][0], 4096) * 4.0f);
    REQUIRE(rms_tail(high_split.bands[1][0], 4096) > rms_tail(high_split.bands[0][0], 4096) * 4.0f);
  }
}

TEST_CASE("Crossover has Linkwitz-Riley half-gain bands at the cutoff", "[mastering][multiband]") {
  Crossover crossover({{1000.0f}, CrossoverSlope::LR4, CrossoverMode::LinkwitzRiley});
  crossover.prepare(48000.0, 65536);

  auto signal = sine(1000.0f, 48000, 65536, 0.4f);
  float* channels[] = {signal.data()};
  const auto output = crossover.split(channels, 1, static_cast<int>(signal.size()));

  const float low_gain = projected_tone_amplitude(output.bands[0][0], 1000.0f, 48000, 8192) / 0.4f;
  const float high_gain = projected_tone_amplitude(output.bands[1][0], 1000.0f, 48000, 8192) / 0.4f;
  const auto reconstructed = reconstruct_mono(output);
  const float combined_gain = projected_tone_amplitude(reconstructed, 1000.0f, 48000, 8192) / 0.4f;

  REQUIRE(low_gain > 0.485f);
  REQUIRE(low_gain < 0.515f);
  REQUIRE(high_gain > 0.485f);
  REQUIRE(high_gain < 0.515f);
  REQUIRE(combined_gain > 0.985f);
  REQUIRE(combined_gain < 1.015f);
}

TEST_CASE("Crossover LR2 has half-gain bands at the cutoff", "[mastering][multiband]") {
  Crossover crossover({{1000.0f}, CrossoverSlope::LR2, CrossoverMode::LinkwitzRiley});
  crossover.prepare(48000.0, 65536);

  auto signal = sine(1000.0f, 48000, 65536, 0.4f);
  float* channels[] = {signal.data()};
  const auto output = crossover.split(channels, 1, static_cast<int>(signal.size()));

  const float low_gain = projected_tone_amplitude(output.bands[0][0], 1000.0f, 48000, 8192) / 0.4f;
  const float high_gain = projected_tone_amplitude(output.bands[1][0], 1000.0f, 48000, 8192) / 0.4f;
  const auto reconstructed = reconstruct_mono(output);
  const float combined_gain = projected_tone_amplitude(reconstructed, 1000.0f, 48000, 8192) / 0.4f;

  REQUIRE(low_gain > 0.485f);
  REQUIRE(low_gain < 0.515f);
  REQUIRE(high_gain > 0.485f);
  REQUIRE(high_gain < 0.515f);
  REQUIRE(combined_gain > 0.985f);
  REQUIRE(combined_gain < 1.015f);
}

TEST_CASE("Crossover LR8 has half-gain bands at the cutoff", "[mastering][multiband]") {
  Crossover crossover({{1000.0f}, CrossoverSlope::LR8, CrossoverMode::LinkwitzRiley});
  crossover.prepare(48000.0, 65536);

  auto signal = sine(1000.0f, 48000, 65536, 0.4f);
  float* channels[] = {signal.data()};
  const auto output = crossover.split(channels, 1, static_cast<int>(signal.size()));

  const float low_gain = projected_tone_amplitude(output.bands[0][0], 1000.0f, 48000, 8192) / 0.4f;
  const float high_gain = projected_tone_amplitude(output.bands[1][0], 1000.0f, 48000, 8192) / 0.4f;
  const auto reconstructed = reconstruct_mono(output);
  const float combined_gain = projected_tone_amplitude(reconstructed, 1000.0f, 48000, 8192) / 0.4f;

  REQUIRE(low_gain > 0.485f);
  REQUIRE(low_gain < 0.515f);
  REQUIRE(high_gain > 0.485f);
  REQUIRE(high_gain < 0.515f);
  REQUIRE(combined_gain > 0.985f);
  REQUIRE(combined_gain < 1.015f);
}

TEST_CASE("Crossover Butterworth has minus-three-db bands at the cutoff",
          "[mastering][multiband]") {
  for (const CrossoverSlope slope :
       {CrossoverSlope::LR2, CrossoverSlope::LR4, CrossoverSlope::LR8}) {
    Crossover crossover({{1000.0f}, slope, CrossoverMode::Butterworth});
    crossover.prepare(48000.0, 65536);

    auto signal = sine(1000.0f, 48000, 65536, 0.4f);
    float* channels[] = {signal.data()};
    const auto output = crossover.split(channels, 1, static_cast<int>(signal.size()));

    const float low_gain =
        projected_tone_amplitude(output.bands[0][0], 1000.0f, 48000, 8192) / 0.4f;
    const float high_gain =
        projected_tone_amplitude(output.bands[1][0], 1000.0f, 48000, 8192) / 0.4f;

    REQUIRE(low_gain > 0.69f);
    REQUIRE(low_gain < 0.725f);
    REQUIRE(high_gain > 0.69f);
    REQUIRE(high_gain < 0.725f);
  }
}

TEST_CASE("Crossover Bessel has minus-three-db bands at the cutoff", "[mastering][multiband]") {
  for (const CrossoverSlope slope :
       {CrossoverSlope::LR2, CrossoverSlope::LR4, CrossoverSlope::LR8}) {
    Crossover crossover({{1000.0f}, slope, CrossoverMode::Bessel});
    crossover.prepare(48000.0, 65536);

    auto signal = sine(1000.0f, 48000, 65536, 0.4f);
    float* channels[] = {signal.data()};
    const auto output = crossover.split(channels, 1, static_cast<int>(signal.size()));

    const float low_gain =
        projected_tone_amplitude(output.bands[0][0], 1000.0f, 48000, 8192) / 0.4f;
    const float high_gain =
        projected_tone_amplitude(output.bands[1][0], 1000.0f, 48000, 8192) / 0.4f;

    REQUIRE(low_gain > 0.69f);
    REQUIRE(low_gain < 0.725f);
    REQUIRE(high_gain > 0.69f);
    REQUIRE(high_gain < 0.725f);
  }
}

TEST_CASE("Crossover cutoff gain is normalized across sample rates and cutoffs",
          "[mastering][multiband]") {
  struct Case {
    CrossoverMode mode;
    float expected_gain;
    float tolerance;
  };

  for (const int sample_rate : {44100, 48000, 96000}) {
    for (const float cutoff_hz : {150.0f, 1000.0f, 8000.0f}) {
      for (const auto test_case : {Case{CrossoverMode::LinkwitzRiley, 0.5f, 0.02f},
                                   Case{CrossoverMode::Butterworth, 0.70710677f, 0.025f},
                                   Case{CrossoverMode::Bessel, 0.70710677f, 0.025f}}) {
        Crossover crossover({{cutoff_hz}, CrossoverSlope::LR4, test_case.mode});
        crossover.prepare(static_cast<double>(sample_rate), 65536);

        auto signal = sine(cutoff_hz, sample_rate, 65536, 0.4f);
        float* channels[] = {signal.data()};
        const auto output = crossover.split(channels, 1, static_cast<int>(signal.size()));

        require_projected_amplitude_near(output.bands[0][0], cutoff_hz, sample_rate,
                                         0.4f * test_case.expected_gain, test_case.tolerance);
        require_projected_amplitude_near(output.bands[1][0], cutoff_hz, sample_rate,
                                         0.4f * test_case.expected_gain, test_case.tolerance);
      }
    }
  }
}

TEST_CASE("Crossover LR2 recombines three-way tones with flat projected amplitude",
          "[mastering][multiband]") {
  Crossover crossover({{200.0f, 2000.0f}, CrossoverSlope::LR2, CrossoverMode::LinkwitzRiley});
  crossover.prepare(48000.0, 65536);

  for (const float frequency_hz : {100.0f, 1000.0f, 8000.0f}) {
    auto signal = sine(frequency_hz, 48000, 65536, 0.25f);
    float* channels[] = {signal.data()};
    const auto output = crossover.split(channels, 1, static_cast<int>(signal.size()));
    const auto reconstructed = reconstruct_mono(output);
    const float gain = projected_tone_amplitude(reconstructed, frequency_hz, 48000, 8192) / 0.25f;

    REQUIRE(gain > 0.985f);
    REQUIRE(gain < 1.015f);
    crossover.reset();
  }
}

TEST_CASE("Crossover streaming output matches single block processing", "[mastering][multiband]") {
  CrossoverConfig config{{250.0f, 2200.0f}, CrossoverSlope::LR4, CrossoverMode::LinkwitzRiley};
  Crossover one_shot(config);
  Crossover streaming(config);
  one_shot.prepare(48000.0, 4096);
  streaming.prepare(48000.0, 2048);

  auto signal = sine(120.0f, 48000, 4096, 0.2f);
  auto mid = sine(1200.0f, 48000, 4096, 0.12f);
  auto high = sine(9000.0f, 48000, 4096, 0.08f);
  for (size_t i = 0; i < signal.size(); ++i) {
    signal[i] += mid[i] + high[i];
  }

  auto full_signal = signal;
  float* full_channels[] = {full_signal.data()};
  const auto full = one_shot.split(full_channels, 1, static_cast<int>(full_signal.size()));

  std::vector<std::vector<float>> streamed_bands(3, std::vector<float>(signal.size(), 0.0f));
  float* first_channels[] = {signal.data()};
  const auto first = streaming.split(first_channels, 1, 2048);
  float* second_channels[] = {signal.data() + 2048};
  const auto second = streaming.split(second_channels, 1, 2048);
  for (int band = 0; band < 3; ++band) {
    std::copy(first.bands[static_cast<size_t>(band)][0].begin(),
              first.bands[static_cast<size_t>(band)][0].end(),
              streamed_bands[static_cast<size_t>(band)].begin());
    std::copy(second.bands[static_cast<size_t>(band)][0].begin(),
              second.bands[static_cast<size_t>(band)][0].end(),
              streamed_bands[static_cast<size_t>(band)].begin() + 2048);
    REQUIRE(max_abs_difference_tail(full.bands[static_cast<size_t>(band)][0],
                                    streamed_bands[static_cast<size_t>(band)], 0) < 0.000001f);
  }
}

TEST_CASE("Crossover Butterworth and Bessel streaming output matches single block processing",
          "[mastering][multiband]") {
  for (const CrossoverMode mode : {CrossoverMode::Butterworth, CrossoverMode::Bessel}) {
    CrossoverConfig config{{250.0f, 2200.0f}, CrossoverSlope::LR4, mode};
    Crossover one_shot(config);
    Crossover streaming(config);
    one_shot.prepare(48000.0, 4096);
    streaming.prepare(48000.0, 2048);

    auto signal = sine(120.0f, 48000, 4096, 0.2f);
    auto mid = sine(1200.0f, 48000, 4096, 0.12f);
    auto high = sine(9000.0f, 48000, 4096, 0.08f);
    for (size_t i = 0; i < signal.size(); ++i) {
      signal[i] += mid[i] + high[i];
    }

    auto full_signal = signal;
    float* full_channels[] = {full_signal.data()};
    const auto full = one_shot.split(full_channels, 1, static_cast<int>(full_signal.size()));

    std::vector<std::vector<float>> streamed_bands(3, std::vector<float>(signal.size(), 0.0f));
    float* first_channels[] = {signal.data()};
    const auto first = streaming.split(first_channels, 1, 2048);
    float* second_channels[] = {signal.data() + 2048};
    const auto second = streaming.split(second_channels, 1, 2048);
    for (int band = 0; band < 3; ++band) {
      std::copy(first.bands[static_cast<size_t>(band)][0].begin(),
                first.bands[static_cast<size_t>(band)][0].end(),
                streamed_bands[static_cast<size_t>(band)].begin());
      std::copy(second.bands[static_cast<size_t>(band)][0].begin(),
                second.bands[static_cast<size_t>(band)][0].end(),
                streamed_bands[static_cast<size_t>(band)].begin() + 2048);
      REQUIRE(max_abs_difference_tail(full.bands[static_cast<size_t>(band)][0],
                                      streamed_bands[static_cast<size_t>(band)], 0) < 0.000001f);
    }
  }
}

TEST_CASE("Crossover reset clears split and compensation state", "[mastering][multiband]") {
  Crossover crossover({{250.0f, 2200.0f}, CrossoverSlope::LR4, CrossoverMode::LinkwitzRiley});
  crossover.prepare(48000.0, 4096);

  auto signal = sine(120.0f, 48000, 4096, 0.2f);
  auto mid = sine(1200.0f, 48000, 4096, 0.12f);
  auto high = sine(9000.0f, 48000, 4096, 0.08f);
  for (size_t i = 0; i < signal.size(); ++i) {
    signal[i] += mid[i] + high[i];
  }

  auto first_signal = signal;
  float* first_channels[] = {first_signal.data()};
  const auto first = crossover.split(first_channels, 1, static_cast<int>(first_signal.size()));

  crossover.reset();

  auto second_signal = signal;
  float* second_channels[] = {second_signal.data()};
  const auto second = crossover.split(second_channels, 1, static_cast<int>(second_signal.size()));

  for (int band = 0; band < first.num_bands(); ++band) {
    REQUIRE(max_abs_difference_tail(first.bands[static_cast<size_t>(band)][0],
                                    second.bands[static_cast<size_t>(band)][0], 0) < 0.000001f);
  }
}

TEST_CASE("Crossover rebuilds state when channel count changes", "[mastering][multiband]") {
  Crossover crossover({{1000.0f}, CrossoverSlope::LR4, CrossoverMode::LinkwitzRiley});
  crossover.prepare(48000.0, 1024);

  auto mono = sine(100.0f, 48000, 2048, 0.2f);
  float* mono_channels[] = {mono.data()};
  const auto mono_split = crossover.split(mono_channels, 1, static_cast<int>(mono.size()));
  REQUIRE(mono_split.num_channels() == 1);

  auto left = sine(100.0f, 48000, 48000, 0.4f);
  auto right = sine(8000.0f, 48000, 48000, 0.4f);
  float* stereo_channels[] = {left.data(), right.data()};
  const auto stereo_split = crossover.split(stereo_channels, 2, static_cast<int>(left.size()));

  REQUIRE(stereo_split.num_channels() == 2);
  REQUIRE(rms_tail(stereo_split.bands[0][0], 4096) >
          rms_tail(stereo_split.bands[1][0], 4096) * 4.0f);
  REQUIRE(rms_tail(stereo_split.bands[1][1], 4096) >
          rms_tail(stereo_split.bands[0][1], 4096) * 4.0f);
}

TEST_CASE("Crossover without cutoffs returns a single unchanged band", "[mastering][multiband]") {
  for (const CrossoverMode mode :
       {CrossoverMode::LinkwitzRiley, CrossoverMode::Butterworth, CrossoverMode::Bessel}) {
    Crossover crossover({{}, CrossoverSlope::LR4, mode});
    crossover.prepare(48000.0, 1024);

    auto signal = four_tone_signal(48000, 4096);
    float* channels[] = {signal.data()};
    const auto output = crossover.split(channels, 1, static_cast<int>(signal.size()));

    REQUIRE(output.num_bands() == 1);
    REQUIRE(output.num_channels() == 1);
    REQUIRE(max_abs_difference_tail(output.bands[0][0], signal, 0) < 0.000001f);
  }
}

TEST_CASE("Crossover set_config updates mode-specific coefficients", "[mastering][multiband]") {
  Crossover crossover({{1000.0f}, CrossoverSlope::LR4, CrossoverMode::LinkwitzRiley});
  crossover.prepare(48000.0, 1024);

  crossover.set_config({{1000.0f}, CrossoverSlope::LR4, CrossoverMode::Bessel});

  auto signal = sine(1000.0f, 48000, 65536, 0.4f);
  float* channels[] = {signal.data()};
  const auto output = crossover.split(channels, 1, static_cast<int>(signal.size()));

  const float low_gain = projected_tone_amplitude(output.bands[0][0], 1000.0f, 48000, 8192) / 0.4f;
  const float high_gain = projected_tone_amplitude(output.bands[1][0], 1000.0f, 48000, 8192) / 0.4f;

  REQUIRE(low_gain > 0.69f);
  REQUIRE(low_gain < 0.725f);
  REQUIRE(high_gain > 0.69f);
  REQUIRE(high_gain < 0.725f);
}

TEST_CASE("Crossover set_config rebuilds state when cutoff count changes",
          "[mastering][multiband]") {
  Crossover crossover({{1000.0f}, CrossoverSlope::LR4, CrossoverMode::LinkwitzRiley});
  crossover.prepare(48000.0, 4096);

  auto warmup = four_tone_signal(48000, 4096);
  float* warmup_channels[] = {warmup.data()};
  static_cast<void>(crossover.split(warmup_channels, 1, static_cast<int>(warmup.size())));

  crossover.set_config(
      {{120.0f, 1000.0f, 6000.0f}, CrossoverSlope::LR4, CrossoverMode::LinkwitzRiley});

  auto signal = four_tone_signal(48000, 65536);
  float* channels[] = {signal.data()};
  const auto output = crossover.split(channels, 1, static_cast<int>(signal.size()));
  const auto reconstructed = reconstruct_mono(output);

  REQUIRE(output.num_bands() == 4);
  require_four_tone_amplitudes_near(reconstructed, 0.015f);
}

TEST_CASE("Crossover set_config updates slope-specific coefficients with existing state",
          "[mastering][multiband]") {
  Crossover crossover({{1000.0f}, CrossoverSlope::LR4, CrossoverMode::LinkwitzRiley});
  crossover.prepare(48000.0, 65536);

  auto warmup = sine(100.0f, 48000, 2048, 0.2f);
  float* warmup_channels[] = {warmup.data()};
  static_cast<void>(crossover.split(warmup_channels, 1, static_cast<int>(warmup.size())));

  crossover.set_config({{1000.0f}, CrossoverSlope::LR2, CrossoverMode::LinkwitzRiley});

  auto signal = sine(1000.0f, 48000, 65536, 0.4f);
  float* channels[] = {signal.data()};
  const auto output = crossover.split(channels, 1, static_cast<int>(signal.size()));
  const auto reconstructed = reconstruct_mono(output);

  require_projected_amplitude_near(output.bands[0][0], 1000.0f, 0.4f * 0.5f, 0.03f);
  require_projected_amplitude_near(output.bands[1][0], 1000.0f, 0.4f * 0.5f, 0.03f);
  require_projected_amplitude_near(reconstructed, 1000.0f, 0.4f, 0.02f);
}

TEST_CASE("Crossover validates cutoff configuration", "[mastering][multiband]") {
  REQUIRE_THROWS(Crossover({{1000.0f, 500.0f}, CrossoverSlope::LR4, CrossoverMode::LinkwitzRiley}));
  REQUIRE_NOTHROW(Crossover({{1000.0f}, CrossoverSlope::LR4, CrossoverMode::Butterworth}));
  REQUIRE_NOTHROW(Crossover({{1000.0f}, CrossoverSlope::LR4, CrossoverMode::Bessel}));

  Crossover crossover({{30000.0f}, CrossoverSlope::LR4, CrossoverMode::LinkwitzRiley});
  REQUIRE_THROWS(crossover.prepare(48000.0, 1024));
}

TEST_CASE("MultibandCompressor compresses only the configured low band", "[mastering][multiband]") {
  MultibandCompressorConfig config;
  config.crossover = {{1000.0f}, CrossoverSlope::LR2, CrossoverMode::LinkwitzRiley};
  config.bands = {
      {-30.0f, 8.0f, 0.0f, 20.0f, 0.0f, 0.0f, false,
       sonare::mastering::dynamics::DetectorMode::Rms},
      {0.0f, 1.0f, 0.0f, 20.0f, 0.0f, 0.0f, false, sonare::mastering::dynamics::DetectorMode::Rms},
  };
  MultibandCompressor compressor(config);
  compressor.prepare(48000.0, 1024);

  auto signal = sine(100.0f, 48000, 48000, 0.6f);
  auto high = sine(8000.0f, 48000, 48000, 0.1f);
  for (size_t i = 0; i < signal.size(); ++i) {
    signal[i] += high[i];
  }

  auto before = signal;
  process(compressor, signal);

  Crossover analyzer(config.crossover);
  analyzer.prepare(48000.0, 1024);
  float* before_channels[] = {before.data()};
  auto before_split = analyzer.split(before_channels, 1, static_cast<int>(before.size()));
  analyzer.reset();
  float* after_channels[] = {signal.data()};
  auto after_split = analyzer.split(after_channels, 1, static_cast<int>(signal.size()));

  REQUIRE(rms_tail(after_split.bands[0][0], 4096) <
          rms_tail(before_split.bands[0][0], 4096) * 0.6f);
  REQUIRE(rms_tail(after_split.bands[1][0], 4096) >
          rms_tail(before_split.bands[1][0], 4096) * 0.65f);
  REQUIRE(compressor.last_gain_reductions_db().size() == 2);
  REQUIRE(compressor.last_gain_reductions_db()[0] < -6.0f);
}

TEST_CASE("MultibandCompressor neutral bands preserve tone amplitudes", "[mastering][multiband]") {
  MultibandCompressorConfig config;
  config.crossover = {
      {120.0f, 1000.0f, 6000.0f}, CrossoverSlope::LR4, CrossoverMode::LinkwitzRiley};
  config.bands = {
      {0.0f, 1.0f, 0.0f, 20.0f, 0.0f, 0.0f, false, sonare::mastering::dynamics::DetectorMode::Rms},
      {0.0f, 1.0f, 0.0f, 20.0f, 0.0f, 0.0f, false, sonare::mastering::dynamics::DetectorMode::Rms},
      {0.0f, 1.0f, 0.0f, 20.0f, 0.0f, 0.0f, false, sonare::mastering::dynamics::DetectorMode::Rms},
      {0.0f, 1.0f, 0.0f, 20.0f, 0.0f, 0.0f, false, sonare::mastering::dynamics::DetectorMode::Rms},
  };
  MultibandCompressor compressor(config);
  compressor.prepare(48000.0, 4096);

  auto signal = four_tone_signal(48000, 65536);

  process(compressor, signal);

  require_four_tone_amplitudes_near(signal, 0.015f);
}

TEST_CASE("MultibandCompressor validates band count", "[mastering][multiband]") {
  MultibandCompressorConfig config;
  config.crossover = {{1000.0f, 4000.0f}, CrossoverSlope::LR4, CrossoverMode::LinkwitzRiley};
  config.bands.resize(2);
  REQUIRE_THROWS(MultibandCompressor(config));
}

TEST_CASE("MultibandLimiter limits only the configured high band", "[mastering][multiband]") {
  MultibandLimiterConfig config;
  config.crossover = {{1000.0f}, CrossoverSlope::LR2, CrossoverMode::LinkwitzRiley};
  config.bands = {
      {0.0f, 0.0f, 20.0f},
      {-18.0f, 0.0f, 20.0f},
  };
  MultibandLimiter limiter(config);
  limiter.prepare(48000.0, 1024);

  auto low = sine(100.0f, 48000, 48000, 0.1f);
  auto high = sine(8000.0f, 48000, 48000, 0.8f);
  const float low_before = rms_tail(low, 4096);
  const float high_before = rms_tail(high, 4096);

  process(limiter, low);
  limiter.reset();
  process(limiter, high);

  REQUIRE(rms_tail(high, 4096) < high_before * 0.45f);
  REQUIRE(rms_tail(low, 4096) > low_before * 0.7f);
  REQUIRE(limiter.last_gain_reductions_db().size() == 2);
  REQUIRE(limiter.last_gain_reductions_db()[1] < -6.0f);
}

TEST_CASE("MultibandLimiter neutral bands preserve tone amplitudes", "[mastering][multiband]") {
  MultibandLimiterConfig config;
  config.crossover = {
      {120.0f, 1000.0f, 6000.0f}, CrossoverSlope::LR4, CrossoverMode::LinkwitzRiley};
  config.bands = {
      {0.0f, 0.0f, 20.0f},
      {0.0f, 0.0f, 20.0f},
      {0.0f, 0.0f, 20.0f},
      {0.0f, 0.0f, 20.0f},
  };
  MultibandLimiter limiter(config);
  limiter.prepare(48000.0, 4096);

  auto signal = four_tone_signal(48000, 65536);

  process(limiter, signal);

  require_four_tone_amplitudes_near(signal, 0.015f);
}

TEST_CASE("MultibandLimiter validates band count", "[mastering][multiband]") {
  MultibandLimiterConfig config;
  config.crossover = {{1000.0f, 4000.0f}, CrossoverSlope::LR4, CrossoverMode::LinkwitzRiley};
  config.bands.resize(2);
  REQUIRE_THROWS(MultibandLimiter(config));
}

TEST_CASE("MultibandExpander reduces only the configured low noise band",
          "[mastering][multiband]") {
  MultibandExpanderConfig config;
  config.crossover = {{1000.0f}, CrossoverSlope::LR2, CrossoverMode::LinkwitzRiley};
  config.bands = {
      {-20.0f, 3.0f, 0.0f, 20.0f, -50.0f},
      {-80.0f, 1.0f, 0.0f, 20.0f, -50.0f},
  };
  MultibandExpander expander(config);
  expander.prepare(48000.0, 1024);

  auto low = sine(100.0f, 48000, 48000, 0.02f);
  auto high = sine(8000.0f, 48000, 48000, 0.3f);
  const float low_before = rms_tail(low, 4096);
  const float high_before = rms_tail(high, 4096);

  process(expander, low);
  expander.reset();
  process(expander, high);

  REQUIRE(rms_tail(low, 4096) < low_before * 0.35f);
  REQUIRE(rms_tail(high, 4096) > high_before * 0.75f);
  REQUIRE(expander.last_gain_reductions_db().size() == 2);
  REQUIRE(expander.last_gain_reductions_db()[1] == 0.0f);
}

TEST_CASE("MultibandExpander neutral bands preserve tone amplitudes", "[mastering][multiband]") {
  MultibandExpanderConfig config;
  config.crossover = {
      {120.0f, 1000.0f, 6000.0f}, CrossoverSlope::LR4, CrossoverMode::LinkwitzRiley};
  config.bands = {
      {0.0f, 1.0f, 0.0f, 20.0f, -60.0f},
      {0.0f, 1.0f, 0.0f, 20.0f, -60.0f},
      {0.0f, 1.0f, 0.0f, 20.0f, -60.0f},
      {0.0f, 1.0f, 0.0f, 20.0f, -60.0f},
  };
  MultibandExpander expander(config);
  expander.prepare(48000.0, 4096);

  auto signal = four_tone_signal(48000, 65536);

  process(expander, signal);

  require_four_tone_amplitudes_near(signal, 0.015f);
}

TEST_CASE("MultibandExpander validates band count", "[mastering][multiband]") {
  MultibandExpanderConfig config;
  config.crossover = {{1000.0f, 4000.0f}, CrossoverSlope::LR4, CrossoverMode::LinkwitzRiley};
  config.bands.resize(2);
  REQUIRE_THROWS(MultibandExpander(config));
}

TEST_CASE("MultibandSaturation saturates only the configured high band", "[mastering][multiband]") {
  MultibandSaturationConfig config;
  config.crossover = {{1000.0f}, CrossoverSlope::LR2, CrossoverMode::LinkwitzRiley};
  config.bands = {
      {0.0f, 0.0f, 0.0f, true},
      {12.0f, 1.0f, -12.0f, true},
  };
  MultibandSaturation saturation(config);
  saturation.prepare(48000.0, 1024);

  auto low = sine(100.0f, 48000, 48000, 0.4f);
  auto high = sine(8000.0f, 48000, 48000, 0.8f);
  const auto low_before = low;
  const auto high_before = high;

  process(saturation, low);
  saturation.reset();
  process(saturation, high);

  REQUIRE(rms_tail(low, 4096) > rms_tail(low_before, 4096) * 0.9f);
  REQUIRE(rms_tail(high, 4096) < rms_tail(high_before, 4096) * 0.75f);
  REQUIRE(high[48000 / 32] != high_before[48000 / 32]);
}

TEST_CASE("MultibandSaturation disabled bands preserve tone amplitudes", "[mastering][multiband]") {
  MultibandSaturationConfig config;
  config.crossover = {
      {120.0f, 1000.0f, 6000.0f}, CrossoverSlope::LR4, CrossoverMode::LinkwitzRiley};
  config.bands = {
      {0.0f, 1.0f, 0.0f, false},
      {0.0f, 1.0f, 0.0f, false},
      {0.0f, 1.0f, 0.0f, false},
      {0.0f, 1.0f, 0.0f, false},
  };
  MultibandSaturation saturation(config);
  saturation.prepare(48000.0, 4096);

  auto signal = four_tone_signal(48000, 65536);

  process(saturation, signal);

  require_four_tone_amplitudes_near(signal, 0.015f);
}

TEST_CASE("MultibandSaturation validates configuration", "[mastering][multiband]") {
  MultibandSaturationConfig config;
  config.crossover = {{1000.0f, 4000.0f}, CrossoverSlope::LR4, CrossoverMode::LinkwitzRiley};
  config.bands.resize(2);
  REQUIRE_THROWS(MultibandSaturation(config));

  config.crossover = {{1000.0f}, CrossoverSlope::LR4, CrossoverMode::LinkwitzRiley};
  config.bands = {{}, {0.0f, 1.5f, 0.0f, true}};
  REQUIRE_THROWS(MultibandSaturation(config));
}

TEST_CASE("MultibandDynamicEq applies dynamic EQ inside selected band", "[mastering][multiband]") {
  MultibandDynamicEqConfig config;
  config.crossover = {{1000.0f}, CrossoverSlope::LR2, CrossoverMode::LinkwitzRiley};
  config.bands = {
      {{sonare::mastering::eq::EqBandType::Peak, 100.0f, 0.0f, 1.0f, -36.0f, 4.0f, -12.0f, true}},
      {},
  };
  MultibandDynamicEq dynamic_eq(config);
  dynamic_eq.prepare(48000.0, 1024);

  auto low = sine(100.0f, 48000, 48000, 0.5f);
  auto high = sine(8000.0f, 48000, 48000, 0.5f);
  const float low_before = rms_tail(low, 4096);
  const float high_before = rms_tail(high, 4096);

  process(dynamic_eq, low);
  const float low_applied_gain_db = dynamic_eq.last_applied_gain_db()[0][0];
  dynamic_eq.reset();
  process(dynamic_eq, high);

  REQUIRE(rms_tail(low, 4096) < low_before * 0.75f);
  REQUIRE(rms_tail(high, 4096) > high_before * 0.75f);
  REQUIRE(dynamic_eq.last_applied_gain_db().size() == 2);
  REQUIRE(low_applied_gain_db < -6.0f);
}

TEST_CASE("MultibandDynamicEq empty bands preserve tone amplitudes", "[mastering][multiband]") {
  MultibandDynamicEqConfig config;
  config.crossover = {
      {120.0f, 1000.0f, 6000.0f}, CrossoverSlope::LR4, CrossoverMode::LinkwitzRiley};
  config.bands = {{}, {}, {}, {}};
  MultibandDynamicEq dynamic_eq(config);
  dynamic_eq.prepare(48000.0, 4096);

  auto signal = four_tone_signal(48000, 65536);

  process(dynamic_eq, signal);

  require_four_tone_amplitudes_near(signal, 0.015f);
}

TEST_CASE("MultibandDynamicEq validates band count and dynamic band count",
          "[mastering][multiband]") {
  MultibandDynamicEqConfig config;
  config.crossover = {{1000.0f, 4000.0f}, CrossoverSlope::LR4, CrossoverMode::LinkwitzRiley};
  config.bands.resize(2);
  REQUIRE_THROWS(MultibandDynamicEq(config));

  config.crossover = {{1000.0f}, CrossoverSlope::LR4, CrossoverMode::LinkwitzRiley};
  config.bands = {{},
                  std::vector<sonare::mastering::eq::DynamicEqBand>(
                      sonare::mastering::eq::DynamicEq::kMaxBands + 1)};
  REQUIRE_THROWS(MultibandDynamicEq(config));
}

TEST_CASE("MultibandImager widens only the configured high band", "[mastering][multiband]") {
  MultibandImagerConfig config;
  config.crossover = {{1000.0f}, CrossoverSlope::LR2, CrossoverMode::LinkwitzRiley};
  config.bands = {
      {1.0f, true},
      {2.0f, true},
  };
  MultibandImager imager(config);
  imager.prepare(48000.0, 1024);

  auto low_left = sine(100.0f, 48000, 48000, 0.3f);
  auto low_right = low_left;
  auto high_left = sine(8000.0f, 48000, 48000, 0.2f);
  auto high_right = high_left;
  for (auto& sample : high_right) {
    sample = -sample;
  }

  const float low_side_before = rms_tail(side_signal(low_left, low_right), 4096);
  const float high_side_before = rms_tail(side_signal(high_left, high_right), 4096);

  process_stereo(imager, low_left, low_right);
  imager.reset();
  process_stereo(imager, high_left, high_right);

  const float low_side_after = rms_tail(side_signal(low_left, low_right), 4096);
  const float high_side_after = rms_tail(side_signal(high_left, high_right), 4096);

  REQUIRE(low_side_after <= low_side_before + 0.000001f);
  REQUIRE(high_side_after > high_side_before * 1.5f);
}

TEST_CASE("MultibandImager mono input preserves level", "[mastering][multiband]") {
  MultibandImagerConfig config;
  config.crossover = {{1000.0f}, CrossoverSlope::LR4, CrossoverMode::LinkwitzRiley};
  config.bands = {{0.0f, true}, {2.0f, true}};
  MultibandImager imager(config);
  imager.prepare(48000.0, 1024);

  auto mono = sine(500.0f, 48000, 48000, 0.3f);
  auto before = mono;
  process(imager, mono);

  REQUIRE(rms_tail(mono, 4096) > rms_tail(before, 4096) * 0.95f);
  REQUIRE(rms_tail(mono, 4096) < rms_tail(before, 4096) * 1.05f);
}

TEST_CASE("MultibandImager neutral bands preserve stereo tone amplitudes",
          "[mastering][multiband]") {
  MultibandImagerConfig config;
  config.crossover = {
      {120.0f, 1000.0f, 6000.0f}, CrossoverSlope::LR4, CrossoverMode::LinkwitzRiley};
  config.bands = {
      {1.0f, true},
      {1.0f, true},
      {1.0f, true},
      {1.0f, true},
  };
  MultibandImager imager(config);
  imager.prepare(48000.0, 4096);

  auto left = four_tone_signal(48000, 65536);
  auto right = left;

  process_stereo(imager, left, right);

  require_four_tone_amplitudes_near(left, 0.015f);
  require_four_tone_amplitudes_near(right, 0.015f);
}

TEST_CASE("MultibandImager validates configuration", "[mastering][multiband]") {
  MultibandImagerConfig config;
  config.crossover = {{1000.0f, 4000.0f}, CrossoverSlope::LR4, CrossoverMode::LinkwitzRiley};
  config.bands.resize(2);
  REQUIRE_THROWS(MultibandImager(config));

  config.crossover = {{1000.0f}, CrossoverSlope::LR4, CrossoverMode::LinkwitzRiley};
  config.bands = {{1.0f, true}, {-1.0f, true}};
  REQUIRE_THROWS(MultibandImager(config));
}
