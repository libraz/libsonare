/// @file project_bounce_overflow_test.cpp
/// @brief Regression tests for checked project-bounce frame arithmetic.

#include <sonare/sonare_c_mixing.h>
#include <sonare/sonare_c_project.h>

#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <limits>

#include "core/audio.h"

namespace {

struct CallbackState {
  int render_calls = 0;
};

void count_render(void* user_data, float* const* /*channels*/, int /*num_channels*/,
                  int /*num_frames*/) {
  ++static_cast<CallbackState*>(user_data)->render_calls;
}

SonareProject* make_midi_project(double start_ppq, uint32_t destination_id, uint32_t* out_track,
                                 uint32_t* out_clip) {
  SonareProject* project = nullptr;
  if (sonare_project_create(&project) != SONARE_OK ||
      sonare_project_set_sample_rate(project, 48000.0) != SONARE_OK ||
      sonare_project_add_midi_clip(project, start_ppq, 1.0, out_track, out_clip) != SONARE_OK ||
      sonare_project_set_track_midi_destination(project, *out_track, destination_id) != SONARE_OK) {
    sonare_project_destroy(project);
    return nullptr;
  }
  return project;
}

SonareProjectBounceOptions bounce_options(int64_t total_frames) {
  SonareProjectBounceOptions options{};
  options.total_frames = total_frames;
  options.block_size = 128;
  options.num_channels = 2;
  options.sample_rate = 48000;
  return options;
}

SonareInstrumentBinding callback_binding(CallbackState* state, int latency_samples = 0,
                                         int tail_samples = 0) {
  SonareInstrumentBinding binding{};
  binding.destination_id = 7;
  binding.callbacks.user_data = state;
  binding.callbacks.render = &count_render;
  binding.callbacks.latency_samples = latency_samples;
  binding.callbacks.tail_samples = tail_samples;
  return binding;
}

}  // namespace

TEST_CASE("project bounce rejects an arrangement event endpoint overflow", "[project][overflow]") {
  uint32_t track = 0;
  uint32_t clip = 0;
  SonareProject* project = make_midi_project(std::numeric_limits<double>::max(), 7, &track, &clip);
  REQUIRE(project != nullptr);

  const SonareMidiEventPod event{0.0, 0x20903C40u, 0u};
  REQUIRE(sonare_project_set_midi_events(project, clip, &event, 1) == SONARE_OK);

  CallbackState state;
  const SonareInstrumentBinding binding = callback_binding(&state);
  float* out = reinterpret_cast<float*>(static_cast<uintptr_t>(0x1));
  size_t out_len = 1;
  const SonareProjectBounceOptions options = bounce_options(std::numeric_limits<int64_t>::min());
  REQUIRE(sonare_project_bounce_with_instruments(project, &options, &binding, 1, &out, &out_len) ==
          SONARE_ERROR_INVALID_PARAMETER);
  REQUIRE(out == nullptr);
  REQUIRE(out_len == 0);
  REQUIRE(state.render_calls == 0);
  sonare_project_destroy(project);
}

TEST_CASE("project bounce rejects arrangement plus instrument-tail overflow",
          "[project][overflow]") {
  uint32_t track = 0;
  uint32_t clip = 0;
  SonareProject* project = make_midi_project(std::numeric_limits<double>::max(), 7, &track, &clip);
  REQUIRE(project != nullptr);

  CallbackState state;
  const SonareInstrumentBinding binding = callback_binding(&state, 0, 1);
  float* out = reinterpret_cast<float*>(static_cast<uintptr_t>(0x1));
  size_t out_len = 1;
  const SonareProjectBounceOptions options = bounce_options(std::numeric_limits<int64_t>::min());
  REQUIRE(sonare_project_bounce_with_instruments(project, &options, &binding, 1, &out, &out_len) ==
          SONARE_ERROR_INVALID_PARAMETER);
  REQUIRE(out == nullptr);
  REQUIRE(out_len == 0);
  REQUIRE(state.render_calls == 0);
  sonare_project_destroy(project);
}

TEST_CASE("project bounce rejects total-frames plus PDC overflow", "[project][overflow]") {
  uint32_t track = 0;
  uint32_t clip = 0;
  SonareProject* project = make_midi_project(0.0, 7, &track, &clip);
  REQUIRE(project != nullptr);

  CallbackState state;
  const SonareInstrumentBinding binding = callback_binding(&state, 1);
  float* out = reinterpret_cast<float*>(static_cast<uintptr_t>(0x1));
  size_t out_len = 1;
  const SonareProjectBounceOptions options = bounce_options(std::numeric_limits<int64_t>::max());
  REQUIRE(sonare_project_bounce_with_instruments(project, &options, &binding, 1, &out, &out_len) ==
          SONARE_ERROR_INVALID_PARAMETER);
  REQUIRE(out == nullptr);
  REQUIRE(out_len == 0);
  REQUIRE(state.render_calls == 0);
  sonare_project_destroy(project);
}

TEST_CASE("project bounce rejects negative host-instrument latency", "[project][overflow]") {
  uint32_t track = 0;
  uint32_t clip = 0;
  SonareProject* project = make_midi_project(0.0, 7, &track, &clip);
  REQUIRE(project != nullptr);

  CallbackState state;
  const SonareInstrumentBinding binding = callback_binding(&state, -1);
  float* out = reinterpret_cast<float*>(static_cast<uintptr_t>(0x1));
  size_t out_len = 1;
  const SonareProjectBounceOptions options = bounce_options(128);
  REQUIRE(sonare_project_bounce_with_instruments(project, &options, &binding, 1, &out, &out_len) ==
          SONARE_ERROR_INVALID_PARAMETER);
  REQUIRE(out == nullptr);
  REQUIRE(out_len == 0);
  REQUIRE(state.render_calls == 0);
  sonare_project_destroy(project);
}

