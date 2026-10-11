/// @file gs_tail_bounce_test.cpp
/// @brief The auto-length project bounce against the instrument tail: a GM
///        project that sends no GS envelope-time edit ends at the unscaled
///        tail of the program it plays, one that does gets the scaled release it asked for, and an
///        instrument reporting an unbounded tail makes the caller name a length.

#include <sonare/sonare_c.h>
#include <sonare/sonare_c_project.h>

#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <iterator>
#include <limits>
#include <vector>

#include "midi/synth/gm_fallback_map.h"
#include "midi/synth/gs_layer.h"
#include "midi/synth/native_synth.h"
#include "midi/synth/piano_resonance.h"

#if defined(SONARE_WITH_ARRANGEMENT)

namespace {

using sonare::midi::synth::EnvelopeTimeScales;
using sonare::midi::synth::gm_fallback_max_tail_samples;
using sonare::midi::synth::gm_fallback_patch;
using sonare::midi::synth::gs_time_scale;
using sonare::midi::synth::kGsPartOffsetMax;
using sonare::midi::synth::kPianoBodyRingS;
using sonare::midi::synth::native_patch_tail_samples;

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

/// The GM piano's voice tail at @p scales plus the body ring the bus adds after it.
int64_t piano_tail_samples(const EnvelopeTimeScales& scales) {
  const int64_t voice = native_patch_tail_samples(gm_fallback_patch(0, 0), kRate, scales, nullptr);
  return voice + static_cast<int64_t>(std::ceil(kRate * kPianoBodyRingS));
}

}  // namespace

TEST_CASE("a GM bounce with no GS envelope-time edit ends at the unscaled tail",
          "[project][midi][gs-tail]") {
  // The clip plays program 0 alone, so its tail is the piano's, not every program's.
  const int64_t frames = bounce_gm_frames(false);
  CHECK(frames >= kArrangementFrames + piano_tail_samples({}));
  CHECK(frames < kArrangementFrames + gm_fallback_max_tail_samples(kRate, 1.0f, 1.0f, 1.0f));
}

TEST_CASE("a GM bounce whose clip edits the GS release covers the scaled release",
          "[project][midi][gs-tail]") {
  const float max_scale = gs_time_scale(kGsPartOffsetMax);
  const int64_t plain = bounce_gm_frames(false);
  const int64_t edited = bounce_gm_frames(true);
  CHECK(edited >= kArrangementFrames + piano_tail_samples({1.0f, 1.0f, max_scale}));
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

namespace {

/// Mean square of the left channel over [@p t0, @p t1) seconds, in dB.
double left_rms_db(const std::vector<float>& interleaved, double t0, double t1) {
  const size_t i0 = static_cast<size_t>(t0 * kRate);
  const size_t i1 = static_cast<size_t>(t1 * kRate);
  REQUIRE(interleaved.size() >= 2 * i1);
  double sum = 0.0;
  for (size_t i = i0; i < i1; ++i) {
    sum += static_cast<double>(interleaved[2 * i]) * interleaved[2 * i];
  }
  return 10.0 * std::log10(sum / static_cast<double>(i1 - i0) + 1.0e-20);
}

/// Middle C struck 0.1-0.4 s; with @p pedal_cc given, that pedal goes down at 0.3 s, while the key
/// is held (so a sostenuto captures it), and up at 2.5 s. Bounced for 4 s through the looped
/// fixture SoundFont or the GM synth bank.
std::vector<float> pedal_bounce(int pedal_cc, bool sf2) {
  // 120 BPM: one quarter is 0.5 s.
  std::vector<SonareMidiEventPod> events;
  const uint32_t cc = 0x20B00000u | (static_cast<uint32_t>(pedal_cc) << 8);
  events.push_back(pod(0.2, 0x20903C64u));
  if (pedal_cc >= 0) events.push_back(pod(0.6, cc | 0x7Fu));
  events.push_back(pod(0.8, 0x20803C00u));
  if (pedal_cc >= 0) events.push_back(pod(5.0, cc));
  SonareProject* project = nullptr;
  REQUIRE(sonare_project_create(&project) == SONARE_OK);
  REQUIRE(sonare_project_set_sample_rate(project, kRate) == SONARE_OK);
  if (sf2) {
    std::ifstream file("tests/fixtures/sf2/minimal_gs.sf2", std::ios::binary);
    const std::vector<uint8_t> bytes{std::istreambuf_iterator<char>(file),
                                     std::istreambuf_iterator<char>()};
    REQUIRE(!bytes.empty());
    REQUIRE(sonare_project_load_soundfont(project, bytes.data(), bytes.size()) == SONARE_OK);
  }
  uint32_t track = 0;
  uint32_t clip = 0;
  REQUIRE(sonare_project_add_midi_clip(project, 0.0, 8.0, &track, &clip) == SONARE_OK);
  REQUIRE(sonare_project_set_midi_events(project, clip, events.data(), events.size()) == SONARE_OK);
  SonareProjectBounceOptions options = auto_length_options();
  options.total_frames = static_cast<int64_t>(4.0 * kRate);
  float* out = nullptr;
  size_t out_len = 0;
  REQUIRE((sf2 ? bounce_sf2 : bounce_native)(project, &options, &out, &out_len) == SONARE_OK);
  std::vector<float> rendered(out, out + out_len);
  sonare_free_floats(out);
  sonare_project_destroy(project);
  return rendered;
}

}  // namespace

TEST_CASE("a held pedal sustains a released note through every bounce kind",
          "[project][midi][gs-tail][sf2]") {
  for (const bool sf2 : {true, false}) {
    INFO((sf2 ? "Sf2Player" : "NativeSynth"));
    const std::vector<float> dry = pedal_bounce(-1, sf2);
    const std::vector<float> sustain = pedal_bounce(64, sf2);
    const std::vector<float> sostenuto = pedal_bounce(66, sf2);
    // The key has been up for 0.6 s: unpedalled, the note has died away.
    const double dry_db = left_rms_db(dry, 1.0, 1.3);
    CHECK(left_rms_db(sustain, 1.0, 1.3) > dry_db + 12.0);
    CHECK(left_rms_db(sostenuto, 1.0, 1.3) > dry_db + 12.0);
  }
}

#endif  // defined(SONARE_WITH_ARRANGEMENT)
