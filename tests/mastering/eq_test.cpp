#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <cmath>
#include <complex>
#include <vector>

#include "mastering/common/biquad_design.h"
#include "mastering/eq/api_style.h"
#include "mastering/eq/band_pass.h"
#include "mastering/eq/cut_filter.h"
#include "mastering/eq/dynamic_eq.h"
#include "mastering/eq/graphic_eq.h"
#include "mastering/eq/linear_phase.h"
#include "mastering/eq/mid_side_eq.h"
#include "mastering/eq/minimum_phase.h"
#include "mastering/eq/parametric.h"
#include "mastering/eq/pultec.h"
#include "mastering/eq/shelving.h"
#include "mastering/eq/tilt.h"

using Catch::Matchers::WithinAbs;
using namespace sonare::mastering::eq;

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

float peak_abs(const std::vector<float>& samples) {
  float peak = 0.0f;
  for (float sample : samples) peak = std::max(peak, std::abs(sample));
  return peak;
}

float kernel_magnitude_at(const std::vector<float>& kernel, double frequency_hz,
                          double sample_rate) {
  std::complex<double> response{0.0, 0.0};
  const double omega = 2.0 * kPi * frequency_hz / sample_rate;
  for (size_t n = 0; n < kernel.size(); ++n) {
    response += static_cast<double>(kernel[n]) *
                std::exp(std::complex<double>(0.0, -omega * static_cast<double>(n)));
  }
  return static_cast<float>(std::abs(response));
}

void process(sonare::mastering::common::ProcessorBase& processor, std::vector<float>& mono) {
  float* channels[] = {mono.data()};
  processor.process(channels, 1, static_cast<int>(mono.size()));
}

void process_stereo(sonare::mastering::common::ProcessorBase& processor, std::vector<float>& left,
                    std::vector<float>& right) {
  float* channels[] = {left.data(), right.data()};
  processor.process(channels, 2, static_cast<int>(std::min(left.size(), right.size())));
}

}  // namespace

TEST_CASE("ParametricEq with no enabled bands preserves audio", "[mastering][eq]") {
  ParametricEq eq;
  eq.prepare(48000.0, 512);

  auto audio = sine(1000.0f, 48000, 4096);
  const auto original = audio;

  process(eq, audio);

  REQUIRE(audio.size() == original.size());
  for (size_t i = 0; i < audio.size(); ++i) {
    REQUIRE_THAT(audio[i], WithinAbs(original[i], 0.000001f));
  }
}

TEST_CASE("ParametricEq peak band boosts its center frequency", "[mastering][eq]") {
  constexpr int sample_rate = 48000;
  ParametricEq eq;
  eq.prepare(sample_rate, 1024);
  eq.set_band(0, {EqBandType::Peak, 1000.0f, 6.0f, 1.0f, true});

  auto center = sine(1000.0f, sample_rate, sample_rate);
  auto off_center = sine(4000.0f, sample_rate, sample_rate);
  const float center_before = rms_tail(center, 4096);
  const float off_before = rms_tail(off_center, 4096);

  process(eq, center);
  eq.reset();
  process(eq, off_center);

  const float center_gain = rms_tail(center, 4096) / center_before;
  const float off_gain = rms_tail(off_center, 4096) / off_before;

  REQUIRE(center_gain > 1.85f);
  REQUIRE(off_gain < center_gain * 0.75f);
}

TEST_CASE("ParametricEq high-pass attenuates low frequencies", "[mastering][eq]") {
  constexpr int sample_rate = 48000;
  ParametricEq eq;
  eq.prepare(sample_rate, 1024);
  eq.set_band(0, {EqBandType::HighPass, 500.0f, 0.0f, 0.70710678f, true});

  auto low = sine(100.0f, sample_rate, sample_rate);
  auto high = sine(2000.0f, sample_rate, sample_rate);
  const float low_before = rms_tail(low, 4096);
  const float high_before = rms_tail(high, 4096);

  process(eq, low);
  eq.reset();
  process(eq, high);

  const float low_gain = rms_tail(low, 4096) / low_before;
  const float high_gain = rms_tail(high, 4096) / high_before;

  REQUIRE(low_gain < 0.08f);
  REQUIRE(high_gain > 0.9f);
}

