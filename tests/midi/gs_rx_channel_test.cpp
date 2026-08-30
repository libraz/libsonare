/// @file gs_rx_channel_test.cpp
/// @brief GS RX CHANNEL (40 1x 02): which MIDI channel each part listens to.
///
/// The parameter is not a permutation. 34 of the 40 corpus files that write it
/// move one part onto a channel another part already answers, so what it is
/// used for is layering, and an implementation that routed a message to at most
/// one part would be wrong for most of them. The dispatch therefore hands a
/// channel message to every part claiming the channel, and 16 means the part
/// claims none.
///
/// The tests are about that fan-out rather than about the byte: a layer has to
/// be the two parts together — checked by identity against the same two parts
/// struck on their own channels, not by the render merely changing.

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

/// Part 1 is slot 0 and block nibble 1; part 2 is slot 1 and block nibble 2.
constexpr uint8_t kPart1Block = 0x11;
constexpr uint8_t kPart2Block = 0x12;
/// Part 6 is slot 5 and block nibble 6 — the part whose channel the move case
/// borrows, and which it switches off first so the channel is free.
constexpr uint8_t kPart6Block = 0x16;
constexpr uint8_t kRxChannel = 0x02;
constexpr uint8_t kRxChannelOff = 0x10;

/// The two programs the fixture voices apart, so "both parts sounded" is
/// distinguishable from "one of them sounded twice".
constexpr uint8_t kSineProgram = 0;
constexpr uint8_t kSquareProgram = 40;

enum class Bank : uint8_t { kSoundFont, kModel };

MidiEvent event(const sonare::midi::Ump& ump) {
  MidiEvent e;
  e.ump = ump;
  return e;
}

std::vector<uint8_t> dt1(uint8_t mid, uint8_t lo, const std::vector<uint8_t>& data) {
  std::vector<uint8_t> msg{0xF0, 0x41, 0x10, 0x42, 0x12, 0x40, mid, lo};
  msg.insert(msg.end(), data.begin(), data.end());
  int sum = 0x40 + mid + lo;
  for (const uint8_t b : data) sum += b;
  msg.push_back(static_cast<uint8_t>((128 - (sum % 128)) & 0x7F));
  msg.push_back(0xF7);
  return msg;
}

std::shared_ptr<Sf2File> make_fixture() {
  Sf2Builder b;
  std::vector<float> sine(96);
  std::vector<float> square(96);
  for (size_t i = 0; i < sine.size(); ++i) {
    const double phase = kTwoPi * static_cast<double>(i) / 32.0;
    sine[i] = 0.5f * static_cast<float>(std::sin(phase));
    square[i] = std::sin(phase) >= 0.0 ? 0.35f : -0.35f;
  }
  const int sine_id = b.add_sample("sine1k", sine, 32000, 60, 32, 96);
  const int square_id = b.add_sample("square1k", square, 32000, 60, 32, 96);

  Sf2Builder::ZoneSpec sz;
  sz.gens.push_back({54 /*sampleModes*/, 1});
  sz.target = sine_id;
  const int sine_inst = b.add_instrument("sineinst", {sz});

  Sf2Builder::ZoneSpec qz;
  qz.gens.push_back({54 /*sampleModes*/, 1});
  qz.target = square_id;
  const int square_inst = b.add_instrument("squareinst", {qz});

  Sf2Builder::ZoneSpec pz;
  pz.target = sine_inst;
  b.add_preset("Sine", 0, kSineProgram, {pz});
  pz.target = square_inst;
  b.add_preset("Square", 0, kSquareProgram, {pz});

  const auto bytes = b.build();
  auto sf2 = std::make_shared<Sf2File>();
  std::string error;
  REQUIRE(sf2->parse(bytes.data(), bytes.size(), &error));
  return sf2;
}

struct Render {
  std::vector<float> left;
  std::vector<float> right;
  bool operator==(const Render& o) const { return left == o.left && right == o.right; }
};

using Writes = std::function<void(Sf2Player&)>;
/// Note-ons as (channel, note) pairs, all struck at time zero in order.
using Strikes = std::vector<std::pair<uint8_t, uint8_t>>;

Render render(Bank bank, const Writes& setup, const Strikes& strikes) {
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

  // The two parts are given their programs on the power-on map, before any
  // remap, so a routing change cannot be mistaken for a program landing
  // somewhere else.
  player.on_event(0, event(sonare::midi::make_midi1_program_change(0, 0, kSineProgram)));
  player.on_event(0, event(sonare::midi::make_midi1_program_change(0, 1, kSquareProgram)));
  if (setup) setup(player);

  Render out;
  out.left.assign(19200, 0.0f);
  out.right.assign(19200, 0.0f);
  for (const auto& s : strikes) {
    player.on_event(0, event(sonare::midi::make_midi1_note_on(0, s.first, s.second, 100)));
  }
  float* chans[2] = {out.left.data(), out.right.data()};
  player.process(chans, 2, 19200);
  return out;
}

