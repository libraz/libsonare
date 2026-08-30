/// @file gs_drum_setup_test.cpp
/// @brief The GS drum setup block (41 mn rr) against the drum NRPNs it aliases.
///
/// docs/gs.md's one-storage-location table names five drum parameters reachable
/// from a SysEx address and an NRPN alike, and says the mapping is verified by a
/// round-trip: written from either side, read back from either side, equal. That
/// is what this file is. Equality is asserted on the rendered audio rather than
/// on a mirror, because the storage has no accessor and audio is what a second
/// copy would diverge in.
///
/// Every case is guarded against vacuity: a parameter whose write changed
/// nothing would make "the two renders agree" true for the wrong reason, so each
/// probe is also required to move the render away from the untouched baseline.

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "midi/midi_event.h"
#include "midi/synth/gs_address_table.h"
#include "midi/synth/gs_layer.h"
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

constexpr double kOutRate = 24000.0;
constexpr double kTwoPi = 6.28318530717958647692;
/// The rhythm part the stimulus plays, and the two notes it strikes.
constexpr uint8_t kDrumChannel = 9;
constexpr uint8_t kNoteA = 38;
constexpr uint8_t kNoteB = 42;
/// Long enough for the power-on 340 ms delay tap to land inside the render, so
/// a delay-send probe has something to move.
constexpr int kHeld = 6000;
constexpr int kTail = 12000;

MidiEvent event(const sonare::midi::Ump& ump) {
  MidiEvent e;
  e.ump = ump;
  return e;
}

/// Bank 128 program 0 is a looped sine carrying its own reverb and chorus send
/// generators, so the 41 m5 / 41 m6 multiplicands have a send to scale whatever
/// the part is sending.
std::shared_ptr<Sf2File> make_fixture() {
  Sf2Builder b;
  std::vector<float> sine(96);
  for (size_t i = 0; i < sine.size(); ++i) {
    sine[i] = 0.5f * static_cast<float>(std::sin(kTwoPi * static_cast<double>(i) / 32.0));
  }
  const int sine_id = b.add_sample("sine", sine, 32000, 60, 32, 96);

  Sf2Builder::ZoneSpec zone;
  zone.gens.push_back({54 /*sampleModes*/, 1});
  zone.gens.push_back({16 /*reverbEffectsSend*/, 500});
  zone.gens.push_back({15 /*chorusEffectsSend*/, 500});
  zone.target = sine_id;
  const int inst = b.add_instrument("sineinst", {zone});

  Sf2Builder::ZoneSpec pz;
  pz.target = inst;
  b.add_preset("Kit", 128, 0, {pz});

  const auto bytes = b.build();
  auto sf2 = std::make_shared<Sf2File>();
  std::string error;
  REQUIRE(sf2->parse(bytes.data(), bytes.size(), &error));
  return sf2;
}

struct StereoRender {
  std::vector<float> left;
  std::vector<float> right;
};

void cc(Sf2Player& p, uint8_t channel, uint8_t controller, uint8_t value) {
  p.on_event(0, event(sonare::midi::make_midi1_control_change(0, channel, controller, value)));
}

/// NRPN (msb, lsb) = value. The drum NRPNs put the parameter in the msb and the
/// drum note in the lsb.
void nrpn(Sf2Player& p, uint8_t msb, uint8_t lsb, uint8_t value) {
  cc(p, kDrumChannel, 99, msb);
  cc(p, kDrumChannel, 98, lsb);
  cc(p, kDrumChannel, 6, value);
}

/// A framed Roland DT1 write of @p data at @p addr, with its checksum.
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

/// The 41 mn rr address for drum map @p map (ZERO-based here, where 40 1x 15's
/// value is one-based), parameter nibble @p param, drum note @p note.
uint32_t drum_addr(uint8_t map, uint8_t param, uint8_t note) {
  return 0x410000u | (static_cast<uint32_t>(map) << 12) | (static_cast<uint32_t>(param) << 8) |
         note;
}

void sysex(Sf2Player& p, const std::vector<uint8_t>& msg) {
  p.handle_sysex(msg.data(), msg.size());
}

/// 40 1x 15 USE FOR RHYTHM PART on the drum channel (block 0), whose value is
/// one-based: 01 is MAP1.
void use_for_rhythm(Sf2Player& p, uint8_t map_value) { sysex(p, dt1(0x401015u, {map_value})); }

