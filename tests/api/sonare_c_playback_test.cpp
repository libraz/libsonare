#include <sonare/sonare_c.h>

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <cstdint>
#include <limits>
#include <string>
#include <vector>

#if defined(SONARE_WITH_PLAYBACK)

namespace {

std::string headphones_config() { return "{}"; }
std::string speakers_5_1_config() { return R"({"target": {"kind": "speakers", "layout": "5.1"}})"; }

SonarePlaybackRenderer* create_renderer(const std::string& config, int sample_rate = 48000,
                                        int max_block_size = 512) {
  SonarePlaybackRenderer* renderer = nullptr;
  REQUIRE(sonare_playback_renderer_create_json(config.c_str(), nullptr, sample_rate, max_block_size,
                                               &renderer) == SONARE_OK);
  REQUIRE(renderer != nullptr);
  return renderer;
}

bool text_contains(const std::string& text, const std::string& needle) {
  return text.find(needle) != std::string::npos;
}

}  // namespace

TEST_CASE("playback renderer handle reports its shape", "[playback][capi]") {
  SonarePlaybackRenderer* renderer = create_renderer(speakers_5_1_config());
  int out_channels = 0;
  CHECK(sonare_playback_renderer_output_channel_count(renderer, &out_channels) == SONARE_OK);
  CHECK(out_channels == 6);
  int latency = 0;
  CHECK(sonare_playback_renderer_latency_samples(renderer, &latency) == SONARE_OK);
  CHECK(latency == 1312);
  sonare_playback_renderer_destroy(renderer);
}

#ifndef __EMSCRIPTEN__
TEST_CASE("sonare_hrtf_set_create_default builds the embedded set", "[playback][capi]") {
  SonareHrtfSet* set = nullptr;
  REQUIRE(sonare_hrtf_set_create_default(&set) == SONARE_OK);
  REQUIRE(set != nullptr);
  sonare_hrtf_set_destroy(set);

  CHECK(sonare_hrtf_set_create_default(nullptr) == SONARE_ERROR_INVALID_PARAMETER);
}
#endif