TEST_CASE("ParametricEq Vicanek mode filters common band types", "[mastering][eq]") {
  constexpr int sample_rate = 48000;

  ParametricEq lowpass;
  lowpass.prepare(sample_rate, 1024);
  lowpass.set_band(
      0, {EqBandType::LowPass, 1000.0f, 0.0f, 0.70710678f, true, BiquadCoeffMode::Vicanek});
  auto low = sine(200.0f, sample_rate, sample_rate);
  auto high = sine(8000.0f, sample_rate, sample_rate);
  const float low_before = rms_tail(low, 4096);
  const float high_before = rms_tail(high, 4096);
  process(lowpass, low);
  lowpass.reset();
  process(lowpass, high);
  REQUIRE(rms_tail(low, 4096) / low_before > 0.75f);
  REQUIRE(rms_tail(high, 4096) / high_before < 0.35f);

  ParametricEq highpass;
  highpass.prepare(sample_rate, 1024);
  highpass.set_band(
      0, {EqBandType::HighPass, 1000.0f, 0.0f, 0.70710678f, true, BiquadCoeffMode::Vicanek});
  low = sine(200.0f, sample_rate, sample_rate);
  high = sine(8000.0f, sample_rate, sample_rate);
  process(highpass, low);
  highpass.reset();
  process(highpass, high);
  REQUIRE(rms_tail(low, 4096) / low_before < 0.35f);
  REQUIRE(rms_tail(high, 4096) / high_before > 0.75f);
}

TEST_CASE("ParametricEq Vicanek peak boosts its center frequency", "[mastering][eq]") {
  constexpr int sample_rate = 48000;
  ParametricEq eq;
  eq.prepare(sample_rate, 1024);
  eq.set_band(0, {EqBandType::Peak, 6000.0f, 6.0f, 1.0f, true, BiquadCoeffMode::Vicanek});

  auto center = sine(6000.0f, sample_rate, sample_rate);
  auto off_center = sine(500.0f, sample_rate, sample_rate);
  const float center_before = rms_tail(center, 4096);
  const float off_before = rms_tail(off_center, 4096);

  process(eq, center);
  eq.reset();
  process(eq, off_center);

  REQUIRE(rms_tail(center, 4096) / center_before > 1.5f);
  REQUIRE(rms_tail(off_center, 4096) / off_before < 1.2f);
}

TEST_CASE("ParametricEq Vicanek shelves match low and high shelf intent", "[mastering][eq]") {
  constexpr int sample_rate = 48000;

  ParametricEq low_shelf;
  low_shelf.prepare(sample_rate, 1024);
  low_shelf.set_band(
      0, {EqBandType::LowShelf, 1000.0f, 6.0f, 0.70710678f, true, BiquadCoeffMode::Vicanek});
  auto low = sine(100.0f, sample_rate, sample_rate);
  auto high = sine(8000.0f, sample_rate, sample_rate);
  const float low_before = rms_tail(low, 4096);
  const float high_before = rms_tail(high, 4096);
  process(low_shelf, low);
  low_shelf.reset();
  process(low_shelf, high);
  REQUIRE(rms_tail(low, 4096) / low_before > 1.7f);
  REQUIRE(rms_tail(high, 4096) / high_before < 1.15f);

  ParametricEq high_shelf;
  high_shelf.prepare(sample_rate, 1024);
  high_shelf.set_band(
      0, {EqBandType::HighShelf, 6000.0f, 6.0f, 0.70710678f, true, BiquadCoeffMode::Vicanek});
  low = sine(100.0f, sample_rate, sample_rate);
  high = sine(12000.0f, sample_rate, sample_rate);
  const float low2_before = rms_tail(low, 4096);
  const float high2_before = rms_tail(high, 4096);
  process(high_shelf, low);
  high_shelf.reset();
  process(high_shelf, high);
  REQUIRE(rms_tail(low, 4096) / low2_before < 1.15f);
  REQUIRE(rms_tail(high, 4096) / high2_before > 1.6f);
}

TEST_CASE("ParametricEq disabled band is bypassed", "[mastering][eq]") {
  ParametricEq eq;
  eq.prepare(48000.0, 512);
  eq.set_band(0, {EqBandType::Peak, 1000.0f, 12.0f, 1.0f, false});

  auto audio = sine(1000.0f, 48000, 4096);
  const auto original = audio;

  process(eq, audio);

  for (size_t i = 0; i < audio.size(); ++i) {
    REQUIRE_THAT(audio[i], WithinAbs(original[i], 0.000001f));
  }
}

TEST_CASE("ParametricEq validates band configuration", "[mastering][eq]") {
  ParametricEq eq;
  eq.prepare(48000.0, 512);

  REQUIRE_THROWS(eq.set_band(ParametricEq::kMaxBands, {}));
  REQUIRE_THROWS(eq.set_band(0, {EqBandType::Peak, 0.0f, 0.0f, 1.0f, true}));
  REQUIRE_THROWS(eq.set_band(0, {EqBandType::Peak, 24000.0f, 0.0f, 1.0f, true}));
  REQUIRE_THROWS(eq.set_band(0, {EqBandType::Peak, 1000.0f, 0.0f, 0.0f, true}));
}

