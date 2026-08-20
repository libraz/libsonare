#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <cmath>
#include <limits>
#include <vector>

#include "mastering/common/hysteresis_ja.h"
#include "mastering/saturation/bitcrusher.h"
#include "mastering/saturation/exciter.h"
#include "mastering/saturation/hard_clipper.h"
#include "mastering/saturation/multiband_exciter.h"
#include "mastering/saturation/soft_clipper.h"
#include "mastering/saturation/tape.h"
#include "mastering/saturation/transformer.h"
#include "mastering/saturation/tube.h"
#include "mastering/saturation/waveshaper.h"
#include "support/audio_fixtures.h"
#include "util/constants.h"

using Catch::Matchers::WithinAbs;
using namespace sonare::mastering::saturation;

namespace {
using sonare::test::generate_sine_samples;
using sonare::test::peak_abs;
using sonare::test::process;
using sonare::test::rms_tail;

float projected_amplitude(const std::vector<float>& samples, float frequency_hz, int sample_rate,
                          size_t skip) {
  double sin_sum = 0.0;
  double cos_sum = 0.0;
  size_t count = 0;
  for (size_t i = std::min(skip, samples.size()); i < samples.size(); ++i) {
    const double phase =
        sonare::constants::kTwoPiD * frequency_hz * static_cast<double>(i) / sample_rate;
    sin_sum += static_cast<double>(samples[i]) * std::sin(phase);
    cos_sum += static_cast<double>(samples[i]) * std::cos(phase);
    ++count;
  }
  return count == 0 ? 0.0f
                    : static_cast<float>(2.0 * std::sqrt(sin_sum * sin_sum + cos_sum * cos_sum) /
                                         static_cast<double>(count));
}

/// Folds a synthesized-harmonic frequency back into [0, sample_rate/2], the
/// same reflection a real-valued sampler applies to any content above
/// Nyquist.
float fold_alias_hz(float source_hz, float sample_rate) {
  float folded = std::fmod(source_hz, sample_rate);
  if (folded < 0.0f) folded += sample_rate;
  if (folded > sample_rate * 0.5f) folded = sample_rate - folded;
  return folded;
}

struct LegacyJaState {
  float M = 0.0f;
  float H_prev = 0.0f;
};

float legacy_langevin(float x) {
  const float ax = std::abs(x);
  if (ax < 1e-4f) {
    return x * (1.0f / 3.0f - x * x / 45.0f);
  }
  return 1.0f / std::tanh(x) - 1.0f / x;
}

float legacy_langevin_derivative(float x) {
  const float ax = std::abs(x);
  if (ax < 1e-4f) {
    return 1.0f / 3.0f - x * x / 15.0f;
  }
  const float sinh_x = std::sinh(x);
  return 1.0f / (x * x) - 1.0f / (sinh_x * sinh_x);
}

/// Transcription of the DAFx-19 loop equation, frozen here so a later edit to
/// the engine alone shows up as a difference.
///
/// It is not an independent oracle, and it does not check that the scheme is
/// right. The body below is a line-for-line copy of JilesAtherton::integrate_step
/// and its Langevin helpers, down to the +-1.2*Ms clamp and to the way each
/// branch of the loop equation is integrated - the irreversible one by an Euler
/// step, the reversible one in closed form, and their sum projected onto the
/// anhysteretic curve it is chasing. What it detects is therefore divergence
/// between the two copies, not an error in the scheme itself: a change applied
/// to both sides passes unnoticed, which is the likely shape of a deliberate
/// model change. It also compares only the single-step rate-independent path: it
/// has no sub-stepping, and no after-effect relaxation, so the caller must keep
/// max_field_step at 0 and call the two-argument process() for the two sides to
/// line up at all. Neither of those paths is compared by it.
///
/// What the scheme itself is answerable to, built from the published equations
/// rather than from this code, is elsewhere: the closed-form small-field
/// susceptibility case in hysteresis_ja_reference_test.cpp, which resolves the
/// mean-field term to 1e-5 relative, and the measured coercivity, remanence,
/// loop area and susceptibility in hysteresis_ja_physical_test.cpp.
float dafx19_reference_ja_process(LegacyJaState& state,
                                  const sonare::mastering::common::JilesAthertonConfig& config,
                                  float field) {
  const float He = field + config.mean_field_coupling * state.M;
  const float x = He / config.anhysteretic_shape;
  const float M_an = config.saturation_magnetization * legacy_langevin(x);

  const float dH = field - state.H_prev;
  if (std::abs(dH) < 1e-9f) {
    state.H_prev = field;
    return state.M;
  }
  const float delta = dH >= 0.0f ? 1.0f : -1.0f;

  const float diff = M_an - state.M;
  const float delta_m = delta * diff >= 0.0f ? 1.0f : 0.0f;
  const float denom =
      (1.0f - config.reversibility) * delta * config.coercivity - config.mean_field_coupling * diff;
  float dM_hyst_dH = 0.0f;
  if (std::abs(denom) > 1e-9f) {
    dM_hyst_dH = (1.0f - config.reversibility) * delta_m * diff / denom;
  }

  const float dL = legacy_langevin_derivative(x);
  const float dM_an_dHe = config.saturation_magnetization * dL / config.anhysteretic_shape;
  const float mean_field_gain = std::max(1.0f - config.mean_field_coupling * dM_an_dHe, 1e-6f);

  const float dM_irr = dM_hyst_dH * dH;

  const float M_an_start = config.saturation_magnetization *
                           legacy_langevin((field - dH + config.mean_field_coupling * state.M) /
                                           config.anhysteretic_shape);
  const float dM_rev = config.reversibility * (M_an - M_an_start) / mean_field_gain;

  const float reach = diff / mean_field_gain;
  state.M += diff >= 0.0f ? std::min(dM_irr + dM_rev, reach) : std::max(dM_irr + dM_rev, reach);
  state.M = std::clamp(state.M, -1.2f * config.saturation_magnetization,
                       1.2f * config.saturation_magnetization);
  state.H_prev = field;
  return state.M;
}

}  // namespace

