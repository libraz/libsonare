/// @file sf2_gs_layer_test.cpp
/// @brief GS architecture layer (build-plan P5): NRPN part parameters applied
///        as relative offsets onto SoundFont generators (TVF cutoff /
///        resonance, TVA envelope, vibrato), GS drum-kit per-note NRPNs,
///        SysEx recognition (GM System On / GS Reset / use-for-rhythm) and
///        the GS reset power-on state.

#include <array>
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <cstdint>
#include <initializer_list>
#include <memory>
#include <string>
#include <vector>

#include "midi/midi_event.h"
#include "midi/synth/gs_layer.h"
#include "midi/synth/sf2_file.h"
#include "midi/synth/sf2_player.h"
#include "midi/ump.h"
#include "support/midi_render.h"
#include "support/sf2_builder.h"

namespace {

using Catch::Approx;
using sonare::midi::MidiEvent;
using sonare::midi::synth::apply_gs_efx_units_sysex;
using sonare::midi::synth::gs_drum_kit_name;
using sonare::midi::synth::gs_efx_insert_chain;
using sonare::midi::synth::gs_efx_insert_name;
using sonare::midi::synth::gs_efx_insert_params;
using sonare::midi::synth::GsEfx;
using sonare::midi::synth::GsSysEx;
using sonare::midi::synth::GsSysExKind;
using sonare::midi::synth::kGsEfxUnitCount;
using sonare::midi::synth::parse_gs_sysex;
using sonare::midi::synth::Sf2File;
using sonare::midi::synth::Sf2Player;
using sonare::midi::synth::Sf2PlayerConfig;
using sonare::test::Sf2Builder;

constexpr double kOutRate = 48000.0;
constexpr double kTwoPi = 6.28318530717958647692;

using sonare::test::event;

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

/// 40 1x 15 USE FOR RHYTHM PART for @p channel with drum map @p map, carrying
/// its checksum. Block 0 is part 10 and blocks 1-9 are parts 1-9, which is
/// gs_part_block_to_channel read backwards.
std::vector<uint8_t> use_for_rhythm_sysex(uint8_t channel, uint8_t map) {
  const uint8_t block =
      channel == 9 ? 0u : (channel < 9 ? static_cast<uint8_t>(channel + 1) : channel);
  const uint8_t addr_mid = static_cast<uint8_t>(0x10 | block);
  const uint8_t sum = static_cast<uint8_t>(0x40 + addr_mid + 0x15 + map);
  const uint8_t checksum = static_cast<uint8_t>((0x80 - (sum & 0x7F)) & 0x7F);
  return {0xF0, 0x41, 0x10, 0x42, 0x12, 0x40, addr_mid, 0x15, map, checksum, 0xF7};
}

/// Builds a framed GS DT1 run from a 24-bit address. The bulk SysEx cases in
/// this file deliberately cross rows, so their checksums should be derived from
/// the exact bytes under test rather than copied from a single-byte fixture.
std::vector<uint8_t> gs_dt1_run(uint8_t addr_mid, uint8_t addr_low,
                                std::initializer_list<uint8_t> values) {
  std::vector<uint8_t> message = {0xF0, 0x41, 0x10, 0x42, 0x12, 0x40, addr_mid, addr_low};
  uint32_t sum = 0x40u + addr_mid + addr_low;
  for (const uint8_t value : values) {
    message.push_back(value);
    sum += value;
  }
  message.push_back(static_cast<uint8_t>((128u - (sum & 0x7Fu)) & 0x7Fu));
  message.push_back(0xF7);
  return message;
}

/// Same frame builder for a long bulk run. The short helper above intentionally
/// takes an initializer list; this one makes the truncation boundary explicit.
std::vector<uint8_t> gs_dt1_long_run(uint8_t addr_mid, uint8_t addr_low,
                                     const std::vector<uint8_t>& values) {
  std::vector<uint8_t> message = {0xF0, 0x41, 0x10, 0x42, 0x12, 0x40, addr_mid, addr_low};
  uint32_t sum = 0x40u + addr_mid + addr_low;
  for (const uint8_t value : values) {
    message.push_back(value);
    sum += value;
  }
  message.push_back(static_cast<uint8_t>((128u - (sum & 0x7Fu)) & 0x7Fu));
  message.push_back(0xF7);
  return message;
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

TEST_CASE("a GS drum note send scales its part's, it does not add to it", "[midi][sf2][gslayer]") {
  // docs/gs.md, deliberate divergences: the manual calls 41 m5/m6/m9 a
  // "Multiplicand of the part reverb level" over 0.0-1.0, so a drum note's send
  // scales what the note sends into that unit. It lands on a scale rather than
  // on the zone's own send value because the part's CC send is the other half of
  // what the render sums, and both halves are scaled — the property that follows
  // from it is asserted on rendered buses in gs_drum_send_test.cpp.
  using sonare::midi::synth::apply_gs_drum_params;
  using sonare::midi::synth::GsDrumNoteParams;
  using sonare::midi::synth::Sf2VoiceParams;

  GsDrumNoteParams drum;
  drum.flags = GsDrumNoteParams::kReverb | GsDrumNoteParams::kChorus | GsDrumNoteParams::kDelay;
  drum.reverb = 80;
  drum.chorus = 80;
  drum.delay = 80;

  SECTION("it lands on the scale, leaving the zone's own send value alone") {
    Sf2VoiceParams params;
    params.reverb_send = 0.5f;
    params.chorus_send = 0.4f;
    apply_gs_drum_params(params, drum);
    REQUIRE(params.reverb_send == Approx(0.5f));
    REQUIRE(params.chorus_send == Approx(0.4f));
    REQUIRE(params.reverb_send_scale == Approx(80.0f / 127.0f));
    REQUIRE(params.chorus_send_scale == Approx(80.0f / 127.0f));
    REQUIRE(params.delay_send_scale == Approx(80.0f / 127.0f));
  }

  SECTION("a multiplicand of 127 is unity, so an unwritten parameter changes nothing") {
    Sf2VoiceParams params;
    GsDrumNoteParams full = drum;
    full.reverb = 127;
    full.chorus = 127;
    full.delay = 127;
    apply_gs_drum_params(params, full);
    REQUIRE(params.reverb_send_scale == 1.0f);
    REQUIRE(params.chorus_send_scale == 1.0f);
    REQUIRE(params.delay_send_scale == 1.0f);
  }

  SECTION("a multiplicand of 0 takes the note out of the bus") {
    Sf2VoiceParams params;
    GsDrumNoteParams none = drum;
    none.reverb = 0;
    none.chorus = 0;
    none.delay = 0;
    apply_gs_drum_params(params, none);
    REQUIRE(params.reverb_send_scale == 0.0f);
    REQUIRE(params.chorus_send_scale == 0.0f);
    REQUIRE(params.delay_send_scale == 0.0f);
  }
}

TEST_CASE("a GS assign group replaces the kit piece's own class", "[midi][sf2][gslayer]") {
  // The three sends above scale what the kit gave the note; this one overwrites
  // it, so 00 is the value that says something rather than the value that says
  // nothing. A file uses it to take a piece out of the group its kit put it in
  // — an open hi-hat that should ring through the closed one — and adding or
  // scaling would leave that unreachable.
  using sonare::midi::synth::apply_gs_drum_params;
  using sonare::midi::synth::GsDrumNoteParams;
  using sonare::midi::synth::Sf2VoiceParams;

  GsDrumNoteParams drum;
  drum.flags = GsDrumNoteParams::kAssignGroup;

  SECTION("an unwritten group leaves the kit's alone") {
    Sf2VoiceParams params;
    params.exclusive_class = 5;
    apply_gs_drum_params(params, GsDrumNoteParams{});
    REQUIRE(params.exclusive_class == 5);
  }

  SECTION("a written group replaces it") {
    Sf2VoiceParams params;
    params.exclusive_class = 5;
    drum.assign_group = 9;
    apply_gs_drum_params(params, drum);
    REQUIRE(params.exclusive_class == 9);
  }

  SECTION("00 is OFF, and takes the note out of the kit's group") {
    Sf2VoiceParams params;
    params.exclusive_class = 5;
    drum.assign_group = 0;
    apply_gs_drum_params(params, drum);
    REQUIRE(params.exclusive_class == 0);
  }
}

TEST_CASE("parse_gs_sysex recognises the GS/GM messages", "[midi][sf2][gslayer]") {
  const uint8_t gm_on[] = {0xF0, 0x7E, 0x7F, 0x09, 0x01, 0xF7};
  REQUIRE(parse_gs_sysex(gm_on, sizeof(gm_on)).kind == GsSysExKind::kGm1Reset);
  const uint8_t gm2_on[] = {0xF0, 0x7E, 0x7F, 0x09, 0x03, 0xF7};
  REQUIRE(parse_gs_sysex(gm2_on, sizeof(gm2_on)).kind == GsSysExKind::kGm2Reset);

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

TEST_CASE("bulk EFX assignment applies accepted bytes after the first address",
          "[midi][sf2][gsefx][gslayer]") {
  // 40 41 20 starts at PART EQ SWITCH and reaches OUTPUT ASSIGN, then PART
  // EFX ASSIGN at 40 41 22. Only the third byte routes part 1 through EFX.
  const std::vector<uint8_t> route = gs_dt1_run(0x41, 0x20, {0x01, 0x00, 0x01});
  // The final value is outside PART EFX ASSIGN's 00-10 range. It must leave
  // the route selected by the previous message untouched.
  const std::vector<uint8_t> invalid = gs_dt1_run(0x41, 0x20, {0x01, 0x00, 0x11});

  SECTION("offline inline mirror") {
    Sf2PlayerConfig cfg;
    cfg.gain = 1.0f;
    cfg.realize_efx_inline = true;
    Sf2Player player(cfg);
    player.set_soundfont(make_fixture());
    player.prepare(kOutRate, 256);

    REQUIRE(player.handle_sysex(route.data(), route.size()));
    REQUIRE(player.gs_efx_assign(0) == 1);
    REQUIRE(player.handle_sysex(invalid.data(), invalid.size()));
    REQUIRE(player.gs_efx_assign(0) == 1);
  }

  SECTION("control-thread mirror") {
    Sf2Player player = make_player();
    player.on_control_sysex(route.data(), route.size());
    REQUIRE(player.gs_efx_assign(0) == 1);
    player.on_control_sysex(invalid.data(), invalid.size());
    REQUIRE(player.gs_efx_assign(0) == 1);
  }
}

TEST_CASE("bulk part runs apply rhythm selection and preserve later bytes",
          "[midi][sf2][gslayer]") {
  // 40 11 14 starts at ASSIGN MODE (the polyphonic/multi assignment), then
  // reaches USE FOR RHYTHM PART, KEY SHIFT and both PITCH OFFSET FINE nibbles.
  const std::vector<uint8_t> map2 = gs_dt1_run(0x11, 0x14, {0x01, 0x02, 0x48, 0x08, 0x00});
  const std::vector<uint8_t> invalid_map = gs_dt1_run(0x11, 0x14, {0x01, 0x7F, 0x48, 0x08, 0x00});

  SECTION("map 2 is selected from the middle of the run") {
    Sf2Player player = make_player();
    REQUIRE(player.handle_sysex(map2.data(), map2.size()));
    REQUIRE(player.assign_mode(0) == 1);
    REQUIRE(player.pitch_key_shift(0) == 0x48);
    REQUIRE(player.pitch_offset_fine(0) == 0x80);

    // The fixture's drum sample is 500 Hz. A +12 semitone drum NRPN edit is
    // held by map 2, proving that the second byte selected the kit map.
    send_nrpn(player, 0, 0x18, 60, 76);
    player.on_event(0, event(sonare::midi::make_midi1_note_on(0, 0, 60, 127)));
    REQUIRE(estimate_frequency(render(player, 24000).left, 4800) == Approx(1000.0).margin(10.0));
  }

  SECTION("an invalid map value follows the GS map-1 divergence") {
    Sf2Player player = make_player();
    REQUIRE(player.handle_sysex(map2.data(), map2.size()));
    send_nrpn(player, 0, 0x18, 60, 76);  // map 2 only
    REQUIRE(player.handle_sysex(invalid_map.data(), invalid_map.size()));
    REQUIRE(player.assign_mode(0) == 1);
    REQUIRE(player.pitch_key_shift(0) == 0x48);
    REQUIRE(player.pitch_offset_fine(0) == 0x80);

    // Invalid USE FOR RHYTHM PART values mean map 1. The map-2 edit above
    // must therefore no longer affect this note.
    player.on_event(0, event(sonare::midi::make_midi1_note_on(0, 0, 60, 127)));
    REQUIRE(estimate_frequency(render(player, 24000).left, 4800) == Approx(500.0).margin(10.0));
  }
}

TEST_CASE("long part bulk runs do not lose a rhythm assignment after 64 bytes",
          "[midi][sf2][gslayer]") {
  // Starting at 40 10 00, the next part block begins at byte 128 and its USE
  // FOR RHYTHM PART row is byte 149. This is beyond apply_gs_part_sysex's old
  // 64-write decode window.
  std::vector<uint8_t> values(150, 0x00);
  const auto preserve_part_defaults = [&values](size_t base, uint8_t rx_channel) {
    values[base + 2] = rx_channel;
    for (size_t offset = base + 3; offset <= base + 18; ++offset) values[offset] = 0x01;
    values[base + 19] = 0x01;  // MONO/POLY: poly.
    values[base + 20] = 0x01;  // ASSIGN MODE: limited multi.
    values[base + 21] = 0x01;  // USE FOR RHYTHM PART: map 1.
    values[base + 22] = 0x40;  // PITCH KEY SHIFT: centre.
    values[base + 23] = 0x08;  // PITCH OFFSET FINE: high nibble.
    values[base + 24] = 0x00;  // PITCH OFFSET FINE: low nibble.
    values[base + 25] = 0x64;  // PART LEVEL.
    values[base + 26] = 0x40;  // VELOCITY SENSE DEPTH.
    values[base + 27] = 0x40;  // VELOCITY SENSE OFFSET.
    values[base + 28] = 0x40;  // PANPOT.
    values[base + 29] = 0x00;  // KEY RANGE LOW.
    values[base + 30] = 0x7F;  // KEY RANGE HIGH.
  };
  preserve_part_defaults(0, 0x09);    // Keep part 10 on MIDI channel 10.
  preserve_part_defaults(128, 0x00);  // Part 1 listens to MIDI channel 1.
  values[147] = 0x01;                 // 40 11 13 MONO/POLY: poly.
  values[148] = 0x01;                 // 40 11 14 ASSIGN MODE: limited multi.
  values[149] = 0x02;                 // 40 11 15 USE FOR RHYTHM PART: drum map 2.
  const std::vector<uint8_t> bulk = gs_dt1_long_run(0x10, 0x00, values);

  Sf2Player player = make_player();
  REQUIRE(player.handle_sysex(bulk.data(), bulk.size()));
  send_nrpn(player, 0, 0x18, 60, 76);  // map 2 drum edit: +12 semitones.
  player.on_event(0, event(sonare::midi::make_midi1_note_on(0, 0, 60, 127)));
  REQUIRE(estimate_frequency(render(player, 24000).left, 4800) == Approx(1000.0).margin(10.0));
}

TEST_CASE("apply_gs_efx_units_sysex captures the EFX block as raw wire", "[midi][sf2][gslayer]") {
  // EFX TYPE write (40 03 00, two data bytes 01 10 = Overdrive). Checksum over
  // 40 03 00 01 10 = 84 -> 0x2C.
  const uint8_t type_write[] = {0xF0, 0x41, 0x10, 0x42, 0x12, 0x40,
                                0x03, 0x00, 0x01, 0x10, 0x2C, 0xF7};
  std::array<GsEfx, kGsEfxUnitCount> efx{};
  REQUIRE(apply_gs_efx_units_sysex(efx, type_write, sizeof(type_write)));
  REQUIRE(efx[0].type == 0x0110);
  REQUIRE(efx[0].assigned);
  // Unframed payload parses identically (framing is stripped).
  std::array<GsEfx, kGsEfxUnitCount> efx_unframed{};
  REQUIRE(apply_gs_efx_units_sysex(efx_unframed, type_write + 1, sizeof(type_write) - 2));
  REQUIRE(efx_unframed[0].type == 0x0110);

  // EFX PARAMETER 1 write (40 03 03, data 0x64 = 100). Checksum 0x56.
  const uint8_t param_write[] = {0xF0, 0x41, 0x10, 0x42, 0x12, 0x40, 0x03, 0x03, 0x64, 0x56, 0xF7};
  REQUIRE(apply_gs_efx_units_sysex(efx, param_write, sizeof(param_write)));
  REQUIRE(efx[0].params[0] == 100);
  REQUIRE(efx[0].type == 0x0110);  // the earlier type is preserved across writes

  // A full-block run from 0x00: type 01 10, reserved 00, params 1..3 = 10 02 00,
  // the last two inside the four- and two-state lists those slots print.
  // Checksum over 40 03 00 01 10 00 10 02 00 = 102 -> 0x1A.
  const uint8_t run[] = {0xF0, 0x41, 0x10, 0x42, 0x12, 0x40, 0x03, 0x00,
                         0x01, 0x10, 0x00, 0x10, 0x02, 0x00, 0x1A, 0xF7};
  std::array<GsEfx, kGsEfxUnitCount> efx_run{};
  REQUIRE(apply_gs_efx_units_sysex(efx_run, run, sizeof(run)));
  REQUIRE(efx_run[0].type == 0x0110);
  REQUIRE(efx_run[0].params[0] == 0x10);
  REQUIRE(efx_run[0].params[1] == 0x02);
  REQUIRE(efx_run[0].params[2] == 0x00);

  // A non-EFX Roland message (GS reset, address 40 00 7F) is not an EFX write.
  const uint8_t gs_reset[] = {0xF0, 0x41, 0x10, 0x42, 0x12, 0x40, 0x00, 0x7F, 0x00, 0x41, 0xF7};
  std::array<GsEfx, kGsEfxUnitCount> untouched{};
  REQUIRE_FALSE(apply_gs_efx_units_sysex(untouched, gs_reset, sizeof(gs_reset)));
  REQUIRE_FALSE(untouched[0].assigned);

  // A bad checksum is rejected and leaves the struct untouched.
  uint8_t corrupt[sizeof(type_write)];
  for (size_t i = 0; i < sizeof(type_write); ++i) corrupt[i] = type_write[i];
  corrupt[10] ^= 0x7F;  // wreck the checksum
  std::array<GsEfx, kGsEfxUnitCount> efx_corrupt{};
  REQUIRE_FALSE(apply_gs_efx_units_sysex(efx_corrupt, corrupt, sizeof(corrupt)));
  REQUIRE_FALSE(efx_corrupt[0].assigned);
  REQUIRE(apply_gs_efx_units_sysex(untouched, nullptr, 0) == false);
}

TEST_CASE("gs_efx_insert_name maps the adapted EFX types to inserts", "[midi][sf2][gslayer]") {
  // GS EFX type numbers (SC-88Pro MSB<<8|LSB) -> insert-factory names.
  REQUIRE(gs_efx_insert_name(0x0100) == "eq.parametric");                    // Stereo-EQ
  REQUIRE(gs_efx_insert_name(0x0101) == "eq.graphic");                       // Spectrum
  REQUIRE(gs_efx_insert_name(0x0102) == "spectral.presenceEnhancer");        // Enhancer
  REQUIRE(gs_efx_insert_name(0x0110) == "saturation.overdrive");             // Overdrive
  REQUIRE(gs_efx_insert_name(0x0111) == "saturation.distortion");            // Distortion
  REQUIRE(gs_efx_insert_name(0x0120) == "effects.modulation.phaser");        // Phaser
  REQUIRE(gs_efx_insert_name(0x0121) == "effects.modulation.autoWah");       // Auto Wah
  REQUIRE(gs_efx_insert_name(0x0122) == "effects.modulation.rotary");        // Rotary
  REQUIRE(gs_efx_insert_name(0x0123) == "effects.modulation.flanger");       // Stereo Flanger
  REQUIRE(gs_efx_insert_name(0x0124) == "effects.modulation.flanger");       // Step Flanger
  REQUIRE(gs_efx_insert_name(0x0126) == "stereo.autoPan");                   // Auto Pan
  REQUIRE(gs_efx_insert_name(0x0130) == "dynamics.compressor");              // Compressor
  REQUIRE(gs_efx_insert_name(0x0131) == "dynamics.limiter");                 // Limiter
  REQUIRE(gs_efx_insert_name(0x0140) == "effects.modulation.ensemble");      // Hexa Chorus
  REQUIRE(gs_efx_insert_name(0x0141) == "effects.modulation.chorus");        // Tremolo Chorus
  REQUIRE(gs_efx_insert_name(0x0142) == "effects.modulation.chorus");        // Stereo Chorus
  REQUIRE(gs_efx_insert_name(0x0143) == "effects.modulation.chorus");        // Space-D
  REQUIRE(gs_efx_insert_name(0x0144) == "effects.modulation.chorus");        // 3D Chorus
  REQUIRE(gs_efx_insert_name(0x0150) == "effects.delay.stereo");             // Stereo Delay
  REQUIRE(gs_efx_insert_name(0x0151) == "effects.delay.stereo");             // Modulation Delay
  REQUIRE(gs_efx_insert_name(0x0152) == "effects.delay.stereo");             // 3-tap Delay
  REQUIRE(gs_efx_insert_name(0x0154) == "effects.delay.stereo");             // Time Control Delay
  REQUIRE(gs_efx_insert_name(0x0155) == "effects.reverb.dattorro");          // Reverb
  REQUIRE(gs_efx_insert_name(0x0156) == "effects.reverb.dattorro");          // Gate Reverb
  REQUIRE(gs_efx_insert_name(0x0157) == "effects.delay.stereo");             // 3D Delay
  REQUIRE(gs_efx_insert_name(0x0160) == "effects.modulation.pitchShifter");  // 2-voice Pitch Shift
  REQUIRE(gs_efx_insert_name(0x0161) == "effects.modulation.pitchShifter");  // Feedback Pitch Shift
  REQUIRE(gs_efx_insert_name(0x0172) == "saturation.bitcrusher");            // Lo-Fi 1
  REQUIRE(gs_efx_insert_name(0x0173) == "saturation.bitcrusher");            // Lo-Fi 2
  REQUIRE(gs_efx_insert_name(0x0000).empty());                               // Thru
  // Tremolo is the ring modulator run as amplitude modulation; the full
  // per-type coverage is in gs_efx_types_test.cpp.
  REQUIRE(gs_efx_insert_name(0x0125) == "effects.modulation.ringModulator");
  REQUIRE(gs_efx_insert_name(0x0103) == "effects.filter.vowel");  // Humanizer
  REQUIRE(gs_efx_insert_name(0x0170) == "stereo.binaural");       // 3D Auto
  REQUIRE(gs_efx_insert_name(0x0171) == "stereo.binaural");       // 3D Manual
}

TEST_CASE("gs_efx_insert_params translates the drive per mapped type", "[midi][sf2][gslayer]") {
  // Overdrive -> the overdrive pedal. EFX PARAMETER 1 is a gain in front of its
  // curve, written by its binding row alone (gainDb): the skeleton's own object
  // does not read the byte.
  GsEfx od;
  od.type = 0x0110;
  od.params[0] = 0;
  const std::string low = gs_efx_insert_params(od);
  const std::string low_chain = gs_efx_insert_chain(od).front().params_json;
  od.params[0] = 127;
  const std::string high = gs_efx_insert_params(od);
  const std::string high_chain = gs_efx_insert_chain(od).front().params_json;
  REQUIRE(low.find("\"gainDb\"") == std::string::npos);
  REQUIRE(low == high);
  REQUIRE(low_chain.find("\"gainDb\"") != std::string::npos);
  REQUIRE(low_chain != high_chain);

  GsEfx dist;
  dist.type = 0x0111;  // Distortion -> the distortion pedal
  dist.params[0] = 127;
  REQUIRE(gs_efx_insert_chain(dist).front().name == "saturation.distortion");

  // Output Level (EFX PARAMETER 20) -> levelDb, the multiplier the unit stores
  // for the byte read in dB over a -24 dB floor. A byte of 0 is the value zero
  // and takes the floor rather than reading as an absence: selecting a type
  // loads that type's own twenty bytes, so a zero here is one the file asked for.
  // It is the unit's own output stage that carries it, not the drive block:
  // every type holds the level at this slot, so it is read once for all of them.
  auto level_of = [](const GsEfx& efx) {
    for (const auto& stage : gs_efx_insert_chain(efx)) {
      if (stage.name == "utility.gain") return stage.params_json;
    }
    return std::string{};
  };
  GsEfx lvl;
  lvl.type = 0x0110;
  lvl.params[0] = 100;
  lvl.params[19] = 0;
  REQUIRE(level_of(lvl).find("\"levelDb\":-24") != std::string::npos);
  lvl.params[19] = 64;  // ~half of unity -> a negative levelDb
  REQUIRE(level_of(lvl).find("\"levelDb\":-") != std::string::npos);
  lvl.params[19] = 127;  // unity -> 0 dB
  REQUIRE(level_of(lvl).find("\"levelDb\":0") != std::string::npos);

  GsEfx thru;  // unmapped type -> the insert's defaults
  thru.type = 0x0114;
  REQUIRE(gs_efx_insert_params(thru) == "{}");
}

TEST_CASE("the pitch shifter's coarse and balance bytes reach its stage", "[midi][sf2][gslayer]") {
  // Coarse Pitch is bound and Balance is the skeleton's, so both are read off the
  // realised chain rather than off either half alone.
  auto shifter_of = [](const GsEfx& efx) {
    for (const auto& stage : gs_efx_insert_chain(efx)) {
      if (stage.name == "effects.modulation.pitchShifter") return stage.params_json;
    }
    return std::string{};
  };
  // Coarse Pitch (EFX PARAMETER 1 = params[0]) is a 64-centred semitone offset.
  GsEfx up;
  up.type = 0x0160;   // 2-voice Pitch Shifter
  up.params[0] = 76;  // 64 + 12 -> +12 semitones (one octave up)
  REQUIRE(shifter_of(up).find("\"semitones\":12") != std::string::npos);
  up.params[0] = 52;  // 64 - 12 -> -12 semitones
  REQUIRE(shifter_of(up).find("\"semitones\":-12") != std::string::npos);

  // Neither byte reads 0 as "unset". A coarse byte of 0 is 64 steps below centre
  // and clamps to the -24 st floor; a balance byte of 0 is all direct signal and
  // is emitted as such. Both keys are always present.
  GsEfx zeroed;
  zeroed.type = 0x0161;  // Feedback Pitch Shifter shares the translation
  zeroed.params[0] = 0;
  zeroed.params[15] = 0;
  REQUIRE(shifter_of(zeroed).find("\"semitones\":-24") != std::string::npos);
  REQUIRE(shifter_of(zeroed).find("\"dryWet\":0") != std::string::npos);

  // Effect Balance (PARAMETER 16 = params[15]) -> dry/wet when set.
  GsEfx mixed;
  mixed.type = 0x0160;
  mixed.params[0] = 71;    // +7 semitones
  mixed.params[15] = 127;  // full effect
  const std::string json = shifter_of(mixed);
  REQUIRE(json.find("\"semitones\":7") != std::string::npos);
  REQUIRE(json.find("\"dryWet\":1") != std::string::npos);
}

TEST_CASE("gs_efx_insert_chain expands a composite type into its block chain",
          "[midi][sf2][gslayer]") {
  // A single-effect type yields a one-stage chain.
  GsEfx od;
  od.type = 0x0110;  // Overdrive
  const auto single = gs_efx_insert_chain(od);
  // One stage realises the effect; the unit's output stage follows it, as it
  // follows every effect, and is checked where it belongs.
  REQUIRE_FALSE(single.empty());
  REQUIRE(single[0].name == "saturation.overdrive");

  // GTR Multi 2 (04 01) yields its Cmp-OD-EQ-CF block chain, in signal order.
  // A composite's parameter block is laid out per type: this type's EQ gains sit
  // at slots 9 and 13, not at the 16/17 the single-effect types use.
  GsEfx gtr;
  gtr.type = 0x0401;
  gtr.params[9] = 52;   // EQ Low Gain -12 dB (0x34, centre 64)
  gtr.params[13] = 76;  // EQ Hi Gain  +12 dB (0x4C)
  // The OD block places both pedals its OD Sel chooses between and an amp for
  // each OD Amp state, and the CF block both the chorus and the flanger its CF
  // Sel chooses between.
  gtr.params[4] = 1;   // OD Sel: the distortion pedal
  gtr.params[6] = 2;   // OD Amp: 2-Stk
  gtr.params[8] = 1;   // OD Sw: on
  gtr.params[14] = 1;  // CF Sel: the flanger
  const auto chain = gs_efx_insert_chain(gtr);
  REQUIRE(chain.size() >= 10);  // the four blocks, then the unit's output stage
  REQUIRE(chain[0].name == "dynamics.compressor");
  REQUIRE(chain[1].name == "saturation.overdrive");
  REQUIRE(chain[2].name == "saturation.distortion");
  for (size_t i = 3; i < 7; ++i) REQUIRE(chain[i].name == "saturation.ampSim");
  REQUIRE(chain[7].name == "eq.parametric");
  REQUIRE(chain[8].name == "effects.modulation.chorus");
  REQUIRE(chain[9].name == "effects.modulation.flanger");
  // The selectors switch exactly one of each set on.
  REQUIRE_FALSE(chain[1].enabled);
  REQUIRE(chain[2].enabled);
  for (size_t i = 3; i < 7; ++i) REQUIRE(chain[i].enabled == (i == 5));
  REQUIRE_FALSE(chain[8].enabled);
  REQUIRE(chain[9].enabled);
  // The EQ block is the composite's true tone control: Low/Hi Gain -> shelves.
  REQUIRE(chain[7].params_json.find("\"band0.gainDb\":-12") != std::string::npos);
  REQUIRE(chain[7].params_json.find("\"band2.gainDb\":12") != std::string::npos);

  // A zero gain byte is the bottom of the window and not an absence: the type
  // loads its own twenty bytes when it is selected, so no state means "unset".
  GsEfx flat;
  flat.type = 0x0401;
  flat.params[9] = 0;
  REQUIRE(gs_efx_insert_chain(flat)[7].params_json.find("\"band0.gainDb\":-12") !=
          std::string::npos);

  // A parallel-2 type (MSB 11) realises its two halves side by side: Cho/Delay
  // puts the chorus in half A and the delay in half B, each followed by its own
  // level and pan.
  GsEfx parallel;
  parallel.type = 0x1100;
  const auto halves = gs_efx_insert_chain(parallel);
  REQUIRE(halves.size() >= 6);
  REQUIRE(halves[0].name == "effects.modulation.chorus");
  REQUIRE(halves[0].branch == sonare::midi::synth::kGsEfxBranchHalfA);
  REQUIRE(halves[1].name == "utility.gain");
  REQUIRE(halves[2].name == "stereo.stereoBalance");
  REQUIRE(halves[2].branch == sonare::midi::synth::kGsEfxBranchHalfA);
  REQUIRE(halves[3].name == "effects.delay.stereo");
  REQUIRE(halves[3].branch == sonare::midi::synth::kGsEfxBranchHalfB);
  REQUIRE(halves[4].name == "utility.gain");
  REQUIRE(halves[5].name == "stereo.stereoBalance");
  REQUIRE(halves[5].branch == sonare::midi::synth::kGsEfxBranchHalfB);

  // Thru yields an empty chain (bypass).
  GsEfx thru;
  REQUIRE(gs_efx_insert_chain(thru).empty());
}

TEST_CASE("gs_efx_insert_chain covers the guitar/bass multi block", "[midi][sf2][gslayer]") {
  // The effect's own stages, which are the head of the chain: the unit's output
  // stage is appended after them for every type and is not this block's shape.
  auto names = [](uint16_t type, size_t blocks) {
    GsEfx efx;
    efx.type = type;
    std::vector<std::string> out;
    for (const auto& stage : gs_efx_insert_chain(efx)) out.push_back(stage.name);
    REQUIRE(out.size() >= blocks);
    out.resize(blocks);
    return out;
  };
  // The SC-88Pro MSB-04 guitar/bass multi block, each in its manual signal order
  // (every block now has a matching insert: Wah / Auto-Wah are realised too). An
  // OD block is the two pedals its OD Sel chooses between and an amp for each
  // OD Amp state, and a CF block the chorus and flanger its CF Sel chooses between.
  const std::string amp = "saturation.ampSim";
  const std::string odrv = "saturation.overdrive";
  const std::string dist = "saturation.distortion";
  const std::string cho = "effects.modulation.chorus";
  const std::string fl = "effects.modulation.flanger";
  const std::string dly = "effects.delay.stereo";
  REQUIRE(names(0x0400, 10) ==  // GTR Multi 1: Cmp-OD-CF-Dly
          std::vector<std::string>{"dynamics.compressor", odrv, dist, amp, amp, amp, amp, cho, fl,
                                   dly});
  REQUIRE(names(0x0402, 10) ==  // GTR Multi 3: Wah-OD-CF-Dly
          std::vector<std::string>{"effects.modulation.wah", odrv, dist, amp, amp, amp, amp, cho,
                                   fl, dly});
  REQUIRE(names(0x0403, 5) ==  // Clean GTR Multi 1: Cmp-EQ-CF-Dly (no OD)
          std::vector<std::string>{"dynamics.compressor", "eq.parametric", cho, fl, dly});
  REQUIRE(names(0x0404, 5) ==  // Clean GTR Multi 2: AW-EQ-CF-Dly
          std::vector<std::string>{"effects.modulation.autoWah", "eq.parametric", cho, fl, dly});
  REQUIRE(names(0x0405, 9) ==  // Bass Multi: Cmp-OD-EQ-CF, three printed amp types
          std::vector<std::string>{"dynamics.compressor", odrv, dist, amp, amp, amp,
                                   "eq.parametric", cho, fl});

  // The OD Amp byte turns one amp on.
  GsEfx bass;
  bass.type = 0x0405;
  bass.params[6] = 2;
  bass.params[8] = 1;  // OD Sw: on
  const auto bass_chain = gs_efx_insert_chain(bass);
  for (size_t i = 3; i < 6; ++i) REQUIRE(bass_chain[i].enabled == (i == 5));
  REQUIRE(bass_chain[5].params_json.find("\"preset\":\"britStack\"") != std::string::npos);
  bass.params[6] = 0;
  REQUIRE(gs_efx_insert_chain(bass)[3].enabled);
}

TEST_CASE("gs_efx_insert_chain expands the series-2 and multi composites", "[midi][sf2][gslayer]") {
  // The effect's own stages, which are the head of the chain: the unit's output
  // stage is appended after them for every type and is not this block's shape.
  auto names = [](uint16_t type, size_t blocks) {
    GsEfx efx;
    efx.type = type;
    std::vector<std::string> out;
    for (const auto& stage : gs_efx_insert_chain(efx)) out.push_back(stage.name);
    REQUIRE(out.size() >= blocks);
    out.resize(blocks);
    return out;
  };
  // Series-2 composites (SC-88Pro MSB 02): two stock effects in signal order.
  // A drive block is its pedal and an amp for each Amp Type state.
  const std::string amp = "saturation.ampSim";
  REQUIRE(names(0x0200, 6) ==  // OD -> Chorus
          std::vector<std::string>{"saturation.overdrive", amp, amp, amp, amp,
                                   "effects.modulation.chorus"});
  REQUIRE(
      names(0x0202, 6) ==  // OD -> Delay
      std::vector<std::string>{"saturation.overdrive", amp, amp, amp, amp, "effects.delay.stereo"});
  REQUIRE(names(0x0206, 2) ==  // EH -> Chorus
          std::vector<std::string>{"spectral.presenceEnhancer", "effects.modulation.chorus"});
  REQUIRE(names(0x0209, 2) ==  // Cho -> Delay
          std::vector<std::string>{"effects.modulation.chorus", "effects.delay.stereo"});
  REQUIRE(names(0x020B, 2) ==  // Cho -> Flanger
          std::vector<std::string>{"effects.modulation.chorus", "effects.modulation.flanger"});
  // The distortion series-2 blocks run the distortion pedal.
  REQUIRE(names(0x0204, 6) ==  // DS -> Flanger
          std::vector<std::string>{"saturation.distortion", amp, amp, amp, amp,
                                   "effects.modulation.flanger"});

  // Rotary Multi: OD -> 3-band EQ -> Rotary, reachable via both type numbers the
  // manual prints for it (chapter-4 body 03 00 and appendix table 02 0C). It
  // prints no amp type, so its OD block runs one amp.
  const std::vector<std::string> rotary_multi{"saturation.overdrive", amp, "eq.parametric",
                                              "effects.modulation.rotary"};
  REQUIRE(names(0x0300, rotary_multi.size()) == rotary_multi);
  REQUIRE(names(0x020C, rotary_multi.size()) == rotary_multi);

  // Rhodes Multi (04 06): Enhancer -> Phaser -> Chorus/Flanger -> Tremolo/Pan,
  // each pair the one its CF Sel and TP Sel choose between.
  REQUIRE(names(0x0406, 6) ==
          std::vector<std::string>{"spectral.presenceEnhancer", "effects.modulation.phaser",
                                   "effects.modulation.chorus", "effects.modulation.flanger",
                                   "effects.modulation.ringModulator", "stereo.autoPan"});
  GsEfx rhodes;
  rhodes.type = 0x0406;
  rhodes.params[14] = 1;  // TP Sel: the auto-pan
  rhodes.params[18] = 1;  // TP Sw: on
  const auto rhodes_chain = gs_efx_insert_chain(rhodes);
  REQUIRE_FALSE(rhodes_chain[4].enabled);
  REQUIRE(rhodes_chain[5].enabled);

  // Keyboard Multi (05 00): Ring Mod -> EQ -> Pitch Shifter -> Phaser -> Delay.
  // This is the only GS type that binds the ring-modulator insert.
  REQUIRE(names(0x0500, 5) ==
          std::vector<std::string>{"effects.modulation.ringModulator", "eq.parametric",
                                   "effects.modulation.pitchShifter", "effects.modulation.phaser",
                                   "effects.delay.stereo"});
}

TEST_CASE("parse_gs_sysex recognises the per-part EFX switch", "[midi][sf2][gslayer]") {
  // 40 41 22 01 = EFX ON for part 1 (channel index 0); checksum 0x5C.
  const uint8_t on[] = {0xF0, 0x41, 0x10, 0x42, 0x12, 0x40, 0x41, 0x22, 0x01, 0x5C, 0xF7};
  const GsSysEx msg = parse_gs_sysex(on, sizeof(on));
  REQUIRE(msg.kind == GsSysExKind::kEfxPartSwitch);
  REQUIRE(msg.channel == 0);
  REQUIRE(msg.value == 1);
  // 40 40 22 00 = EFX OFF for part 10 (block 0 -> channel 9); checksum 0x5E.
  const uint8_t off10[] = {0xF0, 0x41, 0x10, 0x42, 0x12, 0x40, 0x40, 0x22, 0x00, 0x5E, 0xF7};
  const GsSysEx off = parse_gs_sysex(off10, sizeof(off10));
  REQUIRE(off.kind == GsSysExKind::kEfxPartSwitch);
  REQUIRE(off.channel == 9);
  REQUIRE(off.value == 0);
}

TEST_CASE("Sf2Player stores the GS EFX unit and clears it on reset", "[midi][sf2][gslayer]") {
  Sf2Player player = make_player();
  REQUIRE_FALSE(player.gs_efx().assigned);

  const uint8_t type_write[] = {0xF0, 0x41, 0x10, 0x42, 0x12, 0x40,
                                0x03, 0x00, 0x01, 0x11, 0x2B, 0xF7};  // Distortion
  // The control-thread SysEx path owns the EFX mirror (gs_efx()) for a live
  // player; it parses the unit write and republishes the realised inserts.
  player.on_control_sysex(type_write, sizeof(type_write));
  REQUIRE(player.gs_efx().assigned);
  REQUIRE(player.gs_efx().type == 0x0111);

  const uint8_t gs_reset_bytes[] = {0xF0, 0x41, 0x10, 0x42, 0x12, 0x40,
                                    0x00, 0x7F, 0x00, 0x41, 0xF7};
  player.on_control_sysex(gs_reset_bytes, sizeof(gs_reset_bytes));
  REQUIRE_FALSE(player.gs_efx().assigned);
  REQUIRE(player.gs_efx().type == 0);
}

TEST_CASE("an offline bounce clears the GS EFX unit on reset too", "[midi][sf2][gslayer]") {
  // Two separate pieces of code clear the mirror, because the thread that owns
  // it differs: live, the control thread clears it while parsing the reset;
  // offline (realize_efx_inline), the render thread clears it in
  // reset_all_state. The case above covers the first, this one the second --
  // which is the path an offline bounce takes.
  Sf2PlayerConfig cfg;
  cfg.gain = 1.0f;
  cfg.realize_efx_inline = true;
  Sf2Player player(cfg);
  player.set_soundfont(make_fixture());
  player.prepare(kOutRate, 256);

  const uint8_t type_write[] = {0xF0, 0x41, 0x10, 0x42, 0x12, 0x40,
                                0x03, 0x00, 0x01, 0x11, 0x2B, 0xF7};  // Distortion
  const uint8_t gs_reset_bytes[] = {0xF0, 0x41, 0x10, 0x42, 0x12, 0x40,
                                    0x00, 0x7F, 0x00, 0x41, 0xF7};

  // Asserting the selection first is what gives the two below their teeth: a
  // player that never took the type would answer Thru afterwards either way.
  REQUIRE(player.handle_sysex(type_write, sizeof(type_write)));
  REQUIRE(player.gs_efx().assigned);
  REQUIRE(player.gs_efx().type == 0x0111);

  REQUIRE(player.handle_sysex(gs_reset_bytes, sizeof(gs_reset_bytes)));
  REQUIRE_FALSE(player.gs_efx().assigned);
  REQUIRE(player.gs_efx().type == 0);
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

TEST_CASE("drum-note edits belong to the map, not to the part", "[midi][sf2][gslayer]") {
  // 40 1x 15 selects a drum MAP and the drum setup it keys is addressed per map
  // (docs/gs.md), so two parts on one map read one set of per-note edits and a
  // part on the other map reads its own.
  const auto pitch_of = [](uint8_t play_channel) {
    Sf2Player player = make_player();
    const struct {
      uint8_t channel;
      uint8_t map;
    } parts[] = {{1, 1}, {2, 1}, {3, 2}};
    for (const auto& part : parts) {
      const std::vector<uint8_t> bytes = use_for_rhythm_sysex(part.channel, part.map);
      REQUIRE(player.handle_sysex(bytes.data(), bytes.size()));
    }
    // +12 semitones on drum note 60, written from part 2 (map 1).
    send_nrpn(player, 1, 0x18, 60, 76);
    player.on_event(0, event(sonare::midi::make_midi1_note_on(0, play_channel, 60, 127)));
    return estimate_frequency(render(player, 24000).left, 4800);
  };

  CHECK(pitch_of(1) == Approx(1000.0).margin(10.0));  // the part that wrote it
  CHECK(pitch_of(2) == Approx(1000.0).margin(10.0));  // same map: shares it
  CHECK(pitch_of(3) == Approx(500.0).margin(10.0));   // map 2: untouched
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

TEST_CASE("the reset sends come up at the values the map gives", "[midi][sf2][gslayer]") {
  // docs/gs.md: reset defaults are part of the contract, not implementation
  // detail. Nothing pinned these, so 40 1x 21 CHORUS SEND LEVEL sat at 8 where
  // the map says 0 and 40 03 17 EFX SEND TO REVERB at 0 where it says 40, and
  // moving either was invisible.
  const auto render_note = [](Sf2Player& player) {
    player.on_event(0, event(sonare::midi::make_midi1_note_on(0, 0, 60, 127)));
    return render(player, 12000);
  };

  SECTION("chorus send is silent until a file asks for it") {
    // The reset default is only reachable through a reset: a freshly built
    // player initialises its channel state to zero whatever the default is, so
    // a case that skips the reset measures the initialiser and not the
    // contract. This one was written that way first and passed against the
    // wrong value.
    const uint8_t gs_reset[] = {0xF0, 0x41, 0x10, 0x42, 0x12, 0x40, 0x00, 0x7F, 0x00, 0x41, 0xF7};
    // make_player() switches the send-return units off so spectral cases stay
    // dry, which also makes a chorus send unobservable; this case needs one.
    const auto make_wet_player = []() {
      Sf2PlayerConfig cfg;
      cfg.gain = 1.0f;
      Sf2Player player(cfg);
      player.set_soundfont(make_fixture());
      player.prepare(kOutRate, 256);
      return player;
    };
    const auto after_reset = [&](int chorus_cc) {
      Sf2Player player = make_wet_player();
      REQUIRE(player.handle_sysex(gs_reset, sizeof(gs_reset)));
      if (chorus_cc >= 0) {
        player.on_event(0, event(sonare::midi::make_midi1_control_change(
                               0, 0, 93, static_cast<uint8_t>(chorus_cc))));
      }
      return render_note(player).left;
    };
    // Identical to the same reset told to send 0 explicitly means the default
    // already was 0; the second comparison is what says the first could have
    // told them apart.
    REQUIRE(after_reset(-1) == after_reset(0));
    REQUIRE_FALSE(after_reset(-1) == after_reset(8));
  }

  SECTION("the EFX reverb send comes up at 40") {
    Sf2Player player = make_player();
    REQUIRE(player.gs_efx().send_reverb == 40);
    const uint8_t gs_reset[] = {0xF0, 0x41, 0x10, 0x42, 0x12, 0x40, 0x00, 0x7F, 0x00, 0x41, 0xF7};
    REQUIRE(player.handle_sysex(gs_reset, sizeof(gs_reset)));
    REQUIRE(player.gs_efx().send_reverb == 40);
    const uint8_t gm_on[] = {0xF0, 0x7E, 0x7F, 0x09, 0x01, 0xF7};
    REQUIRE(player.handle_sysex(gm_on, sizeof(gm_on)));
    REQUIRE(player.gs_efx().send_reverb == 40);
  }
}

TEST_CASE("SYSTEM MODE SET resets, and only on the value the target accepts",
          "[midi][sf2][gslayer]") {
  // 00 00 7F is the second address that resets on the SC-8850, which has no
  // Mode-2 (docs/gs.md). A census of 4 026 real files writes it in 1 899 of
  // them, with value 00 in 1 988 messages and the SC-88Pro's Mode-2 request in
  // three; ignoring the second is what the target does.
  const auto bend_up = [](Sf2Player& player) {
    player.on_event(0, event(sonare::midi::make_midi1_pitch_bend(0, 0, 16383)));
  };
  const auto sounded_frequency = [](Sf2Player& player) {
    player.on_event(0, event(sonare::midi::make_midi1_note_on(0, 0, 60, 127)));
    const StereoRender out = render(player, 24000);
    return estimate_frequency(out.left, 4800);
  };

  SECTION("value 00 resets") {
    Sf2Player player = make_player();
    bend_up(player);
    const uint8_t sms[] = {0xF0, 0x41, 0x10, 0x42, 0x12, 0x00, 0x00, 0x7F, 0x00, 0x01, 0xF7};
    REQUIRE(player.handle_sysex(sms, sizeof(sms)));
    REQUIRE(sounded_frequency(player) == Approx(500.0).margin(5.0));
  }

  SECTION("value 01, the Mode-2 request, changes nothing") {
    // Asserted on state that would visibly have been cleared, not on a return
    // value: a message that is merely unrecognised and one that resets both
    // report the same thing to the caller.
    Sf2Player player = make_player();
    bend_up(player);
    const uint8_t mode2[] = {0xF0, 0x41, 0x10, 0x42, 0x12, 0x00, 0x00, 0x7F, 0x01, 0x00, 0xF7};
    player.handle_sysex(mode2, sizeof(mode2));
    // The default bend range is two semitones, so a full bend puts the 500 Hz
    // note at 561 Hz; a reset would have put it back at 500.
    REQUIRE(sounded_frequency(player) == Approx(561.0).margin(6.0));
  }
}

TEST_CASE("the bank-select receive switches refuse and zero", "[midi][sf2][gslayer]") {
  using sonare::midi::synth::gs_rx_switch_bit;
  using sonare::midi::synth::GsRxSwitch;

  // One sample at three pitches, so which bank answered is read off the note.
  // The GM2 melodic bank MSB is what puts the LSB in charge of the bank number
  // (gs_effective_bank), which is what makes the LSB switch observable at all.
  const auto banks_fixture = []() {
    Sf2Builder b;
    std::vector<float> square(128);
    for (size_t i = 0; i < square.size(); ++i) {
      double v = 0.0;
      for (int h = 1; h <= 9; h += 2) {
        v += std::sin(kTwoPi * h * static_cast<double>(i) / 64.0) / h;
      }
      square[i] = 0.6f * static_cast<float>(v);
    }
    const int sq = b.add_sample("square500", square, 32000, 60, 0, 128);
    const auto tuned = [&](int semitones, const char* name) {
      Sf2Builder::ZoneSpec zone;
      zone.gens.push_back({54 /*sampleModes*/, 1});
      if (semitones != 0) {
        zone.gens.push_back({51 /*coarseTune*/, static_cast<int16_t>(semitones)});
      }
      zone.target = sq;
      return b.add_instrument(name, {zone});
    };
    const int plain = tuned(0, "plain");
    const int up = tuned(12, "up");
    const int down = tuned(-12, "down");
    Sf2Builder::ZoneSpec pz;
    pz.target = plain;
    b.add_preset("Bank 0", 0, 0, {pz});
    pz.target = up;
    b.add_preset("Bank 8", 8, 0, {pz});
    pz.target = down;
    b.add_preset("Bank 3", 3, 0, {pz});
    const auto bytes = b.build();
    auto sf2 = std::make_shared<Sf2File>();
    std::string error;
    REQUIRE(sf2->parse(bytes.data(), bytes.size(), &error));
    return std::shared_ptr<const Sf2File>(sf2);
  };

  const auto make = [&banks_fixture]() {
    Sf2PlayerConfig cfg;
    cfg.gain = 1.0f;
#if defined(SONARE_MIDI_WITH_FX)
    cfg.effects.enable_reverb = false;
    cfg.effects.enable_chorus = false;
    cfg.effects.enable_delay = false;
#endif
    auto player = std::make_unique<Sf2Player>(cfg);
    player->set_soundfont(banks_fixture());
    player->prepare(kOutRate, 256);
    return player;
  };
  const auto cc = [](Sf2Player& p, uint8_t controller, uint8_t value) {
    p.on_event(0, event(sonare::midi::make_midi1_control_change(0, 0, controller, value)));
  };
  const auto switch_off = [](Sf2Player& p, uint8_t low_byte) {
    // 40 11 xx: part 1, which is channel 0.
    const uint8_t addr[3] = {0x40, 0x11, low_byte};
    const int sum = addr[0] + addr[1] + addr[2];
    const uint8_t msg[] = {
        0xF0,    0x41,    0x10,    0x42, 0x12,
        addr[0], addr[1], addr[2], 0x00, static_cast<uint8_t>((128 - (sum % 128)) & 0x7F),
        0xF7};
    REQUIRE(p.handle_sysex(msg, sizeof(msg)));
  };
  const auto sounded = [](Sf2Player& p) {
    p.on_event(0, event(sonare::midi::make_midi1_note_on(0, 0, 60, 127)));
    return estimate_frequency(render(p, 24000).left, 4800);
  };

  SECTION("RX BANK SELECT covers the MSB as well as the LSB") {
    auto open = make();
    cc(*open, 0, 8);
    REQUIRE(sounded(*open) == Approx(1000.0).margin(10.0));

    auto shut = make();
    switch_off(*shut, 0x23);
    cc(*shut, 0, 8);
    REQUIRE(sounded(*shut) == Approx(500.0).margin(5.0));
  }

  // The two switches are told apart from one starting state: the part already on
  // bank 3 through the GM2 melodic MSB, and the same message arriving after each
  // switch is closed. Refusing leaves the part where it was; zeroing does not.
  const auto on_bank_3 = [&](Sf2Player& p) {
    cc(p, 0, 0x79);  // GM2 melodic: the bank number is the LSB
    cc(p, 32, 3);
  };

  SECTION("the control: with both switches open the LSB is what picks the bank") {
    // Without this the two sections below both pass on a part that never left
    // bank 0, one of them for exactly the wrong reason.
    auto p = make();
    on_bank_3(*p);
    REQUIRE(sounded(*p) == Approx(250.0).margin(5.0));
  }

  SECTION("RX BANK SELECT refuses the message, so the part keeps its bank") {
    auto p = make();
    on_bank_3(*p);
    switch_off(*p, 0x23);
    cc(*p, 32, 0);
    REQUIRE(sounded(*p) == Approx(250.0).margin(5.0));
  }

  SECTION("RX BANK SELECT LSB reads the message as 00 instead") {
    auto p = make();
    on_bank_3(*p);
    switch_off(*p, 0x24);
    cc(*p, 32, 3);
    REQUIRE(sounded(*p) == Approx(500.0).margin(5.0));
  }

  SECTION("a GM1 System On closes the bank pair and NRPN, a GM2 System On closes NRPN alone") {
    const uint32_t bank_bits =
        gs_rx_switch_bit(GsRxSwitch::kBankSelect) | gs_rx_switch_bit(GsRxSwitch::kBankSelectLsb);
    const uint32_t nrpn = gs_rx_switch_bit(GsRxSwitch::kNrpn);
    const uint8_t gs_reset[] = {0xF0, 0x41, 0x10, 0x42, 0x12, 0x40, 0x00, 0x7F, 0x00, 0x41, 0xF7};
    const uint8_t gm1_on[] = {0xF0, 0x7E, 0x7F, 0x09, 0x01, 0xF7};
    const uint8_t gm2_on[] = {0xF0, 0x7E, 0x7F, 0x09, 0x03, 0xF7};
    const auto after = [&make](const uint8_t* msg, size_t size) {
      auto p = make();
      REQUIRE(p->handle_sysex(msg, size));
      return p->rx_switches(0);
    };
    // Measured on a unit rather than read off the map, which states the first
    // two and not the third (docs/gs.md).
    CHECK((after(gs_reset, sizeof(gs_reset)) & (bank_bits | nrpn)) == (bank_bits | nrpn));
    CHECK((after(gm1_on, sizeof(gm1_on)) & (bank_bits | nrpn)) == 0);
    CHECK((after(gm2_on, sizeof(gm2_on)) & (bank_bits | nrpn)) == bank_bits);
  }
}

TEST_CASE("PITCH OFFSET FINE shifts by hertz, not by an interval", "[midi][sf2][gslayer]") {
  // A pure sine rather than the file's filtered square: the claim under test is
  // a 12 Hz difference at half a kilohertz, which the square's harmonics blur
  // past. And tuned to concert pitch rather than to the file's convenient 500 Hz
  // — the offset is a shift of the NOTE's frequency, so a sample whose root key
  // sounds at some other pitch measures the parameter against the wrong number.
  const auto sine_fixture = []() {
    Sf2Builder b;
    std::vector<float> sine(128);
    for (size_t i = 0; i < sine.size(); ++i) {
      sine[i] = 0.6f * static_cast<float>(std::sin(kTwoPi * static_cast<double>(i) / 128.0));
    }
    // One cycle in 128 samples, so the rate is 128 x middle C: note 60 sounds
    // 261.625 Hz, which is note_to_hz(60) to within a thousandth of a hertz.
    const int sid = b.add_sample("sineC4", sine, 33488, 60, 0, 128);
    Sf2Builder::ZoneSpec zone;
    zone.gens.push_back({54 /*sampleModes*/, 1});
    zone.target = sid;
    Sf2Builder::ZoneSpec pz;
    pz.target = b.add_instrument("sineinst", {zone});
    b.add_preset("Sine", 0, 0, {pz});
    const auto bytes = b.build();
    auto sf2 = std::make_shared<Sf2File>();
    std::string error;
    REQUIRE(sf2->parse(bytes.data(), bytes.size(), &error));
    return std::shared_ptr<const Sf2File>(sf2);
  };
  const auto make = [&sine_fixture]() {
    Sf2PlayerConfig cfg;
    cfg.gain = 1.0f;
#if defined(SONARE_MIDI_WITH_FX)
    cfg.effects.enable_reverb = false;
    cfg.effects.enable_chorus = false;
    cfg.effects.enable_delay = false;
#endif
    auto player = std::make_unique<Sf2Player>(cfg);
    player->set_soundfont(sine_fixture());
    player->prepare(kOutRate, 256);
    return player;
  };
  // 40 11 17-18, part 1 = channel 0. The two nibbles go as one run, which is the
  // only way the corpus ever writes them and the only way the hardware takes a
  // nibblized parameter.
  const auto set_offset = [](Sf2Player& p, uint8_t combined) {
    const uint8_t hi = static_cast<uint8_t>(combined >> 4);
    const uint8_t lo = static_cast<uint8_t>(combined & 0x0Fu);
    const int sum = 0x40 + 0x11 + 0x17 + hi + lo;
    const uint8_t msg[] = {0xF0,
                           0x41,
                           0x10,
                           0x42,
                           0x12,
                           0x40,
                           0x11,
                           0x17,
                           hi,
                           lo,
                           static_cast<uint8_t>((128 - (sum % 128)) & 0x7F),
                           0xF7};
    REQUIRE(p.handle_sysex(msg, sizeof(msg)));
    CHECK(p.pitch_offset_fine(0) == combined);
  };
  const auto sounded = [](Sf2Player& p, uint8_t note) {
    p.on_event(0, event(sonare::midi::make_midi1_note_on(0, 0, note, 127)));
    return estimate_frequency(render(p, 24000).left, 4800);
  };

  SECTION("the centre 80 is the identity, on the byte and in the render") {
    auto player = make();
    CHECK(player->pitch_offset_fine(0) == 0x80);
    const double untouched = sounded(*player, 60);
    auto written = make();
    set_offset(*written, 0x80);
    CHECK(sounded(*written, 60) == Approx(untouched).margin(0.01));
  }

  SECTION("the offset is the same number of hertz at both octaves") {
    // F8 is the manual's +12.0 Hz, onto 261.625 and 523.25. An interval-shaped
    // parameter reading the same byte would put the upper octave at 547.25 Hz,
    // which is what the second margin excludes: it is narrower than the 12 Hz
    // between the two readings, so this cannot pass under both.
    auto low = make();
    set_offset(*low, 0xF8);
    CHECK(sounded(*low, 60) == Approx(273.6).margin(1.0));
    auto high = make();
    set_offset(*high, 0xF8);
    CHECK(sounded(*high, 72) == Approx(535.2).margin(2.0));
  }

  SECTION("08 is the same shift downwards") {
    auto player = make();
    set_offset(*player, 0x08);
    CHECK(sounded(*player, 60) == Approx(249.6).margin(1.0));
  }

  SECTION("the converter is finite at the bottom of the keyboard") {
    using sonare::midi::synth::gs_pitch_offset_fine_cents;
    // 08 is -12.0 Hz, which the lowest keys' own frequencies are under. The
    // bound is what keeps the ratio positive; without it note 7 and below take
    // the logarithm of a negative number.
    CHECK(gs_pitch_offset_fine_cents(0x80, 0) == 0.0f);
    // The bound's own fixed point: note 0 IS the lowest key, so a downward
    // offset leaves it where it is. Every key above it moves.
    CHECK(gs_pitch_offset_fine_cents(0x08, 0) == 0.0f);
    for (uint8_t note = 1; note < 24; ++note) {
      INFO("note " << static_cast<int>(note));
      CHECK(std::isfinite(gs_pitch_offset_fine_cents(0x08, note)));
      CHECK(gs_pitch_offset_fine_cents(0x08, note) < 0.0f);
    }
    // Note 16 is the first key the bound leaves alone: 20.6 Hz less 12 is above
    // the lowest key's 8.18, so the cents are the shift's own.
    CHECK(gs_pitch_offset_fine_cents(0x08, 16) ==
          Approx(1200.0 * std::log2(8.6017 / 20.6017)).margin(0.5));
  }

  SECTION("a word below the parameter's own range is bounded, not wrapped") {
    // The row bounds a nibble, so 00 00 decodes and is stored; the aggregate
    // range is the converter's, and it holds the word at 08.
    auto player = make();
    set_offset(*player, 0x00);
    CHECK(sounded(*player, 60) == Approx(249.6).margin(1.0));
  }
}

TEST_CASE("GS drum kit names", "[midi][sf2][gslayer]") {
  REQUIRE(gs_drum_kit_name(0) == "Standard");
  REQUIRE(gs_drum_kit_name(25) == "TR-808");
  REQUIRE(gs_drum_kit_name(56) == "SFX");
  REQUIRE(gs_drum_kit_name(3).empty());
}
