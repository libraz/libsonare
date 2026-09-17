/// @file normalize_test.cpp
/// @brief Tests for audio normalization and trimming.

#include "effects/normalize.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <cmath>
#include <limits>
#include <vector>

#include "metering/basic.h"
#include "util/constants.h"
#include "util/exception.h"

using namespace sonare;
using Catch::Matchers::WithinAbs;
using Catch::Matchers::WithinRel;
using sonare::metering::peak_db;
using sonare::metering::rms_db;

namespace {

/// @brief Creates a test signal with specified amplitude.
Audio create_audio_with_amplitude(float amplitude, int sr = 22050, float duration = 0.5f) {
  int n_samples = static_cast<int>(sr * duration);
  std::vector<float> samples(n_samples);

  for (int i = 0; i < n_samples; ++i) {
    float t = static_cast<float>(i) / static_cast<float>(sr);
    samples[i] = amplitude * std::sin(2.0f * sonare::constants::kPiD * 440.0f * t);
  }

  return Audio::from_vector(std::move(samples), sr);
}

/// @brief Creates audio with silence at beginning and end.
Audio create_audio_with_silence(int sr = 22050) {
  std::vector<float> samples;

  // 0.2s silence
  for (int i = 0; i < sr / 5; ++i) {
    samples.push_back(0.0f);
  }

  // 0.5s tone
  for (int i = 0; i < sr / 2; ++i) {
    float t = static_cast<float>(i) / static_cast<float>(sr);
    samples.push_back(0.5f * std::sin(2.0f * sonare::constants::kPiD * 440.0f * t));
  }

  // 0.3s silence
  for (int i = 0; i < sr * 3 / 10; ++i) {
    samples.push_back(0.0f);
  }

  return Audio::from_vector(std::move(samples), sr);
}

}  // namespace

TEST_CASE("peak_db", "[normalize]") {
  Audio half_amplitude = create_audio_with_amplitude(0.5f);
  Audio full_amplitude = create_audio_with_amplitude(1.0f);

  float half_peak = peak_db(half_amplitude);
  float full_peak = peak_db(full_amplitude);

  // Full amplitude should be 0 dB
  REQUIRE_THAT(full_peak, WithinAbs(0.0f, 0.1f));

  // Half amplitude should be -6 dB
  REQUIRE_THAT(half_peak, WithinAbs(-6.0f, 0.5f));
}

TEST_CASE("rms_db", "[normalize]") {
  Audio audio = create_audio_with_amplitude(1.0f);

  float rms = rms_db(audio);

  // RMS of sine at amplitude 1 is 1/sqrt(2) ≈ -3 dB
  REQUIRE_THAT(rms, WithinAbs(-3.0f, 0.5f));
}

TEST_CASE("apply_gain", "[normalize]") {
  Audio audio = create_audio_with_amplitude(0.5f);

  // Apply +6 dB gain (double amplitude)
  Audio gained = apply_gain(audio, 6.0f);

  float original_peak = peak_db(audio);
  float gained_peak = peak_db(gained);

  REQUIRE_THAT(gained_peak - original_peak, WithinAbs(6.0f, 0.5f));
}

TEST_CASE("apply_gain negative", "[normalize]") {
  Audio audio = create_audio_with_amplitude(1.0f);

  // Apply -6 dB gain (halve amplitude)
  Audio gained = apply_gain(audio, -6.0f);

  float gained_peak = peak_db(gained);

  REQUIRE_THAT(gained_peak, WithinAbs(-6.0f, 0.5f));
}

TEST_CASE("gain and RMS normalization reject non-finite targets", "[normalize]") {
  Audio audio = create_audio_with_amplitude(0.5f);
  const float nan = std::numeric_limits<float>::quiet_NaN();
  const float infinity = std::numeric_limits<float>::infinity();

  REQUIRE_THROWS_AS(apply_gain(audio, nan), SonareException);
  REQUIRE_THROWS_AS(apply_gain(audio, infinity), SonareException);
  REQUIRE_THROWS_AS(normalize_rms(audio, nan), SonareException);
  REQUIRE_THROWS_AS(normalize_rms(audio, infinity), SonareException);
}