TEST_CASE("Waveshaper applies nonlinear shaping", "[mastering][saturation]") {
  Waveshaper shaper({12.0f, 1.0f, 0.0f, 0.0f, WaveshaperCurve::Tanh});
  shaper.prepare(48000.0, 128);
  std::vector<float> signal = {-0.8f, -0.2f, 0.0f, 0.2f, 0.8f};
  process(shaper, signal);
  REQUIRE(std::abs(signal.front()) < 0.999f);
  REQUIRE(signal[1] != -0.2f);
}

TEST_CASE("Waveshaper supports ADAA for tanh and arctan curves", "[mastering][saturation]") {
  Waveshaper direct({0.0f, 1.0f, 0.0f, 0.0f, WaveshaperCurve::Tanh});
  Waveshaper adaa(
      {0.0f, 1.0f, 0.0f, 0.0f, WaveshaperCurve::Tanh, sonare::rt::AliasingControl::Adaa1});
  direct.prepare(48000.0, 128);
  adaa.prepare(48000.0, 128);

  std::vector<float> direct_signal = {1.0f};
  std::vector<float> adaa_signal = direct_signal;
  process(direct, direct_signal);
  process(adaa, adaa_signal);

  REQUIRE(adaa_signal[0] < direct_signal[0]);

  adaa.set_config(
      {0.0f, 1.0f, 0.0f, 0.0f, WaveshaperCurve::Arctan, sonare::rt::AliasingControl::Adaa1});
  std::vector<float> arctan_signal = {1.0f};
  process(adaa, arctan_signal);
  REQUIRE(arctan_signal[0] > 0.0f);
  REQUIRE(arctan_signal[0] < 1.0f);
}

TEST_CASE("Waveshaper rejects ADAA1 + Asymmetric instead of silently aliasing",
          "[mastering][saturation]") {
  // The Asymmetric curve has no ADAA antiderivative and no oversampling
  // fallback, so requesting Adaa1 with it must error at construction rather than
  // silently degrade to direct (aliasing-prone) evaluation in release builds.
  REQUIRE_THROWS(Waveshaper(
      {0.0f, 1.0f, 0.0f, 0.0f, WaveshaperCurve::Asymmetric, sonare::rt::AliasingControl::Adaa1}));

  // set_config must reject the same combination on an already-constructed,
  // valid instance.
  Waveshaper shaper({0.0f, 1.0f, 0.0f, 0.0f, WaveshaperCurve::Asymmetric});
  REQUIRE_THROWS(shaper.set_config(
      {0.0f, 1.0f, 0.0f, 0.0f, WaveshaperCurve::Asymmetric, sonare::rt::AliasingControl::Adaa1}));

  // Asymmetric with no anti-aliasing (or with None) is still valid.
  REQUIRE_NOTHROW(Waveshaper(
      {0.0f, 1.0f, 0.0f, 0.0f, WaveshaperCurve::Asymmetric, sonare::rt::AliasingControl::None}));
}

TEST_CASE("Waveshaper Oversample4x suppresses high-tone alias products",
          "[mastering][saturation]") {
  for (const int sample_rate : {44100, 48000}) {
    CAPTURE(sample_rate);
    const int samples = sample_rate;
    const float input_hz = static_cast<float>(sample_rate) * 0.36f;
    const float alias_hz = fold_alias_hz(3.0f * input_hz, static_cast<float>(sample_rate));

    auto dry = generate_sine_samples(input_hz, sample_rate, samples, 0.9f);

    Waveshaper shaper({24.0f, 1.0f, 0.0f, 0.0f, WaveshaperCurve::Tanh,
                       sonare::rt::AliasingControl::Oversample4x});
    shaper.prepare(sample_rate, samples);
    auto wet = dry;
    process(shaper, wet);
    const auto latency = static_cast<size_t>(shaper.latency_samples());
    REQUIRE(latency > 0);
    for (size_t i = wet.size(); i-- > latency;) {
      wet[i] -= dry[i - latency];
    }
    std::fill(wet.begin(), wet.begin() + static_cast<std::ptrdiff_t>(latency), 0.0f);

    const float distortion_fundamental = projected_amplitude(wet, input_hz, sample_rate, 4096);
    const float alias = projected_amplitude(wet, alias_hz, sample_rate, 4096);
    const float alias_db = 20.0f * std::log10(alias / distortion_fundamental);

    CAPTURE(distortion_fundamental, alias, alias_db, alias_hz);
    REQUIRE(alias_db < -40.0f);
  }
}

TEST_CASE("Waveshaper Oversample4x supports the Asymmetric curve", "[mastering][saturation]") {
  // Oversample4x does not depend on a curve-specific ADAA antiderivative, so
  // it is not restricted the way Adaa1 is.
  REQUIRE_NOTHROW(Waveshaper({0.0f, 1.0f, 0.0f, 0.0f, WaveshaperCurve::Asymmetric,
                              sonare::rt::AliasingControl::Oversample4x}));
}

TEST_CASE("Waveshaper rejects ADAA2 instead of silently aliasing", "[mastering][saturation]") {
  REQUIRE_THROWS(Waveshaper(
      {0.0f, 1.0f, 0.0f, 0.0f, WaveshaperCurve::Tanh, sonare::rt::AliasingControl::Adaa2}));

  Waveshaper shaper({0.0f, 1.0f, 0.0f, 0.0f, WaveshaperCurve::Tanh});
  REQUIRE_THROWS(shaper.set_config(
      {0.0f, 1.0f, 0.0f, 0.0f, WaveshaperCurve::Tanh, sonare::rt::AliasingControl::Adaa2}));
}