TEST_CASE("TiltEq positive tilt brightens highs relative to lows", "[mastering][eq]") {
  constexpr int sample_rate = 48000;
  TiltEq eq;
  eq.prepare(sample_rate, 1024);
  eq.set_tilt_db(6.0f);
  eq.set_pivot_hz(1000.0f);

  auto low = sine(100.0f, sample_rate, sample_rate);
  auto high = sine(8000.0f, sample_rate, sample_rate);
  const float low_before = rms_tail(low, 4096);
  const float high_before = rms_tail(high, 4096);

  process(eq, low);
  eq.reset();
  process(eq, high);

  const float low_gain = rms_tail(low, 4096) / low_before;
  const float high_gain = rms_tail(high, 4096) / high_before;

  REQUIRE(low_gain < 0.9f);
  REQUIRE(high_gain > 1.1f);
  REQUIRE(high_gain > low_gain * 1.5f);
}

TEST_CASE("TiltEq zero tilt is bypassed", "[mastering][eq]") {
  TiltEq eq;
  eq.prepare(48000.0, 512);
  eq.set_tilt_db(0.0f);

  auto audio = sine(2000.0f, 48000, 4096);
  const auto original = audio;

  process(eq, audio);

  for (size_t i = 0; i < audio.size(); ++i) {
    REQUIRE_THAT(audio[i], WithinAbs(original[i], 0.000001f));
  }
}

TEST_CASE("LinearPhaseEq exposes symmetric FIR kernel and latency", "[mastering][eq]") {
  LinearPhaseEq eq({1024, 257});
  eq.prepare(48000.0, 512);

  REQUIRE(eq.latency_samples() == 128);
  REQUIRE(eq.kernel().size() == 257);
  for (size_t i = 0; i < eq.kernel().size() / 2; ++i) {
    REQUIRE_THAT(eq.kernel()[i], WithinAbs(eq.kernel()[eq.kernel().size() - 1 - i], 0.000001f));
  }
}

TEST_CASE("LinearPhaseEq flat response is delayed but otherwise transparent", "[mastering][eq]") {
  LinearPhaseEq eq({1024, 129});
  eq.prepare(48000.0, 512);

  std::vector<float> impulse(256, 0.0f);
  impulse[0] = 1.0f;

  process(eq, impulse);

  for (int i = 0; i < eq.latency_samples(); ++i) {
    REQUIRE_THAT(impulse[static_cast<size_t>(i)], WithinAbs(0.0f, 0.000001f));
  }
  REQUIRE_THAT(impulse[static_cast<size_t>(eq.latency_samples())], WithinAbs(1.0f, 0.000001f));
}

TEST_CASE("LinearPhaseEq high-pass attenuates lows while preserving highs", "[mastering][eq]") {
  constexpr int sample_rate = 48000;
  LinearPhaseEq eq({2048, 513});
  eq.prepare(sample_rate, 1024);
  eq.set_band(0, {EqBandType::HighPass, 500.0f, 0.0f, 0.70710678f, true});

  auto low = sine(100.0f, sample_rate, sample_rate);
  auto high = sine(4000.0f, sample_rate, sample_rate);
  const float low_before = rms_tail(low, 8192);
  const float high_before = rms_tail(high, 8192);

  process(eq, low);
  eq.reset();
  process(eq, high);

  const float low_gain = rms_tail(low, 8192) / low_before;
  const float high_gain = rms_tail(high, 8192) / high_before;

  REQUIRE(low_gain < 0.35f);
  REQUIRE(high_gain > 0.85f);
}

TEST_CASE("LinearPhaseEq kernel follows RBJ biquad magnitude", "[mastering][eq]") {
  constexpr int sample_rate = 48000;
  LinearPhaseEq eq({8192, 2049, false});
  eq.prepare(sample_rate, 1024);
  const EqBand band{EqBandType::Peak, 1000.0f, 6.0f, 0.70710678f, true};
  eq.set_band(0, band);

  const auto coeffs = sonare::mastering::common::rbj_peak(
      static_cast<float>(2.0 * kPi * band.frequency_hz / sample_rate), band.q, band.gain_db);

  for (double frequency_hz : {250.0, 1000.0, 4000.0}) {
    const float expected = sonare::mastering::common::biquad_magnitude(
        coeffs, static_cast<float>(2.0 * kPi * frequency_hz / sample_rate));
    const float actual = kernel_magnitude_at(eq.kernel(), frequency_hz, sample_rate);
    INFO("frequency: " << frequency_hz);
    REQUIRE_THAT(actual, WithinAbs(expected, 0.08f));
  }
}

