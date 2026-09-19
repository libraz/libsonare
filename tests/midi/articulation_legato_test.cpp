/// @file articulation_legato_test.cpp
/// @brief Voice continuation: a second note-on under a held key moves the
///        sounding voice instead of starting one.
///
/// What a slur has to be is three things at once, and any two of them pass for
/// something that is not a slur. The envelope must not break — but a voice that
/// ignored the second note entirely has a perfect envelope. The pitch must be
/// the second note in absolute terms — but a retrigger reaches that too, having
/// restarted. So the envelope claim carries its own control: the SAME two notes
/// played through the retriggering mode must break the envelope by a margin the
/// slur does not, which is what says the measure can see a break at all.
///
/// The fallback is measured rather than heard, because it cannot be heard: an
/// engine that declines to be carried plays the note normally, and normal is
/// exactly what a mode that was never set sounds like. The count is the only
/// thing that separates them.

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <cstdint>
#include <vector>

#include "midi/synth/articulation.h"
#include "midi/synth/native_synth.h"
#include "midi/ump.h"
#include "support/audio_fixtures.h"
#include "support/midi_render.h"

namespace {

using sonare::midi::synth::ArticulationMode;
using sonare::midi::synth::NativeSynth;
using sonare::midi::synth::NativeSynthConfig;
using sonare::midi::synth::NativeSynthPatch;
using sonare::midi::synth::SynthEngineMode;
using sonare::test::event;
using sonare::test::fft_fundamental;
using sonare::test::render_left;

constexpr double kRate = 48000.0;
constexpr int kBlock = 256;
constexpr uint8_t kFirst = 60;
constexpr uint8_t kSecond = 67;  // a fifth up: far enough to measure, inside every engine's reach
constexpr uint8_t kVelocity = 100;
constexpr double kFirstHz = 261.6255653;
constexpr double kSecondHz = 391.9954360;

/// Frames before the slur (the onset and the bore speech are over by then), and
/// frames held on the second note afterwards.
constexpr int kBeforeSlur = 12288;
constexpr int kAfterSlur = 16384;
/// Envelope hop. Short enough that a fade of a few milliseconds shows as a step
/// rather than being averaged away, long enough to be an envelope.
constexpr int kEnvHop = 128;

struct Phrase {
  std::vector<float> audio;
  uint64_t fallbacks = 0;
};

/// note_on(first), hold, note_on(second) — the slur — then the LATE note_off of
/// the first key, which is what a real slur sends and what must not end it.
Phrase play_slur(SynthEngineMode mode, ArticulationMode articulation) {
  NativeSynthConfig cfg;
  cfg.patch = NativeSynthPatch{};
  cfg.patch.mode = mode;
  cfg.patch.cutoff_hz = 20000.0f;
  cfg.patch.amp_env.sustain = 1.0f;
  cfg.patch.amp_env.release_ms = 100.0f;

  NativeSynth synth(cfg);
  synth.prepare(kRate, kBlock);
  synth.set_articulation(0, articulation);
  synth.on_event(0, event(sonare::midi::make_midi1_note_on(0, 0, kFirst, kVelocity)));
  Phrase out;
  out.audio = render_left(synth, kBeforeSlur);
  synth.on_event(0, event(sonare::midi::make_midi1_note_on(0, 0, kSecond, kVelocity)));
  synth.on_event(0, event(sonare::midi::make_midi1_note_off(0, 0, kFirst, 0)));
  const std::vector<float> tail = render_left(synth, kAfterSlur);
  out.audio.insert(out.audio.end(), tail.begin(), tail.end());
  out.fallbacks = synth.legato_fallback_count();
  return out;
}

std::vector<double> envelope(const std::vector<float>& audio) {
  std::vector<double> env;
  env.reserve(audio.size() / kEnvHop);
  for (size_t i = 0; i + kEnvHop <= audio.size(); i += kEnvHop) {
    double acc = 0.0;
    for (size_t k = 0; k < kEnvHop; ++k) {
      const double s = audio[i + k];
      acc += s * s;
    }
    env.push_back(std::sqrt(acc / kEnvHop));
  }
  return env;
}

/// Largest single-hop change in the envelope around the slur, as a fraction of
/// the level the phrase is sitting at. Relative, so it says nothing about how
/// loud the patch is and everything about whether the level broke.
double envelope_step_at_slur(const std::vector<float>& audio) {
  const std::vector<double> env = envelope(audio);
  const size_t centre = kBeforeSlur / kEnvHop;
  const size_t lo = centre > 16 ? centre - 16 : 0;
  const size_t hi = std::min(env.size() - 1, centre + 64);
  double level = 0.0;
  for (size_t i = lo; i <= hi; ++i) level += env[i];
  level /= static_cast<double>(hi - lo + 1);
  if (level <= 0.0) return 0.0;
  double worst = 0.0;
  for (size_t i = lo; i < hi; ++i) worst = std::max(worst, std::fabs(env[i + 1] - env[i]));
  return worst / level;
}

double pitch_after_slur(const std::vector<float>& audio) {
  // Measured well past the slur, so a glide (if the patch had one) has landed.
  return fft_fundamental(audio, static_cast<size_t>(kBeforeSlur + 4096), kSecondHz);
}

}  // namespace