TEST_CASE("SoftClipper and HardClipper constrain peaks", "[mastering][saturation]") {
  std::vector<float> soft = {-2.0f, -0.5f, 0.5f, 2.0f};
  std::vector<float> hard = soft;

  SoftClipper soft_clipper({12.0f, 0.75f, 1.0f});
  HardClipper hard_clipper({0.5f});
  soft_clipper.prepare(48000.0, 128);
  hard_clipper.prepare(48000.0, 128);
  process(soft_clipper, soft);
  process(hard_clipper, hard);

  REQUIRE(peak_abs(soft) <= 0.751f);
  REQUIRE_THAT(hard.front(), WithinAbs(-0.5f, 0.000001f));
  REQUIRE_THAT(hard.back(), WithinAbs(0.5f, 0.000001f));
}

TEST_CASE("SoftClipper and HardClipper support ADAA mode", "[mastering][saturation]") {
  SoftClipper soft({0.0f, 1.0f, 1.0f, sonare::rt::AliasingControl::Adaa1});
  HardClipper hard({1.0f, sonare::rt::AliasingControl::Adaa1});
  soft.prepare(48000.0, 128);
  hard.prepare(48000.0, 128);

  std::vector<float> soft_signal = {1.0f};
  std::vector<float> hard_signal = {2.0f};
  process(soft, soft_signal);
  process(hard, hard_signal);

  REQUIRE(soft_signal[0] > 0.0f);
  REQUIRE(soft_signal[0] < std::tanh(1.0f));
  REQUIRE_THAT(hard_signal[0], WithinAbs(0.75f, 0.0001f));

  hard.reset();
  std::vector<float> repeated = {2.0f};
  process(hard, repeated);
  REQUIRE_THAT(repeated[0], WithinAbs(hard_signal[0], 0.0001f));
}

TEST_CASE("HardClipper Oversample4x suppresses high-tone alias products",
          "[mastering][saturation]") {
  for (const int sample_rate : {44100, 48000}) {
    CAPTURE(sample_rate);
    const int samples = sample_rate;
    // Above a third of Nyquist so the clip's 3rd harmonic exceeds Nyquist and
    // folds back into the audible band.
    const float input_hz = static_cast<float>(sample_rate) * 0.36f;
    const float alias_hz = fold_alias_hz(3.0f * input_hz, static_cast<float>(sample_rate));

    auto dry = generate_sine_samples(input_hz, sample_rate, samples, 0.9f);

    HardClipper clipper({0.5f, sonare::rt::AliasingControl::Oversample4x});
    clipper.prepare(sample_rate, samples);
    auto wet = dry;
    process(clipper, wet);
    const auto latency = static_cast<size_t>(clipper.latency_samples());
    REQUIRE(latency > 0);
    for (size_t i = wet.size(); i-- > latency;) {
      wet[i] -= dry[i - latency];
    }
    std::fill(wet.begin(), wet.begin() + static_cast<std::ptrdiff_t>(latency), 0.0f);

    const float distortion_fundamental = projected_amplitude(wet, input_hz, sample_rate, 4096);
    const float alias = projected_amplitude(wet, alias_hz, sample_rate, 4096);
    const float alias_db = 20.0f * std::log10(alias / distortion_fundamental);

    CAPTURE(distortion_fundamental, alias, alias_db, alias_hz);
    REQUIRE(alias_db < -40.0f);
  }
}

TEST_CASE("HardClipper None mode aliases audibly before Oversample4x is applied",
          "[mastering][saturation]") {
  // Before Oversample4x was implemented it fell through to the same base-rate
  // clamp None uses, so this measurement is the level the alias assertion
  // above would have measured against the unfixed processor.
  constexpr int kSampleRate = 48000;
  constexpr int kSamples = kSampleRate;
  const float input_hz = static_cast<float>(kSampleRate) * 0.36f;
  const float alias_hz = fold_alias_hz(3.0f * input_hz, static_cast<float>(kSampleRate));

  auto dry = generate_sine_samples(input_hz, kSampleRate, kSamples, 0.9f);
  HardClipper clipper({0.5f, sonare::rt::AliasingControl::None});
  clipper.prepare(kSampleRate, kSamples);
  auto wet = dry;
  process(clipper, wet);
  for (size_t i = 0; i < wet.size(); ++i) wet[i] -= dry[i];

  const float distortion_fundamental = projected_amplitude(wet, input_hz, kSampleRate, 4096);
  const float alias = projected_amplitude(wet, alias_hz, kSampleRate, 4096);
  const float alias_db = 20.0f * std::log10(alias / distortion_fundamental);

  CAPTURE(distortion_fundamental, alias, alias_db, alias_hz);
  REQUIRE(alias_db > -20.0f);
}

TEST_CASE("SoftClipper Oversample4x suppresses high-tone alias products",
          "[mastering][saturation]") {
  for (const int sample_rate : {44100, 48000}) {
    CAPTURE(sample_rate);
    const int samples = sample_rate;
    const float input_hz = static_cast<float>(sample_rate) * 0.36f;
    const float alias_hz = fold_alias_hz(3.0f * input_hz, static_cast<float>(sample_rate));

    auto dry = generate_sine_samples(input_hz, sample_rate, samples, 0.9f);

    SoftClipper clipper({24.0f, 1.0f, 1.0f, sonare::rt::AliasingControl::Oversample4x});
    clipper.prepare(sample_rate, samples);
    auto wet = dry;
    process(clipper, wet);
    const auto latency = static_cast<size_t>(clipper.latency_samples());
    REQUIRE(latency > 0);
    for (size_t i = wet.size(); i-- > latency;) {
      wet[i] -= dry[i - latency];
    }
    std::fill(wet.begin(), wet.begin() + static_cast<std::ptrdiff_t>(latency), 0.0f);

    const float distortion_fundamental = projected_amplitude(wet, input_hz, sample_rate, 4096);
    const float alias = projected_amplitude(wet, alias_hz, sample_rate, 4096);
    const float alias_db = 20.0f * std::log10(alias / distortion_fundamental);

    CAPTURE(distortion_fundamental, alias, alias_db, alias_hz);
    REQUIRE(alias_db < -40.0f);
  }
}

