#include "metering/stereo.h"

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

float stereo_rms_tail(const std::vector<float>& left, const std::vector<float>& right,
                      size_t skip) {
  double sum = 0.0;
  size_t count = 0;
  for (size_t i = std::min(skip, left.size()); i < left.size() && i < right.size(); ++i) {
    sum +=
        0.5 * (static_cast<double>(left[i]) * left[i] + static_cast<double>(right[i]) * right[i]);
    ++count;
  }
  return count == 0 ? 0.0f : static_cast<float>(std::sqrt(sum / static_cast<double>(count)));
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

TEST_CASE("MidSide encode preserves total energy (AES convention)", "[mastering][stereo]") {
  // A pair of decorrelated sines (different frequency + amplitude per channel)
  // exercises both the sum (mid) and difference (side) paths.
  constexpr int kSampleRate = 48000;
  constexpr int kSamples = 4096;
  auto left = sine(440.0f, kSampleRate, kSamples, 0.42f);
  auto right = sine(660.0f, kSampleRate, kSamples, 0.17f);
  // Inject a phase offset on right to create non-trivial side content.
  for (int i = 0; i < kSamples; ++i) {
    right[static_cast<size_t>(i)] +=
        0.11f * static_cast<float>(std::cos(2.0 * kPi * 880.0 * i / kSampleRate));
  }

  std::vector<float> mid(left.size());
  std::vector<float> side(left.size());
  encode_buffer(left.data(), right.data(), mid.data(), side.data(), left.size());

  double lr_energy = 0.0;
  double ms_energy = 0.0;
  for (size_t i = 0; i < left.size(); ++i) {
    lr_energy += static_cast<double>(left[i]) * left[i] + static_cast<double>(right[i]) * right[i];
    ms_energy += static_cast<double>(mid[i]) * mid[i] + static_cast<double>(side[i]) * side[i];
  }

  // Energy preservation: M^2 + S^2 == L^2 + R^2 within float round-off.
  REQUIRE(lr_energy > 0.0);
  const double ratio_db = 10.0 * std::log10(ms_energy / lr_energy);
  REQUIRE(std::abs(ratio_db) < 0.05);  // tighter than 0.1 dB target
}

TEST_CASE("MidSide buffer roundtrip is bit-near-exact", "[mastering][stereo]") {
  // Stress the round-trip with a longer, mixed signal.
  constexpr int kSampleRate = 48000;
  constexpr int kSamples = 2048;
  auto left = sine(523.25f, kSampleRate, kSamples, 0.31f);
  auto right = sine(783.99f, kSampleRate, kSamples, 0.27f);
  const auto original_left = left;
  const auto original_right = right;

  std::vector<float> mid(left.size());
  std::vector<float> side(left.size());
  encode_buffer(left.data(), right.data(), mid.data(), side.data(), left.size());
  decode_buffer(mid.data(), side.data(), left.data(), right.data(), left.size());

  for (size_t i = 0; i < left.size(); ++i) {
    REQUIRE_THAT(left[i], WithinAbs(original_left[i], 1.0e-6f));
    REQUIRE_THAT(right[i], WithinAbs(original_right[i], 1.0e-6f));
  }
}

TEST_CASE("MidSide preserves loudness when side gain is applied", "[mastering][stereo]") {
  // Real-world scenario: encode -> apply +6 dB to side -> decode. With the
  // energy-preserving convention the predicted L/R energy after a +g_side dB
  // boost equals 0.5 * (1 + g_side^2) * (L^2 + R^2) for an uncorrelated pair,
  // and matches the analytic expression exactly for a mid+side decomposition.
  constexpr int kSampleRate = 48000;
  constexpr int kSamples = 4096;
  auto left = sine(330.0f, kSampleRate, kSamples, 0.22f);
  auto right = sine(495.0f, kSampleRate, kSamples, 0.14f);

  std::vector<float> mid(left.size());
  std::vector<float> side(left.size());
  encode_buffer(left.data(), right.data(), mid.data(), side.data(), left.size());

  // Energy in M and S separately before gain.
  double pre_mid = 0.0;
  double pre_side = 0.0;
  for (size_t i = 0; i < mid.size(); ++i) {
    pre_mid += static_cast<double>(mid[i]) * mid[i];
    pre_side += static_cast<double>(side[i]) * side[i];
  }

  // Apply +6 dB to side only (linear gain 2.0).
  constexpr float kSideGain = 2.0f;
  for (size_t i = 0; i < side.size(); ++i) {
    side[i] *= kSideGain;
  }

  std::vector<float> out_left(left.size());
  std::vector<float> out_right(left.size());
  decode_buffer(mid.data(), side.data(), out_left.data(), out_right.data(), left.size());

  double post_energy = 0.0;
  for (size_t i = 0; i < out_left.size(); ++i) {
    post_energy += static_cast<double>(out_left[i]) * out_left[i] +
                   static_cast<double>(out_right[i]) * out_right[i];
  }
  // Predicted energy: pre_mid + kSideGain^2 * pre_side (since decode is
  // unitary up to round-off and total energy equals M^2 + S^2).
  const double predicted =
      pre_mid + static_cast<double>(kSideGain) * static_cast<double>(kSideGain) * pre_side;
  REQUIRE(predicted > 0.0);
  const double err_db = 10.0 * std::log10(post_energy / predicted);
  REQUIRE(std::abs(err_db) < 0.05);  // within 0.05 dB
}

TEST_CASE("Imager increases and collapses stereo width", "[mastering][stereo]") {
  auto mid = sine(1000.0f, 48000, 48000, 0.25f);
  auto side = sine(1500.0f, 48000, 48000, 0.05f);
  auto left = mid;
  auto right = mid;
  for (size_t i = 0; i < left.size(); ++i) {
    left[i] += side[i];
    right[i] -= side[i];
  }
  const float side_before = rms_tail(side_signal(left, right), 4096);
  const float energy_before = stereo_rms_tail(left, right, 4096);

  Imager wide({2.0f, 0.0f});
  wide.prepare(48000.0, 1024);
  process_stereo(wide, left, right);
  REQUIRE(rms_tail(side_signal(left, right), 4096) > side_before * 1.7f);
  REQUIRE(stereo_rms_tail(left, right, 4096) > energy_before * 0.98f);
  REQUIRE(stereo_rms_tail(left, right, 4096) < energy_before * 1.02f);

  Imager mono({0.0f, 0.0f});
  mono.prepare(48000.0, 1024);
  process_stereo(mono, left, right);
  REQUIRE(rms_tail(side_signal(left, right), 4096) < 0.000001f);
}

TEST_CASE("Imager decorrelates widened side signal", "[mastering][stereo]") {
  auto left = sine(1000.0f, 48000, 48000, 0.25f);
  auto right = sine(1000.0f, 48000, 48000, -0.25f);

  Imager dry({1.5f, 0.0f, 0.0f, false});
  Imager decorated({1.5f, 0.0f, 1.0f, false});
  dry.prepare(48000.0, 1024);
  decorated.prepare(48000.0, 1024);

  auto dry_left = left;
  auto dry_right = right;
  process_stereo(dry, dry_left, dry_right);
  process_stereo(decorated, left, right);

  REQUIRE(rms_tail(side_signal(left, right), 4096) >
          rms_tail(side_signal(dry_left, dry_right), 4096) * 0.85f);
  REQUIRE(std::abs(left[4096] - dry_left[4096]) > 0.001f);
}

TEST_CASE("Imager validates width", "[mastering][stereo]") {
  REQUIRE_THROWS(Imager({-1.0f, 0.0f}));
  REQUIRE_THROWS(Imager({1.0f, 0.0f, 1.1f}));
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

TEST_CASE("PhaseAlign supports fractional sample delay", "[mastering][stereo]") {
  std::vector<float> left(8, 0.0f);
  std::vector<float> right = {1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f};

  PhaseAlign align({2, true, 0.5f});
  align.prepare(48000.0, 8);
  process_stereo(align, left, right);

  REQUIRE(std::abs(right[2]) > 0.1f);
  REQUIRE(std::abs(right[3]) > std::abs(right[2]));
  REQUIRE(std::abs(right[6]) < 0.1f);
}

TEST_CASE("PhaseAlign estimates relative delay by cross-correlation", "[mastering][stereo]") {
  std::vector<float> reference(256, 0.0f);
  std::vector<float> delayed(256, 0.0f);
  for (size_t i = 0; i < reference.size(); ++i) {
    reference[i] = static_cast<float>(std::sin(0.17 * static_cast<double>(i)) +
                                      0.3 * std::sin(0.031 * static_cast<double>(i * i)));
  }
  for (size_t i = 0; i + 3 < delayed.size(); ++i) {
    delayed[i + 3] = reference[i];
  }

  const float estimate = PhaseAlign::estimate_delay_samples(reference.data(), delayed.data(),
                                                            static_cast<int>(reference.size()), 12);

  REQUIRE_THAT(estimate, WithinAbs(3.0f, 0.1f));
}

TEST_CASE("PhaseAlign validates configuration", "[mastering][stereo]") {
  REQUIRE_THROWS(PhaseAlign({-1, true}));
  REQUIRE_THROWS(PhaseAlign({0, true, -0.1f}));
  REQUIRE_THROWS(PhaseAlign({0, true, 1.0f}));
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

TEST_CASE("MonoCompatCheck reports log-band phase correlation", "[mastering][stereo]") {
  auto left = sine(1000.0f, 48000, 48000, 0.5f);
  auto right = left;
  for (auto& sample : right) {
    sample = -sample;
  }

  const auto bands = mono_compat_check_log_bands(left.data(), right.data(), left.size(), 48000.0, 1,
                                                 700.0f, 1400.0f);

  REQUIRE(bands.size() == 1);
  REQUIRE(bands[0].correlation < -0.99f);
  REQUIRE(bands[0].side_rms > 0.3f);
}

TEST_CASE("MonoCompatCheck validates buffers", "[mastering][stereo]") {
  REQUIRE_THROWS(mono_compat_check(nullptr, nullptr, 0));
  std::vector<float> left(4, 0.0f);
  std::vector<float> right(4, 0.0f);
  REQUIRE_THROWS(mono_compat_check_log_bands(left.data(), right.data(), left.size(), 48000.0, 0));
}
