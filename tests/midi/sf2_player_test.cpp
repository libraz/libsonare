/// @file sf2_player_test.cpp
/// @brief SF2 player MVP (midi/synth/sf2_player): root-key tuning accuracy,
///        loop sustain vs one-shot end, velocity layer selection, pan,
///        release tail inside tail_samples(), channel-mode CC semantics
///        (CC64/120/121/123), drum-channel bank-128 resolution, GS variation
///        fallback, deterministic rendering and a no-alloc audio path.

#include "midi/synth/sf2_player.h"

#include <array>
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <cstdint>
#include <memory>
#include <vector>

#include "midi/control_value.h"
#include "midi/midi_event.h"
#include "midi/synth/sf2_file.h"
#include "midi/synth/sf2_voice.h"
#include "midi/ump.h"
#include "support/alloc_guard.h"
#include "support/audio_fixtures.h"
#include "support/midi_render.h"
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

/// "Nothing is sounding" floor, -80 dBFS. The mix bus carries a DC blocker,
/// and a first-order highpass answers the end of a note by discharging over
/// its ~20 ms time constant rather than snapping to zero, so silence here is a
/// level below audibility instead of a bit-exact zero. Every note these cases
/// play sounds at least two orders of magnitude above it.
constexpr float kSilenceFloor = 1.0e-4f;

using sonare::test::event;

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
  // These cases assert voice routing invariants (silence after tails,
  // hard-pan isolation) that the always-on default room would smear; the GS
  // effect bus has its own coverage in sf2_effects_test.cpp. The config
  // member itself is FX-gated, so there is nothing to disable without it.
#if defined(SONARE_MIDI_WITH_FX)
  cfg.effects.enable_reverb = false;
  cfg.effects.enable_chorus = false;
  cfg.effects.enable_delay = false;
#endif
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
  REQUIRE(peak(tail.left) < kSilenceFloor);
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
  REQUIRE(peak(after.left) < kSilenceFloor);
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
  REQUIRE(peak(after.left) < kSilenceFloor);  // one-shot kit sample ended
}

TEST_CASE("Sf2Player GM2 percussion bank uses drum fallback semantics", "[midi][sf2][synth]") {
  // GM2 selects a rhythm part on any channel through CC0=120, not only the
  // conventional channel 10. Its fallback output must therefore match the
  // standard drum channel and honor per-note drum NRPNs.
  Sf2Player panned = make_fallback_player();
  panned.on_event(0, event(sonare::midi::make_midi1_control_change(0, 1, 0, 0x78)));
  panned.on_event(0, event(sonare::midi::make_midi1_control_change(0, 1, 99, 0x1C)));
  panned.on_event(0, event(sonare::midi::make_midi1_control_change(0, 1, 98, 36)));
  panned.on_event(0, event(sonare::midi::make_midi1_control_change(0, 1, 6, 0)));
  panned.on_event(0, event(sonare::midi::make_midi1_note_on(0, 1, 36, 110)));
  const StereoRender pan = render(panned, 4096);
  REQUIRE(peak(pan.left) > 4.0f * peak(pan.right));
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
    REQUIRE(peak(render(player, 256).left) < kSilenceFloor);
  }

  SECTION("CC120 silences immediately") {
    player.on_event(0, event(sonare::midi::make_midi1_note_on(0, 0, 60, 127)));
    render(player, 2400);
    player.on_event(0, event(sonare::midi::make_midi1_control_change(0, 0, 120, 0)));
    REQUIRE(peak(render(player, 256).left) < kSilenceFloor);
  }

  SECTION("CC123 releases gracefully") {
    player.on_event(0, event(sonare::midi::make_midi1_note_on(0, 0, 60, 127)));
    render(player, 2400);
    player.on_event(0, event(sonare::midi::make_midi1_control_change(0, 0, 123, 0)));
    render(player, player.tail_samples() + 4800);
    REQUIRE(peak(render(player, 256).left) < kSilenceFloor);
  }

  SECTION("CC121 lifts the sustain pedal") {
    player.on_event(0, event(sonare::midi::make_midi1_note_on(0, 0, 60, 127)));
    player.on_event(0, event(sonare::midi::make_midi1_control_change(0, 0, 64, 127)));
    player.on_event(0, event(sonare::midi::make_midi1_note_off(0, 0, 60, 0)));
    player.on_event(0, event(sonare::midi::make_midi1_control_change(0, 0, 121, 0)));
    render(player, player.tail_samples() + 4800);
    REQUIRE(peak(render(player, 256).left) < kSilenceFloor);
  }

  SECTION("channel isolation") {
    player.on_event(0, event(sonare::midi::make_midi1_note_on(0, 0, 60, 127)));
    player.on_event(0, event(sonare::midi::make_midi1_note_on(0, 1, 67, 127)));
    player.on_event(0, event(sonare::midi::make_midi1_control_change(0, 0, 120, 0)));
    REQUIRE(peak(render(player, 256).left) > 0.0f);  // channel 1 still sounds
  }
}

