#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <cmath>
#include <limits>
#include <vector>

#include "mastering/dynamics/brickwall_limiter.h"
#include "mastering/dynamics/compressor.h"
#include "mastering/dynamics/deesser.h"
#include "mastering/dynamics/expander.h"
#include "mastering/dynamics/gate.h"
#include "mastering/dynamics/limiter.h"
#include "mastering/dynamics/parallel_comp.h"
#include "mastering/dynamics/sidechain_router.h"
#include "mastering/dynamics/transient_shaper.h"
#include "mastering/dynamics/upward_compressor.h"
#include "mastering/dynamics/upward_expander.h"
#include "mastering/dynamics/vocal_rider.h"

using Catch::Matchers::WithinAbs;
using namespace sonare::mastering::dynamics;

namespace {

constexpr double kPi = 3.14159265358979323846;

std::vector<float> sine(float frequency_hz, int sample_rate, int samples, float amplitude) {
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

float peak_abs(const std::vector<float>& samples, size_t skip = 0) {
  float peak = 0.0f;
  for (size_t i = std::min(skip, samples.size()); i < samples.size(); ++i) {
    peak = std::max(peak, std::abs(samples[i]));
  }
  return peak;
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

TEST_CASE("Compressor reduces level above threshold", "[mastering][dynamics]") {
  Compressor compressor({-18.0f, 4.0f, 0.0f, 20.0f, 0.0f, 0.0f, false, DetectorMode::Rms});
  compressor.prepare(48000.0, 1024);

  auto quiet = sine(1000.0f, 48000, 48000, 0.05f);
  auto loud = sine(1000.0f, 48000, 48000, 0.8f);
  const float quiet_before = rms_tail(quiet, 4096);
  const float loud_before = rms_tail(loud, 4096);

  process(compressor, quiet);
  compressor.reset();
  process(compressor, loud);

  REQUIRE(rms_tail(quiet, 4096) / quiet_before > 0.95f);
  REQUIRE(rms_tail(loud, 4096) / loud_before < 0.55f);
  REQUIRE(compressor.last_gain_reduction_db() < -4.0f);
}

TEST_CASE("Compressor auto makeup increases compressed output", "[mastering][dynamics]") {
  auto input = sine(1000.0f, 48000, 48000, 0.8f);
  auto no_makeup = input;
  auto with_makeup = input;

  Compressor dry({-18.0f, 4.0f, 0.0f, 20.0f, 0.0f, 0.0f, false, DetectorMode::Rms});
  Compressor makeup({-18.0f, 4.0f, 0.0f, 20.0f, 0.0f, 0.0f, true, DetectorMode::Rms});
  dry.prepare(48000.0, 1024);
  makeup.prepare(48000.0, 1024);

  process(dry, no_makeup);
  process(makeup, with_makeup);

  REQUIRE(rms_tail(with_makeup, 4096) > rms_tail(no_makeup, 4096) * 1.5f);
}

TEST_CASE("Compressor validates configuration", "[mastering][dynamics]") {
  REQUIRE_THROWS(Compressor({-18.0f, 0.5f, 10.0f, 100.0f, 0.0f, 0.0f, false, DetectorMode::Rms}));
  REQUIRE_THROWS(Compressor({-18.0f, 2.0f, -1.0f, 100.0f, 0.0f, 0.0f, false, DetectorMode::Rms}));
}

TEST_CASE("Compressor LogRms detector compresses loud sustained tones", "[mastering][dynamics]") {
  Compressor compressor({-18.0f, 4.0f, 10.0f, 50.0f, 0.0f, 0.0f, false, DetectorMode::LogRms});
  compressor.prepare(48000.0, 1024);

  auto loud = sine(1000.0f, 48000, 48000, 0.8f);
  const float before = rms_tail(loud, 4096);
  process(compressor, loud);

  REQUIRE(rms_tail(loud, 4096) / before < 0.55f);
  REQUIRE(compressor.last_gain_reduction_db() < -4.0f);
}

TEST_CASE("Compressor links detection across stereo channels", "[mastering][dynamics]") {
  Compressor compressor({-18.0f, 4.0f, 10.0f, 50.0f, 0.0f, 0.0f, false, DetectorMode::Peak});
  compressor.prepare(48000.0, 1024);

  auto left = sine(1000.0f, 48000, 48000, 0.8f);
  auto right = sine(1000.0f, 48000, 48000, 0.02f);
  float* channels[] = {left.data(), right.data()};
  compressor.process(channels, 2, static_cast<int>(left.size()));

  // The quiet channel must be attenuated because the loud channel drove the
  // detector — this is the defining property of linked stereo detection.
  REQUIRE(rms_tail(right, 4096) < 0.018f);
}

TEST_CASE("Compressor sidechain HPF ignores low-frequency detector energy",
          "[mastering][dynamics]") {
  auto input = sine(40.0f, 48000, 48000, 0.8f);
  auto full_band = input;
  auto hpf_keyed = input;

  Compressor full({-24.0f, 6.0f, 0.0f, 20.0f, 0.0f, 0.0f, false, DetectorMode::Peak});
  Compressor hpf({-24.0f, 6.0f, 0.0f, 20.0f, 0.0f, 0.0f, false, DetectorMode::Peak, true,
                  120.0f});
  full.prepare(48000.0, 1024);
  hpf.prepare(48000.0, 1024);

  process(full, full_band);
  process(hpf, hpf_keyed);

  REQUIRE(rms_tail(hpf_keyed, 4096) > rms_tail(full_band, 4096) * 1.5f);
}

TEST_CASE("Limiter delays audio and limits peaks", "[mastering][dynamics]") {
  Limiter limiter({-6.0f, 1.0f, 5.0f});
  limiter.prepare(1000.0, 128);

  std::vector<float> impulse(16, 0.0f);
  impulse[0] = 1.0f;

  process(limiter, impulse);

  REQUIRE(limiter.latency_samples() == 1);
  REQUIRE_THAT(impulse[0], WithinAbs(0.0f, 0.0001f));
  REQUIRE(peak_abs(impulse, 1) <= 0.502f);
  REQUIRE(limiter.last_gain_reduction_db() < -5.5f);
}

TEST_CASE("Limiter passes signal below threshold after latency", "[mastering][dynamics]") {
  Limiter limiter({-1.0f, 1.0f, 0.0f});
  limiter.prepare(1000.0, 128);

  std::vector<float> signal = {0.25f, -0.25f, 0.2f, -0.2f};
  process(limiter, signal);

  REQUIRE_THAT(signal[1], WithinAbs(0.25f, 0.0001f));
  REQUIRE_THAT(signal[2], WithinAbs(-0.25f, 0.0001f));
  REQUIRE_THAT(limiter.last_gain_reduction_db(), WithinAbs(0.0f, 0.0001f));
}

TEST_CASE("Limiter validates configuration", "[mastering][dynamics]") {
  REQUIRE_THROWS(Limiter({-1.0f, -1.0f, 10.0f}));
  REQUIRE_THROWS(Limiter({-1.0f, 1.0f, -10.0f}));
}

TEST_CASE("BrickwallLimiter guarantees ceiling", "[mastering][dynamics]") {
  BrickwallLimiter limiter({-6.0f, 0.0f, 0.0f});
  limiter.prepare(48000.0, 128);

  std::vector<float> signal = {0.0f, 0.25f, -0.75f, 1.0f, -1.0f, 0.1f};
  process(limiter, signal);

  REQUIRE(peak_abs(signal) <= 0.502f);
  REQUIRE(limiter.last_gain_reduction_db() < -5.5f);
  REQUIRE(limiter.latency_samples() == 0);
  REQUIRE(limiter.hard_clip_count() == 0);
}

TEST_CASE("BrickwallLimiter sanitizes non-finite output before the hard ceiling",
          "[mastering][dynamics]") {
  BrickwallLimiter limiter({-6.0f, 0.0f, 0.0f});
  limiter.prepare(48000.0, 128);

  std::vector<float> signal = {std::numeric_limits<float>::infinity(),
                               -std::numeric_limits<float>::infinity(),
                               std::numeric_limits<float>::quiet_NaN(), 2.0f};
  process(limiter, signal);

  for (float sample : signal) {
    REQUIRE(std::isfinite(sample));
    REQUIRE(std::abs(sample) <= 0.502f);
  }
  REQUIRE_THAT(signal[2], WithinAbs(0.0f, 0.0001f));
  REQUIRE(limiter.last_gain_reduction_db() <= -120.0f);
  REQUIRE(limiter.hard_clip_count() == 3);
}

TEST_CASE("BrickwallLimiter validates configuration", "[mastering][dynamics]") {
  REQUIRE_THROWS(BrickwallLimiter({-1.0f, -1.0f, 10.0f}));
  REQUIRE_THROWS(BrickwallLimiter({-1.0f, 1.0f, -10.0f}));
  REQUIRE_THROWS(BrickwallLimiter({std::numeric_limits<float>::infinity(), 1.0f, 10.0f}));
}

TEST_CASE("Expander reduces signal below threshold and preserves louder signal",
          "[mastering][dynamics]") {
  Expander expander({-30.0f, 2.0f, 0.0f, 20.0f, -40.0f});
  expander.prepare(48000.0, 1024);

  std::vector<float> quiet(48000, 0.01f);
  std::vector<float> loud(48000, 0.5f);
  const float quiet_before = rms_tail(quiet, 4096);
  const float loud_before = rms_tail(loud, 4096);

  process(expander, quiet);
  expander.reset();
  process(expander, loud);

  REQUIRE(rms_tail(quiet, 4096) / quiet_before < 0.35f);
  REQUIRE(rms_tail(loud, 4096) / loud_before > 0.95f);
  REQUIRE(expander.last_gain_reduction_db() == 0.0f);
}

TEST_CASE("Expander validates configuration", "[mastering][dynamics]") {
  REQUIRE_THROWS(Expander({-40.0f, 0.5f, 5.0f, 100.0f, -40.0f}));
  REQUIRE_THROWS(Expander({-40.0f, 2.0f, -1.0f, 100.0f, -40.0f}));
  REQUIRE_THROWS(Expander({-40.0f, 2.0f, 5.0f, 100.0f, 1.0f}));
}

TEST_CASE("Gate strongly attenuates noise below threshold", "[mastering][dynamics]") {
  Gate gate({-35.0f, 0.0f, 20.0f, -70.0f});
  gate.prepare(48000.0, 1024);

  std::vector<float> noise(48000, 0.005f);
  std::vector<float> signal(48000, 0.4f);
  const float noise_before = rms_tail(noise, 4096);
  const float signal_before = rms_tail(signal, 4096);

  process(gate, noise);
  gate.reset();
  process(gate, signal);

  REQUIRE(rms_tail(noise, 4096) / noise_before < 0.05f);
  REQUIRE(rms_tail(signal, 4096) / signal_before > 0.95f);
}

TEST_CASE("Gate hold and hysteresis keep the gate open briefly", "[mastering][dynamics]") {
  Gate gate({-30.0f, 0.0f, 0.0f, -80.0f, 10.0f, -40.0f, 0.0f});
  gate.prepare(1000.0, 64);

  std::vector<float> signal(32, 0.001f);
  signal[0] = 0.5f;
  process(gate, signal);

  REQUIRE(signal[1] > 0.0008f);
  REQUIRE(std::abs(signal.back()) < 0.0002f);
}

TEST_CASE("Gate validates configuration", "[mastering][dynamics]") {
  REQUIRE_THROWS(Gate({-40.0f, -1.0f, 100.0f, -80.0f}));
  REQUIRE_THROWS(Gate({-40.0f, 1.0f, -100.0f, -80.0f}));
  REQUIRE_THROWS(Gate({-40.0f, 1.0f, 100.0f, 1.0f}));
}

TEST_CASE("UpwardCompressor raises quiet signal below threshold", "[mastering][dynamics]") {
  UpwardCompressor upward({-20.0f, 2.0f, 0.0f, 20.0f, 12.0f});
  upward.prepare(48000.0, 1024);

  std::vector<float> quiet(48000, 0.02f);
  std::vector<float> loud(48000, 0.5f);
  const float quiet_before = rms_tail(quiet, 4096);
  const float loud_before = rms_tail(loud, 4096);

  process(upward, quiet);
  upward.reset();
  process(upward, loud);

  REQUIRE(rms_tail(quiet, 4096) / quiet_before > 2.0f);
  REQUIRE(rms_tail(loud, 4096) / loud_before < 1.05f);
  REQUIRE(upward.last_gain_db() == 0.0f);
}

TEST_CASE("UpwardCompressor validates configuration", "[mastering][dynamics]") {
  REQUIRE_THROWS(UpwardCompressor({-24.0f, 0.5f, 10.0f, 100.0f, 12.0f}));
  REQUIRE_THROWS(UpwardCompressor({-24.0f, 2.0f, -1.0f, 100.0f, 12.0f}));
  REQUIRE_THROWS(UpwardCompressor({-24.0f, 2.0f, 10.0f, 100.0f, -1.0f}));
}

TEST_CASE("UpwardExpander raises signal above threshold and leaves quiet signal alone",
          "[mastering][dynamics]") {
  UpwardExpander upward({-24.0f, 1.5f, 0.0f, 20.0f, 12.0f});
  upward.prepare(48000.0, 1024);

  std::vector<float> quiet(48000, 0.02f);
  std::vector<float> loud(48000, 0.5f);
  const float quiet_before = rms_tail(quiet, 4096);
  const float loud_before = rms_tail(loud, 4096);

  process(upward, quiet);
  upward.reset();
  process(upward, loud);

  REQUIRE(rms_tail(quiet, 4096) / quiet_before < 1.05f);
  REQUIRE(rms_tail(loud, 4096) / loud_before > 1.4f);
  REQUIRE(upward.last_gain_db() > 3.0f);
}

TEST_CASE("UpwardExpander validates configuration", "[mastering][dynamics]") {
  REQUIRE_THROWS(UpwardExpander({-24.0f, 0.5f, 10.0f, 100.0f, 12.0f}));
  REQUIRE_THROWS(UpwardExpander({-24.0f, 2.0f, -1.0f, 100.0f, 12.0f}));
  REQUIRE_THROWS(UpwardExpander({-24.0f, 2.0f, 10.0f, 100.0f, -1.0f}));
}

TEST_CASE("DeEsser attenuates sibilant high band more than low band", "[mastering][dynamics]") {
  DeEsser deesser({5000.0f, -28.0f, 6.0f, 0.0f, 20.0f, 18.0f});
  deesser.prepare(48000.0, 1024);

  auto high = sine(8000.0f, 48000, 48000, 0.5f);
  auto low = sine(1000.0f, 48000, 48000, 0.5f);
  const float high_before = rms_tail(high, 4096);
  const float low_before = rms_tail(low, 4096);

  process(deesser, high);
  const float high_reduction_db = deesser.last_gain_reduction_db();
  deesser.reset();
  process(deesser, low);

  REQUIRE(rms_tail(high, 4096) / high_before < 0.65f);
  REQUIRE(rms_tail(low, 4096) / low_before > 0.75f);
  REQUIRE(high_reduction_db < -1.0f);
}

TEST_CASE("DeEsser validates configuration", "[mastering][dynamics]") {
  REQUIRE_THROWS(DeEsser({0.0f, -24.0f, 4.0f, 1.0f, 60.0f, 12.0f}));
  REQUIRE_THROWS(DeEsser({6000.0f, -24.0f, 0.5f, 1.0f, 60.0f, 12.0f}));
  REQUIRE_THROWS(DeEsser({6000.0f, -24.0f, 4.0f, -1.0f, 60.0f, 12.0f}));
  REQUIRE_THROWS(DeEsser({6000.0f, -24.0f, 4.0f, 1.0f, 60.0f, -1.0f}));
}

TEST_CASE("TransientShaper boosts attacks and can reduce sustain", "[mastering][dynamics]") {
  std::vector<float> impulse(48000, 0.0f);
  impulse[0] = 0.5f;

  auto attack = impulse;
  TransientShaper attack_shaper({6.0f, 0.0f, 0.0f, 20.0f, 20.0f, 200.0f, 1.0f, 12.0f});
  attack_shaper.prepare(48000.0, 1024);
  process(attack_shaper, attack);

  REQUIRE(peak_abs(attack) > peak_abs(impulse) * 1.5f);
  REQUIRE(attack_shaper.last_gain_db() > 4.0f);

  std::vector<float> tail(48000);
  for (size_t i = 0; i < tail.size(); ++i) {
    tail[i] = 0.5f * std::exp(-static_cast<float>(i) / 8000.0f);
  }
  auto shaped_tail = tail;

  TransientShaper sustain_shaper({0.0f, -9.0f, 0.0f, 15.0f, 10.0f, 400.0f, 1.0f, 12.0f});
  sustain_shaper.prepare(48000.0, 1024);
  process(sustain_shaper, shaped_tail);

  REQUIRE(rms_tail(shaped_tail, 4096) < rms_tail(tail, 4096) * 0.8f);
  REQUIRE(sustain_shaper.last_gain_db() < -2.0f);
}

TEST_CASE("TransientShaper validates configuration", "[mastering][dynamics]") {
  REQUIRE_THROWS(TransientShaper({3.0f, 0.0f, -1.0f, 20.0f, 15.0f, 200.0f, 1.0f, 12.0f}));
  REQUIRE_THROWS(TransientShaper({3.0f, 0.0f, 0.0f, 20.0f, 15.0f, 200.0f, -1.0f, 12.0f}));
  REQUIRE_THROWS(TransientShaper({3.0f, 0.0f, 0.0f, 20.0f, 15.0f, 200.0f, 1.0f, -1.0f}));
}

TEST_CASE("TransientShaper supports lookahead and gain smoothing", "[mastering][dynamics]") {
  TransientShaper shaper({6.0f, 0.0f, 0.0f, 20.0f, 20.0f, 200.0f, 1.0f, 12.0f, 1.0f,
                          1.0f});
  shaper.prepare(1000.0, 8);

  std::vector<float> impulse(8, 0.0f);
  impulse[1] = 0.5f;
  process(shaper, impulse);

  REQUIRE_THAT(impulse[0], WithinAbs(0.0f, 0.0001f));
  REQUIRE(peak_abs(impulse) > 0.5f);
}

TEST_CASE("ParallelComp blends dry and compressed signals", "[mastering][dynamics]") {
  auto dry = sine(1000.0f, 48000, 48000, 0.8f);
  auto half = dry;
  auto wet = dry;

  ParallelComp half_comp({-18.0f, 6.0f, 0.0f, 20.0f, 0.0f, 0.5f});
  ParallelComp wet_comp({-18.0f, 6.0f, 0.0f, 20.0f, 0.0f, 1.0f});
  half_comp.prepare(48000.0, 1024);
  wet_comp.prepare(48000.0, 1024);

  process(half_comp, half);
  process(wet_comp, wet);

  const float dry_rms = rms_tail(dry, 4096);
  const float half_rms = rms_tail(half, 4096);
  const float wet_rms = rms_tail(wet, 4096);
  REQUIRE(wet_rms < dry_rms * 0.5f);
  REQUIRE(half_rms > wet_rms);
  REQUIRE(half_rms < dry_rms);
  REQUIRE(wet_comp.last_gain_reduction_db() < -6.0f);
}

TEST_CASE("ParallelComp attack detector operates in amplitude domain", "[mastering][dynamics]") {
  std::vector<float> transient = {1.0f};
  ParallelComp compressor({-18.0f, 20.0f, 10.0f, 20.0f, 0.0f, 1.0f});
  compressor.prepare(1000.0, 1);

  process(compressor, transient);

  REQUIRE(transient[0] > 0.95f);
  REQUIRE_THAT(compressor.last_gain_reduction_db(), WithinAbs(0.0f, 0.0001f));
}

TEST_CASE("ParallelComp links stereo detection and limits wet output", "[mastering][dynamics]") {
  ParallelComp compressor({-18.0f, 20.0f, 0.0f, 20.0f, 12.0f, 1.0f, true, true, -3.0f});
  compressor.prepare(48000.0, 1024);

  std::vector<float> left(48000, 0.9f);
  std::vector<float> right(48000, 0.05f);
  process_stereo(compressor, left, right);

  REQUIRE(rms_tail(right, 4096) < 0.04f);
  REQUIRE(peak_abs(left, 4096) <= 0.708f);
}

TEST_CASE("ParallelComp validates configuration", "[mastering][dynamics]") {
  REQUIRE_THROWS(ParallelComp({-18.0f, 0.5f, 10.0f, 100.0f, 0.0f, 0.5f}));
  REQUIRE_THROWS(ParallelComp({-18.0f, 2.0f, -1.0f, 100.0f, 0.0f, 0.5f}));
  REQUIRE_THROWS(ParallelComp({-18.0f, 2.0f, 10.0f, 100.0f, 0.0f, 1.5f}));
}

TEST_CASE("VocalRider boosts quiet material and cuts loud material", "[mastering][dynamics]") {
  VocalRider rider({-18.0f, 9.0f, 9.0f, 0.0f, 50.0f, 0.0f});
  rider.prepare(48000.0, 1024);

  std::vector<float> quiet(48000, 0.05f);
  std::vector<float> loud(48000, 0.8f);
  const float quiet_before = rms_tail(quiet, 4096);
  const float loud_before = rms_tail(loud, 4096);

  process(rider, quiet);
  rider.reset();
  process(rider, loud);

  REQUIRE(rms_tail(quiet, 4096) > quiet_before * 1.7f);
  REQUIRE(rms_tail(loud, 4096) < loud_before * 0.45f);
  REQUIRE(rider.last_gain_db() < -6.0f);
}

TEST_CASE("VocalRider links stereo detection and respects noise floor", "[mastering][dynamics]") {
  VocalRider rider({-18.0f, 9.0f, 9.0f, 0.0f, 50.0f, 0.0f, 0.0f, -50.0f, true});
  rider.prepare(48000.0, 1024);

  std::vector<float> left(48000, 0.8f);
  std::vector<float> right(48000, 0.02f);
  process_stereo(rider, left, right);
  REQUIRE(rms_tail(right, 4096) < 0.01f);

  rider.reset();
  std::vector<float> noise(48000, 0.0001f);
  const float before = rms_tail(noise, 4096);
  process(rider, noise);
  REQUIRE(rms_tail(noise, 4096) < before * 1.1f);
}

TEST_CASE("VocalRider validates configuration", "[mastering][dynamics]") {
  REQUIRE_THROWS(VocalRider({-18.0f, -1.0f, 6.0f, 50.0f, 500.0f, 0.0f}));
  REQUIRE_THROWS(VocalRider({-18.0f, 6.0f, -1.0f, 50.0f, 500.0f, 0.0f}));
  REQUIRE_THROWS(VocalRider({-18.0f, 6.0f, 6.0f, -1.0f, 500.0f, 0.0f}));
}

TEST_CASE("SidechainRouter ducks from an external detector", "[mastering][dynamics]") {
  SidechainRouter router({-30.0f, 4.0f, 0.0f, 20.0f, 18.0f});
  router.prepare(48000.0, 1024);

  std::vector<float> target(48000, 0.01f);
  std::vector<float> no_sidechain = target;
  std::vector<float> sidechain(48000, 0.8f);
  const float* sidechain_channels[] = {sidechain.data()};

  process(router, no_sidechain);
  router.reset();
  router.set_sidechain(sidechain_channels, 1, static_cast<int>(sidechain.size()));
  process(router, target);

  REQUIRE(rms_tail(no_sidechain, 4096) > 0.009f);
  REQUIRE(rms_tail(target, 4096) < rms_tail(no_sidechain, 4096) * 0.25f);
  REQUIRE(router.last_gain_reduction_db() < -10.0f);
}

TEST_CASE("SidechainRouter validates configuration and sidechain buffers",
          "[mastering][dynamics]") {
  REQUIRE_THROWS(SidechainRouter({-24.0f, 0.5f, 5.0f, 100.0f, 18.0f}));
  REQUIRE_THROWS(SidechainRouter({-24.0f, 4.0f, -1.0f, 100.0f, 18.0f}));
  REQUIRE_THROWS(SidechainRouter({-24.0f, 4.0f, 5.0f, 100.0f, -1.0f}));

  SidechainRouter router;
  REQUIRE_THROWS(router.set_sidechain(nullptr, 1, 128));
}

TEST_CASE("SidechainRouter treats empty sidechain as cleared", "[mastering][dynamics]") {
  SidechainRouter router({-30.0f, 4.0f, 0.0f, 20.0f, 18.0f});
  router.prepare(48000.0, 1024);

  std::vector<float> sidechain(128, 1.0f);
  const float* sidechain_channels[] = {sidechain.data()};
  router.set_sidechain(sidechain_channels, 1, static_cast<int>(sidechain.size()));
  router.set_sidechain(nullptr, 0, 0);

  std::vector<float> target(128, 0.01f);
  process(router, target);

  REQUIRE(rms_tail(target, 0) > 0.009f);
  REQUIRE_THAT(router.last_gain_reduction_db(), WithinAbs(0.0f, 0.0001f));
}

TEST_CASE("SidechainRouter supports HPF mono key and key listen", "[mastering][dynamics]") {
  SidechainRouter listen({-30.0f, 4.0f, 0.0f, 20.0f, 18.0f, true, 120.0f, true, true});
  listen.prepare(48000.0, 1024);

  std::vector<float> target(1024, 0.0f);
  std::vector<float> low = sine(40.0f, 48000, 1024, 1.0f);
  std::vector<float> high = sine(1000.0f, 48000, 1024, 1.0f);
  const float* sidechain[] = {low.data(), high.data()};
  listen.set_sidechain(sidechain, 2, static_cast<int>(target.size()));
  process(listen, target);

  REQUIRE(rms_tail(target, 128) > 0.1f);
}
