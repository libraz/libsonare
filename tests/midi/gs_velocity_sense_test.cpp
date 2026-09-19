/// @file gs_velocity_sense_test.cpp
/// @brief GS VELOCITY SENSE DEPTH and OFFSET (40 1x 1A / 1B): the curve a part
///        puts between the struck velocity and the one its voices are given.
///
/// The manual gives the two a range and a default and no mapping at all, so the
/// curve is libsonare's and these cases are what fixes it. Depth is the slope
/// and pivots on the centre of the velocity axis: a depth of 0 sounds every key
/// alike rather than silencing them, which is what the parameter's name says and
/// is the whole difference from a multiply that runs to zero. Offset then moves
/// the curve as a whole.
///
/// The identity at the power-on 40/40 is checked first and bit-exactly, because
/// a curve applied unconditionally would pass every "did the audio move" case
/// here while moving every render in the repository.
///
/// Both voice banks are exercised: the part reshapes the velocity ahead of the
/// choice of bank, so a shaping that reached one of them only would be half an
/// implementation (docs/gs.md).

#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "midi/midi_event.h"
#include "midi/synth/gs_layer.h"
#include "midi/synth/sf2_file.h"
#include "midi/synth/sf2_player.h"
#include "midi/ump.h"
#include "support/midi_render.h"
#include "support/sf2_builder.h"

namespace {

using sonare::midi::MidiEvent;
using sonare::midi::synth::gs_velocity_sense;
using sonare::midi::synth::Sf2File;
using sonare::midi::synth::Sf2Player;
using sonare::midi::synth::Sf2PlayerConfig;
using sonare::test::Sf2Builder;

constexpr double kOutRate = 48000.0;
constexpr double kTwoPi = 6.28318530717958647692;

constexpr uint8_t kChannel = 0;
/// Part 1 = channel 0 = block nibble 1.
constexpr uint8_t kPartBlock = 0x11;
constexpr uint8_t kDepth = 0x1A;
constexpr uint8_t kOffset = 0x1B;

enum class Bank : uint8_t { kSoundFont, kModel };

using sonare::test::event;

std::vector<uint8_t> dt1(uint8_t mid, uint8_t lo, uint8_t value) {
  std::vector<uint8_t> msg{0xF0, 0x41, 0x10, 0x42, 0x12, 0x40, mid, lo, value};
  const int sum = 0x40 + mid + lo + value;
  msg.push_back(static_cast<uint8_t>((128 - (sum % 128)) & 0x7F));
  msg.push_back(0xF7);
  return msg;
}

std::shared_ptr<Sf2File> make_fixture() {
  Sf2Builder b;
  std::vector<float> sine(96);
  for (size_t i = 0; i < sine.size(); ++i) {
    sine[i] = 0.5f * static_cast<float>(std::sin(kTwoPi * static_cast<double>(i) / 32.0));
  }
  const int sine_id = b.add_sample("sine1k", sine, 32000, 60, 32, 96);

  Sf2Builder::ZoneSpec zone;
  zone.gens.push_back({54 /*sampleModes*/, 1});
  zone.target = sine_id;
  const int inst = b.add_instrument("sineinst", {zone});

  Sf2Builder::ZoneSpec pz;
  pz.target = inst;
  b.add_preset("Sine", 0, 0, {pz});

  const auto bytes = b.build();
  auto sf2 = std::make_shared<Sf2File>();
  std::string error;
  REQUIRE(sf2->parse(bytes.data(), bytes.size(), &error));
  return sf2;
}

using Render = std::vector<float>;
using Writes = std::function<void(Sf2Player&)>;

/// Applies @p setup, strikes middle C at @p velocity, and returns the render.
Render render(Bank bank, const Writes& setup, uint8_t velocity) {
  Sf2PlayerConfig cfg;
  cfg.gain = 1.0f;
  cfg.synth_fallback = bank == Bank::kModel;
#if defined(SONARE_MIDI_WITH_FX)
  cfg.effects.enable_reverb = false;
  cfg.effects.enable_chorus = false;
  cfg.effects.enable_delay = false;
#endif
  Sf2Player player(cfg);
  if (bank == Bank::kSoundFont) player.set_soundfont(make_fixture());
  player.prepare(kOutRate, 256);
  if (setup) setup(player);
  player.on_event(0, event(sonare::midi::make_midi1_note_on(0, kChannel, 60, velocity)));

  std::vector<float> left(9600, 0.0f);
  std::vector<float> right(9600, 0.0f);
  float* chans[2] = {left.data(), right.data()};
  player.process(chans, 2, 9600);

  Render out;
  out.reserve(left.size() * 2);
  for (size_t i = 0; i < left.size(); ++i) {
    out.push_back(left[i]);
    out.push_back(right[i]);
  }
  return out;
}

double level(const Render& r) {
  double acc = 0.0;
  for (const float s : r) acc += static_cast<double>(s) * static_cast<double>(s);
  return std::sqrt(acc / static_cast<double>(r.size()));
}

Writes write(uint8_t lo, uint8_t value) {
  return [lo, value](Sf2Player& p) {
    const std::vector<uint8_t> msg = dt1(kPartBlock, lo, value);
    REQUIRE(p.handle_sysex(msg.data(), msg.size()));
  };
}

}  // namespace

