/// @file gs_mod_amplitude_test.cpp
/// @brief GS MODULATION AMPLITUDE CONTROL (40 2x 02): what the mod wheel does
///        to a part's own level.
///
/// The byte is a percentage of a linear gain rather than a direction, so it is
/// checked quantitatively: the ends of the range have to land on the amounts
/// they name, not merely on the right side of the centre. The centred byte's
/// asymmetry is part of that — 64 steps sit below the centre and 63 above, so
/// 7F is +98.4 %, and a mapping that normalised both sides by 64 would pass a
/// direction case and fail the ratio.
///
/// The other half of the claim is the fold. The percentage is of the part's own
/// level, which is the gain CC7 and CC11 already set, so -100 % at a raised
/// wheel has to reach exactly what a zero CC7 reaches. That is checked as an
/// identity rather than as silence: on the model bank a fallback piano's
/// note-on strikes the part's shared soundboard outside the voice, so neither
/// route silences the part and both have to leave the same thing behind.
///
/// Both voice banks are exercised. The gain is one field and both banks read
/// it, so a bank that missed the fold would be a bank that missed CC7 too.

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
constexpr uint8_t kAmplitudeControl = 0x02;
constexpr uint8_t kLfo1PitchDepth = 0x04;
constexpr uint8_t kModWheel = 1;
constexpr uint8_t kVolume = 7;
/// The power-on MODULATION AMPLITUDE CONTROL: no change at any wheel position.
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

