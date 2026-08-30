/// @file gs_mod_lfo_rate_test.cpp
/// @brief GS MODULATION LFO1 RATE CONTROL (40 2x 03): how far the mod wheel
///        moves the vibrato LFO's frequency.
///
/// The rate is counted rather than inferred. A byte wired to any other quantity
/// would still change the render, so "the render moved" says nothing here; what
/// is measured is how many times the vibrato swings, by tracking the zero
/// crossings of each short window and counting how often that count crosses its
/// own mean. Two of those crossings are one LFO cycle.
///
/// The vibrato is deepened first, and from TONE MODIFY (40 1x 31) rather than
/// from the wheel's own depth: the swing has to be there at every wheel
/// position, or a case comparing a raised wheel against a lowered one would be
/// comparing whether there is a vibrato instead of how fast it is.
///
/// Both voice banks are exercised. They share one Sf2Lfo, so a rate that
/// reached only one of them would mean the scale was applied at the call site
/// of one and not the other (docs/gs.md).

#include <algorithm>
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
/// Part 1 = channel 0 = block nibble 1, in the controller-destination block.
constexpr uint8_t kModBlock = 0x21;
constexpr uint8_t kLfo1Rate = 0x03;
/// The part block, for the TONE MODIFY vibrato depth at 40 1x 31.
constexpr uint8_t kPartBlock = 0x11;
constexpr uint8_t kToneModifyVibDepth = 0x31;
constexpr uint8_t kModWheel = 1;
/// The power-on MODULATION LFO1 RATE CONTROL: no change at any wheel position.
constexpr uint8_t kCentre = 0x40;

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

/// A looped sine, so the pitch swing shows as a clean change in how often the
/// signal crosses zero rather than as a change in its harmonic content.
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

constexpr int kHead = 24000;
constexpr int kRest = 48000;

/// Holds one note for 1.5 s, applying @p before at the prepare and @p during
/// half a second in. The left channel only: the count wants one signal.
Render render(Bank bank, const Writes& before, const Writes& during = nullptr) {
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
  if (before) before(player);

  std::vector<float> left(static_cast<size_t>(kHead + kRest), 0.0f);
  std::vector<float> right(left.size(), 0.0f);

  player.on_event(0, event(sonare::midi::make_midi1_note_on(0, kChannel, 60, 100)));
  float* head[2] = {left.data(), right.data()};
  player.process(head, 2, kHead);
  if (during) during(player);
  float* tail[2] = {left.data() + kHead, right.data() + kHead};
  player.process(tail, 2, kRest);
  return left;
}

/// Vibrato half-cycles in @p r over [from, to): the zero-crossing count of each
/// 10 ms window tracks the instantaneous frequency, and the number of times
/// that track crosses its own mean is the number of half-swings. A window with
/// no signal in it is dropped rather than counted as a low frequency.
int vibrato_half_cycles(const Render& r, size_t from, size_t to) {
  constexpr size_t kWindow = 480;
  std::vector<double> track;
  for (size_t w = from; w + kWindow <= to; w += kWindow) {
    int crossings = 0;
    double peak = 0.0;
    for (size_t i = w + 1; i < w + kWindow; ++i) {
      peak = std::max(peak, std::fabs(static_cast<double>(r[i])));
      if ((r[i - 1] < 0.0f) != (r[i] < 0.0f)) ++crossings;
    }
    if (peak > 1.0e-4) track.push_back(static_cast<double>(crossings));
  }
  if (track.size() < 4) return 0;
  double mean = 0.0;
  for (const double v : track) mean += v;
  mean /= static_cast<double>(track.size());

  int half_cycles = 0;
  bool above = track[0] > mean;
  for (const double v : track) {
    // A dead band, so a window sitting on the mean does not toggle on noise.
    if (above && v < mean - 0.5) {
      above = false;
      ++half_cycles;
    } else if (!above && v > mean + 0.5) {
      above = true;
      ++half_cycles;
    }
  }
  return half_cycles;
}

void write_rate(Sf2Player& p, uint8_t value) {
  const std::vector<uint8_t> m = dt1(kModBlock, kLfo1Rate, {value});
  p.handle_sysex(m.data(), m.size());
}

