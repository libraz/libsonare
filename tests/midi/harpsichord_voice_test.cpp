/// @file harpsichord_voice_test.cpp
/// @brief Harpsichord core (midi/synth/harpsichord_voice): the plectrum's
///        release law, the loss filter solved against decay targets, the
///        registration of separate string choirs, the behind-the-bridge halo
///        and the mechanism at note-off.
///
/// The assertions here are the instrument's published and measured numbers
/// rather than round figures: the whole dynamic range fits in a few dB, a
/// treble string still sounds after four seconds, and the sustained decay rate
/// tracks a captured reference across the compass. Each is a property the voice
/// this engine replaced got wrong.

#include "midi/synth/harpsichord_voice.h"

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <vector>

#include "midi/synth/gm_fallback_map.h"
#include "midi/synth/native_synth.h"
#include "support/alloc_guard.h"

namespace {

using sonare::midi::synth::gm_fallback_patch;
using sonare::midi::synth::harpsichord_buffer_capacity;
using sonare::midi::synth::harpsichord_slab_capacity;
using sonare::midi::synth::HarpsichordPatchParams;
using sonare::midi::synth::HarpsichordVoiceCore;
using sonare::midi::synth::SynthEngineMode;

constexpr double kSr = 48000.0;

/// One voice's slab, sized the way the host sizes it.
struct Slab {
  std::vector<float> data;
  int per_line;
  Slab()
      : data(static_cast<size_t>(harpsichord_slab_capacity(kSr)), 0.0f),
        per_line(harpsichord_buffer_capacity(kSr)) {}
};

/// Renders @p seconds of a held note and returns the samples.
std::vector<float> render_held(const HarpsichordPatchParams& params, uint8_t note, uint8_t velocity,
                               double seconds, Slab& slab) {
  HarpsichordVoiceCore core;
  core.attach(slab.data.data(), slab.per_line);
  core.start(params, kSr, note, velocity, 0x51D5u + note);
  const int n = static_cast<int>(seconds * kSr);
  std::vector<float> out(static_cast<size_t>(n));
  for (int i = 0; i < n; ++i) out[static_cast<size_t>(i)] = core.render(1.0f);
  return out;
}

float peak(const std::vector<float>& x) {
  float m = 0.0f;
  for (float v : x) m = std::max(m, std::abs(v));
  return m;
}

/// RMS in dB over a window given in seconds.
float window_db(const std::vector<float>& x, double from_s, double to_s) {
  const size_t a = static_cast<size_t>(from_s * kSr);
  const size_t b = std::min(x.size(), static_cast<size_t>(to_s * kSr));
  if (b <= a) return -600.0f;
  double sum = 0.0;
  for (size_t i = a; i < b; ++i) sum += static_cast<double>(x[i]) * x[i];
  return 20.0f *
         std::log10(static_cast<float>(std::sqrt(sum / static_cast<double>(b - a))) + 1e-30f);
}

/// Sustained decay rate in dB per second, measured clear of the attack.
float decay_db_s(const std::vector<float>& x) {
  const float early = window_db(x, 1.5, 2.0);
  const float late = window_db(x, 3.0, 3.5);
  return (late - early) / 1.5f;
}

}  // namespace

TEST_CASE("harpsichord velocity spans a few dB, not a piano's forty",
          "[midi][synth][harpsichord]") {
  HarpsichordPatchParams params;
  params.velocity_range_db = 5.0f;
  Slab slab;

  // Every note, because a level law that holds at one pitch and not another is
  // the defect this replaced: the engine before it covered 28 dB uniformly.
  for (uint8_t note : {29, 48, 60, 72, 89}) {
    const float soft = peak(render_held(params, note, 24, 1.0, slab));
    const float hard = peak(render_held(params, note, 120, 1.0, slab));
    REQUIRE(soft > 0.0f);
    const float range_db = 20.0f * std::log10(hard / soft);
    INFO("note " << static_cast<int>(note) << " spans " << range_db << " dB");
    // The literature puts the instrument between 3 and 6 dB; a captured 8'
    // reference measured 6.1. The window is generous at the top and hard at the
    // bottom, because being too responsive is the failure that matters.
    REQUIRE(range_db > 1.0f);
    REQUIRE(range_db < 8.0f);
    // Monotonic while velocity_droop_db is 0.
    REQUIRE(hard > soft);
  }
}

