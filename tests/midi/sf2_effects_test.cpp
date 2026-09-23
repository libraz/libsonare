/// @file sf2_effects_test.cpp
/// @brief GS effect bus (build-plan P4): reverb/chorus/delay send-returns
///        behind CC91/93/94 and the SF2 send generators — send monotonicity,
///        wet tails after the dry signal ends, effect tails inside
///        tail_samples(), per-part insert drive, deterministic effects and a
///        no-alloc audio path with effects engaged.

#include <algorithm>
#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <cstdint>
#include <limits>
#include <memory>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "mastering/api/insert_factory.h"
#include "midi/midi_event.h"
#include "midi/synth/sf2_file.h"
#include "midi/synth/sf2_player.h"
#include "midi/ump.h"
#include "rt/processor_base.h"
#include "support/alloc_guard.h"
#include "support/midi_render.h"
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

/// "No wet return here" floor. The mix bus carries a DC blocker, and a
/// first-order highpass answers the dry burst with an exponentially decaying
/// residual instead of snapping to zero, so an empty measurement window reads
/// as a level below audibility rather than a bit-exact zero. The wet returns
/// these cases look for are orders of magnitude above it.
constexpr float kSilenceFloor = 1.0e-6f;

using sonare::test::event;

/// A framed Roland DT1 write of @p data at @p addr, with its checksum.
std::vector<uint8_t> dt1(uint32_t addr, std::vector<uint8_t> data) {
  const uint8_t a0 = static_cast<uint8_t>((addr >> 16) & 0x7Fu);
  const uint8_t a1 = static_cast<uint8_t>((addr >> 8) & 0x7Fu);
  const uint8_t a2 = static_cast<uint8_t>(addr & 0x7Fu);
  std::vector<uint8_t> msg{0xF0, 0x41, 0x10, 0x42, 0x12, a0, a1, a2};
  msg.insert(msg.end(), data.begin(), data.end());
  int sum = a0 + a1 + a2;
  for (const uint8_t b : data) sum += b;
  msg.push_back(static_cast<uint8_t>((128 - (sum % 128)) & 0x7F));
  msg.push_back(0xF7);
  return msg;
}

/// The part-block nibble of @p channel: block 0 is the rhythm part (channel 10),
/// blocks 1-9 are channels 1-9, blocks A-F are channels 11-16.
uint32_t part_block(uint8_t channel) {
  if (channel == 9) return 0;
  return channel < 9 ? static_cast<uint32_t>(channel) + 1u : channel;
}

/// GS 40 4x 22 PART EFX ASSIGN: 00 bypass, 01 unit 0, 02-10 units 1-15.
std::vector<uint8_t> efx_assign(uint8_t channel, uint8_t value) {
  return dt1(0x404022u | (part_block(channel) << 8), {value});
}

/// The EFX parameter block of @p unit. Unit 0 is the spec block at 40 03 xx;
/// the extension gives every unit the same layout at 40 3u xx (docs/gs.md).
uint32_t efx_block(uint8_t unit) {
  return unit == 0 ? 0x400300u : (0x403000u | (static_cast<uint32_t>(unit) << 8));
}

/// Energy at @p hz, over the settled part of the render.
double tone_at(const std::vector<float>& buf, double hz) {
  const double w = kTwoPi * hz / kOutRate;
  const double coeff = 2.0 * std::cos(w);
  double s1 = 0.0, s2 = 0.0;
  for (size_t i = 2400; i < buf.size(); ++i) {
    const double s0 = static_cast<double>(buf[i]) + coeff * s1 - s2;
    s2 = s1;
    s1 = s0;
  }
  return s1 * s1 + s2 * s2 - coeff * s1 * s2;
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

float peak(const std::vector<float>& buf, size_t from, size_t to) {
  float p = 0.0f;
  to = std::min(to, buf.size());
  for (size_t i = from; i < to; ++i) p = std::max(p, std::fabs(buf[i]));
  return p;
}

// The CC91/93/94 sends below drive the GS effect bus (GsEffectsConfig), which
// only exists on a build with the FX suite; without it the bus is entirely
// absent and these sends are no-ops.
#if defined(SONARE_MIDI_WITH_FX)

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

#endif  // SONARE_MIDI_WITH_FX

}  // namespace

#if defined(SONARE_MIDI_WITH_FX)

TEST_CASE("GS reverb send is monotonic in CC91", "[midi][sf2][gsfx]") {
  const float dry = reverb_tail_rms(0);
  const float mid = reverb_tail_rms(64);
  const float full = reverb_tail_rms(127);
  REQUIRE(dry < kSilenceFloor);  // no send -> no wet tail
  REQUIRE(mid > 0.0f);
  REQUIRE(full > mid * 1.5f);
}

