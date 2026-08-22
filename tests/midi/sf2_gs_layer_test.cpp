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
using sonare::midi::synth::apply_gs_efx_sysex;
using sonare::midi::synth::gs_drum_kit_name;
using sonare::midi::synth::gs_efx_insert_chain;
using sonare::midi::synth::gs_efx_insert_name;
using sonare::midi::synth::gs_efx_insert_params;
using sonare::midi::synth::GsEfx;
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

TEST_CASE("a GS drum note send maps like the same value on CC91/93", "[midi][sf2][gslayer]") {
  // A 0..127 send amount has to mean one depth whichever message carried it,
  // or a GS kit addressed through the drum block comes out drier than the
  // melodic parts addressed through the controllers.
  using sonare::midi::synth::apply_gs_drum_params;
  using sonare::midi::synth::GsDrumNoteParams;
  using sonare::midi::synth::kCcSendDepth;
  using sonare::midi::synth::Sf2VoiceParams;

  Sf2VoiceParams params;
  params.reverb_send = 0.0f;
  params.chorus_send = 0.0f;
  GsDrumNoteParams drum;
  drum.flags = GsDrumNoteParams::kReverb | GsDrumNoteParams::kChorus;
  drum.reverb = 80;
  drum.chorus = 80;
  apply_gs_drum_params(params, drum);

  const float cc_depth = kCcSendDepth * 80.0f / 127.0f;
  REQUIRE(params.reverb_send == Approx(cc_depth));
  REQUIRE(params.chorus_send == Approx(cc_depth));

  // Full scale saturates at unity rather than exceeding the send bus range.
  Sf2VoiceParams full;
  full.reverb_send = 0.9f;
  full.chorus_send = 0.9f;
  GsDrumNoteParams max_drum;
  max_drum.flags = GsDrumNoteParams::kReverb | GsDrumNoteParams::kChorus;
  max_drum.reverb = 127;
  max_drum.chorus = 127;
  apply_gs_drum_params(full, max_drum);
  REQUIRE(full.reverb_send <= 1.0f);
  REQUIRE(full.chorus_send <= 1.0f);
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

TEST_CASE("apply_gs_efx_sysex captures the EFX block as raw wire", "[midi][sf2][gslayer]") {
  // EFX TYPE write (40 03 00, two data bytes 01 10 = Overdrive). Checksum over
  // 40 03 00 01 10 = 84 -> 0x2C.
  const uint8_t type_write[] = {0xF0, 0x41, 0x10, 0x42, 0x12, 0x40,
                                0x03, 0x00, 0x01, 0x10, 0x2C, 0xF7};
  GsEfx efx;
  REQUIRE(apply_gs_efx_sysex(efx, type_write, sizeof(type_write)));
  REQUIRE(efx.type == 0x0110);
  REQUIRE(efx.assigned);
  // Unframed payload parses identically (framing is stripped).
  GsEfx efx_unframed;
  REQUIRE(apply_gs_efx_sysex(efx_unframed, type_write + 1, sizeof(type_write) - 2));
  REQUIRE(efx_unframed.type == 0x0110);

  // EFX PARAMETER 1 write (40 03 03, data 0x64 = 100). Checksum 0x56.
  const uint8_t param_write[] = {0xF0, 0x41, 0x10, 0x42, 0x12, 0x40, 0x03, 0x03, 0x64, 0x56, 0xF7};
  REQUIRE(apply_gs_efx_sysex(efx, param_write, sizeof(param_write)));
  REQUIRE(efx.params[0] == 100);
  REQUIRE(efx.type == 0x0110);  // the earlier type is preserved across writes

  // A full-block run from 0x00: type 01 10, reserved 00, params 1..3 = 10 20 30.
  // Checksum over 40 03 00 01 10 00 10 20 30 = 180 -> 0x4C.
  const uint8_t run[] = {0xF0, 0x41, 0x10, 0x42, 0x12, 0x40, 0x03, 0x00,
                         0x01, 0x10, 0x00, 0x10, 0x20, 0x30, 0x4C, 0xF7};
  GsEfx efx_run;
  REQUIRE(apply_gs_efx_sysex(efx_run, run, sizeof(run)));
  REQUIRE(efx_run.type == 0x0110);
  REQUIRE(efx_run.params[0] == 0x10);
  REQUIRE(efx_run.params[1] == 0x20);
  REQUIRE(efx_run.params[2] == 0x30);

  // A non-EFX Roland message (GS reset, address 40 00 7F) is not an EFX write.
  const uint8_t gs_reset[] = {0xF0, 0x41, 0x10, 0x42, 0x12, 0x40, 0x00, 0x7F, 0x00, 0x41, 0xF7};
  GsEfx untouched;
  REQUIRE_FALSE(apply_gs_efx_sysex(untouched, gs_reset, sizeof(gs_reset)));
  REQUIRE_FALSE(untouched.assigned);

  // A bad checksum is rejected and leaves the struct untouched.
  uint8_t corrupt[sizeof(type_write)];
  for (size_t i = 0; i < sizeof(type_write); ++i) corrupt[i] = type_write[i];
  corrupt[10] ^= 0x7F;  // wreck the checksum
  GsEfx efx_corrupt;
  REQUIRE_FALSE(apply_gs_efx_sysex(efx_corrupt, corrupt, sizeof(corrupt)));
  REQUIRE_FALSE(efx_corrupt.assigned);
  REQUIRE(apply_gs_efx_sysex(untouched, nullptr, 0) == false);
}

TEST_CASE("gs_efx_insert_name maps the adapted EFX types to inserts", "[midi][sf2][gslayer]") {
  // GS EFX type numbers (SC-88Pro MSB<<8|LSB) -> insert-factory names.
  REQUIRE(gs_efx_insert_name(0x0100) == "eq.parametric");                    // Stereo-EQ
  REQUIRE(gs_efx_insert_name(0x0101) == "eq.graphic");                       // Spectrum
  REQUIRE(gs_efx_insert_name(0x0102) == "spectral.presenceEnhancer");        // Enhancer
  REQUIRE(gs_efx_insert_name(0x0110) == "saturation.ampSim");                // Overdrive
  REQUIRE(gs_efx_insert_name(0x0111) == "saturation.ampSim");                // Distortion
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
  // Types with no faithful stock insert stay unmapped: 3D Auto (0x0170, a
  // binaural panner) and Tremolo (0x0125, an amplitude LFO). The SC-88Pro has
  // no standalone Ring Modulator type — that DSP is reachable only bundled in
  // Keyboard Multi (0x0500), realised there as a chain stage.
  REQUIRE(gs_efx_insert_name(0x0170).empty());
  REQUIRE(gs_efx_insert_name(0x0125).empty());
}

TEST_CASE("gs_efx_insert_params translates the drive per mapped type", "[midi][sf2][gslayer]") {
  GsEfx od;
  od.type = 0x0110;  // Overdrive -> amp model, drive rising with EFX PARAMETER 2
  od.params[1] = 0;
  const std::string low = gs_efx_insert_params(od);
  od.params[1] = 127;
  const std::string high = gs_efx_insert_params(od);
  REQUIRE(low.find("\"drive\"") != std::string::npos);
  REQUIRE(low.find("\"ampModel\":0") != std::string::npos);  // classic-crunch voicing
  REQUIRE(low != high);                                      // more drive -> more drive knob

  GsEfx dist;
  dist.type = 0x0111;  // Distortion -> amp model on its high-gain voicing
  dist.params[1] = 127;
  REQUIRE(gs_efx_insert_params(dist).find("\"ampModel\":2") != std::string::npos);

  // Output Level (EFX PARAMETER 20) -> levelDb: an untouched level is unset
  // (0 -> no levelDb, the insert's 0 dB default) and a below-unity level cuts.
  GsEfx lvl;
  lvl.type = 0x0110;
  lvl.params[1] = 100;
  lvl.params[19] = 0;
  REQUIRE(gs_efx_insert_params(lvl).find("levelDb") == std::string::npos);  // unset
  lvl.params[19] = 64;  // ~half of unity -> a negative levelDb
  const std::string cut = gs_efx_insert_params(lvl);
  REQUIRE(cut.find("\"levelDb\":-") != std::string::npos);
  lvl.params[19] = 127;  // unity -> 0 dB
  REQUIRE(gs_efx_insert_params(lvl).find("\"levelDb\":0") != std::string::npos);

  GsEfx thru;  // unmapped type -> the insert's defaults
  thru.type = 0x0114;
  REQUIRE(gs_efx_insert_params(thru) == "{}");
}

TEST_CASE("gs_efx_insert_params translates the pitch shifter coarse and balance",
          "[midi][sf2][gslayer]") {
  // Coarse Pitch (EFX PARAMETER 1 = params[0]) is a 64-centred semitone offset.
  GsEfx up;
  up.type = 0x0160;   // 2-voice Pitch Shifter
  up.params[0] = 76;  // 64 + 12 -> +12 semitones (one octave up)
  REQUIRE(gs_efx_insert_params(up).find("\"semitones\":12") != std::string::npos);
  up.params[0] = 52;  // 64 - 12 -> -12 semitones
  REQUIRE(gs_efx_insert_params(up).find("\"semitones\":-12") != std::string::npos);

  // An untouched block reads 0: unset -> 0 st, never a -64 -> clamped -24 st drop.
  GsEfx untouched;
  untouched.type = 0x0161;  // Feedback Pitch Shifter shares the translation
  REQUIRE(gs_efx_insert_params(untouched).find("\"semitones\":0") != std::string::npos);
  REQUIRE(gs_efx_insert_params(untouched).find("dryWet") == std::string::npos);  // balance unset

  // Effect Balance (PARAMETER 16 = params[15]) -> dry/wet when set.
  GsEfx mixed;
  mixed.type = 0x0160;
  mixed.params[0] = 71;    // +7 semitones
  mixed.params[15] = 127;  // full effect
  const std::string json = gs_efx_insert_params(mixed);
  REQUIRE(json.find("\"semitones\":7") != std::string::npos);
  REQUIRE(json.find("\"dryWet\":1") != std::string::npos);
}

TEST_CASE("gs_efx_insert_chain expands a composite type into its block chain",
          "[midi][sf2][gslayer]") {
  // A single-effect type yields a one-stage chain.
  GsEfx od;
  od.type = 0x0110;  // Overdrive
  const auto single = gs_efx_insert_chain(od);
  REQUIRE(single.size() == 1);
  REQUIRE(single[0].name == "saturation.ampSim");

  // GTR Multi 2 (04 01) yields its Cmp-OD-EQ-CF block chain, in signal order.
  GsEfx gtr;
  gtr.type = 0x0401;
  gtr.params[16] = 52;  // EQ Low Gain -12 dB (0x34, centre 64)
  gtr.params[17] = 76;  // EQ Hi Gain  +12 dB (0x4C)
  const auto chain = gs_efx_insert_chain(gtr);
  REQUIRE(chain.size() == 4);
  REQUIRE(chain[0].name == "dynamics.compressor");
  REQUIRE(chain[1].name == "saturation.ampSim");
  REQUIRE(chain[2].name == "eq.parametric");
  REQUIRE(chain[3].name == "effects.modulation.chorus");
  // The EQ block is the composite's true tone control: Low/Hi Gain -> shelves.
  REQUIRE(chain[2].params_json.find("\"band0.gainDb\":-12") != std::string::npos);
  REQUIRE(chain[2].params_json.find("\"band1.gainDb\":12") != std::string::npos);

  // An untouched EQ gain is unset -> flat (0 dB), never a -12 dB cut.
  GsEfx flat;
  flat.type = 0x0401;
  REQUIRE(gs_efx_insert_chain(flat)[2].params_json.find("\"band0.gainDb\":0") != std::string::npos);

  // A type with no faithful mapping yields an empty chain (bypass). The
  // parallel-2 composites (MSB 11) are intentionally left unmapped: they mix two
  // effects in parallel, which the series insert chain cannot express faithfully.
  GsEfx unknown;
  unknown.type = 0x1100;  // Cho/Delay (parallel) -> unmapped
  REQUIRE(gs_efx_insert_chain(unknown).empty());
}

TEST_CASE("gs_efx_insert_chain covers the guitar/bass multi block", "[midi][sf2][gslayer]") {
  auto names = [](uint16_t type) {
    GsEfx efx;
    efx.type = type;
    std::vector<std::string> out;
    for (const auto& stage : gs_efx_insert_chain(efx)) out.push_back(stage.name);
    return out;
  };
  // The SC-88Pro MSB-04 guitar/bass multi block, each in its manual signal order
  // (every block now has a matching insert: Wah / Auto-Wah are realised too).
  REQUIRE(names(0x0400) ==  // GTR Multi 1: Cmp-OD-CF-Dly
          std::vector<std::string>{"dynamics.compressor", "saturation.ampSim",
                                   "effects.modulation.chorus", "effects.delay.stereo"});
  REQUIRE(names(0x0402) ==  // GTR Multi 3: Wah-OD-CF-Dly
          std::vector<std::string>{"effects.modulation.wah", "saturation.ampSim",
                                   "effects.modulation.chorus", "effects.delay.stereo"});
  REQUIRE(names(0x0403) ==  // Clean GTR Multi 1: Cmp-EQ-CF-Dly (no OD)
          std::vector<std::string>{"dynamics.compressor", "eq.parametric",
                                   "effects.modulation.chorus", "effects.delay.stereo"});
  REQUIRE(names(0x0404) ==  // Clean GTR Multi 2: AW-EQ-CF-Dly
          std::vector<std::string>{"effects.modulation.autoWah", "eq.parametric",
                                   "effects.modulation.chorus", "effects.delay.stereo"});
  REQUIRE(names(0x0405) ==  // Bass Multi: Cmp-OD-EQ-CF
          std::vector<std::string>{"dynamics.compressor", "saturation.ampSim", "eq.parametric",
                                   "effects.modulation.chorus"});

  // The bass multi puts its OD block on the bass cab; the guitar ones do not.
  GsEfx bass;
  bass.type = 0x0405;
  const auto bass_chain = gs_efx_insert_chain(bass);
  REQUIRE(bass_chain[1].params_json.find("\"cabModel\":1") != std::string::npos);
  GsEfx guitar;
  guitar.type = 0x0401;
  REQUIRE(gs_efx_insert_chain(guitar)[1].params_json.find("cabModel") == std::string::npos);
}

TEST_CASE("gs_efx_insert_chain expands the series-2 and multi composites", "[midi][sf2][gslayer]") {
  auto names = [](uint16_t type) {
    GsEfx efx;
    efx.type = type;
    std::vector<std::string> out;
    for (const auto& stage : gs_efx_insert_chain(efx)) out.push_back(stage.name);
    return out;
  };
  // Series-2 composites (SC-88Pro MSB 02): two stock effects in signal order.
  REQUIRE(names(0x0200) ==  // OD -> Chorus
          std::vector<std::string>{"saturation.ampSim", "effects.modulation.chorus"});
  REQUIRE(names(0x0202) ==  // OD -> Delay
          std::vector<std::string>{"saturation.ampSim", "effects.delay.stereo"});
  REQUIRE(names(0x0206) ==  // EH -> Chorus
          std::vector<std::string>{"spectral.presenceEnhancer", "effects.modulation.chorus"});
  REQUIRE(names(0x0209) ==  // Cho -> Delay
          std::vector<std::string>{"effects.modulation.chorus", "effects.delay.stereo"});
  REQUIRE(names(0x020B) ==  // Cho -> Flanger
          std::vector<std::string>{"effects.modulation.chorus", "effects.modulation.flanger"});
  // The distortion series-2 blocks use the high-gain amp voicing.
  GsEfx ds;
  ds.type = 0x0204;  // DS -> Flanger
  const auto ds_chain = gs_efx_insert_chain(ds);
  REQUIRE(ds_chain[0].name == "saturation.ampSim");
  REQUIRE(ds_chain[0].params_json.find("\"ampModel\":2") != std::string::npos);
  REQUIRE(ds_chain[1].name == "effects.modulation.flanger");

  // Rotary Multi: OD -> 3-band EQ -> Rotary, reachable via both type numbers the
  // manual prints for it (chapter-4 body 03 00 and appendix table 02 0C).
  const std::vector<std::string> rotary_multi{"saturation.ampSim", "eq.parametric",
                                              "effects.modulation.rotary"};
  REQUIRE(names(0x0300) == rotary_multi);
  REQUIRE(names(0x020C) == rotary_multi);

  // Rhodes Multi (04 06): Enhancer -> Phaser -> Chorus -> Tremolo/Pan.
  REQUIRE(names(0x0406) == std::vector<std::string>{"spectral.presenceEnhancer",
                                                    "effects.modulation.phaser",
                                                    "effects.modulation.chorus", "stereo.autoPan"});

  // Keyboard Multi (05 00): Ring Mod -> EQ -> Pitch Shifter -> Phaser -> Delay.
  // This is the only GS type that binds the ring-modulator insert.
  REQUIRE(names(0x0500) ==
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