TEST_CASE("Sf2Player clears a stale sostenuto capture when a voice slot is reused", "[midi][sf2]") {
  // A single slot so the second note provably lands in the first note's slot.
  Sf2Player player = make_player(make_fixture(), /*polyphony=*/1);

  // Capture C4 with the sostenuto pedal, then silence the part with All Sound
  // Off while CC66 is still down: the slot goes inactive with its capture still
  // set, and the pedal-up sweep only visits active voices.
  player.on_event(0, event(sonare::midi::make_midi1_note_on(0, 0, 60, 100)));
  render(player, 256);
  player.on_event(0, event(sonare::midi::make_midi1_control_change(0, 0, 66, 127)));
  player.on_event(0, event(sonare::midi::make_midi1_control_change(0, 0, 120, 0)));
  player.on_event(0, event(sonare::midi::make_midi1_control_change(0, 0, 66, 0)));
  REQUIRE(player.active_voice_count() == 0);

  // The next note reuses that slot, and must answer to its own note-off.
  player.on_event(0, event(sonare::midi::make_midi1_note_on(0, 0, 62, 100)));
  REQUIRE(peak(render(player, 256).left) > 1.0e-4f);
  player.on_event(0, event(sonare::midi::make_midi1_note_off(0, 0, 62, 0)));
  render(player, 9600);  // the fixture's release is far shorter than tail_samples()
  REQUIRE(player.active_voice_count() == 0);
  REQUIRE(peak(render(player, 256).left) < kSilenceFloor);
}

