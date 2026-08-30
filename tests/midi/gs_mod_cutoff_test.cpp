/// @file gs_mod_cutoff_test.cpp
/// @brief GS MODULATION TVF CUTOFF CONTROL (40 2x 01): how far the mod wheel
///        moves the part's filter cutoff.
///
/// The byte is a depth rather than an offset — it says what a fully-raised CC1
/// is worth — so the two ends of its range have to render alike with the wheel
/// down. That identity is the first thing checked, because a cutoff shift
/// applied unconditionally would pass every audibility case here and be wrong.
/// It is not checked against an untouched part: writing the address engages the
/// part's filter, the same edit a TONE MODIFY cutoff makes, and the engagement
/// is audible on its own without being the offset.
///
/// The offset reaches the voice per sample rather than being baked at the
/// note-on, which is what lets a wheel moved under a held note be heard; that
/// is checked on a sounding voice rather than inferred from the depth working.
///
/// Both voice banks are exercised: the byte is one quantity and the two banks
/// apply it in their own idiom (docs/gs.md).

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

constexpr uint8_t kChannel = 0;
/// Part 1 = channel 0 = block nibble 1, in the controller-destination block.
constexpr uint8_t kModBlock = 0x21;
constexpr uint8_t kTvfCutoffControl = 0x01;
constexpr uint8_t kModWheel = 1;
/// The power-on MODULATION TVF CUTOFF CONTROL: no offset at any wheel position.
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

/// A sawtooth, so the low-pass has partials to take away. A sine would move in
/// level under a cutoff sweep and say nothing about which frequencies went.
std::shared_ptr<Sf2File> make_fixture() {
  Sf2Builder b;
  std::vector<float> saw(128);
  for (size_t i = 0; i < saw.size(); ++i) {
    const double phase = static_cast<double>(i % 32) / 32.0;
    saw[i] = static_cast<float>(0.5 * (2.0 * phase - 1.0));
  }
  const int saw_id = b.add_sample("saw", saw, 32000, 60, 32, 128);

  Sf2Builder::ZoneSpec zone;
  zone.gens.push_back({54 /*sampleModes*/, 1});
  zone.target = saw_id;
  const int inst = b.add_instrument("sawinst", {zone});

  Sf2Builder::ZoneSpec pz;
  pz.target = inst;
  b.add_preset("Saw", 0, 0, {pz});

  const auto bytes = b.build();
  auto sf2 = std::make_shared<Sf2File>();
  std::string error;
  REQUIRE(sf2->parse(bytes.data(), bytes.size(), &error));
  return sf2;
}

using Render = std::vector<float>;
using Writes = std::function<void(Sf2Player&)>;

/// Plays one note, applying @p before at the prepare and @p during a quarter of
/// the way through the held note. Returns the interleaved render.
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

  constexpr int kFirst = 4800;
  constexpr int kRest = 14400;
  std::vector<float> left(static_cast<size_t>(kFirst + kRest), 0.0f);
  std::vector<float> right(left.size(), 0.0f);

  player.on_event(0, event(sonare::midi::make_midi1_note_on(0, kChannel, 60, 100)));
  float* head[2] = {left.data(), right.data()};
  player.process(head, 2, kFirst);
  if (during) during(player);
  float* tail[2] = {left.data() + kFirst, right.data() + kFirst};
  player.process(tail, 2, kRest);

  Render out;
  out.reserve(left.size() * 2);
  for (size_t i = 0; i < left.size(); ++i) {
    out.push_back(left[i]);
    out.push_back(right[i]);
  }
  return out;
}

/// Spectral tilt over @p r from sample @p from on: the first difference's
/// energy over the signal's own. A low-pass takes the numerator down faster
/// than the denominator, so a darker stretch reads lower. An absolute value
/// would depend on the fixture; only comparisons within one are used.
///
/// The window matters. Closing the filter also takes the level down, so a ratio
/// taken over a render that is loud before the cutoff moved and quiet after is
/// dominated by the loud part and reports the wrong sign — which is what makes
/// the mid-note case below measure its tail rather than the whole render.
double brightness(const Render& r, size_t from = 0) {
  double num = 0.0;
  double den = 0.0;
  for (size_t i = std::max<size_t>(from, 2); i < r.size(); i += 2) {
    const double d = static_cast<double>(r[i]) - static_cast<double>(r[i - 2]);
    num += d * d;
    den += static_cast<double>(r[i]) * static_cast<double>(r[i]);
  }
  return den > 0.0 ? num / den : 0.0;
}

/// The whole render's energy, for the cases that only need the voice to be
/// sounding at all.
double energy(const Render& r) {
  double sum = 0.0;
  for (const float v : r) sum += static_cast<double>(v) * static_cast<double>(v);
  return sum;
}