TEST_CASE("SoftClipper rejects ADAA2 instead of silently aliasing", "[mastering][saturation]") {
  // tanh has no closed-form second antiderivative, so ADAA2 is not
  // implemented here; it must error at construction and set_config rather
  // than silently behave like AliasingControl::None.
  REQUIRE_THROWS(SoftClipper({0.0f, 1.0f, 1.0f, sonare::rt::AliasingControl::Adaa2}));

  SoftClipper clipper({0.0f, 1.0f, 1.0f});
  REQUIRE_THROWS(clipper.set_config({0.0f, 1.0f, 1.0f, sonare::rt::AliasingControl::Adaa2}));
}

TEST_CASE("Tube and Transformer introduce asymmetric shaping", "[mastering][saturation]") {
  std::vector<float> tube_signal = {-0.5f, 0.5f};
  std::vector<float> transformer_signal = tube_signal;

  Tube tube({12.0f, 0.2f, 1.0f, 1});
  Transformer transformer({12.0f, 0.2f, 1.0f});
  tube.prepare(48000.0, 128);
  transformer.prepare(48000.0, 128);
  process(tube, tube_signal);
  process(transformer, transformer_signal);

  REQUIRE(std::abs(tube_signal[0] + tube_signal[1]) > 0.005f);
  REQUIRE(std::abs(transformer_signal[0] + transformer_signal[1]) > 0.01f);
}

TEST_CASE("Tube uses Dempwolf 12AX7 model with configurable oversampling",
          "[mastering][saturation]") {
  Tube tube({18.0f, 0.25f, 1.0f, 4});
  tube.prepare(48000.0, 128);

  std::vector<float> signal = {-0.75f, -0.25f, 0.0f, 0.25f, 0.75f};
  signal.resize(signal.size() + static_cast<size_t>(tube.latency_samples()), 0.0f);
  process(tube, signal);

  REQUIRE(tube.latency_samples() == 12);
  signal.erase(signal.begin(), signal.begin() + tube.latency_samples());

  for (float sample : signal) REQUIRE(std::isfinite(sample));
  REQUIRE(std::abs(signal.front()) < 1.0f);
  REQUIRE(std::abs(signal.back()) < 1.0f);
  REQUIRE(std::abs(signal.front() + signal.back()) > 0.01f);

  tube.set_config({18.0f, 0.25f, 0.5f, 1});
  tube.reset();
  std::vector<float> dry_mix = {0.5f};
  process(tube, dry_mix);
  REQUIRE(dry_mix[0] > 0.0f);
  REQUIRE(dry_mix[0] < 0.5f);
}

TEST_CASE("Tube harmonic drive preserves legacy default and exposes pure model",
          "[mastering][saturation]") {
  Tube legacy_default({18.0f, 0.25f, 1.0f, 1});
  Tube explicit_legacy({18.0f, 0.25f, 1.0f, 1, -1.6f, 1.0f});
  Tube pure_model({18.0f, 0.25f, 1.0f, 1, -1.6f, 0.0f});
  legacy_default.prepare(48000.0, 16);
  explicit_legacy.prepare(48000.0, 16);
  pure_model.prepare(48000.0, 16);

  std::vector<float> default_signal = {-0.5f, 0.0f, 0.5f};
  std::vector<float> explicit_signal = default_signal;
  std::vector<float> pure_signal = default_signal;
  process(legacy_default, default_signal);
  process(explicit_legacy, explicit_signal);
  process(pure_model, pure_signal);

  for (size_t i = 0; i < default_signal.size(); ++i) {
    REQUIRE_THAT(default_signal[i], WithinAbs(explicit_signal[i], 1.0e-7f));
  }
  REQUIRE(std::abs(default_signal.front() - pure_signal.front()) > 1.0e-5f);
  REQUIRE(std::abs(default_signal.back() - pure_signal.back()) > 1.0e-5f);
  REQUIRE(std::abs(pure_signal.back()) < std::abs(default_signal.back()));
}

TEST_CASE("Tube Miller capacitance path keeps block state", "[mastering][saturation]") {
  Tube tube({24.0f, 0.2f, 1.0f, 1});
  tube.prepare(48000.0, 8);

  std::vector<float> first = {1.0f};
  std::vector<float> second = {0.0f};
  process(tube, first);
  process(tube, second);

  REQUIRE(std::isfinite(second[0]));
  REQUIRE(std::abs(second[0]) > 0.00001f);

  tube.reset();
  std::vector<float> reset_probe = {0.0f};
  process(tube, reset_probe);
  REQUIRE(std::abs(reset_probe[0]) < std::abs(second[0]));
}

