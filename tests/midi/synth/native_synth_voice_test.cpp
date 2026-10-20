#include <algorithm>
#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <cstdint>
#include <vector>

#include "midi/midi_event.h"
#include "midi/synth/native_synth.h"
#include "midi/synth/player_trajectory.h"
#include "midi/ump.h"
#include "util/constants.h"

namespace {

using sonare::midi::Velocity16;
using sonare::midi::synth::EnvelopeTimeScales;
using sonare::midi::synth::native_patch_tail_samples;
using sonare::midi::synth::NativeSynthPatch;
using sonare::midi::synth::NativeSynthVoice;
using sonare::midi::synth::SampleBank;
using sonare::midi::synth::SampleDesc;
using sonare::midi::synth::SampleZoneDesc;
using sonare::midi::synth::Sf2ChannelMod;
using sonare::midi::synth::SynthEngineMode;

constexpr int kFrames = 4800;

struct SampleTailCase {
  int64_t declared = 0;
  int stopped = -1;
};

SampleTailCase run_case(NativeSynthVoice& voice, const NativeSynthPatch& patch,
                        const SampleBank& bank, int sample_rate, int frame_count) {
  SampleTailCase result;
  result.declared = native_patch_tail_samples(patch, sample_rate, EnvelopeTimeScales{}, &bank);
  for (int frame = 0; frame < frame_count; ++frame) {
    voice.render(Sf2ChannelMod{});
    if (!voice.active) {
      result.stopped = frame;
      break;
    }
  }
  return result;
}

}  // namespace

TEST_CASE("Native sample one-shot tail follows fixed pitch tuning", "[boundary]") {
  for (const int sample_rate : {48000, 96000}) {
    SampleBank bank;
    std::vector<float> data(static_cast<size_t>(kFrames));
    for (int i = 0; i < kFrames; ++i) {
      data[static_cast<size_t>(i)] =
          0.25f * static_cast<float>(
                      std::sin(6.283185307179586 * 600.0 * static_cast<double>(i) / sample_rate));
    }
    SampleDesc desc;
    uint32_t sample_index = 0;
    REQUIRE(bank.add_sample(data.data(), data.size(), desc, &sample_index));
    SampleZoneDesc zone;
    zone.sample_index = sample_index;
    REQUIRE(bank.add_zone(0, zone));

    std::array<NativeSynthPatch, 3> patches;
    std::array<NativeSynthVoice, 3> voices;
    const std::array<float, 3> pitch_offsets = {0.0f, -1200.0f, 1200.0f};
    const std::array<int, 3> expected_frames = {4800, 9600, 2400};
    for (size_t i = 0; i < patches.size(); ++i) {
      NativeSynthPatch& patch = patches[i];
      patch.mode = SynthEngineMode::kSample;
      patch.one_shot = true;
      patch.amp_env = {0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 1.0f};
      patch.cutoff_hz = 20000.0f;
      patch.sample.key_track = false;
      patch.pitch_offset_cents = pitch_offsets[i];
      voices[i].sampler.attach(&bank);
      voices[i].note = 60;
      voices[i].active = true;
      voices[i].start(patch, sample_rate, Velocity16::from7(100), 0);
    }

    for (size_t i = 0; i < patches.size(); ++i) {
      const SampleTailCase result =
          run_case(voices[i], patches[i], bank, sample_rate, expected_frames[i] + 2);
      INFO("sample rate " << sample_rate << ", voice " << i << ", declared " << result.declared
                          << ", stopped " << result.stopped);
      CHECK(result.declared == expected_frames[i]);
      CHECK(result.stopped == expected_frames[i]);
    }
  }
}

