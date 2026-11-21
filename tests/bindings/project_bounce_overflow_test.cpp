/// @file project_bounce_overflow_test.cpp
/// @brief Regression tests for checked project-bounce frame arithmetic.

#include <sonare/sonare_c_mixing.h>
#include <sonare/sonare_c_project.h>

#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <limits>

#include "core/audio.h"

namespace {

// Bounces with one kind of hosted-instrument list set on a copy of the options.
SonareError bounce_with_instruments(SonareProject* project,
                                    const SonareProjectBounceOptions* options,
                                    const SonareInstrumentBinding* bindings, size_t count,
                                    float** out_interleaved, size_t* out_len,
                                    int* out_num_channels) {
  SonareProjectBounceOptions with_instruments;
  sonare_project_bounce_options_init(&with_instruments);
  if (options != nullptr) with_instruments = *options;
  with_instruments.instruments = bindings;
  with_instruments.instrument_count = count;
  return sonare_project_bounce(project, &with_instruments, out_interleaved, out_len,
                               out_num_channels);
}

SonareError bounce_with_builtin_instruments(SonareProject* project,
                                            const SonareProjectBounceOptions* options,
                                            const SonareBuiltinInstrumentBinding* bindings,
                                            size_t count, float** out_interleaved, size_t* out_len,
                                            int* out_num_channels) {
  SonareProjectBounceOptions with_instruments;
  sonare_project_bounce_options_init(&with_instruments);
  if (options != nullptr) with_instruments = *options;
  with_instruments.builtin_instruments = bindings;
  with_instruments.builtin_instrument_count = count;
  return sonare_project_bounce(project, &with_instruments, out_interleaved, out_len,
                               out_num_channels);
}

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
      sonare_project_set_sample_rate(project, 48000) != SONARE_OK ||
      sonare_project_add_midi_clip(project, start_ppq, 1.0, out_track, out_clip) != SONARE_OK ||
      sonare_project_set_track_midi_destination(project, *out_track, destination_id) != SONARE_OK) {
    sonare_project_destroy(project);
    return nullptr;
  }
  return project;
}

SonareProjectBounceOptions bounce_options(int64_t total_frames) {
  SonareProjectBounceOptions options;
  sonare_project_bounce_options_init(&options);
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
  const SonareProjectBounceOptions options =
      bounce_options(SONARE_PROJECT_BOUNCE_LENGTH_FROM_ARRANGEMENT);
  int bounce_channels1 = 0;
  REQUIRE(bounce_with_instruments(project, &options, &binding, 1, &out, &out_len,
                                  &bounce_channels1) == SONARE_ERROR_INVALID_PARAMETER);
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
  const SonareProjectBounceOptions options =
      bounce_options(SONARE_PROJECT_BOUNCE_LENGTH_FROM_ARRANGEMENT);
  int bounce_channels2 = 0;
  REQUIRE(bounce_with_instruments(project, &options, &binding, 1, &out, &out_len,
                                  &bounce_channels2) == SONARE_ERROR_INVALID_STATE);
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
  int bounce_channels3 = 0;
  REQUIRE(bounce_with_instruments(project, &options, &binding, 1, &out, &out_len,
                                  &bounce_channels3) == SONARE_ERROR_INVALID_PARAMETER);
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
  int bounce_channels4 = 0;
  REQUIRE(bounce_with_instruments(project, &options, &binding, 1, &out, &out_len,
                                  &bounce_channels4) == SONARE_ERROR_INVALID_PARAMETER);
  REQUIRE(out == nullptr);
  REQUIRE(out_len == 0);
  REQUIRE(state.render_calls == 0);
  sonare_project_destroy(project);
}

