#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <cmath>
#include <limits>
#include <vector>

#include "core/audio.h"
#include "editing/pitch_editor/pitch_corrector.h"
#include "util/constants.h"
#include "util/exception.h"

using namespace sonare::editing::pitch_editor;

namespace {

std::vector<float> sine(float frequency_hz, int sample_rate, int samples) {
  std::vector<float> output(static_cast<size_t>(samples), 0.0f);
  for (int i = 0; i < samples; ++i) {
    output[static_cast<size_t>(i)] =
        0.5f * static_cast<float>(std::sin(sonare::constants::kTwoPiD * frequency_hz * i /
                                           static_cast<double>(sample_rate)));
  }
  return output;
}

F0Track track_with_voiced_run(float frequency_hz, int sample_rate, float frame_rate_hz, int frames,
                              int first_voiced, int last_voiced) {
  F0Track track;
  track.sample_rate = sample_rate;
  track.hop_length = 256;
  track.frame_rate_hz = frame_rate_hz;
  track.f0_hz.assign(static_cast<size_t>(frames), 0.0f);
  track.voiced.assign(static_cast<size_t>(frames), false);
  track.voiced_prob.assign(static_cast<size_t>(frames), 0.0f);
  for (int frame = first_voiced; frame < last_voiced; ++frame) {
    track.f0_hz[static_cast<size_t>(frame)] = frequency_hz;
    track.voiced[static_cast<size_t>(frame)] = true;
    track.voiced_prob[static_cast<size_t>(frame)] = 1.0f;
  }
  return track;
}

float positive_zero_crossing_hz(const sonare::Audio& audio, int begin, int end) {
  const int first = std::max(1, begin + 1);
  const int last = std::min(end, static_cast<int>(audio.size()));
  if (last <= first) return 0.0f;

  double first_crossing = 0.0;
  double last_crossing = 0.0;
  int crossings = 0;
  for (int i = first; i < last; ++i) {
    const float previous = audio[static_cast<size_t>(i - 1)];
    const float current = audio[static_cast<size_t>(i)];
    if (!(previous <= 0.0f && current > 0.0f)) continue;
    const double denominator = static_cast<double>(current) - previous;
    const double fraction = denominator > 0.0 ? -static_cast<double>(previous) / denominator : 0.0;
    const double crossing = static_cast<double>(i - 1) + fraction;
    if (crossings == 0) first_crossing = crossing;
    last_crossing = crossing;
    ++crossings;
  }
  if (crossings < 2 || last_crossing <= first_crossing) return 0.0f;
  return static_cast<float>(audio.sample_rate() * static_cast<double>(crossings - 1) /
                            (last_crossing - first_crossing));
}

double maximum_difference(const sonare::Audio& lhs, const sonare::Audio& rhs, int begin, int end) {
  const int first = std::max(0, begin);
  const int last = std::min({end, static_cast<int>(lhs.size()), static_cast<int>(rhs.size())});
  double difference = 0.0;
  for (int i = first; i < last; ++i) {
    difference = std::max(difference, std::abs(static_cast<double>(lhs[static_cast<size_t>(i)]) -
                                               static_cast<double>(rhs[static_cast<size_t>(i)])));
  }
  return difference;
}

template <typename Operation>
void require_invalid_parameter(Operation&& operation) {
  bool threw = false;
  try {
    operation();
  } catch (const sonare::SonareException& error) {
    threw = true;
    CHECK(error.code() == sonare::ErrorCode::InvalidParameter);
  }
  CHECK(threw);
}

}  // namespace

TEST_CASE("PitchCorrector uses an absolute frame origin for voiced runs",
          "[pitch_editor][vocal_origin]") {
  constexpr int sample_rate = 16000;
  constexpr int n_samples = 32000;
  constexpr float input_f0 = 220.0f;
  constexpr float frame_rate_hz = 61.25f;  // 261.224... samples per frame.
  constexpr double frame_origin_sample = 733.75;
  constexpr int frames = 150;
  constexpr int first_voiced = 8;
  constexpr int last_voiced = 120;

  const sonare::Audio audio =
      sonare::Audio::from_vector(sine(input_f0, sample_rate, n_samples), sample_rate);
  const F0Track track = track_with_voiced_run(input_f0, sample_rate, frame_rate_hz, frames,
                                              first_voiced, last_voiced);
  const std::vector<float> deltas(static_cast<size_t>(frames), 6.0f);

  const PitchCorrector corrector;
  const sonare::Audio origin_aware =
      corrector.resynthesize(audio, track, deltas, frame_origin_sample);
  const sonare::Audio origin_zero = corrector.resynthesize(audio, track, deltas, 0.0);

  // Before the origin is an exact dry pass; the zero-origin control shows the origin is honoured.
  constexpr int dry_begin = 2200;
  constexpr int dry_end = 2500;
  for (int i = dry_begin; i < dry_end; ++i) {
    CHECK(origin_aware[static_cast<size_t>(i)] == audio[static_cast<size_t>(i)]);
  }
  CHECK(maximum_difference(origin_zero, audio, dry_begin, dry_end) > 1.0e-3);

  // Measured from the output alone: the active run starts at the origin, not at sample zero.
  const float expected_f0 =
      input_f0 * std::pow(2.0f, 6.0f / sonare::constants::kSemitonesPerOctave);
  CHECK_THAT(positive_zero_crossing_hz(origin_aware, 6000, 11000),
             Catch::Matchers::WithinAbs(expected_f0, 15.0f));
}

