/// @file ensemble_test.cpp
/// @brief String-machine ensemble (effects/modulation/ensemble): dual-rate
///        3-phase modulation audibly animates a static tone, the inverted
///        right-channel LFO polarity decorrelates the stereo image, the BBD
///        tone control darkens the wet path, and rendering is deterministic.

#include "effects/modulation/ensemble.h"

#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <complex>
#include <vector>

#include "core/fft.h"
#ifdef SONARE_WITH_MASTERING
#include "mastering/api/insert_factory.h"
#endif

namespace {

using sonare::effects::modulation::Ensemble;
using sonare::effects::modulation::EnsembleConfig;

constexpr double kRate = 48000.0;
constexpr int kFft = 8192;
constexpr int kNumSamples = 48000;

std::vector<float> sine(double freq_hz, float amplitude, int num_samples) {
  std::vector<float> buf(static_cast<size_t>(num_samples));
  for (int i = 0; i < num_samples; ++i) {
    buf[static_cast<size_t>(i)] =
        amplitude * static_cast<float>(std::sin(2.0 * 3.14159265358979 * freq_hz * i / kRate));
  }
  return buf;
}

std::vector<float> square(double freq_hz, float amplitude, int num_samples) {
  std::vector<float> buf = sine(freq_hz, 1.0f, num_samples);
  for (float& s : buf) s = s >= 0.0f ? amplitude : -amplitude;
  return buf;
}

struct StereoOut {
  std::vector<float> left;
  std::vector<float> right;
};

StereoOut process_stereo(const EnsembleConfig& config, const std::vector<float>& input) {
  StereoOut out{input, input};
  Ensemble ensemble(config);
  ensemble.prepare(kRate, 512);
  for (size_t off = 0; off + 512 <= input.size(); off += 512) {
    float* block[2] = {out.left.data() + off, out.right.data() + off};
    ensemble.process(block, 2, 512);
  }
  return out;
}

float rms(const std::vector<float>& buf, size_t from, size_t to) {
  double acc = 0.0;
  size_t n = 0;
  for (size_t i = from; i < to && i < buf.size(); ++i) {
    acc += static_cast<double>(buf[i]) * buf[i];
    ++n;
  }
  return n > 0 ? static_cast<float>(std::sqrt(acc / static_cast<double>(n))) : 0.0f;
}

/// Fraction of spectral power above @p freq_hz (Hann window at @p from).
double high_band_fraction(const std::vector<float>& buf, size_t from, double freq_hz) {
  std::vector<float> windowed(kFft);
  for (int i = 0; i < kFft; ++i) {
    const double w = 0.5 - 0.5 * std::cos(2.0 * 3.14159265358979 * i / (kFft - 1));
    windowed[static_cast<size_t>(i)] = buf[from + static_cast<size_t>(i)] * static_cast<float>(w);
  }
  sonare::FFT fft(kFft);
  std::vector<std::complex<float>> spectrum(static_cast<size_t>(fft.n_bins()));
  fft.forward(windowed.data(), spectrum.data());
  const int split = static_cast<int>(std::lround(freq_hz / kRate * kFft));
  double low = 0.0;
  double high = 0.0;
  for (int b = 1; b < static_cast<int>(spectrum.size()); ++b) {
    (b >= split ? high : low) += std::norm(spectrum[static_cast<size_t>(b)]);
  }
  const double total = low + high;
  return total > 0.0 ? high / total : 0.0;
}

}  // namespace

TEST_CASE("the 3-phase modulation animates a static tone", "[effects][modulation][ensemble]") {
  // The swept taps beat against the dry path and each other, so the level of
  // a constant-amplitude sine must visibly undulate across the render.
  const StereoOut out = process_stereo({}, sine(440.0, 0.3f, kNumSamples));
  float lo = 1.0e9f;
  float hi = 0.0f;
  for (size_t from = 4800; from + 4800 <= out.left.size(); from += 4800) {
    const float level = rms(out.left, from, from + 4800);
    lo = std::min(lo, level);
    hi = std::max(hi, level);
  }
  REQUIRE(lo > 0.0f);
  REQUIRE(hi > 1.2f * lo);
}

TEST_CASE("inverted right-channel LFO polarity decorrelates the image",
          "[effects][modulation][ensemble]") {
  const StereoOut out = process_stereo({}, sine(440.0, 0.3f, kNumSamples));
  std::vector<float> diff(out.left.size());
  for (size_t i = 0; i < diff.size(); ++i) diff[i] = out.left[i] - out.right[i];
  // A mono input must come out genuinely two-channel.
  REQUIRE(rms(diff, 4800, diff.size()) > 0.1f * rms(out.left, 4800, out.left.size()));
}

TEST_CASE("the BBD tone control darkens the wet path", "[effects][modulation][ensemble]") {
  EnsembleConfig dark;
  dark.dry_wet = 1.0f;  // wet only, so the one-pole is the whole story
  dark.tone_hz = 1000.0f;
  EnsembleConfig bright = dark;
  bright.tone_hz = 18000.0f;
  const std::vector<float> input = square(220.0, 0.25f, kNumSamples);
  const StereoOut dark_out = process_stereo(dark, input);
  const StereoOut bright_out = process_stereo(bright, input);
  REQUIRE(high_band_fraction(bright_out.left, kNumSamples - kFft, 3000.0) >
          2.0 * high_band_fraction(dark_out.left, kNumSamples - kFft, 3000.0));
}

TEST_CASE("ensemble renders deterministically", "[effects][modulation][ensemble]") {
  const std::vector<float> input = sine(330.0, 0.25f, kNumSamples);
  const StereoOut a = process_stereo({}, input);
  const StereoOut b = process_stereo({}, input);
  REQUIRE(a.left == b.left);
  REQUIRE(a.right == b.right);
}

#ifdef SONARE_WITH_MASTERING
TEST_CASE("effects.modulation.ensemble builds through the insert factory",
          "[effects][modulation][ensemble][insert_factory]") {
  const auto names = sonare::mastering::api::insert_factory_names();
  bool listed = false;
  for (const auto& name : names) listed |= name == "effects.modulation.ensemble";
  REQUIRE(listed);

  auto processor = sonare::mastering::api::make_insert(
      "effects.modulation.ensemble",
      R"({"rateSlowHz":0.5,"rateFastHz":6,"depthSlowMs":2,"depthFastMs":0.3,"centerDelayMs":6,"toneHz":7000,"dryWet":0.6})");
  REQUIRE(processor != nullptr);
  REQUIRE(dynamic_cast<Ensemble*>(processor.get()) != nullptr);
}
#endif