/// A quiet looped sine: the loudest case here is nearly twice the level of the
/// plainest one, and the ratio is only exact while nothing on the way out has
/// anything to clip.
std::shared_ptr<Sf2File> make_fixture() {
  Sf2Builder b;
  std::vector<float> sine(96);
  for (size_t i = 0; i < sine.size(); ++i) {
    sine[i] = 0.25f * static_cast<float>(std::sin(kTwoPi * static_cast<double>(i) / 32.0));
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

constexpr int kHead = 12000;
constexpr int kRest = 24000;

/// Holds one note for 0.75 s, applying @p before at the prepare and @p during a
/// third of the way in.
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

double energy(const Render& r, size_t from = 0) {
  double sum = 0.0;
  for (size_t i = from; i < r.size(); ++i)
    sum += static_cast<double>(r[i]) * static_cast<double>(r[i]);
  return sum;
}

void write_control(Sf2Player& p, uint8_t value) {
  const std::vector<uint8_t> m = dt1(kModBlock, kAmplitudeControl, {value});
  p.handle_sysex(m.data(), m.size());
}

/// Takes MODULATION LFO1 PITCH DEPTH to zero. The wheel reaches a part's
/// vibrato as well as its level, and the power-on 0A leaves it doing so; a
/// half-raised wheel then buys half the vibrato as well as half the level, and
/// on a bank whose voice changes energy with pitch that lands in the ratio.
void write_no_vibrato(Sf2Player& p) {
  const std::vector<uint8_t> m = dt1(kModBlock, kLfo1PitchDepth, {0x00});
  p.handle_sysex(m.data(), m.size());
}

void wheel(Sf2Player& p, uint8_t value) {
  p.on_event(0, event(sonare::midi::make_midi1_control_change(0, kChannel, kModWheel, value)));
}

void volume(Sf2Player& p, uint8_t value) {
  p.on_event(0, event(sonare::midi::make_midi1_control_change(0, kChannel, kVolume, value)));
}

}  // namespace

TEST_CASE("40 2x 02 is worth nothing while the mod wheel is down", "[midi][synth][gs]") {
  // One end of the range is silence and the other is nearly twice the level, so
  // a byte reaching the gain without the wheel could not render alike at both —
  // nor alike with a part that was never written.
  for (const Bank bank : {Bank::kSoundFont, Bank::kModel}) {
    const std::string tag = bank == Bank::kSoundFont ? "soundfont" : "model";
    INFO("bank: " << tag);

    const Render plain = render(bank, nullptr);
    const Render quiet = render(bank, [](Sf2Player& p) { write_control(p, 0x00); });
    const Render loud = render(bank, [](Sf2Player& p) { write_control(p, 0x7F); });
    REQUIRE(energy(plain) > 0.0);
    CHECK(quiet == plain);
    CHECK(loud == plain);
  }
}

TEST_CASE("the power-on 40 leaves the level alone with the wheel up", "[midi][synth][gs]") {
  // The centre is the range's no-op, so raising the wheel against it must not
  // move the gain. Without this, a fraction taken from the raw byte rather than
  // from its distance from 40 would still pass every case below.
  for (const Bank bank : {Bank::kSoundFont, Bank::kModel}) {
    const std::string tag = bank == Bank::kSoundFont ? "soundfont" : "model";
    INFO("bank: " << tag);

    const Render up = render(bank, [](Sf2Player& p) { wheel(p, 127); });
    const Render centred = render(bank, [](Sf2Player& p) {
      write_control(p, kCentre);
      wheel(p, 127);
    });
    CHECK(centred == up);
  }
}

TEST_CASE("40 2x 02 at 00 lands where a zero CC7 lands", "[midi][synth][gs]") {
  // -100 % of a linear gain is the whole gain, so a raised wheel against 00
  // reaches exactly what taking the part's volume to zero reaches. That is the
  // fold itself: the address is a percentage of the level CC7 sets, and the two
  // therefore cannot land anywhere but the same place.
  //
  // Not stated as silence, which it is on the SoundFont bank and is not on the
  // model one. A fallback piano's note-on strikes the part's shared soundboard
  // directly (sf2_player_midi.cpp), outside the voice and so outside the
  // channel gain, and CC7 zero does not silence that either — which is what
  // makes the comparison against CC7 the claim worth making here.
  for (const Bank bank : {Bank::kSoundFont, Bank::kModel}) {
    const std::string tag = bank == Bank::kSoundFont ? "soundfont" : "model";
    INFO("bank: " << tag);

    const Render muted = render(bank, [](Sf2Player& p) { volume(p, 0); });
    const Render off = render(bank, [](Sf2Player& p) {
      write_control(p, 0x00);
      wheel(p, 127);
    });
    REQUIRE(energy(render(bank, nullptr)) > 0.0);
    CHECK(off == muted);
  }
}

TEST_CASE("40 2x 02 spends the percentage its byte names", "[midi][synth][gs]") {
  // 64 steps below the centre and 63 above, so 7F is +98.4 % and half a wheel
  // is half of that. The vibrato is taken out first: the wheel reaches it too,
  // and half a wheel would otherwise buy half a vibrato as well as half a level.
  //
  // Quantitative on the SoundFont bank only, where a channel gain is the whole
  // of what a render is scaled by. The model bank's piano adds a soundboard
  // strike that no channel gain touches (the case above), so its energy is the
  // voice plus a constant and its ratio is not the gain's square; the ordering
  // is what it can state, and the case above states the rest.
  for (const Bank bank : {Bank::kSoundFont, Bank::kModel}) {
    const std::string tag = bank == Bank::kSoundFont ? "soundfont" : "model";
    INFO("bank: " << tag);

    const double centred = energy(render(bank, [](Sf2Player& p) {
      write_no_vibrato(p);
      wheel(p, 127);
    }));
    REQUIRE(centred > 0.0);
    auto ratio = [bank, centred](uint8_t cc) {
      return energy(render(bank,
                           [cc](Sf2Player& p) {
                             write_no_vibrato(p);
                             write_control(p, 0x7F);
                             wheel(p, cc);
                           })) /
             centred;
    };

    const double full_gain = 1.0 + 63.0 / 64.0;
    // 64/127 of a wheel, which is the position the mod field takes from CC1 64.
    const double half_gain = 1.0 + (63.0 / 64.0) * (64.0 / 127.0);
    const double full = ratio(127);
    const double half = ratio(64);
    INFO("full " << full << " want " << full_gain * full_gain << " / half " << half << " want "
                 << half_gain * half_gain);
    CHECK(half > 1.0);
    CHECK(full > half);
    if (bank == Bank::kSoundFont) {
      CHECK(std::fabs(full - full_gain * full_gain) < 0.001 * full_gain * full_gain);
      CHECK(std::fabs(half - half_gain * half_gain) < 0.001 * half_gain * half_gain);
    }
  }
}

TEST_CASE("a wheel moved under a held note moves its level", "[midi][synth][gs]") {
  // The gain is rebuilt on the CC and read per sample, so a note started with
  // the wheel down still follows it up.
  for (const Bank bank : {Bank::kSoundFont, Bank::kModel}) {
    const std::string tag = bank == Bank::kSoundFont ? "soundfont" : "model";
    INFO("bank: " << tag);

    const Writes armed = [](Sf2Player& p) { write_control(p, 0x00); };
    const Render still = render(bank, armed);
    const Render moved = render(bank, armed, [](Sf2Player& p) { wheel(p, 127); });
    // The same move made through CC7 instead, which is where the wheel is
    // supposed to arrive. Comparing against it rather than against silence for
    // the reason the fold case gives: on the model bank neither one silences
    // the soundboard, and both have to leave exactly the same thing behind.
    const Render cut = render(bank, armed, [](Sf2Player& p) { volume(p, 0); });

    // Measured from the move on: before it the three are the same note, and the
    // head would otherwise dominate whatever the tail did.
    const size_t from = static_cast<size_t>(kHead);
    INFO("moved " << energy(moved, from) << " vs still " << energy(still, from) << " vs cut "
                  << energy(cut, from));
    CHECK(energy(still, from) > 0.0);
    CHECK(energy(moved, from) < 0.5 * energy(still, from));
    CHECK(energy(moved, from) == energy(cut, from));
  }
}