TEST_CASE("the fallback ambience weighting scales the channel send", "[midi][sf2][gsfx]") {
  // No SoundFont, so every note takes the synth-fallback floor where
  // gm_fallback_sends applies. The weighting is multiplicative on the CC, so
  // CC91 = 0 is dry whatever the program asks for, and a cathedral program is
  // wetter than a close-miked one at the same controller value.
  auto wet_tail = [](uint8_t program, uint8_t cc91) {
    Sf2PlayerConfig cfg;
    cfg.gain = 1.0f;
    Sf2Player player(cfg);  // no set_soundfont -> fallback floor
    player.prepare(kOutRate, 256);
    player.on_event(0, event(sonare::midi::make_midi1_control_change(0, 0, 91, cc91)));
    // GS power-on leaves a chorus send standing; clear it so the measured tail
    // is the reverb return alone.
    player.on_event(0, event(sonare::midi::make_midi1_control_change(0, 0, 93, 0)));
    player.on_event(0, event(sonare::midi::make_midi1_control_change(0, 0, 94, 0)));
    player.on_event(0, event(sonare::midi::make_midi1_program_change(0, 0, program)));
    player.on_event(0, event(sonare::midi::make_midi1_note_on(0, 0, 60, 100)));
    render(player, 4800);
    player.on_event(0, event(sonare::midi::make_midi1_note_off(0, 0, 60, 0)));
    // Kill the voices, leaving only what the effect bus is still returning.
    player.on_event(0, event(sonare::midi::make_midi1_control_change(0, 0, 120, 0)));
    const StereoRender out = render(player, 24000);
    return rms(out.left, 240, 24000) + rms(out.right, 240, 24000);
  };

  REQUIRE(wet_tail(19, 0) < kSilenceFloor);  // church organ, no send -> dry
  REQUIRE(wet_tail(33, 0) < kSilenceFloor);  // electric bass, no send -> dry
  const float organ = wet_tail(19, 64);
  const float bass = wet_tail(33, 64);
  REQUIRE(organ > 0.0f);
  REQUIRE(bass > 0.0f);
  REQUIRE(organ > bass);
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
    // Kill the default room so the echo window holds only the delay return.
    player.on_event(0, event(sonare::midi::make_midi1_control_change(0, 0, 91, 0)));
    player.on_event(0, event(sonare::midi::make_midi1_control_change(0, 0, 94, cc94)));
    player.on_event(0, event(sonare::midi::make_midi1_note_on(0, 0, 60, 127)));
    const StereoRender out = render(player, 20000);
    // Default delay time 340 ms = 16320 samples; the dry burst is ~256.
    return peak(out.left, 15800, 17500);
  };
  REQUIRE(echo_peak(0) < kSilenceFloor);
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

#endif  // SONARE_MIDI_WITH_FX

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

// The kProcessor insert slot routes through mastering::api::make_insert, which
// only exists on a build with the mastering library.
#if defined(SONARE_WITH_MASTERING)

TEST_CASE("per-part processor insert runs an injected factory-built effect", "[midi][sf2][gsfx]") {
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

  // A kProcessor slot builds a real insert (a driven tube) through the injected
  // factory and runs it on the part bus: the 1 kHz sine gains odd harmonics.
  Sf2PlayerConfig cfg;
  cfg.gain = 1.0f;
  cfg.part_inserts[0].type = Sf2InsertType::kProcessor;
  cfg.part_inserts[0].stages = {{"saturation.tube", R"({"driveDb":30})"}};
  cfg.insert_factory = [](std::string_view name, std::string_view json) {
    return sonare::mastering::api::make_insert(std::string(name), std::string(json));
  };
  Sf2Player driven = make_player(cfg);
  driven.on_event(0, event(sonare::midi::make_midi1_note_on(0, 0, 60, 127)));
  const StereoRender drv = render(driven, 9600);

  Sf2Player clean = make_player();
  clean.on_event(0, event(sonare::midi::make_midi1_note_on(0, 0, 60, 127)));
  const StereoRender cln = render(clean, 9600);
  REQUIRE(h3(drv.left) > 10.0 * h3(cln.left));

  // Without a factory (or an unknown name) the kProcessor slot is an inert
  // no-op: the part stays clean, no crash.
  Sf2PlayerConfig no_factory = cfg;
  no_factory.insert_factory = nullptr;
  Sf2Player inert = make_player(no_factory);
  inert.on_event(0, event(sonare::midi::make_midi1_note_on(0, 0, 60, 127)));
  const StereoRender ino = render(inert, 9600);
  REQUIRE(h3(ino.left) < 10.0 * h3(cln.left));
}

