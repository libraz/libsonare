#include <sonare/sonare_c.h>

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <cstdint>
#include <limits>
#include <string>
#include <utility>
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
  CHECK(sonare_playback_renderer_num_output_channels(renderer, &out_channels) == SONARE_OK);
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
        SONARE_ERROR_INVALID_FORMAT);
  CHECK(set == nullptr);
  CHECK(sonare_hrtf_set_create_from_memory(nullptr, 0, &set) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(sonare_hrtf_set_create_from_memory(malformed, sizeof(malformed), nullptr) ==
        SONARE_ERROR_INVALID_PARAMETER);

  sonare_hrtf_set_destroy(nullptr);
}

TEST_CASE("playback renderer creation resolves a zero block size to its default",
          "[playback][capi]") {
  SonarePlaybackRenderer* renderer = nullptr;
  CHECK(sonare_playback_renderer_create_json("{}", nullptr, 48000, -1, &renderer) ==
        SONARE_ERROR_INVALID_PARAMETER);
  SonarePlaybackRenderer* handle = create_renderer(headphones_config(), 48000, 0);

  std::vector<float> in(1025 * 2, 0.0f);
  std::vector<float> out(1025 * 2, 0.0f);
  CHECK(sonare_playback_renderer_process_interleaved(handle, in.data(), 2, out.data(), 2, 1024) ==
        SONARE_OK);
  CHECK(sonare_playback_renderer_process_interleaved(handle, in.data(), 2, out.data(), 2, 1025) ==
        SONARE_ERROR_INVALID_PARAMETER);
  sonare_playback_renderer_destroy(handle);
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
                                             &renderer) == SONARE_ERROR_INVALID_FORMAT);
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
        SONARE_ERROR_INVALID_FORMAT);
  CHECK(renderer == nullptr);
  CHECK(text_contains(sonare_last_error_message(), "byte budget"));

  // The same call with an ordinary config still creates a renderer, so the
  // refusal above is the size and not some other change to config handling.
  renderer = create_renderer(headphones_config());
  CHECK(sonare_playback_renderer_set_config_json(renderer, oversized.c_str()) ==
        SONARE_ERROR_INVALID_FORMAT);
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
            R"({"target": {"kind": "speakers", "layout": "5.1"}, "nightMode": {"amount": 1}})") ==
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
  // report nothing through the error channel), independently for every axis.
  const float nan = std::numeric_limits<float>::quiet_NaN();
  const float inf = std::numeric_limits<float>::infinity();
  CHECK(sonare_playback_renderer_set_head_orientation(renderer, nan, 0.0f, 0.0f) == SONARE_OK);
  CHECK(sonare_playback_renderer_set_head_orientation(renderer, 0.0f, nan, 0.0f) == SONARE_OK);
  CHECK(sonare_playback_renderer_set_head_orientation(renderer, 0.0f, 0.0f, nan) == SONARE_OK);
  CHECK(sonare_playback_renderer_set_head_orientation(renderer, inf, 0.0f, 0.0f) == SONARE_OK);
  CHECK(sonare_playback_renderer_set_head_orientation(renderer, 0.0f, inf, 0.0f) == SONARE_OK);
  CHECK(sonare_playback_renderer_set_head_orientation(renderer, 0.0f, 0.0f, -inf) == SONARE_OK);
  CHECK(sonare_playback_renderer_set_head_orientation(renderer, -inf, 0.0f, 0.0f) == SONARE_OK);
  CHECK(sonare_playback_renderer_set_head_orientation(renderer, 0.0f, -inf, 0.0f) == SONARE_OK);
  CHECK(sonare_playback_renderer_set_head_orientation(renderer, 0.0f, 0.0f, inf) == SONARE_OK);
  const float float32_max = std::numeric_limits<float>::max();
  for (const float value : {float32_max, -float32_max, 0.0f, -0.0f}) {
    CHECK(sonare_playback_renderer_set_head_orientation(renderer, value, 0.0f, 0.0f) == SONARE_OK);
    CHECK(sonare_playback_renderer_set_head_orientation(renderer, 0.0f, value, 0.0f) == SONARE_OK);
    CHECK(sonare_playback_renderer_set_head_orientation(renderer, 0.0f, 0.0f, value) == SONARE_OK);
  }
  CHECK(sonare_playback_renderer_set_head_orientation(renderer, 1440.0f, -720.0f, 360.0f) ==
        SONARE_OK);
  CHECK(sonare_playback_renderer_set_head_orientation(nullptr, 0.0f, 0.0f, 0.0f) ==
        SONARE_ERROR_INVALID_PARAMETER);
  CHECK(sonare_playback_renderer_set_head_orientation(nullptr, nan, 0.0f, 0.0f) ==
        SONARE_ERROR_INVALID_PARAMETER);
  sonare_playback_renderer_destroy(renderer);
}