TEST_CASE("harpsichord peak level is set by the mechanism, not the note",
          "[midi][synth][harpsichord]") {
  HarpsichordPatchParams params;
  Slab slab;
  float lowest = 1.0e9f;
  float highest = 0.0f;
  for (uint8_t note : {29, 41, 53, 65, 77, 89}) {
    const float p = peak(render_held(params, note, 88, 0.5, slab));
    lowest = std::min(lowest, p);
    highest = std::max(highest, p);
  }
  // The plectrum lifts every string to the same place, so the compass is flat:
  // both captured references hold within 3.7 dB over five octaves.
  const float spread_db = 20.0f * std::log10(highest / lowest);
  INFO("compass spread " << spread_db << " dB");
  REQUIRE(spread_db < 6.0f);
}

TEST_CASE("harpsichord treble still sounds after four seconds", "[midi][synth][harpsichord]") {
  HarpsichordPatchParams params;
  params.decay_s = 11.6f;
  params.decay_stretch = 0.40f;
  Slab slab;

  // f''' (MIDI 89) is where a loop filter chosen for its tone kills a string:
  // its fundamental is attenuated on each of 1400 traversals a second, and the
  // note is gone in a fifth of a second however long a decay it was given. What
  // this case guards is the outcome; which part of the loss filter's design
  // delivers it is the solver's own test below.
  const std::vector<float> top = render_held(params, 89, 88, 4.5, slab);
  const float at_start = window_db(top, 0.2, 0.4);
  const float at_four = window_db(top, 4.0, 4.4);
  INFO("f''' falls " << (at_start - at_four) << " dB over four seconds");
  REQUIRE(at_four > at_start - 55.0f);
}

TEST_CASE("harpsichord sustained decay tracks the captured reference across the compass",
          "[midi][synth][harpsichord]") {
  HarpsichordPatchParams params;
  params.decay_s = 11.6f;
  params.decay_stretch = 0.40f;
  Slab slab;

  // Sustained rates measured on the captured GM reference, in dB per second.
  // The tolerance is the spread between the two references, not a rounding.
  struct Row {
    uint8_t note;
    float reference_db_s;
  };
  for (const Row row : {Row{29, 2.12f}, Row{48, 3.86f}, Row{60, 5.61f}, Row{72, 7.47f}}) {
    const std::vector<float> x = render_held(params, row.note, 88, 4.0, slab);
    const float rate = -decay_db_s(x);
    INFO("note " << static_cast<int>(row.note) << " decays at " << rate << " dB/s, reference "
                 << row.reference_db_s);
    REQUIRE(rate > row.reference_db_s * 0.55f);
    REQUIRE(rate < row.reference_db_s * 1.8f);
  }
}

TEST_CASE("harpsichord registration draws real choirs", "[midi][synth][harpsichord]") {
  Slab slab;
  HarpsichordPatchParams eight;
  const std::vector<float> single = render_held(eight, 60, 88, 1.0, slab);

  HarpsichordPatchParams both = eight;
  both.eight_b = true;
  const std::vector<float> unison = render_held(both, 60, 88, 1.0, slab);

  HarpsichordPatchParams with_four = eight;
  with_four.four = true;
  const std::vector<float> octave = render_held(with_four, 60, 88, 1.0, slab);

  // Each drawn stop is a separate set of strings, so each changes the sound.
  REQUIRE(single != unison);
  REQUIRE(single != octave);
  REQUIRE(unison != octave);

  // Drawing nothing leaves the instrument silent rather than falling back to a
  // default choir: a registration with every stop pushed in makes no sound.
  HarpsichordPatchParams none;
  none.eight_a = false;
  REQUIRE(peak(render_held(none, 60, 88, 0.5, slab)) == 0.0f);
}