TEST_CASE("sonare_hrtf_set_create_from_memory validates SHRF bytes", "[playback][capi]") {
  // Malformed data (too short to hold a SHRF v1 header) is rejected; building
  // valid bytes belongs to the SHRF-format tests, which own the writer.
  const uint8_t malformed[8] = {'S', 'H', 'R', 'F', 0, 0, 0, 0};
  SonareHrtfSet* set = reinterpret_cast<SonareHrtfSet*>(uintptr_t{1});
  CHECK(sonare_hrtf_set_create_from_memory(malformed, sizeof(malformed), &set) ==
        SONARE_ERROR_INVALID_PARAMETER);
  CHECK(set == nullptr);
  CHECK(sonare_hrtf_set_create_from_memory(nullptr, 0, &set) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(sonare_hrtf_set_create_from_memory(malformed, sizeof(malformed), nullptr) ==
        SONARE_ERROR_INVALID_PARAMETER);

  sonare_hrtf_set_destroy(nullptr);
}

TEST_CASE("playback renderer creation rejects malformed configuration", "[playback][capi]") {
  SonarePlaybackRenderer* renderer = reinterpret_cast<SonarePlaybackRenderer*>(uintptr_t{1});
  CHECK(sonare_playback_renderer_create_json(nullptr, nullptr, 48000, 512, &renderer) ==
        SONARE_ERROR_INVALID_PARAMETER);
  CHECK(renderer == nullptr);
  CHECK(sonare_playback_renderer_create_json("{}", nullptr, 48000, 512, nullptr) ==
        SONARE_ERROR_INVALID_PARAMETER);

  renderer = reinterpret_cast<SonarePlaybackRenderer*>(uintptr_t{1});
  CHECK(sonare_playback_renderer_create_json(R"({"not_a_key": 1})", nullptr, 48000, 512,
                                             &renderer) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(renderer == nullptr);
  CHECK(text_contains(sonare_last_error_message(), "unknown key"));
}

TEST_CASE("playback renderer config exceeding the JSON byte budget is refused, not parsed",
          "[playback][capi][resource]") {
  // One JSON string value past the 64 MiB config budget. The refusal has to
  // come from the byte precheck, not from the parser walking a document this
  // size into json::Value nodes first.
  const std::string oversized =
      "{\"pad\": \"" + std::string(64u * 1024u * 1024u + 1024u, 'a') + "\"}";

  SonarePlaybackRenderer* renderer = reinterpret_cast<SonarePlaybackRenderer*>(uintptr_t{1});
  CHECK(sonare_playback_renderer_create_json(oversized.c_str(), nullptr, 48000, 512, &renderer) ==
        SONARE_ERROR_INVALID_PARAMETER);
  CHECK(renderer == nullptr);
  CHECK(text_contains(sonare_last_error_message(), "byte budget"));

  // The same call with an ordinary config still creates a renderer, so the
  // refusal above is the size and not some other change to config handling.
  renderer = create_renderer(headphones_config());
  CHECK(sonare_playback_renderer_set_config_json(renderer, oversized.c_str()) ==
        SONARE_ERROR_INVALID_PARAMETER);
  CHECK(text_contains(sonare_last_error_message(), "byte budget"));
  sonare_playback_renderer_destroy(renderer);
}

TEST_CASE("playback renderer set_config_json adopts realtime keys and rejects a prepare change",
          "[playback][capi]") {
  SonarePlaybackRenderer* renderer = create_renderer(speakers_5_1_config());

  CHECK(sonare_playback_renderer_set_config_json(nullptr, "{}") == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(sonare_playback_renderer_set_config_json(renderer, nullptr) ==
        SONARE_ERROR_INVALID_PARAMETER);

  // A realtime-only edit (night_mode.amount) on a document that keeps the
  // renderer's prepare shape (target.kind/layout) is accepted.
  CHECK(sonare_playback_renderer_set_config_json(
            renderer,
            R"({"target": {"kind": "speakers", "layout": "5.1"}, "night_mode": {"amount": 1}})") ==
        SONARE_OK);

  // A document whose target.kind differs from the renderer's prepare shape is
  // rejected, naming the offending key.
  CHECK(sonare_playback_renderer_set_config_json(renderer, headphones_config().c_str()) ==
        SONARE_ERROR_INVALID_PARAMETER);
  CHECK(text_contains(sonare_last_error_message(), "requires a new renderer"));

  sonare_playback_renderer_destroy(renderer);
}

TEST_CASE("playback renderer config_json returns the current document", "[playback][capi]") {
  SonarePlaybackRenderer* renderer = create_renderer(speakers_5_1_config());

  char* current = nullptr;
  REQUIRE(sonare_playback_renderer_config_json(renderer, &current) == SONARE_OK);
  REQUIRE(current != nullptr);
  CHECK(text_contains(current, "\"kind\":\"speakers\""));
  sonare_free_string(current);

  current = reinterpret_cast<char*>(uintptr_t{1});
  CHECK(sonare_playback_renderer_config_json(nullptr, &current) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(current == nullptr);
  CHECK(sonare_playback_renderer_config_json(renderer, nullptr) == SONARE_ERROR_INVALID_PARAMETER);

  sonare_playback_renderer_destroy(renderer);
}

TEST_CASE("playback renderer set_head_orientation is realtime-safe", "[playback][capi]") {
  SonarePlaybackRenderer* renderer = create_renderer(headphones_config());
  CHECK(sonare_playback_renderer_set_head_orientation(renderer, 30.0f, -10.0f, 5.0f) == SONARE_OK);
  // Non-finite angles are silently ignored rather than rejected (RT entries
  // report nothing through the error channel).
  CHECK(sonare_playback_renderer_set_head_orientation(
            renderer, std::numeric_limits<float>::quiet_NaN(), 0.0f, 0.0f) == SONARE_OK);
  CHECK(sonare_playback_renderer_set_head_orientation(nullptr, 0.0f, 0.0f, 0.0f) ==
        SONARE_ERROR_INVALID_PARAMETER);
  sonare_playback_renderer_destroy(renderer);
}

TEST_CASE("playback renderer input channel count follows auto switches", "[playback][capi]") {
  SonarePlaybackRenderer* renderer = create_renderer(speakers_5_1_config());
  int in_channels = 0;
  CHECK(sonare_playback_renderer_input_channel_count(renderer, &in_channels) == SONARE_OK);
  CHECK(in_channels == 2);

  std::vector<float> planes(6 * 64, 0.0f);
  std::vector<const float*> in_ptrs;
  std::vector<float*> out_ptrs;
  for (int c = 0; c < 6; ++c) {
    in_ptrs.push_back(planes.data() + static_cast<size_t>(c) * 64);
    out_ptrs.push_back(planes.data() + static_cast<size_t>(c) * 64);
  }
  CHECK(sonare_playback_renderer_process_planar(renderer, in_ptrs.data(), 6, out_ptrs.data(), 6,
                                                64) == SONARE_OK);
  CHECK(sonare_playback_renderer_input_channel_count(renderer, &in_channels) == SONARE_OK);
  CHECK(in_channels == 6);

  int out_channels = -1;
  CHECK(sonare_playback_renderer_input_channel_count(nullptr, &in_channels) ==
        SONARE_ERROR_INVALID_PARAMETER);
  CHECK(in_channels == 0);
  CHECK(sonare_playback_renderer_output_channel_count(nullptr, &out_channels) ==
        SONARE_ERROR_INVALID_PARAMETER);
  CHECK(out_channels == 0);

  sonare_playback_renderer_destroy(renderer);
}

TEST_CASE("playback renderer process_planar renders and rejects without advancing state",
          "[playback][capi]") {
  constexpr int kMaxBlock = 128;
  SonarePlaybackRenderer* renderer = create_renderer(speakers_5_1_config(), 48000, kMaxBlock);

  std::vector<float> in_l(kMaxBlock, 0.25f), in_r(kMaxBlock, -0.25f);
  const float* in[2] = {in_l.data(), in_r.data()};
  std::vector<std::vector<float>> out_storage(6, std::vector<float>(kMaxBlock, 0.0f));
  float* out[6];
  for (int c = 0; c < 6; ++c) out[c] = out_storage[static_cast<size_t>(c)].data();

  REQUIRE(sonare_playback_renderer_process_planar(renderer, in, 2, out, 6, kMaxBlock) == SONARE_OK);
  for (const auto& plane : out_storage) {
    for (float sample : plane) CHECK(std::isfinite(sample));
  }

  // A wrong output channel count is rejected and must not touch the buffers.
  for (auto& plane : out_storage) std::fill(plane.begin(), plane.end(), -7.0f);
  CHECK(sonare_playback_renderer_process_planar(renderer, in, 2, out, 5, kMaxBlock) ==
        SONARE_ERROR_INVALID_PARAMETER);
  for (const auto& plane : out_storage) {
    for (float sample : plane) CHECK(sample == -7.0f);
  }

  // frames beyond the max block size, an unsupported auto channel count, and a
  // null handle are all rejected the same way.
  CHECK(sonare_playback_renderer_process_planar(renderer, in, 2, out, 6, kMaxBlock + 1) ==
        SONARE_ERROR_INVALID_PARAMETER);
  const float* three[3] = {in_l.data(), in_r.data(), in_l.data()};
  int in_channels_before = 0;
  REQUIRE(sonare_playback_renderer_input_channel_count(renderer, &in_channels_before) == SONARE_OK);
  CHECK(sonare_playback_renderer_process_planar(renderer, three, 3, out, 6, kMaxBlock) ==
        SONARE_ERROR_INVALID_PARAMETER);
  int in_channels_after = 0;
  REQUIRE(sonare_playback_renderer_input_channel_count(renderer, &in_channels_after) == SONARE_OK);
  CHECK(in_channels_after == in_channels_before);
  CHECK(sonare_playback_renderer_process_planar(nullptr, in, 2, out, 6, kMaxBlock) ==
        SONARE_ERROR_INVALID_PARAMETER);

  // frames == 0 is a no-op that ignores a channel-count mismatch and never
  // switches the input layout.
  CHECK(sonare_playback_renderer_process_planar(renderer, three, 3, out, 5, 0) == SONARE_OK);
  REQUIRE(sonare_playback_renderer_input_channel_count(renderer, &in_channels_after) == SONARE_OK);
  CHECK(in_channels_after == in_channels_before);

  sonare_playback_renderer_destroy(renderer);
}

TEST_CASE("playback renderer process_interleaved rejects aliasing", "[playback][capi]") {
  constexpr int kMaxBlock = 128;
  SonarePlaybackRenderer* renderer = create_renderer(speakers_5_1_config(), 48000, kMaxBlock);

  std::vector<float> shared(kMaxBlock * 6, 0.1f);
  CHECK(sonare_playback_renderer_process_interleaved(renderer, shared.data(), 2, shared.data(), 6,
                                                     kMaxBlock) == SONARE_ERROR_INVALID_PARAMETER);

  std::vector<float> in(kMaxBlock * 2, 0.1f);
  std::vector<float> out(kMaxBlock * 6, 0.0f);
  CHECK(sonare_playback_renderer_process_interleaved(renderer, in.data(), 2, out.data(), 6,
                                                     kMaxBlock) == SONARE_OK);
  for (float sample : out) CHECK(std::isfinite(sample));
  CHECK(sonare_playback_renderer_process_interleaved(nullptr, in.data(), 2, out.data(), 6,
                                                     kMaxBlock) == SONARE_ERROR_INVALID_PARAMETER);

  sonare_playback_renderer_destroy(renderer);
}

TEST_CASE("playback renderer reset clears state and reports SONARE_OK", "[playback][capi]") {
  SonarePlaybackRenderer* renderer = create_renderer(headphones_config());
  CHECK(sonare_playback_renderer_reset(renderer) == SONARE_OK);
  CHECK(sonare_playback_renderer_reset(nullptr) == SONARE_ERROR_INVALID_PARAMETER);
  sonare_playback_renderer_destroy(renderer);
}

TEST_CASE("playback renderer latency_samples matches the design constants", "[playback][capi]") {
  SonarePlaybackRenderer* headphones = create_renderer(headphones_config());
  int latency = 0;
  CHECK(sonare_playback_renderer_latency_samples(headphones, &latency) == SONARE_OK);
  CHECK(latency == 1312);
  sonare_playback_renderer_destroy(headphones);

  CHECK(sonare_playback_renderer_latency_samples(nullptr, &latency) ==
        SONARE_ERROR_INVALID_PARAMETER);
  CHECK(latency == 0);
}

TEST_CASE("playback renderer diagnostics_json and non_finite_discard_count", "[playback][capi]") {
  constexpr int kMaxBlock = 128;
  SonarePlaybackRenderer* renderer = create_renderer(speakers_5_1_config(), 48000, kMaxBlock);

  char* diagnostics = nullptr;
  REQUIRE(sonare_playback_renderer_diagnostics_json(renderer, &diagnostics) == SONARE_OK);
  REQUIRE(diagnostics != nullptr);
  CHECK(text_contains(diagnostics, "active_input_layout"));
  sonare_free_string(diagnostics);

  diagnostics = reinterpret_cast<char*>(uintptr_t{1});
  CHECK(sonare_playback_renderer_diagnostics_json(nullptr, &diagnostics) ==
        SONARE_ERROR_INVALID_PARAMETER);
  CHECK(diagnostics == nullptr);

  uint32_t discards = 123;
  CHECK(sonare_playback_renderer_non_finite_discard_count(renderer, &discards) == SONARE_OK);
  CHECK(discards == 0u);

  std::vector<float> in(kMaxBlock * 2, 0.0f);
  in[3] = std::numeric_limits<float>::quiet_NaN();
  std::vector<float> out(kMaxBlock * 6, 0.0f);
  CHECK(sonare_playback_renderer_process_interleaved(renderer, in.data(), 2, out.data(), 6,
                                                     kMaxBlock) == SONARE_OK);
  CHECK(sonare_playback_renderer_non_finite_discard_count(renderer, &discards) == SONARE_OK);
  CHECK(discards == 1u);

  CHECK(sonare_playback_renderer_non_finite_discard_count(nullptr, &discards) ==
        SONARE_ERROR_INVALID_PARAMETER);
  CHECK(discards == 0u);

  sonare_playback_renderer_destroy(renderer);
}

TEST_CASE("sonare_playback_render_interleaved renders offline and validates its input",
          "[playback][capi]") {
  const std::vector<float> in(256 * 2, 0.1f);
  float* out = nullptr;
  size_t out_frames = 0;
  int out_channels = 0;
  REQUIRE(sonare_playback_render_interleaved(in.data(), 256, 2, 48000, headphones_config().c_str(),
                                             nullptr, &out, &out_frames,
                                             &out_channels) == SONARE_OK);
  REQUIRE(out != nullptr);
  CHECK(out_frames == 256u);
  CHECK(out_channels == 2);
  for (size_t i = 0; i < out_frames * static_cast<size_t>(out_channels); ++i) {
    CHECK(std::isfinite(out[i]));
  }
  sonare_free_playback_render(out);
  sonare_free_playback_render(nullptr);

  out = reinterpret_cast<float*>(uintptr_t{1});
  out_frames = 5;
  out_channels = 5;
  CHECK(sonare_playback_render_interleaved(in.data(), 256, 2, 48000, R"({"bad_key": 1})", nullptr,
                                           &out, &out_frames,
                                           &out_channels) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(out == nullptr);
  CHECK(out_frames == 0u);
  CHECK(out_channels == 0);

  CHECK(sonare_playback_render_interleaved(in.data(), 256, 2, 48000, nullptr, nullptr, &out,
                                           &out_frames,
                                           &out_channels) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(sonare_playback_render_interleaved(in.data(), 256, 2, 48000, headphones_config().c_str(),
                                           nullptr, nullptr, &out_frames,
                                           &out_channels) == SONARE_ERROR_INVALID_PARAMETER);

  // A fixed input layout that the channel count does not match is rejected.
  const std::vector<float> in3(256 * 3, 0.1f);
  CHECK(sonare_playback_render_interleaved(
            in3.data(), 256, 3, 48000, R"({"input": {"layout": "stereo"}})", nullptr, &out,
            &out_frames, &out_channels) == SONARE_ERROR_INVALID_PARAMETER);

  // Offline entry point: an empty or non-finite input is refused, the same
  // contract every other offline buffer-taking entry point holds.
  out = reinterpret_cast<float*>(uintptr_t{1});
  out_frames = 5;
  out_channels = 5;
  CHECK(sonare_playback_render_interleaved(nullptr, 0, 2, 48000, headphones_config().c_str(),
                                           nullptr, &out, &out_frames,
                                           &out_channels) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(out == nullptr);
  CHECK(out_frames == 0u);
  CHECK(out_channels == 0);

  std::vector<float> non_finite = in;
  non_finite[7] = std::numeric_limits<float>::quiet_NaN();
  out = reinterpret_cast<float*>(uintptr_t{1});
  CHECK(sonare_playback_render_interleaved(non_finite.data(), 256, 2, 48000,
                                           headphones_config().c_str(), nullptr, &out, &out_frames,
                                           &out_channels) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(out == nullptr);
}

TEST_CASE("playback loudness meter integrates pushed frames", "[playback][capi]") {
  SonarePlaybackLoudnessMeter* meter = nullptr;
  REQUIRE(sonare_playback_loudness_meter_create(2, 48000, &meter) == SONARE_OK);
  REQUIRE(meter != nullptr);

  std::vector<float> block(4096 * 2, 0.0f);
  for (size_t i = 0; i < block.size(); i += 2) {
    const float sample = 0.2f * std::sin(0.05f * static_cast<float>(i));
    block[i] = sample;
    block[i + 1] = sample;
  }
  CHECK(sonare_playback_loudness_meter_push_interleaved(meter, block.data(), 4096) == SONARE_OK);

  float lufs = 0.0f;
  CHECK(sonare_playback_loudness_meter_integrated_lufs(meter, &lufs) == SONARE_OK);
  CHECK(std::isfinite(lufs));
  CHECK(lufs < 0.0f);

  sonare_playback_loudness_meter_destroy(meter);

  SonarePlaybackLoudnessMeter* invalid =
      reinterpret_cast<SonarePlaybackLoudnessMeter*>(uintptr_t{1});
  CHECK(sonare_playback_loudness_meter_create(3, 48000, &invalid) ==
        SONARE_ERROR_INVALID_PARAMETER);
  CHECK(invalid == nullptr);
  CHECK(sonare_playback_loudness_meter_create(2, 48000, nullptr) == SONARE_ERROR_INVALID_PARAMETER);

  CHECK(sonare_playback_loudness_meter_push_interleaved(nullptr, block.data(), 8) ==
        SONARE_ERROR_INVALID_PARAMETER);

  // Offline entry point: an empty or non-finite push is refused, the same
  // contract every other offline buffer-taking entry point holds.
  SonarePlaybackLoudnessMeter* live = nullptr;
  REQUIRE(sonare_playback_loudness_meter_create(2, 48000, &live) == SONARE_OK);
  CHECK(sonare_playback_loudness_meter_push_interleaved(live, block.data(), 0) ==
        SONARE_ERROR_INVALID_PARAMETER);
  std::vector<float> non_finite_block = block;
  non_finite_block[9] = std::numeric_limits<float>::quiet_NaN();
  CHECK(sonare_playback_loudness_meter_push_interleaved(live, non_finite_block.data(), 4096) ==
        SONARE_ERROR_INVALID_PARAMETER);
  sonare_playback_loudness_meter_destroy(live);

  lufs = 1.0f;
  CHECK(sonare_playback_loudness_meter_integrated_lufs(nullptr, &lufs) ==
        SONARE_ERROR_INVALID_PARAMETER);
  CHECK(lufs == 0.0f);
  sonare_playback_loudness_meter_destroy(nullptr);
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