TEST_CASE("a part carries its own insert and the file's EFX in series", "[midi][sf2][gsfx]") {
  // docs/gs.md: the slot is a chain, not a choice. A part given an insert by the
  // host still receives the file's insertion effect, so a guitar with an
  // amplifier does not lose the file's chorus.
  auto factory = [](std::string_view name, std::string_view json) {
    return sonare::mastering::api::make_insert(std::string(name), std::string(json));
  };
  // Overdrive as the file's EFX, switched on for the part on channel 0.
  const std::vector<uint8_t> efx_type = dt1(efx_block(0), {0x01, 0x10});
  const std::vector<uint8_t> efx_on = efx_assign(0, 0x01);

  auto render_case = [&](bool insert, bool efx) {
    Sf2PlayerConfig cfg;
    cfg.gain = 1.0f;
    cfg.realize_efx_inline = true;
    cfg.insert_factory = factory;
    if (insert) {
      cfg.part_inserts[0].type = Sf2InsertType::kProcessor;
      cfg.part_inserts[0].stages = {{"saturation.tube", R"({"driveDb":30})"}};
    }
    Sf2Player player = make_player(cfg);
    if (efx) {
      player.handle_sysex(efx_type.data(), efx_type.size());
      player.handle_sysex(efx_on.data(), efx_on.size());
    }
    player.on_event(0, event(sonare::midi::make_midi1_note_on(0, 0, 60, 127)));
    return render(player, 9600);
  };

  auto distance = [](const StereoRender& a, const StereoRender& b) {
    double acc = 0.0;
    for (size_t i = 2400; i < a.left.size(); ++i) {
      const double d = static_cast<double>(a.left[i]) - static_cast<double>(b.left[i]);
      acc += d * d;
    }
    return std::sqrt(acc / static_cast<double>(a.left.size() - 2400));
  };

  const StereoRender clean = render_case(false, false);
  const StereoRender insert_only = render_case(true, false);
  const StereoRender efx_only = render_case(false, true);
  const StereoRender both = render_case(true, true);

  // Positive controls: each stage moves the render on its own, so neither
  // comparison below can pass on a stage that was never built.
  REQUIRE(distance(insert_only, clean) > 1e-3);
  REQUIRE(distance(efx_only, clean) > 1e-3);

  // In series: the EFX reaches a part that already carries an insert, and the
  // insert survives the EFX arriving.
  REQUIRE(distance(both, insert_only) > 1e-3);
  REQUIRE(distance(both, efx_only) > 1e-3);
}

TEST_CASE("parts assigned to one EFX unit sum into its single instance", "[midi][sf2][gsfx]") {
  // docs/gs.md: parts summing into the unit they share is not a resource limit,
  // it is what an effect is — two guitars into one distortion intermodulate.
  // Two separate instances cannot produce a difference tone, so that is what
  // separates the two arrangements: a 1 kHz part and a 1498 Hz part through one
  // overdrive make 498 Hz, which is a harmonic of neither.
  auto factory = [](std::string_view name, std::string_view json) {
    return sonare::mastering::api::make_insert(std::string(name), std::string(json));
  };
  const std::vector<uint8_t> overdrive = dt1(efx_block(0), {0x01, 0x10});

  // Both parts on unit 0, against each part alone through the same unit. The
  // two solo renders carry the intermodulation neither part can make by itself,
  // which is the control: the difference tone must not already be there.
  auto render_parts = [&](bool part_a, bool part_b) {
    Sf2PlayerConfig cfg;
    cfg.gain = 1.0f;
    cfg.realize_efx_inline = true;
    cfg.insert_factory = factory;
    Sf2Player player = make_player(cfg);
    player.handle_sysex(overdrive.data(), overdrive.size());
    const std::vector<uint8_t> on_a = efx_assign(0, 0x01);
    const std::vector<uint8_t> on_b = efx_assign(1, 0x01);
    player.handle_sysex(on_a.data(), on_a.size());
    player.handle_sysex(on_b.data(), on_b.size());
    if (part_a) player.on_event(0, event(sonare::midi::make_midi1_note_on(0, 0, 60, 127)));
    if (part_b) player.on_event(0, event(sonare::midi::make_midi1_note_on(0, 1, 67, 127)));
    return render(player, 9600);
  };

  const StereoRender a_only = render_parts(true, false);
  const StereoRender b_only = render_parts(false, true);
  const StereoRender both = render_parts(true, true);

  // 1000 Hz and a fifth above it (1498 Hz): the difference is 498 Hz.
  const double diff_solo = std::max(tone_at(a_only.left, 498.0), tone_at(b_only.left, 498.0));
  const double diff_both = tone_at(both.left, 498.0);
  // Each part is present on its own, so the render being compared is not empty.
  REQUIRE(tone_at(a_only.left, 1000.0) > 1e3 * tone_at(a_only.left, 498.0));
  REQUIRE(tone_at(b_only.left, 1498.0) > 1e3 * tone_at(b_only.left, 498.0));
  // One instance, so the two parts intermodulate in it.
  REQUIRE(diff_both > 100.0 * diff_solo);
}

