/// @file gs_master_eq_test.cpp
/// @brief The GS master EQ (40 02 00-03, per-part switch 40 4x 20) and the
///        system-effect block (40 01 30-5A) reaching the audio: transparency at
///        the reset defaults, per-band direction, the per-part bypass, the
///        multi-byte runs real files write, and what a GS Reset restores.
///
/// AUDIBLE is a relative claim here (docs/gs.md): every case measures a
/// difference against another render of the same notes, never against a
/// recording of hardware.

#include "midi/synth/gs_master_eq.h"

#include <algorithm>
#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "midi/midi_event.h"
#include "midi/synth/gs_system_effects.h"
#include "midi/synth/sf2_file.h"
#include "midi/synth/sf2_player.h"
#include "midi/ump.h"
#include "support/alloc_guard.h"
#include "support/sf2_builder.h"

namespace {

using sonare::midi::MidiEvent;
using sonare::midi::synth::GsMasterEq;
using sonare::midi::synth::GsSystemEffects;
using sonare::midi::synth::Sf2File;
using sonare::midi::synth::Sf2Player;
using sonare::midi::synth::Sf2PlayerConfig;
using sonare::test::AllocationGuard;
using sonare::test::Sf2Builder;

constexpr double kOutRate = 48000.0;
constexpr double kTwoPi = 6.28318530717958647692;

/// Channel 0 is GS part block 1 (block 0 is part 10), so its part addresses are
/// 40 41 xx.
constexpr uint8_t kPart1Block = 0x41;

MidiEvent event(const sonare::midi::Ump& ump) {
  MidiEvent e;
  e.ump = ump;
  return e;
}

/// A Roland DT1 frame for @p addr carrying @p data, with the checksum the GS
/// decoder requires.
std::vector<uint8_t> dt1(uint8_t hi, uint8_t mid, uint8_t lo, const std::vector<uint8_t>& data) {
  std::vector<uint8_t> msg{0xF0, 0x41, 0x10, 0x42, 0x12, hi, mid, lo};
  msg.insert(msg.end(), data.begin(), data.end());
  int sum = hi + mid + lo;
  for (const uint8_t b : data) sum += b;
  msg.push_back(static_cast<uint8_t>((128 - (sum % 128)) & 0x7F));
  msg.push_back(0xF7);
  return msg;
}

/// Fixture: program 0 = a looped 1 kHz sine at root key 60, so a note selects
/// the probe frequency (24 = 125 Hz, 36 = 250 Hz, 60 = 1 kHz, 96 = 8 kHz);
/// program 1 = a short one-shot burst for the wet-tail cases.
std::shared_ptr<Sf2File> make_fixture() {
  Sf2Builder b;

  std::vector<float> sine(96);
  for (size_t i = 0; i < sine.size(); ++i) {
    sine[i] = 0.5f * static_cast<float>(std::sin(kTwoPi * static_cast<double>(i) / 32.0));
  }
  const int sine_id = b.add_sample("sine1k", sine, 32000, 60, 32, 96);

  std::vector<float> burst(256);
  for (size_t i = 0; i < burst.size(); ++i) {
    const float envl = 1.0f - static_cast<float>(i) / 256.0f;
    burst[i] = envl * static_cast<float>(std::sin(kTwoPi * static_cast<double>(i) / 16.0));
  }
  const int burst_id = b.add_sample("burst", burst, 48000, 60, 0, 256);

  Sf2Builder::ZoneSpec looped;
  looped.gens.push_back({54 /*sampleModes*/, 1});
  looped.target = sine_id;
  const int sine_inst = b.add_instrument("sine", {looped});

  Sf2Builder::ZoneSpec oneshot;
  oneshot.target = burst_id;
  const int burst_inst = b.add_instrument("burst", {oneshot});

  Sf2Builder::ZoneSpec pz;
  pz.target = sine_inst;
  b.add_preset("Sine", 0, 0, {pz});
  pz.target = burst_inst;
  b.add_preset("Burst", 0, 1, {pz});

  const auto bytes = b.build();
  auto sf2 = std::make_shared<Sf2File>();
  std::string error;
  REQUIRE(sf2->parse(bytes.data(), bytes.size(), &error));
  return sf2;
}

/// An offline player: process() applies a pending GS system-effect state inline,
/// which is the path the bounce takes.
Sf2Player make_offline_player() {
  Sf2PlayerConfig cfg;
  cfg.gain = 1.0f;
  cfg.realize_efx_inline = true;
  Sf2Player player(cfg);
  player.set_soundfont(make_fixture());
  player.prepare(kOutRate, 256);
  return player;
}

struct StereoRender {
  std::vector<float> left;
  std::vector<float> right;
};

StereoRender render(Sf2Player& player, int num_samples) {
  StereoRender out;
  out.left.assign(static_cast<size_t>(num_samples), 0.0f);
  out.right.assign(static_cast<size_t>(num_samples), 0.0f);
  float* chans[2] = {out.left.data(), out.right.data()};
  player.process(chans, 2, num_samples);
  return out;
}

/// Silences the send-return units for channel 0, so a measurement sees the dry
/// part alone: the returns are summed ahead of the EQ and are not part-switched.
void mute_sends(Sf2Player& player) {
  for (const uint8_t cc : {91, 93, 94}) {
    player.on_event(0, event(sonare::midi::make_midi1_control_change(0, 0, cc, 0)));
  }
}

/// Goertzel power at @p hz over [from, to). Every probe frequency completes an
/// integer number of cycles in the windows used below, so the tones do not leak
/// into each other's bins.
double tone_power(const std::vector<float>& buf, double hz, size_t from, size_t to) {
  const double w = kTwoPi * hz / kOutRate;
  const double coeff = 2.0 * std::cos(w);
  double s1 = 0.0;
  double s2 = 0.0;
  to = std::min(to, buf.size());
  for (size_t i = from; i < to; ++i) {
    const double s0 = static_cast<double>(buf[i]) + coeff * s1 - s2;
    s2 = s1;
    s1 = s0;
  }
  return s1 * s1 + s2 * s2 - coeff * s1 * s2;
}

double rms(const std::vector<float>& buf, size_t from, size_t to) {
  double acc = 0.0;
  size_t n = 0;
  to = std::min(to, buf.size());
  for (size_t i = from; i < to; ++i) {
    acc += static_cast<double>(buf[i]) * buf[i];
    ++n;
  }
  return n > 0 ? std::sqrt(acc / static_cast<double>(n)) : 0.0;
}

/// The four master-EQ bytes plus the part switch, as one point in the parameter
/// space the cases below enumerate.
struct EqPoint {
  uint8_t low_freq = 0x00;
  uint8_t low_gain = 0x40;
  uint8_t high_freq = 0x00;
  uint8_t high_gain = 0x40;
  bool part_eq_on = true;
};

/// Every combination of the four addressed EQ values. Three gains per band (the
/// range ends and the flat centre) times the two corners each FREQ selects; the
/// space is small enough to enumerate rather than sample.
std::vector<EqPoint> eq_grid() {
  std::vector<EqPoint> out;
  for (const uint8_t low_freq : {0x00, 0x01}) {
    for (const uint8_t low_gain : {0x34, 0x40, 0x4C}) {
      for (const uint8_t high_freq : {0x00, 0x01}) {
        for (const uint8_t high_gain : {0x34, 0x40, 0x4C}) {
          out.push_back({low_freq, low_gain, high_freq, high_gain, true});
        }
      }
    }
  }
  return out;
}

/// Renders the four probe tones on channel 0 under @p eq. Sends are muted, so
/// the only thing on the bus is the part the EQ switch selects.
StereoRender render_tones(const EqPoint& eq, int num_samples) {
  Sf2Player player = make_offline_player();
  mute_sends(player);
  const std::vector<uint8_t> eq_write =
      dt1(0x40, 0x02, 0x00, {eq.low_freq, eq.low_gain, eq.high_freq, eq.high_gain});
  player.handle_sysex(eq_write.data(), eq_write.size());
  const std::vector<uint8_t> switch_write =
      dt1(0x40, kPart1Block, 0x20, {static_cast<uint8_t>(eq.part_eq_on ? 1 : 0)});
  player.handle_sysex(switch_write.data(), switch_write.size());
  for (const uint8_t note : {24, 36, 60, 96}) {
    player.on_event(0, event(sonare::midi::make_midi1_note_on(0, 0, note, 100)));
  }
  return render(player, num_samples);
}

constexpr int kToneSamples = 12000;
constexpr size_t kToneFrom = 2400;  ///< 9600 frames to the end: an integer
constexpr size_t kToneTo = 12000;   ///< number of cycles for every probe tone.

/// Probe-tone power of a render, in the order 125 / 250 / 1000 / 8000 Hz.
std::array<double, 4> tone_powers(const StereoRender& out) {
  std::array<double, 4> p{};
  const double freqs[4] = {125.0, 250.0, 1000.0, 8000.0};
  for (size_t i = 0; i < 4; ++i) {
    p[i] = tone_power(out.left, freqs[i], kToneFrom, kToneTo) +
           tone_power(out.right, freqs[i], kToneFrom, kToneTo);
  }
  return p;
}

double power_ratio_db(double measured, double reference) {
  return 10.0 * std::log10(std::max(measured, 1e-30) / std::max(reference, 1e-30));
}

}  // namespace