TEST_CASE("playback renderer input channel count follows auto switches", "[playback][capi]") {
  SonarePlaybackRenderer* renderer = create_renderer(speakers_5_1_config());
  int in_channels = 0;
  CHECK(sonare_playback_renderer_num_input_channels(renderer, &in_channels) == SONARE_OK);
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
  CHECK(sonare_playback_renderer_num_input_channels(renderer, &in_channels) == SONARE_OK);
  CHECK(in_channels == 6);

  int out_channels = -1;
  CHECK(sonare_playback_renderer_num_input_channels(nullptr, &in_channels) ==
        SONARE_ERROR_INVALID_PARAMETER);
  CHECK(in_channels == 0);
  CHECK(sonare_playback_renderer_num_output_channels(nullptr, &out_channels) ==
        SONARE_ERROR_INVALID_PARAMETER);
  CHECK(out_channels == 0);

  sonare_playback_renderer_destroy(renderer);
}

TEST_CASE("playback renderer reports the widest input it accepts", "[playback][capi]") {
  int widest = -1;
  SonarePlaybackRenderer* automatic = create_renderer(speakers_5_1_config());
  CHECK(sonare_playback_renderer_max_input_channels(automatic, &widest) == SONARE_OK);
  CHECK(widest == 8);
  sonare_playback_renderer_destroy(automatic);

  SonarePlaybackRenderer* fixed = create_renderer(
      R"({"input": {"layout": "mono"}, "target": {"kind": "speakers", "layout": "5.1"}})");
  CHECK(sonare_playback_renderer_max_input_channels(fixed, &widest) == SONARE_OK);
  CHECK(widest == 1);
  sonare_playback_renderer_destroy(fixed);

  widest = -1;
  CHECK(sonare_playback_renderer_max_input_channels(nullptr, &widest) ==
        SONARE_ERROR_INVALID_PARAMETER);
  CHECK(widest == 0);
}

TEST_CASE("a playback config reports its output channel count without a renderer",
          "[playback][capi]") {
  for (const auto& [config, expected] :
       {std::pair<std::string, int>{headphones_config(), 2},
        std::pair<std::string, int>{speakers_5_1_config(), 6},
        std::pair<std::string, int>{R"({"target": {"kind": "speakers", "layout": "7.1"}})", 8}}) {
    CAPTURE(config);
    int from_config = -1;
    REQUIRE(sonare_playback_config_num_output_channels(config.c_str(), &from_config) == SONARE_OK);
    CHECK(from_config == expected);
    SonarePlaybackRenderer* renderer = create_renderer(config);
    int from_renderer = 0;
    REQUIRE(sonare_playback_renderer_num_output_channels(renderer, &from_renderer) == SONARE_OK);
    CHECK(from_config == from_renderer);
    sonare_playback_renderer_destroy(renderer);
  }

  const char* bad = R"({"target": {"kind": "speakers"}})";
  SonarePlaybackRenderer* refused = nullptr;
  const SonareError create_code =
      sonare_playback_renderer_create_json(bad, nullptr, 48000, 0, &refused);
  CHECK(create_code != SONARE_OK);
  int out = -1;
  CHECK(sonare_playback_config_num_output_channels(bad, &out) == create_code);
  CHECK(out == 0);
  CHECK(sonare_playback_config_num_output_channels(nullptr, &out) ==
        SONARE_ERROR_INVALID_PARAMETER);
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
  REQUIRE(sonare_playback_renderer_num_input_channels(renderer, &in_channels_before) == SONARE_OK);
  CHECK(sonare_playback_renderer_process_planar(renderer, three, 3, out, 6, kMaxBlock) ==
        SONARE_ERROR_INVALID_PARAMETER);
  int in_channels_after = 0;
  REQUIRE(sonare_playback_renderer_num_input_channels(renderer, &in_channels_after) == SONARE_OK);
  CHECK(in_channels_after == in_channels_before);
  CHECK(sonare_playback_renderer_process_planar(nullptr, in, 2, out, 6, kMaxBlock) ==
        SONARE_ERROR_INVALID_PARAMETER);

  // frames == 0 is a no-op that ignores a channel-count mismatch and never
  // switches the input layout.
  CHECK(sonare_playback_renderer_process_planar(renderer, three, 3, out, 5, 0) == SONARE_OK);
  REQUIRE(sonare_playback_renderer_num_input_channels(renderer, &in_channels_after) == SONARE_OK);
  CHECK(in_channels_after == in_channels_before);

  sonare_playback_renderer_destroy(renderer);
}

