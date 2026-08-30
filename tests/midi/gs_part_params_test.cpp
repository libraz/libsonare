/// @file gs_part_params_test.cpp
/// @brief The GS part-parameter block (40 1x xx) as the alias set docs/gs.md
///        says it is: every address here is a second name for a controller,
///        and the promise is that the two share one storage location.
///
/// The promise is checked without a getter, by rendering. A parameter written
/// through its controller and the same parameter written through its SysEx
/// address must produce bit-identical audio — which a second copy of the
/// storage cannot do, because only one of the two paths would reach the voice.
/// Each pair also renders untouched, so a pair that moves nothing fails instead
/// of agreeing vacuously.

#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <cstdint>
#include <functional>
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

/// Part 1 = channel 0 = block nibble 1, so every address below is 40 11 xx.
constexpr uint8_t kPartBlock = 0x11;
/// The fixture's second program: the same sample without the vibrato, filter
/// and envelope edits program 0 carries, so choosing it is plainly audible.
constexpr uint8_t kSecondProgram = 40;

MidiEvent event(const sonare::midi::Ump& ump) {
  MidiEvent e;
  e.ump = ump;
  return e;
}

/// A framed Roland DT1 write of @p data at 40 <block> <lo>, with the checksum.
std::vector<uint8_t> dt1(uint8_t block, uint8_t lo, const std::vector<uint8_t>& data) {
  std::vector<uint8_t> msg{0xF0, 0x41, 0x10, 0x42, 0x12, 0x40, block, lo};
  msg.insert(msg.end(), data.begin(), data.end());
  int sum = 0x40 + block + lo;
  for (const uint8_t b : data) sum += b;
  msg.push_back(static_cast<uint8_t>((128 - (sum % 128)) & 0x7F));
  msg.push_back(0xF7);
  return msg;
}

/// Program 0: a square loop that already carries vibrato and a mid filter, so
/// every TONE MODIFY parameter has something of its own to move — a rate or a
/// delay multiplies a depth, and multiplying a depth of zero is inaudible.
std::shared_ptr<Sf2File> make_fixture() {
  Sf2Builder b;

  std::vector<float> square(128);
  for (size_t i = 0; i < square.size(); ++i) {
    double v = 0.0;
    for (int h = 1; h <= 9; h += 2) {
      v += std::sin(kTwoPi * h * static_cast<double>(i) / 64.0) / h;
    }
    square[i] = 0.6f * static_cast<float>(v);
  }
  const int sq_id = b.add_sample("square500", square, 32000, 60, 0, 128);

  Sf2Builder::ZoneSpec zone;
  zone.gens.push_back({54 /*sampleModes*/, 1});
  zone.gens.push_back({8 /*initialFilterFc*/, 8637});  // ~1.2 kHz
  zone.gens.push_back({6 /*vibLfoToPitch*/, 60});      // cents, so rate/delay bite
  zone.gens.push_back({23 /*delayVibLFO*/, -2000});    // ~0.25 s onset
  zone.gens.push_back({34 /*attackVolEnv*/, -2400});   // 0.25 s, so attack bites
  zone.gens.push_back({36 /*decayVolEnv*/, -1200});    // 0.5 s ...
  zone.gens.push_back({37 /*sustainVolEnv*/, 120});    // ... down to -12 dB, or
                                                       // scaling the decay time
                                                       // scales a flat segment
  zone.gens.push_back({38 /*releaseVolEnv*/, -1200});  // 0.5 s, so release bites
  zone.target = sq_id;
  const int inst = b.add_instrument("squareinst", {zone});

  Sf2Builder::ZoneSpec pz;
  pz.target = inst;
  b.add_preset("Square", 0, 0, {pz});
  // A second program, so the TONE NUMBER pair has one to choose. With one, the
  // row would render inert however well it worked.
  Sf2Builder::ZoneSpec plain;
  plain.gens.push_back({54 /*sampleModes*/, 1});
  plain.target = sq_id;
  Sf2Builder::ZoneSpec plain_pz;
  plain_pz.target = b.add_instrument("plaininst", {plain});
  b.add_preset("Square plain", 0, kSecondProgram, {plain_pz});

  const auto bytes = b.build();
  auto sf2 = std::make_shared<Sf2File>();
  std::string error;
  REQUIRE(sf2->parse(bytes.data(), bytes.size(), &error));
  return sf2;
}

using Render = std::vector<float>;