TEST_CASE("each part reaches the EFX unit it was assigned", "[midi][sf2][gsfx]") {
  // The extension gives every unit the same 00-1F layout at 40 3u xx, and
  // 40 4x 22 selects which one a part runs through (docs/gs.md). Two parts on
  // two units carrying different types therefore render differently, which one
  // shared unit cannot do however it is configured.
  auto factory = [](std::string_view name, std::string_view json) {
    return sonare::mastering::api::make_insert(std::string(name), std::string(json));
  };
  auto render_case = [&](uint8_t unit_for_part_b, uint16_t type_for_that_unit) {
    Sf2PlayerConfig cfg;
    cfg.gain = 1.0f;
    cfg.realize_efx_inline = true;
    cfg.insert_factory = factory;
    Sf2Player player = make_player(cfg);
    // Unit 0 is an overdrive; the second unit is whatever the case names.
    const std::vector<uint8_t> u0 = dt1(efx_block(0), {0x01, 0x10});
    player.handle_sysex(u0.data(), u0.size());
    const std::vector<uint8_t> un =
        dt1(efx_block(unit_for_part_b), {static_cast<uint8_t>(type_for_that_unit >> 8),
                                         static_cast<uint8_t>(type_for_that_unit & 0x7Fu)});
    if (unit_for_part_b != 0) player.handle_sysex(un.data(), un.size());
    const std::vector<uint8_t> on_a = efx_assign(0, 0x01);
    const std::vector<uint8_t> on_b =
        efx_assign(1, static_cast<uint8_t>(unit_for_part_b == 0 ? 0x01 : unit_for_part_b + 1));
    player.handle_sysex(on_a.data(), on_a.size());
    player.handle_sysex(on_b.data(), on_b.size());
    player.on_event(0, event(sonare::midi::make_midi1_note_on(0, 1, 60, 127)));
    return render(player, 9600);
  };

  // Part on channel 1 through unit 0 (overdrive) against the same part through
  // unit 1 carrying a compressor: the harmonics the overdrive makes are the tell.
  const StereoRender through_unit0 = render_case(0, 0x0110);
  const StereoRender through_unit1 = render_case(1, 0x0130);
  REQUIRE(tone_at(through_unit0.left, 3000.0) > 10.0 * tone_at(through_unit1.left, 3000.0));

  // 40 30 xx is unit 0's own block under the extension's uniform layout, so a
  // write there reaches the same unit 40 03 xx does.
  Sf2PlayerConfig cfg;
  cfg.gain = 1.0f;
  cfg.realize_efx_inline = true;
  cfg.insert_factory = factory;
  Sf2Player aliased = make_player(cfg);
  const std::vector<uint8_t> u0_alias = dt1(0x403000u, {0x01, 0x10});
  const std::vector<uint8_t> on = efx_assign(1, 0x01);
  aliased.handle_sysex(u0_alias.data(), u0_alias.size());
  aliased.handle_sysex(on.data(), on.size());
  aliased.on_event(0, event(sonare::midi::make_midi1_note_on(0, 1, 60, 127)));
  const StereoRender via_alias = render(aliased, 9600);
  REQUIRE(tone_at(via_alias.left, 3000.0) > 10.0 * tone_at(through_unit1.left, 3000.0));
}

TEST_CASE("a non-finite insert sample never reaches the mix-bus state", "[midi][sf2][gsfx]") {
  // A part insert is a host-injected processor: whatever it writes lands on the
  // shared mix bus, where the DC blocker holds it in an IIR state forever. One
  // NaN there makes every remaining sample of the render NaN too, so the bus is
  // scrubbed before the blocker sees it.
  struct NanInsert : sonare::rt::ProcessorBase {
    void prepare(double, int) override {}
    void reset() override {}
    void process(float* const* channels, int num_channels, int num_samples) override {
      if (fired) return;
      fired = true;
      for (int ch = 0; ch < num_channels; ++ch) {
        if (channels[ch] != nullptr && num_samples > 0) {
          channels[ch][0] = std::numeric_limits<float>::quiet_NaN();
        }
      }
    }
    bool fired = false;
  };

  Sf2PlayerConfig cfg;
  cfg.gain = 1.0f;
  cfg.part_inserts[0].type = Sf2InsertType::kProcessor;
  cfg.part_inserts[0].stages = {{"test.nan", "{}"}};
  cfg.insert_factory = [](std::string_view, std::string_view) {
    return std::unique_ptr<sonare::rt::ProcessorBase>(new NanInsert());
  };
  Sf2Player player = make_player(cfg);
  player.on_event(0, event(sonare::midi::make_midi1_note_on(0, 0, 60, 127)));
  const StereoRender out = render(player, 9600);
  for (size_t i = 0; i < out.left.size(); ++i) {
    INFO("sample " << i);
    REQUIRE(std::isfinite(out.left[i]));
    REQUIRE(std::isfinite(out.right[i]));
  }
  // The bus recovered rather than being clamped to silence for the rest of the
  // render: the note after the poisoned sample still sounds.
  REQUIRE(peak(out.left, 480, 9600) > 0.01f);
}