TEST_CASE("project bounce keeps the nonpositive total-frames sentinel including INT64_MIN",
          "[project][overflow]") {
  SonareProject* project = nullptr;
  REQUIRE(sonare_project_create(&project) == SONARE_OK);

  float* out = nullptr;
  size_t out_len = 0;
  const SonareProjectBounceOptions options = bounce_options(std::numeric_limits<int64_t>::min());
  REQUIRE(sonare_project_bounce(project, &options, &out, &out_len) == SONARE_OK);
  REQUIRE(out != nullptr);
  REQUIRE(out_len == 0);
  sonare_free_floats(out);
  sonare_project_destroy(project);

  uint32_t track = 0;
  uint32_t clip = 0;
  project = make_midi_project(0.0, 7, &track, &clip);
  REQUIRE(project != nullptr);
  const SonareMidiEventPod event{0.0, 0x20903C40u, 0u};
  REQUIRE(sonare_project_set_midi_events(project, clip, &event, 1) == SONARE_OK);
  SonareBuiltinInstrumentBinding binding{};
  binding.destination_id = 7;
  out = nullptr;
  out_len = 0;
  REQUIRE(sonare_project_bounce_with_builtin_instruments(project, &options, &binding, 1, &out,
                                                         &out_len) == SONARE_OK);
  REQUIRE(out != nullptr);
  REQUIRE(out_len > 0);
  sonare_free_floats(out);
  sonare_project_destroy(project);
}

TEST_CASE("project bounce rejects oversized channel-strip stem shapes", "[project][overflow]") {
#if !defined(SONARE_WITH_MIXING)
  SUCCEED("channel-strip bounce requires the mixing build");
#else
  SonareProject* project = nullptr;
  REQUIRE(sonare_project_create(&project) == SONARE_OK);
  REQUIRE(sonare_project_set_sample_rate(project, 48000.0) == SONARE_OK);
  const char* scene =
      R"({"version":1,"buses":[{"id":"master","role":"master"}],"strips":[{"id":"a"},{"id":"b"},{"id":"c"}]})";
  REQUIRE(sonare_project_set_mixer_scene_json(project, scene) == SONARE_OK);

  constexpr uint32_t kDestination = 19;
  for (const char* route : {"a", "b", "c"}) {
    uint32_t track = 0;
    uint32_t clip = 0;
    REQUIRE(sonare_project_add_midi_clip(project, 0.0, 1.0, &track, &clip) == SONARE_OK);
    REQUIRE(sonare_project_set_track_midi_destination(project, track, kDestination) == SONARE_OK);
    REQUIRE(sonare_project_set_track_route(project, track, route, "") == SONARE_OK);
  }

  SonareBuiltinInstrumentBinding binding{};
  binding.destination_id = kDestination;
  float* out = reinterpret_cast<float*>(static_cast<uintptr_t>(0x1));
  size_t out_len = 1;
  const int64_t frames = static_cast<int64_t>(sonare::kMaxAudioBufferSize / 2u);
  const SonareProjectBounceOptions options = bounce_options(frames);
  REQUIRE(sonare_project_bounce_with_builtin_instruments(
              project, &options, &binding, 1, &out, &out_len) == SONARE_ERROR_INVALID_PARAMETER);
  REQUIRE(out == nullptr);
  REQUIRE(out_len == 0);
  sonare_project_destroy(project);
#endif
}

TEST_CASE("project bounce rejects oversized MIDI source-stem shapes", "[project][overflow]") {
#if !defined(SONARE_WITH_MIXING)
  SUCCEED("source-aware channel-strip bounce requires the mixing build");
#else
  SonareProject* project = nullptr;
  REQUIRE(sonare_project_create(&project) == SONARE_OK);
  REQUIRE(sonare_project_set_sample_rate(project, 48000.0) == SONARE_OK);
  const char* scene =
      R"({"version":1,"buses":[{"id":"master","role":"master"}],"strips":[{"id":"shared"}]})";
  REQUIRE(sonare_project_set_mixer_scene_json(project, scene) == SONARE_OK);

  constexpr uint32_t kDestination = 29;
  for (int i = 0; i < 3; ++i) {
    uint32_t track = 0;
    uint32_t clip = 0;
    REQUIRE(sonare_project_add_midi_clip(project, 0.0, 1.0, &track, &clip) == SONARE_OK);
    REQUIRE(sonare_project_set_track_midi_destination(project, track, kDestination) == SONARE_OK);
    REQUIRE(sonare_project_set_track_route(project, track, "shared", "") == SONARE_OK);
  }

  SonareBuiltinInstrumentBinding binding{};
  binding.destination_id = kDestination;
  float* out = reinterpret_cast<float*>(static_cast<uintptr_t>(0x1));
  size_t out_len = 1;
  const int64_t frames = static_cast<int64_t>(sonare::kMaxAudioBufferSize / 4u);
  const SonareProjectBounceOptions options = bounce_options(frames);
  REQUIRE(sonare_project_bounce_with_builtin_instruments(
              project, &options, &binding, 1, &out, &out_len) == SONARE_ERROR_INVALID_PARAMETER);
  REQUIRE(out == nullptr);
  REQUIRE(out_len == 0);
  sonare_project_destroy(project);
#endif
}
