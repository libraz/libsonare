/// @file gs_key_range_test.cpp
/// @brief GS KEY RANGE (40 1x 1D / 1E): the lowest and highest key a part
///        receives at all.
///
/// The two bytes are not a mute. A key outside the range is not received, so it
/// takes no voice, chokes nothing that is already sounding and does not spend
/// the part's armed portamento — which is why the test sits ahead of all three
/// in note_on rather than beside the voice allocation. Rendering silence proves
/// only the first of those; the choke and the portamento need a note already
/// playing to be taken away from, and each is checked against the same strike
/// with the range open so that "nothing happened" cannot pass by the part being
/// inert to begin with.
///
/// Both voice banks are exercised, since a refused key must not reach the model
/// floor by the other route (docs/gs.md).

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

constexpr uint8_t kChannel = 0;
/// Part 1 = channel 0 = block nibble 1.
constexpr uint8_t kPartBlock = 0x11;
constexpr uint8_t kKeyRangeLow = 0x1D;
constexpr uint8_t kKeyRangeHigh = 0x1E;

/// Which voice bank answers the note: the SoundFont one, or the model floor a
/// player with no SoundFont falls to.
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

/// Applies @p setup, plays @p notes in order at 0.1 s apart, and returns the
/// interleaved render. A -1 slot sends no note-on and only lets the time pass,
/// which is what a strike has to be compared against to say it did nothing.
Render render(Bank bank, const Writes& setup, const std::vector<int>& notes) {
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

  constexpr int kStep = 4800;
  constexpr int kTail = 9600;
  const int total = kStep * static_cast<int>(notes.size()) + kTail;
  std::vector<float> left(static_cast<size_t>(total), 0.0f);
  std::vector<float> right(static_cast<size_t>(total), 0.0f);

  int at = 0;
  for (const int note : notes) {
    if (note >= 0) {
      player.on_event(
          0, event(sonare::midi::make_midi1_note_on(0, kChannel, static_cast<uint8_t>(note), 100)));
    }
    float* chans[2] = {left.data() + at, right.data() + at};
    player.process(chans, 2, kStep);
    at += kStep;
  }
  float* tail[2] = {left.data() + at, right.data() + at};
  player.process(tail, 2, kTail);

  Render out;
  out.reserve(left.size() * 2);
  for (size_t i = 0; i < left.size(); ++i) {
    out.push_back(left[i]);
    out.push_back(right[i]);
  }
  return out;
}

bool silent(const Render& r) {
  for (const float v : r) {
    if (v != 0.0f) return false;
  }
  return true;
}

void write_range(Sf2Player& p, uint8_t lo, uint8_t hi) {
  const std::vector<uint8_t> a = dt1(kPartBlock, kKeyRangeLow, {lo});
  p.handle_sysex(a.data(), a.size());
  const std::vector<uint8_t> b = dt1(kPartBlock, kKeyRangeHigh, {hi});
  p.handle_sysex(b.data(), b.size());
}

}  // namespace

TEST_CASE("40 1x 1D/1E refuse the keys outside the part's range", "[midi][synth][gs]") {
  for (const Bank bank : {Bank::kSoundFont, Bank::kModel}) {
    const std::string tag = bank == Bank::kSoundFont ? "soundfont" : "model";
    INFO("bank: " << tag);

    // A split at 48: the part takes 48 and up, and 47 is the key below it.
    const Writes split = [](Sf2Player& p) { write_range(p, 48, 0x7F); };
    CHECK(silent(render(bank, split, {47})));
    CHECK_FALSE(silent(render(bank, split, {48})));

    const Writes ceiling = [](Sf2Player& p) { write_range(p, 0x00, 72); };
    CHECK(silent(render(bank, ceiling, {73})));
    CHECK_FALSE(silent(render(bank, ceiling, {72})));

    // The endpoints are inclusive on both sides, and a range that admits one
    // key admits exactly that one.
    const Writes single = [](Sf2Player& p) { write_range(p, 60, 60); };
    CHECK_FALSE(silent(render(bank, single, {60})));
    CHECK(silent(render(bank, single, {59})));
    CHECK(silent(render(bank, single, {61})));

    // A low above the high names no key at all, which is what the bytes say
    // rather than something to be corrected into a range.
    const Writes inverted = [](Sf2Player& p) { write_range(p, 72, 48); };
    CHECK(silent(render(bank, inverted, {60})));

    // Whatever the range refuses, an untouched part takes.
    CHECK_FALSE(silent(render(bank, nullptr, {47})));
    CHECK_FALSE(silent(render(bank, nullptr, {73})));
  }
}

TEST_CASE("a key the part does not receive takes nothing away from it", "[midi][synth][gs]") {
  // MONO chokes everything the part is sounding on every note-on, whatever the
  // key, so it is the strongest witness that the refusal came first: an
  // accepted strike would end the held note and a refused one must not.
  const Writes mono = [](Sf2Player& p) {
    const std::vector<uint8_t> m = dt1(kPartBlock, 0x13, {0x00});
    p.handle_sysex(m.data(), m.size());
  };

  for (const Bank bank : {Bank::kSoundFont, Bank::kModel}) {
    const std::string tag = bank == Bank::kSoundFont ? "soundfont" : "model";
    INFO("bank: " << tag);

    const Writes refuse = [&mono](Sf2Player& p) {
      mono(p);
      write_range(p, 48, 0x7F);
    };
    const Writes accept = [&mono](Sf2Player& p) {
      mono(p);
      write_range(p, 0x00, 0x7F);
    };

    // 60 held, then 47 struck. Refused, the render has to be the one where 47
    // was never sent at all — not merely one where 47 is inaudible.
    CHECK(render(bank, refuse, {60, 47}) == render(bank, refuse, {60, -1}));

    // ... and with the range open the same strike does end the held note, so
    // the equality above is the refusal rather than a part that chokes nothing.
    CHECK_FALSE(render(bank, accept, {60, 47}) == render(bank, accept, {60, -1}));
  }
}
