/// @file no_alloc_voice_reverb_test.cpp
/// @brief Voice changer and reverb no-allocation realtime tests.

#include "no_alloc_test_helpers.h"

#ifdef SONARE_WITH_FX
#include <utility>

#include "effects/filter/vowel_filter.h"
#include "effects/modulation/auto_wah.h"
#include "effects/modulation/chorus.h"
#include "effects/modulation/flanger.h"
#include "effects/modulation/phaser.h"
#include "effects/modulation/pitch_shifter.h"
#include "effects/modulation/rotary.h"
#include "effects/modulation/wah.h"

namespace {
template <typename Processor>
void require_allocation_free_layout_reentry(Processor& processor) {
  constexpr int kBlock = 128;
  processor.prepare(48000.0, kBlock);
  std::array<float, kBlock> left{};
  std::array<float, kBlock> right{};
  left.fill(0.03f);
  right.fill(0.05f);
  float* channels[] = {left.data(), right.data()};
  processor.process(channels, 2, kBlock);
  std::size_t allocations = 0;
  {
    AllocationGuard guard;
    processor.process(channels, 1, kBlock);
    processor.process(channels, 2, kBlock);
    processor.reset();
    processor.process(channels, 2, kBlock);
    allocations = guard.count();
  }
  REQUIRE(allocations == 0);
}

template <typename Processor>
void require_allocation_free_stage_reentry(Processor& processor, unsigned int parameter) {
  constexpr int kBlock = 128;
  processor.prepare(48000.0, kBlock);
  std::array<float, kBlock> left{};
  std::array<float, kBlock> right{};
  float* channels[] = {left.data(), right.data()};
  processor.process(channels, 2, kBlock);
  std::size_t allocations = 0;
  bool accepted = true;
  {
    AllocationGuard guard;
    accepted &= processor.set_parameter(parameter, 0.0f);
    processor.process(channels, 2, kBlock);
    accepted &= processor.set_parameter(parameter, 1.0f);
    processor.process(channels, 2, kBlock);
    accepted &= processor.set_parameter(parameter, 0.0f);
    processor.process(channels, 2, kBlock);
    allocations = guard.count();
  }
  REQUIRE(accepted);
  REQUIRE(allocations == 0);
}
}  // namespace

TEST_CASE("Modulation and vowel layout reentry performs no heap allocation",
          "[effects][rt][boundary]") {
  using namespace sonare::effects::modulation;
  Chorus chorus;
  Flanger flanger;
  Phaser phaser;
  Rotary rotary;
  Wah wah;
  AutoWah auto_wah;
  sonare::effects::filter::VowelFilter vowel;
  PitchShifterConfig pitch_config;
  pitch_config.semitones = 7.0f;
  pitch_config.anti_alias = true;
  PitchShifter pitch(pitch_config);
  require_allocation_free_layout_reentry(chorus);
  require_allocation_free_layout_reentry(flanger);
  require_allocation_free_layout_reentry(phaser);
  require_allocation_free_layout_reentry(rotary);
  require_allocation_free_layout_reentry(wah);
  require_allocation_free_layout_reentry(auto_wah);
  require_allocation_free_layout_reentry(vowel);
  require_allocation_free_layout_reentry(pitch);
}

TEST_CASE("Prefilter and rotary model reentry performs no heap allocation",
          "[effects][rt][boundary]") {
  using namespace sonare::effects::modulation;
  ChorusConfig chorus_config;
  chorus_config.pre_filter_hz = 100.0f;
  chorus_config.pre_filter_mode = PreFilterMode::kLowPass;
  Chorus chorus(chorus_config);
  FlangerConfig flanger_config;
  flanger_config.pre_filter_hz = 100.0f;
  flanger_config.pre_filter_mode = PreFilterMode::kLowPass;
  Flanger flanger(flanger_config);
  Rotary rotary;
  require_allocation_free_stage_reentry(chorus, 7);
  require_allocation_free_stage_reentry(flanger, 8);
  require_allocation_free_stage_reentry(rotary, 13);
}