/// A deep vibrato from TONE MODIFY (40 1x 31), which is not the wheel's. The
/// mod wheel's own depth would have made every case below compare depth as well
/// as rate — with the wheel down there is no wheel vibrato to count the swings
/// of — so the depth is put somewhere the wheel does not reach and the wheel is
/// left with nothing to change but the rate.
void write_deep_vibrato(Sf2Player& p) {
  const std::vector<uint8_t> m = dt1(kPartBlock, kToneModifyVibDepth, {0x7F});
  p.handle_sysex(m.data(), m.size());
}

void wheel(Sf2Player& p, uint8_t value) {
  p.on_event(0, event(sonare::midi::make_midi1_control_change(0, kChannel, kModWheel, value)));
}

}  // namespace

TEST_CASE("40 2x 03 is worth nothing while the mod wheel is down", "[midi][synth][gs]") {
  // The ends of the range are a factor of six apart in LFO frequency, so a byte
  // that reached the rate without the wheel could not render alike at both.
  for (const Bank bank : {Bank::kSoundFont, Bank::kModel}) {
    const std::string tag = bank == Bank::kSoundFont ? "soundfont" : "model";
    INFO("bank: " << tag);

    const Render slow = render(bank, [](Sf2Player& p) {
      write_deep_vibrato(p);
      write_rate(p, 0x00);
    });
    const Render fast = render(bank, [](Sf2Player& p) {
      write_deep_vibrato(p);
      write_rate(p, 0x7F);
    });
    CHECK(slow == fast);
  }
}

TEST_CASE("the power-on 40 leaves the vibrato rate alone", "[midi][synth][gs]") {
  // The centre is the range's no-op, so a raised wheel against it must not move
  // the LFO. Without this, a rate computed off the raw byte rather than off its
  // distance from 40 would still pass the direction case below.
  for (const Bank bank : {Bank::kSoundFont, Bank::kModel}) {
    const std::string tag = bank == Bank::kSoundFont ? "soundfont" : "model";
    INFO("bank: " << tag);

    const Render up = render(bank, [](Sf2Player& p) {
      write_deep_vibrato(p);
      wheel(p, 127);
    });
    const Render centred = render(bank, [](Sf2Player& p) {
      write_deep_vibrato(p);
      write_rate(p, kCentre);
      wheel(p, 127);
    });
    CHECK(centred == up);
  }
}

TEST_CASE("40 2x 03 changes how often the vibrato swings", "[midi][synth][gs]") {
  for (const Bank bank : {Bank::kSoundFont, Bank::kModel}) {
    const std::string tag = bank == Bank::kSoundFont ? "soundfont" : "model";
    INFO("bank: " << tag);

    auto swings = [bank](uint8_t rate) {
      const Render r = render(bank, [rate](Sf2Player& p) {
        write_deep_vibrato(p);
        write_rate(p, rate);
        wheel(p, 127);
      });
      return vibrato_half_cycles(r, 0, r.size());
    };
    const int slow = swings(0x00);
    const int centred = swings(kCentre);
    const int fast = swings(0x7F);
    INFO("half-cycles slow " << slow << " centred " << centred << " fast " << fast);

    // The centre has to be countable for the two ends to mean anything: a
    // vibrato nothing could count would report every byte as zero swings.
    REQUIRE(centred > 2);
    CHECK(slow < centred);
    CHECK(fast > centred);
  }
}

TEST_CASE("a wheel moved under a held note retunes its vibrato", "[midi][synth][gs]") {
  // The scale reaches the LFO per sample, so a voice started with the wheel
  // down still follows it up — and the phase carries across the change rather
  // than restarting, which is what makes the count in the tail meaningful.
  for (const Bank bank : {Bank::kSoundFont, Bank::kModel}) {
    const std::string tag = bank == Bank::kSoundFont ? "soundfont" : "model";
    INFO("bank: " << tag);

    const Writes armed = [](Sf2Player& p) {
      write_deep_vibrato(p);
      write_rate(p, 0x7F);
    };
    const Render still = render(bank, armed);
    const Render moved = render(bank, armed, [](Sf2Player& p) { wheel(p, 127); });
    CHECK_FALSE(moved == still);

    // Counted over the stretch after the move on both sides, where one has the
    // raised rate and the other still has the power-on one.
    const size_t from = static_cast<size_t>(kHead);
    const int after_move = vibrato_half_cycles(moved, from, moved.size());
    const int unmoved = vibrato_half_cycles(still, from, still.size());
    INFO("half-cycles after the move " << after_move << " vs unmoved " << unmoved);
    CHECK(after_move > unmoved);
  }
}
