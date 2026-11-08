/// @file note_state_decoder_test.cpp
/// @brief Tests for the joint note-state decoder on synthetic F0 tracks.
///
/// Each take is 1.6 s: 0.1 s of quiet unvoiced lead, then a voiced line at a constant
/// level. The expected spans are the reference model's output for the same inputs at a
/// 512-sample hop and 44.1 kHz; boundaries are held to within two frames of it. Paired
/// cases discriminate: the same line with and without a mid-line valley, and every case
/// again at 96 kHz. The valley detector runs on synthetic strikes and a modulated tone.

#include "editing/note_model/note_state_decoder.h"

#include <algorithm>
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
using sonare::editing::note_model::narrow_valley_frames;
using sonare::editing::note_model::NoteDecoderInput;
using sonare::editing::pitch_editor::F0Track;

constexpr int kHop = 512;
constexpr int kBaseRate = 44100;
constexpr int kHighRate = 96000;
constexpr double kTakeSec = 1.6;
constexpr double kLeadSec = 0.1;
constexpr float kLoudRms = 0.3f;
constexpr float kQuietRms = 1e-5f;
constexpr float kFmin = 65.0f;
constexpr float kFmax = 2093.0f;
constexpr float kThresholdCents = 50.0f;
constexpr float kMinNoteMs = 45.0f;
constexpr int kBoundaryToleranceFrames = 2;
constexpr double kRateBoundaryToleranceSec = 0.012;

enum class Shape { kVibrato, kStep, kSameWithValley, kSame, kGap, kGlide, kBlip };

struct Take {
  F0Track track;
  std::vector<float> rms;
  /// Times of the narrow valleys the line carries (s).
  std::vector<double> valley_sec;
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
    case Shape::kSameWithValley:
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
  if (shape == Shape::kSameWithValley) take.valley_sec.push_back(0.85);
  if (shape == Shape::kGap) take.valley_sec.push_back(0.86);
  return take;
}

