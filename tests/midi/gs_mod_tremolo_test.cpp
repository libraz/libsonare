/// @file gs_mod_tremolo_test.cpp
/// @brief GS MODULATION LFO1 TVA DEPTH (40 2x 06): the tremolo the mod wheel
///        reaches, and which side of the part's level it is spent on.
///
/// A depth of amplitude cannot be measured on a render's total energy: a byte
/// wired to a plain gain would move that too. What is measured is the ripple in
/// the level — the log-variation of a short-block RMS track, which a monotone
/// decay contributes its own range to once and a tremolo adds two log-depths to
/// per LFO cycle. That reads through the model bank's decaying piano as well as
/// through a held sine.
///
/// The direction is checked block by block rather than on a peak. The depth is
/// spent below the part's level (docs/gs.md), so no block of a tremolo'd render
/// may be louder than the same block without it, while some block has to come
/// back to the top and some has to reach the bottom — which is the shape a
/// bipolar swing around the level would fail in its first half.
///
/// Both voice banks are exercised. The LFO is the one 40 2x 03 retunes, so a
/// depth that reached only one bank would mean it was applied beside that LFO
/// rather than on it.

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
constexpr uint8_t kLfo1TvaDepth = 0x06;
constexpr uint8_t kModWheel = 1;
/// The power-on MODULATION LFO1 TVA DEPTH: no tremolo at any wheel position.
constexpr uint8_t kNone = 0x00;

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

/// A looped sine held at one level, so every wobble in the block track is the
/// tremolo's. The model bank has no such fixture and decays instead, which is
/// what the log-variation measure is chosen to survive.
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
/// 5 ms, which is a fortieth of the model bank's 5 Hz LFO and a twenty-fourth
/// of the SoundFont one — short enough that a block sits on the swing rather
/// than averaging it away.
constexpr size_t kBlock = 240;

/// Holds one note for 1.5 s, applying @p before at the prepare and @p during
/// half a second in. The left channel only: the track wants one signal.
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

/// Block RMS over [from, r.size()), one entry per kBlock samples.
std::vector<double> rms_track(const Render& r, size_t from = 0) {
  std::vector<double> track;
  for (size_t w = from; w + kBlock <= r.size(); w += kBlock) {
    double sum = 0.0;
    for (size_t i = w; i < w + kBlock; ++i) {
      sum += static_cast<double>(r[i]) * static_cast<double>(r[i]);
    }
    track.push_back(std::sqrt(sum / static_cast<double>(kBlock)));
  }
  return track;
}

/// Total log-variation of the block track: how far the level travels, counting
/// every reversal. A decay spends its own range once however long it takes; a
/// tremolo spends two log-depths per LFO cycle on top of it, so the measure
/// rises with depth without the trend having to be removed first.
double wobble(const Render& r, size_t from = 0) {
  const std::vector<double> track = rms_track(r, from);
  double peak = 0.0;
  for (const double v : track) peak = std::max(peak, v);
  if (peak <= 0.0) return 0.0;
  // A full-depth trough is exactly silent, so the log needs a floor; it is
  // taken from the track's own peak rather than fixed, and the same floor
  // applies to every render being compared.
  const double floor = peak * 1.0e-4;
  double sum = 0.0;
  for (size_t i = 1; i < track.size(); ++i) {
    sum += std::fabs(std::log(std::max(track[i], floor) / std::max(track[i - 1], floor)));
  }
  return sum;
}

double energy(const Render& r) {
  double sum = 0.0;
  for (const float v : r) sum += static_cast<double>(v) * static_cast<double>(v);
  return sum;
}

void write_depth(Sf2Player& p, uint8_t value) {
  const std::vector<uint8_t> m = dt1(kModBlock, kLfo1TvaDepth, {value});
  p.handle_sysex(m.data(), m.size());
}

void wheel(Sf2Player& p, uint8_t value) {
  p.on_event(0, event(sonare::midi::make_midi1_control_change(0, kChannel, kModWheel, value)));
}

}  // namespace

TEST_CASE("40 2x 06 is worth nothing while the mod wheel is down", "[midi][synth][gs]") {
  // The two ends of the range are the difference between no tremolo and a swing
  // to silence, so a byte that reached the amplitude without the wheel could not
  // render alike at both. Nothing is engaged by writing the address, so the
  // identity is against an untouched part as well as between the two ends.
  for (const Bank bank : {Bank::kSoundFont, Bank::kModel}) {
    const std::string tag = bank == Bank::kSoundFont ? "soundfont" : "model";
    INFO("bank: " << tag);

    const Render plain = render(bank, nullptr);
    const Render deep = render(bank, [](Sf2Player& p) { write_depth(p, 0x7F); });
    REQUIRE(energy(plain) > 0.0);
    CHECK(plain == deep);
  }
}