/// Applies @p setup, then plays one note through its release. The buffer is
/// interleaved so a pan change shows up in the comparison.
Render render_with(const std::function<void(Sf2Player&)>& setup, bool with_fx) {
  Sf2PlayerConfig cfg;
  cfg.gain = 1.0f;
#if defined(SONARE_MIDI_WITH_FX)
  cfg.effects.enable_reverb = with_fx;
  cfg.effects.enable_chorus = with_fx;
  cfg.effects.enable_delay = with_fx;
#else
  (void)with_fx;
#endif
  Sf2Player player(cfg);
  player.set_soundfont(make_fixture());
  player.prepare(kOutRate, 256);
  if (setup) setup(player);

  // Long enough that the attack finishes and the decay is well under way before
  // note-off, or scaling the decay time moves nothing the render reaches.
  constexpr int kHeld = 24000;
  constexpr int kTail = 24000;
  std::vector<float> left(kHeld + kTail, 0.0f);
  std::vector<float> right(kHeld + kTail, 0.0f);

  player.on_event(0, event(sonare::midi::make_midi1_note_on(0, 0, 60, 100)));
  float* held[2] = {left.data(), right.data()};
  player.process(held, 2, kHeld);
  player.on_event(0, event(sonare::midi::make_midi1_note_off(0, 0, 60, 0)));
  float* tail[2] = {left.data() + kHeld, right.data() + kHeld};
  player.process(tail, 2, kTail);

  Render out;
  out.reserve(left.size() * 2);
  for (size_t i = 0; i < left.size(); ++i) {
    out.push_back(left[i]);
    out.push_back(right[i]);
  }
  return out;
}

bool identical(const Render& a, const Render& b) {
  if (a.size() != b.size()) return false;
  for (size_t i = 0; i < a.size(); ++i) {
    if (a[i] != b[i]) return false;
  }
  return true;
}

void send_cc(Sf2Player& player, uint8_t cc, uint8_t value) {
  player.on_event(0, event(sonare::midi::make_midi1_control_change(0, 0, cc, value)));
}

void send_nrpn(Sf2Player& player, uint8_t msb, uint8_t lsb, uint8_t value) {
  send_cc(player, 99, msb);
  send_cc(player, 98, lsb);
  send_cc(player, 6, value);
}

/// One row of the docs/gs.md alias table that lives in this block: the SysEx
/// bytes, and the controller message that must reach the same place.
struct Alias {
  const char* name;
  uint8_t lo;                 ///< 40 11 <lo>
  std::vector<uint8_t> data;  ///< what the SysEx side writes
  std::function<void(Sf2Player&)> by_controller;
  bool needs_fx;
};

std::vector<Alias> aliases() {
  return {
      // Two bytes, and the CC0 half only means anything once the program half
      // has chosen a program some variation bank answers differently.
      {"TONE NUMBER = CC0 + program change",
       0x00,
       {0x00, kSecondProgram},
       [](Sf2Player& p) {
         send_cc(p, 0, 0x00);
         p.on_event(0, event(sonare::midi::make_midi1_program_change(0, 0, kSecondProgram)));
       },
       false},
      {"PART LEVEL = CC7", 0x19, {30}, [](Sf2Player& p) { send_cc(p, 7, 30); }, false},
      // 00 is the RANDOM value CC10 has no equivalent for, so the pair is
      // checked away from it and the divergence has its own case below.
      {"PART PANPOT = CC10", 0x1C, {0x10}, [](Sf2Player& p) { send_cc(p, 10, 0x10); }, false},
      {"CHORUS SEND = CC93", 0x21, {127}, [](Sf2Player& p) { send_cc(p, 93, 127); }, true},
      {"REVERB SEND = CC91", 0x22, {127}, [](Sf2Player& p) { send_cc(p, 91, 127); }, true},
      {"DELAY SEND = CC94", 0x2C, {127}, [](Sf2Player& p) { send_cc(p, 94, 127); }, true},
      {"PITCH FINE TUNE = RPN 00 01",
       0x2A,
       {0x50, 0x00},
       [](Sf2Player& p) {
         send_cc(p, 101, 0x00);
         send_cc(p, 100, 0x01);
         send_cc(p, 6, 0x50);
         send_cc(p, 38, 0x00);
       },
       false},
      {"TONE MODIFY1 vibrato rate = NRPN 01 08",
       0x30,
       {110},
       [](Sf2Player& p) { send_nrpn(p, 0x01, 0x08, 110); },
       false},
      {"TONE MODIFY2 vibrato depth = NRPN 01 09",
       0x31,
       {127},
       [](Sf2Player& p) { send_nrpn(p, 0x01, 0x09, 127); },
       false},
      {"TONE MODIFY3 TVF cutoff = NRPN 01 20",
       0x32,
       {104},
       [](Sf2Player& p) { send_nrpn(p, 0x01, 0x20, 104); },
       false},
      {"TONE MODIFY4 TVF resonance = NRPN 01 21",
       0x33,
       {127},
       [](Sf2Player& p) { send_nrpn(p, 0x01, 0x21, 127); },
       false},
      {"TONE MODIFY5 EG attack = NRPN 01 63",
       0x34,
       {127},
       [](Sf2Player& p) { send_nrpn(p, 0x01, 0x63, 127); },
       false},
      {"TONE MODIFY6 EG decay = NRPN 01 64",
       0x35,
       {127},
       [](Sf2Player& p) { send_nrpn(p, 0x01, 0x64, 127); },
       false},
      {"TONE MODIFY7 EG release = NRPN 01 66",
       0x36,
       {127},
       [](Sf2Player& p) { send_nrpn(p, 0x01, 0x66, 127); },
       false},
      {"TONE MODIFY8 vibrato delay = NRPN 01 0A",
       0x37,
       {127},
       [](Sf2Player& p) { send_nrpn(p, 0x01, 0x0A, 127); },
       false},
  };
}

}  // namespace

