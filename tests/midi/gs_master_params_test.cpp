/// @file gs_master_params_test.cpp
/// @brief The GS master block (40 00 00-06): tuning, volume and pan.
///
/// These three rows carried GsLevel::kAudible while nothing read them, which is
/// the one failure the level scheme cannot catch on its own — a row is present,
/// so the coverage census counts the address as answered. What the census cannot
/// say is whether the answer reaches the audio, and that is what this measures.
///
/// Each case also renders the power-on value explicitly and requires it to be
/// bit-identical to an untouched render, because a master that is not exactly
/// inert at its default moves every bounce in the repository.

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
using sonare::midi::synth::gs_master_pan_gains;
using sonare::midi::synth::gs_master_tune_cents;
using sonare::midi::synth::gs_master_volume_gain;
using sonare::midi::synth::GsMasterParams;
using sonare::midi::synth::Sf2File;
using sonare::midi::synth::Sf2Player;
using sonare::midi::synth::Sf2PlayerConfig;
using sonare::test::Sf2Builder;

constexpr double kOutRate = 48000.0;
constexpr double kTwoPi = 6.28318530717958647692;

MidiEvent event(const sonare::midi::Ump& ump) {
  MidiEvent e;
  e.ump = ump;
  return e;
}

/// A framed Roland DT1 write of @p data at 40 00 <lo>, with the checksum.
std::vector<uint8_t> dt1(uint8_t lo, const std::vector<uint8_t>& data) {
  std::vector<uint8_t> msg{0xF0, 0x41, 0x10, 0x42, 0x12, 0x40, 0x00, lo};
  msg.insert(msg.end(), data.begin(), data.end());
  int sum = 0x40 + 0x00 + lo;
  for (const uint8_t b : data) sum += b;
  msg.push_back(static_cast<uint8_t>((128 - (sum % 128)) & 0x7F));
  msg.push_back(0xF7);
  return msg;
}

/// Program 0: a looped sine at root key 60, so note 60 sounds one frequency and
/// a tuning change is a frequency ratio rather than a spectral shift.
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

struct StereoRender {
  std::vector<float> left;
  std::vector<float> right;
};

StereoRender render_with(const std::function<void(Sf2Player&)>& setup) {
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
  // Centred, so both legs carry signal: a leg that is already exactly zero
  // multiplies to zero by any gain and cannot show a balance change at all.
  player.on_event(0, event(sonare::midi::make_midi1_note_on(0, 0, 60, 100)));
  float* chans[2] = {out.left.data(), out.right.data()};
  player.process(chans, 2, 24000);
  return out;
}

/// A DT1 write of @p data at 40 00 <lo>, as a setup callback.
std::function<void(Sf2Player&)> write(uint8_t lo, const std::vector<uint8_t>& data) {
  return [lo, data](Sf2Player& p) {
    const std::vector<uint8_t> msg = dt1(lo, data);
    p.handle_sysex(msg.data(), msg.size());
  };
}

bool identical(const StereoRender& a, const StereoRender& b) {
  return a.left == b.left && a.right == b.right;
}

double rms(const std::vector<float>& buf) {
  double acc = 0.0;
  for (const float s : buf) acc += static_cast<double>(s) * s;
  return std::sqrt(acc / static_cast<double>(buf.size()));
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

}  // namespace

TEST_CASE("the GS master conversions read the manual's endpoints", "[midi][synth][gs]") {
  // MASTER TUNE is four nibbles making 0018 - 0400 - 07E8 in 0.1-cent steps.
  CHECK(gs_master_tune_cents(GsMasterParams{}) == Approx(0.0f));
  CHECK(gs_master_tune_cents(GsMasterParams{{{0x00, 0x00, 0x01, 0x08}}, 0x7F, 0x40}) ==
        Approx(-100.0f));
  CHECK(gs_master_tune_cents(GsMasterParams{{{0x00, 0x07, 0x0E, 0x08}}, 0x7F, 0x40}) ==
        Approx(100.0f));
  // The manual's own worked example: 445 Hz at A4 is +19.56 cents, written as
  // 00 04 0C 04.
  CHECK(gs_master_tune_cents(GsMasterParams{{{0x00, 0x04, 0x0C, 0x04}}, 0x7F, 0x40}) ==
        Approx(19.6f));

  CHECK(gs_master_volume_gain(0x7F) == Approx(1.0f));
  CHECK(gs_master_volume_gain(0) == Approx(0.0f));

  float pan_l = 0.0f;
  float pan_r = 0.0f;
  gs_master_pan_gains(0x40, &pan_l, &pan_r);
  CHECK(pan_l == Approx(1.0f));
  CHECK(pan_r == Approx(1.0f));
  gs_master_pan_gains(0x01, &pan_l, &pan_r);
  CHECK(pan_l == Approx(1.0f));
  CHECK(pan_r == Approx(0.0f));
  gs_master_pan_gains(0x7F, &pan_l, &pan_r);
  CHECK(pan_l == Approx(0.0f));
  CHECK(pan_r == Approx(1.0f));
}

