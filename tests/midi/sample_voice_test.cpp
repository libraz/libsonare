/// @file sample_voice_test.cpp
/// @brief Host-PCM sample engine (midi/synth/sample_bank, sample_voice,
///        sample_reader): bank validation and the loop invariant it enforces,
///        root-key and source-rate tuning, loop modes, zone selection, and the
///        NativeSynth path that puts a host sample behind the voice's own
///        filter and envelopes.

#include "midi/synth/sample_voice.h"

#include <algorithm>
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <cstdint>
#include <vector>

#include "midi/midi_event.h"
#include "midi/synth/native_synth.h"
#include "midi/synth/sample_bank.h"
#include "midi/ump.h"
#include "support/alloc_guard.h"
#include "util/constants.h"

namespace {

using sonare::midi::MidiEvent;
using sonare::midi::synth::NativeSynth;
using sonare::midi::synth::NativeSynthConfig;
using sonare::midi::synth::SampleBank;
using sonare::midi::synth::SampleDesc;
using sonare::midi::synth::SamplePatchParams;
using sonare::midi::synth::SampleVoiceCore;
using sonare::midi::synth::SampleZoneDesc;
using sonare::midi::synth::SynthEngineMode;

constexpr double kOutRate = 48000.0;

MidiEvent event(const sonare::midi::Ump& ump) {
  MidiEvent e;
  e.ump = ump;
  return e;
}

/// A sine at @p hz, long enough to measure.
std::vector<float> sine(double hz, double rate, size_t n) {
  std::vector<float> out(n);
  for (size_t i = 0; i < n; ++i) {
    out[i] = static_cast<float>(
        std::sin(sonare::constants::kTwoPiD * hz * static_cast<double>(i) / rate));
  }
  return out;
}

/// Frequency from positive-going zero crossings.
double estimate_frequency(const std::vector<float>& buf, double rate) {
  std::vector<double> crossings;
  for (size_t i = 1; i < buf.size(); ++i) {
    if (buf[i - 1] < 0.0f && buf[i] >= 0.0f) {
      const double denom = static_cast<double>(buf[i]) - static_cast<double>(buf[i - 1]);
      const double frac = denom != 0.0 ? -static_cast<double>(buf[i - 1]) / denom : 0.0;
      crossings.push_back(static_cast<double>(i - 1) + frac);
    }
  }
  if (crossings.size() < 2) return 0.0;
  return rate * static_cast<double>(crossings.size() - 1) / (crossings.back() - crossings.front());
}

/// Renders @p n samples of a started core at unmodulated pitch.
std::vector<float> pull(SampleVoiceCore& core, size_t n, bool key_down = true) {
  std::vector<float> out(n);
  for (size_t i = 0; i < n; ++i) out[i] = core.render(1.0f, key_down);
  return out;
}

SampleBank one_shot_bank(size_t n_frames, uint8_t root_key = 60) {
  SampleBank bank;
  const std::vector<float> data(n_frames, 0.5f);
  SampleDesc desc;
  desc.root_key = root_key;
  uint32_t index = 0;
  REQUIRE(bank.add_sample(data.data(), data.size(), desc, &index));
  SampleZoneDesc zone;
  zone.sample_index = index;
  REQUIRE(bank.add_zone(0, zone));
  return bank;
}

}  // namespace

TEST_CASE("SampleBank rejects data and zones it cannot play", "[midi][sample]") {
  SampleBank bank;
  const std::vector<float> data(16, 0.25f);
  SampleDesc desc;

  CHECK_FALSE(bank.add_sample(nullptr, 16, desc, nullptr));
  CHECK_FALSE(bank.add_sample(data.data(), 0, desc, nullptr));
  CHECK(bank.sample_count() == 0);

  uint32_t index = 99;
  REQUIRE(bank.add_sample(data.data(), data.size(), desc, &index));
  CHECK(index == 0);

  SampleZoneDesc zone;
  zone.sample_index = 1;  // not in the bank
  CHECK_FALSE(bank.add_zone(0, zone));

  zone.sample_index = 0;
  zone.key_lo = 60;
  zone.key_hi = 59;  // empty range
  CHECK_FALSE(bank.add_zone(0, zone));

  zone.key_hi = 127;
  zone.key_lo = 0;
  CHECK(bank.add_zone(0, zone));
  CHECK(bank.set_count() == 1);
}

