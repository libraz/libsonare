/// @file sf2_effects_test.cpp
/// @brief GS effect bus (build-plan P4): reverb/chorus/delay send-returns
///        behind CC91/93/94 and the SF2 send generators — send monotonicity,
///        wet tails after the dry signal ends, effect tails inside
///        tail_samples(), per-part insert drive, deterministic effects and a
///        no-alloc audio path with effects engaged.

#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <cstdint>
#include <memory>
#include <vector>

#include "midi/midi_event.h"
#include "midi/synth/sf2_file.h"
#include "midi/synth/sf2_player.h"
#include "midi/ump.h"
#include "support/alloc_guard.h"
#include "support/sf2_builder.h"

namespace {

using sonare::midi::MidiEvent;
using sonare::midi::synth::Sf2File;
using sonare::midi::synth::Sf2InsertType;
using sonare::midi::synth::Sf2Player;
using sonare::midi::synth::Sf2PlayerConfig;
using sonare::test::AllocationGuard;
using sonare::test::Sf2Builder;

constexpr double kOutRate = 48000.0;
constexpr double kTwoPi = 6.28318530717958647692;

MidiEvent event(const sonare::midi::Ump& ump) {
  MidiEvent e;
  e.ump = ump;
  return e;
}

/// Fixture: program 0 = looped 1 kHz sine, program 1 = short one-shot burst,
/// program 2 = the burst with a zone-level reverb send (gen 16 = 500 -> 0.5).
std::shared_ptr<Sf2File> make_fixture() {
  Sf2Builder b;

  std::vector<float> sine(96);
  for (size_t i = 0; i < sine.size(); ++i) {
    sine[i] = 0.9f * static_cast<float>(std::sin(kTwoPi * static_cast<double>(i) / 32.0));
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

  Sf2Builder::ZoneSpec wet = oneshot;
  wet.gens.push_back({16 /*reverbEffectsSend*/, 500});
  const int wet_inst = b.add_instrument("wetburst", {wet});

  auto preset = [&](const char* name, uint16_t prog, int inst) {
    Sf2Builder::ZoneSpec pz;
    pz.target = inst;
    b.add_preset(name, 0, prog, {pz});
  };
  preset("Sine", 0, sine_inst);
  preset("Burst", 1, burst_inst);
  preset("WetBurst", 2, wet_inst);

  const auto bytes = b.build();
  auto sf2 = std::make_shared<Sf2File>();
  std::string error;
  REQUIRE(sf2->parse(bytes.data(), bytes.size(), &error));
  return sf2;
}

Sf2Player make_player(Sf2PlayerConfig cfg = {}) {
  if (!(cfg.gain > 0.0f)) cfg.gain = 1.0f;
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

float rms(const std::vector<float>& buf, size_t from, size_t to) {
  double acc = 0.0;
  size_t n = 0;
  to = std::min(to, buf.size());
  for (size_t i = from; i < to; ++i) {
    acc += static_cast<double>(buf[i]) * buf[i];
    ++n;
  }
  return n > 0 ? static_cast<float>(std::sqrt(acc / static_cast<double>(n))) : 0.0f;
}

float peak(const std::vector<float>& buf, size_t from, size_t to) {
  float p = 0.0f;
  to = std::min(to, buf.size());
  for (size_t i = from; i < to; ++i) p = std::max(p, std::fabs(buf[i]));
  return p;
}

/// Wet-tail energy: play the one-shot burst (program @p prog) with CC91 set
/// to @p cc91 and measure RMS well after the dry burst (~5 ms) has ended.
float reverb_tail_rms(uint8_t cc91, uint8_t prog = 1) {
  Sf2Player player = make_player();
  player.on_event(0, event(sonare::midi::make_midi1_program_change(0, 0, prog)));
  player.on_event(0, event(sonare::midi::make_midi1_control_change(0, 0, 91, cc91)));
  player.on_event(0, event(sonare::midi::make_midi1_note_on(0, 0, 60, 127)));
  const StereoRender out = render(player, 24000);
  return rms(out.left, 4800, 24000) + rms(out.right, 4800, 24000);
}

}  // namespace

TEST_CASE("GS reverb send is monotonic in CC91", "[midi][sf2][gsfx]") {
  const float dry = reverb_tail_rms(0);
  const float mid = reverb_tail_rms(64);
  const float full = reverb_tail_rms(127);
  REQUIRE(dry == 0.0f);  // no send -> no wet tail
  REQUIRE(mid > 0.0f);
  REQUIRE(full > mid * 1.5f);
}

TEST_CASE("SF2 zone reverb send generator feeds the bus without CC", "[midi][sf2][gsfx]") {
  // Program 2 carries reverbEffectsSend=500 in the zone itself.
  const float zone_wet = reverb_tail_rms(0, 2);
  REQUIRE(zone_wet > 0.0f);
  // CC91 adds on top of the zone send.
  const float zone_plus_cc = reverb_tail_rms(127, 2);
  REQUIRE(zone_plus_cc > zone_wet);
}

TEST_CASE("GS delay send produces an echo at the delay time", "[midi][sf2][gsfx]") {
  auto echo_peak = [](uint8_t cc94) {
    Sf2Player player = make_player();
    player.on_event(0, event(sonare::midi::make_midi1_program_change(0, 0, 1)));
    player.on_event(0, event(sonare::midi::make_midi1_control_change(0, 0, 94, cc94)));
    player.on_event(0, event(sonare::midi::make_midi1_note_on(0, 0, 60, 127)));
    const StereoRender out = render(player, 20000);
    // Default delay time 340 ms = 16320 samples; the dry burst is ~256.
    return peak(out.left, 15800, 17500);
  };
  REQUIRE(echo_peak(0) == 0.0f);
  REQUIRE(echo_peak(127) > 0.001f);
}

TEST_CASE("GS chorus send adds wet signal", "[midi][sf2][gsfx]") {
  auto early_rms = [](uint8_t cc93) {
    Sf2Player player = make_player();
    player.on_event(0, event(sonare::midi::make_midi1_program_change(0, 0, 1)));
    player.on_event(0, event(sonare::midi::make_midi1_control_change(0, 0, 93, cc93)));
    player.on_event(0, event(sonare::midi::make_midi1_note_on(0, 0, 60, 127)));
    const StereoRender out = render(player, 4800);
    // The chorus return arrives ~center_delay (14 ms) after the dry burst.
    return rms(out.left, 600, 2400) + rms(out.right, 600, 2400);
  };
  const float off = early_rms(0);
  const float on = early_rms(127);
  REQUIRE(on > off + 1e-4f);
}

TEST_CASE("tail_samples covers the effect ring-out", "[midi][sf2][gsfx]") {
  Sf2PlayerConfig with_fx;
  with_fx.gain = 1.0f;
  Sf2Player player = make_player(with_fx);

  Sf2PlayerConfig no_fx;
  no_fx.gain = 1.0f;
#if defined(SONARE_MIDI_WITH_FX)
  no_fx.effects.enable_reverb = false;
  no_fx.effects.enable_chorus = false;
  no_fx.effects.enable_delay = false;
#endif
  Sf2Player dry_player = make_player(no_fx);

#if defined(SONARE_MIDI_WITH_FX)
  REQUIRE(player.tail_samples() > dry_player.tail_samples());
#else
  REQUIRE(player.tail_samples() == dry_player.tail_samples());
#endif

  // The wet tail must actually be silent after tail_samples.
  player.on_event(0, event(sonare::midi::make_midi1_program_change(0, 0, 1)));
  player.on_event(0, event(sonare::midi::make_midi1_control_change(0, 0, 91, 127)));
  player.on_event(0, event(sonare::midi::make_midi1_note_on(0, 0, 60, 127)));
  player.on_event(0, event(sonare::midi::make_midi1_note_off(0, 0, 60, 0)));
  render(player, player.tail_samples() + 4800);
  const StereoRender after = render(player, 2400);
  REQUIRE(peak(after.left, 0, after.left.size()) < 1e-3f);
}

TEST_CASE("per-part insert drive saturates only its part", "[midi][sf2][gsfx]") {
  // Drive on part 0: a looped 1 kHz sine gains odd harmonics.
  Sf2PlayerConfig cfg;
  cfg.gain = 1.0f;
  cfg.part_inserts[0].type = Sf2InsertType::kDrive;
  cfg.part_inserts[0].amount = 1.0f;
  Sf2Player driven = make_player(cfg);
  driven.on_event(0, event(sonare::midi::make_midi1_note_on(0, 0, 60, 127)));
  const StereoRender drv = render(driven, 9600);

  Sf2Player clean = make_player();
  clean.on_event(0, event(sonare::midi::make_midi1_note_on(0, 0, 60, 127)));
  const StereoRender cln = render(clean, 9600);

  // Goertzel at the 3rd harmonic (3 kHz).
  auto h3 = [](const std::vector<float>& buf) {
    const double w = kTwoPi * 3000.0 / kOutRate;
    const double coeff = 2.0 * std::cos(w);
    double s1 = 0.0, s2 = 0.0;
    for (size_t i = 2400; i < buf.size(); ++i) {
      const double s0 = static_cast<double>(buf[i]) + coeff * s1 - s2;
      s2 = s1;
      s1 = s0;
    }
    return s1 * s1 + s2 * s2 - coeff * s1 * s2;
  };
  REQUIRE(h3(drv.left) > 100.0 * h3(cln.left));

  // A note on part 1 (no insert) stays clean even when part 0 has drive.
  Sf2Player other = make_player(cfg);
  other.on_event(0, event(sonare::midi::make_midi1_note_on(0, 1, 60, 127)));
  const StereoRender oth = render(other, 9600);
  REQUIRE(h3(oth.left) < 100.0 * h3(cln.left));
}

TEST_CASE("GS effects render bit-identically", "[midi][sf2][gsfx]") {
  auto run = [] {
    Sf2Player player = make_player();
    player.on_event(0, event(sonare::midi::make_midi1_program_change(0, 0, 1)));
    player.on_event(0, event(sonare::midi::make_midi1_control_change(0, 0, 91, 100)));
    player.on_event(0, event(sonare::midi::make_midi1_control_change(0, 0, 93, 80)));
    player.on_event(0, event(sonare::midi::make_midi1_control_change(0, 0, 94, 60)));
    player.on_event(0, event(sonare::midi::make_midi1_note_on(0, 0, 60, 127)));
    const StereoRender out = render(player, 24000);
    std::vector<float> both = out.left;
    both.insert(both.end(), out.right.begin(), out.right.end());
    return both;
  };
  REQUIRE(run() == run());
}

TEST_CASE("GS effect bus audio path performs no heap allocation", "[midi][sf2][gsfx][rt]") {
  Sf2PlayerConfig cfg;
  cfg.gain = 1.0f;
  cfg.part_inserts[0].type = Sf2InsertType::kDrive;
  cfg.part_inserts[0].amount = 0.5f;
  Sf2Player player = make_player(cfg);
  std::vector<float> left(512, 0.0f), right(512, 0.0f);
  float* chans[2] = {left.data(), right.data()};

  player.on_event(0, event(sonare::midi::make_midi1_control_change(0, 0, 91, 100)));
  player.on_event(0, event(sonare::midi::make_midi1_control_change(0, 0, 94, 100)));
  player.on_event(0, event(sonare::midi::make_midi1_note_on(0, 0, 60, 100)));
  player.process(chans, 2, 512);  // warm-up

  AllocationGuard guard;
  player.on_event(0, event(sonare::midi::make_midi1_note_on(0, 0, 64, 100)));
  player.process(chans, 2, 512);
  player.on_event(0, event(sonare::midi::make_midi1_control_change(0, 0, 123, 0)));
  player.process(chans, 2, 512);
  REQUIRE(guard.count() == 0);
}
