/// @file sf2_gs_layer_test.cpp
/// @brief GS architecture layer (build-plan P5): NRPN part parameters applied
///        as relative offsets onto SoundFont generators (TVF cutoff /
///        resonance, TVA envelope, vibrato), GS drum-kit per-note NRPNs,
///        SysEx recognition (GM System On / GS Reset / use-for-rhythm) and
///        the GS reset power-on state.

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <cstdint>
#include <memory>
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
using sonare::midi::synth::gs_drum_kit_name;
using sonare::midi::synth::GsSysEx;
using sonare::midi::synth::GsSysExKind;
using sonare::midi::synth::parse_gs_sysex;
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

/// Program 0: bright square loop with a mid filter (~2.4 kHz). The bank-128
/// kit maps the same loop so drum NRPNs can be measured tonally.
std::shared_ptr<Sf2File> make_fixture() {
  Sf2Builder b;

  std::vector<float> square(128);
  for (size_t i = 0; i < square.size(); ++i) {
    double v = 0.0;
    for (int h = 1; h <= 9; h += 2) {
      v += std::sin(kTwoPi * h * static_cast<double>(i) / 64.0) / h;
    }
    square[i] = 0.6f * static_cast<float>(v);
  }
  // 500 Hz at root 60 (period 64 at 32 kHz).
  const int sq_id = b.add_sample("square500", square, 32000, 60, 0, 128);

  Sf2Builder::ZoneSpec zone;
  zone.gens.push_back({54 /*sampleModes*/, 1});
  zone.gens.push_back({8 /*initialFilterFc*/, 8637});  // ~1.2 kHz
  zone.target = sq_id;
  const int inst = b.add_instrument("squareinst", {zone});

  Sf2Builder::ZoneSpec pz;
  pz.target = inst;
  b.add_preset("Square", 0, 0, {pz});

  Sf2Builder::ZoneSpec dz;
  dz.target = inst;
  b.add_preset("Kit", 128, 0, {dz});

  const auto bytes = b.build();
  auto sf2 = std::make_shared<Sf2File>();
  std::string error;
  REQUIRE(sf2->parse(bytes.data(), bytes.size(), &error));
  return sf2;
}