TEST_CASE("GS EFX SysEx routes a part through a realised insert", "[midi][sf2][gsfx]") {
  auto make_factory = [] {
    return [](std::string_view name, std::string_view json) {
      return sonare::mastering::api::make_insert(std::string(name), std::string(json));
    };
  };
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
  // GS sequence: enable EFX on part 1 (channel 0), select Overdrive (01 10),
  // set EFX PARAMETER 1 (the drive, 40 03 03) to max. Checksums per the Roland
  // DT1 rule.
  const uint8_t part_on[] = {0xF0, 0x41, 0x10, 0x42, 0x12, 0x40, 0x41, 0x22, 0x01, 0x5C, 0xF7};
  const uint8_t od_type[] = {0xF0, 0x41, 0x10, 0x42, 0x12, 0x40,
                             0x03, 0x00, 0x01, 0x10, 0x2C, 0xF7};
  const uint8_t od_drive[] = {0xF0, 0x41, 0x10, 0x42, 0x12, 0x40, 0x03, 0x03, 0x7F, 0x3B, 0xF7};

  Sf2PlayerConfig cfg;
  cfg.gain = 1.0f;
  cfg.insert_factory = make_factory();
  Sf2Player player = make_player(cfg);
  // Live control-thread path: parse + realise + publish the inserts wait-free.
  player.on_control_sysex(part_on, sizeof(part_on));
  player.on_control_sysex(od_type, sizeof(od_type));
  player.on_control_sysex(od_drive, sizeof(od_drive));

  player.on_event(0, event(sonare::midi::make_midi1_note_on(0, 0, 60, 127)));
  const StereoRender efx = render(player, 9600);

  Sf2Player clean = make_player();
  clean.on_event(0, event(sonare::midi::make_midi1_note_on(0, 0, 60, 127)));
  const StereoRender cln = render(clean, 9600);
  REQUIRE(h3(efx.left) > 10.0 * h3(cln.left));

  // Turning the part EFX off tears the insert down (fresh player, no ring-over).
  const uint8_t part_off[] = {0xF0, 0x41, 0x10, 0x42, 0x12, 0x40, 0x41, 0x22, 0x00, 0x5D, 0xF7};
  Sf2PlayerConfig cfg_off;
  cfg_off.gain = 1.0f;
  cfg_off.insert_factory = make_factory();
  Sf2Player off_player = make_player(cfg_off);
  off_player.on_control_sysex(part_on, sizeof(part_on));
  off_player.on_control_sysex(od_type, sizeof(od_type));
  off_player.on_control_sysex(od_drive, sizeof(od_drive));
  off_player.on_control_sysex(part_off, sizeof(part_off));
  off_player.on_event(0, event(sonare::midi::make_midi1_note_on(0, 0, 60, 127)));
  const StereoRender after = render(off_player, 9600);
  REQUIRE(h3(after.left) < 10.0 * h3(cln.left) + 1e-9);
}

TEST_CASE("a composite GS EFX type realises a multi-stage chain", "[midi][sf2][gsfx]") {
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
  // Enable EFX on part 1, select GTR Multi 2 (04 01) = Cmp-OD-EQ-CF. The amp,
  // compressor and EQ stages are always built; the chorus stage is skipped on a
  // no-FX build, so the chain still colours the part either way.
  const uint8_t part_on[] = {0xF0, 0x41, 0x10, 0x42, 0x12, 0x40, 0x41, 0x22, 0x01, 0x5C, 0xF7};
  const uint8_t gtr_type[] = {0xF0, 0x41, 0x10, 0x42, 0x12, 0x40,
                              0x03, 0x00, 0x04, 0x01, 0x38, 0xF7};

  Sf2PlayerConfig cfg;
  cfg.gain = 1.0f;
  cfg.insert_factory = [](std::string_view name, std::string_view json) {
    return sonare::mastering::api::make_insert(std::string(name), std::string(json));
  };
  Sf2Player player = make_player(cfg);
  player.on_control_sysex(part_on, sizeof(part_on));
  player.on_control_sysex(gtr_type, sizeof(gtr_type));
  player.on_event(0, event(sonare::midi::make_midi1_note_on(0, 0, 60, 127)));
  const StereoRender efx = render(player, 9600);

  Sf2Player clean = make_player();
  clean.on_event(0, event(sonare::midi::make_midi1_note_on(0, 0, 60, 127)));
  const StereoRender cln = render(clean, 9600);
  // The amp stage inside the composite drives the 1 kHz sine into harmonics.
  REQUIRE(h3(efx.left) > 10.0 * h3(cln.left));
}

