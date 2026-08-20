/// @file shared_strip_midi_controls_test.cpp
/// @brief A MIDI track's gain/pan reach the bounce on a shared channel strip.

#include <sonare/sonare_c.h>
#include <sonare/sonare_c_project.h>

#include <algorithm>
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <cstdint>
#include <vector>

#include "bindings/binding_project_parity_test_helpers.h"

#if defined(SONARE_WITH_MIXING)

namespace {

constexpr int kSampleRate = 48000;
constexpr int kBlockSize = 128;
constexpr int64_t kFrames = 24000;
constexpr uint32_t kDestination = 11;

struct StereoEnergy {
  double left = 0.0;
  double right = 0.0;

  double total() const noexcept { return left + right; }
};

// One MIDI track holding a sustained note, plus a second, empty track bound to
// the same scene strip. Only the second track's presence makes the strip SHARED,
// which is what diverts the first track's gain/pan away from the strip.
SonareProject* make_shared_strip_project(uint32_t* out_track) {
  SonareProject* project = nullptr;
  REQUIRE(sonare_project_create(&project) == SONARE_OK);
  REQUIRE(sonare_project_set_sample_rate(project, kSampleRate) == SONARE_OK);
  const char* scene =
      R"({"version":1,"buses":[{"id":"master","role":"master"}],"strips":[{"id":"shared"}]})";
  REQUIRE(sonare_project_set_mixer_scene_json(project, scene) == SONARE_OK);

  uint32_t track = 0;
  uint32_t clip = 0;
  REQUIRE(sonare_project_add_midi_clip(project, 0.0, 4.0, &track, &clip) == SONARE_OK);
  const SonareMidiEventPod events[] = {{0.0, 0x20903C7Fu, 0u}};
  REQUIRE(sonare_project_set_midi_events(project, clip, events, 1) == SONARE_OK);
  REQUIRE(sonare_project_set_track_midi_destination(project, track, kDestination) == SONARE_OK);
  REQUIRE(sonare_project_set_track_route(project, track, "shared", "") == SONARE_OK);

  SonareProjectTrackDesc desc{};
  desc.kind = SONARE_TRACK_MIDI;
  desc.name = "shared b";
  uint32_t neighbor = 0;
  REQUIRE(sonare_project_add_track(project, &desc, &neighbor) == SONARE_OK);
  REQUIRE(sonare_project_set_track_route(project, neighbor, "shared", "") == SONARE_OK);

  *out_track = track;
  return project;
}

StereoEnergy bounce_energy(SonareProject* project) {
  SonareProjectBounceOptions options{};
  options.total_frames = kFrames;
  options.block_size = kBlockSize;
  options.num_channels = 2;
  options.sample_rate = kSampleRate;
  SonareBuiltinInstrumentBinding binding{};
  binding.destination_id = kDestination;
  binding.config.polyphony = 8;
  binding.config.gain = 1.0f;

  float* out = nullptr;
  size_t out_len = 0;
  REQUIRE(sonare_project_bounce_with_builtin_instruments(project, &options, &binding, 1, &out,
                                                         &out_len) == SONARE_OK);
  StereoEnergy energy;
  for (size_t i = 0; i + 1 < out_len; i += 2) {
    energy.left += static_cast<double>(out[i]) * static_cast<double>(out[i]);
    energy.right += static_cast<double>(out[i + 1]) * static_cast<double>(out[i + 1]);
  }
  sonare_free_floats(out);
  return energy;
}

}  // namespace

TEST_CASE("a MIDI track's gain reaches the bounce on a shared channel strip",
          "[arrangement][project]") {
  uint32_t track = 0;
  SonareProject* project = make_shared_strip_project(&track);

  const StereoEnergy unity = bounce_energy(project);
  REQUIRE(unity.total() > 1.0e-6);

  // A -6 dB linear gain is a quarter of the energy wherever it is applied.
  REQUIRE(sonare_project_set_track_gain(project, track, 0.5f) == SONARE_OK);
  const StereoEnergy halved = bounce_energy(project);
  INFO("unity energy " << unity.total() << ", half-gain energy " << halved.total());
  REQUIRE(halved.total() / unity.total() == Catch::Approx(0.25).margin(0.01));

  sonare_project_destroy(project);
}

