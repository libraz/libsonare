#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <vector>

#include "mastering/spectral/air_band.h"
#include "mastering/spectral/low_end_focus.h"
#include "mastering/spectral/presence_enhancer.h"
#include "mastering/spectral/spectral_shaper.h"

using namespace sonare::mastering::spectral;

namespace {

float peak_abs(const std::vector<float>& samples) {
  float peak = 0.0f;
  for (float sample : samples) peak = std::max(peak, std::abs(sample));
  return peak;
}

float rms(const std::vector<float>& samples) {
  double sum = 0.0;
  for (float sample : samples) sum += static_cast<double>(sample) * sample;
  return static_cast<float>(std::sqrt(sum / static_cast<double>(samples.size())));
}

std::vector<float> sine(float frequency_hz, int sample_rate, int samples, float amplitude) {
  std::vector<float> out(static_cast<size_t>(samples));
  for (int i = 0; i < samples; ++i) {
    out[static_cast<size_t>(i)] =
        amplitude *
        static_cast<float>(std::sin(2.0 * 3.14159265358979323846 * frequency_hz * i / sample_rate));
  }
  return out;
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

void process(sonare::mastering::common::ProcessorBase& processor, std::vector<float>& mono) {
  float* channels[] = {mono.data()};
  processor.process(channels, 1, static_cast<int>(mono.size()));
}

}  // namespace

TEST_CASE("SpectralShaper reduces material above threshold", "[mastering][spectral]") {
  SpectralShaper shaper({0.05f, 1.0f, 500.0f, 8000.0f, 0.0f, 0.0f, 24.0f});
  shaper.prepare(48000.0, 512);

  auto signal = sine(4000.0f, 48000, 512, 0.5f);
  const float before = rms_tail(signal, 128);
  process(shaper, signal);

  REQUIRE(rms_tail(signal, 128) < before * 0.75f);
  REQUIRE(shaper.last_reduction_db() < 0.0f);
}

TEST_CASE("SpectralShaper targets high-band energy more than low-band energy",
          "[mastering][spectral]") {
  SpectralShaper shaper({0.05f, 1.0f, 2000.0f, 6000.0f, 0.0f, 0.0f, 24.0f});
  shaper.prepare(48000.0, 2048);

  auto low = sine(500.0f, 48000, 2048, 0.5f);
  auto target = sine(4000.0f, 48000, 2048, 0.5f);

  const float low_before = rms_tail(low, 512);
  const float target_before = rms_tail(target, 512);
  process(shaper, low);
  shaper.reset();
  process(shaper, target);

  REQUIRE(rms_tail(low, 512) > low_before * 0.85f);
  REQUIRE(rms_tail(target, 512) < target_before * 0.7f);
}

TEST_CASE("SpectralShaper attack slows initial gain reduction", "[mastering][spectral]") {
  SpectralShaper fast({0.05f, 1.0f, 2000.0f, 6000.0f, 0.0f, 20.0f, 24.0f});
  SpectralShaper slow({0.05f, 1.0f, 2000.0f, 6000.0f, 50.0f, 20.0f, 24.0f});
  fast.prepare(48000.0, 256);
  slow.prepare(48000.0, 256);

  auto fast_signal = sine(4000.0f, 48000, 256, 0.5f);
  auto slow_signal = fast_signal;
  const float before = rms(fast_signal);
  process(fast, fast_signal);
  process(slow, slow_signal);

  REQUIRE(rms(fast_signal) < before * 0.8f);
  REQUIRE(rms(slow_signal) > rms(fast_signal) * 1.1f);
}

TEST_CASE("LowEndFocus narrows low-frequency stereo difference", "[mastering][spectral]") {
  LowEndFocus focus({20000.0f, 0.0f});
  focus.prepare(48000.0, 8);

  std::vector<float> left(32, 1.0f);
  std::vector<float> right(32, -1.0f);
  float* channels[] = {left.data(), right.data()};
  focus.process(channels, 2, static_cast<int>(left.size()));

  REQUIRE(std::abs(left.back() - right.back()) < 2.0f);
}

TEST_CASE("AirBand adds high-frequency detail", "[mastering][spectral]") {
  AirBand air({0.5f});
  air.prepare(48000.0, 16);

  std::vector<float> signal = {0.0f, 0.4f, -0.4f, 0.4f, -0.4f};
  const float before = peak_abs(signal);
  process(air, signal);

  REQUIRE(peak_abs(signal) > before);
}

TEST_CASE("PresenceEnhancer increases RMS with harmonic drive", "[mastering][spectral]") {
  PresenceEnhancer enhancer({0.3f, 3.0f});
  enhancer.prepare(48000.0, 16);

  std::vector<float> signal = {0.1f, -0.1f, 0.2f, -0.2f};
  const float before = rms(signal);
  process(enhancer, signal);

  REQUIRE(rms(signal) > before);
}

TEST_CASE("Spectral processors validate configuration and state", "[mastering][spectral]") {
  REQUIRE_THROWS(SpectralShaper({0.1f, 1.5f}));
  REQUIRE_THROWS(SpectralShaper({0.1f, 0.5f, 6000.0f, 2000.0f}));
  REQUIRE_THROWS(LowEndFocus({0.0f, 0.0f}));
  REQUIRE_THROWS(AirBand({-0.1f}));
  REQUIRE_THROWS(PresenceEnhancer({0.1f, 0.0f}));

  SpectralShaper unprepared;
  std::vector<float> signal(4, 0.0f);
  float* channels[] = {signal.data()};
  REQUIRE_THROWS(unprepared.process(channels, 1, 4));
}
