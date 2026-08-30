/// @file gs_key_shift_test.cpp
/// @brief GS key shift: MASTER KEY-SHIFT (40 00 05) and the part's own PITCH
///        KEY SHIFT (40 1x 16), which are the two pitch offsets a rhythm part
///        does not take.
///
/// Two manual facts are pinned here. Key shift stops at the drum part — "Even
/// if you adjust Key Shift for all Parts, the pitch of the Drum Part will not
/// be affected" is printed beside both parameters — while every other pitch
/// offset reaches it. And PITCH KEY SHIFT is not an alias of RPN 00 02 Master
/// Coarse Tuning however exactly their ranges coincide: the parameter map
/// annotates every alias it has and annotates this row with nothing, so the two
/// are separate storage locations and they add.
///
/// The fixture carries a bank-128 preset sounding the same looped sine as the
/// melodic one, so the rhythm part has a pitch to measure. A silent drum part
/// would satisfy "it was not transposed" by having no pitch at all, and the
/// exclusion could then be deleted with nothing red.

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
using sonare::midi::synth::gs_key_shift_cents;
using sonare::midi::synth::Sf2File;
using sonare::midi::synth::Sf2Player;
using sonare::midi::synth::Sf2PlayerConfig;
using sonare::test::Sf2Builder;

constexpr double kOutRate = 48000.0;
constexpr double kTwoPi = 6.28318530717958647692;

constexpr uint8_t kMelodicChannel = 0;
constexpr uint8_t kDrumChannel = 9;
/// GS part block nibbles: block 1 = part 1 = channel 0, block 0 = part 10.
constexpr uint8_t kMelodicBlock = 0x11;
constexpr uint8_t kDrumBlock = 0x10;

MidiEvent event(const sonare::midi::Ump& ump) {
  MidiEvent e;
  e.ump = ump;
  return e;
}

/// A framed Roland DT1 write of @p data at 40 <mid> <lo>, with the checksum.
std::vector<uint8_t> dt1(uint8_t mid, uint8_t lo, const std::vector<uint8_t>& data) {
  std::vector<uint8_t> msg{0xF0, 0x41, 0x10, 0x42, 0x12, 0x40, mid, lo};
  msg.insert(msg.end(), data.begin(), data.end());
  int sum = 0x40 + mid + lo;
  for (const uint8_t b : data) sum += b;
  msg.push_back(static_cast<uint8_t>((128 - (sum % 128)) & 0x7F));
  msg.push_back(0xF7);
  return msg;
}

/// A looped sine at root key 60, published both as the melodic program 0 and as
/// the bank-128 rhythm set, so note 60 sounds one frequency on either part kind
/// and a key shift is a frequency ratio rather than a spectral shift.
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

StereoRender render_on(uint8_t channel, const std::function<void(Sf2Player&)>& setup) {
  Sf2PlayerConfig cfg;
  cfg.gain = 1.0f;
#if defined(SONARE_MIDI_WITH_FX)
  cfg.effects.enable_reverb = false;
  cfg.effects.enable_chorus = false;
  cfg.effects.enable_delay = false;
#endif
  Sf2Player player(cfg);
  player.set_soundfont(make_fixture());
  player.prepare(kOutRate, 256);
  if (setup) setup(player);

  StereoRender out;
  out.left.assign(24000, 0.0f);
  out.right.assign(24000, 0.0f);
  player.on_event(0, event(sonare::midi::make_midi1_note_on(0, channel, 60, 100)));
  float* chans[2] = {out.left.data(), out.right.data()};
  player.process(chans, 2, 24000);
  return out;
}

bool identical(const StereoRender& a, const StereoRender& b) {
  return a.left == b.left && a.right == b.right;
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

double frequency_on(uint8_t channel, const std::function<void(Sf2Player&)>& setup) {
  return estimate_frequency(render_on(channel, setup).left);
}

/// Selects RPN 00 02 on @p channel and writes @p msb as its data entry.
void coarse_tune(Sf2Player& p, uint8_t channel, uint8_t msb) {
  p.on_event(0, event(sonare::midi::make_midi1_control_change(0, channel, 101, 0)));
  p.on_event(0, event(sonare::midi::make_midi1_control_change(0, channel, 100, 2)));
  p.on_event(0, event(sonare::midi::make_midi1_control_change(0, channel, 6, msb)));
}

void write_sysex(Sf2Player& p, uint8_t mid, uint8_t lo, uint8_t value) {
  const std::vector<uint8_t> msg = dt1(mid, lo, {value});
  p.handle_sysex(msg.data(), msg.size());
}

}  // namespace