TEST_CASE("playback renderer process_silence renders without fixing the input layout",
          "[playback][capi]") {
  constexpr int kMaxBlock = 128;
  SonarePlaybackRenderer* renderer = create_renderer(speakers_5_1_config(), 48000, kMaxBlock);
  std::vector<std::vector<float>> planes(6, std::vector<float>(kMaxBlock, 1.0f));
  std::vector<float*> out;
  for (auto& plane : planes) out.push_back(plane.data());

  CHECK(sonare_playback_renderer_process_silence(renderer, out.data(), 6, kMaxBlock) == SONARE_OK);
  for (const auto& plane : planes) {
    for (float sample : plane) CHECK(sample == 0.0f);
  }
  CHECK(sonare_playback_renderer_process_silence(renderer, out.data(), 6, 0) == SONARE_OK);
  CHECK(sonare_playback_renderer_process_silence(renderer, out.data(), 2, kMaxBlock) ==
        SONARE_ERROR_INVALID_PARAMETER);
  CHECK(sonare_playback_renderer_process_silence(renderer, out.data(), 6, kMaxBlock + 1) ==
        SONARE_ERROR_INVALID_PARAMETER);
  CHECK(sonare_playback_renderer_process_silence(renderer, out.data(), 6, -1) ==
        SONARE_ERROR_INVALID_PARAMETER);
  CHECK(sonare_playback_renderer_process_silence(renderer, nullptr, 6, kMaxBlock) ==
        SONARE_ERROR_INVALID_PARAMETER);
  CHECK(sonare_playback_renderer_process_silence(nullptr, out.data(), 6, kMaxBlock) ==
        SONARE_ERROR_INVALID_PARAMETER);

  // The first block that carries input still sets the layout.
  std::vector<float> in(kMaxBlock * 6, 0.1f);
  std::vector<float> interleaved(kMaxBlock * 6, 0.0f);
  CHECK(sonare_playback_renderer_process_interleaved(renderer, in.data(), 6, interleaved.data(), 6,
                                                     kMaxBlock) == SONARE_OK);
  int in_channels = 0;
  CHECK(sonare_playback_renderer_num_input_channels(renderer, &in_channels) == SONARE_OK);
  CHECK(in_channels == 6);
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
  CHECK(text_contains(diagnostics, "activeInputLayout"));
  sonare_free_string(diagnostics);

  diagnostics = reinterpret_cast<char*>(uintptr_t{1});
  CHECK(sonare_playback_renderer_diagnostics_json(nullptr, &diagnostics) ==
        SONARE_ERROR_INVALID_PARAMETER);
  CHECK(diagnostics == nullptr);

  size_t discards = 123;
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
  sonare_free_floats(out);
  sonare_free_floats(nullptr);

  out = reinterpret_cast<float*>(uintptr_t{1});
  out_frames = 5;
  out_channels = 5;
  CHECK(sonare_playback_render_interleaved(in.data(), 256, 2, 48000, R"({"bad_key": 1})", nullptr,
                                           &out, &out_frames,
                                           &out_channels) == SONARE_ERROR_INVALID_FORMAT);
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

TEST_CASE("playback renderer render_offline matches the one-shot render through a used handle",
          "[playback][capi]") {
  // Unequal distances make the per-plane calibration delay part of the trim decision.
  const std::string config =
      R"({"target": {"kind": "speakers", "layout": "stereo",
                     "speakers": {"left": {"distanceM": 1.0}, "right": {"distanceM": 5.0}}},
          "outputLimiter": {"enabled": false}})";
  constexpr size_t kFrames = 6000;
  std::vector<float> in(kFrames * 2, 0.0f);
  for (size_t i = 0; i < 480; ++i) {
    const float v = 0.1f * std::sin(0.13f * static_cast<float>(i));
    in[2 * i] = v;
    in[2 * i + 1] = v;
  }

  float* expected = nullptr;
  size_t expected_frames = 0;
  int expected_channels = 0;
  REQUIRE(sonare_playback_render_interleaved(in.data(), kFrames, 2, 48000, config.c_str(), nullptr,
                                             &expected, &expected_frames,
                                             &expected_channels) == SONARE_OK);

  SonarePlaybackRenderer* renderer = create_renderer(config, 48000, 4096);
  // Leave state behind first: the offline render resets the handle before it starts.
  std::vector<float> scratch_out(512 * 2, 0.0f);
  REQUIRE(sonare_playback_renderer_process_interleaved(renderer, in.data(), 2, scratch_out.data(),
                                                       2, 512) == SONARE_OK);

  float* out = nullptr;
  size_t out_frames = 0;
  int out_channels = 0;
  REQUIRE(sonare_playback_renderer_render_offline(renderer, in.data(), kFrames, 2, &out,
                                                  &out_frames, &out_channels) == SONARE_OK);
  REQUIRE(out_frames == expected_frames);
  REQUIRE(out_channels == expected_channels);
  CHECK(std::equal(out, out + out_frames * static_cast<size_t>(out_channels), expected));
  sonare_free_floats(out);
  sonare_free_floats(expected);

  out = reinterpret_cast<float*>(uintptr_t{1});
  out_frames = 5;
  out_channels = 5;
  CHECK(sonare_playback_renderer_render_offline(nullptr, in.data(), kFrames, 2, &out, &out_frames,
                                                &out_channels) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(out == nullptr);
  CHECK(out_frames == 0u);
  CHECK(out_channels == 0);
  CHECK(sonare_playback_renderer_render_offline(renderer, nullptr, 0, 2, &out, &out_frames,
                                                &out_channels) == SONARE_ERROR_INVALID_PARAMETER);
  const std::vector<float> in3(256 * 3, 0.1f);
  CHECK(sonare_playback_renderer_render_offline(renderer, in3.data(), 256, 3, &out, &out_frames,
                                                &out_channels) == SONARE_ERROR_INVALID_PARAMETER);
  std::vector<float> non_finite(256 * 2, 0.1f);
  non_finite[7] = std::numeric_limits<float>::quiet_NaN();
  CHECK(sonare_playback_renderer_render_offline(renderer, non_finite.data(), 256, 2, &out,
                                                &out_frames,
                                                &out_channels) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(out == nullptr);
  sonare_playback_renderer_destroy(renderer);
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
  // 4096 frames is 85 ms, short of the 400 ms window the first gating block needs.
  CHECK(std::isnan(lufs));

  for (int push = 0; push < 5; ++push) {
    CHECK(sonare_playback_loudness_meter_push_interleaved(meter, block.data(), 4096) == SONARE_OK);
  }
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
  CHECK(sonare_playback_renderer_num_input_channels(nullptr, &value) == SONARE_ERROR_NOT_SUPPORTED);
  CHECK(value == 0);
  value = -1;
  CHECK(sonare_playback_renderer_num_output_channels(nullptr, &value) ==
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
  size_t count = 7;
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
  sonare_free_floats(nullptr);
  CHECK(sonare_playback_renderer_render_offline(nullptr, plane.data(), 16, 1, &rendered,
                                                &rendered_frames,
                                                &rendered_channels) == SONARE_ERROR_NOT_SUPPORTED);

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

TEST_CASE("a NULL argument is refused by its parameter name", "[playback][capi]") {
  int channels = -1;
  CHECK(sonare_playback_config_num_output_channels(nullptr, &channels) ==
        SONARE_ERROR_INVALID_PARAMETER);
  CHECK(std::string(sonare_last_error_message()) == "document must not be NULL");
  CHECK(channels == 0);

  SonarePlaybackRenderer* renderer = create_renderer(headphones_config());
  CHECK(sonare_playback_renderer_max_input_channels(renderer, nullptr) ==
        SONARE_ERROR_INVALID_PARAMETER);
  CHECK(std::string(sonare_last_error_message()) == "out must not be NULL");
  sonare_playback_renderer_destroy(renderer);

  SonarePlaybackLoudnessMeter* meter = nullptr;
  REQUIRE(sonare_playback_loudness_meter_create(2, 48000, &meter) == SONARE_OK);
  CHECK(sonare_playback_loudness_meter_push_interleaved(meter, nullptr, 4) ==
        SONARE_ERROR_INVALID_PARAMETER);
  CHECK(std::string(sonare_last_error_message()) == "in must not be NULL when frames is not 0");
  // No samples to read, so a NULL buffer is accepted and the last error is cleared.
  CHECK(sonare_playback_loudness_meter_push_interleaved(meter, nullptr, 0) == SONARE_OK);
  CHECK(sonare_last_error_code() == SONARE_OK);
  sonare_playback_loudness_meter_destroy(meter);
}

#endif
