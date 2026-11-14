/// @file drum_exclusive_group_test.cpp
/// @brief Drum exclusive/mute groups choke across BOTH voice pools.
///
/// A part sounds through the SoundFont pool where the bank has a zone and
/// through the modelled fallback floor where it does not, so a trimmed GM kit
/// can have an open hi-hat sampled and a closed one modelled. The group is the
/// same on both sides; the engine a voice happens to sound through is not part
/// of the comparison. These cases assert presence and silence rather than a
/// timbre: there is no reference recording for a choke.

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "midi/midi_event.h"
#include "midi/synth/sf2_file.h"
#include "midi/synth/sf2_player.h"
#include "midi/ump.h"
#include "support/midi_render.h"
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

using sonare::test::event;

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

/// Renders the window following a strike of @p second, optionally with @p first
/// left ringing into it. A negative @p first strikes @p second on its own.
std::vector<float> strike_over(int first, uint8_t second) {
  Sf2Player player = make_player();
  if (first >= 0) note_on(player, static_cast<uint8_t>(first));
  render(player, 2048);  // let the first voice establish
  note_on(player, second);
  return render(player, 2048);
}

/// What @p first still contributes once @p second is struck, relative to that
/// strike on its own. A choke drives this to ~0.
///
/// Two things this has to avoid measuring. The peak of the window itself is
/// dominated by the second strike's own attack whether or not the first voice
/// was silenced, so the comparison is against that same strike alone. And a
/// termination is a short fade rather than a cut, so the difference at the
/// instant of the strike is the first voice's full amplitude however fast the
/// fade is -- measuring across it reports ~1.0 for a working choke. Only the
/// second half of the window, well past any termination ramp, answers the
/// question asked.
float residual(uint8_t first, uint8_t second) {
  const std::vector<float> over = strike_over(static_cast<int>(first), second);
  const std::vector<float> alone = strike_over(-1, second);
  const size_t settled = over.size() / 2;
  std::vector<float> difference(over.size() - settled, 0.0f);
  for (size_t i = settled; i < over.size(); ++i) difference[i - settled] = over[i] - alone[i];
  return peak_abs(difference) / std::max(peak_abs(alone), 1e-9f);
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
  REQUIRE(residual(kOpenHat, kClosedHat) < 0.01f);
}

TEST_CASE("A sampled strike chokes a ringing modelled voice of the same group",
          "[midi][sf2][drums]") {
  // The other direction, which the finding's user-impact paragraph does not
  // describe and an author would not naturally test.
  REQUIRE(residual(kClosedHat, kOpenHat) < 0.01f);
}

TEST_CASE("Same-pool choking still works in both pools", "[midi][sf2][drums]") {
  // The positive control: a cross-pool fix that broke either existing
  // within-pool loop would still pass the two cases above.
  REQUIRE(residual(kOpenHat, kPedalHat) < 0.01f);

  Sf2Player player = make_player();
  note_on(player, kClosedHat);
  render(player, 2048);
  note_on(player, kClosedHat);
  REQUIRE(peak_abs(render(player, 2048)) > 0.0f);
}

namespace {

/// A framed Roland DT1 write of @p data at @p addr, with the checksum.
std::vector<uint8_t> dt1(uint32_t addr, const std::vector<uint8_t>& data) {
  const uint8_t a0 = static_cast<uint8_t>((addr >> 16) & 0x7Fu);
  const uint8_t a1 = static_cast<uint8_t>((addr >> 8) & 0x7Fu);
  const uint8_t a2 = static_cast<uint8_t>(addr & 0x7Fu);
  std::vector<uint8_t> msg{0xF0, 0x41, 0x10, 0x42, 0x12, a0, a1, a2};
  msg.insert(msg.end(), data.begin(), data.end());
  int sum = a0 + a1 + a2;
  for (const uint8_t b : data) sum += b;
  msg.push_back(static_cast<uint8_t>((128 - (sum % 128)) & 0x7F));
  msg.push_back(0xF7);
  return msg;
}

/// A modelled key in no group of its own, which the cases move into one.
constexpr uint8_t kGroupedKey = 75;  // Claves

/// What @p first still contributes once kGroupedKey, written into @p group by
/// `41 03 rr` ASSIGN GROUP, is struck over it; the same measure as `residual`.
float residual_after_assign(uint8_t first, uint8_t group) {
  auto run = [group](int ringing) {
    Sf2Player player = make_player();
    const std::vector<uint8_t> msg = dt1(0x410300u | kGroupedKey, {group});
    REQUIRE(player.handle_sysex(msg.data(), msg.size()));
    if (ringing >= 0) note_on(player, static_cast<uint8_t>(ringing));
    render(player, 2048);
    note_on(player, kGroupedKey);
    return render(player, 2048);
  };
  const std::vector<float> over = run(first);
  const std::vector<float> alone = run(-1);
  const size_t settled = over.size() / 2;
  std::vector<float> difference(over.size() - settled, 0.0f);
  for (size_t i = settled; i < over.size(); ++i) difference[i - settled] = over[i] - alone[i];
  return peak_abs(difference) / std::max(peak_abs(alone), 1e-9f);
}

}  // namespace

TEST_CASE("ASSIGN GROUP numbers name the groups the GS drum map prints", "[midi][sf2][drums]") {
  // The write is a raw group number, so a kit whose pairs are right but
  // numbered differently chokes the wrong pair. Each row strikes the pair's
  // sustaining member, then a key written into the map's number for it, and
  // requires the choke; the pair that owned that number before is the control.
  struct Row {
    uint8_t group;
    uint8_t member;  ///< The map's pair for this number.
    uint8_t other;   ///< A pair in a different group.
  };
  const Row rows[] = {
      {1, 46, 72},  // hi-hat / whistle
      {2, 72, 79},  // whistle / cuica
      {3, 74, 81},  // guiro / triangle
      {4, 79, 72},  // cuica / whistle
      {5, 81, 74},  // triangle / guiro
      {6, 87, 79},  // surdo / cuica
      {7, 30, 87},  // scratch / surdo
  };
  for (const Row& r : rows) {
    CAPTURE(r.group, r.member, r.other);
    REQUIRE(ringing_alone(r.member) > 0.01f);
    REQUIRE(ringing_alone(r.other) > 0.01f);
    CHECK(residual_after_assign(r.member, r.group) < 0.01f);
    CHECK(residual_after_assign(r.other, r.group) > 0.01f);
  }
}

TEST_CASE("Scratch Push and Scratch Pull choke each other", "[midi][sf2][drums]") {
  // One exclusive pair in the GS drum map, both modelled here. Both pieces are
  // noise, whose draw differs between two renders, so the choke is read off the
  // voice count rather than off a difference of two renders.
  for (const auto& [first, second] : {std::pair<uint8_t, uint8_t>{30, 29}, {29, 30}}) {
    CAPTURE(first, second);
    Sf2Player player = make_player();
    note_on(player, first);
    render(player, 2048);
    REQUIRE(player.active_voice_count() == 1);
    note_on(player, second);
    render(player, 2048);
    CHECK(player.active_voice_count() == 1);
  }
}