TEST_CASE("LinearPhaseEq partitioned convolution matches direct convolution", "[mastering][eq]") {
  LinearPhaseEq direct({1024, 257, false});
  LinearPhaseEq partitioned({1024, 257, true, 128});
  direct.prepare(48000.0, 128);
  partitioned.prepare(48000.0, 128);

  const EqBand band{EqBandType::Peak, 2500.0f, 4.0f, 1.2f, true};
  direct.set_band(0, band);
  partitioned.set_band(0, band);

  auto direct_signal = sine(700.0f, 48000, 1024, 0.25f);
  auto partitioned_signal = direct_signal;
  process(direct, direct_signal);
  process(partitioned, partitioned_signal);

  for (size_t i = 0; i < direct_signal.size(); ++i) {
    REQUIRE_THAT(partitioned_signal[i], WithinAbs(direct_signal[i], 0.00001f));
  }
}

TEST_CASE("LinearPhaseEq validates configuration and bands", "[mastering][eq]") {
  REQUIRE_THROWS(LinearPhaseEq({1000, 257}));
  REQUIRE_THROWS(LinearPhaseEq({1024, 258}));
  REQUIRE_THROWS(LinearPhaseEq({1024, 2049}));

  LinearPhaseEq eq({1024, 257});
  eq.prepare(48000.0, 512);
  REQUIRE_THROWS(eq.set_band(LinearPhaseEq::kMaxBands, {}));
  REQUIRE_THROWS(eq.set_band(0, {EqBandType::Peak, 0.0f, 0.0f, 1.0f, true}));
  REQUIRE_THROWS(eq.set_band(0, {EqBandType::Peak, 1000.0f, 0.0f, 0.0f, true}));
}

TEST_CASE("MinimumPhaseEq reports zero latency and processes immediately", "[mastering][eq]") {
  MinimumPhaseEq eq;
  eq.prepare(48000.0, 512);

  REQUIRE(eq.latency_samples() == 0);

  std::vector<float> impulse(32, 0.0f);
  impulse[0] = 1.0f;

  process(eq, impulse);

  REQUIRE_THAT(impulse[0], WithinAbs(1.0f, 0.000001f));
}

TEST_CASE("MinimumPhaseEq high-pass attenuates low frequencies", "[mastering][eq]") {
  constexpr int sample_rate = 48000;
  MinimumPhaseEq eq;
  eq.prepare(sample_rate, 1024);
  eq.set_band(0, {EqBandType::HighPass, 500.0f, 0.0f, 0.70710678f, true});

  auto low = sine(100.0f, sample_rate, sample_rate);
  auto high = sine(2000.0f, sample_rate, sample_rate);
  const float low_before = rms_tail(low, 4096);
  const float high_before = rms_tail(high, 4096);

  process(eq, low);
  eq.reset();
  process(eq, high);

  const float low_gain = rms_tail(low, 4096) / low_before;
  const float high_gain = rms_tail(high, 4096) / high_before;

  REQUIRE(low_gain < 0.08f);
  REQUIRE(high_gain > 0.9f);
}

TEST_CASE("MinimumPhaseEq validates bands through its own API", "[mastering][eq]") {
  MinimumPhaseEq eq;
  eq.prepare(48000.0, 512);

  REQUIRE_THROWS(eq.set_band(MinimumPhaseEq::kMaxBands, {}));
  REQUIRE_THROWS(eq.set_band(0, {EqBandType::Peak, 0.0f, 0.0f, 1.0f, true}));
  REQUIRE_THROWS(eq.set_band(0, {EqBandType::Peak, 1000.0f, 0.0f, 0.0f, true}));
}

TEST_CASE("ShelvingEq boosts low and high shelves independently", "[mastering][eq]") {
  constexpr int sample_rate = 48000;
  ShelvingEq eq;
  eq.prepare(sample_rate, 1024);
  eq.set_low_shelf(250.0f, 6.0f);

  auto low = sine(100.0f, sample_rate, sample_rate);
  auto high = sine(8000.0f, sample_rate, sample_rate);
  const float low_before = rms_tail(low, 4096);
  const float high_before = rms_tail(high, 4096);

  process(eq, low);
  eq.reset();
  process(eq, high);

  const float low_gain = rms_tail(low, 4096) / low_before;
  const float high_gain = rms_tail(high, 4096) / high_before;

  REQUIRE(low_gain > 1.7f);
  REQUIRE(high_gain < 1.1f);

  eq.clear();
  eq.set_high_shelf(4000.0f, 6.0f);
  low = sine(100.0f, sample_rate, sample_rate);
  high = sine(8000.0f, sample_rate, sample_rate);

  process(eq, low);
  eq.reset();
  process(eq, high);

  REQUIRE(rms_tail(low, 4096) / low_before < 1.1f);
  REQUIRE(rms_tail(high, 4096) / high_before > 1.7f);
}