TEST_CASE("a mapped GS EFX modulation type is realised through the factory", "[midi][sf2][gsfx]") {
  // Skip on builds without the FX suite (effects.* return null there).
  if (sonare::mastering::api::make_insert("effects.modulation.chorus", "{}") == nullptr) return;

  // Enable EFX on part 1, select Stereo Chorus (01 42).
  const uint8_t part_on[] = {0xF0, 0x41, 0x10, 0x42, 0x12, 0x40, 0x41, 0x22, 0x01, 0x5C, 0xF7};
  const uint8_t chorus[] = {0xF0, 0x41, 0x10, 0x42, 0x12, 0x40, 0x03, 0x00, 0x01, 0x42, 0x7A, 0xF7};

  Sf2PlayerConfig cfg;
  cfg.gain = 1.0f;
  cfg.insert_factory = [](std::string_view name, std::string_view json) {
    return sonare::mastering::api::make_insert(std::string(name), std::string(json));
  };
  Sf2Player player = make_player(cfg);
  player.on_control_sysex(part_on, sizeof(part_on));
  player.on_control_sysex(chorus, sizeof(chorus));
  player.on_event(0, event(sonare::midi::make_midi1_note_on(0, 0, 60, 127)));
  const StereoRender efx = render(player, 9600);

  Sf2Player clean = make_player();
  clean.on_event(0, event(sonare::midi::make_midi1_note_on(0, 0, 60, 127)));
  const StereoRender cln = render(clean, 9600);
  // The chorus modulates the part, so its output diverges from the dry signal.
  REQUIRE(efx.left != cln.left);
}

TEST_CASE("an EFX-capable player with no active insert renders bit-identically",
          "[midi][sf2][gsfx]") {
  // Injecting an insert factory must not perturb an otherwise dry render: a part
  // is only summed through its insert bus once an EFX (or a static insert) is
  // live, so the dry mix's summation order is unchanged. This guards the
  // offline bounce, which always injects a factory.
  Sf2PlayerConfig cfg;
  cfg.gain = 1.0f;
  cfg.insert_factory = [](std::string_view name, std::string_view json) {
    return sonare::mastering::api::make_insert(std::string(name), std::string(json));
  };
  Sf2Player capable = make_player(cfg);
  capable.on_event(0, event(sonare::midi::make_midi1_note_on(0, 0, 60, 100)));
  const StereoRender with_factory = render(capable, 9600);

  Sf2PlayerConfig plain_cfg;  // no factory injected, same gain as the capable one
  plain_cfg.gain = 1.0f;
  Sf2Player plain = make_player(plain_cfg);
  plain.on_event(0, event(sonare::midi::make_midi1_note_on(0, 0, 60, 100)));
  const StereoRender without = render(plain, 9600);

  REQUIRE(with_factory.left == without.left);
  REQUIRE(with_factory.right == without.right);
}

TEST_CASE("the offline realize_efx_inline path installs EFX without a manual pump",
          "[midi][sf2][gsfx]") {
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
  // Same OD-on-part-1 sequence as the manual-pump test, but the offline host
  // never calls realize_gs_efx(): process() must realise the pending change
  // inline (this is how the single-threaded bounce applies a mid-render EFX).
  const uint8_t part_on[] = {0xF0, 0x41, 0x10, 0x42, 0x12, 0x40, 0x41, 0x22, 0x01, 0x5C, 0xF7};
  const uint8_t od_type[] = {0xF0, 0x41, 0x10, 0x42, 0x12, 0x40,
                             0x03, 0x00, 0x01, 0x10, 0x2C, 0xF7};
  const uint8_t od_drive[] = {0xF0, 0x41, 0x10, 0x42, 0x12, 0x40, 0x03, 0x03, 0x7F, 0x3B, 0xF7};

  Sf2PlayerConfig cfg;
  cfg.gain = 1.0f;
  cfg.realize_efx_inline = true;
  cfg.insert_factory = [](std::string_view name, std::string_view json) {
    return sonare::mastering::api::make_insert(std::string(name), std::string(json));
  };
  Sf2Player player = make_player(cfg);
  REQUIRE(player.handle_sysex(part_on, sizeof(part_on)));
  REQUIRE(player.handle_sysex(od_type, sizeof(od_type)));
  REQUIRE(player.handle_sysex(od_drive, sizeof(od_drive)));
  REQUIRE(player.gs_efx_dirty());  // not realised yet — no manual pump
  player.on_event(0, event(sonare::midi::make_midi1_note_on(0, 0, 60, 127)));
  const StereoRender efx = render(player, 9600);
  REQUIRE_FALSE(player.gs_efx_dirty());  // process() realised it inline

  Sf2Player clean = make_player();
  clean.on_event(0, event(sonare::midi::make_midi1_note_on(0, 0, 60, 127)));
  const StereoRender cln = render(clean, 9600);
  REQUIRE(h3(efx.left) > 10.0 * h3(cln.left));
}