TEST_CASE("the GS master values are exactly inert at their reset defaults", "[midi][synth][gs]") {
  const StereoRender untouched = render_with(nullptr);
  CHECK(identical(untouched, render_with(write(0x00, {0x00, 0x04, 0x00, 0x00}))));
  CHECK(identical(untouched, render_with(write(0x04, {0x7F}))));
  CHECK(identical(untouched, render_with(write(0x06, {0x40}))));
}

TEST_CASE("MASTER VOLUME scales the whole output", "[midi][synth][gs]") {
  const StereoRender untouched = render_with(nullptr);
  const StereoRender half = render_with(write(0x04, {0x40}));
  const StereoRender off = render_with(write(0x04, {0x00}));
  // The same square law CC7 and velocity use: 64/127 of full scale is a quarter
  // of the level.
  const double expected = (64.0 / 127.0) * (64.0 / 127.0);
  CHECK(rms(half.left) == Approx(rms(untouched.left) * expected).epsilon(0.02));
  CHECK(rms(off.left) == Approx(0.0));
  CHECK(rms(off.right) == Approx(0.0));
}

TEST_CASE("MASTER TUNE moves the pitch by the cents it names", "[midi][synth][gs]") {
  const double base = estimate_frequency(render_with(nullptr).left);
  REQUIRE(base > 100.0);
  const double sharp = estimate_frequency(render_with(write(0x00, {0x00, 0x07, 0x0E, 0x08})).left);
  const double flat = estimate_frequency(render_with(write(0x00, {0x00, 0x00, 0x01, 0x08})).left);
  CHECK(sharp / base == Approx(std::pow(2.0, 100.0 / 1200.0)).epsilon(0.005));
  CHECK(flat / base == Approx(std::pow(2.0, -100.0 / 1200.0)).epsilon(0.005));
}

TEST_CASE("MASTER PAN balances the finished mix", "[midi][synth][gs]") {
  const StereoRender untouched = render_with(nullptr);
  REQUIRE(rms(untouched.left) > 0.0);
  REQUIRE(rms(untouched.right) > 0.0);
  const StereoRender hard_right = render_with(write(0x06, {0x7F}));
  CHECK(rms(hard_right.left) == Approx(0.0));
  // A balance attenuates only the far leg, so the near one comes through
  // untouched rather than boosted.
  CHECK(hard_right.right == untouched.right);
  const StereoRender hard_left = render_with(write(0x06, {0x01}));
  CHECK(rms(hard_left.right) == Approx(0.0));
  CHECK(hard_left.left == untouched.left);
}

TEST_CASE("an out-of-range MASTER PAN is ignored, not clamped", "[midi][synth][gs]") {
  // 00 is out of the row's 01-7F: master pan has no random value, unlike a
  // part's, so there is nothing for it to mean.
  CHECK(identical(render_with(nullptr), render_with(write(0x06, {0x00}))));
}

TEST_CASE("GS Reset restores the master values", "[midi][synth][gs]") {
  static constexpr uint8_t kGsReset[] = {0xF0, 0x41, 0x10, 0x42, 0x12, 0x40,
                                         0x00, 0x7F, 0x00, 0x41, 0xF7};
  const StereoRender untouched = render_with(nullptr);
  const StereoRender reset = render_with([](Sf2Player& p) {
    write(0x04, {0x20})(p);
    write(0x00, {0x00, 0x07, 0x0E, 0x08})(p);
    write(0x06, {0x7F})(p);
    REQUIRE(p.handle_sysex(kGsReset, sizeof(kGsReset)));
  });
  CHECK(identical(untouched, reset));
}
