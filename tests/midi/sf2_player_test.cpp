/// @file sf2_player_test.cpp
/// @brief SF2 player MVP (midi/synth/sf2_player): root-key tuning accuracy,
///        loop sustain vs one-shot end, velocity layer selection, pan,
///        release tail inside tail_samples(), channel-mode CC semantics
///        (CC64/120/121/123), drum-channel bank-128 resolution, GS variation
///        fallback, deterministic rendering and a no-alloc audio path.

#include "midi/synth/sf2_player.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <cstdint>
#include <memory>
#include <vector>

#include "midi/midi_event.h"
#include "midi/synth/sf2_file.h"
#include "midi/synth/sf2_voice.h"
#include "midi/ump.h"
#include "support/alloc_guard.h"
#include "support/sf2_builder.h"

namespace {

using Catch::Approx;
using sonare::midi::MidiEvent;
using sonare::midi::synth::Sf2File;
using sonare::midi::synth::Sf2Player;
using sonare::midi::synth::Sf2PlayerConfig;
using sonare::midi::synth::Sf2Voice;
using sonare::midi::synth::Sf2VoiceParams;
using sonare::test::AllocationGuard;
using sonare::test::Sf2Builder;

constexpr double kOutRate = 48000.0;

MidiEvent event(const sonare::midi::Ump& ump) {
  MidiEvent e;
  e.ump = ump;
  return e;
}

/// Fixture: a looped 1 kHz sine preset (program 0), a hard-left-panned copy
/// (program 1), a one-shot preset (program 2) and a bank-128 drum kit.
std::shared_ptr<Sf2File> make_fixture() {
  Sf2Builder b;

  // 96-sample sine, period 32, recorded at 32 kHz -> 1000 Hz at root key 60.
  std::vector<float> sine(96);
  for (size_t i = 0; i < sine.size(); ++i) {
    sine[i] =
        0.9f * static_cast<float>(std::sin(2.0 * 3.14159265358979 * static_cast<double>(i) / 32.0));
  }
  const int sine_id = b.add_sample("sine1k", sine, 32000, 60, 32, 96);

  std::vector<float> burst(64);
  for (size_t i = 0; i < burst.size(); ++i) burst[i] = ((i * 37) % 64) / 64.0f - 0.5f;
  const int burst_id = b.add_sample("burst", burst, 44100, 60, 0, 64);

  Sf2Builder::ZoneSpec looped;
  looped.gens.push_back({54 /*sampleModes*/, 1});
  looped.target = sine_id;
  const int melodic = b.add_instrument("melodic", {looped});

  Sf2Builder::ZoneSpec left = looped;
  left.gens.push_back({17 /*pan*/, -500});
  const int left_inst = b.add_instrument("left", {left});

  Sf2Builder::ZoneSpec oneshot;
  oneshot.target = burst_id;
  const int perc = b.add_instrument("oneshot", {oneshot});

  Sf2Builder::ZoneSpec pz0;
  pz0.target = melodic;
  b.add_preset("Sine", 0, 0, {pz0});

  Sf2Builder::ZoneSpec pz1;
  pz1.target = left_inst;
  b.add_preset("SineLeft", 0, 1, {pz1});

  Sf2Builder::ZoneSpec gm2_lsb;
  gm2_lsb.target = left_inst;
  b.add_preset("Gm2Lsb", 5, 0, {gm2_lsb});

  Sf2Builder::ZoneSpec pz2;
  pz2.target = perc;
  b.add_preset("Burst", 0, 2, {pz2});

  Sf2Builder::ZoneSpec low_only_inst_zone = looped;
  low_only_inst_zone.key_lo = 0;
  low_only_inst_zone.key_hi = 48;
  const int low_only_inst = b.add_instrument("low-only", {low_only_inst_zone});
  Sf2Builder::ZoneSpec low_only_preset_zone;
  low_only_preset_zone.target = low_only_inst;
  b.add_preset("LowOnly", 0, 3, {low_only_preset_zone});

  Sf2Builder::ZoneSpec dz;
  dz.target = perc;
  b.add_preset("Kit", 128, 0, {dz});

  const auto bytes = b.build();
  auto sf2 = std::make_shared<Sf2File>();
  std::string error;
  REQUIRE(sf2->parse(bytes.data(), bytes.size(), &error));
  return sf2;
}

Sf2Player make_player(std::shared_ptr<Sf2File> sf2, int polyphony = 48,
                      bool prefer_model_for_modeled_families = false) {
  Sf2PlayerConfig cfg;
  cfg.gain = 1.0f;
  cfg.polyphony = polyphony;
  cfg.prefer_model_for_modeled_families = prefer_model_for_modeled_families;
  // These cases assert voice routing invariants (exact silence after tails,
  // hard-pan isolation) that the always-on default room would smear; the GS
  // effect bus has its own coverage in sf2_effects_test.cpp.
  cfg.effects.enable_reverb = false;
  cfg.effects.enable_chorus = false;
  cfg.effects.enable_delay = false;
  Sf2Player player(cfg);
  player.set_soundfont(std::move(sf2));
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

float peak(const std::vector<float>& buf) {
  float p = 0.0f;
  for (float s : buf) p = std::max(p, std::fabs(s));
  return p;
}

/// A player with no SoundFont: every note resolves through the synth fallback
/// floor (the data-free model), where the GS drum kit + NRPN edits live.
Sf2Player make_fallback_player() {
  Sf2PlayerConfig cfg;
  cfg.gain = 1.0f;
  Sf2Player player(cfg);
  player.prepare(kOutRate, 256);  // no set_soundfont -> synth fallback
  return player;
}

TEST_CASE("Sf2Player model-first preference bypasses covered melodic presets but not drums",
          "[midi][sf2][synth]") {
  const auto sf2 = make_fixture();
  Sf2Player sampled = make_player(sf2);
  Sf2Player modeled = make_player(sf2, 48, true);

  sampled.on_event(0, event(sonare::midi::make_midi1_note_on(0, 0, 60, 100)));
  modeled.on_event(0, event(sonare::midi::make_midi1_note_on(0, 0, 60, 100)));
  const StereoRender sampled_melodic = render(sampled, 512);
  const StereoRender modeled_melodic = render(modeled, 512);
  REQUIRE(peak(sampled_melodic.left) > 1.0e-4f);
  REQUIRE(peak(modeled_melodic.left) > 1.0e-4f);
  // The fixture's sampled preset is a looped sine, while program 0's fallback
  // is the dedicated physical piano model. Exact equality would mean the
  // model-first branch failed to bypass the covered SoundFont preset.
  REQUIRE(sampled_melodic.left != modeled_melodic.left);

  sampled.reset();
  modeled.reset();
  sampled.on_event(0, event(sonare::midi::make_midi1_note_on(0, 9, 36, 100)));
  modeled.on_event(0, event(sonare::midi::make_midi1_note_on(0, 9, 36, 100)));
  const StereoRender sampled_drum = render(sampled, 512);
  const StereoRender modeled_drum = render(modeled, 512);
  REQUIRE(peak(sampled_drum.left) > 1.0e-4f);
  REQUIRE(sampled_drum.left == modeled_drum.left);
  REQUIRE(sampled_drum.right == modeled_drum.right);
}

TEST_CASE("Sf2Player model-first preference has no effect without a SoundFont",
          "[midi][sf2][synth]") {
  Sf2Player default_player = make_player(nullptr);
  Sf2Player preferred_player = make_player(nullptr, 48, true);
  const auto note_on = event(sonare::midi::make_midi1_note_on(0, 0, 60, 100));
  default_player.on_event(0, note_on);
  preferred_player.on_event(0, note_on);

  const StereoRender default_audio = render(default_player, 512);
  const StereoRender preferred_audio = render(preferred_player, 512);
  REQUIRE(peak(default_audio.left) > 1.0e-4f);
  REQUIRE(default_audio.left == preferred_audio.left);
  REQUIRE(default_audio.right == preferred_audio.right);
}

/// Single-frequency magnitude (Goertzel) of a buffer at @p freq_hz.
double goertzel(const std::vector<float>& buf, double freq_hz) {
  const double w = 2.0 * M_PI * freq_hz / kOutRate;
  const double coeff = 2.0 * std::cos(w);
  double s_prev = 0.0;
  double s_prev2 = 0.0;
  for (float x : buf) {
    const double s = static_cast<double>(x) + coeff * s_prev - s_prev2;
    s_prev2 = s_prev;
    s_prev = s;
  }
  return std::sqrt(s_prev * s_prev + s_prev2 * s_prev2 - coeff * s_prev * s_prev2);
}

float rms(const std::vector<float>& buf, size_t from = 0) {
  double acc = 0.0;
  size_t n = 0;
  for (size_t i = from; i < buf.size(); ++i) {
    acc += static_cast<double>(buf[i]) * buf[i];
    ++n;
  }
  return n > 0 ? static_cast<float>(std::sqrt(acc / static_cast<double>(n))) : 0.0f;
}

/// Fundamental frequency estimate from interpolated rising zero crossings.
double estimate_frequency(const std::vector<float>& buf, size_t from) {
  double first = -1.0;
  double last = -1.0;
  int cycles = -1;
  for (size_t i = from + 1; i < buf.size(); ++i) {
    if (buf[i - 1] < 0.0f && buf[i] >= 0.0f) {
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
  }
  if (cycles < 1 || last <= first) return 0.0;
  return kOutRate * static_cast<double>(cycles) / (last - first);
}

}  // namespace

TEST_CASE("Sf2Player plays a looped sample at the root-key frequency", "[midi][sf2]") {
  Sf2Player player = make_player(make_fixture());
  player.on_event(0, event(sonare::midi::make_midi1_note_on(0, 0, 60, 127)));
  const StereoRender out = render(player, 48000);

  REQUIRE(peak(out.left) > 0.1f);
  // 1000 Hz within a few cents (a cent at 1 kHz is ~0.58 Hz).
  const double freq = estimate_frequency(out.left, 4800);
  REQUIRE(freq == Approx(1000.0).margin(2.0));

  // An octave up doubles the frequency (root-key tuning).
  Sf2Player player2 = make_player(make_fixture());
  player2.on_event(0, event(sonare::midi::make_midi1_note_on(0, 0, 72, 127)));
  const StereoRender out2 = render(player2, 48000);
  REQUIRE(estimate_frequency(out2.left, 4800) == Approx(2000.0).margin(4.0));

  // The loop sustains: the last quarter of a 1 s render is still sounding.
  REQUIRE(rms(out.left, 36000) > 0.1f);
}

TEST_CASE("Sf2Player falls back when a covered preset has no matching zone", "[midi][sf2]") {
  Sf2Player player = make_player(make_fixture());
  player.on_event(0, event(sonare::midi::make_midi1_program_change(0, 0, 3)));
  player.on_event(0, event(sonare::midi::make_midi1_note_on(0, 0, 84, 127)));

  REQUIRE(player.active_voice_count() == 1);
  const StereoRender out = render(player, 4096);
  REQUIRE(peak(out.left) > 0.001f);
}

TEST_CASE("Sf2Player one-shot samples end and release tail is bounded", "[midi][sf2]") {
  Sf2Player player = make_player(make_fixture());
  // Program 2 = unlooped burst (~64 samples at 44.1k -> ~70 output samples).
  player.on_event(0, event(sonare::midi::make_midi1_program_change(0, 0, 2)));
  player.on_event(0, event(sonare::midi::make_midi1_note_on(0, 0, 60, 127)));
  const StereoRender head = render(player, 256);
  REQUIRE(peak(head.left) > 0.01f);
  const StereoRender tail = render(player, 256);
  REQUIRE(peak(tail.left) == 0.0f);
  REQUIRE(player.active_voice_count() == 0);
}

TEST_CASE("Sf2Player note-off releases through tail_samples", "[midi][sf2]") {
  Sf2Player player = make_player(make_fixture());
  REQUIRE(player.tail_samples() > 0);

  player.on_event(0, event(sonare::midi::make_midi1_note_on(0, 0, 60, 127)));
  render(player, 4800);
  player.on_event(0, event(sonare::midi::make_midi1_note_off(0, 0, 60, 0)));
  // After the tail the player must be silent (no truncated/never-ending release).
  render(player, player.tail_samples() + 256);
  const StereoRender after = render(player, 256);
  REQUIRE(peak(after.left) == 0.0f);
  REQUIRE(player.active_voice_count() == 0);
}

TEST_CASE("Sf2Player velocity scales loudness", "[midi][sf2]") {
  Sf2Player loud = make_player(make_fixture());
  loud.on_event(0, event(sonare::midi::make_midi1_note_on(0, 0, 60, 127)));
  const float loud_rms = rms(render(loud, 9600).left, 2400);

  Sf2Player soft = make_player(make_fixture());
  soft.on_event(0, event(sonare::midi::make_midi1_note_on(0, 0, 60, 40)));
  const float soft_rms = rms(render(soft, 9600).left, 2400);

  REQUIRE(loud_rms > 0.1f);
  REQUIRE(soft_rms > 0.0f);
  REQUIRE(soft_rms < loud_rms * 0.4f);
}

TEST_CASE("Sf2Player pan generator routes to the stereo legs", "[midi][sf2]") {
  Sf2Player player = make_player(make_fixture());
  player.on_event(0, event(sonare::midi::make_midi1_program_change(0, 0, 1)));
  player.on_event(0, event(sonare::midi::make_midi1_note_on(0, 0, 60, 127)));
  const StereoRender out = render(player, 4800);
  REQUIRE(peak(out.left) > 0.1f);
  REQUIRE(peak(out.right) < peak(out.left) * 1e-3f);
}

TEST_CASE("Sf2Player decodes MIDI 2.0 banked program changes", "[midi][sf2]") {
  Sf2Player player = make_player(make_fixture());
  player.on_event(0, event(sonare::midi::make_midi2_program_change(0, 0, 1, 0, 0, true)));
  player.on_event(0, event(sonare::midi::make_midi2_note_on(0, 0, 60, 0xFFFFu)));
  const StereoRender out = render(player, 4800);
  REQUIRE(peak(out.left) > 0.1f);
  REQUIRE(peak(out.right) < peak(out.left) * 1e-3f);
}

TEST_CASE("Sf2Player drum channel resolves bank 128", "[midi][sf2]") {
  Sf2Player player = make_player(make_fixture());
  // Channel 9 ignores the melodic banks and plays the kit (one-shot burst).
  player.on_event(0, event(sonare::midi::make_midi1_note_on(0, 9, 60, 127)));
  const StereoRender out = render(player, 256);
  REQUIRE(peak(out.left) > 0.01f);
  const StereoRender after = render(player, 512);
  REQUIRE(peak(after.left) == 0.0f);  // one-shot kit sample ended
}

TEST_CASE("Sf2Player unknown variation bank falls back to the capital tone", "[midi][sf2]") {
  Sf2Player player = make_player(make_fixture());
  // GS variation bank 8 is not in the fixture; program 0 must still sound.
  player.on_event(0, event(sonare::midi::make_midi1_control_change(0, 0, 0, 8)));
  player.on_event(0, event(sonare::midi::make_midi1_program_change(0, 0, 0)));
  player.on_event(0, event(sonare::midi::make_midi1_note_on(0, 0, 60, 127)));
  REQUIRE(peak(render(player, 4800).left) > 0.1f);
}

TEST_CASE("Sf2Player resolves GM2 Bank Select LSB variations", "[midi][sf2]") {
  Sf2Player player = make_player(make_fixture());
  player.on_event(0, event(sonare::midi::make_midi1_control_change(0, 0, 0, 0x79)));
  player.on_event(0, event(sonare::midi::make_midi1_control_change(0, 0, 32, 5)));
  player.on_event(0, event(sonare::midi::make_midi1_program_change(0, 0, 0)));
  player.on_event(0, event(sonare::midi::make_midi1_note_on(0, 0, 60, 127)));
  const StereoRender out = render(player, 4800);
  REQUIRE(peak(out.left) > 0.1f);
  REQUIRE(peak(out.right) < peak(out.left) * 1e-3f);
}

TEST_CASE("Sf2Player channel-mode CCs match BuiltinSynth semantics", "[midi][sf2]") {
  Sf2Player player = make_player(make_fixture());

  SECTION("CC64 holds released notes until lifted") {
    player.on_event(0, event(sonare::midi::make_midi1_note_on(0, 0, 60, 127)));
    player.on_event(0, event(sonare::midi::make_midi1_control_change(0, 0, 64, 127)));
    player.on_event(0, event(sonare::midi::make_midi1_note_off(0, 0, 60, 0)));
    render(player, player.tail_samples() + 4800);
    REQUIRE(peak(render(player, 256).left) > 0.0f);  // still sounding
    player.on_event(0, event(sonare::midi::make_midi1_control_change(0, 0, 64, 0)));
    render(player, player.tail_samples() + 4800);
    REQUIRE(peak(render(player, 256).left) == 0.0f);
  }

  SECTION("CC120 silences immediately") {
    player.on_event(0, event(sonare::midi::make_midi1_note_on(0, 0, 60, 127)));
    render(player, 2400);
    player.on_event(0, event(sonare::midi::make_midi1_control_change(0, 0, 120, 0)));
    REQUIRE(peak(render(player, 256).left) == 0.0f);
  }

  SECTION("CC123 releases gracefully") {
    player.on_event(0, event(sonare::midi::make_midi1_note_on(0, 0, 60, 127)));
    render(player, 2400);
    player.on_event(0, event(sonare::midi::make_midi1_control_change(0, 0, 123, 0)));
    render(player, player.tail_samples() + 4800);
    REQUIRE(peak(render(player, 256).left) == 0.0f);
  }

  SECTION("CC121 lifts the sustain pedal") {
    player.on_event(0, event(sonare::midi::make_midi1_note_on(0, 0, 60, 127)));
    player.on_event(0, event(sonare::midi::make_midi1_control_change(0, 0, 64, 127)));
    player.on_event(0, event(sonare::midi::make_midi1_note_off(0, 0, 60, 0)));
    player.on_event(0, event(sonare::midi::make_midi1_control_change(0, 0, 121, 0)));
    render(player, player.tail_samples() + 4800);
    REQUIRE(peak(render(player, 256).left) == 0.0f);
  }

  SECTION("channel isolation") {
    player.on_event(0, event(sonare::midi::make_midi1_note_on(0, 0, 60, 127)));
    player.on_event(0, event(sonare::midi::make_midi1_note_on(0, 1, 67, 127)));
    player.on_event(0, event(sonare::midi::make_midi1_control_change(0, 0, 120, 0)));
    REQUIRE(peak(render(player, 256).left) > 0.0f);  // channel 1 still sounds
  }
}

TEST_CASE("Sf2Player renders bit-identically for identical event streams", "[midi][sf2]") {
  auto run = [] {
    Sf2Player player = make_player(make_fixture(), 8);
    std::vector<float> out;
    for (int block = 0; block < 8; ++block) {
      // A busy, steal-heavy sequence.
      for (int n = 0; n < 4; ++n) {
        player.on_event(0, event(sonare::midi::make_midi1_note_on(
                               0, static_cast<uint8_t>(n % 3),
                               static_cast<uint8_t>(48 + (block * 4 + n) % 24), 100)));
      }
      if (block % 2 == 1) {
        player.on_event(0, event(sonare::midi::make_midi1_note_off(
                               0, 0, static_cast<uint8_t>(48 + (block * 4) % 24), 0)));
      }
      const StereoRender r = render(player, 512);
      out.insert(out.end(), r.left.begin(), r.left.end());
      out.insert(out.end(), r.right.begin(), r.right.end());
    }
    return out;
  };
  const std::vector<float> a = run();
  const std::vector<float> b = run();
  REQUIRE(a == b);
}

TEST_CASE("Sf2Player audio path performs no heap allocation after prepare", "[midi][sf2][rt]") {
  Sf2Player player = make_player(make_fixture(), 16);
  std::vector<float> left(512, 0.0f);
  std::vector<float> right(512, 0.0f);
  float* chans[2] = {left.data(), right.data()};

  // Warm-up block.
  player.on_event(0, event(sonare::midi::make_midi1_note_on(0, 0, 60, 100)));
  player.process(chans, 2, 512);

  AllocationGuard guard;
  for (int n = 0; n < 24; ++n) {
    player.on_event(0, event(sonare::midi::make_midi1_note_on(0, static_cast<uint8_t>(n % 16),
                                                              static_cast<uint8_t>(40 + n), 100)));
  }
  player.process(chans, 2, 512);
  player.on_event(0, event(sonare::midi::make_midi1_control_change(0, 0, 123, 0)));
  player.process(chans, 2, 512);
  REQUIRE(guard.count() == 0);
}

TEST_CASE("Sf2Voice wraps sustained loops in constant time", "[midi][sf2]") {
  const float pool[] = {0.0f, 1.0f, -1.0f, 0.5f};
  Sf2VoiceParams params;
  params.start = 0;
  params.end = 4;
  params.loop_start = 1;
  params.loop_end = 2;
  params.loop_mode = 1;
  params.pitch_increment = 1.0;

  Sf2Voice voice;
  voice.start(pool, params, kOutRate, 1.0f);
  voice.active = true;
  voice.pos = 1.0e12;

  const float sample = voice.render({});

  REQUIRE(std::isfinite(sample));
  REQUIRE(voice.pos < 3.0);
}

TEST_CASE("Sf2Player applies GS per-note drum NRPN to the fallback voice", "[midi][sf2]") {
  // The GS per-note drum edits (pitch coarse / TVA level / absolute pan) must
  // reach the data-free model floor, not just SF2 voices. NRPN sequence:
  // CC99 = param MSB (0x18 pitch / 0x1A level / 0x1C pan), CC98 = drum note,
  // CC6 = data. Note 56 (cowbell) has clear ~587/845 Hz partials.
  auto nrpn = [](Sf2Player& p, uint8_t msb, uint8_t note, uint8_t data) {
    p.on_event(0, event(sonare::midi::make_midi1_control_change(0, 9, 99, msb)));
    p.on_event(0, event(sonare::midi::make_midi1_control_change(0, 9, 98, note)));
    p.on_event(0, event(sonare::midi::make_midi1_control_change(0, 9, 6, data)));
  };
  auto strike = [](Sf2Player& p, uint8_t note) {
    p.on_event(0, event(sonare::midi::make_midi1_note_on(0, 9, note, 110)));
  };

  SECTION("pitch coarse raises the drum an octave") {
    Sf2Player base = make_fallback_player();
    strike(base, 56);
    const StereoRender b = render(base, 8000);
    Sf2Player up = make_fallback_player();
    nrpn(up, 0x18, 56, 76);  // +12 semitones (data - 64)
    strike(up, 56);
    const StereoRender u = render(up, 8000);
    REQUIRE(goertzel(b.left, 587.0) > goertzel(b.left, 1174.0));  // baseline centred low
    REQUIRE(goertzel(u.left, 1174.0) > goertzel(u.left, 587.0));  // shifted up an octave
  }

  SECTION("TVA level attenuates the drum") {
    Sf2Player loud = make_fallback_player();
    strike(loud, 56);
    const float loud_peak = peak(render(loud, 8000).left);
    Sf2Player soft = make_fallback_player();
    nrpn(soft, 0x1A, 56, 64);  // level 64 -> (64/127)^2 ~ 0.25
    strike(soft, 56);
    const float soft_peak = peak(render(soft, 8000).left);
    REQUIRE(soft_peak > 0.0f);
    REQUIRE(soft_peak < 0.5f * loud_peak);
  }

  SECTION("absolute pan places the drum hard left") {
    Sf2Player p = make_fallback_player();
    nrpn(p, 0x1C, 56, 0);  // pan 0 -> hard left
    strike(p, 56);
    const StereoRender out = render(p, 8000);
    REQUIRE(peak(out.left) > 4.0f * peak(out.right));
  }
}