std::vector<DecodedSpan> decode(const Take& take) {
  NoteDecoderInput input;
  input.track = &take.track;
  input.frame_rms = take.rms;
  const double h = frame_period(take);
  for (const double sec : take.valley_sec) {
    input.valley_frames.push_back(static_cast<int>(std::nearbyint(sec / h)));
  }
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

TEST_CASE("(c) a held pitch with a valley mid-line decodes as two notes", "[note_state_decoder]") {
  require_spans(decode(make_take(Shape::kSameWithValley, kBaseRate)), {{9, 75}, {75, 138}});
}

TEST_CASE("(d) the same held pitch with no mid-line valley decodes as one note",
          "[note_state_decoder]") {
  require_spans(decode(make_take(Shape::kSame, kBaseRate)), {{9, 138}});
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
  // The glide is left out: the reference model cuts it into four notes at 5.33 ms frames.
  for (const Shape shape : {Shape::kVibrato, Shape::kStep, Shape::kSameWithValley, Shape::kSame,
                            Shape::kGap, Shape::kBlip}) {
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
  single.valley_sec = {0.0};
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

namespace {

constexpr int kStrikeMidi = 69;
constexpr double kStrikeLeadSec = 0.2;
constexpr double kStrikeSec = 0.125;
constexpr int kStrikeCount = 4;
constexpr double kAttackSec = 0.004;
constexpr double kReleaseSec = 0.008;
constexpr int kPartials = 5;

double midi_hz(int midi) {
  return static_cast<double>(sonare::constants::kA4Hz) *
         std::pow(2.0, static_cast<double>(midi - 69) / 12.0);
}

/// Five-partial sawtooth-like tone at @p hz and time @p t (s).
double partials(double hz, double t) {
  double sum = 0.0;
  for (int k = 1; k <= kPartials; ++k) {
    sum += std::sin(sonare::constants::kTwoPiD * hz * k * t) / k;
  }
  return sum;
}

/// Linear attack and release ramps of a tone lasting @p duration (s), at @p t.
double edge(double t, double duration) {
  return std::min({1.0, t / kAttackSec, std::max(0.0, duration - t) / kReleaseSec});
}

/// Four abutting same-pitch strikes after a 0.2 s lead, then 0.2 s of silence.
std::vector<float> repeated_strikes(int sample_rate, double gain) {
  const double sr = static_cast<double>(sample_rate);
  const double tail = kStrikeLeadSec + kStrikeCount * kStrikeSec + kStrikeLeadSec;
  std::vector<float> x(static_cast<std::size_t>(std::nearbyint(tail * sr)), 0.0f);
  const double hz = midi_hz(kStrikeMidi);
  for (int a = 0; a < kStrikeCount; ++a) {
    const auto s = static_cast<std::size_t>(std::nearbyint((kStrikeLeadSec + a * kStrikeSec) * sr));
    const auto e =
        static_cast<std::size_t>(std::nearbyint((kStrikeLeadSec + (a + 1) * kStrikeSec) * sr));
    const double duration = static_cast<double>(e - s) / sr;
    for (std::size_t i = s; i < e; ++i) {
      const double t = static_cast<double>(i - s) / sr;
      x[i] = static_cast<float>(gain * edge(t, duration) * partials(hz, t));
    }
  }
  return x;
}

/// A 1.6 s tone after a 0.2 s lead whose amplitude dips by @p depth at @p am_hz.
std::vector<float> modulated_tone(int sample_rate, double am_hz, double depth) {
  const double sr = static_cast<double>(sample_rate);
  const auto s = static_cast<std::size_t>(std::nearbyint(0.2 * sr));
  const auto e = static_cast<std::size_t>(std::nearbyint(1.8 * sr));
  std::vector<float> x(static_cast<std::size_t>(std::nearbyint(2.0 * sr)), 0.0f);
  const double duration = static_cast<double>(e - s) / sr;
  const double hz = midi_hz(kStrikeMidi);
  for (std::size_t i = s; i < e; ++i) {
    const double t = static_cast<double>(i - s) / sr;
    const double amp = 1.0 - depth * 0.5 * (1.0 + std::cos(sonare::constants::kTwoPiD * am_hz * t));
    x[i] = static_cast<float>(0.18 * edge(t, duration) * amp * partials(hz, t));
  }
  return x;
}

/// A track voiced at the strike pitch on frames starting inside `[on_sec, off_sec)`.
F0Track tone_track(std::size_t samples, int sample_rate, double on_sec, double off_sec) {
  F0Track track;
  track.sample_rate = sample_rate;
  track.hop_length = kHop;
  const std::size_t n = (samples + kHop - 1) / kHop;
  for (std::size_t i = 0; i < n; ++i) {
    const double t = static_cast<double>(i) * kHop / sample_rate;
    const bool voiced = t >= on_sec && t < off_sec;
    track.f0_hz.push_back(voiced ? static_cast<float>(midi_hz(kStrikeMidi)) : 0.0f);
    track.voiced.push_back(voiced);
  }
  return track;
}

}  // namespace

TEST_CASE("narrow_valley_frames finds the boundaries of repeated strikes and not an AM trough",
          "[editing][narrow_valley]") {
  // Expected frames are the reference detector's output on the same signals.
  struct Case {
    int sample_rate;
    std::vector<int> frames;
  };
  for (const Case& c : {Case{22050, {14, 19, 25}}, Case{44100, {28, 39, 49}}}) {
    for (const double gain : {0.03, 0.18}) {
      INFO("rate " << c.sample_rate << " gain " << gain);
      const std::vector<float> x = repeated_strikes(c.sample_rate, gain);
      const F0Track track = tone_track(x.size(), c.sample_rate, 0.2, 0.7);
      const sonare::Audio audio = sonare::Audio::from_buffer(x.data(), x.size(), c.sample_rate);
      const std::vector<int> frames = narrow_valley_frames(audio, track);
      CHECK(frames == c.frames);
      REQUIRE(frames.size() == 3);
      for (int k = 0; k < 3; ++k) {
        const double boundary = (kStrikeLeadSec + (k + 1) * kStrikeSec) * c.sample_rate / kHop;
        CHECK(std::abs(frames[static_cast<std::size_t>(k)] - boundary) <= 1.0);
      }
    }

    INFO("rate " << c.sample_rate << " 8 Hz 95% AM");
    const std::vector<float> am = modulated_tone(c.sample_rate, 8.0, 0.95);
    const F0Track am_track = tone_track(am.size(), c.sample_rate, 0.2, 1.8);
    const sonare::Audio am_audio = sonare::Audio::from_buffer(am.data(), am.size(), c.sample_rate);
    CHECK(narrow_valley_frames(am_audio, am_track).empty());
  }
}