TEST_CASE("fades reject negative and non-finite durations", "[normalize]") {
  Audio audio = create_audio_with_amplitude(0.5f);
  const float nan = std::numeric_limits<float>::quiet_NaN();
  const float infinity = std::numeric_limits<float>::infinity();

  REQUIRE_THROWS_AS(fade_in(audio, -0.5f), SonareException);
  REQUIRE_THROWS_AS(fade_out(audio, nan), SonareException);
  REQUIRE_THROWS_AS(fade_in(audio, infinity), SonareException);
}

TEST_CASE("normalize to 0 dB", "[normalize]") {
  Audio audio = create_audio_with_amplitude(0.25f);

  Audio normalized = normalize(audio, 0.0f);

  float normalized_peak = peak_db(normalized);

  REQUIRE_THAT(normalized_peak, WithinAbs(0.0f, 0.1f));
}

TEST_CASE("normalize to -6 dB", "[normalize]") {
  Audio audio = create_audio_with_amplitude(1.0f);

  Audio normalized = normalize(audio, -6.0f);

  float normalized_peak = peak_db(normalized);

  REQUIRE_THAT(normalized_peak, WithinAbs(-6.0f, 0.5f));
}

TEST_CASE("normalize rejects a positive clipped target", "[normalize]") {
  Audio audio = create_audio_with_amplitude(0.25f);
  REQUIRE_THROWS_AS(normalize(audio, 3.0f), SonareException);

  const Audio unclipped = normalize(audio, 3.0f, false);
  REQUIRE(peak_db(unclipped) > 0.0f);
}

TEST_CASE("normalize_rms", "[normalize]") {
  Audio audio = create_audio_with_amplitude(0.25f);

  Audio normalized = normalize_rms(audio, -10.0f);

  float normalized_rms = rms_db(normalized);

  REQUIRE_THAT(normalized_rms, WithinAbs(-10.0f, 0.5f));
}

TEST_CASE("trim_absolute silence", "[normalize]") {
  Audio audio = create_audio_with_silence();

  Audio trimmed = trim_absolute(audio, -40.0f);

  // Trimmed should be shorter
  REQUIRE(trimmed.size() < audio.size());

  // Trimmed should not be empty
  REQUIRE(!trimmed.empty());
}

TEST_CASE("detect_silence_boundaries", "[normalize]") {
  Audio audio = create_audio_with_silence();

  auto [start, end] = detect_silence_boundaries(audio, -40.0f);

  // Start should be after initial silence
  REQUIRE(start > 0);

  // End should be before final silence
  REQUIRE(end < audio.size());

  // Valid range
  REQUIRE(start < end);
}

TEST_CASE("detect_silence_boundaries rejects non-positive frame/hop", "[normalize]") {
  Audio audio = create_audio_with_silence();
  // A zero/negative hop would spin forever; a zero frame_length divides by zero.
  REQUIRE_THROWS(detect_silence_boundaries(audio, -40.0f, 2048, 0));
  REQUIRE_THROWS(detect_silence_boundaries(audio, -40.0f, 0, 512));
  REQUIRE_THROWS(detect_silence_boundaries(audio, -40.0f, 2048, -1));
  REQUIRE_THROWS(trim_absolute(audio, -40.0f, 2048, 0));
}

TEST_CASE("fade_in", "[normalize]") {
  Audio audio = create_audio_with_amplitude(1.0f, 22050, 1.0f);

  Audio faded = fade_in(audio, 0.1f);

  REQUIRE(!faded.empty());
  REQUIRE(faded.size() == audio.size());

  // First sample should be near zero
  REQUIRE(std::abs(faded.data()[0]) < 0.01f);

  // Sample at 50% of fade should be between 0 and original
  int mid_fade = audio.sample_rate() / 20;  // 0.05s
  REQUIRE(std::abs(faded.data()[mid_fade]) < std::abs(audio.data()[mid_fade]));
}

TEST_CASE("fade_out", "[normalize]") {
  Audio audio = create_audio_with_amplitude(1.0f, 22050, 1.0f);

  Audio faded = fade_out(audio, 0.1f);

  REQUIRE(!faded.empty());
  REQUIRE(faded.size() == audio.size());

  // Last sample should be near zero
  REQUIRE(std::abs(faded.data()[faded.size() - 1]) < 0.01f);
}