TEST_CASE("the velocity sense curve is the identity at its power-on values", "[midi][gs][vel]") {
  // Written at the centre from either address, the part sounds exactly what an
  // untouched one does — not merely something close to it. A curve applied
  // unconditionally would round the same velocity to a neighbouring one and
  // move every render that never asked for this parameter.
  REQUIRE(gs_velocity_sense(0x40, 0x40, 1) == 1);
  REQUIRE(gs_velocity_sense(0x40, 0x40, 64) == 64);
  REQUIRE(gs_velocity_sense(0x40, 0x40, 127) == 127);

  for (const Bank bank : {Bank::kSoundFont, Bank::kModel}) {
    const Render untouched = render(bank, nullptr, 100);
    REQUIRE(render(bank, write(kDepth, 0x40), 100) == untouched);
    REQUIRE(render(bank, write(kOffset, 0x40), 100) == untouched);
  }
}

TEST_CASE("velocity sense depth is a slope through the centre", "[midi][gs][vel]") {
  // A depth of zero flattens the axis onto the offset rather than onto silence:
  // every key sounds alike, which is what "no velocity sensitivity" means and
  // what separates this from a multiply that runs to zero.
  REQUIRE(gs_velocity_sense(0x00, 0x40, 1) == 64);
  REQUIRE(gs_velocity_sense(0x00, 0x40, 127) == 64);
  // Above the centre the slope steepens, below it flattens, and both keep the
  // pivot: a velocity of 64 is unmoved whatever the depth.
  REQUIRE(gs_velocity_sense(0x00, 0x40, 64) == 64);
  REQUIRE(gs_velocity_sense(0x7F, 0x40, 64) == 64);
  REQUIRE(gs_velocity_sense(0x7F, 0x40, 100) > gs_velocity_sense(0x40, 0x40, 100));
  REQUIRE(gs_velocity_sense(0x20, 0x40, 100) < gs_velocity_sense(0x40, 0x40, 100));

  for (const Bank bank : {Bank::kSoundFont, Bank::kModel}) {
    // A soft strike and a hard one on a part made insensitive land on the same
    // velocity, so the two renders agree where an untouched part's differ.
    const Writes flat = write(kDepth, 0x00);
    REQUIRE(render(bank, flat, 30) == render(bank, flat, 120));
    REQUIRE_FALSE(render(bank, nullptr, 30) == render(bank, nullptr, 120));
    // And the flattened part sounds at the centre rather than falling silent.
    REQUIRE(level(render(bank, flat, 30)) > 1e-4);
  }
}

TEST_CASE("velocity sense offset moves the whole curve", "[midi][gs][vel]") {
  REQUIRE(gs_velocity_sense(0x40, 0x50, 60) == 76);
  REQUIRE(gs_velocity_sense(0x40, 0x30, 60) == 44);
  // Clamped to a velocity a struck note can carry: a shaped 0 is a note-off on
  // the wire, and 127 is the top of the axis.
  REQUIRE(gs_velocity_sense(0x40, 0x00, 1) == 1);
  REQUIRE(gs_velocity_sense(0x40, 0x7F, 127) == 127);

  for (const Bank bank : {Bank::kSoundFont, Bank::kModel}) {
    const double quiet = level(render(bank, write(kOffset, 0x20), 64));
    const double plain = level(render(bank, nullptr, 64));
    const double loud = level(render(bank, write(kOffset, 0x60), 64));
    REQUIRE(quiet < plain);
    REQUIRE(loud > plain);
  }
}

TEST_CASE("velocity sense is out of range or in it, never clamped", "[midi][gs][vel]") {
  // The row accepts 00-7F, so no value a SysEx byte can carry is out of range
  // here; what is checked is that the two addresses are separate storage and
  // that neither reads the other's byte.
  for (const Bank bank : {Bank::kSoundFont, Bank::kModel}) {
    const Render depth_only = render(bank, write(kDepth, 0x20), 100);
    const Render offset_only = render(bank, write(kOffset, 0x20), 100);
    REQUIRE_FALSE(depth_only == offset_only);
  }
}