TEST_CASE("a GS reset after the EFX frames clears the chain, not only one at tick 0",
          "[midi][sf2][gsfx]") {
  const uint8_t part_on[] = {0xF0, 0x41, 0x10, 0x42, 0x12, 0x40, 0x41, 0x22, 0x01, 0x5C, 0xF7};
  const uint8_t od_type[] = {0xF0, 0x41, 0x10, 0x42, 0x12, 0x40,
                             0x03, 0x00, 0x01, 0x10, 0x2C, 0xF7};
  const uint8_t gs_reset[] = {0xF0, 0x41, 0x10, 0x42, 0x12, 0x40, 0x00, 0x7F, 0x00, 0x41, 0xF7};

  auto efx_config = [] {
    Sf2PlayerConfig cfg;
    cfg.gain = 1.0f;
    cfg.realize_efx_inline = true;
    cfg.insert_factory = [](std::string_view name, std::string_view json) {
      return sonare::mastering::api::make_insert(std::string(name), std::string(json));
    };
    return cfg;
  };

  // Overdrive returns far below dry, so a peak separates "the chain is still
  // installed" from "it is gone" without a spectral measure. The probe note
  // starts AFTER the reset: asking what the already-sounding note does cannot
  // answer this, for the reason the second section below measures.
  const auto probe_note_peak = [&](bool with_efx, bool reset_midstream) {
    Sf2Player player = make_player(efx_config());
    if (with_efx) {
      REQUIRE(player.handle_sysex(part_on, sizeof(part_on)));
      REQUIRE(player.handle_sysex(od_type, sizeof(od_type)));
    }
    player.on_event(0, event(sonare::midi::make_midi1_note_on(0, 0, 60, 127)));
    static_cast<void>(render(player, 24000));  // the chain is realised in here
    if (reset_midstream) {
      REQUIRE(player.handle_sysex(gs_reset, sizeof(gs_reset)));
      REQUIRE_FALSE(player.gs_efx().assigned);
    }
    player.on_event(0, event(sonare::midi::make_midi1_note_on(0, 0, 64, 127)));
    const StereoRender out = render(player, 24000);
    return peak(out.left, 0, out.left.size());
  };

  const float dry = probe_note_peak(/*with_efx=*/false, /*reset_midstream=*/false);
  const float through_efx = probe_note_peak(true, false);
  const float after_reset = probe_note_peak(true, true);

  // Without this the last assertion passes on a chain that was never audible.
  REQUIRE(through_efx < 0.2f * dry);
  REQUIRE(after_reset > 0.5f * dry);

  SECTION("the reset silences the sounding note, which reads as a surviving chain") {
    // A GS reset runs all-sound-off before restoring the power-on state, so
    // measuring the note that was already sounding answers a different question:
    // the note is cut and what remains is the chain's tail. That render is
    // QUIETER than the un-reset one rather than dry, which is the shape a
    // surviving chain would never produce.
    Sf2Player player = make_player(efx_config());
    REQUIRE(player.handle_sysex(part_on, sizeof(part_on)));
    REQUIRE(player.handle_sysex(od_type, sizeof(od_type)));
    player.on_event(0, event(sonare::midi::make_midi1_note_on(0, 0, 60, 127)));
    static_cast<void>(render(player, 24000));
    REQUIRE(player.handle_sysex(gs_reset, sizeof(gs_reset)));
    const float tail = peak(render(player, 24000).left, 0, 24000);

    Sf2Player held = make_player(efx_config());
    REQUIRE(held.handle_sysex(part_on, sizeof(part_on)));
    REQUIRE(held.handle_sysex(od_type, sizeof(od_type)));
    held.on_event(0, event(sonare::midi::make_midi1_note_on(0, 0, 60, 127)));
    static_cast<void>(render(held, 24000));
    const float sustained = peak(render(held, 24000).left, 0, 24000);

    REQUIRE(tail < sustained);
  }
}

#endif  // SONARE_WITH_MASTERING

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

#if defined(SONARE_WITH_MASTERING)

TEST_CASE("a live GS EFX swap keeps the audio path allocation-free", "[midi][sf2][gsfx][rt]") {
  // Enable EFX on part 1 (channel 0) and select Overdrive (01 10); Roland DT1
  // checksums. The control thread builds + publishes; the audio thread swaps.
  const uint8_t part_on[] = {0xF0, 0x41, 0x10, 0x42, 0x12, 0x40, 0x41, 0x22, 0x01, 0x5C, 0xF7};
  const uint8_t od_type[] = {0xF0, 0x41, 0x10, 0x42, 0x12, 0x40,
                             0x03, 0x00, 0x01, 0x10, 0x2C, 0xF7};

  Sf2PlayerConfig cfg;
  cfg.gain = 1.0f;
  // Live-capable (realize_efx_inline stays false): the control thread realises.
  cfg.insert_factory = [](std::string_view name, std::string_view json) {
    return sonare::mastering::api::make_insert(std::string(name), std::string(json));
  };
  Sf2Player player = make_player(cfg);
  std::vector<float> left(512, 0.0f), right(512, 0.0f);
  float* chans[2] = {left.data(), right.data()};

  // Control thread installs the EFX (allocates a chain, publishes a snapshot).
  player.on_control_sysex(part_on, sizeof(part_on));
  player.on_control_sysex(od_type, sizeof(od_type));
  player.on_event(0, event(sonare::midi::make_midi1_note_on(0, 0, 60, 100)));
  player.process(chans, 2, 512);  // warm-up: adopt the published chain
  // A second control-thread publish leaves a pending swap for the next block.
  player.on_control_sysex(od_type, sizeof(od_type));

  AllocationGuard guard;
  // The audio thread adopts the pending snapshot (wait-free swap) and runs the
  // installed inserts: no allocation and no free on the render path.
  player.process(chans, 2, 512);
  player.process(chans, 2, 512);
  REQUIRE(guard.count() == 0);
}

