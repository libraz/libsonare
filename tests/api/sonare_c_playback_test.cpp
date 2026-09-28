#include <sonare/sonare_c.h>

#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <vector>

#if defined(SONARE_WITH_PLAYBACK)

TEST_CASE("playback renderer handle reports its shape", "[playback][capi]") {
  SonarePlaybackRenderer* renderer = nullptr;
  REQUIRE(
      sonare_playback_renderer_create_json(R"({"target": {"kind": "speakers", "layout": "5.1"}})",
                                           nullptr, 48000, 512, &renderer) == SONARE_OK);
  REQUIRE(renderer != nullptr);
  int out_channels = 0;
  CHECK(sonare_playback_renderer_output_channel_count(renderer, &out_channels) == SONARE_OK);
  CHECK(out_channels == 6);
  int latency = 0;
  CHECK(sonare_playback_renderer_latency_samples(renderer, &latency) == SONARE_OK);
  CHECK(latency == 1312);
  sonare_playback_renderer_destroy(renderer);
}

#else

TEST_CASE("playback entry points return NOT_SUPPORTED when compiled out",
          "[playback][capi][stub]") {
  SonareHrtfSet* set = reinterpret_cast<SonareHrtfSet*>(uintptr_t{1});
  CHECK(sonare_hrtf_set_create_default(&set) == SONARE_ERROR_NOT_SUPPORTED);
  CHECK(set == nullptr);
  const uint8_t bytes[4] = {'S', 'H', 'R', 'F'};
  set = reinterpret_cast<SonareHrtfSet*>(uintptr_t{1});
  CHECK(sonare_hrtf_set_create_from_memory(bytes, sizeof(bytes), &set) ==
        SONARE_ERROR_NOT_SUPPORTED);
  CHECK(set == nullptr);
  sonare_hrtf_set_destroy(nullptr);

  SonarePlaybackRenderer* renderer = reinterpret_cast<SonarePlaybackRenderer*>(uintptr_t{1});
  CHECK(sonare_playback_renderer_create_json("{}", nullptr, 48000, 512, &renderer) ==
        SONARE_ERROR_NOT_SUPPORTED);
  CHECK(renderer == nullptr);
  sonare_playback_renderer_destroy(nullptr);
  CHECK(sonare_playback_renderer_reset(nullptr) == SONARE_ERROR_NOT_SUPPORTED);
  CHECK(sonare_playback_renderer_set_config_json(nullptr, "{}") == SONARE_ERROR_NOT_SUPPORTED);
  char* json = reinterpret_cast<char*>(uintptr_t{1});
  CHECK(sonare_playback_renderer_config_json(nullptr, &json) == SONARE_ERROR_NOT_SUPPORTED);
  CHECK(json == nullptr);
  CHECK(sonare_playback_renderer_set_head_orientation(nullptr, 10.0f, 0.0f, 0.0f) ==
        SONARE_ERROR_NOT_SUPPORTED);
  int value = -1;
  CHECK(sonare_playback_renderer_input_channel_count(nullptr, &value) ==
        SONARE_ERROR_NOT_SUPPORTED);
  CHECK(value == 0);
  value = -1;
  CHECK(sonare_playback_renderer_output_channel_count(nullptr, &value) ==
        SONARE_ERROR_NOT_SUPPORTED);
  CHECK(value == 0);
  std::vector<float> plane(16, 0.0f);
  const float* in[1] = {plane.data()};
  float* out[1] = {plane.data()};
  CHECK(sonare_playback_renderer_process_planar(nullptr, in, 1, out, 1, 16) ==
        SONARE_ERROR_NOT_SUPPORTED);
  std::vector<float> dst(16, 0.0f);
  CHECK(sonare_playback_renderer_process_interleaved(nullptr, plane.data(), 1, dst.data(), 1, 16) ==
        SONARE_ERROR_NOT_SUPPORTED);
  value = -1;
  CHECK(sonare_playback_renderer_latency_samples(nullptr, &value) == SONARE_ERROR_NOT_SUPPORTED);
  CHECK(value == 0);
  json = reinterpret_cast<char*>(uintptr_t{1});
  CHECK(sonare_playback_renderer_diagnostics_json(nullptr, &json) == SONARE_ERROR_NOT_SUPPORTED);
  CHECK(json == nullptr);
  uint32_t count = 7;
  CHECK(sonare_playback_renderer_non_finite_discard_count(nullptr, &count) ==
        SONARE_ERROR_NOT_SUPPORTED);
  CHECK(count == 0u);

  float* rendered = reinterpret_cast<float*>(uintptr_t{1});
  size_t rendered_frames = 5;
  int rendered_channels = 5;
  CHECK(sonare_playback_render_interleaved(plane.data(), 16, 1, 48000, "{}", nullptr, &rendered,
                                           &rendered_frames,
                                           &rendered_channels) == SONARE_ERROR_NOT_SUPPORTED);
  CHECK(rendered == nullptr);
  CHECK(rendered_frames == 0u);
  CHECK(rendered_channels == 0);
  sonare_free_playback_render(nullptr);

  SonarePlaybackLoudnessMeter* meter = reinterpret_cast<SonarePlaybackLoudnessMeter*>(uintptr_t{1});
  CHECK(sonare_playback_loudness_meter_create(2, 48000, &meter) == SONARE_ERROR_NOT_SUPPORTED);
  CHECK(meter == nullptr);
  CHECK(sonare_playback_loudness_meter_push_interleaved(nullptr, plane.data(), 8) ==
        SONARE_ERROR_NOT_SUPPORTED);
  float lufs = 1.0f;
  CHECK(sonare_playback_loudness_meter_integrated_lufs(nullptr, &lufs) ==
        SONARE_ERROR_NOT_SUPPORTED);
  CHECK(lufs == 0.0f);
  sonare_playback_loudness_meter_destroy(nullptr);
}

#endif