using Writes = std::function<void(Sf2Player&)>;

/// One render of the stimulus with @p writes applied before the first note-on.
StereoRender render(const Writes& writes) {
  Sf2PlayerConfig cfg;
  cfg.gain = 1.0f;
  cfg.synth_fallback = false;
  // Offline: process() realises the pending GS state inline, the path a bounce
  // takes and the one handle_sysex feeds.
  cfg.realize_efx_inline = true;
  Sf2Player player(cfg);
  player.set_soundfont(make_fixture());
  player.prepare(kOutRate, 256);
  // Both power on at zero, so without them neither unit carries signal and no
  // chorus or delay probe could move anything.
  cc(player, kDrumChannel, 93, 64);
  cc(player, kDrumChannel, 94, 64);
  if (writes) writes(player);

  StereoRender out;
  out.left.assign(static_cast<size_t>(kHeld + kTail), 0.0f);
  out.right.assign(static_cast<size_t>(kHeld + kTail), 0.0f);
  float* chans[2] = {out.left.data(), out.right.data()};
  player.on_event(0,
                  event(sonare::midi::make_midi1_note_on(0, kDrumChannel, kNoteA, uint8_t{110})));
  player.on_event(0,
                  event(sonare::midi::make_midi1_note_on(0, kDrumChannel, kNoteB, uint8_t{110})));
  player.process(chans, 2, kHeld);
  chans[0] += kHeld;
  chans[1] += kHeld;
  player.on_event(0, event(sonare::midi::make_midi1_note_off(0, kDrumChannel, kNoteA, uint8_t{0})));
  player.on_event(0, event(sonare::midi::make_midi1_note_off(0, kDrumChannel, kNoteB, uint8_t{0})));
  player.process(chans, 2, kTail);
  return out;
}

/// One strike of @p note released by a note-off on @p off_note, on whichever
/// bank @p soundfont selects: the fixture answers every note, or nothing does
/// and the physical-model floor takes it.
StereoRender render_one(uint8_t note, uint8_t off_note, bool soundfont, const Writes& writes) {
  Sf2PlayerConfig cfg;
  cfg.gain = 1.0f;
  cfg.synth_fallback = !soundfont;
  cfg.realize_efx_inline = true;
  Sf2Player player(cfg);
  if (soundfont) player.set_soundfont(make_fixture());
  player.prepare(kOutRate, 256);
  cc(player, kDrumChannel, 93, 64);
  cc(player, kDrumChannel, 94, 64);
  if (writes) writes(player);

  StereoRender out;
  out.left.assign(static_cast<size_t>(kHeld + kTail), 0.0f);
  out.right.assign(static_cast<size_t>(kHeld + kTail), 0.0f);
  float* chans[2] = {out.left.data(), out.right.data()};
  player.on_event(0, event(sonare::midi::make_midi1_note_on(0, kDrumChannel, note, uint8_t{110})));
  player.process(chans, 2, kHeld);
  chans[0] += kHeld;
  chans[1] += kHeld;
  player.on_event(0,
                  event(sonare::midi::make_midi1_note_off(0, kDrumChannel, off_note, uint8_t{0})));
  player.process(chans, 2, kTail);
  return out;
}

bool identical(const StereoRender& a, const StereoRender& b) {
  return a.left == b.left && a.right == b.right;
}

/// One aliased parameter: the NRPN msb, the 41 mn rr parameter nibble, and two
/// values that render differently from each other and from the untouched kit.
struct Aliased {
  const char* name;
  uint8_t nrpn_msb;
  uint8_t param_nibble;
  uint8_t first;
  uint8_t second;
};

constexpr std::array<Aliased, 5> kAliased = {{
    {"LEVEL", 0x1A, 0x2, 0x28, 0x60},
    // 00 is the one value the two sides disagree on by design, and it has its
    // own case below; both probes here are ordinary positions.
    {"PANPOT", 0x1C, 0x4, 0x01, 0x7F},
    {"REVERB SEND", 0x1D, 0x5, 0x14, 0x64},
    {"CHORUS SEND", 0x1E, 0x6, 0x14, 0x64},
    {"DELAY SEND", 0x1F, 0x9, 0x14, 0x64},
}};

}  // namespace

