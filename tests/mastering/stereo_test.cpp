#include "analysis/meter/stereo.h"

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <cmath>
#include <vector>

#include "mastering/stereo/auto_pan.h"
#include "mastering/stereo/haas_enhancer.h"
#include "mastering/stereo/imager.h"
#include "mastering/stereo/mid_side.h"
#include "mastering/stereo/mono_compat_check.h"
#include "mastering/stereo/mono_maker.h"
#include "mastering/stereo/phase_align.h"
#include "mastering/stereo/stereo_balance.h"

using Catch::Matchers::WithinAbs;
using namespace sonare::mastering::stereo;

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

float rms_tail(const std::vector<float>& samples, size_t skip) {
  double sum = 0.0;
  size_t count = 0;
  for (size_t i = std::min(skip, samples.size()); i < samples.size(); ++i) {
    sum += static_cast<double>(samples[i]) * samples[i];
    ++count;
  }
  return count == 0 ? 0.0f : static_cast<float>(std::sqrt(sum / static_cast<double>(count)));
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

}  // namespace

TEST_CASE("MidSide encode and decode round-trips stereo buffers", "[mastering][stereo]") {
  std::vector<float> left = {1.0f, 0.25f, -0.5f, 0.0f};
  std::vector<float> right = {0.25f, -0.75f, -0.5f, 1.0f};
  std::vector<float> mid(left.size());
  std::vector<float> side(left.size());
  std::vector<float> decoded_left(left.size());
  std::vector<float> decoded_right(left.size());

  encode_buffer(left.data(), right.data(), mid.data(), side.data(), left.size());
  decode_buffer(mid.data(), side.data(), decoded_left.data(), decoded_right.data(), left.size());

  for (size_t i = 0; i < left.size(); ++i) {
    REQUIRE_THAT(decoded_left[i], WithinAbs(left[i], 0.000001f));
    REQUIRE_THAT(decoded_right[i], WithinAbs(right[i], 0.000001f));
  }
}

TEST_CASE("Imager increases and collapses stereo width", "[mastering][stereo]") {
  auto left = sine(1000.0f, 48000, 48000, 0.25f);
  auto right = left;
  for (auto& sample : right) {
    sample *= -1.0f;
  }
  const float side_before = rms_tail(side_signal(left, right), 4096);

  Imager wide({2.0f, 0.0f});
  wide.prepare(48000.0, 1024);
  process_stereo(wide, left, right);
  REQUIRE(rms_tail(side_signal(left, right), 4096) > side_before * 1.9f);

  Imager mono({0.0f, 0.0f});
  mono.prepare(48000.0, 1024);
  process_stereo(mono, left, right);
  REQUIRE(rms_tail(side_signal(left, right), 4096) < 0.000001f);
}

TEST_CASE("Imager validates width", "[mastering][stereo]") {
  REQUIRE_THROWS(Imager({-1.0f, 0.0f}));
}

TEST_CASE("MonoMaker collapses stereo to identical channels", "[mastering][stereo]") {
  auto left = sine(100.0f, 48000, 48000, 0.3f);
  auto right = sine(100.0f, 48000, 48000, 0.1f);

  MonoMaker mono_maker({1.0f});
  mono_maker.prepare(48000.0, 1024);
  process_stereo(mono_maker, left, right);

  REQUIRE(rms_tail(side_signal(left, right), 4096) < 0.000001f);
}

TEST_CASE("MonoMaker validates amount", "[mastering][stereo]") {
  REQUIRE_THROWS(MonoMaker({-0.1f}));
  REQUIRE_THROWS(MonoMaker({1.1f}));
}

TEST_CASE("StereoBalance attenuates the opposite side", "[mastering][stereo]") {
  std::vector<float> left(48000, 0.5f);
  std::vector<float> right(48000, 0.5f);
  const float left_before = rms_tail(left, 0);
  const float right_before = rms_tail(right, 0);

  StereoBalance balance({0.5f, false});
  balance.prepare(48000.0, 1024);
  process_stereo(balance, left, right);

  REQUIRE(rms_tail(left, 0) < left_before * 0.55f);
  REQUIRE(rms_tail(right, 0) == right_before);
}