TEST_CASE("the master EQ is a bit-exact pass-through at its reset defaults", "[midi][synth][gs]") {
  // Writing the block its own power-on values must be indistinguishable from
  // never writing it: a knob that does not reproduce the baseline exactly at its
  // no-op value makes every measurement below an artifact.
  Sf2Player untouched = make_offline_player();
  mute_sends(untouched);
  untouched.on_event(0, event(sonare::midi::make_midi1_note_on(0, 0, 60, 100)));
  const StereoRender baseline = render(untouched, 4800);

  Sf2Player written = make_offline_player();
  mute_sends(written);
  const GsMasterEq defaults;
  const std::vector<uint8_t> eq_write =
      dt1(0x40, 0x02, 0x00,
          {defaults.low_freq, defaults.low_gain, defaults.high_freq, defaults.high_gain});
  REQUIRE(written.handle_sysex(eq_write.data(), eq_write.size()));
  written.on_event(0, event(sonare::midi::make_midi1_note_on(0, 0, 60, 100)));
  const StereoRender flat = render(written, 4800);

  REQUIRE(flat.left == baseline.left);
  REQUIRE(flat.right == baseline.right);
}

TEST_CASE("the master EQ at its reset defaults equals bypassing it", "[midi][synth][gs]") {
  // Compared against a render the EQ provably cannot touch — a part switched out
  // of a lifted EQ. A flat band that is not exactly unity, or a GAIN centre off
  // by a byte, separates the two.
  EqPoint bypassed;
  bypassed.low_gain = 0x4C;
  bypassed.high_gain = 0x4C;
  bypassed.part_eq_on = false;
  const StereoRender dry = render_tones(bypassed, 4800);
  const StereoRender flat = render_tones(EqPoint{}, 4800);
  REQUIRE(flat.left == dry.left);
  REQUIRE(flat.right == dry.right);
}