TEST_CASE("Tube oversampling is invariant to process block partitioning",
          "[mastering][saturation]") {
  TubeConfig config{18.0f, 0.25f, 0.6f, 4};
  constexpr int kFrames = 2048;
  auto input = generate_sine_samples(1300.0f, 48000, kFrames, 0.7f);
  Tube one_shot(config);
  Tube partitioned(config);
  one_shot.prepare(48000.0, kFrames + one_shot.latency_samples());
  partitioned.prepare(48000.0, 128);
  REQUIRE(one_shot.latency_samples() == 12);
  REQUIRE(partitioned.latency_samples() == one_shot.latency_samples());

  input.resize(input.size() + static_cast<size_t>(one_shot.latency_samples()), 0.0f);
  auto expected = input;
  auto actual = input;
  process(one_shot, expected);
  for (size_t offset = 0; offset < actual.size(); offset += 128) {
    const int count = static_cast<int>(std::min<size_t>(128, actual.size() - offset));
    float* channels[] = {actual.data() + offset};
    partitioned.process(channels, 1, count);
  }

  REQUIRE(actual == expected);
}

TEST_CASE("Tube supports bounded offline mono and stereo scratch capacity",
          "[mastering][saturation]") {
  // An offline mastering caller that knows it is mono/stereo must not pay for
  // the oversampler scratch (up_scratch_/down_scratch_/oversampler_states_)
  // sized to the realtime channel ceiling. prepare(sr, block, 2) bounds the
  // Tube's per-channel state to exactly 2 channels; a wider block is rejected
  // rather than silently falling back to the 64-channel realtime capacity.
  Tube tube({18.0f, 0.25f, 1.0f, 4});
  tube.prepare(48000.0, 256, 2);

  std::vector<float> left(256, 0.25f);
  std::vector<float> right(256, -0.25f);
  float* stereo[] = {left.data(), right.data()};
  REQUIRE_NOTHROW(tube.process(stereo, 2, 256));

  float* too_many_channels[] = {left.data(), right.data(), left.data()};
  REQUIRE_THROWS_AS(tube.process(too_many_channels, 3, 1), sonare::SonareException);
  REQUIRE_THROWS_AS(tube.prepare(48000.0, 256, 0), sonare::SonareException);
  REQUIRE_THROWS_AS(tube.prepare(48000.0, 256, 65), sonare::SonareException);
}

TEST_CASE("Tube exposes voltage-domain bias control", "[mastering][saturation]") {
  Tube cold({18.0f, 0.0f, 1.0f, 1, -2.2f});
  Tube hot({18.0f, 0.0f, 1.0f, 1, -0.9f});
  cold.prepare(48000.0, 16);
  hot.prepare(48000.0, 16);

  std::vector<float> cold_signal = {0.5f};
  std::vector<float> hot_signal = {0.5f};
  process(cold, cold_signal);
  process(hot, hot_signal);

  REQUIRE(std::abs(hot_signal[0] - cold_signal[0]) > 0.001f);
  REQUIRE_THROWS(Tube({18.0f, 0.0f, 1.0f, 1, std::numeric_limits<float>::infinity()}));
  REQUIRE_THROWS(Tube({18.0f, 0.0f, 1.0f, 1, -1.6f, -0.1f}));
  REQUIRE_THROWS(Tube({18.0f, 0.0f, 1.0f, 1, -1.6f, 1.1f}));
}

TEST_CASE("Tube rejects non-finite grid bias and keeps output finite", "[mastering][saturation]") {
  // A non-finite grid bias feeds plate_current_ma() and would otherwise turn the
  // whole render into silent NaN, so both construction and set_parameter must
  // reject it.
  REQUIRE_THROWS(Tube({18.0f, std::numeric_limits<float>::infinity(), 1.0f, 1}));
  REQUIRE_THROWS(Tube({18.0f, std::numeric_limits<float>::quiet_NaN(), 1.0f, 1}));

  Tube tube({18.0f, 0.25f, 1.0f, 1});
  tube.prepare(48000.0, 16);

  // param_id 1 is the grid bias: non-finite requests must be rejected and must
  // leave the previously valid configuration in place.
  REQUIRE_FALSE(tube.set_parameter(1, std::numeric_limits<float>::quiet_NaN()));
  REQUIRE_FALSE(tube.set_parameter(1, std::numeric_limits<float>::infinity()));

  std::vector<float> signal = {-0.5f, -0.1f, 0.0f, 0.2f, 0.6f};
  process(tube, signal);
  for (float sample : signal) REQUIRE(std::isfinite(sample));
}

TEST_CASE("Tape saturation changes driven signal and keeps state resettable",
          "[mastering][saturation]") {
  auto signal = generate_sine_samples(1000.0f, 48000, 48000, 0.8f);
  auto first = signal;
  auto second = signal;
  Tape tape({9.0f, 0.8f, 0.2f, -3.0f});
  tape.prepare(48000.0, 1024);
  process(tape, first);
  tape.reset();
  process(tape, second);
  REQUIRE(rms_tail(first, 4096) != rms_tail(signal, 4096));
  REQUIRE_THAT(rms_tail(first, 4096), WithinAbs(rms_tail(second, 4096), 0.000001f));
}

TEST_CASE("Tape hysteresis loop depends on prior signal direction", "[mastering][saturation]") {
  // A defining property of the Jiles-Atherton model: the output at H=0 differs
  // depending on whether the field is rising or falling — the hallmark of
  // hysteresis. A memoryless saturator (tanh) cannot exhibit this property.
  Tape tape({6.0f, 0.5f, 0.9f, 0.0f});
  tape.prepare(48000.0, 1024);

  std::vector<float> ramp_up(2048, 0.0f);
  for (size_t i = 0; i < ramp_up.size(); ++i) {
    ramp_up[i] = static_cast<float>(i) / static_cast<float>(ramp_up.size() - 1);  // 0 → 1
  }
  std::vector<float> ramp_down(2048, 0.0f);
  for (size_t i = 0; i < ramp_down.size(); ++i) {
    ramp_down[i] = 1.0f - static_cast<float>(i) / static_cast<float>(ramp_down.size() - 1);
  }

  auto rising = ramp_up;
  process(tape, rising);
  const float at_top = rising.back();

  auto falling = ramp_down;
  process(tape, falling);
  const float at_bottom = falling.back();

  // Returning to H = 0 from saturation does not return to M = 0 (remanence).
  REQUIRE(std::abs(at_bottom) > 0.01f);
  REQUIRE(at_top > at_bottom);
}

