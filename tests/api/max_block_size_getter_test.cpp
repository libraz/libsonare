/// @file max_block_size_getter_test.cpp
/// @brief Every handle that reads a block size of 0 as the library default reports the
/// effective maximum, so a C caller can chunk at it without repeating the default.

#include <sonare/sonare_c_engine.h>
#include <sonare/sonare_c_mastering.h>
#include <sonare/sonare_c_mixing.h>
#include <sonare/sonare_c_playback.h>
#include <sonare/sonare_c_voice_changer.h>

#include <catch2/catch_test_macros.hpp>
#include <vector>

#include "editing/voice_changer/realtime.h"
#include "engine/realtime_engine.h"
#include "mastering/api/chain.h"
#include "mastering/eq/equalizer.h"
#include "mixing/api/scene.h"
#include "playback/renderer.h"

namespace {

constexpr int kRate = 48000;

}  // namespace

#if defined(SONARE_WITH_VOICE_CHANGER)
TEST_CASE("realtime voice changer reports its default max block size", "[c-api]") {
  SonareRealtimeVoiceChanger* handle = nullptr;
  REQUIRE(sonare_realtime_voice_changer_create(nullptr, kRate, 0, 1, &handle) == SONARE_OK);
  int max_block = -1;
  REQUIRE(sonare_realtime_voice_changer_max_block_size(handle, &max_block) == SONARE_OK);
  CHECK(max_block == sonare::editing::voice_changer::kRealtimeVoiceChangerDefaultMaxBlock);

  std::vector<float> in(static_cast<size_t>(max_block) + 1, 0.0f);
  std::vector<float> out(in.size(), 0.0f);
  CHECK(sonare_realtime_voice_changer_process_mono(handle, in.data(), nullptr, out.data(),
                                                   static_cast<size_t>(max_block)) == SONARE_OK);
  CHECK(sonare_realtime_voice_changer_process_mono(handle, in.data(), nullptr, out.data(),
                                                   in.size()) == SONARE_ERROR_INVALID_PARAMETER);

  max_block = -1;
  CHECK(sonare_realtime_voice_changer_max_block_size(nullptr, &max_block) ==
        SONARE_ERROR_INVALID_PARAMETER);
  CHECK(max_block == 0);
  CHECK(sonare_realtime_voice_changer_max_block_size(handle, nullptr) ==
        SONARE_ERROR_INVALID_PARAMETER);
  sonare_realtime_voice_changer_destroy(handle);
}
#endif  // defined(SONARE_WITH_VOICE_CHANGER)

#if defined(SONARE_WITH_MASTERING)
TEST_CASE("streaming mastering chain reports its default max block size", "[c-api]") {
  SonareStreamingMasteringChain* handle = nullptr;
  REQUIRE(sonare_streaming_mastering_chain_create(nullptr, nullptr, &handle) == SONARE_OK);
  REQUIRE(handle != nullptr);
  int max_block = -1;
  REQUIRE(sonare_streaming_mastering_chain_max_block_size(handle, &max_block) == SONARE_OK);
  CHECK(max_block == 0);  // not prepared yet

  REQUIRE(sonare_streaming_mastering_chain_prepare(handle, kRate, 0, 1) == SONARE_OK);
  REQUIRE(sonare_streaming_mastering_chain_max_block_size(handle, &max_block) == SONARE_OK);
  CHECK(max_block == sonare::mastering::api::MasteringChain::kDefaultMaxBlock);

  std::vector<float> samples(static_cast<size_t>(max_block) + 1, 0.0f);
  CHECK(sonare_streaming_mastering_chain_process_mono(handle, samples.data(),
                                                      static_cast<size_t>(max_block)) == SONARE_OK);
  CHECK(sonare_streaming_mastering_chain_process_mono(handle, samples.data(), samples.size()) ==
        SONARE_ERROR_INVALID_PARAMETER);

  max_block = -1;
  CHECK(sonare_streaming_mastering_chain_max_block_size(nullptr, &max_block) ==
        SONARE_ERROR_INVALID_PARAMETER);
  CHECK(max_block == 0);
  sonare_streaming_mastering_chain_destroy(handle);
}