bool silent(const Render& r) {
  for (const float v : r.left) {
    if (v != 0.0f) return false;
  }
  for (const float v : r.right) {
    if (v != 0.0f) return false;
  }
  return true;
}

void write_rx(Sf2Player& p, uint8_t block, uint8_t channel) {
  const std::vector<uint8_t> msg = dt1(block, kRxChannel, {channel});
  p.handle_sysex(msg.data(), msg.size());
}

}  // namespace

TEST_CASE("writing the power-on RX CHANNEL map is exactly inert", "[midi][synth][gs]") {
  // Every part told to listen to the channel it already listens to. The map is
  // the one real files most often write, so it has to cost nothing.
  const Writes power_on = [](Sf2Player& p) {
    for (uint8_t block = 0; block < 16; ++block) {
      const uint8_t mid = static_cast<uint8_t>(0x10 | block);
      write_rx(p, mid, block == 0 ? 9 : (block <= 9 ? block - 1 : block));
    }
  };
  for (const Bank bank : {Bank::kSoundFont, Bank::kModel}) {
    INFO("bank: " << (bank == Bank::kSoundFont ? "soundfont" : "model"));
    CHECK(render(bank, power_on, {{0, 60}}) == render(bank, nullptr, {{0, 60}}));
    CHECK(render(bank, power_on, {{1, 64}}) == render(bank, nullptr, {{1, 64}}));
  }
}

TEST_CASE("40 1x 02 moves a part onto the channel it names", "[midi][synth][gs]") {
  for (const Bank bank : {Bank::kSoundFont, Bank::kModel}) {
    INFO("bank: " << (bank == Bank::kSoundFont ? "soundfont" : "model"));
    // Channel 6 vacated first — its own part switched off — so that the strike
    // below reaches part 2 alone and this is about the move rather than about
    // the layer the next case covers.
    const Writes vacated = [](Sf2Player& p) { write_rx(p, kPart6Block, kRxChannelOff); };
    const Writes moved = [&vacated](Sf2Player& p) {
      vacated(p);
      write_rx(p, kPart2Block, 5);
    };

    // By identity: part 2 on channel 6 has to be the part 2 that channel 2
    // played, not merely something that channel 6 now makes a sound on.
    CHECK(render(bank, moved, {{5, 64}}) == render(bank, vacated, {{1, 64}}));
    // ... and its own channel no longer reaches it.
    CHECK(silent(render(bank, moved, {{1, 64}})));
    // Neither half of that identity is vacuous: channel 2 reached the part
    // before the move, and channel 6 reached nothing after it was vacated.
    CHECK_FALSE(silent(render(bank, vacated, {{1, 64}})));
    CHECK(silent(render(bank, vacated, {{5, 64}})));
  }
}

TEST_CASE("two parts on one channel both answer it", "[midi][synth][gs]") {
  for (const Bank bank : {Bank::kSoundFont, Bank::kModel}) {
    INFO("bank: " << (bank == Bank::kSoundFont ? "soundfont" : "model"));
    // Part 2 laid over part 1's channel — the arrangement most corpus files
    // that write this parameter end up in.
    const Writes layered = [](Sf2Player& p) { write_rx(p, kPart2Block, 0); };

    // The layer IS the two parts: one strike on channel 0 has to give what
    // striking each on its own channel gives, and not what either gives alone.
    CHECK(render(bank, layered, {{0, 60}}) == render(bank, nullptr, {{0, 60}, {1, 60}}));
    CHECK_FALSE(render(bank, layered, {{0, 60}}) == render(bank, nullptr, {{0, 60}}));
    CHECK_FALSE(render(bank, layered, {{0, 60}}) == render(bank, nullptr, {{1, 60}}));
  }
}

TEST_CASE("RX CHANNEL 16 leaves a part listening to nothing", "[midi][synth][gs]") {
  for (const Bank bank : {Bank::kSoundFont, Bank::kModel}) {
    INFO("bank: " << (bank == Bank::kSoundFont ? "soundfont" : "model"));
    const Writes off = [](Sf2Player& p) { write_rx(p, kPart1Block, kRxChannelOff); };
    CHECK(silent(render(bank, off, {{0, 60}})));
    // Only that part: its neighbour still answers its own channel.
    CHECK(render(bank, off, {{1, 64}}) == render(bank, nullptr, {{1, 64}}));
    CHECK_FALSE(silent(render(bank, nullptr, {{0, 60}})));
  }
}