TEST_CASE("StereoBalance validates balance", "[mastering][stereo]") {
  REQUIRE_THROWS(StereoBalance({-1.1f, true}));
  REQUIRE_THROWS(StereoBalance({1.1f, true}));
}

TEST_CASE("AutoPan modulates stereo gains over time", "[mastering][stereo]") {
  std::vector<float> left(1000, 1.0f);
  std::vector<float> right(1000, 1.0f);

  AutoPan pan({1.0f, 1.0f, 0.0f});
  pan.prepare(1000.0, 1000);
  process_stereo(pan, left, right);

  REQUIRE_THAT(left[0], WithinAbs(0.70710678f, 0.0001f));
  REQUIRE_THAT(right[0], WithinAbs(0.70710678f, 0.0001f));
  REQUIRE(left[250] < 0.01f);
  REQUIRE(right[250] > 0.99f);
}

TEST_CASE("AutoPan validates configuration", "[mastering][stereo]") {
  REQUIRE_THROWS(AutoPan({-1.0f, 1.0f, 0.0f}));
  REQUIRE_THROWS(AutoPan({1.0f, 1.1f, 0.0f}));
}

TEST_CASE("HaasEnhancer delays one side from the opposite channel", "[mastering][stereo]") {
  std::vector<float> left = {1.0f, 0.0f, 0.0f, 0.0f};
  std::vector<float> right(4, 0.0f);

  HaasEnhancer haas({1.0f, 1.0f, true});
  haas.prepare(1000.0, 4);
  process_stereo(haas, left, right);

  REQUIRE(haas.delay_samples() == 1);
  REQUIRE_THAT(right[0], WithinAbs(0.0f, 0.000001f));
  REQUIRE_THAT(right[1], WithinAbs(1.0f, 0.000001f));
}

TEST_CASE("HaasEnhancer validates configuration", "[mastering][stereo]") {
  REQUIRE_THROWS(HaasEnhancer({-1.0f, 1.0f, true}));
  REQUIRE_THROWS(HaasEnhancer({1.0f, 1.5f, true}));
}

TEST_CASE("PhaseAlign delays the selected channel by integer samples", "[mastering][stereo]") {
  std::vector<float> left(5, 0.0f);
  std::vector<float> right = {1.0f, 0.0f, 0.0f, 0.0f, 0.0f};

  PhaseAlign align({2, true});
  align.prepare(48000.0, 5);
  process_stereo(align, left, right);

  REQUIRE_THAT(right[0], WithinAbs(0.0f, 0.000001f));
  REQUIRE_THAT(right[1], WithinAbs(0.0f, 0.000001f));
  REQUIRE_THAT(right[2], WithinAbs(1.0f, 0.000001f));
}

TEST_CASE("PhaseAlign validates configuration", "[mastering][stereo]") {
  REQUIRE_THROWS(PhaseAlign({-1, true}));
}

TEST_CASE("MonoCompatCheck detects anti-phase stereo risk", "[mastering][stereo]") {
  auto left = sine(1000.0f, 48000, 48000, 0.5f);
  auto right = left;
  for (auto& sample : right) {
    sample = -sample;
  }

  const auto result = mono_compat_check(left.data(), right.data(), left.size(), 0.0f);

  REQUIRE(result.correlation < -0.99f);
  REQUIRE(result.width > 100.0f);
  REQUIRE(result.mono_peak < 0.000001f);
  REQUIRE(!result.likely_mono_compatible);
}

TEST_CASE("MonoCompatCheck validates buffers", "[mastering][stereo]") {
  REQUIRE_THROWS(mono_compat_check(nullptr, nullptr, 0));
}
