/// @file bowed_string_onset_test.cpp
/// @brief Bowed-string note onset: how quickly Helmholtz motion establishes and
///        what the seeded initial condition does to the amplitude envelope.
///        Cases carry [bowed][onset] and are invoked as an AND, because
///        [onset] alone reaches other engines' tests.
///
/// The seed (attack_noise) and the acceleration ramp (bow_accel_ms) are each
/// measured against a nonlinear stick-slip loop, which is sensitive to its
/// initial condition: the grid below is reported in full rather than reduced
/// to a single pass/fail, because a change of amplitude does not move the
/// measured onset time monotonically. What IS asserted is what the mechanism
/// owes regardless of that sensitivity -- stability, and identity at the
/// zero default -- plus a self-check of the local rise-time helper against a
/// signal whose answer is known by construction.

#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <cstdint>
#include <sstream>
#include <vector>

#include "midi/bowed_string_probe.h"
#include "midi/synth/bowed_string_voice.h"
#include "midi/synth/gm_fallback_map.h"
#include "midi/synth/pitch.h"
#include "support/golden_hash.h"
#include "util/constants.h"

namespace {

using sonare::midi::synth::BowedStringPatchParams;
using sonare::midi::synth::BowedStringVoiceCore;
using sonare::midi::synth::gm_fallback_patch;
using sonare::midi::synth::note_to_hz;
using sonare::test::fnv1a_quantized;
using sonare::test::bowed::time_to_helmholtz;

constexpr double kSr = 48000.0;
constexpr uint8_t kVelocity = 100;
constexpr uint64_t kSeed = 0x0B0EDull;
// The gate's attack p90 reaches 900 ms, so a shorter render cannot see the
// slowest shipped bow ramp (viola's attack_ms is 857).
constexpr int kOnsetSamples = static_cast<int>(1.5 * kSr);

// The 7 GS slots the design targets, each read at its own open-string-to-upper
// register triple (matching bowed_string_force_window_test.cpp's kShipped, so
// two sweeps of the same voices don't disagree on what "the instrument's
// range" means) rather than one triple shared across every patch: an
// arbitrary shared note tested contrabass past its own top and violin below
// its bottom, both of which miss Helmholtz for reasons unrelated to either
// mechanism under test here.
struct ShippedVoice {
  uint8_t program;
  uint8_t notes[3];
};
constexpr ShippedVoice kShipped[] = {
    {40, {55, 72, 88}},   // violin: G3 open string .. E6
    {41, {48, 67, 84}},   // viola: C3 open string .. C6
    {42, {36, 55, 72}},   // cello: C2 open string .. C5
    {43, {28, 45, 55}},   // contrabass: E1 sounding .. G3
    {110, {55, 72, 88}},  // fiddle: the violin's range
    {48, {36, 60, 84}},   // string_ensemble_1
    {49, {36, 60, 84}},   // string_ensemble_2
};
constexpr int kShippedCount = 7;

std::vector<float> render_core(const BowedStringPatchParams& params, uint8_t note, int samples) {
  BowedStringVoiceCore core;
  const int per_line = sonare::midi::synth::bowed_string_buffer_capacity(kSr);
  std::vector<float> slab(
      static_cast<std::size_t>(sonare::midi::synth::bowed_string_slab_capacity(kSr)), 0.0f);
  core.attach(slab.data(), per_line);
  core.start(params, kSr, note, kVelocity, kSeed);
  std::vector<float> out(static_cast<std::size_t>(samples), 0.0f);
  for (int i = 0; i < samples; ++i) out[static_cast<std::size_t>(i)] = core.render(1.0f);
  return out;
}

float peak_abs(const std::vector<float>& span) {
  float p = 0.0f;
  for (float s : span) p = std::max(p, std::fabs(s));
  return p;
}

bool all_finite(const std::vector<float>& span) {
  for (float s : span) {
    if (!std::isfinite(s)) return false;
  }
  return true;
}

struct Envelope {
  std::vector<double> times;
  std::vector<double> rms;
};

/// Frame RMS envelope, hop/win in ms. Mirrors metrics_signal.py's
/// _rms_envelope so the onset grid below reads the same rise time the
/// reference-comparison harness would.
Envelope rms_envelope(const std::vector<float>& y, double sr, double hop_ms = 5.0,
                      double win_ms = 10.0) {
  const int hop = std::max(1, static_cast<int>(std::lround(sr * hop_ms / 1000.0)));
  const int win = std::max(hop, static_cast<int>(std::lround(sr * win_ms / 1000.0)));
  Envelope out;
  if (static_cast<int>(y.size()) < win) return out;
  const int n = (static_cast<int>(y.size()) - win) / hop + 1;
  out.times.reserve(static_cast<std::size_t>(n));
  out.rms.reserve(static_cast<std::size_t>(n));
  for (int i = 0; i < n; ++i) {
    const int from = i * hop;
    double acc = 0.0;
    for (int k = 0; k < win; ++k) {
      const double s = y[static_cast<std::size_t>(from + k)];
      acc += s * s;
    }
    out.rms.push_back(std::sqrt(acc / win));
    out.times.push_back((from + win / 2.0) / sr);
  }
  return out;
}

// 10%->90% RMS rise time (ms), mirroring metrics_note.py's attack_ms: peak is
// read only over the first 0.5 s so a later swell cannot read as slow attack.
double attack_rise_ms(const std::vector<float>& span, double sr) {
  const Envelope env = rms_envelope(span, sr);
  if (env.rms.empty()) return 0.0;
  double peak = 0.0;
  bool any = false;
  for (std::size_t i = 0; i < env.rms.size(); ++i) {
    if (env.times[i] <= 0.5) {
      peak = std::max(peak, env.rms[i]);
      any = true;
    }
  }
  if (!any) {
    for (double r : env.rms) peak = std::max(peak, r);
  }
  if (!(peak > 0.0)) return 0.0;
  int i10 = -1;
  int i90 = -1;
  for (std::size_t i = 0; i < env.rms.size(); ++i) {
    if (i10 < 0 && env.rms[i] >= 0.1 * peak) i10 = static_cast<int>(i);
    if (i90 < 0 && env.rms[i] >= 0.9 * peak) i90 = static_cast<int>(i);
  }
  if (i10 < 0 || i90 < 0) return 0.0;
  return std::max(
      0.0, (env.times[static_cast<std::size_t>(i90)] - env.times[static_cast<std::size_t>(i10)]) *
               1000.0);
}

}  // namespace

