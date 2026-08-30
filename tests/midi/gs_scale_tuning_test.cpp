/// @file gs_scale_tuning_test.cpp
/// @brief GS SCALE TUNING (40 1x 40-4B): twelve bytes, one per pitch class, a
///        cent a step from a centred 40.
///
/// What makes this a temperament rather than a part-wide detune is that it is
/// indexed, so the tests are about the indexing: a byte written for C moves
/// every C and no other key, and it moves the Cs in every octave. The amount is
/// checked by frequency against the semitone it is written to equal, which is
/// an identity rather than a movement — a table read with the wrong index still
/// moves the render, and the audibility gate would pass it.
///
/// Both voice banks are exercised. They apply the offset in different idioms —
/// a sample increment on one, the render's pitch sum on the other — because
/// every model engine but the subtractive one is started from the note NUMBER,
/// so the shared thing is the conversion in gs_layer.h rather than the site.

#include <catch2/catch_approx.hpp>
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
#include "support/sf2_builder.h"

namespace {

using Catch::Approx;
using sonare::midi::MidiEvent;
using sonare::midi::synth::gs_scale_tuning_cents;
using sonare::midi::synth::kGsScaleTuningEqual;
using sonare::midi::synth::Sf2File;
using sonare::midi::synth::Sf2Player;
using sonare::midi::synth::Sf2PlayerConfig;
using sonare::test::Sf2Builder;

constexpr double kOutRate = 48000.0;
constexpr double kTwoPi = 6.28318530717958647692;

constexpr uint8_t kChannel = 0;
/// Part 1 = channel 0 = block nibble 1; the twelve bytes start at 40.
constexpr uint8_t kPartBlock = 0x11;
constexpr uint8_t kScaleTuningBase = 0x40;

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

/// A looped sine at root key 60, so a pitch offset is a frequency ratio the
/// zero-crossing estimate below can read straight off.
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

struct Render {
  std::vector<float> left;
  std::vector<float> right;
  bool operator==(const Render& o) const { return left == o.left && right == o.right; }
};

using Writes = std::function<void(Sf2Player&)>;

Render render(Bank bank, const Writes& setup, uint8_t note) {
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

  Render out;
  out.left.assign(24000, 0.0f);
  out.right.assign(24000, 0.0f);
  player.on_event(0, event(sonare::midi::make_midi1_note_on(0, kChannel, note, 100)));
  float* chans[2] = {out.left.data(), out.right.data()};
  player.process(chans, 2, 24000);
  return out;
}

/// Zero-crossing frequency estimate over the steady part of the render.
double estimate_frequency(const std::vector<float>& buf) {
  double first = -1.0;
  double last = -1.0;
  int cycles = -1;
  for (size_t i = 4801; i < buf.size(); ++i) {
    if (buf[i - 1] >= 0.0f || buf[i] < 0.0f) continue;
    const double frac =
        static_cast<double>(buf[i - 1]) / (static_cast<double>(buf[i - 1]) - buf[i]);
    const double t = static_cast<double>(i - 1) + frac;
    if (first < 0.0) {
      first = t;
    } else {
      last = t;
    }
    ++cycles;
  }
  if (cycles < 1 || last <= first) return 0.0;
  return kOutRate * static_cast<double>(cycles) / (last - first);
}

/// Writes the whole twelve-byte table in one run, as a file does.
Writes table(const sonare::midi::synth::GsScaleTuning& scale) {
  return [scale](Sf2Player& p) {
    const std::vector<uint8_t> data(scale.begin(), scale.end());
    const std::vector<uint8_t> msg = dt1(kPartBlock, kScaleTuningBase, data);
    p.handle_sysex(msg.data(), msg.size());
  };
}

/// The equal-tempered table with @p pitch_class detuned by @p cents.
sonare::midi::synth::GsScaleTuning one_class(size_t pitch_class, int cents) {
  sonare::midi::synth::GsScaleTuning scale = kGsScaleTuningEqual;
  scale[pitch_class] = static_cast<uint8_t>(0x40 + cents);
  return scale;
}

}  // namespace