TEST_CASE("fade preserves duration", "[normalize]") {
  Audio audio = create_audio_with_amplitude(1.0f);

  Audio faded_in = fade_in(audio, 0.1f);
  Audio faded_out = fade_out(audio, 0.1f);

  REQUIRE(faded_in.duration() == audio.duration());
  REQUIRE(faded_out.duration() == audio.duration());
}

TEST_CASE("detect_silence_boundaries with all-silent input", "[normalize]") {
  // No frame clears the threshold, so the full-signal bounds are the contract:
  // anything narrower makes trim_absolute delete a quiet take.
  std::vector<float> silent(22050, 0.0f);
  Audio silent_audio = Audio::from_buffer(silent.data(), silent.size(), 22050);
  auto [start, end] = detect_silence_boundaries(silent_audio);
  CHECK(start == 0);
  CHECK(end == silent_audio.size());
  CHECK(start < end);
}

TEST_CASE("detect_silence_boundaries with very short input", "[normalize]") {
  // Shorter than frame_length (2048), so no frame is scanned at all and the
  // full-signal bounds are the contract.
  std::vector<float> short_signal(100, 0.5f);
  Audio short_audio = Audio::from_buffer(short_signal.data(), short_signal.size(), 22050);
  auto [start, end] = detect_silence_boundaries(short_audio);
  CHECK(start == 0);
  CHECK(end == short_audio.size());
  CHECK(start < end);
}

TEST_CASE("trim_absolute keeps all-silent and sub-frame input intact", "[normalize]") {
  SECTION("all-silent") {
    std::vector<float> silent(22050, 0.0f);
    Audio silent_audio = Audio::from_buffer(silent.data(), silent.size(), 22050);
    Audio trimmed = trim_absolute(silent_audio);
    CHECK(trimmed.size() == silent_audio.size());
    CHECK_FALSE(trimmed.empty());
  }

  SECTION("shorter than frame_length") {
    std::vector<float> short_signal(100, 0.5f);
    Audio short_audio = Audio::from_buffer(short_signal.data(), short_signal.size(), 22050);
    Audio trimmed = trim_absolute(short_audio);
    CHECK(trimmed.size() == short_audio.size());
    CHECK_FALSE(trimmed.empty());
  }
}

TEST_CASE("detect_silence_boundaries with sound only at end", "[normalize]") {
  // Tests the backward scan edge case (regression: size_t underflow)
  std::vector<float> signal(22050, 0.0f);
  // Put sound at the very end
  for (size_t i = signal.size() - 100; i < signal.size(); ++i) {
    signal[i] = 0.5f;
  }
  Audio audio = Audio::from_buffer(signal.data(), signal.size(), 22050);
  auto [start, end] = detect_silence_boundaries(audio);
  REQUIRE(end > start);
}

// ===================================================================
// The clip path must not turn a non-finite sample into a peak. Every
// assertion below is on the value the sample takes, because a check
// that the output is finite passes whether or not that happened.
// ===================================================================
TEST_CASE("apply_gain clipping does not launder a non-finite sample", "[normalize]") {
  const float nan = std::numeric_limits<float>::quiet_NaN();
  const float inf = std::numeric_limits<float>::infinity();
  std::vector<float> samples = {0.5f, nan, 2.0f, inf, -2.0f, -inf};
  Audio audio = Audio::from_vector(std::move(samples), 22050);

  const Audio clipped = apply_gain(audio, 0.0f, /*clip=*/true);
  // apply_gain returns to its caller, which is the downstream, so the NaN
  // propagates and is itself the report. A nested std::max(-1, std::min(1, x))
  // sent it to +1.0f instead, indistinguishable from a peak this function meant
  // to produce.
  REQUIRE(std::isnan(clipped.data()[1]));
  // The clip's own bounds still apply to everything else, infinities included:
  // folding one onto the bound is the limit of the transfer function rather
  // than a substituted value.
  REQUIRE(clipped.data()[0] == 0.5f);
  REQUIRE(clipped.data()[2] == 1.0f);
  REQUIRE(clipped.data()[3] == 1.0f);
  REQUIRE(clipped.data()[4] == -1.0f);
  REQUIRE(clipped.data()[5] == -1.0f);

  // Control: with clipping off nothing is bounded, so the assertions above are
  // about the clip path rather than about apply_gain's arithmetic.
  const Audio unclipped = apply_gain(audio, 0.0f, /*clip=*/false);
  REQUIRE(unclipped.data()[2] == 2.0f);
  REQUIRE(unclipped.data()[3] == inf);
  REQUIRE(std::isnan(unclipped.data()[1]));
}