TEST_CASE("a GS drum parameter is one storage location reached from two sides",
          "[midi][synth][gs]") {
  const StereoRender untouched = render(nullptr);
  for (const Aliased& p : kAliased) {
    INFO(p.name);
    auto by_nrpn = [&p](uint8_t value) {
      return [&p, value](Sf2Player& player) { nrpn(player, p.nrpn_msb, kNoteA, value); };
    };
    auto by_sysex = [&p](uint8_t value) {
      return [&p, value](Sf2Player& player) {
        sysex(player, dt1(drum_addr(0, p.param_nibble, kNoteA), {value}));
      };
    };

    const StereoRender nrpn_first = render(by_nrpn(p.first));
    const StereoRender sysex_first = render(by_sysex(p.first));
    const StereoRender nrpn_second = render(by_nrpn(p.second));
    const StereoRender sysex_second = render(by_sysex(p.second));

    // Vacuity guards: a probe that moved nothing would satisfy every equality
    // below without the two sides sharing anything at all.
    CHECK_FALSE(identical(nrpn_first, untouched));
    CHECK_FALSE(identical(nrpn_second, untouched));
    CHECK_FALSE(identical(nrpn_first, nrpn_second));

    // Written from either side, the same value sounds the same.
    CHECK(identical(nrpn_first, sysex_first));
    CHECK(identical(nrpn_second, sysex_second));

    // And in both orders the second write wins outright, which a second copy
    // holding the first side's value could not do.
    const StereoRender nrpn_then_sysex = render([&](Sf2Player& player) {
      by_nrpn(p.first)(player);
      by_sysex(p.second)(player);
    });
    const StereoRender sysex_then_nrpn = render([&](Sf2Player& player) {
      by_sysex(p.first)(player);
      by_nrpn(p.second)(player);
    });
    CHECK(identical(nrpn_then_sysex, nrpn_second));
    CHECK(identical(sysex_then_nrpn, nrpn_second));
  }
}

TEST_CASE("41 m1 rr sounds another note and still hangs on the struck key", "[midi][synth][gs]") {
  const Writes play_b = [](Sf2Player& player) {
    sysex(player, dt1(drum_addr(0, 0x1, kNoteA), {kNoteB}));
  };

  // The only drum-setup parameter with no NRPN alias, and the only one whose
  // reset value is the address's own note rather than a constant, so it is
  // checked by identity: A with PLAY NOTE B has to be B, not merely not-A.
  for (const bool soundfont : {true, false}) {
    INFO((soundfont ? "soundfont bank" : "model bank"));
    const StereoRender struck_a = render_one(kNoteA, kNoteA, soundfont, nullptr);
    const StereoRender struck_b = render_one(kNoteB, kNoteB, soundfont, nullptr);
    // Two notes that already sound alike would make the identity below true
    // without the write reaching anything.
    CHECK_FALSE(identical(struck_a, struck_b));
    CHECK(identical(render_one(kNoteA, kNoteA, soundfont, play_b), struck_b));
  }

  // The struck key still holds the voice, so a note-off addressed to the note
  // that SOUNDS releases nothing. Only on the SoundFont bank: the model floor's
  // kit pieces are one-shots, which no note-off releases either way.
  const StereoRender released = render_one(kNoteA, kNoteA, true, play_b);
  const StereoRender hanging = render_one(kNoteA, kNoteB, true, play_b);
  CHECK_FALSE(identical(released, hanging));
}