TEST_CASE("the key-shift conversion reads the manual's endpoints", "[midi][synth][gs]") {
  CHECK(gs_key_shift_cents(0x40) == Approx(0.0f));
  CHECK(gs_key_shift_cents(0x28) == Approx(-2400.0f));
  CHECK(gs_key_shift_cents(0x58) == Approx(2400.0f));
}

TEST_CASE("both key shifts are exactly inert at their reset defaults", "[midi][synth][gs]") {
  const StereoRender untouched = render_on(kMelodicChannel, nullptr);
  CHECK(identical(untouched, render_on(kMelodicChannel, [](Sf2Player& p) {
                    write_sysex(p, 0x00, 0x05, 0x40);
                    write_sysex(p, kMelodicBlock, 0x16, 0x40);
                  })));
}

TEST_CASE("each key shift moves the melodic pitch on its own", "[midi][synth][gs]") {
  const double base = frequency_on(kMelodicChannel, nullptr);
  REQUIRE(base > 100.0);

  const double master_up =
      frequency_on(kMelodicChannel, [](Sf2Player& p) { write_sysex(p, 0x00, 0x05, 0x4C); });
  CHECK(master_up / base == Approx(std::pow(2.0, 12.0 / 12.0)).epsilon(0.005));

  const double part_down = frequency_on(
      kMelodicChannel, [](Sf2Player& p) { write_sysex(p, kMelodicBlock, 0x16, 0x34); });
  CHECK(part_down / base == Approx(std::pow(2.0, -12.0 / 12.0)).epsilon(0.005));
}

TEST_CASE("the rhythm part sounds a pitch of its own", "[midi][synth][gs]") {
  // Everything the drum rows below assert rests on this: silence would agree
  // with "not transposed" whatever the render did.
  const double drum = frequency_on(kDrumChannel, nullptr);
  const double melodic = frequency_on(kMelodicChannel, nullptr);
  REQUIRE(drum > 100.0);
  CHECK(drum == Approx(melodic).epsilon(0.005));
}

TEST_CASE("an out-of-range key-shift byte is ignored rather than clamped", "[midi][synth][gs]") {
  // The standing rule everywhere but 40 1x 15: a value outside the row's 28-58
  // leaves the parameter where it was rather than landing on the nearest end.
  const StereoRender untouched = render_on(kMelodicChannel, nullptr);
  for (const uint8_t value : {uint8_t{0x20}, uint8_t{0x6F}}) {
    INFO("value=" << int(value));
    uint8_t master_after = 0;
    uint8_t part_after = 0;
    const StereoRender out = render_on(kMelodicChannel, [&](Sf2Player& p) {
      write_sysex(p, 0x00, 0x05, value);
      write_sysex(p, kMelodicBlock, 0x16, value);
      master_after = p.master_key_shift();
      part_after = p.pitch_key_shift(kMelodicChannel);
    });
    CHECK(master_after == 0x40);
    CHECK(part_after == 0x40);
    CHECK(identical(untouched, out));
  }
}

