/// @file note_state_decoder_test.cpp
/// @brief Tests for the joint note-state decoder on synthetic F0 tracks.
///
/// Each take is 1.6 s: 0.1 s of quiet unvoiced lead, then a voiced line at a constant
/// level with an onset peak where it starts. The expected spans are the reference model's
/// output for the same inputs at a 512-sample hop and 44.1 kHz; boundaries are held to
/// within two frames of it. Paired cases discriminate: the same line with and without a
/// mid-line onset, and every case again at 96 kHz.

#include "editing/note_model/note_state_decoder.h"

#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <cstdlib>
#include <utility>
#include <vector>

#include "core/audio.h"
#include "editing/pitch_editor/f0_provider.h"
#include "util/constants.h"

namespace {

using sonare::editing::note_model::decode_note_states;
using sonare::editing::note_model::DecodedSpan;
using sonare::editing::note_model::frame_rms_for_track;
using sonare::editing::note_model::NoteDecoderInput;
using sonare::editing::pitch_editor::F0Track;

constexpr int kHop = 512;
constexpr int kBaseRate = 44100;
constexpr int kHighRate = 96000;
constexpr double kTakeSec = 1.6;
constexpr double kLeadSec = 0.1;
constexpr float kLoudRms = 0.3f;
constexpr float kQuietRms = 1e-5f;
constexpr int kLeadOnsetFrame = 20;
constexpr float kFmin = 65.0f;
constexpr float kFmax = 2093.0f;
constexpr float kThresholdCents = 50.0f;
constexpr float kMinNoteMs = 100.0f;
constexpr int kBoundaryToleranceFrames = 2;
constexpr double kRateBoundaryToleranceSec = 0.012;

enum class Shape { kVibrato, kStep, kSameWithOnset, kSame, kGap, kGlide, kBlip };

struct Take {
  F0Track track;
  std::vector<float> rms;
  std::vector<int> onsets;
  bool onsets_available = true;
};

double frame_period(const Take& take) { return 1.0 / static_cast<double>(take.track.frame_rate()); }

/// Cents above A4 of the line at @p t seconds; @p voiced is cleared inside a gap.
double line_cents(Shape shape, double t, bool& voiced) {
  switch (shape) {
    case Shape::kVibrato:
      return t >= 0.6 ? 80.0 * std::sin(sonare::constants::kTwoPiD * 6.0 * (t - 0.6)) : 0.0;
    case Shape::kStep:
      return t >= 0.85 ? 200.0 : 0.0;
    case Shape::kGap:
      if (t >= 0.8 && t < 0.86) voiced = false;
      return 0.0;
    case Shape::kGlide:
      return 300.0 * (t - kLeadSec) / 1.5;
    case Shape::kBlip:
      return (t >= 0.85 && t < 0.87) ? 300.0 : 0.0;
    case Shape::kSameWithOnset:
    case Shape::kSame:
      return 0.0;
  }
  return 0.0;
}

Take make_take(Shape shape, int sample_rate) {
  Take take;
  take.track.sample_rate = sample_rate;
  take.track.hop_length = kHop;
  const double h = frame_period(take);
  const auto n = static_cast<int>(std::lround(kTakeSec / h));
  for (int i = 0; i < n; ++i) {
    const double t = static_cast<double>(i) * h;
    bool voiced = t >= kLeadSec;
    const double cents = voiced ? line_cents(shape, t, voiced) : 0.0;
    const double hz =
        static_cast<double>(sonare::constants::kA4Hz) *
        std::pow(2.0, cents / static_cast<double>(sonare::constants::kCentsPerOctave));
    take.track.f0_hz.push_back(voiced ? static_cast<float>(hz) : 0.0f);
    take.track.voiced.push_back(voiced);
    take.rms.push_back(voiced ? kLoudRms : kQuietRms);
  }
  take.onsets.push_back(kLeadOnsetFrame);
  if (shape == Shape::kSameWithOnset) take.onsets.push_back(170);
  if (shape == Shape::kGap) take.onsets.push_back(172);
  return take;
}

std::vector<DecodedSpan> decode(const Take& take) {
  NoteDecoderInput input;
  input.track = &take.track;
  input.frame_rms = take.rms;
  input.onset_frames = take.onsets;
  input.onsets_available = take.onsets_available;
  return decode_note_states(input, kFmin, kFmax, sonare::constants::kA4Hz, kThresholdCents,
                            kMinNoteMs);
}

/// Spans match @p expected in count, each boundary within the frame tolerance.
void require_spans(const std::vector<DecodedSpan>& got,
                   const std::vector<std::pair<int, int>>& expected) {
  REQUIRE(got.size() == expected.size());
  for (std::size_t i = 0; i < got.size(); ++i) {
    INFO("span " << i << ": [" << got[i].frame_start << ", " << got[i].frame_end << ")");
    CHECK(std::abs(got[i].frame_start - expected[i].first) <= kBoundaryToleranceFrames);
    CHECK(std::abs(got[i].frame_end - expected[i].second) <= kBoundaryToleranceFrames);
  }
}

}  // namespace