TEST_CASE("Tape keeps channel state when switching from mono to stereo",
          "[mastering][saturation]") {
  Tape mono_reference({6.0f, 0.5f, 0.9f, 0.0f});
  Tape switched({6.0f, 0.5f, 0.9f, 0.0f});
  mono_reference.prepare(48000.0, 1024);
  switched.prepare(48000.0, 1024);

  std::vector<float> ramp_up(1024, 0.0f);
  for (size_t i = 0; i < ramp_up.size(); ++i) {
    ramp_up[i] = static_cast<float>(i) / static_cast<float>(ramp_up.size() - 1);
  }
  auto warm_a = ramp_up;
  auto warm_b = ramp_up;
  process(mono_reference, warm_a);
  process(switched, warm_b);

  std::vector<float> mono_probe = {0.0f};
  process(mono_reference, mono_probe);

  std::vector<float> left_probe = {0.0f};
  std::vector<float> right_probe = {0.0f};
  float* stereo_channels[] = {left_probe.data(), right_probe.data()};
  switched.process(stereo_channels, 2, 1);

  REQUIRE_THAT(left_probe[0], WithinAbs(mono_probe[0], 1e-7f));
  REQUIRE(std::abs(left_probe[0]) > 0.01f);
}

TEST_CASE("Shared Jiles-Atherton engine matches DAFx-19 reference equation",
          "[mastering][saturation]") {
  namespace common = sonare::mastering::common;

  const common::JilesAthertonConfig config{1.0f, 0.5f - 0.4f * 0.65f, 0.05f + 0.30f * 0.75f,
                                           1.6e-3f, 0.4f};
  common::JilesAtherton engine(config);
  common::JilesAthertonState shared_state;
  LegacyJaState legacy_state;

  const std::vector<float> fields = {0.0f,  0.01f, 0.05f,  0.18f, 0.45f, 0.9f,
                                     0.35f, -0.2f, -0.75f, -0.1f, 0.25f};
  for (float field : fields) {
    const float expected = dafx19_reference_ja_process(legacy_state, config, field);
    const float actual = engine.process(shared_state, field);
    REQUIRE_THAT(actual, WithinAbs(expected, 1e-7f));
  }

  common::JilesAtherton::reset(shared_state);
  REQUIRE_THAT(shared_state.magnetization, WithinAbs(0.0f, 0.0f));
  REQUIRE_THAT(shared_state.previous_field, WithinAbs(0.0f, 0.0f));
}

TEST_CASE("Jiles-Atherton presets expose tape, steel, and mu-metal cores",
          "[mastering][saturation]") {
  namespace presets = sonare::mastering::common::jiles_atherton_presets;
  namespace plan_presets = sonare::mastering::common::presets;

  const auto oxide = presets::oxide_tape();
  const auto tape = presets::tape();
  const auto steel = presets::silicon_steel();
  const auto mu_metal = presets::mu_metal();
  const sonare::mastering::common::JaParams plan_params = plan_presets::oxide_tape();
  sonare::mastering::common::HysteresisJa plan_engine(plan_params);
  sonare::mastering::common::JilesAthertonState plan_state;

  REQUIRE_THAT(tape.coercivity, WithinAbs(oxide.coercivity, 0.0f));
  REQUIRE(std::isfinite(plan_engine.process(plan_state, 0.1f)));
  REQUIRE(steel.coercivity > mu_metal.coercivity);
  REQUIRE(mu_metal.reversibility > steel.reversibility);
}

TEST_CASE("Tape speed controls head bump and gap loss", "[mastering][saturation]") {
  auto slow = generate_sine_samples(160.0f, 48000, 48000, 0.2f);
  auto fast = slow;
  Tape slow_tape({6.0f, 0.4f, 0.3f, 0.0f, 7.5f, 9.0f, 0.05f, 0.3f});
  Tape fast_tape({6.0f, 0.4f, 0.3f, 0.0f, 30.0f, 9.0f, 0.05f, 0.3f});
  slow_tape.prepare(48000.0, 1024);
  fast_tape.prepare(48000.0, 1024);
  process(slow_tape, slow);
  process(fast_tape, fast);

  REQUIRE(std::abs(rms_tail(slow, 4096) - rms_tail(fast, 4096)) > 0.0005f);
}

TEST_CASE("Transformer uses stateful Jiles-Atherton hysteresis", "[mastering][saturation]") {
  Transformer transformer({9.0f, 0.25f, 1.0f});
  transformer.prepare(48000.0, 1024);

  std::vector<float> ramp_up(1024, 0.0f);
  std::vector<float> ramp_down(1024, 0.0f);
  for (size_t i = 0; i < ramp_up.size(); ++i) {
    const float t = static_cast<float>(i) / static_cast<float>(ramp_up.size() - 1);
    ramp_up[i] = t;
    ramp_down[i] = 1.0f - t;
  }

  process(transformer, ramp_up);
  process(transformer, ramp_down);

  REQUIRE(std::abs(ramp_down.back()) > 0.001f);

  const auto first = ramp_up;
  transformer.reset();
  auto repeated = std::vector<float>(1024, 0.0f);
  for (size_t i = 0; i < repeated.size(); ++i) {
    repeated[i] = static_cast<float>(i) / static_cast<float>(repeated.size() - 1);
  }
  process(transformer, repeated);

  REQUIRE_THAT(repeated.back(), WithinAbs(first.back(), 0.000001f));
}