TEST_CASE("concurrent live GS EFX realises and audio render stay safe",
          "[midi][sf2][gsfx][rt][.][slow]") {
  // Stress the wait-free chain swap under real thread contention: one control
  // thread hammers EFX realises (build + publish + free of retired snapshots)
  // while the audio thread renders (acquire + swap + run). Catches gross
  // corruption on any build; run under ThreadSanitizer it validates the swap has
  // no data race. RtPublisher's single-consumer contract holds — only the audio
  // thread calls process()/acquire(), only the control thread publishes.
  const uint8_t part_on[] = {0xF0, 0x41, 0x10, 0x42, 0x12, 0x40, 0x41, 0x22, 0x01, 0x5C, 0xF7};
  const uint8_t od_type[] = {0xF0, 0x41, 0x10, 0x42, 0x12, 0x40,
                             0x03, 0x00, 0x01, 0x10, 0x2C, 0xF7};
  const uint8_t chorus[] = {0xF0, 0x41, 0x10, 0x42, 0x12, 0x40, 0x03, 0x00, 0x01, 0x42, 0x7A, 0xF7};
  const uint8_t gs_reset[] = {0xF0, 0x41, 0x10, 0x42, 0x12, 0x40, 0x00, 0x7F, 0x00, 0x41, 0xF7};

  Sf2PlayerConfig cfg;
  cfg.gain = 1.0f;
  cfg.insert_factory = [](std::string_view name, std::string_view json) {
    return sonare::mastering::api::make_insert(std::string(name), std::string(json));
  };
  Sf2Player player = make_player(cfg);
  player.on_event(0, event(sonare::midi::make_midi1_note_on(0, 0, 60, 100)));

  std::atomic<bool> stop{false};
  std::thread control([&] {
    for (int i = 0; i < 1000 && !stop.load(std::memory_order_relaxed); ++i) {
      player.on_control_sysex(part_on, sizeof(part_on));
      player.on_control_sysex((i & 1) ? od_type : chorus,
                              (i & 1) ? sizeof(od_type) : sizeof(chorus));
      if (i % 9 == 0) player.on_control_sysex(gs_reset, sizeof(gs_reset));
    }
  });

  std::vector<float> left(512, 0.0f), right(512, 0.0f);
  float* chans[2] = {left.data(), right.data()};
  size_t non_finite = 0;
  float render_peak = 0.0f;
  for (int b = 0; b < 1000; ++b) {
    player.process(chans, 2, 512);
    for (int i = 0; i < 512; ++i) {
      if (!std::isfinite(left[static_cast<size_t>(i)]) ||
          !std::isfinite(right[static_cast<size_t>(i)])) {
        ++non_finite;
        continue;
      }
      render_peak = std::max(render_peak, std::abs(left[static_cast<size_t>(i)]));
      render_peak = std::max(render_peak, std::abs(right[static_cast<size_t>(i)]));
    }
  }
  stop.store(true, std::memory_order_relaxed);
  control.join();

  WARN("render peak under contention: " << render_peak);
  CHECK(non_finite == 0);
  // Above an audible floor, not merely above zero: a buffer of denormals is
  // not evidence the render survived the contention.
  CHECK(render_peak > 1.0e-3f);

  // The control thread stopped mid-sequence, so pin the realise against a known
  // final publish: the mirror carries the last type and nothing stayed pending.
  player.on_control_sysex(part_on, sizeof(part_on));
  player.on_control_sysex(od_type, sizeof(od_type));
  CHECK(player.gs_efx(0).type == 0x0110u);
  CHECK_FALSE(player.gs_efx_dirty());
  player.on_control_sysex(chorus, sizeof(chorus));
  CHECK(player.gs_efx(0).type == 0x0142u);
  CHECK_FALSE(player.gs_efx_dirty());

  // The audio thread swaps the final snapshot in on the next block.
  player.process(chans, 2, 512);
  size_t post_swap_non_finite = 0;
  for (int i = 0; i < 512; ++i) {
    if (!std::isfinite(left[static_cast<size_t>(i)]) ||
        !std::isfinite(right[static_cast<size_t>(i)])) {
      ++post_swap_non_finite;
    }
  }
  CHECK(post_swap_non_finite == 0);
}

#endif  // SONARE_WITH_MASTERING