TEST_CASE("PitchCorrector validates frame origins before identity and handles huge finite origins",
          "[pitch_editor][vocal_origin]") {
  constexpr int sample_rate = 16000;
  constexpr int n_samples = 12000;
  constexpr float frame_rate_hz = 59.5f;
  constexpr int frames = 80;
  const sonare::Audio audio =
      sonare::Audio::from_vector(sine(180.0f, sample_rate, n_samples), sample_rate);
  const F0Track valid =
      track_with_voiced_run(180.0f, sample_rate, frame_rate_hz, frames, 1, frames);
  const std::vector<float> identity(static_cast<size_t>(frames), 0.0f);
  const PitchCorrector corrector;

  for (const double origin :
       {std::numeric_limits<double>::quiet_NaN(), std::numeric_limits<double>::infinity(),
        -std::numeric_limits<double>::infinity()}) {
    require_invalid_parameter([&] { corrector.resynthesize(audio, valid, identity, origin); });
  }

  F0Track malformed = valid;
  malformed.voiced.pop_back();
  require_invalid_parameter([&] { corrector.resynthesize(audio, malformed, identity, 0.0); });

  // A finite origin outside the audio range is valid. With frame 0 unvoiced,
  // positive infinity-like placement must remain a complete dry pass. This is
  // also the non-throwing identity case that guards the origin validation path.
  const double huge_positive = std::numeric_limits<double>::max() / 4.0;
  const sonare::Audio positive_dry = corrector.resynthesize(
      audio, valid, std::vector<float>(identity.size(), 6.0f), huge_positive);
  REQUIRE(positive_dry.size() == audio.size());
  for (size_t i = 0; i < audio.size(); ++i) CHECK(positive_dry[i] == audio[i]);

  // Negative huge origins exercise the clipped upper frame path. Every sample
  // maps beyond the final frame, so the result remains finite and duration
  // preserving instead of converting an out-of-range double directly to int.
  const double huge_negative = -std::numeric_limits<double>::max() / 4.0;
  const sonare::Audio negative = corrector.resynthesize(
      audio, valid, std::vector<float>(identity.size(), 6.0f), huge_negative);
  REQUIRE(negative.size() == audio.size());
  for (size_t i = 0; i < negative.size(); ++i) CHECK(std::isfinite(negative[i]));
}

TEST_CASE("PitchCorrector dry-passes a voiced curve when its origin is beyond the buffer",
          "[pitch_editor][vocal_origin]") {
  constexpr int sample_rate = 16000;
  constexpr int n_samples = 8000;
  constexpr float frame_rate_hz = 80.5f;
  constexpr int frames = 64;
  const sonare::Audio audio =
      sonare::Audio::from_vector(sine(250.0f, sample_rate, n_samples), sample_rate);
  const F0Track track =
      track_with_voiced_run(250.0f, sample_rate, frame_rate_hz, frames, 1, frames);
  const std::vector<float> deltas(static_cast<size_t>(frames), 7.0f);

  // The first frame is unvoiced, and all positive audio positions resolve to it
  // when the origin is beyond the buffer. The nonzero requested curve therefore
  // must not manufacture a correction in the source audio.
  const sonare::Audio dry =
      PitchCorrector().resynthesize(audio, track, deltas, static_cast<double>(n_samples) + 1.0);
  REQUIRE(dry.size() == audio.size());
  for (size_t i = 0; i < dry.size(); ++i) CHECK(dry[i] == audio[i]);
}

TEST_CASE("explicit vocal grid cadence is independent of rounded F0 track rate",
          "[vocal_origin][vocal_exact_cadence]") {
  const auto audio = sonare::Audio::from_vector(sine(220.0f, 16000, 16000), 16000);
  auto track = track_with_voiced_run(220.0f, 16000, 61.25f, 80, 0, 80);
  std::vector<float> deltas(80);
  for (size_t i = 0; i < deltas.size(); ++i) deltas[i] = i < 30 ? 2.0f : -3.0f;
  const double cadence = 16000.0 / 61.250000012345;
  const auto first = PitchCorrector().resynthesize(audio, track, deltas, 73.125, cadence);
  track.frame_rate_hz = 60.0f;
  const auto second = PitchCorrector().resynthesize(audio, track, deltas, 73.125, cadence);
  CHECK(maximum_difference(first, second, 0, 16000) == 0.0);
  require_invalid_parameter([&] {
    PitchCorrector().resynthesize(audio, track, deltas, 73.125,
                                  std::numeric_limits<double>::infinity());
  });
}