TEST_CASE("SampleBank drops a loop mode whose loop is empty", "[midi][sample]") {
  // SampleReader reads both taps unchecked, so a loop that survives the clamp
  // as an empty span must come back as "no loop" rather than as a wrap over
  // zero samples.
  SampleBank bank;
  const std::vector<float> data(8, 1.0f);
  SampleDesc desc;
  desc.loop_mode = 1;
  desc.loop_start = 4;
  desc.loop_end = 4;
  uint32_t index = 0;
  REQUIRE(bank.add_sample(data.data(), data.size(), desc, &index));
  SampleZoneDesc zone;
  zone.sample_index = index;
  REQUIRE(bank.add_zone(0, zone));

  const auto* resolved = bank.find(0, 60, 100);
  REQUIRE(resolved != nullptr);
  CHECK(resolved->region.loop_mode == 0);
}

TEST_CASE("SampleBank clamps a loop past the end of its sample", "[midi][sample]") {
  SampleBank bank;
  const std::vector<float> data(8, 1.0f);
  SampleDesc desc;
  desc.loop_mode = 1;
  desc.loop_start = 2;
  desc.loop_end = 400;
  uint32_t index = 0;
  REQUIRE(bank.add_sample(data.data(), data.size(), desc, &index));
  SampleZoneDesc zone;
  zone.sample_index = index;
  REQUIRE(bank.add_zone(0, zone));

  const auto* resolved = bank.find(0, 60, 100);
  REQUIRE(resolved != nullptr);
  CHECK(resolved->region.loop_end == resolved->region.end);
  CHECK(resolved->region.loop_mode == 1);
}

TEST_CASE("SampleBank selects a zone by key and velocity", "[midi][sample]") {
  SampleBank bank;
  const std::vector<float> soft(8, 0.1f);
  const std::vector<float> loud(8, 0.9f);
  uint32_t soft_index = 0;
  uint32_t loud_index = 0;
  REQUIRE(bank.add_sample(soft.data(), soft.size(), SampleDesc{}, &soft_index));
  REQUIRE(bank.add_sample(loud.data(), loud.size(), SampleDesc{}, &loud_index));

  SampleZoneDesc soft_zone;
  soft_zone.sample_index = soft_index;
  soft_zone.vel_lo = 1;
  soft_zone.vel_hi = 63;
  REQUIRE(bank.add_zone(0, soft_zone));

  SampleZoneDesc loud_zone;
  loud_zone.sample_index = loud_index;
  loud_zone.vel_lo = 64;
  loud_zone.vel_hi = 127;
  loud_zone.key_lo = 60;
  loud_zone.key_hi = 72;
  REQUIRE(bank.add_zone(0, loud_zone));

  CHECK(bank.find(0, 64, 30) != nullptr);
  CHECK(bank.find(0, 64, 100) != nullptr);
  CHECK(bank.find(0, 64, 30) != bank.find(0, 64, 100));
  // Above the loud zone's key range the soft zone still covers a soft note,
  // but a loud one falls through every rectangle and gets nothing.
  CHECK(bank.find(0, 90, 30) != nullptr);
  CHECK(bank.find(0, 90, 100) == nullptr);
  CHECK(bank.find(1, 64, 100) == nullptr);   // no such set
  CHECK(bank.find(-1, 64, 100) == nullptr);  // the no-keymap sentinel
}

TEST_CASE("SampleVoiceCore is silent without a bank or a covering zone", "[midi][sample]") {
  SamplePatchParams params;
  SampleVoiceCore core;
  CHECK_FALSE(core.start(params, kOutRate, 60, 100));
  CHECK(core.finished());
  CHECK(core.render(1.0f, true) == 0.0f);

  SampleBank bank = one_shot_bank(16);
  core.attach(&bank);
  params.set_index = 7;  // no such set
  CHECK_FALSE(core.start(params, kOutRate, 60, 100));

  params.set_index = 0;
  CHECK(core.start(params, kOutRate, 60, 100));
  CHECK_FALSE(core.finished());
}

TEST_CASE("A one-shot region ends after its own length", "[midi][sample]") {
  SampleBank bank = one_shot_bank(32);
  SamplePatchParams params;
  SampleVoiceCore core;
  core.attach(&bank);
  REQUIRE(core.start(params, kOutRate, 60, 100));

  const std::vector<float> out = pull(core, 40);
  CHECK(core.finished());
  for (size_t i = 0; i < 32; ++i) CHECK(out[i] > 0.0f);
  for (size_t i = 32; i < out.size(); ++i) CHECK(out[i] == 0.0f);
}