Sf2Player make_player() {
  Sf2PlayerConfig cfg;
  cfg.gain = 1.0f;
#if defined(SONARE_MIDI_WITH_FX)
  cfg.effects.enable_reverb = false;  // keep spectral measurements dry
  cfg.effects.enable_chorus = false;
  cfg.effects.enable_delay = false;
#endif
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

/// Sends NRPN (msb, lsb) = value on channel.
void send_nrpn(Sf2Player& player, uint8_t channel, uint8_t msb, uint8_t lsb, uint8_t value) {
  player.on_event(0, event(sonare::midi::make_midi1_control_change(0, channel, 99, msb)));
  player.on_event(0, event(sonare::midi::make_midi1_control_change(0, channel, 98, lsb)));
  player.on_event(0, event(sonare::midi::make_midi1_control_change(0, channel, 6, value)));
}

double band_energy(const std::vector<float>& buf, size_t from, double freq) {
  const double w = kTwoPi * freq / kOutRate;
  const double coeff = 2.0 * std::cos(w);
  double s1 = 0.0, s2 = 0.0;
  for (size_t i = from; i < buf.size(); ++i) {
    const double s0 = static_cast<double>(buf[i]) + coeff * s1 - s2;
    s2 = s1;
    s1 = s0;
  }
  return s1 * s1 + s2 * s2 - coeff * s1 * s2;
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

double estimate_frequency(const std::vector<float>& buf, size_t from) {
  double first = -1.0, last = -1.0;
  int cycles = -1;
  for (size_t i = from + 1; i < buf.size(); ++i) {
    if (buf[i - 1] < 0.0f && buf[i] >= 0.0f) {
      const double frac =
          static_cast<double>(buf[i - 1]) / (static_cast<double>(buf[i - 1]) - buf[i]);
      const double t = static_cast<double>(i - 1) + frac;
      (first < 0.0 ? first : last) = t;
      if (first < 0.0) first = t;
      ++cycles;
    }
  }
  if (cycles < 1 || last <= first) return 0.0;
  return kOutRate * static_cast<double>(cycles) / (last - first);
}

/// Harmonic balance (5th/fund) after applying a TVF cutoff NRPN offset.
double brightness_with_cutoff_nrpn(uint8_t data) {
  Sf2Player player = make_player();
  send_nrpn(player, 0, 0x01, 0x20, data);
  player.on_event(0, event(sonare::midi::make_midi1_note_on(0, 0, 60, 127)));
  const StereoRender out = render(player, 24000);
  return band_energy(out.left, 4800, 2500.0) / band_energy(out.left, 4800, 500.0);
}

}  // namespace

TEST_CASE("GS NRPN TVF cutoff shifts brightness monotonically", "[midi][sf2][gslayer]") {
  const double dark = brightness_with_cutoff_nrpn(44);     // -20 steps
  const double centre = brightness_with_cutoff_nrpn(64);   // no edit
  const double bright = brightness_with_cutoff_nrpn(104);  // +40 steps
  REQUIRE(dark < centre * 0.5);
  REQUIRE(bright > centre * 1.5);
}

TEST_CASE("GS NRPN EG release lengthens the tail", "[midi][sf2][gslayer]") {
  auto tail_rms_after_off = [](bool lengthen) {
    Sf2Player player = make_player();
    if (lengthen) send_nrpn(player, 0, 0x01, 0x66, 127);  // +63 steps
    player.on_event(0, event(sonare::midi::make_midi1_note_on(0, 0, 60, 127)));
    render(player, 4800);
    player.on_event(0, event(sonare::midi::make_midi1_note_off(0, 0, 60, 0)));
    const StereoRender out = render(player, 9600);
    return rms(out.left, 2400, 9600);  // 50..200 ms after note-off
  };
  const float normal = tail_rms_after_off(false);
  const float longer = tail_rms_after_off(true);
  REQUIRE(longer > normal * 2.0f + 1e-6f);
}

TEST_CASE("GS NRPN vibrato depth adds pitch modulation", "[midi][sf2][gslayer]") {
  Sf2Player player = make_player();
  send_nrpn(player, 0, 0x01, 0x09, 127);  // +63 steps ~ +189 cents depth
  player.on_event(0, event(sonare::midi::make_midi1_note_on(0, 0, 60, 127)));
  const StereoRender out = render(player, 48000);
  double min_hz = 1e9, max_hz = 0.0, prev = -1.0;
  for (size_t i = 9601; i < out.left.size(); ++i) {
    if (out.left[i - 1] < 0.0f && out.left[i] >= 0.0f) {
      const double frac = static_cast<double>(out.left[i - 1]) /
                          (static_cast<double>(out.left[i - 1]) - out.left[i]);
      const double t = static_cast<double>(i - 1) + frac;
      if (prev >= 0.0 && t > prev) {
        const double hz = kOutRate / (t - prev);
        min_hz = std::min(min_hz, hz);
        max_hz = std::max(max_hz, hz);
      }
      prev = t;
    }
  }
  REQUIRE(max_hz - min_hz > 20.0);  // audible vibrato spread around 500 Hz
}

TEST_CASE("GS drum NRPNs override pitch, level and pan per note", "[midi][sf2][gslayer]") {
  SECTION("pitch coarse (msb 0x18) transposes the note") {
    Sf2Player player = make_player();
    send_nrpn(player, 9, 0x18, 60, 76);  // +12 semitones on note 60
    player.on_event(0, event(sonare::midi::make_midi1_note_on(0, 9, 60, 127)));
    const StereoRender out = render(player, 24000);
    REQUIRE(estimate_frequency(out.left, 4800) == Approx(1000.0).margin(10.0));
  }

  SECTION("level (msb 0x1A) attenuates the note") {
    Sf2Player loud = make_player();
    loud.on_event(0, event(sonare::midi::make_midi1_note_on(0, 9, 60, 127)));
    const float loud_rms = rms(render(loud, 9600).left, 2400, 9600);

    Sf2Player soft = make_player();
    send_nrpn(soft, 9, 0x1A, 60, 40);
    soft.on_event(0, event(sonare::midi::make_midi1_note_on(0, 9, 60, 127)));
    const float soft_rms = rms(render(soft, 9600).left, 2400, 9600);
    REQUIRE(soft_rms < loud_rms * 0.5f);
  }

  SECTION("pan (msb 0x1C) moves the note in the stereo field") {
    Sf2Player player = make_player();
    send_nrpn(player, 9, 0x1C, 60, 127);  // hard right
    player.on_event(0, event(sonare::midi::make_midi1_note_on(0, 9, 60, 127)));
    const StereoRender out = render(player, 9600);
    REQUIRE(rms(out.right, 2400, 9600) > 10.0f * rms(out.left, 2400, 9600));
  }

  SECTION("drum NRPNs only apply to the addressed note") {
    Sf2Player player = make_player();
    send_nrpn(player, 9, 0x18, 62, 76);  // transpose note 62, not 60
    player.on_event(0, event(sonare::midi::make_midi1_note_on(0, 9, 60, 127)));
    const StereoRender out = render(player, 24000);
    REQUIRE(estimate_frequency(out.left, 4800) == Approx(500.0).margin(5.0));
  }
}

TEST_CASE("parse_gs_sysex recognises the GS/GM messages", "[midi][sf2][gslayer]") {
  const uint8_t gm_on[] = {0xF0, 0x7E, 0x7F, 0x09, 0x01, 0xF7};
  REQUIRE(parse_gs_sysex(gm_on, sizeof(gm_on)).kind == GsSysExKind::kGmReset);
  const uint8_t gm2_on[] = {0xF0, 0x7E, 0x7F, 0x09, 0x03, 0xF7};
  REQUIRE(parse_gs_sysex(gm2_on, sizeof(gm2_on)).kind == GsSysExKind::kGmReset);

  const uint8_t gs_reset[] = {0xF0, 0x41, 0x10, 0x42, 0x12, 0x40, 0x00, 0x7F, 0x00, 0x41, 0xF7};
  REQUIRE(parse_gs_sysex(gs_reset, sizeof(gs_reset)).kind == GsSysExKind::kGsReset);
  // Unframed payload (store strips F0/F7).
  REQUIRE(parse_gs_sysex(gs_reset + 1, sizeof(gs_reset) - 2).kind == GsSysExKind::kGsReset);

  // Use-for-rhythm: block 0x12 -> part 2 -> channel index 1, map 1.
  const uint8_t rhythm[] = {0xF0, 0x41, 0x10, 0x42, 0x12, 0x40, 0x12, 0x15, 0x01, 0x18, 0xF7};
  const GsSysEx msg = parse_gs_sysex(rhythm, sizeof(rhythm));
  REQUIRE(msg.kind == GsSysExKind::kUseForRhythm);
  REQUIRE(msg.channel == 1);
  REQUIRE(msg.value == 1);

  // Block 0 addresses part 10 (the default drum channel).
  const uint8_t rhythm10[] = {0xF0, 0x41, 0x10, 0x42, 0x12, 0x40, 0x10, 0x15, 0x01, 0x1A, 0xF7};
  REQUIRE(parse_gs_sysex(rhythm10, sizeof(rhythm10)).channel == 9);

  const uint8_t bad_sum[] = {0xF0, 0x41, 0x10, 0x42, 0x12, 0x40, 0x12, 0x15, 0x01, 0x19, 0xF7};
  REQUIRE(parse_gs_sysex(bad_sum, sizeof(bad_sum)).kind == GsSysExKind::kNone);

  const uint8_t missing_sum[] = {0xF0, 0x41, 0x10, 0x42, 0x12, 0x40, 0x12, 0x15, 0x01, 0xF7};
  REQUIRE(parse_gs_sysex(missing_sum, sizeof(missing_sum)).kind == GsSysExKind::kNone);

  const uint8_t junk[] = {0xF0, 0x43, 0x10, 0x4C, 0x00, 0x00, 0x7E, 0x00, 0xF7};  // XG reset
  REQUIRE(parse_gs_sysex(junk, sizeof(junk)).kind == GsSysExKind::kNone);
  REQUIRE(parse_gs_sysex(nullptr, 0).kind == GsSysExKind::kNone);
}

TEST_CASE("use-for-rhythm SysEx turns a melodic channel into drums", "[midi][sf2][gslayer]") {
  Sf2Player player = make_player();
  // Channel 1 plays the melodic preset by default; mark it as rhythm and it
  // must resolve bank 128 (the kit) even with bank MSB 0.
  const uint8_t rhythm[] = {0xF0, 0x41, 0x10, 0x42, 0x12, 0x40, 0x12, 0x15, 0x01, 0x18, 0xF7};
  REQUIRE(player.handle_sysex(rhythm, sizeof(rhythm)));
  // Drum NRPNs now work on channel 1.
  send_nrpn(player, 1, 0x18, 60, 76);
  player.on_event(0, event(sonare::midi::make_midi1_note_on(0, 1, 60, 127)));
  const StereoRender out = render(player, 24000);
  REQUIRE(estimate_frequency(out.left, 4800) == Approx(1000.0).margin(10.0));
}

TEST_CASE("GS reset restores power-on state", "[midi][sf2][gslayer]") {
  Sf2Player player = make_player();
  // Make edits: NRPN cutoff, program change, bank, bend.
  send_nrpn(player, 0, 0x01, 0x20, 24);
  player.on_event(0, event(sonare::midi::make_midi1_control_change(0, 0, 0, 8)));
  player.on_event(0, event(sonare::midi::make_midi1_pitch_bend(0, 0, 16383)));

  const uint8_t gs_reset_bytes[] = {0xF0, 0x41, 0x10, 0x42, 0x12, 0x40,
                                    0x00, 0x7F, 0x00, 0x41, 0xF7};
  REQUIRE(player.handle_sysex(gs_reset_bytes, sizeof(gs_reset_bytes)));

  // After reset the NRPN cutoff edit is gone: brightness matches a fresh player.
  player.on_event(0, event(sonare::midi::make_midi1_note_on(0, 0, 60, 127)));
  const StereoRender out = render(player, 24000);
  const double after = band_energy(out.left, 4800, 2500.0) / band_energy(out.left, 4800, 500.0);

  Sf2Player fresh = make_player();
  fresh.on_event(0, event(sonare::midi::make_midi1_note_on(0, 0, 60, 127)));
  const StereoRender fresh_out = render(fresh, 24000);
  const double baseline =
      band_energy(fresh_out.left, 4800, 2500.0) / band_energy(fresh_out.left, 4800, 500.0);
  REQUIRE(after == Approx(baseline).epsilon(0.05));

  // Pitch bend was reset too: frequency back at 500 Hz.
  REQUIRE(estimate_frequency(out.left, 4800) == Approx(500.0).margin(5.0));
}

TEST_CASE("GS drum kit names", "[midi][sf2][gslayer]") {
  REQUIRE(gs_drum_kit_name(0) == "Standard");
  REQUIRE(gs_drum_kit_name(25) == "TR-808");
  REQUIRE(gs_drum_kit_name(56) == "SFX");
  REQUIRE(gs_drum_kit_name(3).empty());
}