TEST_CASE("CutFilter high-pass slope controls attenuation depth", "[mastering][eq]") {
  constexpr int sample_rate = 48000;
  auto low = sine(100.0f, sample_rate, sample_rate);
  auto high = sine(4000.0f, sample_rate, sample_rate);
  const float low_before = rms_tail(low, 4096);
  const float high_before = rms_tail(high, 4096);

  CutFilter gentle;
  gentle.prepare(sample_rate, 1024);
  gentle.set_high_pass(500.0f, 0.70710678f, CutFilterSlope::Db12PerOct);
  process(gentle, low);

  CutFilter steep;
  steep.prepare(sample_rate, 1024);
  steep.set_high_pass(500.0f, 0.70710678f, CutFilterSlope::Db24PerOct);
  auto steep_low = sine(100.0f, sample_rate, sample_rate);
  process(steep, steep_low);

  CutFilter high_pass;
  high_pass.prepare(sample_rate, 1024);
  high_pass.set_high_pass(500.0f, 0.70710678f, CutFilterSlope::Db24PerOct);
  process(high_pass, high);

  const float gentle_low_gain = rms_tail(low, 4096) / low_before;
  const float steep_low_gain = rms_tail(steep_low, 4096) / low_before;
  const float high_gain = rms_tail(high, 4096) / high_before;

  REQUIRE(steep_low_gain < gentle_low_gain * 0.25f);
  REQUIRE(steep_low_gain < 0.01f);
  REQUIRE(high_gain > 0.9f);
}

TEST_CASE("CutFilter low-pass attenuates high frequencies", "[mastering][eq]") {
  constexpr int sample_rate = 48000;
  CutFilter eq;
  eq.prepare(sample_rate, 1024);
  eq.set_low_pass(1000.0f, 0.70710678f, CutFilterSlope::Db24PerOct);

  auto low = sine(100.0f, sample_rate, sample_rate);
  auto high = sine(8000.0f, sample_rate, sample_rate);
  const float low_before = rms_tail(low, 4096);
  const float high_before = rms_tail(high, 4096);

  process(eq, low);
  eq.reset();
  process(eq, high);

  REQUIRE(rms_tail(low, 4096) / low_before > 0.95f);
  REQUIRE(rms_tail(high, 4096) / high_before < 0.02f);
}

TEST_CASE("BandPassEq passes center frequency and rejects off-band tones", "[mastering][eq]") {
  constexpr int sample_rate = 48000;
  BandPassEq eq;
  eq.prepare(sample_rate, 1024);
  eq.set_band_pass(1000.0f, 4.0f);

  auto center = sine(1000.0f, sample_rate, sample_rate);
  auto low = sine(100.0f, sample_rate, sample_rate);
  auto high = sine(8000.0f, sample_rate, sample_rate);
  const float center_before = rms_tail(center, 4096);
  const float low_before = rms_tail(low, 4096);
  const float high_before = rms_tail(high, 4096);

  process(eq, center);
  eq.reset();
  process(eq, low);
  eq.reset();
  process(eq, high);

  REQUIRE(rms_tail(center, 4096) / center_before > 0.85f);
  REQUIRE(rms_tail(low, 4096) / low_before < 0.2f);
  REQUIRE(rms_tail(high, 4096) / high_before < 0.2f);
}

TEST_CASE("BandPassEq notch rejects center frequency", "[mastering][eq]") {
  constexpr int sample_rate = 48000;
  BandPassEq eq;
  eq.prepare(sample_rate, 1024);
  eq.set_notch(1000.0f, 8.0f);

  auto center = sine(1000.0f, sample_rate, sample_rate);
  auto high = sine(8000.0f, sample_rate, sample_rate);
  const float center_before = rms_tail(center, 4096);
  const float high_before = rms_tail(high, 4096);

  process(eq, center);
  eq.reset();
  process(eq, high);

  REQUIRE(rms_tail(center, 4096) / center_before < 0.1f);
  REQUIRE(rms_tail(high, 4096) / high_before > 0.9f);
}

TEST_CASE("MidSideEq mid band affects mono-compatible content", "[mastering][eq]") {
  constexpr int sample_rate = 48000;
  MidSideEq eq;
  eq.prepare(sample_rate, 1024);
  eq.set_mid_band(0, {EqBandType::Peak, 1000.0f, 6.0f, 1.0f, true});

  auto left = sine(1000.0f, sample_rate, sample_rate);
  auto right = left;
  const float before = rms_tail(left, 4096);

  process_stereo(eq, left, right);

  const float left_gain = rms_tail(left, 4096) / before;
  const float right_gain = rms_tail(right, 4096) / before;
  REQUIRE(left_gain > 1.8f);
  REQUIRE_THAT(left_gain, WithinAbs(right_gain, 0.0001f));
}

