#include <algorithm>
#include <array>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <cmath>
#include <complex>
#include <vector>

#include "mastering/eq/api_style.h"
#include "mastering/eq/band_pass.h"
#include "mastering/eq/cut_filter.h"
#include "mastering/eq/dynamic_eq.h"
#include "mastering/eq/equalizer.h"
#include "mastering/eq/graphic_eq.h"
#include "mastering/eq/linear_phase.h"
#include "mastering/eq/mid_side_eq.h"
#include "mastering/eq/minimum_phase.h"
#include "mastering/eq/parametric.h"
#include "mastering/eq/pultec.h"
#include "mastering/eq/shelving.h"
#include "mastering/eq/spectrum_engine.h"
#include "mastering/eq/spectrum_registry.h"
#include "mastering/eq/tilt.h"
#include "rt/biquad_design.h"
#include "util/constants.h"

using Catch::Matchers::WithinAbs;
using namespace sonare::mastering::eq;

namespace {

using sonare::constants::kButterworthQ;
using sonare::constants::kPiD;

std::vector<float> sine(float frequency_hz, int sample_rate, int samples, float amplitude = 0.25f) {
  std::vector<float> out(static_cast<size_t>(samples));
  for (int i = 0; i < samples; ++i) {
    out[static_cast<size_t>(i)] =
        amplitude * static_cast<float>(std::sin(2.0 * kPiD * frequency_hz * i / sample_rate));
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

float max_abs_difference(const std::vector<float>& lhs, const std::vector<float>& rhs) {
  const size_t count = std::min(lhs.size(), rhs.size());
  float peak = 0.0f;
  for (size_t i = 0; i < count; ++i) {
    peak = std::max(peak, std::abs(lhs[i] - rhs[i]));
  }
  return peak;
}

float kernel_magnitude_at(const std::vector<float>& kernel, double frequency_hz,
                          double sample_rate) {
  std::complex<double> response{0.0, 0.0};
  const double omega = 2.0 * kPiD * frequency_hz / sample_rate;
  for (size_t n = 0; n < kernel.size(); ++n) {
    response += static_cast<double>(kernel[n]) *
                std::exp(std::complex<double>(0.0, -omega * static_cast<double>(n)));
  }
  return static_cast<float>(std::abs(response));
}

void process(sonare::rt::ProcessorBase& processor, std::vector<float>& mono) {
  float* channels[] = {mono.data()};
  processor.process(channels, 1, static_cast<int>(mono.size()));
}

void process_stereo(sonare::rt::ProcessorBase& processor, std::vector<float>& left,
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
  eq.set_band(0, {EqBandType::HighPass, 500.0f, 0.0f, kButterworthQ, true});

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
      0, {EqBandType::LowPass, 1000.0f, 0.0f, kButterworthQ, true, BiquadCoeffMode::Vicanek});
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
      0, {EqBandType::HighPass, 1000.0f, 0.0f, kButterworthQ, true, BiquadCoeffMode::Vicanek});
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
      0, {EqBandType::LowShelf, 1000.0f, 6.0f, kButterworthQ, true, BiquadCoeffMode::Vicanek});
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
      0, {EqBandType::HighShelf, 6000.0f, 6.0f, kButterworthQ, true, BiquadCoeffMode::Vicanek});
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