TEST_CASE("normalize_stereo moves both channels by a single gain", "[effects][normalize]") {
  // 12 dB apart, so a gain applied per channel and a gain shared by the pair put
  // the quiet side in two places far outside any tolerance.
  const Audio left = create_audio_with_amplitude(0.5f);
  const Audio right = create_audio_with_amplitude(0.125f);
  const float before = peak_db(left) - peak_db(right);
  REQUIRE_THAT(before, WithinAbs(12.0f, 0.1f));

  const auto result = normalize_stereo(left, right, -1.0f);

  // The louder channel reaches the target and the other keeps its distance.
  REQUIRE_THAT(peak_db(result.left), WithinAbs(-1.0f, 0.05f));
  REQUIRE_THAT(peak_db(result.left) - peak_db(result.right), WithinAbs(before, 0.01f));
  REQUIRE_THAT(result.applied_gain_db, WithinAbs(-1.0f - peak_db(left), 0.05f));

  // Control: this is where a per-channel gain would have put the quiet side. The
  // assertions above can only hold for one of the two, so they are not merely
  // observing that normalization happened.
  const Audio per_channel_right = normalize(right, -1.0f);
  REQUIRE_THAT(peak_db(per_channel_right), WithinAbs(-1.0f, 0.05f));
  REQUIRE(peak_db(result.right) < peak_db(per_channel_right) - 10.0f);
}

TEST_CASE("normalize_rms_stereo measures the two channels together", "[effects][normalize]") {
  const Audio left = create_audio_with_amplitude(0.5f);
  const Audio right = create_audio_with_amplitude(0.125f);

  const auto result = normalize_rms_stereo(left, right, -20.0f);

  // The quantity driven to the target is the RMS over both channels' samples,
  // which is the quadratic mean of the two per-channel figures.
  const double l_lin = std::pow(10.0, rms_db(result.left) / 20.0);
  const double r_lin = std::pow(10.0, rms_db(result.right) / 20.0);
  const double joint_db = 20.0 * std::log10(std::sqrt((l_lin * l_lin + r_lin * r_lin) / 2.0));
  REQUIRE_THAT(static_cast<float>(joint_db), WithinAbs(-20.0f, 0.05f));

  // Control: neither channel lands on the target by itself, so the assertion
  // above is about the joint figure rather than about either channel.
  REQUIRE(std::abs(rms_db(result.left) + 20.0f) > 1.0f);
  REQUIRE(std::abs(rms_db(result.right) + 20.0f) > 1.0f);
}

TEST_CASE("the stereo normalizers refuse a pair they cannot process", "[effects][normalize]") {
  const Audio left = create_audio_with_amplitude(0.5f);
  const Audio shorter = left.slice_samples(0, left.size() / 2);
  const Audio other_rate = create_audio_with_amplitude(0.5f, 44100);
  const Audio empty = Audio::from_vector({}, 22050);

  REQUIRE_THROWS_AS(normalize_stereo(left, shorter), SonareException);
  REQUIRE_THROWS_AS(normalize_stereo(left, other_rate), SonareException);
  REQUIRE_THROWS_AS(normalize_stereo(left, empty), SonareException);
  REQUIRE_THROWS_AS(normalize_rms_stereo(left, shorter), SonareException);

  // Control: the same pair with none of those faults is accepted, so the
  // refusals above are about the pair rather than about the entry point.
  REQUIRE_NOTHROW(normalize_stereo(left, left));
}

TEST_CASE("a silent stereo pair is left alone", "[effects][normalize]") {
  const Audio silence = Audio::from_vector(std::vector<float>(1024, 0.0f), 22050);

  const auto result = normalize_stereo(silence, silence);

  REQUIRE(result.applied_gain_db == 0.0f);
  REQUIRE(result.left.data()[0] == 0.0f);
  REQUIRE(result.right.data()[512] == 0.0f);

  // Control: a pair that is not silent does move, so the assertions above are
  // about the silence short-circuit rather than about normalize_stereo being a
  // no-op.
  const Audio quiet = create_audio_with_amplitude(0.01f);
  REQUIRE(normalize_stereo(quiet, quiet).applied_gain_db > 10.0f);
}