TEST_CASE("MidSideEq side band affects side-only content", "[mastering][eq]") {
  constexpr int sample_rate = 48000;
  MidSideEq eq;
  eq.prepare(sample_rate, 1024);
  eq.set_side_band(0, {EqBandType::Peak, 1000.0f, 6.0f, 1.0f, true});

  auto left = sine(1000.0f, sample_rate, sample_rate);
  auto right = left;
  for (auto& sample : right) {
    sample = -sample;
  }
  const float before = rms_tail(left, 4096);

  process_stereo(eq, left, right);

  const float left_gain = rms_tail(left, 4096) / before;
  const float right_gain = rms_tail(right, 4096) / before;
  REQUIRE(left_gain > 1.8f);
  REQUIRE_THAT(left_gain, WithinAbs(right_gain, 0.0001f));
  REQUIRE_THAT(left[5000], WithinAbs(-right[5000], 0.0001f));
}

TEST_CASE("MidSideEq requires stereo input", "[mastering][eq]") {
  MidSideEq eq;
  eq.prepare(48000.0, 512);

  auto mono = sine(1000.0f, 48000, 1024);
  float* channels[] = {mono.data()};
  REQUIRE_THROWS(eq.process(channels, 1, static_cast<int>(mono.size())));
}

TEST_CASE("GraphicEq exposes 31 bands and nearest band lookup", "[mastering][eq]") {
  GraphicEq eq;
  eq.prepare(48000.0, 512);

  REQUIRE(GraphicEq::kNumBands == 31);
  REQUIRE_THAT(eq.center_frequency(0), WithinAbs(20.0f, 0.0001f));
  REQUIRE_THAT(eq.center_frequency(17), WithinAbs(1000.0f, 0.0001f));
  REQUIRE_THAT(eq.center_frequency(30), WithinAbs(20000.0f, 0.0001f));
  REQUIRE(eq.nearest_band(990.0f) == 17);
  REQUIRE(GraphicEq::band_q_for_gain_db(12.0f) > GraphicEq::band_q_for_gain_db(3.0f));
  REQUIRE_THAT(GraphicEq::band_q_for_gain_db(12.0f),
               WithinAbs(GraphicEq::band_q_for_gain_db(-12.0f), 0.0001f));
  REQUIRE_THROWS(eq.center_frequency(31));
  REQUIRE_THROWS(eq.nearest_band(0.0f));
}

TEST_CASE("GraphicEq boosts selected band more than distant bands", "[mastering][eq]") {
  constexpr int sample_rate = 48000;
  GraphicEq eq;
  eq.prepare(sample_rate, 1024);
  eq.set_gain_for_frequency(1000.0f, 6.0f);

  auto center = sine(1000.0f, sample_rate, sample_rate);
  auto distant = sine(8000.0f, sample_rate, sample_rate);
  const float center_before = rms_tail(center, 4096);
  const float distant_before = rms_tail(distant, 4096);

  process(eq, center);
  eq.reset();
  process(eq, distant);

  const float center_gain = rms_tail(center, 4096) / center_before;
  const float distant_gain = rms_tail(distant, 4096) / distant_before;

  REQUIRE(center_gain > 1.8f);
  REQUIRE(distant_gain < 1.15f);
}

TEST_CASE("PultecEq low boost and attenuation shape low end", "[mastering][eq]") {
  constexpr int sample_rate = 48000;
  PultecEq eq;
  eq.prepare(sample_rate, 1024);
  eq.set_low_frequency(60.0f);
  eq.set_low_boost(5.0f);
  eq.set_low_attenuation(4.0f);

  auto sub = sine(50.0f, sample_rate, sample_rate);
  auto low_mid = sine(160.0f, sample_rate, sample_rate);
  auto high = sine(5000.0f, sample_rate, sample_rate);
  const float sub_before = rms_tail(sub, 4096);
  const float low_mid_before = rms_tail(low_mid, 4096);
  const float high_before = rms_tail(high, 4096);

  process(eq, sub);
  eq.reset();
  process(eq, low_mid);
  eq.reset();
  process(eq, high);

  REQUIRE(rms_tail(sub, 4096) / sub_before > 1.25f);
  REQUIRE(rms_tail(low_mid, 4096) / low_mid_before < 0.9f);
  REQUIRE(rms_tail(high, 4096) / high_before < 1.1f);
}

TEST_CASE("PultecEq high boost and attenuation shape top end", "[mastering][eq]") {
  constexpr int sample_rate = 48000;
  PultecEq eq;
  eq.prepare(sample_rate, 1024);
  eq.set_high_boost(8000.0f, 5.0f, 0.8f);
  eq.set_high_attenuation(12000.0f, 4.0f);

  auto presence = sine(8000.0f, sample_rate, sample_rate);
  auto air = sine(16000.0f, sample_rate, sample_rate);
  const float presence_before = rms_tail(presence, 4096);
  const float air_before = rms_tail(air, 4096);

  process(eq, presence);
  eq.reset();
  process(eq, air);

  REQUIRE(rms_tail(presence, 4096) / presence_before > 1.3f);
  REQUIRE(rms_tail(air, 4096) / air_before < 1.0f);
}