TEST_CASE("BitCrusher quantizes and holds samples", "[mastering][saturation]") {
  std::vector<float> signal = {0.1f, 0.2f, 0.3f, 0.4f};
  BitCrusher crusher({4, 2, 1.0f});
  crusher.prepare(48000.0, 128);
  process(crusher, signal);
  REQUIRE_THAT(signal[1], WithinAbs(signal[0], 0.000001f));
  REQUIRE_THAT(signal[3], WithinAbs(signal[2], 0.000001f));
}

TEST_CASE("BitCrusher can apply deterministic dither before quantization",
          "[mastering][saturation]") {
  std::vector<float> a(64, 0.0f);
  std::vector<float> b(64, 0.0f);
  BitCrusher first({2, 1, 1.0f, sonare::mastering::final::DitherType::Tpdf, 1234});
  BitCrusher second({2, 1, 1.0f, sonare::mastering::final::DitherType::Tpdf, 1234});
  first.prepare(48000.0, 64);
  second.prepare(48000.0, 64);

  process(first, a);
  process(second, b);

  REQUIRE(a == b);
  REQUIRE(std::any_of(a.begin(), a.end(), [](float sample) { return sample != 0.0f; }));
}

TEST_CASE("Exciter adds high-frequency enhancement", "[mastering][saturation]") {
  auto signal = generate_sine_samples(8000.0f, 48000, 48000, 0.2f);
  const float before = rms_tail(signal, 4096);
  Exciter exciter({3000.0f, 12.0f, 0.5f});
  exciter.prepare(48000.0, 1024);
  process(exciter, signal);
  REQUIRE(rms_tail(signal, 4096) > before * 1.1f);
}

TEST_CASE("Exciter focuses harmonic generation around resonant band", "[mastering][saturation]") {
  auto center = generate_sine_samples(4000.0f, 48000, 48000, 0.2f);
  auto low = generate_sine_samples(300.0f, 48000, 48000, 0.2f);
  const float center_before = rms_tail(center, 4096);
  const float low_before = rms_tail(low, 4096);
  Exciter exciter({4000.0f, 12.0f, 0.7f, 2.0f, 0.2f});
  exciter.prepare(48000.0, 1024);
  process(exciter, center);
  exciter.reset();
  process(exciter, low);

  REQUIRE(rms_tail(center, 4096) / center_before > rms_tail(low, 4096) / low_before);
}

TEST_CASE("Exciter even branch generates a second harmonic without DC", "[mastering][saturation]") {
  constexpr int kSampleRate = 48000;
  constexpr int kSamples = 48000;
  constexpr float kFundamentalHz = 1000.0f;
  auto signal = generate_sine_samples(kFundamentalHz, kSampleRate, kSamples, 0.25f);

  Exciter exciter({kFundamentalHz, 12.0f, 1.0f, 1.0f, 0.0f});
  exciter.prepare(kSampleRate, kSamples);
  process(exciter, signal);

  constexpr size_t kSkip = 4800;
  const float fundamental = projected_amplitude(signal, kFundamentalHz, kSampleRate, kSkip);
  const float second = projected_amplitude(signal, 2.0f * kFundamentalHz, kSampleRate, kSkip);
  const float third = projected_amplitude(signal, 3.0f * kFundamentalHz, kSampleRate, kSkip);
  double dc = 0.0;
  for (size_t i = kSkip; i < signal.size(); ++i) dc += signal[i];
  dc /= static_cast<double>(signal.size() - kSkip);

  CAPTURE(fundamental, second, third, dc);
  REQUIRE(second > fundamental * 0.05f);
  REQUIRE(second > third * 10.0f);
  REQUIRE(std::abs(dc) < 1.0e-4);
}

TEST_CASE("Exciter set_config preserves filter history", "[mastering][saturation]") {
  Exciter exciter({1000.0f, 12.0f, 0.0f});
  exciter.prepare(1000.0, 128);

  std::vector<float> settle(128, 1.0f);
  process(exciter, settle);

  exciter.set_config({1000.0f, 12.0f, 1.0f});
  std::vector<float> after_config = {1.0f};
  process(exciter, after_config);

  REQUIRE_THAT(after_config[0], WithinAbs(1.0f, 0.1f));
}

TEST_CASE("Exciter Oversample4x suppresses high-tone alias products", "[mastering][saturation]") {
  for (const int sample_rate : {44100, 48000}) {
    CAPTURE(sample_rate);
    const int samples = sample_rate;
    const float input_hz = static_cast<float>(sample_rate) * 0.36f;
    const float alias_hz = fold_alias_hz(3.0f * input_hz, static_cast<float>(sample_rate));

    auto dry = generate_sine_samples(input_hz, sample_rate, samples, 0.5f);

    Exciter exciter({input_hz, 24.0f, 1.0f, 1.0f, 1.0f, sonare::rt::AliasingControl::Oversample4x});
    exciter.prepare(sample_rate, samples);
    auto wet = dry;
    process(exciter, wet);
    const auto latency = static_cast<size_t>(exciter.latency_samples());
    REQUIRE(latency > 0);
    for (size_t i = wet.size(); i-- > latency;) {
      wet[i] -= dry[i - latency];
    }
    std::fill(wet.begin(), wet.begin() + static_cast<std::ptrdiff_t>(latency), 0.0f);

    const float distortion_fundamental = projected_amplitude(wet, input_hz, sample_rate, 4096);
    const float alias = projected_amplitude(wet, alias_hz, sample_rate, 4096);
    const float alias_db = 20.0f * std::log10(alias / distortion_fundamental);

    CAPTURE(distortion_fundamental, alias, alias_db, alias_hz);
    REQUIRE(alias_db < -40.0f);
  }
}

