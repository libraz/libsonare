/// @file gs_tail_bounce_test.cpp
/// @brief The auto-length project bounce against the instrument tail: a GM
///        project that sends no GS envelope-time edit ends at the unscaled
///        bound, one that does gets the scaled release it asked for, and an
///        instrument reporting an unbounded tail makes the caller name a length.

#include <sonare/sonare_c.h>
#include <sonare/sonare_c_project.h>

#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <limits>
#include <vector>

#include "midi/synth/gm_fallback_map.h"
#include "midi/synth/gs_layer.h"

#if defined(SONARE_WITH_ARRANGEMENT)

namespace {

using sonare::midi::synth::gm_fallback_max_tail_samples;
using sonare::midi::synth::gs_time_scale;
using sonare::midi::synth::kGsPartOffsetMax;

constexpr double kRate = 48000.0;
/// One quarter at the default 120 BPM.
constexpr int64_t kArrangementFrames = 24000;

/// A one-quarter MIDI clip on destination 0 holding @p events.
SonareProject* make_project(const std::vector<SonareMidiEventPod>& events) {
  SonareProject* project = nullptr;
  REQUIRE(sonare_project_create(&project) == SONARE_OK);
  REQUIRE(sonare_project_set_sample_rate(project, kRate) == SONARE_OK);
  uint32_t track = 0;
  uint32_t clip = 0;
  REQUIRE(sonare_project_add_midi_clip(project, 0.0, 1.0, &track, &clip) == SONARE_OK);
  REQUIRE(sonare_project_set_midi_events(project, clip, events.data(), events.size()) == SONARE_OK);
  REQUIRE(sonare_project_set_track_midi_destination(project, track, 0) == SONARE_OK);
  return project;
}

SonareMidiEventPod pod(double ppq, uint32_t word) {
  SonareMidiEventPod e{};
  e.ppq = ppq;
  e.data0 = word;
  e.data1 = 0u;
  return e;
}

std::vector<SonareMidiEventPod> note_events(bool release_edit) {
  std::vector<SonareMidiEventPod> events;
  if (release_edit) {
    // NRPN 01 66 = 7F: EG release offset +63 on channel 1.
    events.push_back(pod(0.0, 0x20B06301u));
    events.push_back(pod(0.0, 0x20B06266u));
    events.push_back(pod(0.0, 0x20B0067Fu));
  }
  events.push_back(pod(0.0, 0x20903C64u));
  events.push_back(pod(0.5, 0x20803C00u));
  return events;
}

SonareProjectBounceOptions auto_length_options() {
  SonareProjectBounceOptions options{};
  options.total_frames = 0;
  options.block_size = 128;
  options.num_channels = 2;
  options.sample_rate = static_cast<int>(kRate);
  return options;
}

int64_t bounce_gm_frames(bool release_edit) {
  SonareProject* project = make_project(note_events(release_edit));
  SonareSynthInstrumentBinding binding{};
  binding.destination_id = 0;
  REQUIRE(sonare_synth_preset_patch("sine", &binding.patch) == SONARE_OK);
  binding.use_gm_programs = 1;
  const SonareProjectBounceOptions options = auto_length_options();
  float* out = nullptr;
  size_t out_len = 0;
  REQUIRE(sonare_project_bounce_with_synth_instruments(project, &options, &binding, 1, &out,
                                                       &out_len) == SONARE_OK);
  sonare_free_floats(out);
  sonare_project_destroy(project);
  return static_cast<int64_t>(out_len / 2);
}

/// A note at frame 0, plus @p trailing events after it has ended.
std::vector<SonareMidiEventPod> note_then(const std::vector<SonareMidiEventPod>& trailing) {
  std::vector<SonareMidiEventPod> events{pod(0.0, 0x20903C64u), pod(0.5, 0x20803C00u)};
  events.insert(events.end(), trailing.begin(), trailing.end());
  return events;
}

/// The first 9600 frames of an auto-length bounce, interleaved.
template <typename Bounce>
std::vector<float> opening(const std::vector<SonareMidiEventPod>& events, Bounce bounce) {
  SonareProject* project = make_project(events);
  const SonareProjectBounceOptions options = auto_length_options();
  float* out = nullptr;
  size_t out_len = 0;
  REQUIRE(bounce(project, &options, &out, &out_len) == SONARE_OK);
  REQUIRE(out_len >= 9600u * 2u);
  std::vector<float> head(out, out + 9600 * 2);
  sonare_free_floats(out);
  sonare_project_destroy(project);
  return head;
}

SonareError bounce_native(SonareProject* project, const SonareProjectBounceOptions* options,
                          float** out, size_t* out_len) {
  SonareSynthInstrumentBinding binding{};
  REQUIRE(sonare_synth_preset_patch("sine", &binding.patch) == SONARE_OK);
  binding.use_gm_programs = 1;
  return sonare_project_bounce_with_synth_instruments(project, options, &binding, 1, out, out_len);
}

SonareError bounce_sf2(SonareProject* project, const SonareProjectBounceOptions* options,
                       float** out, size_t* out_len) {
  SonareSf2InstrumentBinding binding{};
  return sonare_project_bounce_with_sf2_instruments(project, options, &binding, 1, out, out_len);
}

void silent_render(void*, float* const*, int, int) {}

}  // namespace