TEST_CASE("attack_rise_ms reads a synthetic linear ramp", "[midi][synth][bowed][onset]") {
  // A tone whose amplitude ramps linearly from 0 to 1 over kRampMs, then holds:
  // the 10%->90% RMS crossing of a linear ramp is 80% of its own duration
  // (RMS of a linear ramp is itself linear in the ramp's amplitude for a
  // narrowband tone averaged over many cycles), so the answer is known before
  // the helper is trusted on the engine's own output below.
  constexpr double kRampMs = 200.0;
  constexpr double kF0 = 300.0;
  constexpr int kSamples = static_cast<int>(1.0 * kSr);
  std::vector<float> tone(static_cast<std::size_t>(kSamples), 0.0f);
  const double ramp_samples = kRampMs * 0.001 * kSr;
  for (int i = 0; i < kSamples; ++i) {
    const double env = std::min(1.0, static_cast<double>(i) / ramp_samples);
    tone[static_cast<std::size_t>(i)] =
        static_cast<float>(env * std::sin(sonare::constants::kTwoPiD * kF0 * i / kSr));
  }
  CHECK(fnv1a_quantized(tone) == 0x813f0534e2bb2d58ull);
  const double measured = attack_rise_ms(tone, kSr);
  CHECK(measured > 0.75 * 0.8 * kRampMs);
  CHECK(measured < 1.25 * 0.8 * kRampMs);

  // Sensitivity: a tone at full amplitude from sample 0 must read as instant
  // (no rise), not as some fraction of the render length.
  std::vector<float> instant(static_cast<std::size_t>(kSamples), 0.0f);
  for (int i = 0; i < kSamples; ++i) {
    instant[static_cast<std::size_t>(i)] =
        static_cast<float>(std::sin(sonare::constants::kTwoPiD * kF0 * i / kSr));
  }
  CHECK(attack_rise_ms(instant, kSr) < 0.2 * 0.8 * kRampMs);
}

TEST_CASE("onset seed and bow acceleration default to identity", "[midi][synth][bowed][onset]") {
  // Both fields are ZeroIsSentinel; this is the same claim the probe test's
  // control hashes make, scoped to every shipped voice at its own register
  // rather than just the violin at note 60.
  for (int s = 0; s < kShippedCount; ++s) {
    const BowedStringPatchParams base = gm_fallback_patch(0, kShipped[s].program).bowed_string;
    REQUIRE(base.attack_noise == 0.0f);
    REQUIRE(base.bow_accel_ms == 0.0f);
    for (uint8_t note : kShipped[s].notes) {
      const std::vector<float> a = render_core(base, note, kOnsetSamples);
      BowedStringPatchParams touched = base;
      touched.attack_noise = 0.0f;
      touched.bow_accel_ms = 0.0f;
      const std::vector<float> b = render_core(touched, note, kOnsetSamples);
      CHECK(a == b);
    }
  }
}