TEST_CASE("equalizer reports its default max block size", "[c-api]") {
  SonareEq* eq = nullptr;
  REQUIRE(sonare_eq_create(kRate, 0, &eq) == SONARE_OK);
  REQUIRE(eq != nullptr);
  int max_block = -1;
  REQUIRE(sonare_eq_max_block_size(eq, &max_block) == SONARE_OK);
  CHECK(max_block == sonare::mastering::eq::EqualizerProcessor::kDefaultMaxBlock);

  std::vector<float> left(static_cast<size_t>(max_block) + 1, 0.0f);
  std::vector<float> right(left.size(), 0.0f);
  float* channels[2] = {left.data(), right.data()};
  CHECK(sonare_eq_process(eq, channels, 2, max_block) == SONARE_OK);
  CHECK(sonare_eq_process(eq, channels, 2, max_block + 1) == SONARE_ERROR_INVALID_PARAMETER);

  max_block = -1;
  CHECK(sonare_eq_max_block_size(nullptr, &max_block) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(max_block == 0);
  sonare_eq_destroy(eq);
}
#endif  // defined(SONARE_WITH_MASTERING)

#if defined(SONARE_WITH_MIXING)
TEST_CASE("mixer reports its default max block size", "[c-api]") {
  SonareMixer* mixer = nullptr;
  REQUIRE(sonare_mixer_create(kRate, 0, &mixer) == SONARE_OK);
  REQUIRE(mixer != nullptr);
  int max_block = -1;
  REQUIRE(sonare_mixer_max_block_size(mixer, &max_block) == SONARE_OK);
  CHECK(max_block == sonare::mixing::api::kMixerDefaultMaxBlock);

  std::vector<float> left(static_cast<size_t>(max_block) + 1, 0.0f);
  std::vector<float> right(left.size(), 0.0f);
  CHECK(sonare_mixer_process_stereo(mixer, nullptr, nullptr, 0, left.data(), right.data(),
                                    static_cast<size_t>(max_block)) == SONARE_OK);
  CHECK(sonare_mixer_process_stereo(mixer, nullptr, nullptr, 0, left.data(), right.data(),
                                    left.size()) == SONARE_ERROR_INVALID_PARAMETER);

  max_block = -1;
  CHECK(sonare_mixer_max_block_size(nullptr, &max_block) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(max_block == 0);
  sonare_mixer_destroy(mixer);
}
#endif  // defined(SONARE_WITH_MIXING)

#if defined(SONARE_WITH_PLAYBACK)
TEST_CASE("playback renderer reports its default max block size", "[c-api]") {
  SonarePlaybackRenderer* renderer = nullptr;
  REQUIRE(sonare_playback_renderer_create_json("{}", nullptr, kRate, 0, &renderer) == SONARE_OK);
  int max_block = -1;
  REQUIRE(sonare_playback_renderer_max_block_size(renderer, &max_block) == SONARE_OK);
  CHECK(max_block == sonare::playback::kPlaybackRendererDefaultMaxBlock);

  int in_channels = 0;
  int out_channels = 0;
  REQUIRE(sonare_playback_renderer_num_input_channels(renderer, &in_channels) == SONARE_OK);
  REQUIRE(sonare_playback_renderer_num_output_channels(renderer, &out_channels) == SONARE_OK);
  const size_t frames = static_cast<size_t>(max_block) + 1;
  std::vector<std::vector<float>> in(static_cast<size_t>(in_channels),
                                     std::vector<float>(frames, 0.0f));
  std::vector<std::vector<float>> out(static_cast<size_t>(out_channels),
                                      std::vector<float>(frames, 0.0f));
  std::vector<const float*> in_ptrs;
  std::vector<float*> out_ptrs;
  for (auto& ch : in) in_ptrs.push_back(ch.data());
  for (auto& ch : out) out_ptrs.push_back(ch.data());
  CHECK(sonare_playback_renderer_process_planar(renderer, in_ptrs.data(), in_channels,
                                                out_ptrs.data(), out_channels,
                                                max_block) == SONARE_OK);
  CHECK(sonare_playback_renderer_process_planar(renderer, in_ptrs.data(), in_channels,
                                                out_ptrs.data(), out_channels,
                                                max_block + 1) == SONARE_ERROR_INVALID_PARAMETER);

  max_block = -1;
  CHECK(sonare_playback_renderer_max_block_size(nullptr, &max_block) ==
        SONARE_ERROR_INVALID_PARAMETER);
  CHECK(max_block == 0);
  sonare_playback_renderer_destroy(renderer);
}
#endif  // defined(SONARE_WITH_PLAYBACK)

TEST_CASE("realtime engine reports its default max block size", "[c-api]") {
  SonareEngine* engine = nullptr;
  REQUIRE(sonare_engine_create(&engine) == SONARE_OK);
  int max_block = -1;
  REQUIRE(sonare_engine_max_block_size(engine, &max_block) == SONARE_OK);
  CHECK(max_block == 0);  // not prepared yet

  REQUIRE(sonare_engine_prepare(engine, kRate, 0, 0, 0) == SONARE_OK);
  REQUIRE(sonare_engine_max_block_size(engine, &max_block) == SONARE_OK);
  CHECK(max_block == sonare::engine::RealtimeEngine::kDefaultMaxBlock);

  std::vector<float> left(static_cast<size_t>(max_block), 0.0f);
  std::vector<float> right(left.size(), 0.0f);
  float* channels[2] = {left.data(), right.data()};
  CHECK(sonare_engine_process(engine, channels, 2, max_block) == SONARE_OK);

  max_block = -1;
  CHECK(sonare_engine_max_block_size(nullptr, &max_block) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(max_block == 0);
  sonare_engine_destroy(engine);
}