TEST_CASE("ParametricEq Vicanek high shelf falls back when endpoint error is excessive",
          "[mastering][eq]") {
  constexpr int sample_rate = 48000;
  constexpr float gain_db = 24.0f;
  const float w0 = static_cast<float>(2.0 * kPiD * 18000.0 / sample_rate);
  const auto coeffs = sonare::rt::vicanek_high_shelf(w0, gain_db);

  const float nyquist_mag =
      sonare::rt::biquad_magnitude(coeffs, static_cast<float>(sonare::constants::kPiD * 0.999));
  REQUIRE_THAT(20.0f * std::log10(nyquist_mag), WithinAbs(gain_db, 0.05f));

  ParametricEq eq;
  eq.prepare(sample_rate, 1024);
  eq.set_band(
      0, {EqBandType::HighShelf, 18000.0f, gain_db, kButterworthQ, true, BiquadCoeffMode::Vicanek});

  auto high = sine(23000.0f, sample_rate, sample_rate);
  const float before = rms_tail(high, 4096);
  process(eq, high);
  REQUIRE(20.0f * std::log10(rms_tail(high, 4096) / before) > 22.0f);
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

TEST_CASE("ParametricEq supports 24 bands while preserving old aggregate initialization",
          "[mastering][eq]") {
  ParametricEq eq;
  eq.prepare(48000.0, 512);
  eq.set_band(23, {EqBandType::Peak, 1000.0f, 0.0f, 1.0f, true});

  REQUIRE(eq.band(23).enabled);
  REQUIRE(eq.band(23).type == EqBandType::Peak);
  REQUIRE(eq.band(23).coeff_mode == BiquadCoeffMode::Rbj);
  REQUIRE(eq.band(23).phase == PhaseMode::Inherit);
}

TEST_CASE("EqualizerProcessor matches ParametricEq for E0 stereo zero-latency bands",
          "[mastering][eq]") {
  constexpr int sample_rate = 48000;
  const EqBand band{EqBandType::Peak, 1000.0f, 6.0f, 1.0f, true};

  ParametricEq reference;
  reference.prepare(sample_rate, 1024);
  reference.set_band(0, band);

  EqualizerProcessor eq({2});
  eq.prepare(sample_rate, 4096);
  eq.set_band(0, band);

  auto ref_left = sine(1000.0f, sample_rate, 4096);
  auto ref_right = sine(4000.0f, sample_rate, 4096);
  auto got_left = ref_left;
  auto got_right = ref_right;

  process_stereo(reference, ref_left, ref_right);
  process_stereo(eq, got_left, got_right);

  REQUIRE(eq.latency_samples() == 0);
  for (size_t i = 0; i < got_left.size(); ++i) {
    REQUIRE_THAT(got_left[i], WithinAbs(ref_left[i], 0.000001f));
    REQUIRE_THAT(got_right[i], WithinAbs(ref_right[i], 0.000001f));
  }
}

TEST_CASE("EqualizerProcessor realtime parameter updates preserve IIR history", "[mastering][eq]") {
  constexpr int sample_rate = 48000;
  constexpr int block = 512;
  const EqBand band{EqBandType::Peak, 700.0f, 6.0f, 0.6f, true};

  EqualizerProcessor reference({1});
  reference.prepare(sample_rate, block);
  reference.set_band(0, band);
  EqualizerProcessor updated({1});
  updated.prepare(sample_rate, block);
  updated.set_band(0, band);

  auto ref = sine(143.0f, sample_rate, block * 4, 0.3f);
  auto got = ref;
  for (int offset = 0; offset < static_cast<int>(ref.size()); offset += block) {
    float* ref_channels[] = {ref.data() + offset};
    reference.process(ref_channels, 1, block);

    float* got_channels[] = {got.data() + offset};
    updated.process(got_channels, 1, block);
    if (offset == block) {
      REQUIRE(updated.set_parameter(1, band.gain_db));
    }
  }

  for (size_t i = 0; i < got.size(); ++i) {
    REQUIRE_THAT(got[i], WithinAbs(ref[i], 0.000001f));
  }
}

TEST_CASE("EqualizerProcessor rejects realtime-unsafe LinearPhase dynamic bands",
          "[mastering][eq]") {
  EqualizerProcessor eq;
  eq.prepare(48000.0, 512);

  EqBand band{EqBandType::Peak, 1000.0f, 0.0f, 1.0f, true};
  band.phase = PhaseMode::LinearPhase;
  band.dyn.enabled = true;
  band.dyn.threshold_db = -30.0f;
  REQUIRE_THROWS(eq.set_band(0, band));
}

TEST_CASE("EqualizerProcessor routes stereo LinearPhase bands before IIR and reports latency",
          "[mastering][eq]") {
  constexpr int sample_rate = 48000;
  constexpr int block_size = 4096;

  EqBand linear_band{EqBandType::Peak, 1000.0f, 6.0f, 1.0f, true};
  linear_band.phase = PhaseMode::LinearPhase;
  EqBand iir_band{EqBandType::HighShelf, 6000.0f, -3.0f, 0.8f, true};

  LinearPhaseEq linear_reference;
  linear_reference.prepare(sample_rate, block_size);
  linear_reference.prepare_channels(2);
  linear_reference.set_band(0, linear_band);
  ParametricEq iir_reference;
  iir_reference.prepare(sample_rate, block_size);
  iir_reference.set_band(0, iir_band);

  EqualizerProcessor eq({2});
  eq.prepare(sample_rate, block_size);
  eq.set_band(0, linear_band);
  eq.set_band(1, iir_band);

  auto ref_left = sine(1000.0f, sample_rate, block_size);
  auto ref_right = sine(7000.0f, sample_rate, block_size, 0.18f);
  auto got_left = ref_left;
  auto got_right = ref_right;

  process_stereo(linear_reference, ref_left, ref_right);
  process_stereo(iir_reference, ref_left, ref_right);
  process_stereo(eq, got_left, got_right);

  REQUIRE(eq.latency_samples() == linear_reference.latency_samples());
  for (size_t i = 0; i < got_left.size(); ++i) {
    REQUIRE_THAT(got_left[i], WithinAbs(ref_left[i], 0.000001f));
    REQUIRE_THAT(got_right[i], WithinAbs(ref_right[i], 0.000001f));
  }
}

TEST_CASE("EqualizerProcessor routes Left and Right LinearPhase stages with matched delay",
          "[mastering][eq]") {
  constexpr int sample_rate = 48000;
  constexpr int block_size = 2048;

  EqBand left_band{EqBandType::Peak, 1000.0f, 6.0f, 1.0f, true};
  left_band.phase = PhaseMode::LinearPhase;
  left_band.placement = StereoPlacement::Left;

  LinearPhaseEq left_reference;
  left_reference.prepare(sample_rate, block_size);
  left_reference.prepare_channels(1);
  left_reference.set_band(0, left_band);
  LinearPhaseEq right_reference;
  right_reference.prepare(sample_rate, block_size);
  right_reference.prepare_channels(1);

  EqualizerProcessor eq({2});
  eq.prepare(sample_rate, block_size);
  eq.set_band(0, left_band);

  auto ref_left = sine(1000.0f, sample_rate, block_size);
  auto ref_right = sine(1000.0f, sample_rate, block_size);
  auto got_left = ref_left;
  auto got_right = ref_right;

  process(left_reference, ref_left);
  process(right_reference, ref_right);
  process_stereo(eq, got_left, got_right);

  REQUIRE(eq.latency_samples() == left_reference.latency_samples());
  for (size_t i = 0; i < got_left.size(); ++i) {
    REQUIRE_THAT(got_left[i], WithinAbs(ref_left[i], 0.000001f));
    REQUIRE_THAT(got_right[i], WithinAbs(ref_right[i], 0.000001f));
  }
}

TEST_CASE("EqualizerProcessor routes Mid and Side LinearPhase stages with matched delay",
          "[mastering][eq]") {
  constexpr int sample_rate = 48000;
  constexpr int block_size = 2048;

  EqBand mid_band{EqBandType::Peak, 1000.0f, 6.0f, 1.0f, true};
  mid_band.phase = PhaseMode::LinearPhase;
  mid_band.placement = StereoPlacement::Mid;

  LinearPhaseEq mid_reference;
  mid_reference.prepare(sample_rate, block_size);
  mid_reference.prepare_channels(1);
  mid_reference.set_band(0, mid_band);
  LinearPhaseEq side_reference;
  side_reference.prepare(sample_rate, block_size);
  side_reference.prepare_channels(1);

  EqualizerProcessor eq({2});
  eq.prepare(sample_rate, block_size);
  eq.set_band(0, mid_band);

  auto left = sine(1000.0f, sample_rate, block_size);
  auto right = sine(3000.0f, sample_rate, block_size, 0.18f);
  std::vector<float> ref_mid(left.size());
  std::vector<float> ref_side(left.size());
  for (size_t i = 0; i < left.size(); ++i) {
    ref_mid[i] = (left[i] + right[i]) * 0.5f;
    ref_side[i] = (left[i] - right[i]) * 0.5f;
  }

  auto got_left = left;
  auto got_right = right;
  process(mid_reference, ref_mid);
  process(side_reference, ref_side);
  process_stereo(eq, got_left, got_right);

  REQUIRE(eq.latency_samples() == mid_reference.latency_samples());
  for (size_t i = 0; i < got_left.size(); ++i) {
    REQUIRE_THAT(got_left[i], WithinAbs(ref_mid[i] + ref_side[i], 0.000001f));
    REQUIRE_THAT(got_right[i], WithinAbs(ref_mid[i] - ref_side[i], 0.000001f));
  }

  float* mono[] = {got_left.data()};
  REQUIRE_THROWS(eq.process(mono, 1, static_cast<int>(got_left.size())));
}

TEST_CASE("EqualizerProcessor applies Left and Right placement independently", "[mastering][eq]") {
  constexpr int sample_rate = 48000;
  EqualizerProcessor eq({2});
  eq.prepare(sample_rate, 4096);

  EqBand left_boost{EqBandType::Peak, 1000.0f, 6.0f, 1.0f, true};
  left_boost.placement = StereoPlacement::Left;
  EqBand right_cut{EqBandType::Peak, 1000.0f, -6.0f, 1.0f, true};
  right_cut.placement = StereoPlacement::Right;
  eq.set_band(0, left_boost);
  eq.set_band(1, right_cut);

  auto left = sine(1000.0f, sample_rate, 4096);
  auto right = left;
  const float before = rms_tail(left, 512);
  process_stereo(eq, left, right);

  REQUIRE(rms_tail(left, 512) / before > 1.7f);
  REQUIRE(rms_tail(right, 512) / before < 0.6f);
}

TEST_CASE("EqualizerProcessor Mid and Side placement matches MidSideEq convention",
          "[mastering][eq]") {
  constexpr int sample_rate = 48000;
  EqBand mid_band{EqBandType::Peak, 1000.0f, 6.0f, 1.0f, true};
  mid_band.placement = StereoPlacement::Mid;
  EqBand side_band{EqBandType::Peak, 3000.0f, -4.0f, 1.0f, true};
  side_band.placement = StereoPlacement::Side;

  EqualizerProcessor eq({2});
  eq.prepare(sample_rate, 4096);
  eq.set_band(0, mid_band);
  eq.set_band(1, side_band);

  MidSideEq reference;
  reference.prepare(sample_rate, 4096);
  reference.set_mid_band(0, mid_band);
  reference.set_side_band(1, side_band);

  auto ref_left = sine(1000.0f, sample_rate, 4096);
  auto ref_right = sine(3000.0f, sample_rate, 4096, 0.18f);
  auto got_left = ref_left;
  auto got_right = ref_right;

  process_stereo(reference, ref_left, ref_right);
  process_stereo(eq, got_left, got_right);

  for (size_t i = 0; i < got_left.size(); ++i) {
    REQUIRE_THAT(got_left[i], WithinAbs(ref_left[i], 0.000001f));
    REQUIRE_THAT(got_right[i], WithinAbs(ref_right[i], 0.000001f));
  }
}

TEST_CASE("EqualizerProcessor rejects Mid and Side placement on mono input", "[mastering][eq]") {
  EqualizerProcessor eq({2});
  eq.prepare(48000.0, 512);
  EqBand band{EqBandType::Peak, 1000.0f, 0.0f, 1.0f, true};
  band.placement = StereoPlacement::Mid;
  eq.set_band(0, band);

  auto mono = sine(1000.0f, 48000, 512);
  float* channels[] = {mono.data()};
  REQUIRE_THROWS(eq.process(channels, 1, static_cast<int>(mono.size())));
}

TEST_CASE("EqualizerProcessor respects bypass and solo listen selection", "[mastering][eq]") {
  constexpr int sample_rate = 48000;
  EqualizerProcessor eq({1});
  eq.prepare(sample_rate, 1024);

  EqBand boost{EqBandType::Peak, 1000.0f, 12.0f, 1.0f, true};
  boost.bypassed = true;
  eq.set_band(0, boost);

  auto bypassed = sine(1000.0f, sample_rate, 1024);
  const auto original = bypassed;
  process(eq, bypassed);
  for (size_t i = 0; i < bypassed.size(); ++i) {
    REQUIRE_THAT(bypassed[i], WithinAbs(original[i], 0.000001f));
  }

  eq.clear();
  EqBand selected{EqBandType::Peak, 1000.0f, 12.0f, 1.0f, true};
  selected.soloed = true;
  EqBand ignored{EqBandType::Peak, 1000.0f, -12.0f, 1.0f, true};
  eq.set_band(0, selected);
  eq.set_band(1, ignored);

  ParametricEq reference;
  reference.prepare(sample_rate, 1024);
  reference.set_band(0, {EqBandType::BandPass, selected.frequency_hz, 0.0f, selected.q, true});

  auto expected = sine(1000.0f, sample_rate, 1024);
  auto actual = expected;
  process(reference, expected);
  process(eq, actual);
  for (size_t i = 0; i < actual.size(); ++i) {
    REQUIRE_THAT(actual[i], WithinAbs(expected[i], 0.000001f));
  }
}

TEST_CASE("EqualizerProcessor resolves inherited phase mode and enforces prepared channels",
          "[mastering][eq]") {
  constexpr int sample_rate = 48000;
  const EqBand inherited{EqBandType::Peak, 9000.0f, 9.0f, 0.7f, true};

  EqualizerProcessor eq({2});
  eq.prepare(sample_rate, 256);
  eq.set_phase_mode(PhaseMode::NaturalPhase);
  eq.set_band(0, inherited);

  ParametricEq reference;
  reference.prepare(sample_rate, 256);
  reference.set_band(0, {EqBandType::Peak, 9000.0f, 9.0f, 0.7f, true, BiquadCoeffMode::Vicanek});

  auto ref_left = sine(9000.0f, sample_rate, 256);
  auto ref_right = sine(1200.0f, sample_rate, 256);
  auto got_left = ref_left;
  auto got_right = ref_right;
  process_stereo(reference, ref_left, ref_right);
  process_stereo(eq, got_left, got_right);
  for (size_t i = 0; i < got_left.size(); ++i) {
    REQUIRE_THAT(got_left[i], WithinAbs(ref_left[i], 0.000001f));
    REQUIRE_THAT(got_right[i], WithinAbs(ref_right[i], 0.000001f));
  }

  float* too_many[] = {got_left.data(), got_right.data(), got_left.data()};
  REQUIRE_THROWS(eq.process(too_many, 3, static_cast<int>(got_left.size())));
  float* stereo[] = {got_left.data(), got_right.data()};
  REQUIRE_THROWS(eq.process(stereo, 2, 257));
  REQUIRE_THROWS(eq.set_phase_mode(PhaseMode::Inherit));
  eq.clear();
  eq.set_phase_mode(PhaseMode::LinearPhase);
  eq.set_band(0, inherited);
  REQUIRE(eq.latency_samples() > 0);
}

TEST_CASE(
    "EqualizerProcessor per-band NaturalPhase uses Vicanek without changing the global default",
    "[mastering][eq]") {
  constexpr int sample_rate = 48000;
  EqBand requested{EqBandType::Peak, 12000.0f, 9.0f, 0.8f, true, BiquadCoeffMode::Rbj};
  requested.phase = PhaseMode::NaturalPhase;

  EqualizerProcessor eq({1});
  eq.prepare(sample_rate, 4096);
  REQUIRE(eq.phase_mode() == PhaseMode::ZeroLatency);
  eq.set_band(0, requested);
  REQUIRE(eq.band(0).coeff_mode == BiquadCoeffMode::Rbj);

  ParametricEq reference;
  reference.prepare(sample_rate, 512);
  reference.set_band(0, {EqBandType::Peak, 12000.0f, 9.0f, 0.8f, true, BiquadCoeffMode::Vicanek});

  auto expected = sine(12000.0f, sample_rate, 4096);
  auto actual = expected;
  process(reference, expected);
  process(eq, actual);

  double diff = 0.0;
  for (size_t i = 0; i < actual.size(); ++i) {
    diff += std::abs(static_cast<double>(actual[i] - expected[i]));
  }
  REQUIRE(diff < 1.0e-6);
}

TEST_CASE("EqualizerProcessor dynamic band attenuates above threshold on any of 24 bands",
          "[mastering][eq]") {
  constexpr int sample_rate = 48000;
  EqualizerProcessor eq({1});
  eq.prepare(sample_rate, 4096);

  EqBand band{EqBandType::Peak, 1000.0f, 0.0f, 2.0f, true};
  band.dyn.enabled = true;
  band.dyn.threshold_db = -40.0f;
  band.dyn.ratio = 4.0f;
  band.dyn.range_db = -12.0f;
  band.dyn.attack_ms = 0.0f;
  band.dyn.release_ms = 10.0f;
  eq.set_band(23, band);

  auto quiet = sine(1000.0f, sample_rate, 4096, 0.002f);
  auto loud = sine(1000.0f, sample_rate, 4096, 0.5f);
  const float quiet_before = rms_tail(quiet, 512);
  const float loud_before = rms_tail(loud, 512);

  process(eq, quiet);
  const float quiet_gain = rms_tail(quiet, 512) / quiet_before;
  const float quiet_applied = eq.last_applied_gain_db(23);
  eq.reset();
  process(eq, loud);
  const float loud_gain = rms_tail(loud, 512) / loud_before;

  REQUIRE(eq.last_band_detector_db(23) > -40.0f);
  REQUIRE(quiet_applied == 0.0f);
  REQUIRE(eq.last_applied_gain_db(23) < -3.0f);
  REQUIRE(loud_gain < quiet_gain * 0.8f);
}

TEST_CASE("EqualizerProcessor dynamic band can use an external sidechain", "[mastering][eq]") {
  constexpr int sample_rate = 48000;
  EqualizerProcessor eq({1});
  eq.prepare(sample_rate, 4096);

  EqBand band{EqBandType::Peak, 1000.0f, 0.0f, 2.0f, true};
  band.dyn.enabled = true;
  band.dyn.external_sidechain = true;
  band.dyn.threshold_db = -32.0f;
  band.dyn.ratio = 4.0f;
  band.dyn.range_db = -12.0f;
  band.dyn.attack_ms = 0.0f;
  band.dyn.release_ms = 10.0f;
  eq.set_band(0, band);

  auto quiet = sine(1000.0f, sample_rate, 4096, 0.005f);
  auto internal = quiet;
  process(eq, internal);
  const float internal_gain = eq.last_applied_gain_db(0);

  eq.reset();
  quiet = sine(1000.0f, sample_rate, 4096, 0.005f);
  auto key = sine(1000.0f, sample_rate, 4096, 0.8f);
  const float* key_channels[] = {key.data()};
  eq.set_sidechain(key_channels, 1, static_cast<int>(key.size()));
  process(eq, quiet);

  REQUIRE_THAT(internal_gain, WithinAbs(0.0f, 0.0001f));
  REQUIRE(eq.last_band_detector_db(0) > -32.0f);
  REQUIRE(eq.last_applied_gain_db(0) < -3.0f);
  REQUIRE(rms_tail(quiet, 512) < rms_tail(internal, 512) * 0.8f);
}

TEST_CASE("EqualizerProcessor validates and clears external sidechain buffers", "[mastering][eq]") {
  EqualizerProcessor eq({2});
  eq.prepare(48000, 128);

  std::array<float, 128> left{};
  std::array<float, 128> right{};
  const float* stereo_key[] = {left.data(), right.data()};
  REQUIRE_NOTHROW(eq.set_sidechain(stereo_key, 2, 128));
  eq.clear_sidechain();
  REQUIRE_THROWS(eq.set_sidechain(nullptr, 1, 128));
  const float* bad_key[] = {left.data(), nullptr};
  REQUIRE_THROWS(eq.set_sidechain(bad_key, 2, 128));

  REQUIRE_NOTHROW(eq.set_sidechain(stereo_key, 2, 64));
  float* audio[] = {left.data(), right.data()};
  REQUIRE_THROWS(eq.process(audio, 2, 128));

  REQUIRE_NOTHROW(eq.set_sidechain(nullptr, 0, 0));
}

TEST_CASE("EqualizerProcessor dynamic auto-threshold is input-gain relative", "[mastering][eq]") {
  static constexpr int sample_rate = 48000;

  auto run = [](float amplitude) {
    EqualizerProcessor eq({1});
    eq.prepare(sample_rate, 4096);
    EqBand band{EqBandType::Peak, 1000.0f, 0.0f, 2.0f, true};
    band.dyn.enabled = true;
    band.dyn.auto_threshold = true;
    band.dyn.ratio = 4.0f;
    band.dyn.range_db = -12.0f;
    band.dyn.attack_ms = 0.0f;
    band.dyn.release_ms = 10.0f;
    eq.set_band(0, band);
    auto audio = sine(1000.0f, sample_rate, 4096, amplitude);
    process(eq, audio);
    return eq.last_applied_gain_db(0);
  };

  const float quiet_gain = run(0.05f);
  const float loud_gain = run(0.5f);
  REQUIRE(quiet_gain < -2.0f);
  REQUIRE(loud_gain < -2.0f);
  REQUIRE_THAT(loud_gain, WithinAbs(quiet_gain, 0.2f));
}

TEST_CASE("EqualizerProcessor auto-gain compensates block RMS changes", "[mastering][eq]") {
  constexpr int sample_rate = 48000;
  const EqBand boost{EqBandType::Peak, 1000.0f, 12.0f, 1.0f, true};

  EqualizerProcessor plain({1});
  plain.prepare(sample_rate, 4096);
  plain.set_band(0, boost);
  EqualizerProcessor compensated({1});
  compensated.prepare(sample_rate, 4096);
  compensated.set_band(0, boost);
  compensated.set_auto_gain_enabled(true);

  auto plain_audio = sine(1000.0f, sample_rate, 4096, 0.2f);
  auto compensated_audio = plain_audio;
  const float before = rms_tail(plain_audio, 512);
  process(plain, plain_audio);
  process(compensated, compensated_audio);

  REQUIRE(rms_tail(plain_audio, 512) > before * 3.0f);
  REQUIRE(rms_tail(compensated_audio, 512) < rms_tail(plain_audio, 512) * 0.55f);
  REQUIRE(compensated.last_auto_gain_db() < -6.0f);
}

TEST_CASE("EqualizerProcessor gain scale controls applied static and dynamic gain",
          "[mastering][eq]") {
  constexpr int sample_rate = 48000;

  EqualizerProcessor scaled({1});
  scaled.prepare(sample_rate, 4096);
  scaled.set_gain_scale(0.5f);
  EqBand peak{EqBandType::Peak, 1000.0f, 6.0f, 1.0f, true};
  scaled.set_band(0, peak);

  ParametricEq reference;
  reference.prepare(sample_rate, 4096);
  reference.set_band(0, {EqBandType::Peak, 1000.0f, 3.0f, 1.0f, true});

  auto scaled_audio = sine(1000.0f, sample_rate, 4096, 0.1f);
  auto reference_audio = scaled_audio;
  process(scaled, scaled_audio);
  process(reference, reference_audio);
  REQUIRE_THAT(rms_tail(scaled_audio, 1024), WithinAbs(rms_tail(reference_audio, 1024), 0.002f));
  REQUIRE_THAT(scaled.spectrum_snapshot().band_gain_db[0], WithinAbs(3.0f, 0.0001f));

  EqualizerProcessor dynamic({1});
  dynamic.prepare(sample_rate, 4096);
  dynamic.set_gain_scale(0.5f);
  EqBand dyn{EqBandType::Peak, 1000.0f, 0.0f, 2.0f, true};
  dyn.dyn.enabled = true;
  dyn.dyn.threshold_db = -40.0f;
  dyn.dyn.ratio = 4.0f;
  dyn.dyn.range_db = -12.0f;
  dyn.dyn.attack_ms = 0.0f;
  dyn.dyn.release_ms = 10.0f;
  dynamic.set_band(0, dyn);
  auto loud = sine(1000.0f, sample_rate, 4096, 0.5f);
  process(dynamic, loud);
  REQUIRE(dynamic.last_applied_gain_db(0) < -3.0f);
  REQUIRE(dynamic.last_applied_gain_db(0) > -6.1f);
  REQUIRE_THAT(dynamic.spectrum_snapshot().band_gain_db[0],
               WithinAbs(dynamic.last_applied_gain_db(0), 0.0001f));
}

TEST_CASE("EqualizerProcessor output gain and pan apply after EQ", "[mastering][eq]") {
  EqualizerProcessor eq({2});
  eq.prepare(48000, 128);
  eq.set_output_gain_db(6.0f);
  eq.set_output_pan(1.0f);

  std::vector<float> left(128, 0.25f);
  std::vector<float> right(128, 0.25f);
  process_stereo(eq, left, right);
  REQUIRE(peak_abs(left) < 0.000001f);
  REQUIRE_THAT(right[0], WithinAbs(0.25f * std::pow(10.0f, 6.0f / 20.0f), 0.0001f));

  eq.reset();
  eq.set_output_pan(-1.0f);
  left.assign(128, 0.25f);
  right.assign(128, 0.25f);
  process_stereo(eq, left, right);
  REQUIRE_THAT(left[0], WithinAbs(0.25f * std::pow(10.0f, 6.0f / 20.0f), 0.0001f));
  REQUIRE(peak_abs(right) < 0.000001f);

  eq.reset();
  eq.set_output_pan(1.0f);
  std::vector<float> mono(128, 0.25f);
  process(eq, mono);
  REQUIRE_THAT(mono[0], WithinAbs(0.25f * std::pow(10.0f, 6.0f / 20.0f), 0.0001f));
}

TEST_CASE("EqualizerProcessor soloed band listens through a bandpass region", "[mastering][eq]") {
  constexpr int sample_rate = 48000;
  EqBand solo{EqBandType::Peak, 1000.0f, 12.0f, 4.0f, true};
  solo.soloed = true;

  EqualizerProcessor eq({1});
  eq.prepare(sample_rate, 4096);
  eq.set_band(0, solo);

  auto center = sine(1000.0f, sample_rate, 4096, 0.25f);
  auto distant = sine(8000.0f, sample_rate, 4096, 0.25f);
  const float center_before = rms_tail(center, 512);
  const float distant_before = rms_tail(distant, 512);
  process(eq, center);
  eq.reset();
  process(eq, distant);

  REQUIRE(rms_tail(center, 512) > center_before * 0.55f);
  REQUIRE(rms_tail(distant, 512) < distant_before * 0.2f);
}

TEST_CASE("EqualizerProcessor proportional Q narrows bell gain without mutating stored Q",
          "[mastering][eq]") {
  constexpr int sample_rate = 48000;
  EqBand regular{EqBandType::Peak, 1000.0f, 12.0f, 1.0f, true};
  EqBand proportional = regular;
  proportional.proportional_q = true;
  proportional.proportional_q_strength = 0.04f;

  EqualizerProcessor wide({1});
  wide.prepare(sample_rate, 4096);
  wide.set_band(0, regular);
  EqualizerProcessor narrow({1});
  narrow.prepare(sample_rate, 4096);
  narrow.set_band(0, proportional);

  auto wide_edge = sine(1800.0f, sample_rate, 4096, 0.2f);
  auto narrow_edge = wide_edge;
  process(wide, wide_edge);
  process(narrow, narrow_edge);

  REQUIRE(rms_tail(narrow_edge, 512) < rms_tail(wide_edge, 512) * 0.85f);
  REQUIRE_THAT(narrow.band(0).q, WithinAbs(1.0f, 0.0001f));
}

TEST_CASE("EqualizerProcessor supports 30 kHz bands when sample rate allows it",
          "[mastering][eq]") {
  constexpr int sample_rate = 96000;
  EqualizerProcessor eq({1});
  eq.prepare(sample_rate, 4096);
  eq.set_band(0, {EqBandType::Peak, 30000.0f, 9.0f, 2.0f, true, BiquadCoeffMode::Vicanek});

  auto center = sine(30000.0f, sample_rate, 4096, 0.2f);
  auto low = sine(8000.0f, sample_rate, 4096, 0.2f);
  const float center_before = rms_tail(center, 512);
  const float low_before = rms_tail(low, 512);
  process(eq, center);
  eq.reset();
  process(eq, low);

  REQUIRE(rms_tail(center, 512) > center_before * 1.6f);
  REQUIRE(rms_tail(low, 512) < low_before * 1.2f);
}

TEST_CASE("EqualizerProcessor TiltShelf maps to opposite shelves", "[mastering][eq]") {
  constexpr int sample_rate = 48000;
  EqualizerProcessor eq({1});
  eq.prepare(sample_rate, 4096);
  eq.set_band(23, {EqBandType::TiltShelf, 1000.0f, 12.0f, 1.0f, true});

  auto low = sine(120.0f, sample_rate, 4096, 0.2f);
  auto high = sine(8000.0f, sample_rate, 4096, 0.2f);
  const float low_before = rms_tail(low, 512);
  const float high_before = rms_tail(high, 512);
  process(eq, low);
  eq.reset();
  process(eq, high);

  REQUIRE(rms_tail(low, 512) < low_before * 0.65f);
  REQUIRE(rms_tail(high, 512) > high_before * 1.6f);
}

TEST_CASE("EqualizerProcessor FlatTilt tilts gently and differs from TiltShelf",
          "[mastering][eq]") {
  constexpr int sample_rate = 48000;

  // Correct tilt direction: lows cut, highs boosted around the pivot.
  EqualizerProcessor eq({1});
  eq.prepare(sample_rate, 4096);
  eq.set_band(0, {EqBandType::FlatTilt, 1000.0f, 12.0f, 1.0f, true});
  auto low = sine(120.0f, sample_rate, 4096, 0.2f);
  auto high = sine(8000.0f, sample_rate, 4096, 0.2f);
  const float low_before = rms_tail(low, 512);
  const float high_before = rms_tail(high, 512);
  process(eq, low);
  eq.reset();
  process(eq, high);
  REQUIRE(rms_tail(low, 512) < low_before);
  REQUIRE(rms_tail(high, 512) > high_before);

  // Near the pivot, FlatTilt's spread shelves cut less than TiltShelf's
  // Butterworth-Q shelf, proving the two band types are no longer identical.
  auto tilt_response = [&](EqBandType type, float frequency) {
    EqualizerProcessor probe({1});
    probe.prepare(sample_rate, 4096);
    probe.set_band(0, {type, 1000.0f, 12.0f, 1.0f, true});
    auto buffer = sine(frequency, sample_rate, 4096, 0.2f);
    process(probe, buffer);
    return rms_tail(buffer, 512);
  };
  const float flat_300 = tilt_response(EqBandType::FlatTilt, 300.0f);
  const float tilt_300 = tilt_response(EqBandType::TiltShelf, 300.0f);
  REQUIRE(flat_300 > tilt_300 * 1.05f);
}

TEST_CASE("EqualizerProcessor rejects internal backend overflow without partial commit",
          "[mastering][eq]") {
  EqualizerProcessor eq({1});
  eq.prepare(48000.0, 512);

  EqBand tilt{EqBandType::TiltShelf, 1000.0f, 6.0f, 1.0f, true};
  for (size_t i = 0; i < EqualizerProcessor::kMaxBands / 2; ++i) {
    tilt.frequency_hz = 200.0f + static_cast<float>(i) * 100.0f;
    eq.set_band(i, tilt);
  }

  tilt.frequency_hz = 3000.0f;
  REQUIRE_THROWS(eq.set_band(EqualizerProcessor::kMaxBands / 2, tilt));
  REQUIRE_FALSE(eq.band(EqualizerProcessor::kMaxBands / 2).enabled);

  auto audio = sine(1000.0f, 48000, 512);
  REQUIRE_NOTHROW(process(eq, audio));
}

TEST_CASE("EqualizerProcessor validates expanded backend capacity before prepare",
          "[mastering][eq]") {
  EqualizerProcessor eq({1});

  EqBand tilt{EqBandType::TiltShelf, 1000.0f, 6.0f, 1.0f, true};
  for (size_t i = 0; i < EqualizerProcessor::kMaxBands / 2; ++i) {
    tilt.frequency_hz = 200.0f + static_cast<float>(i) * 100.0f;
    eq.set_band(i, tilt);
  }

  tilt.frequency_hz = 3000.0f;
  REQUIRE_THROWS(eq.set_band(EqualizerProcessor::kMaxBands / 2, tilt));
  REQUIRE_FALSE(eq.band(EqualizerProcessor::kMaxBands / 2).enabled);

  REQUIRE_NOTHROW(eq.prepare(48000.0, 512));
}

TEST_CASE("spectrum_grab_band chooses the nearest enabled EQ band or an empty slot",
          "[mastering][eq]") {
  std::array<EqBand, EqualizerProcessor::kMaxBands> bands{};
  bands[0] = {EqBandType::Peak, 200.0f, 0.0f, 1.0f, true};
  bands[1] = {EqBandType::Peak, 2000.0f, 0.0f, 1.0f, true};
  bands[2] = {EqBandType::Peak, 8000.0f, 0.0f, 1.0f, true};

  const auto existing = spectrum_grab_band(2600.0f, bands.data(), bands.size());
  REQUIRE(existing.use_existing);
  REQUIRE(existing.index == 1);

  bands[0].enabled = false;
  bands[1].enabled = false;
  bands[2].enabled = false;
  const auto empty = spectrum_grab_band(2600.0f, bands.data(), bands.size());
  REQUIRE_FALSE(empty.use_existing);
  REQUIRE(empty.index == 0);
}

TEST_CASE("SpectrumRegistry stores fixed profiles and reports overlapping bands",
          "[mastering][eq]") {
  auto& registry = SpectrumRegistry::instance();
  registry.reset();

  SpectrumProfile kick;
  kick.instance_id = 101;
  kick.active = true;
  kick.seq = 3;
  kick.band_db.fill(-120.0f);
  kick.band_db[2] = -18.0f;
  kick.band_db[8] = -42.0f;

  SpectrumProfile bass;
  bass.instance_id = 202;
  bass.active = true;
  bass.seq = 4;
  bass.band_db.fill(-120.0f);
  bass.band_db[2] = -12.0f;
  bass.band_db[6] = -20.0f;

  registry.publish(kick);
  registry.publish(bass);

  SpectrumProfile read_back;
  REQUIRE(registry.read(101, read_back));
  REQUIRE(read_back.seq == 3);
  REQUIRE_THAT(read_back.band_db[2], WithinAbs(-18.0f, 0.0001f));

  const auto report = registry.collisions(101, 202, -60.0f);
  REQUIRE(report.count == 1);
  REQUIRE(report.bands[0].band == 2);
  REQUIRE_THAT(report.bands[0].score_db, WithinAbs(-18.0f, 0.0001f));

  bass.band_db.fill(-120.0f);
  bass.band_db[3] = -14.0f;
  registry.publish(bass);
  const auto adjacent_report = registry.collisions(101, 202, -60.0f);
  REQUIRE(adjacent_report.count == 1);
  REQUIRE(adjacent_report.bands[0].band == 2);
  REQUIRE_THAT(adjacent_report.bands[0].score_db, WithinAbs(-18.0f, 0.0001f));

  registry.remove(101);
  REQUIRE_FALSE(registry.read(101, read_back));
}

TEST_CASE("SpectrumRegistry keeps fixed capacity entries stable when full", "[mastering][eq]") {
  auto& registry = SpectrumRegistry::instance();
  registry.reset();

  SpectrumProfile profile;
  profile.active = true;
  profile.seq = 1;
  profile.band_db.fill(-120.0f);
  for (uint64_t id = 1; id <= 64; ++id) {
    profile.instance_id = id;
    profile.band_db[0] = -60.0f + static_cast<float>(id);
    registry.publish(profile);
  }

  profile.instance_id = 65;
  profile.band_db[0] = 0.0f;
  registry.publish(profile);

  SpectrumProfile read_back;
  REQUIRE_FALSE(registry.read(65, read_back));
  REQUIRE(registry.read(1, read_back));
  REQUIRE_THAT(read_back.band_db[0], WithinAbs(-59.0f, 0.0001f));
  REQUIRE(registry.read(64, read_back));
  REQUIRE_THAT(read_back.band_db[0], WithinAbs(4.0f, 0.0001f));

  registry.reset();
}

TEST_CASE("EqualizerProcessor exposes pre/post spectrum snapshots and publishes a registry profile",
          "[mastering][eq]") {
  constexpr int sample_rate = 48000;
  SpectrumRegistry::instance().reset();
  {
    EqualizerProcessor eq({2, 303});
    eq.prepare(sample_rate, 1024);
    EqBand boost{EqBandType::Peak, 1000.0f, 9.0f, 1.0f, true};
    eq.set_band(0, boost);

    auto left = sine(1000.0f, sample_rate, 1024, 0.25f);
    auto right = sine(2000.0f, sample_rate, 1024, 0.1f);
    const float first_left = left[4];
    process_stereo(eq, left, right);

    const auto snapshot = eq.spectrum_snapshot();
    REQUIRE(snapshot.seq == 1);
    REQUIRE(snapshot.pre_count == kSpectrumStreamCapacity);
    REQUIRE(snapshot.post_count == kSpectrumStreamCapacity);
    REQUIRE_THAT(snapshot.pre[1].left, WithinAbs(first_left, 0.000001f));
    REQUIRE(std::abs(snapshot.post[1].left - snapshot.pre[1].left) > 0.000001f);
    REQUIRE_THAT(snapshot.band_gain_db[0], WithinAbs(9.0f, 0.0001f));

    SpectrumProfile profile;
    REQUIRE(SpectrumRegistry::instance().read(303, profile));
    REQUIRE(profile.seq == 1);
    bool has_activity = false;
    for (float db : profile.band_db) {
      has_activity = has_activity || db > -120.0f;
    }
    REQUIRE(has_activity);
  }

  SpectrumProfile removed;
  REQUIRE_FALSE(SpectrumRegistry::instance().read(303, removed));
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

TEST_CASE("LinearPhaseEq resolution presets select deterministic FFT and FIR sizes",
          "[mastering][eq]") {
  LinearPhaseEqConfig low;
  low.resolution = LinearPhaseEqConfig::Resolution::Low;
  low.partition_size = 512;
  LinearPhaseEq low_eq(low);
  low_eq.prepare(48000.0, 512);

  LinearPhaseEqConfig maximum;
  maximum.resolution = LinearPhaseEqConfig::Resolution::Maximum;
  maximum.partition_size = 512;
  // Explicit sizes are ignored when a named resolution is selected.
  maximum.fft_size = 2048;
  maximum.kernel_size = 513;
  LinearPhaseEq max_eq(maximum);
  max_eq.prepare(48000.0, 512);

  REQUIRE(low_eq.kernel().size() == 1025);
  REQUIRE(low_eq.latency_samples() == 512);
  REQUIRE(max_eq.kernel().size() == 16385);
  REQUIRE(max_eq.latency_samples() == 8192);
}

TEST_CASE("EqualizerProcessor forwards LinearPhase resolution config to FIR stages",
          "[mastering][eq]") {
  LinearPhaseEqConfig linear_config;
  linear_config.resolution = LinearPhaseEqConfig::Resolution::Low;
  linear_config.partition_size = 512;

  EqualizerProcessorConfig eq_config;
  eq_config.max_channels = 2;
  eq_config.linear_phase_config = linear_config;
  EqualizerProcessor eq(eq_config);
  eq.prepare(48000.0, 512);
  eq.set_phase_mode(PhaseMode::LinearPhase);
  eq.set_band(0, {EqBandType::Peak, 1000.0f, 3.0f, 1.0f, true});

  REQUIRE(eq.latency_samples() == 512);
  REQUIRE(eq.latency_samples_q8() == (512 << 8));
}

TEST_CASE("EqualizerProcessor high-pass slope controls attenuation depth", "[mastering][eq]") {
  constexpr int sample_rate = 48000;
  constexpr int samples = sample_rate;
  const auto slopes = std::array{6, 12, 24, 48, 72, 96};

  float previous_stop_gain = 1.0f;
  for (int slope : slopes) {
    EqualizerProcessor eq({1});
    eq.prepare(sample_rate, samples);
    EqBand band{EqBandType::HighPass, 1000.0f, 0.0f, sonare::constants::kButterworthQ, true};
    band.slope_db_oct = slope;
    eq.set_band(0, band);

    auto stop = sine(500.0f, sample_rate, samples);
    auto pass = sine(8000.0f, sample_rate, samples);
    const float stop_before = rms_tail(stop, 4096);
    const float pass_before = rms_tail(pass, 4096);
    process(eq, stop);
    eq.reset();
    process(eq, pass);

    const float stop_gain = rms_tail(stop, 4096) / stop_before;
    const float pass_gain = rms_tail(pass, 4096) / pass_before;
    REQUIRE(stop_gain < previous_stop_gain);
    REQUIRE(pass_gain > 0.85f);
    previous_stop_gain = stop_gain;
  }
}

TEST_CASE("EqualizerProcessor applies cut resonance consistently in cascades", "[mastering][eq]") {
  constexpr int sample_rate = 48000;

  EqBand flat_band{EqBandType::HighPass, 1000.0f, 0.0f, sonare::constants::kButterworthQ, true};
  flat_band.slope_db_oct = 48;
  EqualizerProcessor flat({1});
  flat.prepare(sample_rate, sample_rate);
  flat.set_band(0, flat_band);

  EqBand resonant_band = flat_band;
  resonant_band.q = 2.0f;
  EqualizerProcessor resonant({1});
  resonant.prepare(sample_rate, sample_rate);
  resonant.set_band(0, resonant_band);

  auto flat_cutoff = sine(1000.0f, sample_rate, sample_rate);
  auto resonant_cutoff = flat_cutoff;
  const float before = rms_tail(flat_cutoff, 4096);
  process(flat, flat_cutoff);
  process(resonant, resonant_cutoff);

  REQUIRE(rms_tail(resonant_cutoff, 4096) / before > rms_tail(flat_cutoff, 4096) / before * 1.2f);
}

TEST_CASE("EqualizerProcessor brickwall cuts route through FIR latency and rejection",
          "[mastering][eq]") {
  constexpr int sample_rate = 48000;

  EqualizerProcessorConfig config;
  config.max_channels = 1;
  config.linear_phase_config.resolution = LinearPhaseEqConfig::Resolution::Low;
  config.linear_phase_config.partition_size = 512;
  EqualizerProcessor eq(config);
  eq.prepare(sample_rate, sample_rate);

  EqBand high_pass{EqBandType::HighPass, 1000.0f, 0.0f, sonare::constants::kButterworthQ, true};
  high_pass.slope_db_oct = 0;
  eq.set_band(0, high_pass);

  REQUIRE(eq.latency_samples() == 512);
  REQUIRE(eq.latency_samples_q8() == (512 << 8));

  auto stop = sine(250.0f, sample_rate, sample_rate);
  auto pass = sine(8000.0f, sample_rate, sample_rate);
  const float stop_before = rms_tail(stop, 8192);
  const float pass_before = rms_tail(pass, 8192);

  process(eq, stop);
  eq.reset();
  process(eq, pass);

  REQUIRE(rms_tail(stop, 8192) / stop_before < 0.002f);
  REQUIRE(rms_tail(pass, 8192) / pass_before > 0.85f);

  EqBand low_pass{EqBandType::LowPass, 1000.0f, 0.0f, sonare::constants::kButterworthQ, true};
  low_pass.slope_db_oct = 0;
  eq.set_band(0, low_pass);

  pass = sine(100.0f, sample_rate, sample_rate);
  stop = sine(8000.0f, sample_rate, sample_rate);
  const float low_pass_before = rms_tail(pass, 8192);
  const float high_stop_before = rms_tail(stop, 8192);

  process(eq, pass);
  eq.reset();
  process(eq, stop);

  REQUIRE(rms_tail(pass, 8192) / low_pass_before > 0.85f);
  REQUIRE(rms_tail(stop, 8192) / high_stop_before < 0.002f);
}

TEST_CASE("EQ phase modes expose expected impulse timing", "[mastering][eq]") {
  constexpr int sample_rate = 48000;
  EqBand band{EqBandType::Peak, 1000.0f, 0.0f, 1.0f, true};

  ParametricEq zero;
  zero.prepare(sample_rate, 512);
  zero.set_band(0, band);
  MinimumPhaseEq natural;
  natural.prepare(sample_rate, 512);
  natural.set_band(0, band);
  LinearPhaseEq linear({1024, 257});
  linear.prepare(sample_rate, 512);
  linear.set_band(0, band);

  auto zero_impulse = std::vector<float>(512, 0.0f);
  auto natural_impulse = zero_impulse;
  auto linear_impulse = zero_impulse;
  zero_impulse[0] = 1.0f;
  natural_impulse[0] = 1.0f;
  linear_impulse[0] = 1.0f;
  process(zero, zero_impulse);
  process(natural, natural_impulse);
  process(linear, linear_impulse);

  const auto peak_index = [](const std::vector<float>& samples) {
    return static_cast<size_t>(
        std::max_element(samples.begin(), samples.end(),
                         [](float a, float b) { return std::abs(a) < std::abs(b); }) -
        samples.begin());
  };

  REQUIRE(zero.latency_samples() == 0);
  REQUIRE(natural.latency_samples() == 0);
  REQUIRE(linear.latency_samples() == 128);
  REQUIRE(peak_index(zero_impulse) == 0);
  REQUIRE(peak_index(natural_impulse) == 0);
  REQUIRE(peak_index(linear_impulse) == static_cast<size_t>(linear.latency_samples()));
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
  eq.set_band(0, {EqBandType::HighPass, 500.0f, 0.0f, kButterworthQ, true});

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
  const EqBand band{EqBandType::Peak, 1000.0f, 6.0f, kButterworthQ, true};
  eq.set_band(0, band);

  const auto coeffs = sonare::rt::rbj_peak(
      static_cast<float>(2.0 * kPiD * band.frequency_hz / sample_rate), band.q, band.gain_db);

  for (double frequency_hz : {250.0, 1000.0, 4000.0}) {
    const float expected = sonare::rt::biquad_magnitude(
        coeffs, static_cast<float>(2.0 * kPiD * frequency_hz / sample_rate));
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
  eq.set_band(0, {EqBandType::HighPass, 500.0f, 0.0f, kButterworthQ, true});

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

TEST_CASE("MinimumPhaseEq forces Vicanek natural-phase coefficients", "[mastering][eq]") {
  constexpr int sample_rate = 48000;
  const EqBand requested{EqBandType::Peak, 12000.0f, 9.0f, 0.8f, true, BiquadCoeffMode::Rbj};

  MinimumPhaseEq natural;
  natural.prepare(sample_rate, 1024);
  natural.set_band(0, requested);

  REQUIRE(natural.band(0).coeff_mode == BiquadCoeffMode::Vicanek);
  REQUIRE(natural.band(0).phase == PhaseMode::NaturalPhase);

  ParametricEq vicanek;
  vicanek.prepare(sample_rate, 1024);
  vicanek.set_band(0, {EqBandType::Peak, 12000.0f, 9.0f, 0.8f, true, BiquadCoeffMode::Vicanek});

  ParametricEq rbj;
  rbj.prepare(sample_rate, 1024);
  rbj.set_band(0, requested);

  auto natural_audio = sine(12000.0f, sample_rate, 4096);
  auto vicanek_audio = natural_audio;
  auto rbj_audio = natural_audio;
  process(natural, natural_audio);
  process(vicanek, vicanek_audio);
  process(rbj, rbj_audio);

  double natural_vs_vicanek = 0.0;
  double natural_vs_rbj = 0.0;
  for (size_t i = 0; i < natural_audio.size(); ++i) {
    natural_vs_vicanek += std::abs(static_cast<double>(natural_audio[i] - vicanek_audio[i]));
    natural_vs_rbj += std::abs(static_cast<double>(natural_audio[i] - rbj_audio[i]));
  }

  REQUIRE(natural_vs_vicanek < 1.0e-6);
  REQUIRE(natural_vs_rbj > 0.01);
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
  gentle.set_high_pass(500.0f, kButterworthQ, CutFilterSlope::Db12PerOct);
  process(gentle, low);

  CutFilter steep;
  steep.prepare(sample_rate, 1024);
  steep.set_high_pass(500.0f, kButterworthQ, CutFilterSlope::Db24PerOct);
  auto steep_low = sine(100.0f, sample_rate, sample_rate);
  process(steep, steep_low);

  CutFilter high_pass;
  high_pass.prepare(sample_rate, 1024);
  high_pass.set_high_pass(500.0f, kButterworthQ, CutFilterSlope::Db24PerOct);
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
  eq.set_low_pass(1000.0f, kButterworthQ, CutFilterSlope::Db24PerOct);

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

TEST_CASE("CutFilter supports 6 through 96 dB/oct high-pass slopes", "[mastering][eq]") {
  constexpr int sample_rate = 48000;
  constexpr int samples = sample_rate;
  const auto slopes = std::array{
      CutFilterSlope::Db6PerOct,  CutFilterSlope::Db12PerOct, CutFilterSlope::Db24PerOct,
      CutFilterSlope::Db48PerOct, CutFilterSlope::Db72PerOct, CutFilterSlope::Db96PerOct,
  };

  float previous_stop_gain = 1.0f;
  for (CutFilterSlope slope : slopes) {
    CutFilter eq;
    eq.prepare(sample_rate, 1024);
    eq.set_high_pass(1000.0f, sonare::constants::kButterworthQ, slope);

    auto stop = sine(500.0f, sample_rate, samples);
    auto pass = sine(8000.0f, sample_rate, samples);
    const float stop_before = rms_tail(stop, 4096);
    const float pass_before = rms_tail(pass, 4096);
    process(eq, stop);
    eq.reset();
    process(eq, pass);

    const float stop_gain = rms_tail(stop, 4096) / stop_before;
    const float pass_gain = rms_tail(pass, 4096) / pass_before;
    REQUIRE(stop_gain < previous_stop_gain);
    REQUIRE(pass_gain > 0.85f);
    previous_stop_gain = stop_gain;
  }
}

TEST_CASE("CutFilter supports 6 through 96 dB/oct low-pass slopes", "[mastering][eq]") {
  constexpr int sample_rate = 48000;
  constexpr int samples = sample_rate;
  const auto slopes = std::array{
      CutFilterSlope::Db6PerOct,  CutFilterSlope::Db12PerOct, CutFilterSlope::Db24PerOct,
      CutFilterSlope::Db48PerOct, CutFilterSlope::Db72PerOct, CutFilterSlope::Db96PerOct,
  };

  float previous_stop_gain = 1.0f;
  for (CutFilterSlope slope : slopes) {
    CutFilter eq;
    eq.prepare(sample_rate, 1024);
    eq.set_low_pass(1000.0f, sonare::constants::kButterworthQ, slope);

    auto pass = sine(100.0f, sample_rate, samples);
    auto stop = sine(2000.0f, sample_rate, samples);
    const float pass_before = rms_tail(pass, 4096);
    const float stop_before = rms_tail(stop, 4096);
    process(eq, pass);
    eq.reset();
    process(eq, stop);

    const float pass_gain = rms_tail(pass, 4096) / pass_before;
    const float stop_gain = rms_tail(stop, 4096) / stop_before;
    REQUIRE(stop_gain < previous_stop_gain);
    REQUIRE(pass_gain > 0.85f);
    previous_stop_gain = stop_gain;
  }
}

TEST_CASE("CutFilter applies resonance only to the final cut stage", "[mastering][eq]") {
  constexpr int sample_rate = 48000;

  CutFilter flat;
  flat.prepare(sample_rate, 1024);
  flat.set_high_pass(1000.0f, sonare::constants::kButterworthQ, CutFilterSlope::Db48PerOct);

  CutFilter resonant;
  resonant.prepare(sample_rate, 1024);
  resonant.set_high_pass(1000.0f, 2.0f, CutFilterSlope::Db48PerOct);

  auto flat_cutoff = sine(1000.0f, sample_rate, sample_rate);
  auto resonant_cutoff = flat_cutoff;
  const float before = rms_tail(flat_cutoff, 4096);
  process(flat, flat_cutoff);
  process(resonant, resonant_cutoff);

  REQUIRE(rms_tail(resonant_cutoff, 4096) / before > rms_tail(flat_cutoff, 4096) / before * 1.2f);
}

TEST_CASE("CutFilter brickwall high-pass uses linear-phase FIR latency and steep rejection",
          "[mastering][eq]") {
  constexpr int sample_rate = 48000;
  CutFilter eq;
  eq.prepare(sample_rate, 1024);
  eq.set_high_pass(1000.0f, sonare::constants::kButterworthQ, CutFilterSlope::Brickwall);

  REQUIRE(eq.latency_samples() > 0);

  auto stop = sine(250.0f, sample_rate, sample_rate);
  auto pass = sine(8000.0f, sample_rate, sample_rate);
  const float stop_before = rms_tail(stop, 8192);
  const float pass_before = rms_tail(pass, 8192);

  process(eq, stop);
  eq.reset();
  process(eq, pass);

  REQUIRE(rms_tail(stop, 8192) / stop_before < 0.002f);
  REQUIRE(rms_tail(pass, 8192) / pass_before > 0.85f);

  eq.clear_high_pass();
  REQUIRE(eq.latency_samples() == 0);
}

TEST_CASE("CutFilter brickwall low-pass uses linear-phase FIR latency and steep rejection",
          "[mastering][eq]") {
  constexpr int sample_rate = 48000;
  CutFilter eq;
  eq.prepare(sample_rate, 1024);
  eq.set_low_pass(1000.0f, sonare::constants::kButterworthQ, CutFilterSlope::Brickwall);

  REQUIRE(eq.latency_samples() > 0);

  auto pass = sine(100.0f, sample_rate, sample_rate);
  auto stop = sine(8000.0f, sample_rate, sample_rate);
  const float pass_before = rms_tail(pass, 8192);
  const float stop_before = rms_tail(stop, 8192);

  process(eq, pass);
  eq.reset();
  process(eq, stop);

  REQUIRE(rms_tail(pass, 8192) / pass_before > 0.85f);
  REQUIRE(rms_tail(stop, 8192) / stop_before < 0.002f);
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

TEST_CASE("PultecEq component model preserves channel state when channel count grows",
          "[mastering][eq]") {
  PultecEq mono_path;
  PultecEq stereo_path;
  mono_path.prepare(48000.0, 512);
  stereo_path.prepare(48000.0, 512);
  mono_path.set_component_model(PultecComponentModel::Eqp1aWdf);
  stereo_path.set_component_model(PultecComponentModel::Eqp1aWdf);
  mono_path.set_low_boost(5.0f);
  stereo_path.set_low_boost(5.0f);

  auto warmup = sine(60.0f, 48000, 4096, 0.5f);
  auto warmup_copy = warmup;
  process(mono_path, warmup);
  process(stereo_path, warmup_copy);

  auto expected_left = sine(60.0f, 48000, 512, 0.5f);
  auto actual_left = expected_left;
  auto actual_right = expected_left;
  process(mono_path, expected_left);
  process_stereo(stereo_path, actual_left, actual_right);

  REQUIRE(max_abs_difference(actual_left, expected_left) < 1.0e-6f);
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