void write_control(Sf2Player& p, uint8_t value) {
  const std::vector<uint8_t> m = dt1(kModBlock, kTvfCutoffControl, {value});
  p.handle_sysex(m.data(), m.size());
}

void wheel(Sf2Player& p, uint8_t value) {
  p.on_event(0, event(sonare::midi::make_midi1_control_change(0, kChannel, kModWheel, value)));
}

}  // namespace

TEST_CASE("40 2x 01 is worth nothing while the mod wheel is down", "[midi][synth][gs]") {
  // The two ends of the range are 19050 cents apart, so if the byte reached the
  // cutoff without the wheel they could not render alike. They are compared
  // against each other rather than against an untouched part because writing
  // the address engages the part's filter — the same edit TONE MODIFY's cutoff
  // makes, and audible on its own — and that engagement is not the offset.
  for (const Bank bank : {Bank::kSoundFont, Bank::kModel}) {
    const std::string tag = bank == Bank::kSoundFont ? "soundfont" : "model";
    INFO("bank: " << tag);

    const Render closed = render(bank, [](Sf2Player& p) { write_control(p, 0x00); });
    const Render open = render(bank, [](Sf2Player& p) { write_control(p, 0x7F); });
    REQUIRE(energy(closed) > 0.0);
    CHECK(closed == open);
  }
}

TEST_CASE("the power-on 40 stays inert with the wheel fully up", "[midi][synth][gs]") {
  // The other half of the identity: the centre byte is the no-op of the range,
  // so raising the wheel against it must not move the cutoff either. Without
  // this a shift computed off the raw byte rather than off its distance from
  // 40 would still pass every case below.
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

TEST_CASE("40 2x 01 moves the cutoff in the direction its byte names", "[midi][synth][gs]") {
  for (const Bank bank : {Bank::kSoundFont, Bank::kModel}) {
    const std::string tag = bank == Bank::kSoundFont ? "soundfont" : "model";
    INFO("bank: " << tag);

    const Render centred = render(bank, [](Sf2Player& p) {
      write_control(p, kCentre);
      wheel(p, 127);
    });
    const Render dark = render(bank, [](Sf2Player& p) {
      write_control(p, 0x00);
      wheel(p, 127);
    });

    // Below the centre the wheel closes the filter, which is the direction the
    // signed byte names. Checked on the spectral tilt rather than on level: a
    // gain change would move both and this must not pass for one.
    INFO("centred " << brightness(centred) << " vs dark " << brightness(dark));
    CHECK(brightness(dark) < brightness(centred));
    CHECK(energy(dark) > 0.0);
  }
}

TEST_CASE("the wheel scales what 40 2x 01 is worth", "[midi][synth][gs]") {
  // A depth, not a switch: half a wheel buys a cutoff between the two ends
  // rather than either of them.
  for (const Bank bank : {Bank::kSoundFont, Bank::kModel}) {
    const std::string tag = bank == Bank::kSoundFont ? "soundfont" : "model";
    INFO("bank: " << tag);

    auto at = [bank](uint8_t cc) {
      return brightness(render(bank, [cc](Sf2Player& p) {
        write_control(p, 0x00);
        wheel(p, cc);
      }));
    };
    const double down = at(0);
    const double half = at(64);
    const double full = at(127);
    INFO("wheel 0 " << down << " / 64 " << half << " / 127 " << full);
    CHECK(half < down);
    CHECK(full < half);
  }
}

TEST_CASE("a wheel moved under a held note is heard by it", "[midi][synth][gs]") {
  // The offset arrives per sample, so the voice that was started with the wheel
  // down still has to follow it up. Compared against the wheel never moving,
  // and against its having been up from the note-on: the first says the move
  // was heard, the second that it was heard from the move and not before it.
  for (const Bank bank : {Bank::kSoundFont, Bank::kModel}) {
    const std::string tag = bank == Bank::kSoundFont ? "soundfont" : "model";
    INFO("bank: " << tag);

    const Writes armed = [](Sf2Player& p) { write_control(p, 0x00); };
    const Render still = render(bank, armed);
    const Render moved = render(bank, armed, [](Sf2Player& p) { wheel(p, 127); });
    const Render from_start = render(bank, [&armed](Sf2Player& p) {
      armed(p);
      wheel(p, 127);
    });

    CHECK_FALSE(moved == still);
    CHECK_FALSE(moved == from_start);
    // Measured over the second half, which is after the cutoff SETTLED rather
    // than merely after it moved. The two blocks either side of the move are the
    // filter discharging the loud signal the voice had already built, and that
    // discharge is broadband enough to read brighter than the render it is
    // darkening — the sign inverts if the window starts at the move itself.
    const size_t kSettled = moved.size() / 2;
    INFO("moved " << brightness(moved, kSettled) << " vs still " << brightness(still, kSettled));
    CHECK(brightness(moved, kSettled) < brightness(still, kSettled));
  }
}