TEST_CASE("harpsichord options are inert at their off values", "[midi][synth][harpsichord]") {
  Slab slab;
  HarpsichordPatchParams plain;
  const std::vector<float> base = render_held(plain, 60, 88, 0.5, slab);

  // A knob at its no-op value must reproduce the baseline exactly, or whatever
  // its sweep shows is an artifact of turning it on rather than of its value.
  HarpsichordPatchParams no_rear = plain;
  no_rear.rear_segment_mm = 0.0f;
  REQUIRE(render_held(no_rear, 60, 88, 0.5, slab) == base);

  HarpsichordPatchParams no_noise = plain;
  no_noise.pluck_noise = 0.0f;
  no_noise.jack_noise = 0.0f;
  REQUIRE(render_held(no_noise, 60, 88, 0.5, slab) == base);

  // And each must actually do something when turned on.
  HarpsichordPatchParams rear = plain;
  rear.rear_segment_mm = 90.0f;
  REQUIRE(render_held(rear, 60, 88, 0.5, slab) != base);

  HarpsichordPatchParams chiff = plain;
  chiff.pluck_noise = 0.5f;
  REQUIRE(render_held(chiff, 60, 88, 0.5, slab) != base);
}

TEST_CASE("harpsichord damper stops a released string and the undamped 4' top does not",
          "[midi][synth][harpsichord]") {
  Slab slab;
  HarpsichordPatchParams params;
  params.decay_s = 11.6f;
  params.damper_s = 0.12f;

  auto tail_after_release = [&](const HarpsichordPatchParams& p, uint8_t note) {
    HarpsichordVoiceCore core;
    core.attach(slab.data.data(), slab.per_line);
    core.start(p, kSr, note, 88, 0x51D5u);
    std::vector<float> out(static_cast<size_t>(2.0 * kSr));
    const int release_at = static_cast<int>(1.0 * kSr);
    for (size_t i = 0; i < out.size(); ++i) {
      if (static_cast<int>(i) == release_at) core.release();
      out[i] = core.render(1.0f);
    }
    return window_db(out, 1.5, 1.9) - window_db(out, 0.6, 0.9);
  };

  // A damped string is stopped by the felt, not merely left to decay.
  const float damped = tail_after_release(params, 60);
  INFO("damped string falls " << damped << " dB after release");
  REQUIRE(damped < -30.0f);

  // The top of the 4' choir carries no dampers, so above the break those
  // strings go on sounding after the key is released.
  HarpsichordPatchParams undamped = params;
  undamped.eight_a = false;
  undamped.four = true;
  undamped.undamped_from_note = 84;
  const float ringing = tail_after_release(undamped, 89);
  INFO("undamped 4' falls " << ringing << " dB after release");
  REQUIRE(ringing > damped + 20.0f);
}

TEST_CASE("harpsichord render is allocation-free and deterministic", "[midi][synth][harpsichord]") {
  Slab slab;
  HarpsichordPatchParams params;
  params.eight_b = true;
  params.four = true;
  params.rear_segment_mm = 90.0f;
  params.pluck_noise = 0.4f;
  params.jack_noise = 0.4f;

  HarpsichordVoiceCore core;
  core.attach(slab.data.data(), slab.per_line);
  std::vector<float> first(4096);
  {
    sonare::test::AllocationGuard guard;
    core.start(params, kSr, 60, 88, 0x51D5u);
    for (size_t i = 0; i < first.size(); ++i) {
      if (i == 3000) core.release();
      first[i] = core.render(1.0f);
    }
    core.kill();
    REQUIRE(guard.count() == 0);
  }

  // Same events, same seed, same samples: the plectrum is not a noise source
  // and the mechanism bursts draw from the counter-based per-voice stream.
  std::vector<float> second(first.size());
  core.start(params, kSr, 60, 88, 0x51D5u);
  for (size_t i = 0; i < second.size(); ++i) {
    if (i == 3000) core.release();
    second[i] = core.render(1.0f);
  }
  REQUIRE(first == second);
}

TEST_CASE("GM harpsichord banks are voiced by the harpsichord engine",
          "[midi][synth][harpsichord]") {
  for (uint16_t bank : {0, 1, 2, 3}) {
    const auto& patch = gm_fallback_patch(bank, 6);
    INFO("bank " << bank);
    REQUIRE(patch.mode == SynthEngineMode::kHarpsichord);
    REQUIRE(patch.harpsichord.eight_a);
    // The instrument's defining number survives clamping on every bank.
    REQUIRE(patch.harpsichord.velocity_range_db > 0.0f);
    REQUIRE(patch.harpsichord.velocity_range_db <= 8.0f);
  }
}
