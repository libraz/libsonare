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
  const float dM_an_dH = dM_an_dHe / std::max(1.0f - config.mean_field_coupling * dM_an_dHe, 1e-6f);
  const float dM_dH = dM_hyst_dH + config.reversibility * dM_an_dH;

  state.M += dM_dH * dH;
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
  process(tube, signal);

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

TEST_CASE("MultibandExciter can enhance high band while leaving low band close",
          "[mastering][saturation]") {
  MultibandExciterConfig config;
  config.crossover = {{1000.0f},
                      sonare::mastering::multiband::CrossoverSlope::LR2,
                      sonare::mastering::multiband::CrossoverMode::LinkwitzRiley};
  config.bands = {{3000.0f, 0.0f, 0.0f}, {3000.0f, 12.0f, 0.5f}};
  MultibandExciter exciter(config);
  exciter.prepare(48000.0, 1024);

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