TEST_CASE("a GS drum setup write lands in the map the address names", "[midi][synth][gs]") {
  const StereoRender untouched = render(nullptr);
  const uint8_t kSilent = 0x00;  // LEVEL 0, the loudest change a probe can make

  SECTION("the address nibble is zero-based, so 41 02 rr is the map part 10 powers on reading") {
    const StereoRender map1 = render(
        [&](Sf2Player& player) { sysex(player, dt1(drum_addr(0, 0x2, kNoteA), {kSilent})); });
    CHECK_FALSE(identical(map1, untouched));
  }

  SECTION("a write to map 2's nibble does not reach a part on map 1") {
    const StereoRender map2 = render(
        [&](Sf2Player& player) { sysex(player, dt1(drum_addr(1, 0x2, kNoteA), {kSilent})); });
    CHECK(identical(map2, untouched));
  }

  SECTION("and does reach the part once it is moved onto map 2") {
    // The VALUE is one-based where the address nibble is not: 02 is MAP2.
    const StereoRender moved = render([&](Sf2Player& player) {
      use_for_rhythm(player, 0x02);
      sysex(player, dt1(drum_addr(1, 0x2, kNoteA), {kSilent}));
    });
    const StereoRender moved_only = render([](Sf2Player& player) { use_for_rhythm(player, 0x02); });
    CHECK_FALSE(identical(moved, moved_only));
  }

  SECTION("a map the machine does not have is ignored, not folded into one it does") {
    // The mid byte carries m and the parameter nibble in seven bits, so the
    // highest map a file can even address is 7; there is no eighth to test.
    for (uint8_t map = 2; map < 8; ++map) {
      INFO("map nibble " << static_cast<int>(map));
      const StereoRender out = render(
          [&](Sf2Player& player) { sysex(player, dt1(drum_addr(map, 0x2, kNoteA), {kSilent})); });
      CHECK(identical(out, untouched));
    }
  }
}

TEST_CASE("a GS drum setup run applies every byte, not the first", "[midi][synth][gs]") {
  // A file writes the block as a run over consecutive notes. The run below
  // starts at kNoteA and reaches kNoteB, so both sounding notes are covered and
  // the bytes between them land on notes nothing strikes.
  std::vector<uint8_t> data;
  for (uint8_t note = kNoteA; note <= kNoteB; ++note) {
    data.push_back(static_cast<uint8_t>(note == kNoteA ? 0x20 : 0x50));
  }
  const StereoRender run =
      render([&](Sf2Player& player) { sysex(player, dt1(drum_addr(0, 0x2, kNoteA), data)); });
  const StereoRender one_by_one = render([&](Sf2Player& player) {
    for (size_t i = 0; i < data.size(); ++i) {
      sysex(player, dt1(drum_addr(0, 0x2, static_cast<uint8_t>(kNoteA + i)), {data[i]}));
    }
  });
  const StereoRender first_only =
      render([&](Sf2Player& player) { sysex(player, dt1(drum_addr(0, 0x2, kNoteA), {data[0]})); });
  CHECK(identical(run, one_by_one));
  // kNoteB took the run's last byte, which applying only the first would miss.
  CHECK_FALSE(identical(run, first_only));
}

TEST_CASE("41 m4 rr 00 is centre where NRPN 1C 00 is hard left", "[midi][synth][gs]") {
  // docs/gs.md: the address's 00 is RANDOM on the hardware and answers centre
  // here, while the NRPN has no RANDOM value and 00 is the leftmost position.
  // One storage location, two entry points, and this is the value they part
  // company on — the same split 40 1x 1C and CC10 have.
  const StereoRender by_sysex =
      render([](Sf2Player& player) { sysex(player, dt1(drum_addr(0, 0x4, kNoteA), {0x00})); });
  const StereoRender by_nrpn = render([](Sf2Player& player) { nrpn(player, 0x1C, kNoteA, 0x00); });
  const StereoRender centred = render([](Sf2Player& player) { nrpn(player, 0x1C, kNoteA, 0x40); });
  CHECK(identical(by_sysex, centred));
  CHECK_FALSE(identical(by_nrpn, centred));
}

TEST_CASE("a GS drum send scales the part's send and 7F is unity", "[midi][synth][gs]") {
  // The three send rows default to 7F because that is the multiplicand at which
  // the parameter changes nothing: an unwritten one has to mean "the kit's".
  const StereoRender untouched = render(nullptr);
  for (const Aliased& p : kAliased) {
    if (p.nrpn_msb < 0x1D) continue;  // the two sends' aliases and the delay's
    INFO(p.name);
    const StereoRender unity = render([&](Sf2Player& player) {
      sysex(player, dt1(drum_addr(0, p.param_nibble, kNoteA), {0x7F}));
    });
    CHECK(identical(unity, untouched));
    const StereoRender silenced = render([&](Sf2Player& player) {
      sysex(player, dt1(drum_addr(0, p.param_nibble, kNoteA), {0x00}));
    });
    CHECK_FALSE(identical(silenced, untouched));
  }
}