TEST_CASE("a part switched out of the master EQ is untouched by every EQ setting",
          "[midi][synth][gs]") {
  const StereoRender baseline = render_tones(EqPoint{}, 4800);
  for (EqPoint point : eq_grid()) {
    point.part_eq_on = false;
    INFO("low " << int(point.low_freq) << "/" << int(point.low_gain) << " high "
                << int(point.high_freq) << "/" << int(point.high_gain));
    const StereoRender out = render_tones(point, 4800);
    REQUIRE(out.left == baseline.left);
    REQUIRE(out.right == baseline.right);
  }
}

TEST_CASE("each master EQ band moves its own end of the spectrum", "[midi][synth][gs]") {
  const std::array<double, 4> flat = tone_powers(render_tones(EqPoint{}, kToneSamples));
  for (const EqPoint& point : eq_grid()) {
    INFO("low " << int(point.low_freq) << "/" << int(point.low_gain) << " high "
                << int(point.high_freq) << "/" << int(point.high_gain));
    const std::array<double, 4> p = tone_powers(render_tones(point, kToneSamples));
    const double low_db = power_ratio_db(p[0], flat[0]);   // 125 Hz
    const double high_db = power_ratio_db(p[3], flat[3]);  // 8 kHz
    // The low band owns 125 Hz and the high band owns 8 kHz; each corner is far
    // enough from the other end that the far band reads as untouched.
    if (point.low_gain > 0x40) {
      REQUIRE(low_db > 6.0);
    } else if (point.low_gain < 0x40) {
      REQUIRE(low_db < -6.0);
    } else {
      REQUIRE(std::fabs(low_db) < 0.5);
    }
    if (point.high_gain > 0x40) {
      REQUIRE(high_db > 6.0);
    } else if (point.high_gain < 0x40) {
      REQUIRE(high_db < -6.0);
    } else {
      REQUIRE(std::fabs(high_db) < 0.5);
    }
  }
}