TEST_CASE("project bounce derives the length from the named constant and refuses other lengths",
          "[project][overflow]") {
  SonareProject* project = nullptr;
  REQUIRE(sonare_project_create(&project) == SONARE_OK);
  for (const int64_t refused : {int64_t{0}, int64_t{-2}, std::numeric_limits<int64_t>::min()}) {
    const SonareProjectBounceOptions bad = bounce_options(refused);
    float* bad_out = nullptr;
    size_t bad_len = 0;
    int bad_channels = 0;
    REQUIRE(sonare_project_bounce(project, &bad, &bad_out, &bad_len, &bad_channels) ==
            SONARE_ERROR_INVALID_PARAMETER);
    CHECK(bad_out == nullptr);
  }

  float* out = nullptr;
  size_t out_len = 0;
  const SonareProjectBounceOptions options =
      bounce_options(SONARE_PROJECT_BOUNCE_LENGTH_FROM_ARRANGEMENT);
  int bounce_channels5 = 0;
  REQUIRE(sonare_project_bounce(project, &options, &out, &out_len, &bounce_channels5) == SONARE_OK);
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
  sonare_builtin_synth_config_init(&binding.config);
  binding.destination_id = 7;
  out = nullptr;
  out_len = 0;
  int bounce_channels6 = 0;
  REQUIRE(bounce_with_builtin_instruments(project, &options, &binding, 1, &out, &out_len,
                                          &bounce_channels6) == SONARE_OK);
  REQUIRE(out != nullptr);
  REQUIRE(out_len > 0);
  sonare_free_floats(out);
  sonare_project_destroy(project);
}

TEST_CASE("project bounce rejects oversized channel-strip stem shapes", "[project][overflow]") {
#if !defined(SONARE_WITH_MIXING)
  SKIP("channel-strip bounce requires the mixing build");
#else
  SonareProject* project = nullptr;
  REQUIRE(sonare_project_create(&project) == SONARE_OK);
  REQUIRE(sonare_project_set_sample_rate(project, 48000) == SONARE_OK);
  const char* scene =
      R"({"version":2,"buses":[{"id":"master","role":"master"}],"strips":[{"id":"a"},{"id":"b"},{"id":"c"}]})";
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
  sonare_builtin_synth_config_init(&binding.config);
  binding.destination_id = kDestination;
  float* out = reinterpret_cast<float*>(static_cast<uintptr_t>(0x1));
  size_t out_len = 1;
  const int64_t frames = static_cast<int64_t>(sonare::kMaxAudioBufferSize / 2u);
  const SonareProjectBounceOptions options = bounce_options(frames);
  int bounce_channels7 = 0;
  REQUIRE(bounce_with_builtin_instruments(project, &options, &binding, 1, &out, &out_len,
                                          &bounce_channels7) == SONARE_ERROR_INVALID_PARAMETER);
  REQUIRE(out == nullptr);
  REQUIRE(out_len == 0);
  sonare_project_destroy(project);
#endif
}

TEST_CASE("project bounce rejects oversized MIDI source-stem shapes", "[project][overflow]") {
#if !defined(SONARE_WITH_MIXING)
  SKIP("source-aware channel-strip bounce requires the mixing build");
#else
  SonareProject* project = nullptr;
  REQUIRE(sonare_project_create(&project) == SONARE_OK);
  REQUIRE(sonare_project_set_sample_rate(project, 48000) == SONARE_OK);
  const char* scene =
      R"({"version":2,"buses":[{"id":"master","role":"master"}],"strips":[{"id":"shared"}]})";
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
  sonare_builtin_synth_config_init(&binding.config);
  binding.destination_id = kDestination;
  float* out = reinterpret_cast<float*>(static_cast<uintptr_t>(0x1));
  size_t out_len = 1;
  const int64_t frames = static_cast<int64_t>(sonare::kMaxAudioBufferSize / 4u);
  const SonareProjectBounceOptions options = bounce_options(frames);
  int bounce_channels8 = 0;
  REQUIRE(bounce_with_builtin_instruments(project, &options, &binding, 1, &out, &out_len,
                                          &bounce_channels8) == SONARE_ERROR_INVALID_PARAMETER);
  REQUIRE(out == nullptr);
  REQUIRE(out_len == 0);
  sonare_project_destroy(project);
#endif
}