TEST_CASE("PitchShifter parameter updates perform no heap allocation after prepare",
          "[effects][rt]") {
  constexpr int kBlock = 128;
  sonare::effects::modulation::PitchShifterConfig config;
  config.semitones = 7.0f;
  config.anti_alias = true;
  sonare::effects::modulation::PitchShifter shifter(config);
  shifter.prepare(48000.0, kBlock);

  std::array<float, kBlock> buffer{};
  buffer.fill(0.05f);
  float* channels[] = {buffer.data()};
  shifter.process(channels, 1, kBlock);

  const std::array<std::pair<unsigned int, float>, 10> controls{{
      {0, 11.0f},
      {2, 25.0f},
      {4, 7.0f},
      {5, -25.0f},
      {6, 1.0f},
      {9, 1.0f},
      {11, 0.0f},
      {11, 1.0f},
      {1, 0.0f},
      {1, 1.0f},
  }};
  bool accepted = true;
  std::size_t allocations = 0;
  {
    AllocationGuard guard;
    for (const auto& control : controls) {
      accepted &= shifter.set_parameter(control.first, control.second);
      shifter.process(channels, 1, kBlock);
    }
    allocations = guard.count();
  }
  REQUIRE(accepted);
  REQUIRE(allocations == 0);
}
#endif

#ifdef SONARE_WITH_VOICE_CHANGER
#include <sonare/sonare_c.h>

#include "editing/voice_changer/realtime.h"

TEST_CASE("RealtimeVoiceChanger process_block performs no heap allocation after prepare",
          "[voice_changer][rt]") {
  constexpr int kBlock = 128;
  constexpr int kSampleRate = 48000;

  sonare::editing::voice_changer::RealtimeVoiceChanger changer(
      sonare::editing::voice_changer::realtime_voice_changer_preset(
          sonare::editing::voice_changer::VoiceCharacterPreset::NeutralMonitor));
  changer.prepare(kSampleRate, kBlock, 1);

  std::array<float, kBlock> input{};
  std::array<float, kBlock> output{};
  input.fill(0.01f);

  // Warm up: drive one block so any lazy first-block work (initial snapshot
  // adoption, derived-state computation) completes before we start counting.
  changer.process_block(input.data(), output.data(), kBlock);

  AllocationGuard guard;
  changer.process_block(input.data(), output.data(), kBlock);
  REQUIRE(guard.count() == 0);
}

TEST_CASE("RealtimeVoiceChanger planar process_block performs no heap allocation after prepare",
          "[voice_changer][rt]") {
  constexpr int kBlock = 128;
  constexpr int kSampleRate = 48000;

  sonare::editing::voice_changer::RealtimeVoiceChanger changer(
      sonare::editing::voice_changer::realtime_voice_changer_preset(
          sonare::editing::voice_changer::VoiceCharacterPreset::BrightIdol));
  changer.prepare(kSampleRate, kBlock, 2);

  std::array<float, kBlock> left{};
  std::array<float, kBlock> right{};
  left.fill(0.05f);
  right.fill(-0.05f);
  float* channels[] = {left.data(), right.data()};

  // Warm up block.
  changer.process_block(channels, 2, kBlock);

  AllocationGuard guard;
  changer.process_block(channels, 2, kBlock);
  REQUIRE(guard.count() == 0);
}

TEST_CASE("RealtimeVoiceChanger::set_config performs no heap allocation", "[voice_changer][rt]") {
  // Regression coverage for the AudioWorklet render-thread hazard: on WASM,
  // an AudioWorkletProcessor's port message handler runs on the SAME single
  // rendering thread as process_block() (there is no separate "config
  // thread" there), so a UI control that fires set_config() every frame
  // while dragging a slider must not allocate — an allocation inside the
  // ~2.67 ms/128-frame budget risks dropouts. This reproduces that call
  // pattern directly: set_config() interleaved with process_block() on one
  // thread, hammered repeatedly, all while the allocation counter is armed.
  constexpr int kBlock = 128;
  constexpr int kSampleRate = 48000;

  sonare::editing::voice_changer::RealtimeVoiceChanger changer(
      sonare::editing::voice_changer::realtime_voice_changer_preset(
          sonare::editing::voice_changer::VoiceCharacterPreset::NeutralMonitor));
  changer.prepare(kSampleRate, kBlock, 1);

  std::array<float, kBlock> input{};
  std::array<float, kBlock> output{};
  input.fill(0.01f);

  // Warm up: drive one block so any lazy first-block work completes before
  // we start counting.
  changer.process_block(input.data(), output.data(), kBlock);

  auto config = changer.config();

  AllocationGuard guard;
  for (int i = 0; i < 16; ++i) {
    // Vary several fields per call, as a real UI slider drag would.
    config.wet_mix = (i % 2 == 0) ? 0.2f : 0.8f;
    config.eq.presence_db = static_cast<float>(i) * 0.25f;
    config.retune.semitones = static_cast<float>(i % 5);
    changer.set_config(config);
    changer.process_block(input.data(), output.data(), kBlock);
  }
  REQUIRE(guard.count() == 0);
}