TEST_CASE("master EQ LOW FREQ moves where the lift is", "[midi][synth][gs]") {
  const std::array<double, 4> flat = tone_powers(render_tones(EqPoint{}, kToneSamples));
  EqPoint at200;
  at200.low_freq = 0x00;
  at200.low_gain = 0x4C;
  EqPoint at400 = at200;
  at400.low_freq = 0x01;
  const std::array<double, 4> lifted200 = tone_powers(render_tones(at200, kToneSamples));
  const std::array<double, 4> lifted400 = tone_powers(render_tones(at400, kToneSamples));
  // 250 Hz sits above the 200 Hz corner and below the 400 Hz one, so moving the
  // corner up moves the shelf over it.
  const double db200 = power_ratio_db(lifted200[1], flat[1]);
  const double db400 = power_ratio_db(lifted400[1], flat[1]);
  REQUIRE(db400 > db200 + 2.0);
  // 125 Hz is inside both shelves; the higher corner still lifts it at least as
  // much, which is what says the corner moved rather than the plateau.
  REQUIRE(power_ratio_db(lifted400[0], flat[0]) >= power_ratio_db(lifted200[0], flat[0]) - 0.1);
}

TEST_CASE("a GS system-effect run lands on every field of its block", "[midi][synth][gs]") {
  // The census finds these blocks written as runs — 40 01 30 with 8 data bytes,
  // 40 01 50 with 11, 40 02 00 with 4 — so a run must reach the last field, not
  // just the first.
  Sf2Player player = make_offline_player();

  // Reverb: 30 selects a macro (which overwrites 31-37), then 31-35 and 37 take
  // their own bytes. 36 has no row and is skipped without disturbing the run.
  const std::vector<uint8_t> reverb =
      dt1(0x40, 0x01, 0x30, {0x02, 0x03, 0x04, 0x50, 0x60, 0x11, 0x00, 0x22});
  REQUIRE(player.handle_sysex(reverb.data(), reverb.size()));
  const GsSystemEffects& fx = player.gs_system_effects();
  CHECK(fx.reverb_macro == 0x02);
  CHECK(fx.reverb_character == 0x03);
  CHECK(fx.reverb_pre_lpf == 0x04);
  CHECK(fx.reverb_level == 0x50);
  CHECK(fx.reverb_time == 0x60);
  CHECK(fx.reverb_delay_feedback == 0x11);
  CHECK(fx.reverb_predelay == 0x22);

  const std::vector<uint8_t> delay =
      dt1(0x40, 0x01, 0x50, {0x03, 0x05, 0x40, 0x18, 0x0C, 0x70, 0x60, 0x50, 0x30, 0x20, 0x10});
  REQUIRE(player.handle_sysex(delay.data(), delay.size()));
  CHECK(fx.delay_macro == 0x03);
  CHECK(fx.delay_pre_lpf == 0x05);
  CHECK(fx.delay_time_center == 0x40);
  CHECK(fx.delay_time_ratio_left == 0x18);
  CHECK(fx.delay_time_ratio_right == 0x0C);
  CHECK(fx.delay_level_center == 0x70);
  CHECK(fx.delay_level_left == 0x60);
  CHECK(fx.delay_level_right == 0x50);
  CHECK(fx.delay_level == 0x30);
  CHECK(fx.delay_feedback == 0x20);
  CHECK(fx.delay_send_to_reverb == 0x10);

  const std::vector<uint8_t> chorus =
      dt1(0x40, 0x01, 0x38, {0x05, 0x02, 0x60, 0x30, 0x40, 0x20, 0x50, 0x10, 0x08});
  REQUIRE(player.handle_sysex(chorus.data(), chorus.size()));
  CHECK(fx.chorus_macro == 0x05);
  CHECK(fx.chorus_pre_lpf == 0x02);
  CHECK(fx.chorus_level == 0x60);
  CHECK(fx.chorus_feedback == 0x30);
  CHECK(fx.chorus_delay == 0x40);
  CHECK(fx.chorus_rate == 0x20);
  CHECK(fx.chorus_depth == 0x50);
  CHECK(fx.chorus_send_to_reverb == 0x10);
  CHECK(fx.chorus_send_to_delay == 0x08);

  const std::vector<uint8_t> eq = dt1(0x40, 0x02, 0x00, {0x01, 0x4C, 0x01, 0x34});
  REQUIRE(player.handle_sysex(eq.data(), eq.size()));
  CHECK(player.gs_master_eq().low_freq == 0x01);
  CHECK(player.gs_master_eq().low_gain == 0x4C);
  CHECK(player.gs_master_eq().high_freq == 0x01);
  CHECK(player.gs_master_eq().high_gain == 0x34);
}