TEST_CASE("the two key shifts and RPN 00 02 combine over both part kinds", "[midi][synth][gs]") {
  // Four factors covered over all 36 pairs: the master byte, the part byte, the
  // part kind, and RPN 00 02. 6F and 20 are outside 28-58 and are ignored rather
  // than clamped, so they contribute nothing.
  struct Row {
    uint8_t master;  // 40 00 05
    uint8_t part;    // 40 1x 16
    bool drum;       // the part kind
    uint8_t coarse;  // RPN 00 02 data-entry MSB
    int semitones;   // expected offset from the untouched render
  };
  static constexpr Row kRows[] = {
      {0x40, 0x40, false, 0x40, 0}, {0x40, 0x34, true, 0x45, 5},  {0x40, 0x20, false, 0x45, 5},
      {0x4C, 0x40, true, 0x45, 5},  {0x4C, 0x34, false, 0x40, 0}, {0x4C, 0x20, false, 0x40, 12},
      {0x6F, 0x40, false, 0x45, 5}, {0x6F, 0x34, true, 0x40, 0},  {0x6F, 0x20, true, 0x45, 5},
  };

  const double melodic_base = frequency_on(kMelodicChannel, nullptr);
  const double drum_base = frequency_on(kDrumChannel, nullptr);
  REQUIRE(melodic_base > 100.0);
  REQUIRE(drum_base > 100.0);

  for (const Row& row : kRows) {
    const uint8_t channel = row.drum ? kDrumChannel : kMelodicChannel;
    const uint8_t block = row.drum ? kDrumBlock : kMelodicBlock;
    // Each row twice, with the RPN written before and after the two SysEx
    // bytes: separate storage locations answer the same either way, and one
    // folded into the other would be overwritten by whichever wrote last.
    for (const bool coarse_first : {false, true}) {
      const double hz = frequency_on(channel, [&row, channel, block, coarse_first](Sf2Player& p) {
        if (coarse_first) coarse_tune(p, channel, row.coarse);
        write_sysex(p, 0x00, 0x05, row.master);
        write_sysex(p, block, 0x16, row.part);
        if (!coarse_first) coarse_tune(p, channel, row.coarse);
      });
      const double base = row.drum ? drum_base : melodic_base;
      INFO("master=" << int(row.master) << " part=" << int(row.part) << " drum=" << row.drum
                     << " coarse=" << int(row.coarse) << " coarse_first=" << coarse_first
                     << " hz=" << hz);
      REQUIRE(hz > 100.0);
      CHECK(hz / base ==
            Approx(std::pow(2.0, static_cast<double>(row.semitones) / 12.0)).epsilon(0.005));
    }
  }
}

TEST_CASE("PITCH KEY SHIFT adds to RPN 00 02 rather than overwriting it", "[midi][synth][gs]") {
  const double base = frequency_on(kMelodicChannel, nullptr);
  REQUIRE(base > 100.0);
  const double both = frequency_on(kMelodicChannel, [](Sf2Player& p) {
    coarse_tune(p, kMelodicChannel, 0x45);      // +5 semitones
    write_sysex(p, kMelodicBlock, 0x16, 0x4C);  // +12 semitones
    // Two storage locations, so neither write is visible in the other's value.
    REQUIRE(p.pitch_coarse_tune(kMelodicChannel) == 5);
    REQUIRE(p.pitch_key_shift(kMelodicChannel) == 0x4C);
  });
  CHECK(both / base == Approx(std::pow(2.0, 17.0 / 12.0)).epsilon(0.005));
}

TEST_CASE("GS Reset restores both key shifts", "[midi][synth][gs]") {
  static constexpr uint8_t kGsReset[] = {0xF0, 0x41, 0x10, 0x42, 0x12, 0x40,
                                         0x00, 0x7F, 0x00, 0x41, 0xF7};
  const StereoRender untouched = render_on(kMelodicChannel, nullptr);
  const StereoRender reset = render_on(kMelodicChannel, [](Sf2Player& p) {
    write_sysex(p, 0x00, 0x05, 0x4C);
    write_sysex(p, kMelodicBlock, 0x16, 0x34);
    REQUIRE(p.handle_sysex(kGsReset, sizeof(kGsReset)));
    REQUIRE(p.master_key_shift() == 0x40);
    REQUIRE(p.pitch_key_shift(kMelodicChannel) == 0x40);
  });
  CHECK(identical(untouched, reset));
}

TEST_CASE("a part that becomes a rhythm part stops taking its key shift", "[midi][synth][gs]") {
  const double base = frequency_on(kMelodicChannel, nullptr);
  REQUIRE(base > 100.0);
  // Positive control: while the part is melodic, the same write transposes it.
  const double melodic = frequency_on(
      kMelodicChannel, [](Sf2Player& p) { write_sysex(p, kMelodicBlock, 0x16, 0x4C); });
  CHECK(melodic / base == Approx(2.0).epsilon(0.005));

  // USE FOR RHYTHM PART arrives after the key shift, so the exclusion has to be
  // decided at the render rather than when the byte was applied.
  const double rhythm = frequency_on(kMelodicChannel, [](Sf2Player& p) {
    write_sysex(p, kMelodicBlock, 0x16, 0x4C);
    write_sysex(p, kMelodicBlock, 0x15, 0x01);
  });
  REQUIRE(rhythm > 100.0);
  CHECK(rhythm / base == Approx(1.0).epsilon(0.005));
}
