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

void process(sonare::mastering::common::ProcessorBase& processor, std::vector<float>& mono) {
  float* channels[] = {mono.data()};
  processor.process(channels, 1, static_cast<int>(mono.size()));
}

}  // namespace

TEST_CASE("SpectralShaper reduces material above threshold", "[mastering][spectral]") {
  SpectralShaper shaper({0.25f, 0.75f});
  shaper.prepare(48000.0, 16);

  std::vector<float> signal = {0.1f, 0.2f, 0.5f, -0.6f};
  process(shaper, signal);

  REQUIRE(peak_abs(signal) < 0.6f);
  REQUIRE(shaper.last_reduction_db() < 0.0f);
}

TEST_CASE("SpectralShaper targets high-band energy more than low-band energy",
          "[mastering][spectral]") {
  SpectralShaper shaper({0.05f, 1.0f, 1000.0f});
  shaper.prepare(48000.0, 256);

  std::vector<float> low(256, 0.5f);
  std::vector<float> high(256);
  for (size_t i = 0; i < high.size(); ++i) high[i] = (i % 2 == 0) ? 0.5f : -0.5f;

  const float low_before = rms(low);
  const float high_before = rms(high);
  process(shaper, low);
  shaper.reset();
  process(shaper, high);

  REQUIRE(rms(low) > low_before * 0.85f);
  REQUIRE(rms(high) < high_before * 0.7f);
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
  REQUIRE_THROWS(LowEndFocus({0.0f, 0.0f}));
  REQUIRE_THROWS(AirBand({-0.1f}));
  REQUIRE_THROWS(PresenceEnhancer({0.1f, 0.0f}));

  SpectralShaper unprepared;
  std::vector<float> signal(4, 0.0f);
  float* channels[] = {signal.data()};
  REQUIRE_THROWS(unprepared.process(channels, 1, 4));
}