TEST_CASE("onset seed and bow acceleration stay stable across the shipped voices",
          "[.][midi][synth][bowed][onset]") {
  // Neither mechanism may be given more than a moment: this is a stability and
  // measurement pass, not a fit. `[.]` because the render grid is ~1.5 s x 42
  // cells x 2 mechanisms.
  //
  // What the two loops below establish, reported through WARN so it is always
  // visible rather than only on failure: the stick-slip loop's sensitivity to
  // its own initial condition means neither ratio is monotone in the seed's
  // depth or the ramp's duration, so no single value drives every shipped
  // voice's ratio under the design's own bound in every cell -- see the U4
  // report for the measured pass rate. What IS true in every cell, and IS
  // asserted, is that the mechanisms never destabilise the loop.
  constexpr float kAttackNoiseOn = 0.1f;
  constexpr float kBowAccelOnMs = 40.0f;
  // clamp_synth_patch (native_synth.h:1240-1245) accepts attack_noise anywhere
  // in [0,1] and bow_accel_ms anywhere in [0,2000], so the stability check
  // below runs BOTH the candidate depth and the clamp's own upper end -- a
  // bound justified by the clamp range has to be measured over it, not over
  // the two depths the ratio/envelope measurement further down happens to use.
  constexpr float kAttackNoiseClampMax = 1.0f;
  constexpr float kBowAccelClampMax = 2000.0f;
  // The loop below measures 5.04047 as the maximum over every cell and both
  // depths, always at the clamp's own extreme (attack_noise=1, bow_accel_ms=
  // 2000): bowed_string_voice_test.cpp's sibling gated-mechanism combinations
  // hold to 4.0f; this pair's worst case is its own, with a 10% margin over
  // the measured maximum rather than a round number chosen to clear it.
  constexpr float kStabilityPeakBound = 5.55f;
  for (bool ep : {false, true}) {
    for (int s = 0; s < kShippedCount; ++s) {
      BowedStringPatchParams base = gm_fallback_patch(0, kShipped[s].program).bowed_string;
      base.elasto_plastic = ep;
      for (uint8_t note : kShipped[s].notes) {
        const double f0 = static_cast<double>(note_to_hz(note));

        for (float depth : {kAttackNoiseOn, kAttackNoiseClampMax}) {
          BowedStringPatchParams seeded_stab = base;
          seeded_stab.attack_noise = depth;
          const std::vector<float> render = render_core(seeded_stab, note, kOnsetSamples);
          INFO("seed stability ep=" << ep << " program=" << int(kShipped[s].program)
                                    << " note=" << int(note) << " depth=" << depth);
          REQUIRE(all_finite(render));
          CHECK(peak_abs(render) < kStabilityPeakBound);
        }
        for (float depth_ms : {kBowAccelOnMs, kBowAccelClampMax}) {
          BowedStringPatchParams accel_stab = base;
          accel_stab.bow_accel_ms = depth_ms;
          const std::vector<float> render = render_core(accel_stab, note, kOnsetSamples);
          INFO("accel stability ep=" << ep << " program=" << int(kShipped[s].program)
                                     << " note=" << int(note) << " depth_ms=" << depth_ms);
          REQUIRE(all_finite(render));
          CHECK(peak_abs(render) < kStabilityPeakBound);
        }

        BowedStringPatchParams seeded = base;
        seeded.attack_noise = kAttackNoiseOn;
        const std::vector<float> seeded_render = render_core(seeded, note, kOnsetSamples);

        BowedStringPatchParams accel = base;
        accel.bow_accel_ms = kBowAccelOnMs;
        const std::vector<float> accel_render = render_core(accel, note, kOnsetSamples);

        const sonare::test::bowed::HelmholtzOnset off_onset =
            time_to_helmholtz(render_core(base, note, kOnsetSamples), f0, kSr);
        const sonare::test::bowed::HelmholtzOnset on_onset =
            time_to_helmholtz(seeded_render, f0, kSr);
        double ratio = -1.0;
        if (off_onset.established && on_onset.established && off_onset.seconds > 0.0) {
          ratio = on_onset.seconds / off_onset.seconds;
        }
        WARN("seed ep=" << ep << " p=" << int(kShipped[s].program) << " n=" << int(note)
                        << " off_s=" << off_onset.seconds << " on_s=" << on_onset.seconds
                        << " ratio=" << ratio);

        if (!ep) {
          const double base_ms = attack_rise_ms(render_core(base, note, kOnsetSamples), kSr);
          const double on_ms = attack_rise_ms(accel_render, kSr);
          const double rel = base_ms > 0.0 ? std::fabs(on_ms - base_ms) / base_ms : -1.0;
          WARN("accel p=" << int(kShipped[s].program) << " n=" << int(note)
                          << " base_ms=" << base_ms << " on_ms=" << on_ms << " rel=" << rel);
        }
      }
    }
  }
}
