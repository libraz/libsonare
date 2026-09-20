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
#include "midi/mpe.h"
#include "midi/ump.h"
#include "support/audio_fixtures.h"
#include "support/golden_hash.h"
#include "support/midi_render.h"

namespace {

using sonare::midi::BuiltinSynth;
using sonare::midi::BuiltinSynthConfig;
using sonare::midi::MidiEvent;
using sonare::midi::MidiInstrumentSourceOutput;

using sonare::test::event;

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

namespace {

using sonare::midi::MpeChannelRole;
using sonare::midi::MpeState;

constexpr double kMpeRate = 48000.0;
constexpr uint8_t kMpeVelocity = 100;

BuiltinSynth mpe_synth() {
  BuiltinSynthConfig cfg;
  cfg.sustain = 1.0f;
  cfg.attack_ms = 1.0f;
  cfg.decay_ms = 1.0f;
  BuiltinSynth synth(cfg);
  synth.prepare(kMpeRate, 256);
  return synth;
}

void mpe_send(BuiltinSynth& synth, const sonare::midi::Ump& ump) {
  synth.on_event(0, sonare::test::event(ump));
}

/// The MPE Configuration Message: RPN 00 06 with the member count in Data Entry
/// MSB.
void mpe_send_mcm(BuiltinSynth& synth, uint8_t manager_channel, uint8_t members) {
  mpe_send(synth, sonare::midi::make_midi1_control_change(0, manager_channel, 101, 0));
  mpe_send(synth, sonare::midi::make_midi1_control_change(0, manager_channel, 100, 6));
  mpe_send(synth, sonare::midi::make_midi1_control_change(0, manager_channel, 6, members));
}

/// Sounding frequency of the single note on @p channel, in cents from the
/// unbent note 60.
double mpe_cents(BuiltinSynth& synth, uint8_t channel, double expect_cents) {
  mpe_send(synth, sonare::midi::make_midi1_note_on(0, channel, 60, kMpeVelocity));
  std::vector<float> buffer(16384, 0.0f);
  float* channels[1] = {buffer.data()};
  synth.process(channels, 1, static_cast<int>(buffer.size()));
  constexpr double kC4Hz = 261.6255653;
  const double hz =
      sonare::test::fft_fundamental(buffer, 4096, kC4Hz * std::pow(2.0, expect_cents / 1200.0));
  return 1200.0 * std::log2(hz / kC4Hz);
}

}  // namespace

TEST_CASE("BuiltinSynth reads a bend through the zone's range once an MCM arrives",
          "[midi][synth][mpe]") {
  // The same bend message, the same patch, the same note: only the MCM differs,
  // and the two ranges it chooses between are this synth's fixed 2 semitones and
  // the member channel's 48.
  constexpr uint16_t kBendUp = 8192 + 2048;  // a quarter of the way up
  BuiltinSynth plain = mpe_synth();
  mpe_send(plain, sonare::midi::make_midi1_pitch_bend(0, 2, kBendUp));
  const double without_zone = mpe_cents(plain, 2, 50.0);

  BuiltinSynth zoned = mpe_synth();
  mpe_send_mcm(zoned, 0, 7);
  mpe_send(zoned, sonare::midi::make_midi1_pitch_bend(0, 2, kBendUp));
  const double with_zone = mpe_cents(zoned, 2, 1200.0);

  CAPTURE(without_zone, with_zone);
  REQUIRE(std::fabs(without_zone - 50.0) < 5.0);
  REQUIRE(std::fabs(with_zone - 1200.0) < 5.0);
}

TEST_CASE("BuiltinSynth lets a manager's bend reach a note on a member", "[midi][synth][mpe]") {
  BuiltinSynth synth = mpe_synth();
  mpe_send_mcm(synth, 0, 7);
  // Nothing is sent to channel 2 at all. The manager keeps the ordinary
  // 2-semitone range rather than the member's 48, so the size of the move says
  // which of the two sensitivities was read.
  mpe_send(synth, sonare::midi::make_midi1_pitch_bend(0, 0, 8192 + 2048));
  const double cents = mpe_cents(synth, 2, 50.0);
  CAPTURE(cents);
  REQUIRE(std::fabs(cents - 50.0) < 5.0);
}

TEST_CASE("BuiltinSynth folds a manager's pressure into a member's", "[midi][synth][mpe]") {
  // Pressure is a gain here, so the fold is a level: the manager's value alone
  // has to reach a note on a member channel, and the two have to add.
  auto peak_with = [](bool zoned, uint8_t manager_value, uint8_t member_value) {
    BuiltinSynth synth = mpe_synth();
    if (zoned) mpe_send_mcm(synth, 0, 7);
    mpe_send(synth, sonare::midi::make_midi1_channel_pressure(0, 0, manager_value));
    mpe_send(synth, sonare::midi::make_midi1_channel_pressure(0, 2, member_value));
    mpe_send(synth, sonare::midi::make_midi1_note_on(0, 2, 60, kMpeVelocity));
    return render_peak(&synth, 8192);
  };

  const float none = peak_with(true, 0, 0);
  const float member_only = peak_with(true, 0, 40);
  const float manager_only = peak_with(true, 40, 0);
  const float both = peak_with(true, 40, 40);
  const float unzoned = peak_with(false, 40, 0);
  CAPTURE(none, member_only, manager_only, both, unzoned);

  REQUIRE(member_only > none);
  // Sent to the manager and nowhere else, and it still reaches the note.
  REQUIRE(manager_only == Catch::Approx(member_only).epsilon(0.01));
  // Sent to both, and they add rather than one replacing the other.
  REQUIRE(both > member_only * 1.1f);
  // Outside a zone the manager channel is an ordinary channel.
  REQUIRE(unzoned == Catch::Approx(none).epsilon(0.01));
}

TEST_CASE("BuiltinSynth obeys the zone's prohibitions", "[midi][synth][mpe]") {
  BuiltinSynth synth = mpe_synth();
  mpe_send_mcm(synth, 0, 7);
  mpe_send(synth, sonare::midi::make_midi1_note_on(0, 2, 60, kMpeVelocity));
  render_peak(&synth, 2048);

  // Poly key pressure is prohibited on a member channel (2.2.7). Observed as a
  // level, paired with the channel pressure that IS accepted there, so a synth
  // ignoring pressure outright would fail the second arm.
  const float plain = render_peak(&synth, 2048);
  mpe_send(synth, sonare::midi::make_midi1_poly_pressure(0, 2, 60, 127));
  REQUIRE(render_peak(&synth, 2048) == Catch::Approx(plain).epsilon(0.01));
  mpe_send(synth, sonare::midi::make_midi1_channel_pressure(0, 2, 127));
  REQUIRE(render_peak(&synth, 2048) > plain * 1.5f);

  // CC#126 / #127 are prohibited on a manager channel, and they carry an
  // all-notes-off everywhere else -- so a receiver that took them there would
  // silence the whole zone on a message it was required to drop.
  mpe_send(synth, sonare::midi::make_midi1_control_change(0, 0, 126, 0));
  const float after_manager = render_peak(&synth, 2048);
  REQUIRE(after_manager > 0.0f);
  mpe_send(synth, sonare::midi::make_midi1_control_change(0, 2, 126, 0));
  render_peak(&synth, 16384);
  REQUIRE(render_peak(&synth, 2048) < after_manager);
}

TEST_CASE("BuiltinSynth renders a project that configures no zone unchanged",
          "[.][midi][synth][golden]") {
  // The zone model is inert until an MCM arrives, and this is the hash that says
  // so: every controller this synth honours, sent on an ordinary channel, over a
  // render long enough to cover the attack, the sustain and the release. Hidden
  // like every other render hash: the 1e-6 quantization is finer than float
  // reproducibility across architectures and libm implementations, so the digest
  // is specific to the host that recorded it.
  BuiltinSynthConfig cfg;
  cfg.waveform = sonare::midi::SynthWaveform::kSaw;
  cfg.gain = 0.3f;
  BuiltinSynth synth(cfg);
  synth.prepare(kMpeRate, 256);

  mpe_send(synth, sonare::midi::make_midi1_control_change(0, 3, 7, 110));
  mpe_send(synth, sonare::midi::make_midi1_control_change(0, 3, 10, 40));
  mpe_send(synth, sonare::midi::make_midi1_control_change(0, 3, 11, 90));
  mpe_send(synth, sonare::midi::make_midi1_note_on(0, 3, 57, 96));
  mpe_send(synth, sonare::midi::make_midi1_note_on(0, 3, 64, 80));
  mpe_send(synth, sonare::midi::make_midi1_pitch_bend(0, 3, 10240));
  mpe_send(synth, sonare::midi::make_midi1_channel_pressure(0, 3, 64));
  mpe_send(synth, sonare::midi::make_midi1_poly_pressure(0, 3, 64, 100));
  // The parameter numbers the zone model added: on a channel in no zone they
  // select nothing this synth acts on, which is what the hash has to show.
  mpe_send(synth, sonare::midi::make_midi1_control_change(0, 3, 101, 0));
  mpe_send(synth, sonare::midi::make_midi1_control_change(0, 3, 100, 0));
  mpe_send(synth, sonare::midi::make_midi1_control_change(0, 3, 6, 12));
  mpe_send(synth, sonare::midi::make_midi1_control_change(0, 3, 64, 127));

  std::vector<float> left(24000, 0.0f);
  std::vector<float> right(24000, 0.0f);
  float* channels[2] = {left.data(), right.data()};
  synth.process(channels, 2, 12000);
  mpe_send(synth, sonare::midi::make_midi1_note_off(0, 3, 57, 0));
  mpe_send(synth, sonare::midi::make_midi1_control_change(0, 3, 64, 0));
  float* tail[2] = {left.data() + 12000, right.data() + 12000};
  synth.process(tail, 2, 12000);

  const uint64_t hash = sonare::test::fnv1a_quantized_stereo(left, right);
  CAPTURE(hash);
  REQUIRE(hash == 0x1205b45ac235cd9dull);
}
