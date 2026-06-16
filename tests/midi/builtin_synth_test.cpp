/// @file builtin_synth_test.cpp
/// @brief MIDI core: the minimal built-in synth's channel-mode handling —
///        CC#123 (All Notes Off) releases voices and CC#120 (All Sound Off)
///        silences them immediately.

#include "midi/builtin_synth.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <vector>

#include "midi/midi_event.h"
#include "midi/ump.h"

namespace {

using sonare::midi::BuiltinSynth;
using sonare::midi::BuiltinSynthConfig;
using sonare::midi::MidiEvent;

MidiEvent event(const sonare::midi::Ump& ump) {
  MidiEvent e;
  e.ump = ump;
  return e;
}

// Renders `num_samples` mono frames and returns the peak absolute amplitude.
float render_peak(BuiltinSynth* synth, int num_samples) {
  std::vector<float> buffer(static_cast<size_t>(num_samples), 0.0f);
  float* channels[1] = {buffer.data()};
  synth->process(channels, 1, num_samples);
  float peak = 0.0f;
  for (float s : buffer) peak = std::max(peak, std::fabs(s));
  return peak;
}

// Estimates the fundamental of the (sine) output by counting rising zero
// crossings over the rendered window: each crossing marks one period.
float estimate_freq(BuiltinSynth* synth, int num_samples, double sample_rate) {
  std::vector<float> buffer(static_cast<size_t>(num_samples), 0.0f);
  float* channels[1] = {buffer.data()};
  synth->process(channels, 1, num_samples);
  int crossings = 0;
  for (int i = 1; i < num_samples; ++i) {
    if (buffer[static_cast<size_t>(i) - 1] <= 0.0f && buffer[static_cast<size_t>(i)] > 0.0f) {
      ++crossings;
    }
  }
  return static_cast<float>(crossings) * static_cast<float>(sample_rate) /
         static_cast<float>(num_samples);
}

}  // namespace

TEST_CASE("BuiltinSynth CC#123 (All Notes Off) releases sounding voices", "[midi][synth]") {
  BuiltinSynthConfig config;
  config.release_ms = 5.0f;  // Short release so the tail decays quickly.
  BuiltinSynth synth(config);
  synth.prepare(48000.0, 0);

  synth.on_event(0, event(sonare::midi::make_midi1_note_on(0, 0, 60, 100)));
  REQUIRE(render_peak(&synth, 256) > 0.0f);  // The note is sounding.

  // A bare CC#123 with no accompanying note-off must still silence the synth.
  synth.on_event(0, event(sonare::midi::make_midi1_control_change(0, 0, 123, 0)));
  // After the (short) release tail, output must reach silence.
  render_peak(&synth, 2048);
  REQUIRE(render_peak(&synth, 256) == 0.0f);
}

TEST_CASE("BuiltinSynth ignores events before prepare without poisoning later render",
          "[midi][synth]") {
  BuiltinSynth synth({});

  synth.on_event(0, event(sonare::midi::make_midi1_note_on(0, 0, 60, 100)));
  synth.on_event(0, event(sonare::midi::make_midi1_note_off(0, 0, 60, 64)));
  synth.on_event(0, event(sonare::midi::make_midi1_control_change(0, 0, 123, 0)));
  synth.on_event(0, event(sonare::midi::make_midi1_control_change(0, 0, 120, 0)));

  synth.prepare(48000.0, 128);
  REQUIRE(render_peak(&synth, 256) == 0.0f);

  synth.on_event(0, event(sonare::midi::make_midi1_note_on(0, 0, 60, 100)));
  REQUIRE(render_peak(&synth, 256) > 0.0f);
}

TEST_CASE("BuiltinSynth CC#120 (All Sound Off) silences voices immediately", "[midi][synth]") {
  BuiltinSynthConfig config;
  config.release_ms = 2000.0f;  // Long release: only an immediate kill silences it fast.
  BuiltinSynth synth(config);
  synth.prepare(48000.0, 0);

  synth.on_event(0, event(sonare::midi::make_midi1_note_on(0, 0, 64, 100)));
  REQUIRE(render_peak(&synth, 256) > 0.0f);

  synth.on_event(0, event(sonare::midi::make_midi1_control_change(0, 0, 120, 0)));
  // No release tail — the very next block is already silent.
  REQUIRE(render_peak(&synth, 256) == 0.0f);
}

TEST_CASE("BuiltinSynth CC#64 holds released notes until pedal is lifted", "[midi][synth]") {
  BuiltinSynthConfig config;
  config.release_ms = 5.0f;
  BuiltinSynth synth(config);
  synth.prepare(48000.0, 0);

  synth.on_event(0, event(sonare::midi::make_midi1_note_on(0, 0, 60, 100)));
  REQUIRE(render_peak(&synth, 256) > 0.0f);

  synth.on_event(0, event(sonare::midi::make_midi1_control_change(0, 0, 64, 127)));
  synth.on_event(0, event(sonare::midi::make_midi1_note_off(0, 0, 60, 0)));

  render_peak(&synth, 2048);
  REQUIRE(render_peak(&synth, 256) > 0.0f);

  synth.on_event(0, event(sonare::midi::make_midi1_control_change(0, 0, 64, 0)));
  render_peak(&synth, 2048);
  REQUIRE(render_peak(&synth, 256) == 0.0f);
}

