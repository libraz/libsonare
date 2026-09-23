/// @file retrigger_test.cpp
/// @brief NativeSynthPatch::retrigger: under kNote a note played again after the
///        first has fully ended renders the same samples; under kFree it does not.

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <string>
#include <vector>

#include "midi/midi_event.h"
#include "midi/synth/native_synth.h"
#include "midi/synth/synth_presets.h"
#include "midi/ump.h"
#include "support/midi_render.h"

namespace {

using sonare::midi::synth::NativeSynth;
using sonare::midi::synth::NativeSynthConfig;
using sonare::midi::synth::SynthEngineMode;
using sonare::midi::synth::SynthRetrigger;
using sonare::test::event;

constexpr double kRate = 48000.0;
constexpr int kBlock = 256;
constexpr int kHoldBlocks = 40;
constexpr int kCaptureBlocks = 120;
constexpr int kMaxTailBlocks = 4000;

std::vector<float> render_blocks(NativeSynth& synth, int blocks) {
  std::vector<float> out;
  std::vector<float> l(kBlock), r(kBlock);
  for (int b = 0; b < blocks; ++b) {
    std::fill(l.begin(), l.end(), 0.0f);
    std::fill(r.begin(), r.end(), 0.0f);
    float* chans[2] = {l.data(), r.data()};
    synth.process(chans, 2, kBlock);
    for (int i = 0; i < kBlock; ++i) {
      out.push_back(l[static_cast<size_t>(i)]);
      out.push_back(r[static_cast<size_t>(i)]);
    }
  }
  return out;
}

/// Plays note 48 for kHoldBlocks, captures kCaptureBlocks from its note-on,
/// waits for the voice to end, idles an odd gap, and plays it again.
struct TwoPlays {
  std::vector<float> first;
  std::vector<float> second;
  bool ended = false;
};

TwoPlays play_config(NativeSynthConfig cfg, SynthRetrigger retrigger);

TwoPlays play_twice(SynthEngineMode mode, SynthRetrigger retrigger) {
  NativeSynthConfig cfg;
  cfg.patch.mode = mode;
  return play_config(cfg, retrigger);
}

/// The bus DC blocker is off: its state settles into a denormal limit cycle
/// (about 1e-43) that is instrument state, not voice state, and never reaches
/// zero between the plays.
TwoPlays play_config(NativeSynthConfig cfg, SynthRetrigger retrigger) {
  cfg.dc_block = false;
  cfg.patch.retrigger = retrigger;
  NativeSynth synth(cfg);
  synth.prepare(kRate, kBlock);
  TwoPlays out;
  auto play = [&synth]() {
    synth.on_event(0, event(sonare::midi::make_midi1_note_on(0, 0, 48, 100)));
    std::vector<float> a = render_blocks(synth, kHoldBlocks);
    synth.on_event(0, event(sonare::midi::make_midi1_note_off(0, 0, 48, 0)));
    std::vector<float> b = render_blocks(synth, kCaptureBlocks - kHoldBlocks);
    a.insert(a.end(), b.begin(), b.end());
    return a;
  };
  out.first = play();
  for (int b = 0; b < kMaxTailBlocks && synth.active_voice_count() > 0; ++b) {
    render_blocks(synth, 1);
  }
  out.ended = synth.active_voice_count() == 0;
  // The bus DC blocker and any shared body keep ringing below the voice; the
  // second play waits for the whole instrument to fall silent.
  for (int b = 0; b < kMaxTailBlocks; ++b) {
    const std::vector<float> idle = render_blocks(synth, 1);
    if (std::all_of(idle.begin(), idle.end(), [](float v) { return v == 0.0f; })) break;
  }
  render_blocks(synth, 7);
  out.second = play();
  return out;
}

float max_diff(const std::vector<float>& a, const std::vector<float>& b) {
  float d = 0.0f;
  for (size_t i = 0; i < a.size() && i < b.size(); ++i) d = std::max(d, std::fabs(a[i] - b[i]));
  return d;
}

float peak(const std::vector<float>& a) {
  float p = 0.0f;
  for (float v : a) p = std::max(p, std::fabs(v));
  return p;
}

}  // namespace

TEST_CASE("note retrigger renders a repeated note identically", "[midi][synth][retrigger]") {
  // Every engine that sounds from a bare patch. The sample engine needs a bank;
  // piano is excluded because its shared soundboard and resonance bank keep
  // ringing (and drawing their own noise) at the bus between the two plays.
  const SynthEngineMode modes[] = {
      SynthEngineMode::kSubtractive,   SynthEngineMode::kKarplusStrong,
      SynthEngineMode::kAdditive,      SynthEngineMode::kPipeOrgan,
      SynthEngineMode::kBowedString,   SynthEngineMode::kReed,
      SynthEngineMode::kBrass,         SynthEngineMode::kFlute,
      SynthEngineMode::kPluckedString, SynthEngineMode::kVocal,
      SynthEngineMode::kFreeReed,      SynthEngineMode::kHarpsichord,
  };
  for (SynthEngineMode mode : modes) {
    CAPTURE(static_cast<int>(mode));
    const TwoPlays note = play_twice(mode, SynthRetrigger::kNote);
    REQUIRE(note.ended);
    REQUIRE(peak(note.first) > 1.0e-3f);
    CHECK(max_diff(note.first, note.second) == 0.0f);
  }
  // FM and modal are silent from a bare patch, so they are reached through a
  // catalog preset instead.
  for (const char* name : {"e-piano", "bell"}) {
    CAPTURE(name);
    const auto* preset = sonare::midi::synth::find_synth_preset(name);
    REQUIRE(preset != nullptr);
    const TwoPlays note = play_config(preset->config, SynthRetrigger::kNote);
    REQUIRE(note.ended);
    REQUIRE(peak(note.first) > 1.0e-3f);
    CHECK(max_diff(note.first, note.second) == 0.0f);
  }
}

TEST_CASE("note retrigger reaches every seeded subtractive variation", "[midi][synth][retrigger]") {
  // Unison jitter and start phases, drift depth and rate, the matrix's random
  // source and the pan scatter all draw from the voice seed.
  NativeSynthConfig cfg;
  cfg.patch.unison = 5;
  cfg.patch.detune_cents = 20.0f;
  cfg.patch.drift_cents = 15.0f;
  cfg.patch.stereo_spread = 1.0f;
  cfg.patch.mod_matrix.routes[0] = {sonare::midi::synth::ModSource::kRandom,
                                    sonare::midi::synth::ModDestination::kCutoffCents, 1200.0f};
  cfg.patch.cutoff_hz = 2000.0f;
  const TwoPlays free = play_config(cfg, SynthRetrigger::kFree);
  REQUIRE(free.ended);
  REQUIRE(max_diff(free.first, free.second) > 1.0e-3f);
  const TwoPlays note = play_config(cfg, SynthRetrigger::kNote);
  REQUIRE(note.ended);
  REQUIRE(max_diff(note.first, note.second) == 0.0f);
}

TEST_CASE("free retrigger varies a repeated note", "[midi][synth][retrigger]") {
  // The contrast that keeps the identity above from being vacuous: the default
  // seeds from the slot and the allocation count, which differ between plays.
  const TwoPlays free = play_twice(SynthEngineMode::kSubtractive, SynthRetrigger::kFree);
  REQUIRE(free.ended);
  REQUIRE(max_diff(free.first, free.second) > 1.0e-3f);
  const TwoPlays note = play_twice(SynthEngineMode::kSubtractive, SynthRetrigger::kNote);
  REQUIRE(max_diff(note.first, note.second) == 0.0f);
}