TEST_CASE("a slur moves the sounding voice without breaking its envelope",
          "[midi][synth][articulation]") {
  const Phrase slurred = play_slur(SynthEngineMode::kReed, ArticulationMode::kMonoLegato);
  const Phrase retriggered = play_slur(SynthEngineMode::kReed, ArticulationMode::kMonoRetrigger);

  // Neither arm is silence, or every claim below is about two kinds of nothing.
  REQUIRE(*std::max_element(slurred.audio.begin(), slurred.audio.end()) > 0.0f);
  REQUIRE(*std::max_element(retriggered.audio.begin(), retriggered.audio.end()) > 0.0f);

  const double slur_step = envelope_step_at_slur(slurred.audio);
  const double retrigger_step = envelope_step_at_slur(retriggered.audio);
  CAPTURE(slur_step, retrigger_step);

  // The control: the same two notes, the same patch, the same instant, through
  // the mode that stops and restarts. Without this the bound would be a number
  // nothing had shown could be exceeded. Measured 0.16 slurred against 0.96
  // retriggered -- a bore that changes length does move the level, and what
  // separates the two is that only one of them drops out.
  REQUIRE(retrigger_step > 4.0 * slur_step);

  // And the slur arrived: absolutely the second note, not merely a pitch that
  // moved. A retune that added the interval twice, or applied it to the wrong
  // reference, lands somewhere else and is caught here rather than by the
  // envelope.
  const double sounding = pitch_after_slur(slurred.audio);
  const double cents = 1200.0 * std::log2(sounding / kSecondHz);
  CAPTURE(sounding, cents);
  // 25 cents is the reed's own tuning residual plus the analysis bin, with room
  // to spare: the slur lands 1.4 cents off.
  REQUIRE(std::fabs(cents) < 25.0);
  // The slur really moved: the first note is 700 cents away and would fail the
  // bound above, so state the interval it crossed as well.
  REQUIRE(std::fabs(1200.0 * std::log2(sounding / kFirstHz)) > 600.0);

  // The late note-off of the FIRST key did not end the phrase -- the path the
  // whole design turns on.
  const std::vector<double> env = envelope(slurred.audio);
  REQUIRE(env.back() > 0.1 * env[static_cast<size_t>(kBeforeSlur / kEnvHop) - 1]);
  REQUIRE(slurred.fallbacks == 0);
}