TEST_CASE("Exciter None mode aliases audibly before Oversample4x is applied",
          "[mastering][saturation]") {
  // Before Oversample4x was implemented it fell through to the same base-rate
  // tanh None uses, so this measurement is the level the alias assertion
  // above would have measured against the unfixed processor.
  constexpr int kSampleRate = 48000;
  constexpr int kSamples = kSampleRate;
  const float input_hz = static_cast<float>(kSampleRate) * 0.36f;
  const float alias_hz = fold_alias_hz(3.0f * input_hz, static_cast<float>(kSampleRate));

  auto dry = generate_sine_samples(input_hz, kSampleRate, kSamples, 0.5f);
  Exciter exciter({input_hz, 24.0f, 1.0f, 1.0f, 1.0f});
  exciter.prepare(kSampleRate, kSamples);
  auto wet = dry;
  process(exciter, wet);
  for (size_t i = 0; i < wet.size(); ++i) wet[i] -= dry[i];

  const float distortion_fundamental = projected_amplitude(wet, input_hz, kSampleRate, 4096);
  const float alias = projected_amplitude(wet, alias_hz, kSampleRate, 4096);
  const float alias_db = 20.0f * std::log10(alias / distortion_fundamental);

  CAPTURE(distortion_fundamental, alias, alias_db, alias_hz);
  REQUIRE(alias_db > -20.0f);
}

TEST_CASE("Exciter rejects ADAA anti-aliasing instead of silently aliasing",
          "[mastering][saturation]") {
  // The harmonic generator mixes squaring and tanh with no ADAA
  // antiderivative wired up here, so only None and Oversample4x are valid.
  REQUIRE_THROWS(Exciter({3000.0f, 6.0f, 0.25f, 1.0f, 0.5f, sonare::rt::AliasingControl::Adaa1}));
  REQUIRE_THROWS(Exciter({3000.0f, 6.0f, 0.25f, 1.0f, 0.5f, sonare::rt::AliasingControl::Adaa2}));

  Exciter exciter({3000.0f, 6.0f, 0.25f});
  REQUIRE_THROWS(
      exciter.set_config({3000.0f, 6.0f, 0.25f, 1.0f, 0.5f, sonare::rt::AliasingControl::Adaa1}));
}

TEST_CASE("MultibandExciter can enhance high band while leaving low band close",
          "[mastering][saturation]") {
  MultibandExciterConfig config;
  config.crossover = {{1000.0f},
                      sonare::mastering::multiband::CrossoverSlope::LR2,
                      sonare::mastering::multiband::CrossoverMode::LinkwitzRiley};
  config.bands = {{3000.0f, 0.0f, 0.0f}, {3000.0f, 12.0f, 0.5f}};
  MultibandExciter exciter(config);
  exciter.prepare(48000.0, 48000);

  auto low = generate_sine_samples(100.0f, 48000, 48000, 0.2f);
  auto high = generate_sine_samples(8000.0f, 48000, 48000, 0.2f);
  const float low_before = rms_tail(low, 4096);
  const float high_before = rms_tail(high, 4096);
  process(exciter, low);
  exciter.reset();
  process(exciter, high);
  REQUIRE(rms_tail(low, 4096) > low_before * 0.9f);
  REQUIRE(rms_tail(high, 4096) > high_before * 1.05f);
}

TEST_CASE("MultibandExciter reports crossover latency for FIR mode", "[mastering][saturation]") {
  MultibandExciterConfig config;
  config.crossover = {{1000.0f},
                      sonare::mastering::multiband::CrossoverSlope::LR4,
                      sonare::mastering::multiband::CrossoverMode::FirLinearPhase,
                      257};
  config.bands = {{3000.0f, 0.0f, 0.0f}, {3000.0f, 6.0f, 0.5f}};
  MultibandExciter exciter(config);
  exciter.prepare(48000.0, 1024);
  // FIR linear-phase crossover introduces kernel/2 latency, which must be
  // reported for host PDC (matching the other multiband processors).
  REQUIRE(exciter.latency_samples() == 257 / 2);

  // IIR crossover mode has no reported latency.
  MultibandExciterConfig iir;
  iir.crossover = {{1000.0f},
                   sonare::mastering::multiband::CrossoverSlope::LR2,
                   sonare::mastering::multiband::CrossoverMode::LinkwitzRiley};
  iir.bands = {{3000.0f, 0.0f, 0.0f}, {3000.0f, 6.0f, 0.5f}};
  MultibandExciter iir_exciter(iir);
  iir_exciter.prepare(48000.0, 1024);
  REQUIRE(iir_exciter.latency_samples() == 0);
}

TEST_CASE("Saturation processors validate configurations", "[mastering][saturation]") {
  REQUIRE_THROWS(Waveshaper({0.0f, -0.1f, 0.0f, 0.0f, WaveshaperCurve::Tanh}));
  REQUIRE_THROWS(SoftClipper({0.0f, 0.0f, 1.0f}));
  REQUIRE_THROWS(HardClipper({0.0f}));
  REQUIRE_THROWS(Tube({0.0f, 0.0f, 1.5f}));
  REQUIRE_THROWS(Tube({0.0f, 0.0f, 1.0f, 3}));
  REQUIRE_THROWS(Tape({0.0f, -0.1f, 0.2f, 0.0f}));
  REQUIRE_THROWS(Transformer({0.0f, 0.0f, -0.1f}));
  REQUIRE_THROWS(BitCrusher({0, 1, 1.0f}));
  REQUIRE_THROWS(BitCrusher({12, 1, 1.1f}));
  REQUIRE_THROWS(Exciter({0.0f, 0.0f, 0.1f}));
  MultibandExciterConfig config;
  config.bands.resize(1);
  REQUIRE_THROWS(MultibandExciter(config));
}
