#include <algorithm>
#include <array>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <cmath>
#include <vector>

#include "effects/common/dc_blocker.h"
#include "effects/delay/stereo_delay.h"
#include "effects/modulation/chorus.h"
#include "effects/modulation/flanger.h"
#include "effects/modulation/lfo.h"
#include "effects/modulation/mod_delay_line.h"
#include "effects/modulation/phaser.h"
#include "effects/reverb/convolution_reverb.h"
#include "effects/reverb/dattorro_reverb.h"
#include "effects/reverb/fdn_reverb.h"
#include "effects/reverb/velvet_reverb.h"
#include "util/constants.h"

using Catch::Matchers::WithinAbs;

namespace {

bool all_finite(const std::vector<float>& samples) {
  return std::all_of(samples.begin(), samples.end(),
                     [](float sample) { return std::isfinite(sample); });
}

float peak_abs(const std::vector<float>& samples, size_t start = 0) {
  float peak = 0.0f;
  for (size_t i = std::min(start, samples.size()); i < samples.size(); ++i) {
    peak = std::max(peak, std::abs(samples[i]));
  }
  return peak;
}

// Sum of squared samples over [begin, end).
double window_energy(const std::vector<float>& samples, size_t begin, size_t end) {
  end = std::min(end, samples.size());
  double energy = 0.0;
  for (size_t i = std::min(begin, samples.size()); i < end; ++i) {
    energy += static_cast<double>(samples[i]) * static_cast<double>(samples[i]);
  }
  return energy;
}

// Energy of the first-difference (a crude high-pass) over [begin, end).
double hf_energy(const std::vector<float>& samples, size_t begin, size_t end) {
  end = std::min(end, samples.size());
  double energy = 0.0;
  for (size_t i = std::max<size_t>(begin, 1); i < end; ++i) {
    const double d = static_cast<double>(samples[i]) - static_cast<double>(samples[i - 1]);
    energy += d * d;
  }
  return energy;
}

}  // namespace

TEST_CASE("ModDelayLine returns delayed samples with interpolation", "[fx]") {
  sonare::effects::modulation::ModDelayLine delay;
  delay.prepare(8);

  REQUIRE_THAT(delay.process(1.0f, 2.0f), WithinAbs(0.0f, 0.0001f));
  REQUIRE_THAT(delay.process(0.0f, 2.0f), WithinAbs(0.0f, 0.0001f));
  REQUIRE_THAT(delay.process(0.0f, 2.0f), WithinAbs(1.0f, 0.0001f));
}

TEST_CASE("Lfo stays in bipolar range", "[fx]") {
  sonare::effects::modulation::Lfo lfo;
  lfo.prepare(100.0);
  lfo.set_rate_hz(1.0f);

  for (int i = 0; i < 200; ++i) {
    const float value = lfo.process();
    REQUIRE(value >= -1.0001f);
    REQUIRE(value <= 1.0001f);
  }
}

TEST_CASE("StereoDelay emits a delayed wet impulse", "[fx]") {
  constexpr int kSamples = 8;
  std::array<float, kSamples> left{};
  std::array<float, kSamples> right{};
  left[0] = 1.0f;
  right[0] = 1.0f;
  float* channels[] = {left.data(), right.data()};

  sonare::effects::delay::StereoDelay delay({2.0f, 2.0f, 0.0f, 0.0f, 1.0f});
  delay.prepare(1000.0, kSamples);
  delay.process(channels, 2, kSamples);

  REQUIRE_THAT(left[0], WithinAbs(0.0f, 0.0001f));
  REQUIRE_THAT(left[2], WithinAbs(1.0f, 0.0001f));
  REQUIRE_THAT(right[2], WithinAbs(1.0f, 0.0001f));
}

TEST_CASE("Modulation processors keep output finite", "[fx]") {
  std::vector<float> left(256, 0.1f);
  std::vector<float> right(256, -0.1f);
  float* channels[] = {left.data(), right.data()};

  sonare::effects::modulation::Chorus chorus;
  chorus.prepare(48000.0, static_cast<int>(left.size()));
  chorus.process(channels, 2, static_cast<int>(left.size()));
  REQUIRE(all_finite(left));
  REQUIRE(all_finite(right));

  sonare::effects::modulation::Flanger flanger;
  flanger.prepare(48000.0, static_cast<int>(left.size()));
  flanger.process(channels, 2, static_cast<int>(left.size()));
  REQUIRE(all_finite(left));
  REQUIRE(all_finite(right));

  sonare::effects::modulation::Phaser phaser;
  phaser.prepare(48000.0, static_cast<int>(left.size()));
  phaser.process(channels, 2, static_cast<int>(left.size()));
  REQUIRE(all_finite(left));
  REQUIRE(all_finite(right));
}