TEST_CASE("an engine that declines to be carried plays the note and says so",
          "[midi][synth][articulation]") {
  // A struck string cannot be slurred: its exciter is spent before the second
  // sample, so continuing the voice would sound a note that was never struck.
  REQUIRE_FALSE(sonare::midi::synth::accepts_legato(SynthEngineMode::kPiano, kFirst, kSecond));

  const Phrase declined = play_slur(SynthEngineMode::kPiano, ArticulationMode::kMonoLegato);
  // The refusal is counted, which is the only thing that separates it from a
  // mode that was never set.
  REQUIRE(declined.fallbacks >= 1);
  // And the note sounds: a refusal falls back to an ordinary note rather than
  // dropping it.
  const std::vector<double> env = envelope(declined.audio);
  const size_t after = static_cast<size_t>(kBeforeSlur / kEnvHop) + 8;
  REQUIRE(after < env.size());
  REQUIRE(env[after] > 0.0);
  const double sounding = pitch_after_slur(declined.audio);
  REQUIRE(std::fabs(1200.0 * std::log2(sounding / kSecondHz)) < 40.0);
}

TEST_CASE("a pitch below the engine's delay line is refused rather than pinned",
          "[midi][synth][articulation]") {
  // Upward is unconditional -- it shortens the delay.
  REQUIRE(sonare::midi::synth::accepts_legato(SynthEngineMode::kReed, 24, 96));
  // Downward is bounded by the line, and the bound is the engine's own floor
  // rather than a number written here: 20 Hz is under MIDI note 16.
  REQUIRE(sonare::midi::synth::accepts_legato(SynthEngineMode::kReed, 60, 24));
  REQUIRE_FALSE(sonare::midi::synth::accepts_legato(SynthEngineMode::kReed, 60, 12));
  // A pipe organ voicing a 16' rank runs out an octave earlier than an 8' one,
  // and the caller supplies the registration because the mode cannot know it.
  // The pair is the point: the same target note, decided differently by the
  // registration alone.
  REQUIRE(sonare::midi::synth::accepts_legato(SynthEngineMode::kPipeOrgan, 60, 23, 1.0f));
  REQUIRE_FALSE(sonare::midi::synth::accepts_legato(SynthEngineMode::kPipeOrgan, 60, 23, 0.5f));
}

TEST_CASE("every engine's legato floor is a pitch its delay line can hold",
          "[midi][synth][articulation]") {
  // The floors are declared beside the legato rows and the capacities beside
  // the engines, so the two are only ever compared here. A capacity formula
  // that changed without its floor would otherwise leave the row promising a
  // reach the line no longer has.
  struct Row {
    SynthEngineMode mode;
    int capacity;
    const char* label;
  };
  const Row rows[] = {
      {SynthEngineMode::kBowedString, sonare::midi::synth::bowed_string_buffer_capacity(kRate),
       "bowed string"},
      {SynthEngineMode::kReed, sonare::midi::synth::reed_buffer_capacity(kRate), "reed"},
      {SynthEngineMode::kBrass, sonare::midi::synth::brass_buffer_capacity(kRate), "brass"},
      {SynthEngineMode::kFlute, sonare::midi::synth::flute_buffer_capacity(kRate), "flute"},
      {SynthEngineMode::kPipeOrgan, sonare::midi::synth::pipe_organ_buffer_capacity(kRate),
       "pipe organ"},
  };
  size_t checked = 0;
  for (const Row& row : rows) {
    CAPTURE(row.label);
    const float floor_hz = sonare::midi::synth::engine_legato(row.mode).lowest_hz;
    REQUIRE(floor_hz > 0.0f);
    // The line is read at the new note's period and the interpolator keeps four
    // samples of stencil margin, which is the clamp the engines apply.
    const double period = kRate / static_cast<double>(floor_hz);
    CAPTURE(floor_hz, period, row.capacity);
    REQUIRE(period <= static_cast<double>(row.capacity - 4));
    ++checked;
  }
  INFO("engines checked: " << checked);
  REQUIRE(checked == std::size(rows));
}