TEST_CASE("PultecEq clear bypasses processing", "[mastering][eq]") {
  PultecEq eq;
  eq.prepare(48000.0, 512);
  eq.set_low_boost(8.0f);
  eq.set_high_boost(8000.0f, 8.0f);
  eq.clear();

  auto audio = sine(1000.0f, 48000, 4096);
  const auto original = audio;

  process(eq, audio);

  for (size_t i = 0; i < audio.size(); ++i) {
    REQUIRE_THAT(audio[i], WithinAbs(original[i], 0.000001f));
  }
}

TEST_CASE("PultecEq component model adds passive loss and output nonlinearity", "[mastering][eq]") {
  PultecEq curve;
  PultecEq component;
  curve.prepare(48000.0, 512);
  component.prepare(48000.0, 512);
  curve.set_low_boost(5.0f);
  component.set_low_boost(5.0f);
  component.set_component_model(PultecComponentModel::Eqp1aWdf);
  component.set_output_drive(6.0f);

  auto curve_audio = sine(60.0f, 48000, 4096, 0.8f);
  auto component_audio = curve_audio;
  process(curve, curve_audio);
  process(component, component_audio);

  REQUIRE(rms_tail(component_audio, 512) < rms_tail(curve_audio, 512));
  REQUIRE(peak_abs(component_audio) <= 1.1f);
  REQUIRE(component.component_model() == PultecComponentModel::Eqp1aWdf);
}

TEST_CASE("ApiStyleEq snaps frequency and gain to stepped controls", "[mastering][eq]") {
  ApiStyleEq eq;
  eq.prepare(48000.0, 512);

  eq.set_band(ApiStyleEq::Band::LowMid, 520.0f, 5.1f);

  REQUIRE_THAT(eq.frequency(ApiStyleEq::Band::LowMid), WithinAbs(500.0f, 0.0001f));
  REQUIRE_THAT(eq.gain_db(ApiStyleEq::Band::LowMid), WithinAbs(6.0f, 0.0001f));
  REQUIRE_THAT(eq.snapped_frequency(ApiStyleEq::Band::High, 11000.0f),
               WithinAbs(10000.0f, 0.0001f));
  REQUIRE_THAT(eq.snapped_gain(-5.2f), WithinAbs(-6.0f, 0.0001f));
}

TEST_CASE("ApiStyleEq boosts selected band more than distant bands", "[mastering][eq]") {
  constexpr int sample_rate = 48000;
  ApiStyleEq eq;
  eq.prepare(sample_rate, 1024);
  eq.set_band(ApiStyleEq::Band::LowMid, 500.0f, 6.0f);

  auto center = sine(500.0f, sample_rate, sample_rate);
  auto distant = sine(8000.0f, sample_rate, sample_rate);
  const float center_before = rms_tail(center, 4096);
  const float distant_before = rms_tail(distant, 4096);

  process(eq, center);
  eq.reset();
  process(eq, distant);

  REQUIRE(rms_tail(center, 4096) / center_before > 1.8f);
  REQUIRE(rms_tail(distant, 4096) / distant_before < 1.15f);
}

TEST_CASE("DynamicEq applies cut only above threshold", "[mastering][eq]") {
  constexpr int sample_rate = 48000;
  DynamicEq eq;
  eq.prepare(sample_rate, 1024);
  eq.set_band(0, {EqBandType::Peak, 1000.0f, 0.0f, 2.0f, -20.0f, 4.0f, -9.0f, true});

  auto quiet = sine(1000.0f, sample_rate, sample_rate, 0.02f);
  auto loud = sine(1000.0f, sample_rate, sample_rate, 0.5f);
  const float quiet_before = rms_tail(quiet, 4096);
  const float loud_before = rms_tail(loud, 4096);

  process(eq, quiet);
  const float quiet_gain_db = eq.last_applied_gain_db(0);
  eq.reset();
  process(eq, loud);
  const float loud_gain_db = eq.last_applied_gain_db(0);

  REQUIRE(rms_tail(quiet, 4096) / quiet_before > 0.95f);
  REQUIRE(rms_tail(loud, 4096) / loud_before < 0.55f);
  REQUIRE_THAT(quiet_gain_db, WithinAbs(0.0f, 0.001f));
  REQUIRE(loud_gain_db < -6.0f);
}