TEST_CASE("DattorroReverb produces a finite delayed tail", "[fx]") {
  // ~0.5 s so the tank has time to build up and then audibly decay.
  std::vector<float> left(24000, 0.0f);
  std::vector<float> right(24000, 0.0f);
  left[0] = 1.0f;
  right[0] = 1.0f;
  float* channels[] = {left.data(), right.data()};

  sonare::effects::reverb::DattorroReverb reverb({0.6f, 1.0f});
  reverb.prepare(48000.0, static_cast<int>(left.size()));
  reverb.process(channels, 2, static_cast<int>(left.size()));

  REQUIRE(all_finite(left));
  REQUIRE(all_finite(right));
  REQUIRE(reverb.latency_samples() == 0);

  // A reverb tail exists well after the input impulse.
  REQUIRE(peak_abs(left, 1500) > 1e-4f);
  REQUIRE(peak_abs(right, 1500) > 1e-4f);

  // The tail decays: a far-late window holds less energy than an early window.
  const double early = window_energy(left, 2000, 6000);
  const double late = window_energy(left, 20000, 24000);
  REQUIRE(early > 1e-6);
  REQUIRE(late < early);

  // Stereo output is decorrelated (the two channels are not identical).
  double channel_diff = 0.0;
  for (size_t i = 1000; i < left.size(); ++i) {
    channel_diff += std::abs(static_cast<double>(left[i]) - static_cast<double>(right[i]));
  }
  REQUIRE(channel_diff > 1e-3);
}

TEST_CASE("DcBlocker attenuates sustained DC", "[fx]") {
  std::vector<float> mono(2048, 1.0f);
  float* channels[] = {mono.data()};
  sonare::effects::common::DcBlocker blocker(0.99f);

  blocker.prepare(48000.0, static_cast<int>(mono.size()));
  blocker.process(channels, 1, static_cast<int>(mono.size()));

  REQUIRE(std::abs(mono.back()) < 0.01f);
}

TEST_CASE("FdnReverb produces a decaying tail and damps highs", "[fx]") {
  auto run_fdn = [](float hf_damping) {
    std::vector<float> left(8192, 0.0f);
    std::vector<float> right(8192, 0.0f);
    left[0] = 1.0f;
    right[0] = 1.0f;
    float* channels[] = {left.data(), right.data()};

    sonare::effects::reverb::FdnReverb fdn({0.6f, hf_damping, 1.0f});
    fdn.prepare(48000.0, static_cast<int>(left.size()));
    fdn.process(channels, 2, static_cast<int>(left.size()));
    return left;
  };

  const std::vector<float> light = run_fdn(0.05f);  // low HF damping
  const std::vector<float> heavy = run_fdn(0.95f);  // heavy HF damping

  REQUIRE(all_finite(light));
  REQUIRE(all_finite(heavy));
  REQUIRE(peak_abs(light, 1500) > 1e-4f);

  // The tail decays over time.
  const double early = window_energy(light, 1000, 2500);
  const double late = window_energy(light, 6000, 8000);
  REQUIRE(early > 1e-6);
  REQUIRE(late < early);

  // Heavier HF damping removes high-frequency energy from the late tail, so the
  // high-passed late-tail energy of the heavily damped run is smaller.
  const double hf_light = hf_energy(light, 4000, 8000);
  const double hf_heavy = hf_energy(heavy, 4000, 8000);
  REQUIRE(hf_heavy < hf_light);
}

TEST_CASE("VelvetReverb produces a sparse decaying tail", "[fx]") {
  std::vector<float> left(8192, 0.0f);
  std::vector<float> right(8192, 0.0f);
  left[0] = 1.0f;
  right[0] = 1.0f;
  float* channels[] = {left.data(), right.data()};

  sonare::effects::reverb::VelvetReverb velvet({0.6f, 1.0f});
  velvet.prepare(48000.0, static_cast<int>(left.size()));
  velvet.process(channels, 2, static_cast<int>(left.size()));

  REQUIRE(all_finite(left));
  REQUIRE(all_finite(right));

  // Tail present after the input impulse, and decaying over time.
  REQUIRE(peak_abs(left, 512) > 1e-4f);
  const double early = window_energy(left, 256, 2000);
  const double late = window_energy(left, 6000, 8000);
  REQUIRE(early > 1e-6);
  REQUIRE(late < early);
}

TEST_CASE("ConvolutionReverb partitioned-convolves with reported latency", "[fx]") {
  const std::vector<float> ir{0.5f, 0.25f, -0.125f, 0.0625f};
  const int ir_len = static_cast<int>(ir.size());

  sonare::effects::reverb::ConvolutionReverb convolution;
  convolution.prepare(48000.0, 64);
  convolution.load_ir(ir);
  REQUIRE(convolution.ir_size() == ir_len);

  const int latency = convolution.latency_samples();
  REQUIRE(latency > 0);

  // Input impulse at sample 0, padded well past latency + IR length so the
  // partitioned convolver fully flushes the response.
  const int total = latency + ir_len + 512;
  std::vector<float> mono(static_cast<size_t>(total), 0.0f);
  mono[0] = 1.0f;

  // Direct linear convolution of the impulse with the IR is just the IR itself.
  std::vector<float> direct(static_cast<size_t>(total), 0.0f);
  for (int n = 0; n < ir_len; ++n) {
    direct[static_cast<size_t>(n)] = ir[static_cast<size_t>(n)];
  }

  float* channels[] = {mono.data()};
  convolution.process(channels, 1, total);

  // Output equals the direct convolution delayed by the reported latency.
  for (int n = 0; n < ir_len + 8; ++n) {
    const int idx = latency + n;
    REQUIRE_THAT(mono[static_cast<size_t>(idx)], WithinAbs(direct[static_cast<size_t>(n)], 1e-4f));
  }
  // Nothing leaks out ahead of the latency.
  for (int n = 0; n < latency; ++n) {
    REQUIRE_THAT(mono[static_cast<size_t>(n)], WithinAbs(0.0f, 1e-4f));
  }
}

