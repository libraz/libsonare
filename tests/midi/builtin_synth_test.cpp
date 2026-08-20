/// @file builtin_synth_test.cpp
/// @brief MIDI core: the minimal built-in synth's channel-mode handling —
///        CC#123 (All Notes Off) releases voices and CC#120 (All Sound Off)
///        silences them immediately.

#include "midi/builtin_synth.h"

#include <array>
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <cstdint>
#include <vector>

#include "midi/midi_event.h"
#include "midi/ump.h"

namespace {

using sonare::midi::BuiltinSynth;
using sonare::midi::BuiltinSynthConfig;
using sonare::midi::MidiEvent;
using sonare::midi::MidiInstrumentSourceOutput;

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

// Renders `num_samples` frames into `num_channels` planar buffers and returns
// the per-channel RMS. Used for the mix controllers, whose effect is a level
// ratio rather than a change in the waveform.
std::vector<float> render_rms(BuiltinSynth* synth, int num_channels, int num_samples) {
  std::vector<std::vector<float>> buffers(
      static_cast<size_t>(num_channels),
      std::vector<float>(static_cast<size_t>(num_samples), 0.0f));
  std::vector<float*> channels;
  for (auto& buffer : buffers) channels.push_back(buffer.data());
  synth->process(channels.data(), num_channels, num_samples);
  std::vector<float> rms;
  for (const auto& buffer : buffers) {
    double sum = 0.0;
    for (float s : buffer) sum += static_cast<double>(s) * static_cast<double>(s);
    rms.push_back(static_cast<float>(std::sqrt(sum / static_cast<double>(num_samples))));
  }
  return rms;
}

// The concave controller curve shared with the SF2 / native voices.
float cc_gain(int value) {
  const float v = static_cast<float>(value) / 127.0f;
  return v * v;
}

MidiEvent control_change(int controller, int value) {
  return event(sonare::midi::make_midi1_control_change(0, 0, static_cast<uint8_t>(controller),
                                                       static_cast<uint8_t>(value)));
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

TEST_CASE("BuiltinSynth Reset All Controllers recenters pitch bend and clears pressure",
          "[midi][synth]") {
  constexpr double kSampleRate = 48000.0;
  BuiltinSynth synth(BuiltinSynthConfig{});  // Default sine: clean zero crossings.
  synth.prepare(kSampleRate, 0);

  synth.on_event(0, event(sonare::midi::make_midi1_note_on(0, 0, 69, 100)));  // A4.
  render_peak(&synth, 4096);  // Settle into sustain.
  const float centered = estimate_freq(&synth, 24000, kSampleRate);
  const float level_before = render_peak(&synth, 512);
  REQUIRE(centered == Catch::Approx(440.0f).margin(4.0f));
  REQUIRE(level_before > 0.0f);

  // Apply a full upward bend and full channel pressure: pitch rises, level rises.
  synth.on_event(0, event(sonare::midi::make_midi1_pitch_bend(0, 0, 16383)));
  synth.on_event(0, event(sonare::midi::make_midi1_channel_pressure(0, 0, 127)));
  REQUIRE(estimate_freq(&synth, 24000, kSampleRate) > centered * 1.10f);
  REQUIRE(render_peak(&synth, 512) > level_before * 1.5f);

  // Reset All Controllers must recenter pitch AND drop the residual pressure.
  synth.on_event(0, event(sonare::midi::make_midi1_control_change(0, 0, 121, 0)));
  REQUIRE(estimate_freq(&synth, 24000, kSampleRate) == Catch::Approx(centered).margin(4.0f));
  REQUIRE(render_peak(&synth, 512) == Catch::Approx(level_before).epsilon(0.05));
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

TEST_CASE("BuiltinSynth tracks CC11 expression over a sustained note", "[midi][synth]") {
  constexpr int kSettle = 8192;  // Past attack + decay, into the sustain stage.
  constexpr int kWindow = 4800;  // Whole periods enough for a steady RMS.
  BuiltinSynth synth(BuiltinSynthConfig{});
  synth.prepare(48000.0, 0);

  synth.on_event(0, event(sonare::midi::make_midi1_note_on(0, 0, 60, 100)));
  render_rms(&synth, 1, kSettle);
  // Reference level at the power-on expression (CC11 == 127).
  const float reference = render_rms(&synth, 1, kWindow)[0];
  REQUIRE(reference > 0.0f);

  // A fade-out: each step's level must follow the controller, not stay flat.
  float previous = reference;
  for (int value : {96, 64, 32}) {
    synth.on_event(0, control_change(11, value));
    const float level = render_rms(&synth, 1, kWindow)[0];
    REQUIRE(level == Catch::Approx(reference * cc_gain(value)).epsilon(0.03));
    REQUIRE(level < previous);
    previous = level;
  }

  // Expression 0 mutes the part while the note is still held.
  synth.on_event(0, control_change(11, 0));
  REQUIRE(render_rms(&synth, 1, kWindow)[0] == 0.0f);

  // Restoring expression brings the same note back at the reference level.
  synth.on_event(0, control_change(11, 127));
  REQUIRE(render_rms(&synth, 1, kWindow)[0] == Catch::Approx(reference).epsilon(0.03));
}

TEST_CASE("BuiltinSynth balances parts with CC7 volume", "[midi][synth]") {
  constexpr int kSettle = 8192;
  constexpr int kWindow = 4800;
  BuiltinSynth synth(BuiltinSynthConfig{});
  synth.prepare(48000.0, 0);

  synth.on_event(0, event(sonare::midi::make_midi1_note_on(0, 0, 60, 100)));
  render_rms(&synth, 1, kSettle);
  const float reference = render_rms(&synth, 1, kWindow)[0];
  REQUIRE(reference > 0.0f);

  // Volume and expression are independent gains through the same curve.
  synth.on_event(0, control_change(7, 50));
  REQUIRE(render_rms(&synth, 1, kWindow)[0] ==
          Catch::Approx(reference * cc_gain(50) / cc_gain(100)).epsilon(0.03));

  synth.on_event(0, control_change(11, 64));
  REQUIRE(render_rms(&synth, 1, kWindow)[0] ==
          Catch::Approx(reference * cc_gain(50) / cc_gain(100) * cc_gain(64)).epsilon(0.03));

  // Reset All Controllers restores expression but keeps the mix settings, as
  // MIDI RP-015 requires.
  synth.on_event(0, control_change(121, 0));
  REQUIRE(render_rms(&synth, 1, kWindow)[0] ==
          Catch::Approx(reference * cc_gain(50) / cc_gain(100)).epsilon(0.03));
}

TEST_CASE("BuiltinSynth places a part in the stereo field with CC10 pan", "[midi][synth]") {
  constexpr int kSettle = 8192;
  constexpr int kWindow = 4800;
  BuiltinSynth synth(BuiltinSynthConfig{});
  synth.prepare(48000.0, 0);

  synth.on_event(0, event(sonare::midi::make_midi1_note_on(0, 0, 60, 100)));
  render_rms(&synth, 2, kSettle);

  // Power-on pan is centre: both legs carry the same level.
  const std::vector<float> centre = render_rms(&synth, 2, kWindow);
  REQUIRE(centre[0] > 0.0f);
  REQUIRE(centre[0] == Catch::Approx(centre[1]).epsilon(0.001));

  synth.on_event(0, control_change(10, 0));
  const std::vector<float> left = render_rms(&synth, 2, kWindow);
  REQUIRE(left[0] > centre[0]);
  REQUIRE(left[1] < left[0] * 0.01f);

  synth.on_event(0, control_change(10, 127));
  const std::vector<float> right = render_rms(&synth, 2, kWindow);
  REQUIRE(right[1] > centre[1]);
  REQUIRE(right[0] < right[1] * 0.01f);

  // Constant power: the pair carries the same energy wherever it is placed.
  REQUIRE(left[0] * left[0] + left[1] * left[1] ==
          Catch::Approx(centre[0] * centre[0] + centre[1] * centre[1]).epsilon(0.02));

  // A second part can sit on the opposite side of the same synth.
  synth.on_event(0, event(sonare::midi::make_midi1_note_on(0, 1, 67, 100)));
  synth.on_event(0, control_change(10, 0));  // Channel 0 stays hard left.
  synth.on_event(0, event(sonare::midi::make_midi1_control_change(0, 1, 10, 127)));
  render_rms(&synth, 2, kSettle);
  const std::vector<float> split = render_rms(&synth, 2, kWindow);
  REQUIRE(split[0] > 0.0f);
  REQUIRE(split[1] > 0.0f);
}

TEST_CASE("BuiltinSynth renders shared-pool voices into their source tracks", "[midi][synth]") {
  BuiltinSynthConfig config;
  config.polyphony = 2;
  BuiltinSynth split(config);
  BuiltinSynth reference(config);
  split.prepare(48000.0, 256);
  reference.prepare(48000.0, 256);

  MidiEvent first = event(sonare::midi::make_midi1_note_on(0, 0, 60, 100));
  first.source_track_id = 101;
  MidiEvent second = event(sonare::midi::make_midi1_note_on(0, 0, 67, 100));
  second.source_track_id = 202;
  split.on_event(7, first);
  split.on_event(7, second);
  first.source_track_id = 0;
  second.source_track_id = 0;
  reference.on_event(7, first);
  reference.on_event(7, second);

  std::array<float, 256> fallback{};
  std::array<float, 256> first_track{};
  std::array<float, 256> second_track{};
  std::array<float, 256> combined{};
  float* fallback_channels[] = {fallback.data()};
  float* first_channels[] = {first_track.data()};
  float* second_channels[] = {second_track.data()};
  float* combined_channels[] = {combined.data()};
  const MidiInstrumentSourceOutput outputs[] = {
      {0, fallback_channels}, {101, first_channels}, {202, second_channels}};

  REQUIRE(split.process_source_tracks(outputs, std::size(outputs), 1, 256));
  reference.process(combined_channels, 1, 256);
  float first_peak = 0.0f;
  float second_peak = 0.0f;
  for (size_t i = 0; i < combined.size(); ++i) {
    first_peak = std::max(first_peak, std::abs(first_track[i]));
    second_peak = std::max(second_peak, std::abs(second_track[i]));
    REQUIRE(fallback[i] == 0.0f);
    REQUIRE(first_track[i] + second_track[i] == Catch::Approx(combined[i]).margin(1.0e-6f));
  }
  REQUIRE(first_peak > 0.0f);
  REQUIRE(second_peak > 0.0f);
}
