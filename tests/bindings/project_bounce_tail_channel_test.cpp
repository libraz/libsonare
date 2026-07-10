/// @file project_bounce_tail_channel_test.cpp
/// @brief Regression tests for two project-bounce behaviors: channel-strip
/// bounce must keep real stem tails when an explicit total_frames extends past
/// the arrangement, and project bounce must reject channel counts beyond
/// mono/stereo (which engine-bounce rejects) instead of writing silent planes.

#include <sonare/sonare_c_engine.h>

#include <algorithm>
#include <cmath>

#include "bindings/binding_project_parity_test_helpers.h"

namespace {

// A callback instrument that emits a constant DC level on every render() block,
// independent of note state, and declares no release tail (tail_samples == 0).
// This is the minimal shape that reproduces the stem-tail truncation: the
// arrangement's musical end (and the zero declared tail) sits well before the
// caller's explicit total_frames, yet the instrument keeps producing audio, so
// the bounce must feed those stem frames through the mixer instead of draining
// them to silence.
void dc_render(void* /*user*/, float* const* channels, int num_channels, int num_frames) {
  for (int ch = 0; ch < num_channels; ++ch) {
    for (int i = 0; i < num_frames; ++i) channels[ch][i] += 0.5f;
  }
}

// Peak absolute amplitude over [start, len) of an interleaved buffer.
float region_peak(const float* data, size_t len, size_t start) {
  float peak = 0.0f;
  for (size_t i = std::min(start, len); i < len; ++i) peak = std::max(peak, std::abs(data[i]));
  return peak;
}

}  // namespace

TEST_CASE("channel-strip bounce keeps a stem tail past the arrangement for explicit total_frames",
          "[project]") {
  SonareProject* project = nullptr;
  REQUIRE(sonare_project_create(&project) == SONARE_OK);
  REQUIRE(sonare_project_set_sample_rate(project, 48000.0) == SONARE_OK);

  // One strip routed straight to the master (transparent: no inserts, unity
  // fader), so a drained (zero-input) tail region collapses to exact silence and
  // a fed region carries the instrument's DC.
  const char* scene_json =
      "{\"version\":1,\"strips\":[{\"id\":\"s0\"}],"
      "\"buses\":[{\"id\":\"master\",\"role\":\"master\"}],"
      "\"connections\":[{\"source\":\"s0\",\"destination\":\"master\"}]}";
  REQUIRE(sonare_project_set_mixer_scene_json(project, scene_json) == SONARE_OK);

  // A MIDI track whose clip ends at 1 quarter (~24000 frames at 120 BPM / 48 kHz)
  // routed to destination 0 and bound to strip s0. The clip length fixes the
  // arrangement's musical end; the DC instrument at destination 0 keeps sounding
  // long after it.
  uint32_t track = 0;
  uint32_t clip = 0;
  REQUIRE(sonare_project_add_midi_clip(project, 0.0, 1.0, &track, &clip) == SONARE_OK);
  SonareMidiEventPod events[2];
  events[0].ppq = 0.0;
  events[0].data0 = 0x20903C40u;  // note-on, note 60, vel 64
  events[0].data1 = 0u;
  events[1].ppq = 0.5;
  events[1].data0 = 0x20803C00u;  // note-off, note 60
  events[1].data1 = 0u;
  REQUIRE(sonare_project_set_midi_events(project, clip, events, 2) == SONARE_OK);
  REQUIRE(sonare_project_set_track_midi_destination(project, track, 0) == SONARE_OK);
  REQUIRE(sonare_project_set_track_route(project, track, "s0", nullptr) == SONARE_OK);

  // The arrangement ends near frame 24000; request a window well past it.
  constexpr int64_t kArrangementFrames = 24000;
  constexpr int64_t kTotalFrames = 36000;
  SonareProjectBounceOptions options{};
  options.total_frames = kTotalFrames;
  options.block_size = 128;
  options.num_channels = 2;
  options.sample_rate = 48000;

  SonareInstrumentBinding binding{};
  binding.destination_id = 0;
  binding.callbacks.render = &dc_render;
  binding.callbacks.tail_samples = 0;

  float* out = nullptr;
  size_t out_len = 0;
  REQUIRE(sonare_project_bounce_with_instruments(project, &options, &binding, 1, &out, &out_len) ==
          SONARE_OK);
  REQUIRE(out != nullptr);
  REQUIRE(out_len == static_cast<size_t>(kTotalFrames) * 2u);

  // Sanity: the arranged portion carries audio.
  REQUIRE(region_peak(out, out_len, 0) > 0.1f);

  // The decisive assertion: a region strictly past the arrangement's musical end
  // (well beyond frame 24000, up to the requested 36000) must still carry the
  // instrument's ongoing stem. The defect pinned the mixer input span to the
  // arrangement end + declared tail (0), draining this region to silence.
  const size_t tail_start = static_cast<size_t>(kArrangementFrames + 6000) * 2u;
  REQUIRE(region_peak(out, out_len, tail_start) > 0.1f);

  sonare_free_floats(out);
  sonare_project_destroy(project);
}

TEST_CASE("project bounce rejects channel counts beyond mono/stereo like engine-bounce",
          "[project]") {
  SonareProject* project = nullptr;
  REQUIRE(sonare_project_create(&project) == SONARE_OK);
  REQUIRE(sonare_project_set_sample_rate(project, 48000.0) == SONARE_OK);

  // A MIDI clip routed to an unbound destination: the plain (no-instrument)
  // bounce renders it to silence, which is enough to exercise the stereo success
  // path. The wide-count rejection short-circuits at the channel-count guard
  // before any rendering.
  uint32_t track = 0;
  uint32_t clip = 0;
  REQUIRE(sonare_project_add_midi_clip(project, 0.0, 1.0, &track, &clip) == SONARE_OK);
  REQUIRE(sonare_project_set_track_midi_destination(project, track, 5) == SONARE_OK);

  SonareProjectBounceOptions options{};
  options.total_frames = 128;
  options.block_size = 128;
  options.num_channels = 6;  // 5.1 has a layout for engine-bounce, but the project
                             // bounce sums to a stereo master and only writes
                             // mono/stereo, so it must reject anything past 2.
  options.sample_rate = 48000;

  float* out = reinterpret_cast<float*>(static_cast<uintptr_t>(0x1));
  size_t out_len = 123;
  REQUIRE(sonare_project_bounce(project, &options, &out, &out_len) ==
          SONARE_ERROR_INVALID_PARAMETER);
  REQUIRE(out == nullptr);
  REQUIRE(out_len == 0);

  // A non-layout width (3) is rejected too.
  options.num_channels = 3;
  out = reinterpret_cast<float*>(static_cast<uintptr_t>(0x1));
  out_len = 123;
  REQUIRE(sonare_project_bounce(project, &options, &out, &out_len) ==
          SONARE_ERROR_INVALID_PARAMETER);
  REQUIRE(out == nullptr);
  REQUIRE(out_len == 0);

  // Stereo still bounces.
  options.num_channels = 2;
  REQUIRE(sonare_project_bounce(project, &options, &out, &out_len) == SONARE_OK);
  REQUIRE(out != nullptr);
  REQUIRE(out_len == 256u);
  sonare_free_floats(out);

  // This mirrors engine-bounce's own layout guard (sonare_c_engine_render.cpp:
  // layout_from_channel_count rejects any count that is not a supported layout),
  // so the project surface now behaves like engine-bounce instead of emitting
  // silent planes for a count it cannot actually write.
  sonare_project_destroy(project);
}