TEST_CASE("Key tracking steps the region at the played note", "[midi][sample]") {
  // An octave up doubles the increment, so the same region runs out in half
  // the frames.
  SampleBank bank = one_shot_bank(64, 60);
  SamplePatchParams params;

  SampleVoiceCore at_root;
  at_root.attach(&bank);
  REQUIRE(at_root.start(params, kOutRate, 60, 100));
  pull(at_root, 33);
  CHECK_FALSE(at_root.finished());

  SampleVoiceCore octave_up;
  octave_up.attach(&bank);
  REQUIRE(octave_up.start(params, kOutRate, 72, 100));
  pull(octave_up, 33);
  CHECK(octave_up.finished());
}

TEST_CASE("Key tracking off plays every key at the recorded pitch", "[midi][sample]") {
  SampleBank bank = one_shot_bank(64, 60);
  SamplePatchParams params;
  params.key_track = false;

  SampleVoiceCore high;
  high.attach(&bank);
  REQUIRE(high.start(params, kOutRate, 96, 100));
  pull(high, 33);
  CHECK_FALSE(high.finished());
}

TEST_CASE("A sample carries its own rate into the increment", "[midi][sample]") {
  // A 24 kHz sample rendered at 48 kHz steps half a frame at a time, so it
  // sounds at the pitch it was recorded at rather than an octave up.
  SampleBank bank;
  const std::vector<float> data(64, 0.5f);
  SampleDesc desc;
  desc.source_rate = kOutRate / 2.0;
  uint32_t index = 0;
  REQUIRE(bank.add_sample(data.data(), data.size(), desc, &index));
  SampleZoneDesc zone;
  zone.sample_index = index;
  REQUIRE(bank.add_zone(0, zone));

  SampleVoiceCore core;
  core.attach(&bank);
  REQUIRE(core.start(SamplePatchParams{}, kOutRate, 60, 100));
  pull(core, 100);
  CHECK_FALSE(core.finished());
  pull(core, 40);
  CHECK(core.finished());
}

TEST_CASE("A continuous loop outlives its region", "[midi][sample]") {
  SampleBank bank;
  const std::vector<float> data = {0.0f, 1.0f, -1.0f, 0.5f, 0.25f, 0.75f, -0.5f, -0.25f};
  SampleDesc desc;
  desc.loop_mode = 1;
  desc.loop_start = 2;
  desc.loop_end = 6;
  uint32_t index = 0;
  REQUIRE(bank.add_sample(data.data(), data.size(), desc, &index));
  SampleZoneDesc zone;
  zone.sample_index = index;
  REQUIRE(bank.add_zone(0, zone));

  SampleVoiceCore core;
  core.attach(&bank);
  REQUIRE(core.start(SamplePatchParams{}, kOutRate, 60, 100));
  pull(core, 4096);
  CHECK_FALSE(core.finished());
}

TEST_CASE("A key-down loop runs out once the key lifts", "[midi][sample]") {
  SampleBank bank;
  const std::vector<float> data(32, 0.5f);
  SampleDesc desc;
  desc.loop_mode = 3;
  desc.loop_start = 8;
  desc.loop_end = 16;
  uint32_t index = 0;
  REQUIRE(bank.add_sample(data.data(), data.size(), desc, &index));
  SampleZoneDesc zone;
  zone.sample_index = index;
  REQUIRE(bank.add_zone(0, zone));

  SampleVoiceCore core;
  core.attach(&bank);
  REQUIRE(core.start(SamplePatchParams{}, kOutRate, 60, 100));
  pull(core, 256, /*key_down=*/true);
  CHECK_FALSE(core.finished());
  pull(core, 64, /*key_down=*/false);
  CHECK(core.finished());
}

TEST_CASE("A loop override replaces what the bank recorded", "[midi][sample]") {
  SampleBank bank;
  const std::vector<float> data(32, 0.5f);
  SampleDesc desc;
  desc.loop_mode = 1;
  desc.loop_start = 8;
  desc.loop_end = 16;
  uint32_t index = 0;
  REQUIRE(bank.add_sample(data.data(), data.size(), desc, &index));
  SampleZoneDesc zone;
  zone.sample_index = index;
  REQUIRE(bank.add_zone(0, zone));

  SamplePatchParams params;
  params.loop_override = 0;
  SampleVoiceCore core;
  core.attach(&bank);
  REQUIRE(core.start(params, kOutRate, 60, 100));
  pull(core, 64);
  CHECK(core.finished());
}

