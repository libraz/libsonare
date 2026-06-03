/// @file no_alloc_voice_reverb_test.cpp
/// @brief Voice changer and reverb no-allocation realtime tests.

#include "no_alloc_test_helpers.h"

#ifdef SONARE_WITH_VOICE_CHANGER
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
  REQUIRE(guard.count() == 0);
}
#endif  // SONARE_WITH_FX && SONARE_WITH_ACOUSTIC_SIM