TEST_CASE("sonare_streaming_retune_process_mono performs no heap allocation after prepare",
          "[voice_changer][rt][c_api]") {
  // The C entry point works in place, so it needs somewhere to read the input
  // from while the core writes the output. It used to build that copy as a
  // fresh std::vector per block -- 375 allocations a second at 128 samples /
  // 48 kHz, on top of the copy each binding already makes -- against a core
  // that documents "Allocation happens only in prepare()".
  constexpr int kBlock = 128;
  constexpr int kSampleRate = 48000;

  SonareStreamingRetune* retune = sonare_streaming_retune_create(5.0f, 1.0f, 0);
  REQUIRE(retune != nullptr);
  REQUIRE(sonare_streaming_retune_prepare(retune, kSampleRate, kBlock) == SONARE_OK);

  std::array<float, kBlock> block{};
  block.fill(0.01f);

  // Warm up: the entry point clears the thread-local error message, and the
  // first touch of a thread_local can allocate its TLS storage. A real audio
  // callback pays that once too; what must not repeat is the per-block copy.
  REQUIRE(sonare_streaming_retune_process_mono(retune, block.data(), block.size()) == SONARE_OK);

  {
    AllocationGuard guard;
    for (int i = 0; i < 8; ++i) {
      sonare_streaming_retune_process_mono(retune, block.data(), block.size());
    }
    REQUIRE(guard.count() == 0);
  }

  sonare_streaming_retune_destroy(retune);
}
#endif  // SONARE_WITH_VOICE_CHANGER

#if defined(SONARE_WITH_FX) && defined(SONARE_WITH_ACOUSTIC_SIM)
TEST_CASE("RoomMorphProcessor process performs no heap allocation after prepare",
          "[effects][acoustic][rt]") {
  constexpr int kBlock = 256;

  sonare::effects::acoustic::RoomMorphConfig config;
  config.target.dims = {8.0f, 6.0f, 3.5f};
  for (auto& wall : config.target.walls) {
    wall = sonare::acoustic::uniform_material(0.12f, 0.0f);
  }
  config.placement = {{1.5f, 1.0f, 1.2f}, {5.0f, 4.0f, 1.7f}};
  config.wet = 0.6f;
  config.source_tail_suppression = 0.5f;

  sonare::effects::acoustic::RoomMorphProcessor morph(config);
  morph.prepare(48000.0, kBlock);
  REQUIRE(morph.tail_samples() == morph.target_ir_size());
  REQUIRE(morph.tail_samples() > 0);

  std::array<float, kBlock> buf{};
  buf.fill(0.05f);
  buf[0] = 1.0f;
  float* channels[] = {buf.data()};

  // Warm-up block (the convolver's internal partition fill happens here).
  morph.process(channels, 1, kBlock);

  AllocationGuard guard;
  morph.process(channels, 1, kBlock);
  REQUIRE(guard.count() == 0);
}

TEST_CASE("RoomReverb process performs no heap allocation after prepare",
          "[effects][acoustic][rt]") {
  constexpr int kBlock = 256;

  // The 5th reverb engine synthesizes its RIR in prepare(); process() is the
  // inherited ConvolutionReverb path and must stay allocation-free.
  sonare::effects::reverb::RoomReverbConfig config;
  config.dims = {8.0f, 6.0f, 3.5f};
  config.absorption = 0.12f;
  config.dry_wet = 0.5f;

  sonare::effects::reverb::RoomReverb reverb(config);
  reverb.prepare(48000.0, kBlock);

  std::array<float, kBlock> left{};
  std::array<float, kBlock> right{};
  left.fill(0.05f);
  right.fill(0.05f);
  left[0] = 1.0f;
  right[0] = 1.0f;
  float* channels[] = {left.data(), right.data()};

  // Warm-up block fills the convolver's internal partitions.
  reverb.process(channels, 2, kBlock);

  AllocationGuard guard;
  reverb.process(channels, 2, kBlock);
  reverb.process(channels, 1, kBlock);
  reverb.process(channels, 2, kBlock);
  REQUIRE(guard.count() == 0);
}
#endif  // SONARE_WITH_FX && SONARE_WITH_ACOUSTIC_SIM