TEST_CASE("A start offset skips into the region", "[midi][sample]") {
  SampleBank bank = one_shot_bank(64);
  SamplePatchParams params;
  params.start_offset01 = 0.5f;
  SampleVoiceCore core;
  core.attach(&bank);
  REQUIRE(core.start(params, kOutRate, 60, 100));

  pull(core, 31);
  CHECK_FALSE(core.finished());
  pull(core, 4);
  CHECK(core.finished());
}

TEST_CASE("NativeSynth plays a host sample at the note it was asked for", "[midi][sample]") {
  // The whole point of the engine: PCM the host prepared, tuned by the keymap
  // and stepped by the voice's own pitch chain.
  const double kSampleHz = 440.0;
  const std::vector<float> data = sine(kSampleHz, kOutRate, 48000);

  SampleBank bank;
  SampleDesc desc;
  desc.root_key = 69;  // A4, the pitch the sample was recorded at
  desc.source_rate = kOutRate;
  desc.loop_mode = 1;
  desc.loop_start = 0;
  desc.loop_end = static_cast<uint32_t>(data.size());
  uint32_t index = 0;
  REQUIRE(bank.add_sample(data.data(), data.size(), desc, &index));
  SampleZoneDesc zone;
  zone.sample_index = index;
  REQUIRE(bank.add_zone(0, zone));

  NativeSynthConfig config;
  config.patch.mode = SynthEngineMode::kSample;
  config.patch.sample.set_index = 0;
  config.patch.amp_env.attack_ms = 1.0f;
  config.patch.amp_env.sustain = 1.0f;
  config.patch.gain = 1.0f;
  config.gain = 1.0f;

  auto render_note = [&](uint8_t note) {
    NativeSynth synth(config);
    synth.set_sample_bank(&bank);
    synth.prepare(kOutRate, 512);
    synth.on_event(0, event(sonare::midi::make_midi1_note_on(0, 0, note, 100)));
    std::vector<float> left(8192, 0.0f);
    std::vector<float> right(8192, 0.0f);
    float* chans[2] = {left.data(), right.data()};
    synth.process(chans, 2, 8192);
    return left;
  };

  const std::vector<float> a4 = render_note(69);
  const std::vector<float> a5 = render_note(81);

  // Skip the attack so the envelope's ramp does not bias the crossings.
  const std::vector<float> a4_body(a4.begin() + 1024, a4.end());
  const std::vector<float> a5_body(a5.begin() + 1024, a5.end());

  CHECK(estimate_frequency(a4_body, kOutRate) == Catch::Approx(kSampleHz).epsilon(0.02));
  CHECK(estimate_frequency(a5_body, kOutRate) == Catch::Approx(2.0 * kSampleHz).epsilon(0.02));
}

TEST_CASE("A keymap zone places its sample in the stereo field", "[midi][sample]") {
  const std::vector<float> data = sine(440.0, kOutRate, 4800);

  SampleBank bank;
  SampleDesc desc;
  desc.root_key = 69;
  desc.source_rate = kOutRate;
  desc.loop_mode = 1;
  desc.loop_start = 0;
  desc.loop_end = static_cast<uint32_t>(data.size());
  uint32_t index = 0;
  REQUIRE(bank.add_sample(data.data(), data.size(), desc, &index));
  SampleZoneDesc zone;
  zone.sample_index = index;
  zone.pan_units = -500.0f;  // hard left
  REQUIRE(bank.add_zone(0, zone));

  NativeSynthConfig config;
  config.patch.mode = SynthEngineMode::kSample;
  config.patch.sample.set_index = 0;
  config.patch.amp_env.attack_ms = 1.0f;
  config.patch.amp_env.sustain = 1.0f;
  config.gain = 1.0f;

  NativeSynth synth(config);
  synth.set_sample_bank(&bank);
  synth.prepare(kOutRate, 512);
  synth.on_event(0, event(sonare::midi::make_midi1_note_on(0, 0, 69, 100)));
  std::vector<float> left(4096, 0.0f);
  std::vector<float> right(4096, 0.0f);
  float* chans[2] = {left.data(), right.data()};
  synth.process(chans, 2, 4096);

  float lp = 0.0f;
  float rp = 0.0f;
  for (size_t i = 1024; i < left.size(); ++i) {
    lp = std::max(lp, std::fabs(left[i]));
    rp = std::max(rp, std::fabs(right[i]));
  }
  CHECK(lp > 0.05f);
  CHECK(rp < lp * 0.25f);
}