TEST_CASE("the power-on 00 leaves the level alone with the wheel up", "[midi][synth][gs]") {
  // The other half of the identity: 00 is the range's no-op, so raising the
  // wheel against it must not move the amplitude either. Without this, a depth
  // taken as 1 minus the byte rather than as the byte would still pass below.
  for (const Bank bank : {Bank::kSoundFont, Bank::kModel}) {
    const std::string tag = bank == Bank::kSoundFont ? "soundfont" : "model";
    INFO("bank: " << tag);

    const Render up = render(bank, [](Sf2Player& p) { wheel(p, 127); });
    const Render written = render(bank, [](Sf2Player& p) {
      write_depth(p, kNone);
      wheel(p, 127);
    });
    CHECK(written == up);
  }
}

TEST_CASE("40 2x 06 makes the part's level swing", "[midi][synth][gs]") {
  for (const Bank bank : {Bank::kSoundFont, Bank::kModel}) {
    const std::string tag = bank == Bank::kSoundFont ? "soundfont" : "model";
    INFO("bank: " << tag);

    auto at = [bank](uint8_t depth) {
      return wobble(render(bank, [depth](Sf2Player& p) {
        write_depth(p, depth);
        wheel(p, 127);
      }));
    };
    const double none = at(kNone);
    const double half = at(0x40);
    const double deep = at(0x7F);
    INFO("wobble none " << none << " / half " << half << " / deep " << deep);

    // A depth, not a switch: the middle byte buys a swing between the two ends.
    CHECK(half > none);
    CHECK(deep > half);
  }
}

TEST_CASE("40 2x 06 spends its depth below the part's level", "[midi][synth][gs]") {
  // The claim the shape rests on. A swing around the level would take a
  // full-depth part to twice its volume at the LFO's peak, which is not what a
  // depth means; the peak here is the level the part was given and the trough
  // is that level less the depth.
  for (const Bank bank : {Bank::kSoundFont, Bank::kModel}) {
    const std::string tag = bank == Bank::kSoundFont ? "soundfont" : "model";
    INFO("bank: " << tag);

    const std::vector<double> plain = rms_track(render(bank, [](Sf2Player& p) { wheel(p, 127); }));
    const std::vector<double> deep = rms_track(render(bank, [](Sf2Player& p) {
      write_depth(p, 0x7F);
      wheel(p, 127);
    }));
    REQUIRE(plain.size() == deep.size());

    double loudest = 0.0;
    double quietest = 2.0;
    size_t louder_blocks = 0;
    for (size_t i = 0; i < plain.size(); ++i) {
      if (plain[i] <= 0.0) continue;
      const double ratio = deep[i] / plain[i];
      loudest = std::max(loudest, ratio);
      quietest = std::min(quietest, ratio);
      // A block's own averaging carries a little of the swing either side of
      // the LFO's peak, so the ceiling is 1 rather than 1 exactly.
      if (ratio > 1.0 + 1.0e-3) ++louder_blocks;
    }
    INFO("ratio range " << quietest << " .. " << loudest << ", blocks over 1: " << louder_blocks);
    CHECK(louder_blocks == 0);
    // Non-vacuous in both directions: the swing has to reach the top and the
    // bottom, or "never louder" would also be satisfied by a plain attenuation.
    CHECK(loudest > 0.9);
    CHECK(quietest < 0.2);
  }
}

TEST_CASE("a wheel moved under a held note starts the tremolo", "[midi][synth][gs]") {
  // The depth reaches the voice per sample, so a note started with the wheel
  // down still begins to tremble when it comes up. Measured after the move on
  // both sides, where one is swinging and the other is not.
  for (const Bank bank : {Bank::kSoundFont, Bank::kModel}) {
    const std::string tag = bank == Bank::kSoundFont ? "soundfont" : "model";
    INFO("bank: " << tag);

    const Writes armed = [](Sf2Player& p) { write_depth(p, 0x7F); };
    const Render still = render(bank, armed);
    const Render moved = render(bank, armed, [](Sf2Player& p) { wheel(p, 127); });
    CHECK_FALSE(moved == still);

    const size_t from = static_cast<size_t>(kHead);
    INFO("wobble after the move " << wobble(moved, from) << " vs unmoved " << wobble(still, from));
    CHECK(wobble(moved, from) > wobble(still, from));
  }
}