TEST_CASE("Native sample one-shot tail follows a live downward pitch offset", "[boundary]") {
  using sonare::midi::make_midi1_note_off;
  using sonare::midi::make_midi1_note_on;
  using sonare::midi::MidiEvent;
  using sonare::midi::synth::NativeSynth;
  using sonare::midi::synth::NativeSynthConfig;
  using sonare::midi::synth::NativeSynthParamId;

  constexpr int kBlock = 256;
  for (const int sample_rate : {48000, 96000}) {
    const int frames = sample_rate / 10;
    SampleBank bank;
    std::vector<float> data(static_cast<size_t>(frames));
    for (int i = 0; i < frames; ++i) {
      data[static_cast<size_t>(i)] =
          0.25f * static_cast<float>(std::sin(sonare::constants::kTwoPiD * 600.0 *
                                              static_cast<double>(i) / sample_rate));
    }
    SampleDesc desc;
    uint32_t sample_index = 0;
    REQUIRE(bank.add_sample(data.data(), data.size(), desc, &sample_index));
    SampleZoneDesc zone;
    zone.sample_index = sample_index;
    REQUIRE(bank.add_zone(0, zone));

    NativeSynthConfig config;
    config.gain = 1.0f;
    config.dc_block = false;
    config.patch.mode = SynthEngineMode::kSample;
    config.patch.one_shot = true;
    config.patch.amp_env = {0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 1.0f};
    config.patch.cutoff_hz = 20000.0f;
    config.patch.sample.key_track = false;
    NativeSynth synth(config);
    synth.set_sample_bank(&bank);
    synth.prepare(sample_rate, kBlock);

    std::array<float, kBlock> left{};
    std::array<float, kBlock> right{};
    float* channels[2] = {left.data(), right.data()};
    MidiEvent event;
    event.ump = make_midi1_note_on(0, 0, 60, 100);
    synth.on_event(0, event);
    synth.process(channels, 2, kBlock);
    // An octave down halves the playback rate of what is left of the zone.
    REQUIRE(synth.apply_parameter(static_cast<unsigned int>(NativeSynthParamId::kPitchOffsetCents),
                                  -1200.0f));
    synth.process(channels, 2, kBlock);
    event.ump = make_midi1_note_off(0, 0, 60, 0);
    synth.on_event(0, event);

    const int declared = synth.tail_samples();
    const int frame_count = 4 * frames;
    int last_above_floor = -1;
    for (int offset = 0; offset < frame_count; offset += kBlock) {
      left.fill(0.0f);
      right.fill(0.0f);
      synth.process(channels, 2, kBlock);
      for (int i = 0; i < kBlock; ++i) {
        const float value = std::max(std::fabs(left[static_cast<size_t>(i)]),
                                     std::fabs(right[static_cast<size_t>(i)]));
        if (value > 1.0e-5f) last_above_floor = offset + i;
      }
    }
    INFO("sample rate " << sample_rate << ", declared " << declared << ", last sound "
                        << last_above_floor);
    REQUIRE(last_above_floor > frames);
    CHECK(last_above_floor < declared);
  }
}

TEST_CASE("a releasing OnsetContour reaches exactly zero and never holds a subnormal",
          "[midi][synth][contour]") {
  using sonare::midi::synth::OnsetContour;
  using sonare::midi::synth::ramp_coeff;
  constexpr double kSr = 48000.0;
  for (const float release_ms : {20.0f, 64.0f, 120.0f}) {
    OnsetContour contour;
    contour.attack_coeff = ramp_coeff(10.0f, kSr);
    contour.release_coeff = ramp_coeff(release_ms, kSr);
    contour.level = 1.0f;
    contour.release();
    // Five time constants of the 95% rise per millisecond is far inside this bound.
    const int bound = static_cast<int>(10.0 * release_ms * 0.001 * kSr);
    int reached = -1;
    for (int i = 0; i < bound; ++i) {
      const float level = contour.advance();
      REQUIRE(std::fpclassify(level) != FP_SUBNORMAL);
      if (level == 0.0f) {
        reached = i;
        break;
      }
    }
    INFO("release_ms " << release_ms);
    CHECK(reached >= 0);
    CHECK(contour.advance() == 0.0f);
  }
}

TEST_CASE("a GS absolute drum pan is the same on every strike whatever the seed",
          "[midi][synth][pan]") {
  using sonare::midi::synth::DrumVoiceMod;
  NativeSynthPatch patch;
  patch.stereo_spread = 1.0f;
  DrumVoiceMod mod;
  mod.pan_units = 200.0f;
  std::array<float, 2> left_gain{};
  std::array<float, 2> right_gain{};
  std::array<float, 2> free_left{};
  for (uint32_t index = 0; index < 2; ++index) {
    NativeSynthVoice voice;
    voice.note = 38;
    voice.active = true;
    voice.start(patch, 48000.0, Velocity16::from7(100), index + 7, 0.0f, false, 0, mod);
    voice.render(Sf2ChannelMod{});
    left_gain[index] = voice.gain_left;
    right_gain[index] = voice.gain_right;
    NativeSynthVoice scattered;
    scattered.note = 38;
    scattered.active = true;
    scattered.start(patch, 48000.0, Velocity16::from7(100), index + 7);
    scattered.render(Sf2ChannelMod{});
    free_left[index] = scattered.gain_left;
  }
  CHECK(left_gain[0] == left_gain[1]);
  CHECK(right_gain[0] == right_gain[1]);
  // Without a drum pan the scatter still places each strike apart.
  CHECK(free_left[0] != free_left[1]);
}