TEST_CASE("DynamicEq supports upward dynamic boost", "[mastering][eq]") {
  constexpr int sample_rate = 48000;
  DynamicEq eq;
  eq.prepare(sample_rate, 1024);
  eq.set_band(0, {EqBandType::Peak, 1000.0f, 0.0f, 2.0f, -24.0f, 3.0f, 6.0f, true});

  auto loud = sine(1000.0f, sample_rate, sample_rate, 0.5f);
  const float before = rms_tail(loud, 4096);

  process(eq, loud);

  REQUIRE(eq.last_applied_gain_db(0) > 4.0f);
  REQUIRE(rms_tail(loud, 4096) / before > 1.45f);
}

TEST_CASE("DynamicEq detects each band from its own frequency region", "[mastering][eq]") {
  constexpr int sample_rate = 48000;
  DynamicEq eq;
  eq.prepare(sample_rate, 1024);
  eq.set_band(0, {EqBandType::Peak, 1000.0f, 0.0f, 2.0f, -24.0f, 4.0f, -9.0f, true});
  eq.set_band(1, {EqBandType::Peak, 8000.0f, 0.0f, 2.0f, -24.0f, 4.0f, -9.0f, true});

  auto low = sine(1000.0f, sample_rate, sample_rate, 0.5f);
  process(eq, low);

  REQUIRE(eq.last_band_detector_db(0) > -10.0f);
  REQUIRE(eq.last_band_detector_db(1) < -40.0f);
  REQUIRE(eq.last_applied_gain_db(0) < -6.0f);
  REQUIRE_THAT(eq.last_applied_gain_db(1), WithinAbs(0.0f, 0.001f));
}

TEST_CASE("DynamicEq supports external sidechain for dynamic bands", "[mastering][eq]") {
  constexpr int sample_rate = 48000;
  DynamicEq eq;
  eq.prepare(sample_rate, 1024);
  eq.set_band(0, {EqBandType::Peak, 1000.0f, 0.0f, 2.0f, -24.0f, 4.0f, -9.0f, true});

  auto program = sine(1000.0f, sample_rate, sample_rate, 0.02f);
  auto sidechain = sine(1000.0f, sample_rate, sample_rate, 0.8f);
  const float before = rms_tail(program, 4096);
  const float* sidechain_channels[] = {sidechain.data()};

  eq.set_sidechain(sidechain_channels, 1, static_cast<int>(sidechain.size()));
  process(eq, program);

  REQUIRE(eq.last_band_detector_db(0) > -6.0f);
  REQUIRE(eq.last_applied_gain_db(0) < -6.0f);
  REQUIRE(rms_tail(program, 4096) / before < 0.7f);
}

TEST_CASE("DynamicEq supports tunable sidechain frequency and timing", "[mastering][eq]") {
  constexpr int sample_rate = 48000;
  DynamicEq eq;
  eq.prepare(sample_rate, 1024);
  eq.set_band(0, {EqBandType::Peak, 1000.0f, 0.0f, 2.0f, -24.0f, 4.0f, -9.0f, true, 2.0f, 8000.0f,
                  0.1f, 20.0f, 0.5f});

  auto program = sine(1000.0f, sample_rate, sample_rate, 0.2f);
  auto sidechain = sine(8000.0f, sample_rate, sample_rate, 0.8f);
  const float* sidechain_channels[] = {sidechain.data()};

  eq.set_sidechain(sidechain_channels, 1, static_cast<int>(sidechain.size()));
  process(eq, program);

  REQUIRE(eq.last_band_detector_db(0) > -8.0f);
  REQUIRE(eq.last_applied_gain_db(0) < -6.0f);
}

TEST_CASE("DynamicEq validates band parameters", "[mastering][eq]") {
  DynamicEq eq;
  eq.prepare(48000.0, 512);

  REQUIRE_THROWS(eq.set_band(DynamicEq::kMaxBands, {}));
  REQUIRE_THROWS(eq.set_band(0, {EqBandType::Peak, 0.0f, 0.0f, 1.0f, -20.0f, 2.0f, -6.0f, true}));
  REQUIRE_THROWS(
      eq.set_band(0, {EqBandType::Peak, 1000.0f, 0.0f, 0.0f, -20.0f, 2.0f, -6.0f, true}));
  REQUIRE_THROWS(
      eq.set_band(0, {EqBandType::Peak, 1000.0f, 0.0f, 1.0f, -20.0f, 0.5f, -6.0f, true}));
  REQUIRE_THROWS(
      eq.set_band(0, {EqBandType::Peak, 1000.0f, 0.0f, 1.0f, -20.0f, 2.0f, -6.0f, true, 0.0f}));
  std::vector<float> sidechain(4, 0.0f);
  const float* sidechain_channels[] = {sidechain.data()};
  eq.set_sidechain(sidechain_channels, 1, 4);
  std::vector<float> program(3, 0.0f);
  float* program_channels[] = {program.data()};
  REQUIRE_THROWS(eq.process(program_channels, 1, 3));
}