TEST_CASE("BuiltinSynth Reset All Controllers lifts sustain pedal", "[midi][synth]") {
  BuiltinSynthConfig config;
  config.release_ms = 5.0f;
  BuiltinSynth synth(config);
  synth.prepare(48000.0, 0);

  synth.on_event(0, event(sonare::midi::make_midi1_note_on(0, 0, 60, 100)));
  REQUIRE(render_peak(&synth, 256) > 0.0f);
  synth.on_event(0, event(sonare::midi::make_midi1_control_change(0, 0, 64, 127)));
  synth.on_event(0, event(sonare::midi::make_midi1_note_off(0, 0, 60, 0)));

  synth.on_event(0, event(sonare::midi::make_midi1_control_change(0, 0, 121, 0)));
  render_peak(&synth, 2048);
  REQUIRE(render_peak(&synth, 256) == 0.0f);
}

TEST_CASE("BuiltinSynth pitch bend retunes sounding voices on the channel", "[midi][synth]") {
  constexpr double kSampleRate = 48000.0;
  BuiltinSynthConfig config;  // Default sine waveform: clean zero crossings.
  BuiltinSynth synth(config);
  synth.prepare(kSampleRate, 0);

  synth.on_event(0, event(sonare::midi::make_midi1_note_on(0, 0, 69, 100)));  // A4 = 440 Hz.
  render_peak(&synth, 2048);  // Settle the envelope.
  const float centered = estimate_freq(&synth, 24000, kSampleRate);
  REQUIRE(centered == Catch::Approx(440.0f).margin(4.0f));

  // Full upward bend (+2 semitones) raises the pitch by ~12.2%.
  synth.on_event(0, event(sonare::midi::make_midi1_pitch_bend(0, 0, 16383)));
  const float bent_up = estimate_freq(&synth, 24000, kSampleRate);
  REQUIRE(bent_up > centered * 1.10f);

  // Full downward bend (-2 semitones) lowers it by ~11%.
  synth.on_event(0, event(sonare::midi::make_midi1_pitch_bend(0, 0, 0)));
  const float bent_down = estimate_freq(&synth, 24000, kSampleRate);
  REQUIRE(bent_down < centered * 0.92f);

  // Returning to center restores the original pitch.
  synth.on_event(0, event(sonare::midi::make_midi1_pitch_bend(0, 0, 8192)));
  const float restored = estimate_freq(&synth, 24000, kSampleRate);
  REQUIRE(restored == Catch::Approx(centered).margin(4.0f));
}

TEST_CASE("BuiltinSynth MIDI 2.0 pitch bend retunes voices", "[midi][synth]") {
  constexpr double kSampleRate = 48000.0;
  BuiltinSynth synth(BuiltinSynthConfig{});
  synth.prepare(kSampleRate, 0);

  synth.on_event(0, event(sonare::midi::make_midi2_note_on(0, 0, 69, 0x8000)));
  render_peak(&synth, 2048);
  const float centered = estimate_freq(&synth, 24000, kSampleRate);

  synth.on_event(0, event(sonare::midi::make_midi2_pitch_bend(0, 0, 0xFFFFFFFFu)));
  const float bent_up = estimate_freq(&synth, 24000, kSampleRate);
  REQUIRE(bent_up > centered * 1.10f);
}

TEST_CASE("BuiltinSynth channel pressure boosts amplitude per channel", "[midi][synth]") {
  BuiltinSynth synth(BuiltinSynthConfig{});
  synth.prepare(48000.0, 0);

  synth.on_event(0, event(sonare::midi::make_midi1_note_on(0, 0, 60, 100)));
  render_peak(&synth, 4096);  // Settle into sustain.
  const float before = render_peak(&synth, 512);
  REQUIRE(before > 0.0f);

  synth.on_event(0, event(sonare::midi::make_midi1_channel_pressure(0, 0, 127)));
  const float after = render_peak(&synth, 512);
  REQUIRE(after > before * 1.5f);

  // Releasing pressure restores the original level.
  synth.on_event(0, event(sonare::midi::make_midi1_channel_pressure(0, 0, 0)));
  const float released = render_peak(&synth, 512);
  REQUIRE(released == Catch::Approx(before).epsilon(0.05));
}

TEST_CASE("BuiltinSynth poly pressure boosts only the addressed note", "[midi][synth]") {
  BuiltinSynth synth(BuiltinSynthConfig{});
  synth.prepare(48000.0, 0);

  synth.on_event(0, event(sonare::midi::make_midi1_note_on(0, 0, 60, 100)));
  render_peak(&synth, 4096);
  const float before = render_peak(&synth, 512);
  REQUIRE(before > 0.0f);

  synth.on_event(0, event(sonare::midi::make_midi1_poly_pressure(0, 0, 60, 127)));
  const float after = render_peak(&synth, 512);
  REQUIRE(after > before * 1.5f);
}

TEST_CASE("BuiltinSynth All Notes Off only affects the addressed channel", "[midi][synth]") {
  BuiltinSynthConfig config;
  config.release_ms = 5.0f;
  BuiltinSynth synth(config);
  synth.prepare(48000.0, 0);

  synth.on_event(0, event(sonare::midi::make_midi1_note_on(0, 0, 60, 100)));
  synth.on_event(0, event(sonare::midi::make_midi1_note_on(0, 1, 67, 100)));

  // All Sound Off on channel 0 must leave channel 1's note sounding.
  synth.on_event(0, event(sonare::midi::make_midi1_control_change(0, 0, 120, 0)));
  REQUIRE(render_peak(&synth, 256) > 0.0f);
}
