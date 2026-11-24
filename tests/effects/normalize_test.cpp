/// @file normalize_test.cpp
/// @brief Tests for audio normalization and trimming.

#include "effects/normalize.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <cmath>
#include <limits>
#include <string>
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

TEST_CASE("normalize refusals read as core member names", "[normalize]") {
  Audio audio = create_audio_with_amplitude(0.5f);
  const float nan = std::numeric_limits<float>::quiet_NaN();
  const char* finite = " must be finite and in [-3.4028235e38, 3.4028235e38]";

  REQUIRE_THROWS_WITH(apply_gain(audio, nan), std::string("gain_db") + finite);
  REQUIRE_THROWS_WITH(apply_gain(audio, 800.0f), "gain_db must produce a finite linear gain");
  REQUIRE_THROWS_WITH(normalize(audio, nan), std::string("target_db") + finite);
  REQUIRE_THROWS_WITH(fade_in(audio, -0.5f), "duration_sec must be finite and at least 0");
  REQUIRE_THROWS_WITH(detect_silence_boundaries(audio, nan, 2048, 512),
                      std::string("threshold_db") + finite);
  REQUIRE_THROWS_WITH(detect_silence_boundaries(audio, -40.0f, 0, 512),
                      "frame_length must be an integer in [1, 2147483647]");
  REQUIRE_THROWS_WITH(detect_silence_boundaries(audio, -40.0f, 2048, 0),
                      "hop_length must be an integer in [1, 2147483647]");
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

TEST_CASE("normalize reaches a positive target, leaving the peak above full scale", "[normalize]") {
  const Audio audio = create_audio_with_amplitude(0.25f);
  REQUIRE_THAT(peak_db(normalize(audio, 3.0f)), WithinAbs(3.0f, 0.05f));
  REQUIRE_THAT(rms_db(normalize_rms(audio, 2.0f)), WithinAbs(2.0f, 1e-3f));
  REQUIRE_THAT(peak_db(normalize_stereo(audio, audio, 3.0f).left), WithinAbs(3.0f, 0.05f));
  REQUIRE_THAT(rms_db(normalize_rms_stereo(audio, audio, 2.0f).left), WithinAbs(2.0f, 1e-3f));
}

TEST_CASE("an RMS target lands exactly on a high-crest input whose peaks pass full scale",
          "[normalize]") {
  // A quiet bed with sparse spikes: about 24 dB of crest, so an RMS of -6 dBFS needs peaks near
  // +18 dBFS. A clip at full scale would cut the spikes and leave the RMS well under the target.
  std::vector<float> samples(22050, 0.01f);
  for (size_t i = 0; i < samples.size(); i += 441) samples[i] = 0.5f;
  const Audio audio = Audio::from_vector(std::move(samples), 22050);

  const Audio mono = normalize_rms(audio, -6.0f);
  REQUIRE_THAT(rms_db(mono), WithinAbs(-6.0f, 1e-3f));
  REQUIRE(peak_db(mono) > 6.0f);

  const auto pair = normalize_rms_stereo(audio, audio, -6.0f);
  REQUIRE_THAT(rms_db(pair.left), WithinAbs(-6.0f, 1e-3f));
  REQUIRE(peak_db(pair.right) > 6.0f);
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

// Every assertion is on the value a sample takes: a finiteness check passes whether or not a
// value was bounded or laundered on the way.
TEST_CASE("apply_gain leaves every sample its gained value", "[normalize]") {
  const float nan = std::numeric_limits<float>::quiet_NaN();
  const float inf = std::numeric_limits<float>::infinity();
  std::vector<float> samples = {0.5f, nan, 2.0f, inf, -2.0f, -inf};
  const Audio gained = apply_gain(Audio::from_vector(std::move(samples), 22050), 0.0f);
  REQUIRE(gained.data()[0] == 0.5f);
  REQUIRE(std::isnan(gained.data()[1]));
  REQUIRE(gained.data()[2] == 2.0f);
  REQUIRE(gained.data()[3] == inf);
  REQUIRE(gained.data()[4] == -2.0f);
  REQUIRE(gained.data()[5] == -inf);
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
  const Audio other_rate = Audio::from_vector(std::vector<float>(left.size(), 0.5f), 44100);
  const Audio empty = Audio::from_vector({}, 22050);

  REQUIRE_THROWS_WITH(normalize_stereo(left, shorter), "left and right must have the same length");
  REQUIRE_THROWS_WITH(normalize_stereo(left, other_rate),
                      "left and right must share a sample rate");
  REQUIRE_THROWS_WITH(normalize_stereo(left, empty), "right must not be empty");
  REQUIRE_THROWS_WITH(normalize_rms_stereo(left, shorter),
                      "left and right must have the same length");

  // Control: the same pair with none of those faults is accepted, so the
  // refusals above are about the pair rather than about the entry point.
  REQUIRE_NOTHROW(normalize_stereo(left, left));
}

TEST_CASE("normalizing digital silence is refused, however quiet a signal may be",
          "[effects][normalize]") {
  const Audio silence = Audio::from_vector(std::vector<float>(1024, 0.0f), 22050);
  const auto refused = [](auto&& call) {
    try {
      call();
    } catch (const SonareException& error) {
      CHECK(error.code() == ErrorCode::InvalidParameter);
      CHECK(std::string(error.what()) == "audio has no measurable level");
      return true;
    }
    return false;
  };
  CHECK(refused([&] { (void)normalize(silence, -1.0f); }));
  CHECK(refused([&] { (void)normalize_rms(silence, -20.0f); }));
  CHECK(refused([&] { (void)normalize_stereo(silence, silence, -1.0f); }));
  CHECK(refused([&] { (void)normalize_rms_stereo(silence, silence, -20.0f); }));

  // No epsilon stands in for silence: a -250 dBFS tone has a level and reaches the target.
  const Audio faint = create_audio_with_amplitude(3.1622777e-13f);
  const Audio peaked = normalize(faint, -6.0f);
  CHECK_THAT(peak_db(peaked), WithinAbs(-6.0f, 0.01f));
  const auto pair = normalize_stereo(faint, faint, -6.0f);
  CHECK_THAT(pair.applied_gain_db, WithinAbs(244.0f, 0.05f));
}

TEST_CASE("apply_gain refuses a finite dB value with no finite multiplier", "[normalize]") {
  const Audio silent = Audio::from_vector(std::vector<float>(64, 0.0f), 48000);
  REQUIRE_THROWS_AS(apply_gain(silent, 800.0f), SonareException);
  // A large but representable gain still applies: 1e-30 raised by 700 dB is 1e5.
  const Audio quiet = Audio::from_vector(std::vector<float>(64, 1.0e-30f), 48000);
  const Audio gained = apply_gain(quiet, 700.0f);
  for (size_t i = 0; i < gained.size(); ++i) {
    REQUIRE(std::isfinite(gained[i]));
    REQUIRE_THAT(gained[i], WithinRel(1.0e5f, 1e-3f));
  }
}