TEST_CASE("an out-of-range system-effect value is ignored, not clamped", "[midi][synth][gs]") {
  Sf2Player player = make_offline_player();
  // DELAY TIME CENTER accepts 01-73; 00 and 7F are outside it, and the row's
  // default must survive both.
  for (const uint8_t value : {0x00, 0x7F}) {
    const std::vector<uint8_t> write = dt1(0x40, 0x01, 0x52, {value});
    player.handle_sysex(write.data(), write.size());
    CHECK(player.gs_system_effects().delay_time_center == 0x61);
  }
}

TEST_CASE("GS Reset restores the system-effect and master-EQ defaults", "[midi][synth][gs]") {
  Sf2Player player = make_offline_player();
  const std::vector<uint8_t> reverb = dt1(0x40, 0x01, 0x30, {0x06, 0x06, 0x07, 0x10, 0x7F});
  const std::vector<uint8_t> chorus = dt1(0x40, 0x01, 0x38, {0x05});
  const std::vector<uint8_t> delay = dt1(0x40, 0x01, 0x50, {0x09});
  const std::vector<uint8_t> eq = dt1(0x40, 0x02, 0x00, {0x01, 0x34, 0x01, 0x4C});
  const std::vector<uint8_t> eq_off = dt1(0x40, kPart1Block, 0x20, {0x00});
  for (const std::vector<uint8_t>* msg : {&reverb, &chorus, &delay, &eq, &eq_off}) {
    REQUIRE(player.handle_sysex(msg->data(), msg->size()));
  }
  const GsSystemEffects fx_defaults;
  const GsMasterEq eq_defaults;
  REQUIRE(std::memcmp(&player.gs_system_effects(), &fx_defaults, sizeof(GsSystemEffects)) != 0);
  REQUIRE_FALSE(player.gs_part_eq_enabled(0));

  const std::vector<uint8_t> gs_reset = dt1(0x40, 0x00, 0x7F, {0x00});
  REQUIRE(player.handle_sysex(gs_reset.data(), gs_reset.size()));
  CHECK(std::memcmp(&player.gs_system_effects(), &fx_defaults, sizeof(GsSystemEffects)) == 0);
  CHECK(std::memcmp(&player.gs_master_eq(), &eq_defaults, sizeof(GsMasterEq)) == 0);
  for (uint8_t ch = 0; ch < 16; ++ch) CHECK(player.gs_part_eq_enabled(ch));
}

// The system-effect units only exist on a build with the FX suite; without it
// the reverb / chorus / delay writes are held and nothing sounds.
#if defined(SONARE_MIDI_WITH_FX)

