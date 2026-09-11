/// @file drum_exclusive_group_test.cpp
/// @brief Drum exclusive/mute groups choke across BOTH voice pools.
///
/// A part sounds through the SoundFont pool where the bank has a zone and
/// through the modelled fallback floor where it does not, so a trimmed GM kit
/// can have an open hi-hat sampled and a closed one modelled. The group is the
/// same on both sides; the engine a voice happens to sound through is not part
/// of the comparison. These cases assert presence and silence rather than a
/// timbre: there is no reference recording for a choke.

#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "midi/midi_event.h"
#include "midi/synth/sf2_file.h"
#include "midi/synth/sf2_player.h"
#include "midi/ump.h"
#include "support/sf2_builder.h"

namespace {

using sonare::midi::MidiEvent;
using sonare::midi::synth::Sf2File;
using sonare::midi::synth::Sf2Player;
using sonare::midi::synth::Sf2PlayerConfig;
using sonare::test::Sf2Builder;

constexpr double kOutRate = 48000.0;
constexpr double kTwoPi = 6.28318530717958647692;
constexpr uint8_t kDrumChannel = 9;
constexpr uint8_t kOpenHat = 46;    ///< Covered by the fixture's kit.
constexpr uint8_t kClosedHat = 42;  ///< Deliberately NOT covered.
constexpr uint8_t kPedalHat = 44;   ///< Also covered, for the same-pool control.

MidiEvent event(const sonare::midi::Ump& ump) {
  MidiEvent e;
  e.ump = ump;
  return e;
}

/// Bank 128 program 0, with a long sustaining sine on notes 44 and 46 ONLY.
///
/// The coverage hole is what the cases turn on: note 42 finds no renderable
/// zone and falls through to the modelled floor, while 44 and 46 allocate in the
/// SoundFont pool. Without it every note lands in one pool and a cross-pool
/// defect is invisible. Exclusive class 1 is the value the model bank gives
/// notes 42/44/46 (gm_fallback_drums.cpp), so the two sides share a group.
std::shared_ptr<Sf2File> make_fixture() {
  Sf2Builder b;
  std::vector<float> sine(128);
  for (size_t i = 0; i < sine.size(); ++i) {
    sine[i] = 0.5f * static_cast<float>(std::sin(kTwoPi * static_cast<double>(i) / 64.0));
  }
  const int sine_id = b.add_sample("hat", sine, 32000, 60, 0, 128);

  Sf2Builder::ZoneSpec zone;
  zone.key_lo = kPedalHat;
  zone.key_hi = kOpenHat;  // 44..46: covers 44 and 46, leaves 42 uncovered.
  zone.gens.push_back({54 /*sampleModes*/, 1});
  zone.gens.push_back({57 /*exclusiveClass*/, 1});
  zone.target = sine_id;
  const int inst = b.add_instrument("hatinst", {zone});

  Sf2Builder::ZoneSpec pz;
  pz.target = inst;
  b.add_preset("Kit", 128, 0, {pz});

  const auto bytes = b.build();
  auto sf2 = std::make_shared<Sf2File>();
  std::string error;
  REQUIRE(sf2->parse(bytes.data(), bytes.size(), &error));
  return sf2;
}

Sf2Player make_player() {
  Sf2PlayerConfig cfg;
  cfg.gain = 1.0f;
  cfg.synth_fallback = true;
#if defined(SONARE_MIDI_WITH_FX)
  cfg.effects.enable_reverb = false;
  cfg.effects.enable_chorus = false;
  cfg.effects.enable_delay = false;
#endif
  Sf2Player player(cfg);
  player.set_soundfont(make_fixture());
  player.prepare(kOutRate, 256);
  return player;
}

void note_on(Sf2Player& player, uint8_t note) {
  player.on_event(0, event(sonare::midi::make_midi1_note_on(0, kDrumChannel, note, 100)));
}

float peak_abs(const std::vector<float>& samples) {
  float peak = 0.0f;
  for (const float s : samples) peak = std::max(peak, std::abs(s));
  return peak;
}

std::vector<float> render(Sf2Player& player, int num_samples) {
  std::vector<float> left(static_cast<size_t>(num_samples), 0.0f);
  std::vector<float> right(static_cast<size_t>(num_samples), 0.0f);
  float* chans[2] = {left.data(), right.data()};
  player.process(chans, 2, num_samples);
  return left;
}

/// Peak of what @p first leaves ringing after @p second is struck, relative to
/// what it leaves ringing when nothing follows it. A choke drives this to ~0.
float ringing_after(uint8_t first, uint8_t second) {
  Sf2Player player = make_player();
  note_on(player, first);
  render(player, 2048);  // let it establish
  note_on(player, second);
  return peak_abs(render(player, 2048));
}

float ringing_alone(uint8_t first) {
  Sf2Player player = make_player();
  note_on(player, first);
  render(player, 2048);
  return peak_abs(render(player, 2048));
}

}  // namespace

TEST_CASE("The fixture's coverage hole puts the two hats in different pools",
          "[midi][sf2][drums]") {
  // Load-bearing rather than incidental: if both notes allocated in the same
  // pool, every cross-pool case below would pass against the defect. Both
  // sound, so neither is silent for want of a voice.
  REQUIRE(ringing_alone(kOpenHat) > 0.01f);
  REQUIRE(ringing_alone(kClosedHat) > 0.01f);
}

TEST_CASE("A modelled strike chokes a ringing sampled voice of the same group",
          "[midi][sf2][drums]") {
  // 46 is sampled, 42 is modelled. Before the cross-pool walk the modelled
  // strike could not reach the sampled voice and the open hat rang on.
  REQUIRE(ringing_after(kOpenHat, kClosedHat) < 0.01f * ringing_alone(kOpenHat));
}

TEST_CASE("A sampled strike chokes a ringing modelled voice of the same group",
          "[midi][sf2][drums]") {
  // The other direction, which the finding's user-impact paragraph does not
  // describe and an author would not naturally test.
  REQUIRE(ringing_after(kClosedHat, kOpenHat) < 0.01f * ringing_alone(kClosedHat));
}

TEST_CASE("Same-pool choking still works in both pools", "[midi][sf2][drums]") {
  // The positive control: a cross-pool fix that broke either existing
  // within-pool loop would still pass the two cases above.
  REQUIRE(ringing_after(kOpenHat, kPedalHat) < 0.01f * ringing_alone(kOpenHat));

  Sf2Player player = make_player();
  note_on(player, kClosedHat);
  render(player, 2048);
  note_on(player, kClosedHat);
  REQUIRE(peak_abs(render(player, 2048)) > 0.0f);
}