TEST_CASE("Sf2Player exclusive class spares the layers of the same note-on", "[midi][sf2]") {
  // SoundFont 2.04 section 8.1.2 scopes exclusiveClass to notes already
  // sounding: two layers sounded by one note-on (a stereo hi-hat's legs) share
  // a class by design and must both survive.
  Sf2Builder b;
  std::vector<float> sine(96);
  for (size_t i = 0; i < sine.size(); ++i) {
    sine[i] =
        0.9f * static_cast<float>(std::sin(2.0 * 3.14159265358979 * static_cast<double>(i) / 32.0));
  }
  const int sine_id = b.add_sample("sine1k", sine, 32000, 60, 32, 96);

  Sf2Builder::ZoneSpec left;
  left.gens.push_back({54 /*sampleModes*/, 1});
  left.gens.push_back({57 /*exclusiveClass*/, 1});
  left.gens.push_back({17 /*pan*/, -500});
  left.target = sine_id;
  Sf2Builder::ZoneSpec right = left;
  right.gens[2] = {17, 500};
  const int stereo_pair = b.add_instrument("stereo-pair", {left, right});

  Sf2Builder::ZoneSpec pz;
  pz.target = stereo_pair;
  b.add_preset("Pair", 0, 0, {pz});

  const std::vector<uint8_t> bytes = b.build();
  auto sf2 = std::make_shared<Sf2File>();
  REQUIRE(sf2->parse(bytes.data(), bytes.size(), nullptr));
  Sf2Player player = make_player(std::move(sf2));

  player.on_event(0, event(sonare::midi::make_midi1_note_on(0, 0, 60, 100)));
  const StereoRender out = render(player, 512);
  // Both legs must still be sounding well past the ~1 ms default release, so
  // measure the second half rather than the note-on transient.
  const auto sustained_peak = [](const std::vector<float>& buf) {
    float p = 0.0f;
    for (size_t i = buf.size() / 2; i < buf.size(); ++i) p = std::max(p, std::fabs(buf[i]));
    return p;
  };
  REQUIRE(player.active_voice_count() == 2);
  REQUIRE(sustained_peak(out.left) > 1.0e-4f);
  REQUIRE(sustained_peak(out.right) > 1.0e-4f);

  // A later strike of the same class still chokes the pair already sounding,
  // leaving only the new one once the choked release has run out.
  player.on_event(0, event(sonare::midi::make_midi1_note_on(0, 0, 60, 100)));
  render(player, 9600);  // the choked release is far shorter than tail_samples()
  REQUIRE(player.active_voice_count() == 2);
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
  voice.reader.set_position(1.0e12);

  const float sample = voice.render({});

  REQUIRE(std::isfinite(sample));
  REQUIRE(voice.reader.position() < 3.0);
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

TEST_CASE("Sf2Player keeps a ringing note on the body it was struck through", "[midi][sf2][body]") {
  // A part plays a note on one program, then switches program and strikes a
  // near-silent note. The control strikes that note on another part instead, so
  // the two renders differ only in whether the part's body was handed over.
  struct Switch {
    uint8_t first_program;
    uint8_t second_program;
  };
  const Switch cases[] = {
      {0, 19},  // piano -> church organ (no body)
      {25, 0},  // steel guitar (halo) -> piano (board)
      {0, 25},  // piano -> steel guitar
  };
  const int lead = static_cast<int>(0.3 * kOutRate);
  const int window = static_cast<int>(0.1 * kOutRate);
  auto program = [](Sf2Player& p, uint8_t channel, uint8_t value) {
    p.on_event(0, event(sonare::midi::make_midi1_program_change(0, channel, value)));
  };
  auto note = [](Sf2Player& p, uint8_t channel, uint8_t key, uint8_t velocity) {
    p.on_event(0, event(sonare::midi::make_midi1_note_on(0, channel, key, velocity)));
  };
  auto window_rms = [&](uint8_t second_channel, const Switch& s) {
    Sf2Player p = make_fallback_player();
    program(p, 0, s.first_program);
    note(p, 0, 60, 100);
    render(p, lead);
    program(p, second_channel, s.second_program);
    note(p, second_channel, 72, 1);
    const StereoRender out = render(p, window);
    double acc = 0.0;
    for (size_t i = 0; i < out.left.size(); ++i) {
      acc += static_cast<double>(out.left[i]) * out.left[i] +
             static_cast<double>(out.right[i]) * out.right[i];
    }
    return std::sqrt(acc / (2.0 * static_cast<double>(out.left.size())));
  };
  for (const Switch& s : cases) {
    CAPTURE(static_cast<int>(s.first_program), static_cast<int>(s.second_program));
    const double same_part = window_rms(0, s);
    const double other_part = window_rms(1, s);
    REQUIRE(other_part > 1.0e-3);
    const double delta_db = 20.0 * std::log10(same_part / other_part);
    CAPTURE(same_part, other_part, delta_db);
    CHECK(std::abs(delta_db) < 0.1);
  }
}

namespace {

using sonare::midi::Bend32;
using sonare::midi::Control32;
using sonare::midi::MidiInstrumentSourceOutput;
using sonare::midi::Velocity16;

constexpr int kFirst50Ms = 2400;  // at kOutRate

/// Midpoint of two raw values, strictly between them for neighbouring upscale points.
uint32_t midpoint(uint32_t lo, uint32_t hi) { return lo + (hi - lo) / 2; }

const std::shared_ptr<Sf2File>& shared_fixture() {
  static const std::shared_ptr<Sf2File> sf2 = make_fixture();
  return sf2;
}

Sf2Player fixture_player() { return make_player(shared_fixture()); }

void send(Sf2Player& p, const sonare::midi::Ump& ump) { p.on_event(0, event(ump)); }

/// Absolute peak of the first 50 ms after @p setup.
template <typename Setup>
float first_50ms_peak(Setup setup) {
  Sf2Player player = fixture_player();
  setup(player);
  const StereoRender out = render(player, kFirst50Ms);
  return std::max(peak(out.left), peak(out.right));
}

/// Fundamental of a mono render in Hz: the span of all rising zero crossings, which resolves a
/// pitch step far finer than one FFT bin.
double crossing_hz(const std::vector<float>& buffer) {
  double first = -1.0;
  double last = 0.0;
  int count = 0;
  for (size_t i = 1; i < buffer.size(); ++i) {
    const float a = buffer[i - 1];
    const float b = buffer[i];
    if (a <= 0.0f && b > 0.0f) {
      const double t =
          static_cast<double>(i - 1) + static_cast<double>(a) / static_cast<double>(a - b);
      if (first < 0.0) first = t;
      last = t;
      ++count;
    }
  }
  REQUIRE(count > 2);
  return kOutRate * static_cast<double>(count - 1) / (last - first);
}

double cents_between(double hz, double reference_hz) {
  return 1200.0 * std::log2(hz / reference_hz);
}

/// Left channel of one second, after the DC blocker has settled its first blocks.
std::vector<float> render_one_second(Sf2Player& player) { return render(player, 48000).left; }

/// Sounding pitch of a note 60 (a 1 kHz sine at that key) after @p setup, in cents from 1 kHz.
template <typename Setup>
double note_60_cents(Setup setup, uint8_t note = 60, uint8_t program_select = 0) {
  Sf2Player player = fixture_player();
  if (program_select != 0) {
    send(player, sonare::midi::make_midi1_program_change(0, 0, program_select));
  }
  setup(player);
  send(player, sonare::midi::make_midi2_note_on(0, 0, note, 0xC000));
  return cents_between(crossing_hz(render_one_second(player)), 1000.0);
}

/// Renders @p player split by source track (ids 1 and 2) and returns both tracks.
std::array<std::vector<float>, 2> render_tracks(Sf2Player& player, int num_samples) {
  std::array<std::vector<float>, 2> tracks;
  std::vector<float> fallback(static_cast<size_t>(num_samples), 0.0f);
  for (auto& t : tracks) t.assign(static_cast<size_t>(num_samples), 0.0f);
  float* fallback_channels[] = {fallback.data()};
  float* one[] = {tracks[0].data()};
  float* two[] = {tracks[1].data()};
  const MidiInstrumentSourceOutput outputs[] = {{0, fallback_channels}, {1, one}, {2, two}};
  REQUIRE(player.process_source_tracks(outputs, std::size(outputs), 1, num_samples));
  return tracks;
}

MidiEvent on_track(const sonare::midi::Ump& ump, uint32_t track) {
  MidiEvent e = event(ump);
  e.source_track_id = track;
  return e;
}

}  // namespace

TEST_CASE("Sf2Player hears a MIDI 2.0 velocity between the 7-bit steps", "[midi][sf2][midi2]") {
  constexpr uint8_t kV = 64;
  const uint32_t lo = Velocity16::from7(kV).raw;
  const uint32_t hi = Velocity16::from7(kV + 1).raw;
  auto peak_at = [](uint32_t raw) {
    return first_50ms_peak([raw](Sf2Player& p) {
      send(p, sonare::midi::make_midi2_note_on(0, 0, 60, static_cast<uint16_t>(raw)));
    });
  };
  const float p_lo = peak_at(lo);
  const float p_mid = peak_at(midpoint(lo, hi));
  const float p_hi = peak_at(hi);
  CAPTURE(p_lo, p_mid, p_hi);
  REQUIRE(p_lo < p_mid);
  REQUIRE(p_mid < p_hi);
}

TEST_CASE("Sf2Player hears a 32-bit volume, expression and pressure between the 7-bit steps",
          "[midi][sf2][midi2]") {
  constexpr uint8_t kV = 100;
  const uint32_t lo = Control32::from7(kV).raw;
  const uint32_t hi = Control32::from7(kV + 1).raw;
  // Channel pressure has no destination at power-on; route it to AMPLITUDE CONTROL at -100 %
  // (40 21 22) so that more pressure means less level.
  const std::vector<uint8_t> route = [] {
    std::vector<uint8_t> msg{0xF0, 0x41, 0x10, 0x42, 0x12, 0x40, 0x21, 0x22, 0x00};
    int sum = 0x40 + 0x21 + 0x22;
    msg.push_back(static_cast<uint8_t>((128 - (sum % 128)) & 0x7F));
    msg.push_back(0xF7);
    return msg;
  }();
  for (const int which : {7, 11, -1}) {
    auto peak_at = [&](uint32_t raw) {
      return first_50ms_peak([&](Sf2Player& p) {
        if (which < 0) {
          REQUIRE(p.handle_sysex(route.data(), route.size()));
          send(p, sonare::midi::make_midi2_channel_pressure(0, 0, raw));
        } else {
          send(p, sonare::midi::make_midi2_control_change(0, 0, static_cast<uint8_t>(which), raw));
        }
        send(p, sonare::midi::make_midi1_note_on(0, 0, 60, 100));
      });
    };
    const float p_lo = peak_at(lo);
    const float p_mid = peak_at(midpoint(lo, hi));
    const float p_hi = peak_at(hi);
    CAPTURE(which, p_lo, p_mid, p_hi);
    if (which < 0) {
      REQUIRE(p_lo > p_mid);
      REQUIRE(p_mid > p_hi);
    } else {
      REQUIRE(p_lo < p_mid);
      REQUIRE(p_mid < p_hi);
    }
  }
}

TEST_CASE("Sf2Player hears a 32-bit modulation wheel between the 7-bit steps",
          "[midi][sf2][midi2]") {
  // Route part 1's wheel to AMPLITUDE CONTROL at -100 % (40 21 02) so that more wheel means less
  // level.
  constexpr uint8_t kV = 64;
  const uint32_t lo = Control32::from7(kV).raw;
  const uint32_t hi = Control32::from7(kV + 1).raw;
  std::vector<uint8_t> route{0xF0, 0x41, 0x10, 0x42, 0x12, 0x40, 0x21, 0x02, 0x00};
  route.push_back(static_cast<uint8_t>((128 - ((0x40 + 0x21 + 0x02) % 128)) & 0x7F));
  route.push_back(0xF7);
  auto peak_at = [&](uint32_t raw) {
    return first_50ms_peak([&](Sf2Player& p) {
      REQUIRE(p.handle_sysex(route.data(), route.size()));
      send(p, sonare::midi::make_midi2_control_change(0, 0, 1, raw));
      send(p, sonare::midi::make_midi1_note_on(0, 0, 60, 100));
    });
  };
  const float p_lo = peak_at(lo);
  const float p_mid = peak_at(midpoint(lo, hi));
  const float p_hi = peak_at(hi);
  CAPTURE(p_lo, p_mid, p_hi);
  REQUIRE(p_lo > p_mid);
  REQUIRE(p_mid > p_hi);
}

TEST_CASE("Sf2Player hears a 32-bit bend between the 14-bit steps", "[midi][sf2][midi2]") {
  // One 14-bit step at the default 2-semitone range is 0.024 cents; the pitch is read as the span
  // of a second of rising zero crossings.
  constexpr uint16_t kBend = 12000;
  const uint32_t lo = Bend32::from14(kBend).raw;
  const uint32_t hi = Bend32::from14(kBend + 1).raw;
  auto cents_at = [](uint32_t raw) {
    return note_60_cents(
        [raw](Sf2Player& p) { send(p, sonare::midi::make_midi2_pitch_bend(0, 0, raw)); });
  };
  const double c_lo = cents_at(lo);
  const double c_mid = cents_at(midpoint(lo, hi));
  const double c_hi = cents_at(hi);
  CAPTURE(c_lo, c_mid, c_hi);
  REQUIRE(c_lo < c_mid);
  REQUIRE(c_mid < c_hi);
}

TEST_CASE("Sf2Player per-note pitch bend moves only its own note", "[midi][sf2][midi2]") {
  // Two notes on one channel. A per-note bend on 60 retunes 60 and leaves 67 exactly as if it
  // sounded alone: the mix is linear, so pair == bent 60 alone + 67 alone.
  constexpr int kLen = 16384;
  constexpr uint32_t kUpOneSemitone = 0xC0000000u;  // half of the default 2 semitones
  const auto strike = [&](bool n60, bool n67) {
    Sf2Player player = fixture_player();
    if (n60) send(player, sonare::midi::make_midi2_note_on(0, 0, 60, 0xC000));
    if (n67) send(player, sonare::midi::make_midi2_note_on(0, 0, 67, 0xC000));
    send(player, sonare::midi::make_midi2_per_note_pitch_bend(0, 0, 60, kUpOneSemitone));
    return render(player, kLen).left;
  };
  const std::vector<float> both = strike(true, true);
  const std::vector<float> bent = strike(true, false);
  const std::vector<float> alone = strike(false, true);
  float worst = 0.0f;
  for (size_t i = 0; i < both.size(); ++i) {
    worst = std::max(worst, std::fabs(both[i] - (bent[i] + alone[i])));
  }
  CAPTURE(worst);
  REQUIRE(worst < 1.0e-5f);
  REQUIRE(peak(alone) > 0.1f);

  const double cents = cents_between(crossing_hz(bent), 1000.0);
  CAPTURE(cents);
  REQUIRE(std::fabs(cents - 100.0) < 5.0);
}

TEST_CASE("Sf2Player detaches a voice on Per-Note Management D=1", "[midi][sf2][midi2]") {
  constexpr int kLen = 16384;
  constexpr uint32_t kUpOneSemitone = 0xC0000000u;
  constexpr uint32_t kFullUp = 0xFFFFFFFFu;
  auto render_60 = [&](bool detach_then_rebend) {
    Sf2Player player = fixture_player();
    player.on_event(0, on_track(sonare::midi::make_midi2_note_on(0, 0, 60, 0xC000), 1));
    send(player, sonare::midi::make_midi2_per_note_pitch_bend(0, 0, 60, kUpOneSemitone));
    if (detach_then_rebend) {
      send(player, sonare::midi::make_midi2_per_note_management(0, 0, 60, true, false));
      send(player, sonare::midi::make_midi2_per_note_pitch_bend(0, 0, 60, kFullUp));
    }
    return render_tracks(player, kLen)[0];
  };
  // The detached voice keeps the bend it had when it was detached.
  REQUIRE(render_60(true) == render_60(false));

  // The row itself took the new bend, so the next note on the key sounds it.
  const double next_note = note_60_cents([](Sf2Player& p) {
    send(p, sonare::midi::make_midi2_per_note_pitch_bend(0, 0, 60, 0xC0000000u));
    send(p, sonare::midi::make_midi2_per_note_management(0, 0, 60, true, false));
    send(p, sonare::midi::make_midi2_per_note_pitch_bend(0, 0, 60, 0xFFFFFFFFu));
  });
  CAPTURE(next_note);
  REQUIRE(std::fabs(next_note - 200.0) < 5.0);

  // S=1 returns the row to unset, so the next note is in tune.
  const double after_reset = note_60_cents([](Sf2Player& p) {
    send(p, sonare::midi::make_midi2_per_note_pitch_bend(0, 0, 60, 0xFFFFFFFFu));
    send(p, sonare::midi::make_midi2_per_note_management(0, 0, 60, false, true));
  });
  CAPTURE(after_reset);
  REQUIRE(std::fabs(after_reset) < 5.0);
}

TEST_CASE("Sf2Player takes a note's absolute pitch from RPNC #3 and attribute #3",
          "[midi][sf2][midi2]") {
  // RPNC #3 Pitch 7.25 on key 60 makes it sound 72; attribute #3 Pitch 7.9 on the note-on
  // outranks it (M2-104-UM §7.4.15).
  const double rpnc = note_60_cents([](Sf2Player& p) {
    send(p, sonare::midi::make_midi2_per_note_controller(0, 0, 60, 3, 72u << 25));
  });
  CAPTURE(rpnc);
  REQUIRE(std::fabs(rpnc - 1200.0) < 5.0);

  Sf2Player player = fixture_player();
  send(player, sonare::midi::make_midi2_per_note_controller(0, 0, 60, 3, 72u << 25));
  send(player, sonare::midi::make_midi2_note_on(0, 0, 60, 0xC000, 3, 67 * 512));
  const double attribute = cents_between(crossing_hz(render_one_second(player)), 1000.0);
  CAPTURE(attribute);
  REQUIRE(std::fabs(attribute - 700.0) < 5.0);
}

TEST_CASE("Sf2Player returns a sounding absolute-pitch voice to the composed pitch on S=1",
          "[midi][sf2][midi2]") {
  // RPNC #3 makes key 60 sound 72; resetting the row while the voice sounds drops its pitch back
  // to key 60 rather than leaving it at the zone key it was started on.
  const double after = note_60_cents([](Sf2Player& p) {
    send(p, sonare::midi::make_midi2_per_note_controller(0, 0, 60, 3, 72u << 25));
    send(p, sonare::midi::make_midi2_note_on(0, 0, 60, 0xC000));
    (void)render_one_second(p);
    send(p, sonare::midi::make_midi2_per_note_management(0, 0, 60, false, true));
  });
  CAPTURE(after);
  REQUIRE(std::fabs(after) < 5.0);

  // An attribute #3 pitch survives the reset, and the voice stays on it.
  Sf2Player player = fixture_player();
  send(player, sonare::midi::make_midi2_note_on(0, 0, 60, 0xC000, 3, 67 * 512));
  (void)render_one_second(player);
  send(player, sonare::midi::make_midi2_per_note_management(0, 0, 60, false, true));
  const double attribute = cents_between(crossing_hz(render_one_second(player)), 1000.0);
  CAPTURE(attribute);
  REQUIRE(std::fabs(attribute - 700.0) < 5.0);
}

TEST_CASE("Sf2Player picks the sample zone by the integer part of an absolute pitch",
          "[midi][sf2][midi2]") {
  // Preset 3 covers keys 0-48 only. Key 60 with Pitch 7.25 = 40 plays that preset's sample at
  // key 40 (about 315 Hz); by the note number it would have fallen out of the zone.
  const double cents = note_60_cents(
      [](Sf2Player& p) {
        send(p, sonare::midi::make_midi2_per_note_controller(0, 0, 60, 3, 40u << 25));
      },
      60, 3);
  const double expect = 100.0 * (40 - 60);
  CAPTURE(cents, expect);
  REQUIRE(std::fabs(cents - expect) < 5.0);
}

TEST_CASE("Sf2Player scales per-note bend by RC 0/7, absolute and relative", "[midi][sf2][midi2]") {
  const double absolute = note_60_cents([](Sf2Player& p) {
    send(p, sonare::midi::make_midi2_registered_controller(0, 0, 0, 7, 12u << 25));
    send(p, sonare::midi::make_midi2_per_note_pitch_bend(0, 0, 60, 0xFFFFFFFFu));
  });
  // Relative +10 semitones on the default 2.
  const double relative = note_60_cents([](Sf2Player& p) {
    send(p, sonare::midi::make_midi2_relative_registered_controller(0, 0, 0, 7, 10u << 25));
    send(p, sonare::midi::make_midi2_per_note_pitch_bend(0, 0, 60, 0xFFFFFFFFu));
  });
  // A delta far below zero saturates at 0 semitones instead of wrapping.
  const double saturated = note_60_cents([](Sf2Player& p) {
    send(p, sonare::midi::make_midi2_relative_registered_controller(0, 0, 0, 7, 0x80000000u));
    send(p, sonare::midi::make_midi2_per_note_pitch_bend(0, 0, 60, 0xFFFFFFFFu));
  });
  CAPTURE(absolute, relative, saturated);
  REQUIRE(std::fabs(absolute - 1200.0) < 5.0);
  REQUIRE(std::fabs(relative - 1200.0) < 5.0);
  REQUIRE(std::fabs(saturated) < 5.0);
}

TEST_CASE("Sf2Player keeps per-note pitch across Reset All Controllers", "[midi][sf2][midi2]") {
  const double cents = note_60_cents([](Sf2Player& p) {
    send(p, sonare::midi::make_midi2_per_note_pitch_bend(0, 0, 60, 0xC0000000u));
    send(p, sonare::midi::make_midi1_control_change(0, 0, 121, 0));
  });
  CAPTURE(cents);
  REQUIRE(std::fabs(cents - 100.0) < 5.0);
}

TEST_CASE("Sf2Player takes the channel bend range from a MIDI 2.0 Registered Controller",
          "[midi][sf2][midi2]") {
  // RC 0/0 carries RPN 0/0 as one message with the semitones in the top seven bits.
  const double absolute = note_60_cents([](Sf2Player& p) {
    send(p, sonare::midi::make_midi2_registered_controller(0, 0, 0, 0, 12u << 25));
    send(p, sonare::midi::make_midi1_pitch_bend(0, 0, 16383));
  });
  // +10 semitones on the default 2.
  const double relative = note_60_cents([](Sf2Player& p) {
    send(p, sonare::midi::make_midi2_relative_registered_controller(0, 0, 0, 0, 10u << 25));
    send(p, sonare::midi::make_midi1_pitch_bend(0, 0, 16383));
  });
  const double untouched = note_60_cents(
      [](Sf2Player& p) { send(p, sonare::midi::make_midi1_pitch_bend(0, 0, 16383)); });
  CAPTURE(absolute, relative, untouched);
  REQUIRE(std::fabs(absolute - 1200.0) < 5.0);
  REQUIRE(std::fabs(relative - 1200.0) < 5.0);
  REQUIRE(std::fabs(untouched - 200.0) < 5.0);
}

TEST_CASE("Sf2Player moves the master tuning by a relative RC 0/1 and 0/2", "[midi][sf2][midi2]") {
  Sf2Player player = fixture_player();
  // Coarse tuning: +3 semitones on the centre.
  send(player, sonare::midi::make_midi2_relative_registered_controller(0, 0, 0, 2, 3u << 25));
  REQUIRE(player.pitch_coarse_tune(0) == 3);
  // Fine tuning: half of the 14-bit range up from the centre, saturating at full scale.
  send(player, sonare::midi::make_midi2_relative_registered_controller(0, 0, 0, 1, 0x7FFFFFFFu));
  REQUIRE(player.pitch_fine_tune(0) == 16383);
}

TEST_CASE("Sf2Player bends a synth-fallback voice per note", "[midi][sf2][midi2]") {
  // No SoundFont: the note plays the data-free model. The bend must reach it as well.
  auto fundamental = [](bool bend) {
    Sf2Player player = make_fallback_player();
    send(player, sonare::midi::make_midi1_program_change(0, 0, 80));
    if (bend) send(player, sonare::midi::make_midi2_per_note_pitch_bend(0, 0, 69, 0xFFFFFFFFu));
    send(player, sonare::midi::make_midi2_note_on(0, 0, 69, 0xC000));
    const double hint = bend ? 493.88 : 440.0;
    return sonare::test::fft_fundamental(render_one_second(player), 4096, hint);
  };
  const double bent = cents_between(fundamental(true), fundamental(false));
  CAPTURE(bent);
  REQUIRE(std::fabs(bent - 200.0) < 10.0);
}

TEST_CASE("Sf2Player counts what it decodes and does not realise", "[midi][sf2][midi2]") {
  Sf2Player player = fixture_player();
  REQUIRE(player.skipped_event_count() == 0);
  // Reserved MIDI 2.0 status 0x7.
  sonare::midi::Ump reserved = sonare::midi::make_midi2_channel_pressure(0, 0, 0);
  reserved.words[0] = (reserved.words[0] & 0xFF0FFFFFu) | 0x00700000u;
  send(player, reserved);
  // A per-note controller other than pitch, and an assignable one.
  send(player, sonare::midi::make_midi2_per_note_controller(0, 0, 60, 7, 0x80000000u));
  send(player, sonare::midi::make_midi2_assignable_per_note_controller(0, 0, 60, 1, 0));
  // Relative on a parameter this player does not hold.
  send(player, sonare::midi::make_midi2_relative_registered_controller(0, 0, 0, 3, 1u << 25));
  send(player, sonare::midi::make_midi2_relative_assignable_controller(0, 0, 3, 4, 1u << 25));
  REQUIRE(player.skipped_event_count() == 5);
  // Pitch per-note and the held controllers are realised, not counted.
  send(player, sonare::midi::make_midi2_per_note_controller(0, 0, 60, 3, 60u << 25));
  send(player, sonare::midi::make_midi2_per_note_pitch_bend(0, 0, 60, 0x80000000u));
  send(player, sonare::midi::make_midi2_registered_controller(0, 0, 0, 7, 2u << 25));
  send(player, sonare::midi::make_midi2_relative_registered_controller(0, 0, 0, 0, 1u << 25));
  REQUIRE(player.skipped_event_count() == 5);
  player.reset();
  REQUIRE(player.skipped_event_count() == 0);
}