TEST_CASE("the scale-tuning conversion reads a byte per pitch class", "[midi][synth][gs]") {
  CHECK(gs_scale_tuning_cents(kGsScaleTuningEqual, 60) == Approx(0.0f));
  // C is pitch class 0 whatever octave it is struck in, and only C moves.
  const auto c_up = one_class(0, 12);
  CHECK(gs_scale_tuning_cents(c_up, 60) == Approx(12.0f));
  CHECK(gs_scale_tuning_cents(c_up, 0) == Approx(12.0f));
  CHECK(gs_scale_tuning_cents(c_up, 120) == Approx(12.0f));
  CHECK(gs_scale_tuning_cents(c_up, 61) == Approx(0.0f));
  CHECK(gs_scale_tuning_cents(c_up, 59) == Approx(0.0f));
  // 40 is in tune and the byte is signed about it.
  CHECK(gs_scale_tuning_cents(one_class(3, -30), 63) == Approx(-30.0f));
}

TEST_CASE("a written equal-tempered table is exactly inert", "[midi][synth][gs]") {
  for (const Bank bank : {Bank::kSoundFont, Bank::kModel}) {
    INFO("bank: " << (bank == Bank::kSoundFont ? "soundfont" : "model"));
    // The identity value, checked before anything is read into a sweep: a table
    // of 40 has to leave the render the file would have had without it.
    CHECK(render(bank, table(kGsScaleTuningEqual), 60) == render(bank, nullptr, 60));
    CHECK(render(bank, table(kGsScaleTuningEqual), 67) == render(bank, nullptr, 67));
  }
}

TEST_CASE("40 1x 40-4B tunes the pitch class it is indexed by", "[midi][synth][gs]") {
  for (const Bank bank : {Bank::kSoundFont, Bank::kModel}) {
    INFO("bank: " << (bank == Bank::kSoundFont ? "soundfont" : "model"));
    const Writes c_up = table(one_class(0, 40));

    // C moves in every octave, and nothing else does — which is the difference
    // between a temperament and a part-wide detune.
    CHECK_FALSE(render(bank, c_up, 48) == render(bank, nullptr, 48));
    CHECK_FALSE(render(bank, c_up, 60) == render(bank, nullptr, 60));
    CHECK_FALSE(render(bank, c_up, 72) == render(bank, nullptr, 72));
    CHECK(render(bank, c_up, 61) == render(bank, nullptr, 61));
    CHECK(render(bank, c_up, 67) == render(bank, nullptr, 67));
    CHECK(render(bank, c_up, 71) == render(bank, nullptr, 71));

    // The byte for D reaches D and leaves C where it was, so the index is the
    // pitch class and not the order the run was written in.
    const Writes d_down = table(one_class(2, -40));
    CHECK_FALSE(render(bank, d_down, 62) == render(bank, nullptr, 62));
    CHECK(render(bank, d_down, 60) == render(bank, nullptr, 60));
  }
}

TEST_CASE("a scale-tuning byte is worth the cents it names", "[midi][synth][gs]") {
  // By identity against the semitone it is written to equal: C tuned up a full
  // 64 cents lands where the estimator reads C plus 64 cents, and the ladder is
  // linear in the byte rather than merely monotone in it.
  const double c4 = estimate_frequency(render(Bank::kSoundFont, nullptr, 60).left);
  REQUIRE(c4 > 0.0);
  for (const int cents : {-63, -24, 12, 63}) {
    INFO("cents: " << cents);
    const double tuned =
        estimate_frequency(render(Bank::kSoundFont, table(one_class(0, cents)), 60).left);
    CHECK(1200.0 * std::log2(tuned / c4) == Approx(static_cast<double>(cents)).margin(0.5));
  }
}