TEST_CASE("every part-parameter alias writes the storage its controller owns",
          "[midi][synth][gs]") {
  for (const Alias& a : aliases()) {
    INFO(a.name);
#if !defined(SONARE_MIDI_WITH_FX)
    if (a.needs_fx) continue;
#endif
    const Render untouched = render_with(nullptr, a.needs_fx);
    const Render by_cc = render_with(a.by_controller, a.needs_fx);
    const Render by_sysex = render_with(
        [&a](Sf2Player& p) {
          const std::vector<uint8_t> msg = dt1(kPartBlock, a.lo, a.data);
          REQUIRE(p.handle_sysex(msg.data(), msg.size()));
        },
        a.needs_fx);

    // Vacuity guard: a pair neither side moves would agree for the wrong
    // reason.
    CHECK_FALSE(identical(untouched, by_cc));
    CHECK(identical(by_cc, by_sysex));
  }
}

TEST_CASE("a part-parameter run lands on every address it crosses", "[midi][synth][gs]") {
  // TONE MODIFY 1-8 share one table row, so a block dump is where an index that
  // is off by one shows up: written one at a time and written as a run, the
  // eight have to reach the same eight fields.
  const std::vector<uint8_t> block{100, 96, 104, 120, 90, 88, 110, 118};
  const Render as_run = render_with(
      [&block](Sf2Player& p) {
        const std::vector<uint8_t> msg = dt1(kPartBlock, 0x30, block);
        REQUIRE(p.handle_sysex(msg.data(), msg.size()));
      },
      false);
  const Render one_at_a_time = render_with(
      [&block](Sf2Player& p) {
        for (size_t i = 0; i < block.size(); ++i) {
          const std::vector<uint8_t> msg =
              dt1(kPartBlock, static_cast<uint8_t>(0x30 + i), {block[i]});
          REQUIRE(p.handle_sysex(msg.data(), msg.size()));
        }
      },
      false);
  const Render untouched = render_with(nullptr, false);
  CHECK_FALSE(identical(untouched, as_run));
  CHECK(identical(as_run, one_at_a_time));
}

TEST_CASE("PART PANPOT 00 is centre, which is where CC10 00 is not", "[midi][synth][gs]") {
  // The one place the address and its controller deliberately disagree: 00 is
  // RANDOM at 40 1x 1C and hard left at CC10, and libsonare answers RANDOM with
  // centre rather than a value no bounce could reproduce (docs/gs.md).
  auto by_sysex = [](uint8_t value) {
    return render_with(
        [value](Sf2Player& p) {
          const std::vector<uint8_t> msg = dt1(kPartBlock, 0x1C, {value});
          REQUIRE(p.handle_sysex(msg.data(), msg.size()));
        },
        false);
  };
  const Render random = by_sysex(0x00);
  const Render centre = by_sysex(0x40);
  const Render hard_left = render_with([](Sf2Player& p) { send_cc(p, 10, 0x00); }, false);
  CHECK(identical(random, centre));
  CHECK_FALSE(identical(random, hard_left));
}