TEST_CASE("a GM bounce with no GS envelope-time edit ends at the unscaled tail",
          "[project][midi][gs-tail]") {
  const float max_scale = gs_time_scale(kGsPartOffsetMax);
  const int64_t frames = bounce_gm_frames(false);
  CHECK(frames >= kArrangementFrames + gm_fallback_max_tail_samples(kRate, 1.0f, 1.0f, 1.0f));
  CHECK(frames <
        kArrangementFrames + gm_fallback_max_tail_samples(kRate, max_scale, max_scale, max_scale));
}

TEST_CASE("a GM bounce whose clip edits the GS release covers the scaled release",
          "[project][midi][gs-tail]") {
  const float max_scale = gs_time_scale(kGsPartOffsetMax);
  const int64_t plain = bounce_gm_frames(false);
  const int64_t edited = bounce_gm_frames(true);
  CHECK(edited >= kArrangementFrames + gm_fallback_max_tail_samples(kRate, 1.0f, 1.0f, max_scale));
  CHECK(edited > plain);
}

TEST_CASE("an unbounded instrument tail makes the auto-length bounce refuse",
          "[project][midi][gs-tail]") {
  SonareProject* project = make_project(note_events(false));
  SonareInstrumentBinding binding{};
  binding.destination_id = 0;
  binding.callbacks.render = silent_render;
  binding.callbacks.tail_samples = std::numeric_limits<int>::max();
  SonareProjectBounceOptions options = auto_length_options();
  float* out = nullptr;
  size_t out_len = 0;
  CHECK(sonare_project_bounce_with_instruments(project, &options, &binding, 1, &out, &out_len) ==
        SONARE_ERROR_INVALID_PARAMETER);
  CHECK(out == nullptr);

  // An explicit length is the caller's answer to the unbounded tail.
  options.total_frames = kArrangementFrames;
  REQUIRE(sonare_project_bounce_with_instruments(project, &options, &binding, 1, &out, &out_len) ==
          SONARE_OK);
  CHECK(out_len == static_cast<size_t>(kArrangementFrames) * 2);
  sonare_free_floats(out);
  sonare_project_destroy(project);
}

TEST_CASE("events after a note do not reach the bounce's opening through the tail probe",
          "[project][midi][gs-tail]") {
  const std::vector<std::vector<SonareMidiEventPod>> trailing = {
      {pod(0.9, 0x20B00700u)},                                                 // CC7 = 0
      {pod(0.9, 0x20C02800u)},                                                 // program 40
      {pod(0.8, 0x20B06301u), pod(0.85, 0x20B06220u), pod(0.9, 0x20B00610u)},  // NRPN 01 20
  };
  for (const auto& bounce : {&bounce_native, &bounce_sf2}) {
    const std::vector<float> plain = opening(note_then({}), bounce);
    // The comparison can see the event: CC7 = 0 inside the opening silences it.
    REQUIRE(opening(note_then({pod(0.1, 0x20B00700u)}), bounce) != plain);
    for (size_t i = 0; i < trailing.size(); ++i) {
      INFO("trailing case " << i << (bounce == &bounce_native ? " (NativeSynth)" : " (Sf2Player)"));
      CHECK(opening(note_then(trailing[i]), bounce) == plain);
    }
  }
}

#endif  // defined(SONARE_WITH_ARRANGEMENT)