TEST_CASE("a MIDI track's pan reaches the bounce on a shared channel strip",
          "[arrangement][project]") {
  uint32_t track = 0;
  SonareProject* project = make_shared_strip_project(&track);

  const StereoEnergy centered = bounce_energy(project);
  REQUIRE(centered.total() > 1.0e-6);
  REQUIRE(centered.left == Catch::Approx(centered.right).epsilon(0.01));

  REQUIRE(sonare_project_set_track_pan(project, track, -1.0f) == SONARE_OK);
  const StereoEnergy panned = bounce_energy(project);
  INFO("centered L/R " << centered.left << "/" << centered.right << ", panned L/R " << panned.left
                       << "/" << panned.right);
  REQUIRE(panned.right < panned.left * 0.01);

  sonare_project_destroy(project);
}

// Pins the one configuration the header calls out as an exception: the track
// lane is fed per source track only by an instrument that preserves source-track
// identity. An opaque host callback renders one buffer per destination, which
// enters the lane keyed by the destination id, so a shared strip leaves the
// track's controls with nowhere to apply. On an exclusive strip the same
// instrument is covered, because there the controls ride the scene strip.
TEST_CASE("an opaque callback instrument keeps its strip's controls, not its track's",
          "[arrangement][project]") {
  SonareProjectBounceOptions options{};
  options.total_frames = kFrames;
  options.block_size = kBlockSize;
  options.num_channels = 2;
  options.sample_rate = kSampleRate;
  CallbackInstrumentState state;
  SonareInstrumentBinding binding{};
  binding.destination_id = kDestination;
  binding.callbacks.user_data = &state;
  binding.callbacks.prepare = &cb_prepare;
  binding.callbacks.on_event = &cb_on_event;
  binding.callbacks.render = &cb_render;

  const auto energy = [&](SonareProject* project) {
    state = CallbackInstrumentState{};
    float* out = nullptr;
    size_t out_len = 0;
    REQUIRE(sonare_project_bounce_with_instruments(project, &options, &binding, 1, &out,
                                                   &out_len) == SONARE_OK);
    double sum = 0.0;
    for (size_t i = 0; i < out_len; ++i)
      sum += static_cast<double>(out[i]) * static_cast<double>(out[i]);
    sonare_free_floats(out);
    return sum;
  };
  const auto gain_energy_ratio = [&](SonareProject* project, uint32_t track) {
    const double unity = energy(project);
    REQUIRE(unity > 1.0e-6);
    REQUIRE(sonare_project_set_track_gain(project, track, 0.5f) == SONARE_OK);
    return energy(project) / unity;
  };

  SonareProject* exclusive = nullptr;
  REQUIRE(sonare_project_create(&exclusive) == SONARE_OK);
  REQUIRE(sonare_project_set_sample_rate(exclusive, kSampleRate) == SONARE_OK);
  const char* scene =
      R"({"version":1,"buses":[{"id":"master","role":"master"}],"strips":[{"id":"solo"}]})";
  REQUIRE(sonare_project_set_mixer_scene_json(exclusive, scene) == SONARE_OK);
  uint32_t exclusive_track = 0;
  uint32_t exclusive_clip = 0;
  REQUIRE(sonare_project_add_midi_clip(exclusive, 0.0, 4.0, &exclusive_track, &exclusive_clip) ==
          SONARE_OK);
  const SonareMidiEventPod events[] = {{0.0, 0x20903C7Fu, 0u}};
  REQUIRE(sonare_project_set_midi_events(exclusive, exclusive_clip, events, 1) == SONARE_OK);
  REQUIRE(sonare_project_set_track_midi_destination(exclusive, exclusive_track, kDestination) ==
          SONARE_OK);
  REQUIRE(sonare_project_set_track_route(exclusive, exclusive_track, "solo", "") == SONARE_OK);
  REQUIRE(gain_energy_ratio(exclusive, exclusive_track) == Catch::Approx(0.25).margin(0.01));
  sonare_project_destroy(exclusive);

  uint32_t shared_track = 0;
  SonareProject* shared = make_shared_strip_project(&shared_track);
  REQUIRE(gain_energy_ratio(shared, shared_track) == Catch::Approx(1.0).margin(0.01));
  sonare_project_destroy(shared);
}

#endif  // SONARE_WITH_MIXING