namespace {

/// Wet-tail energy left after the voices are cut, under a system-effect block
/// written by @p writes.
double wet_tail_with_send(const std::vector<std::vector<uint8_t>>& writes, uint8_t reverb_send) {
  Sf2Player player = make_offline_player();
  for (const std::vector<uint8_t>& msg : writes) {
    REQUIRE(player.handle_sysex(msg.data(), msg.size()));
  }
  player.on_event(0, event(sonare::midi::make_midi1_program_change(0, 0, 1)));
  player.on_event(0, event(sonare::midi::make_midi1_control_change(0, 0, 91, reverb_send)));
  player.on_event(0, event(sonare::midi::make_midi1_control_change(0, 0, 93, 0)));
  player.on_event(0, event(sonare::midi::make_midi1_control_change(0, 0, 94, 0)));
  player.on_event(0, event(sonare::midi::make_midi1_note_on(0, 0, 60, 127)));
  render(player, 2400);
  player.on_event(0, event(sonare::midi::make_midi1_control_change(0, 0, 120, 0)));
  const StereoRender out = render(player, 48000);
  return rms(out.left, 24000, 48000) + rms(out.right, 24000, 48000);
}

double wet_tail(const std::vector<std::vector<uint8_t>>& writes) {
  return wet_tail_with_send(writes, 127);
}

}  // namespace

TEST_CASE("the reverb return is what the wet tail measures", "[midi][synth][gs]") {
  // Without this, "the decay did not change" and "there is no reverb in this
  // measurement at all" both read as a flat set of numbers, and the cases below
  // cannot tell them apart.
  const double dry = wet_tail_with_send({}, 0);
  const double wet = wet_tail_with_send({}, 127);
  INFO("dry tail " << dry << ", wet tail " << wet);
  REQUIRE(wet > dry * 4.0);
}

TEST_CASE("a REVERB TIME write lengthens the measured decay monotonically", "[midi][synth][gs]") {
  const double shortest = wet_tail({dt1(0x40, 0x01, 0x34, {0x10})});
  const double middle = wet_tail({dt1(0x40, 0x01, 0x34, {0x40})});
  const double longest = wet_tail({dt1(0x40, 0x01, 0x34, {0x70})});
  // Printed because three equal numbers mean the write never reached the unit,
  // which is a different defect from a decay that moved the wrong way.
  INFO("tails " << shortest << " / " << middle << " / " << longest);
  REQUIRE(shortest > 0.0);
  REQUIRE(middle > shortest * 1.5);
  REQUIRE(longest > middle * 1.5);
}

TEST_CASE("the reverb macros select distinguishable rooms", "[midi][synth][gs]") {
  std::array<double, 8> tails{};
  for (uint8_t macro = 0; macro < 8; ++macro) {
    tails[macro] = wet_tail({dt1(0x40, 0x01, 0x30, {macro})});
  }
  for (size_t a = 0; a < 7; ++a) {
    for (size_t b = a + 1; b < 7; ++b) {
      INFO("macros " << a << " and " << b);
      REQUIRE(std::fabs(tails[a] - tails[b]) > 1e-9);
    }
  }
  // Delay and Panning Delay write identical parameter blocks and differ only in
  // a REVERB CHARACTER that selects the delay unit — a routing decision the bus
  // does not make yet, so the two are the same room today.
  REQUIRE(tails[7] == tails[6]);
}

TEST_CASE("a live system-effect edit reaches the audio without allocating",
          "[midi][synth][gs][rt]") {
  // The live path: the control thread owns the mirror and hands the audio thread
  // coefficients, so a mid-render edit neither rebuilds a unit nor allocates.
  Sf2PlayerConfig cfg;
  cfg.gain = 1.0f;  // realize_efx_inline stays false
  Sf2Player player(cfg);
  player.set_soundfont(make_fixture());
  player.prepare(kOutRate, 256);
  mute_sends(player);
  player.on_event(0, event(sonare::midi::make_midi1_note_on(0, 0, 24, 100)));
  const StereoRender before = render(player, 4800);

  const std::vector<uint8_t> lift = dt1(0x40, 0x02, 0x00, {0x00, 0x4C, 0x00, 0x40});
  player.on_control_sysex(lift.data(), lift.size());
  REQUIRE(player.gs_master_eq().low_gain == 0x4C);

  std::vector<float> left(512, 0.0f);
  std::vector<float> right(512, 0.0f);
  float* chans[2] = {left.data(), right.data()};
  AllocationGuard guard;
  player.process(chans, 2, 512);
  player.process(chans, 2, 512);
  REQUIRE(guard.count() == 0);

  const StereoRender after = render(player, 4800);
  REQUIRE(rms(after.left, 0, 4800) > rms(before.left, 2400, 4800) * 1.5);
}

#endif  // SONARE_MIDI_WITH_FX