TEST_CASE("ConvolutionReverb dry_wet blends time-aligned dry signal", "[fx]") {
  // Delta IR (gain 1, single tap) so the wet path is an exact delayed copy of
  // the input. With a 50/50 dry/wet mix and a time-aligned dry path, the output
  // at the latency offset must equal the input scaled by (dry + wet) == 1.
  const std::vector<float> ir{1.0f};

  sonare::effects::reverb::ConvolutionReverb convolution;
  convolution.prepare(48000.0, 64);
  convolution.load_ir(ir);
  REQUIRE(convolution.set_parameter(0, 0.5f));  // dry_wet
  REQUIRE_FALSE(convolution.set_parameter(99, 0.0f));

  const int latency = convolution.latency_samples();
  const int total = latency + 256;
  std::vector<float> mono(static_cast<size_t>(total), 0.0f);
  mono[0] = 1.0f;

  float* channels[] = {mono.data()};
  convolution.process(channels, 1, total);

  REQUIRE(all_finite(mono));
  // Delta IR: dry and wet are both the input delayed by the latency, so the
  // mixed output reconstructs the original impulse at the latency offset.
  REQUIRE_THAT(mono[static_cast<size_t>(latency)], WithinAbs(1.0f, 1e-4f));
  // No dry leakage ahead of the (delayed) dry path.
  for (int n = 0; n < latency; ++n) {
    REQUIRE_THAT(mono[static_cast<size_t>(n)], WithinAbs(0.0f, 1e-4f));
  }
}

TEST_CASE("ConvolutionReverb dry_wet=0 passes the dry signal through", "[fx]") {
  const std::vector<float> ir{0.5f, 0.25f};

  sonare::effects::reverb::ConvolutionReverb convolution;
  convolution.prepare(48000.0, 64);
  convolution.load_ir(ir);
  REQUIRE(convolution.set_parameter(0, 0.0f));  // fully dry

  const int latency = convolution.latency_samples();
  const int total = latency + 256;
  std::vector<float> mono(static_cast<size_t>(total), 0.0f);
  mono[0] = 1.0f;

  float* channels[] = {mono.data()};
  convolution.process(channels, 1, total);

  REQUIRE(all_finite(mono));
  // The dry path is delayed by the same latency as the wet path so the mix stays
  // time-aligned; with no wet contribution the impulse appears at the latency.
  REQUIRE_THAT(mono[static_cast<size_t>(latency)], WithinAbs(1.0f, 1e-4f));
  // No wet tail when fully dry: the second IR tap must not appear.
  REQUIRE_THAT(mono[static_cast<size_t>(latency + 1)], WithinAbs(0.0f, 1e-4f));
}

TEST_CASE("Flanger buffer accommodates full LFO depth without plateauing", "[fx]") {
  // center + depth = 80 + 80 = 160 ms exceeds the historical 100 ms buffer; at
  // the LFO peaks the modulated read tap previously plateaued at the buffer
  // length, distorting the sweep. With correct sizing the output stays finite
  // and the per-channel decorrelation from independent LFOs is preserved.
  sonare::effects::modulation::FlangerConfig config;
  config.rate_hz = 2.0f;
  config.center_delay_ms = 80.0f;
  config.depth_ms = 80.0f;
  config.feedback = 0.0f;
  config.dry_wet = 1.0f;

  constexpr int kSampleRate = 48000;
  constexpr int kSamples = kSampleRate / 4;  // 0.25 s spans multiple LFO cycles
  std::vector<float> left(kSamples);
  std::vector<float> right(kSamples);
  const double w = 2.0 * sonare::constants::kPiD * 220.0 / static_cast<double>(kSampleRate);
  for (int i = 0; i < kSamples; ++i) {
    left[i] = static_cast<float>(std::sin(w * static_cast<double>(i)));
    right[i] = left[i];
  }
  float* channels[] = {left.data(), right.data()};

  sonare::effects::modulation::Flanger flanger(config);
  flanger.prepare(static_cast<double>(kSampleRate), kSamples);
  flanger.process(channels, 2, kSamples);

  REQUIRE(all_finite(left));
  REQUIRE(all_finite(right));

  // The two channels use phase-offset LFOs, so a correctly sized (non-clipped)
  // sweep keeps them decorrelated. A plateaued sweep would collapse the
  // difference toward zero.
  double channel_diff = 0.0;
  for (int i = kSamples / 4; i < kSamples; ++i) {
    channel_diff += std::abs(static_cast<double>(left[i]) - static_cast<double>(right[i]));
  }
  REQUIRE(channel_diff > 1e-2);
}