TEST_CASE("(a) a flat line turning into a 6 Hz 80-cent vibrato decodes as one note",
          "[note_state_decoder]") {
  require_spans(decode(make_take(Shape::kVibrato, kBaseRate)), {{9, 138}});
}

TEST_CASE("(b) a 200-cent step decodes as two notes split at the step", "[note_state_decoder]") {
  require_spans(decode(make_take(Shape::kStep, kBaseRate)), {{9, 74}, {74, 138}});
}

TEST_CASE("(c) a held pitch with an onset peak mid-line decodes as two notes",
          "[note_state_decoder]") {
  require_spans(decode(make_take(Shape::kSameWithOnset, kBaseRate)), {{9, 74}, {74, 138}});
}

TEST_CASE("(d) the same held pitch with no mid-line onset decodes as one note",
          "[note_state_decoder]") {
  require_spans(decode(make_take(Shape::kSame, kBaseRate)), {{9, 138}});

  // Onset evidence that could not be computed gives no cue even where a peak is listed.
  Take unavailable = make_take(Shape::kSameWithOnset, kBaseRate);
  unavailable.onsets_available = false;
  require_spans(decode(unavailable), {{9, 138}});
}

TEST_CASE("(e) a 60 ms unvoiced gap in a held pitch decodes as two notes", "[note_state_decoder]") {
  require_spans(decode(make_take(Shape::kGap, kBaseRate)), {{9, 69}, {75, 138}});
}

TEST_CASE("(f) a 300-cent glide over 1.5 s decodes as three notes", "[note_state_decoder]") {
  require_spans(decode(make_take(Shape::kGlide, kBaseRate)), {{9, 59}, {59, 102}, {102, 138}});
}

TEST_CASE("(g) a 20 ms 300-cent blip does not split the note", "[note_state_decoder]") {
  require_spans(decode(make_take(Shape::kBlip, kBaseRate)), {{9, 138}});
}

TEST_CASE("(h) every case decodes alike at 11.61 ms and 5.33 ms frames", "[note_state_decoder]") {
  for (const Shape shape : {Shape::kVibrato, Shape::kStep, Shape::kSameWithOnset, Shape::kSame,
                            Shape::kGap, Shape::kGlide, Shape::kBlip}) {
    const Take base = make_take(shape, kBaseRate);
    const Take high = make_take(shape, kHighRate);
    const std::vector<DecodedSpan> a = decode(base);
    const std::vector<DecodedSpan> b = decode(high);
    INFO("shape " << static_cast<int>(shape));
    REQUIRE(!a.empty());
    REQUIRE(a.size() == b.size());
    const double ha = frame_period(base);
    const double hb = frame_period(high);
    for (std::size_t i = 0; i < a.size(); ++i) {
      CHECK(std::abs(a[i].frame_start * ha - b[i].frame_start * hb) <= kRateBoundaryToleranceSec);
      CHECK(std::abs(a[i].frame_end * ha - b[i].frame_end * hb) <= kRateBoundaryToleranceSec);
    }
  }
}

TEST_CASE("(i) an all-unvoiced, a one-frame and an empty track decode to no notes",
          "[note_state_decoder]") {
  Take silent;
  silent.track.sample_rate = kBaseRate;
  silent.track.hop_length = kHop;
  silent.track.f0_hz.assign(100, 0.0f);
  silent.track.voiced.assign(100, false);
  silent.rms.assign(100, 0.0f);
  CHECK(decode(silent).empty());

  Take single;
  single.track.sample_rate = kBaseRate;
  single.track.hop_length = kHop;
  single.track.f0_hz = {sonare::constants::kA4Hz};
  single.track.voiced = {true};
  single.rms = {kLoudRms};
  single.onsets = {0};
  CHECK(decode(single).empty());

  Take empty;
  empty.track.sample_rate = kBaseRate;
  empty.track.hop_length = kHop;
  CHECK(decode(empty).empty());
}

TEST_CASE("frame_rms_for_track covers each frame's samples and cuts at the end of the audio",
          "[editing][frame_rms]") {
  constexpr int kRate = 1000;
  std::vector<float> samples(10, 0.5f);
  samples[7] = -0.5f;
  F0Track track;
  track.sample_rate = kRate;
  track.frame_rate_hz = 300.0f;  // 3.33 samples per frame: [0, 3), [3, 6), [6, 10), ...
  track.f0_hz.assign(4, 0.0f);
  track.voiced.assign(4, false);
  const sonare::Audio audio = sonare::Audio::from_buffer(samples.data(), samples.size(), kRate);
  std::vector<float> rms = frame_rms_for_track(audio, track);
  REQUIRE(rms.size() == 4);
  CHECK(rms[0] == 0.5f);
  CHECK(rms[2] == 0.5f);
  CHECK(rms[3] == 0.0f);  // past the end of the audio

  samples[4] = std::nanf("");
  const sonare::Audio with_nan = sonare::Audio::from_buffer(samples.data(), samples.size(), kRate);
  rms = frame_rms_for_track(with_nan, track);
  CHECK(rms[0] == 0.5f);
  CHECK(rms[1] == 0.0f);
}