TEST_CASE("The sample engine's audio path is allocation-free", "[midi][sample]") {
  // Zone lookup walks the bank's own vectors and start() copies what it needs,
  // so note-on and render must not reach the allocator on the audio thread.
  const std::vector<float> data = sine(440.0, kOutRate, 4800);

  SampleBank bank;
  SampleDesc desc;
  desc.root_key = 69;
  desc.source_rate = kOutRate;
  desc.loop_mode = 1;
  desc.loop_start = 0;
  desc.loop_end = static_cast<uint32_t>(data.size());
  uint32_t index = 0;
  REQUIRE(bank.add_sample(data.data(), data.size(), desc, &index));
  SampleZoneDesc zone;
  zone.sample_index = index;
  REQUIRE(bank.add_zone(0, zone));

  NativeSynthConfig config;
  config.patch.mode = SynthEngineMode::kSample;
  config.patch.sample.set_index = 0;
  NativeSynth synth(config);
  synth.set_sample_bank(&bank);
  synth.prepare(kOutRate, 256);

  std::vector<float> left(256, 0.0f);
  std::vector<float> right(256, 0.0f);
  float* chans[2] = {left.data(), right.data()};
  // Warm the pool before measuring: prepare() is the allocation site.
  synth.on_event(0, event(sonare::midi::make_midi1_note_on(0, 0, 69, 100)));
  synth.process(chans, 2, 256);
  synth.on_event(0, event(sonare::midi::make_midi1_note_off(0, 0, 69, 0)));
  synth.process(chans, 2, 256);

  {
    sonare::test::AllocationGuard guard;
    synth.on_event(0, event(sonare::midi::make_midi1_note_on(0, 0, 72, 100)));
    synth.process(chans, 2, 256);
    synth.on_event(0, event(sonare::midi::make_midi1_note_off(0, 0, 72, 0)));
    synth.process(chans, 2, 256);
    REQUIRE(guard.count() == 0);
  }
}

TEST_CASE("The voice filter colours a host sample", "[midi][sample]") {
  // The reason the engine lives inside the subtractive voice rather than
  // beside it: a host sample reaches the resonant multi-mode filter.
  const std::vector<float> data = sine(4000.0, kOutRate, 24000);

  SampleBank bank;
  SampleDesc desc;
  desc.root_key = 60;
  desc.source_rate = kOutRate;
  desc.loop_mode = 1;
  desc.loop_start = 0;
  desc.loop_end = static_cast<uint32_t>(data.size());
  uint32_t index = 0;
  REQUIRE(bank.add_sample(data.data(), data.size(), desc, &index));
  SampleZoneDesc zone;
  zone.sample_index = index;
  REQUIRE(bank.add_zone(0, zone));

  auto peak_at_cutoff = [&](float cutoff_hz) {
    NativeSynthConfig config;
    config.patch.mode = SynthEngineMode::kSample;
    config.patch.sample.set_index = 0;
    config.patch.cutoff_hz = cutoff_hz;
    config.patch.amp_env.attack_ms = 1.0f;
    config.patch.amp_env.sustain = 1.0f;
    config.patch.gain = 1.0f;
    config.gain = 1.0f;

    NativeSynth synth(config);
    synth.set_sample_bank(&bank);
    synth.prepare(kOutRate, 512);
    synth.on_event(0, event(sonare::midi::make_midi1_note_on(0, 0, 60, 100)));
    std::vector<float> left(4096, 0.0f);
    std::vector<float> right(4096, 0.0f);
    float* chans[2] = {left.data(), right.data()};
    synth.process(chans, 2, 4096);
    float p = 0.0f;
    for (size_t i = 2048; i < left.size(); ++i) p = std::max(p, std::fabs(left[i]));
    return p;
  };

  const float open = peak_at_cutoff(20000.0f);
  const float closed = peak_at_cutoff(300.0f);
  CHECK(open > 0.05f);
  CHECK(closed < open * 0.5f);
}
