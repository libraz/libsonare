#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <cmath>
#include <limits>
#include <new>
#include <vector>

#include "mastering/api/named_processor.h"
#include "mastering/common/loudness_measure.h"
#include "mastering/match/ab_switcher.h"
#include "support/alloc_guard.h"
#include "support/audio_fixtures.h"
#include "util/constants.h"

using Catch::Matchers::WithinAbs;
using namespace sonare;
using namespace sonare::mastering::api;
using namespace sonare::mastering::match;
using sonare::mastering::api::Param;
using sonare::test::generate_sine_audio;

namespace {

// Full-scale-ish square wave: much higher BS.1770 loudness than a sine at the
// same peak, so it stands in for a heavily loudness-maximized reference.
Audio square_audio(float frequency_hz, float amplitude, int sample_rate = 48000,
                   float duration_sec = 1.0f) {
  const int samples = static_cast<int>(duration_sec * static_cast<float>(sample_rate));
  std::vector<float> out(static_cast<std::size_t>(samples));
  const double period = static_cast<double>(sample_rate) / frequency_hz;
  for (int i = 0; i < samples; ++i) {
    const double phase = std::fmod(static_cast<double>(i), period) / period;
    out[static_cast<std::size_t>(i)] = phase < 0.5 ? amplitude : -amplitude;
  }
  return Audio::from_vector(std::move(out), sample_rate);
}

}  // namespace

TEST_CASE("ABMatchLoudness matches a quiet b to a loud a", "[mastering][match][ab-match]") {
  const auto a = generate_sine_audio(1000.0f, 48000, 1.0f, 0.5f);
  const auto b = generate_sine_audio(1000.0f, 48000, 1.0f, 0.05f);

  const auto matched = ab_match_loudness(a, b);

  // The output's measured loudness is what a stub returning gain 1 (unity)
  // cannot produce: it would leave b's LUFS exactly where it started, clearly
  // short of a's.
  const float a_lufs = mastering::common::measure_lufs(a);
  const float matched_b_lufs = mastering::common::measure_lufs(matched.b);
  REQUIRE_THAT(matched_b_lufs, WithinAbs(a_lufs, 0.2f));
  REQUIRE(matched.applied_gain_db > 5.0f);
}

TEST_CASE("ABMatchLoudness applies ~0 dB when a and b already match",
          "[mastering][match][ab-match]") {
  const auto a = generate_sine_audio(1000.0f, 48000, 1.0f, 0.2f);
  const auto b = generate_sine_audio(1000.0f, 48000, 1.0f, 0.2f);

  const auto matched = ab_match_loudness(a, b);

  REQUIRE_THAT(matched.applied_gain_db, WithinAbs(0.0f, 0.05f));
  REQUIRE_THAT(matched.b[0], WithinAbs(b[0], 1e-4f));
}

TEST_CASE("ABMatchLoudness leaves silent b unchanged instead of diverging",
          "[mastering][match][ab-match]") {
  const auto a = generate_sine_audio(1000.0f, 48000, 1.0f, 0.5f);
  const std::vector<float> zeros(48000, 0.0f);
  const Audio silent_b = Audio::from_vector(zeros, 48000);

  const auto matched = ab_match_loudness(a, silent_b);

  REQUIRE(matched.applied_gain_db == 0.0f);
  REQUIRE(matched.b.size() == silent_b.size());
  for (std::size_t i = 0; i < matched.b.size(); ++i) {
    REQUIRE(matched.b[i] == 0.0f);
  }
  // Pins the finite silence sentinel true_peak_db() reports here instead of
  // -inf or NaN; no other case in this file is silent, so nothing else checks it.
  REQUIRE(matched.matched_true_peak_dbtp == sonare::constants::kFloorDb);
}

TEST_CASE("ABMatchLoudness leaves b unchanged when a is silent", "[mastering][match][ab-match]") {
  const std::vector<float> zeros(48000, 0.0f);
  const Audio silent_a = Audio::from_vector(zeros, 48000);
  const auto b = generate_sine_audio(1000.0f, 48000, 1.0f, 0.2f);

  const auto matched = ab_match_loudness(silent_a, b);

  REQUIRE(matched.applied_gain_db == 0.0f);
  for (std::size_t i = 0; i < matched.b.size(); ++i) {
    REQUIRE_THAT(matched.b[i], WithinAbs(b[i], 1e-6f));
  }
  REQUIRE(matched.matched_true_peak_dbtp > sonare::constants::kFloorDb);
}

TEST_CASE("ABMatchLoudness matches faithfully even when b is near full scale",
          "[mastering][match][ab-match]") {
  // b sits close to full scale already. A clamp keyed on b's own headroom
  // would leave b at its own loudness here; the match must still be applied
  // in full, with the resulting overshoot only reported, not prevented.
  const auto b = generate_sine_audio(1000.0f, 48000, 1.0f, 0.98f);
  const auto a = square_audio(1000.0f, 0.98f);

  const float a_lufs = mastering::common::measure_lufs(a);
  const float b_lufs = mastering::common::measure_lufs(b);
  REQUIRE(a_lufs > b_lufs + 1.0f);  // sanity: the square really is louder in LUFS

  const auto matched = ab_match_loudness(a, b);

  const float matched_b_lufs = mastering::common::measure_lufs(matched.b);
  REQUIRE_THAT(matched_b_lufs, WithinAbs(a_lufs, 0.2f));
  REQUIRE(matched.matched_true_peak_dbtp > 0.0f);
}

TEST_CASE("ABMatchLoudness validates like the existing A/B helpers",
          "[mastering][match][ab-match]") {
  const Audio empty;
  const auto b = generate_sine_audio(1000.0f, 48000, 1.0f, 0.2f);
  REQUIRE_THROWS(ab_match_loudness(empty, b));
  REQUIRE_THROWS(ab_match_loudness(b, empty));

  const auto a_44k = generate_sine_audio(1000.0f, 44100, 1.0f, 0.2f);
  const auto b_48k = generate_sine_audio(1000.0f, 48000, 1.0f, 0.2f);
  REQUIRE_THROWS(ab_match_loudness(a_44k, b_48k));
}

TEST_CASE("ABMatchLoudness returns a unchanged", "[mastering][match][ab-match]") {
  const auto a = generate_sine_audio(1000.0f, 48000, 1.0f, 0.3f);
  const auto b = generate_sine_audio(1000.0f, 48000, 1.0f, 0.1f);

  const auto matched = ab_match_loudness(a, b);

  REQUIRE(matched.a.data() == a.data());
  REQUIRE(matched.a.size() == a.size());
}

TEST_CASE("ABMatchLoudness composes with the existing switch and crossfade",
          "[mastering][match][ab-match]") {
  const auto a = generate_sine_audio(1000.0f, 48000, 1.0f, 0.3f);
  const auto b = generate_sine_audio(1000.0f, 48000, 1.0f, 0.05f);

  const auto matched = ab_match_loudness(a, b);
  const auto switched = ab_switch(matched.a, matched.b, ABSelection::B);
  const auto crossfaded = ab_crossfade(matched.a, matched.b, 0.5f);

  const float a_lufs = mastering::common::measure_lufs(a);
  REQUIRE_THAT(mastering::common::measure_lufs(switched), WithinAbs(a_lufs, 0.2f));
  REQUIRE(crossfaded.size() == std::min(matched.a.size(), matched.b.size()));
}

TEST_CASE("ABMatchLoudnessStereo uses one gain without downmix cancellation",
          "[mastering][match][ab-match][stereo]") {
  constexpr int kSampleRate = 48000;
  const auto reference_left = generate_sine_audio(440.0f, kSampleRate, 1.0f, 0.25f);
  const auto reference_right = generate_sine_audio(660.0f, kSampleRate, 1.0f, 0.25f);
  const auto source_left = generate_sine_audio(440.0f, kSampleRate, 1.0f, 0.05f);
  auto source_right = generate_sine_audio(660.0f, kSampleRate, 1.0f, 0.05f);
  std::vector<float> source_left_data(source_left.begin(), source_left.end());
  std::vector<float> source_right_data(source_right.begin(), source_right.end());
  for (float& sample : source_left_data) sample *= 0.2f;
  for (float& sample : source_right_data) sample *= -0.2f;

  const StereoAudioPair reference{reference_left, reference_right};
  const StereoAudioPair source{Audio::from_vector(std::move(source_left_data), kSampleRate),
                               Audio::from_vector(std::move(source_right_data), kSampleRate)};
  const auto matched = ab_match_loudness_stereo(reference, source);

  std::vector<float> reference_interleaved;
  reference_interleaved.reserve(reference_left.size() * 2);
  for (std::size_t index = 0; index < reference_left.size(); ++index) {
    reference_interleaved.push_back(reference_left[index]);
    reference_interleaved.push_back(reference_right[index]);
  }

  REQUIRE(matched.a.left.size() == reference_left.size());
  REQUIRE(matched.b.left.size() == source.left.size());
  REQUIRE(matched.b.right.size() == source.right.size());
  REQUIRE_THAT(matched.reference_lufs,
               WithinAbs(mastering::common::measure_lufs_interleaved(
                             reference_interleaved.data(), reference_left.size(), 2, kSampleRate),
                         0.01f));
  std::vector<float> matched_interleaved;
  matched_interleaved.reserve(matched.b.left.size() * 2);
  for (std::size_t index = 0; index < matched.b.left.size(); ++index) {
    matched_interleaved.push_back(matched.b.left[index]);
    matched_interleaved.push_back(matched.b.right[index]);
  }
  const float matched_lufs = mastering::common::measure_lufs_interleaved(
      matched_interleaved.data(), matched.b.left.size(), 2, kSampleRate);
  REQUIRE_THAT(matched_lufs, WithinAbs(matched.reference_lufs, 0.2f));
  REQUIRE(std::isfinite(matched.applied_gain_db));
  REQUIRE(matched.applied_gain_db > 0.0f);

  const float source_ratio =
      source.left[1000] == 0.0f ? 0.0f : source.right[1000] / source.left[1000];
  const float matched_ratio =
      matched.b.left[1000] == 0.0f ? 0.0f : matched.b.right[1000] / matched.b.left[1000];
  REQUIRE_THAT(matched_ratio, WithinAbs(source_ratio, 1.0e-4f));
}

TEST_CASE("ABMatchLoudnessStereo measures exact antiphase and dual-mono controls",
          "[mastering][match][ab-match][stereo]") {
  constexpr int kSampleRate = 48000;
  constexpr float kStereoToMonoDb = 3.0103f;
  const Audio reference_left = generate_sine_audio(440.0f, kSampleRate, 1.0f, 0.5f);
  const Audio source_left = generate_sine_audio(440.0f, kSampleRate, 1.0f, 0.125f);
  std::vector<float> reference_right(reference_left.size());
  std::vector<float> source_right(source_left.size());
  for (std::size_t index = 0; index < reference_left.size(); ++index) {
    reference_right[index] = -reference_left[index];
    source_right[index] = -source_left[index];
  }

  const StereoAudioPair reference{reference_left,
                                  Audio::from_vector(std::move(reference_right), kSampleRate)};
  const StereoAudioPair source{source_left,
                               Audio::from_vector(std::move(source_right), kSampleRate)};
  const auto matched = ab_match_loudness_stereo(reference, source);
  const float mono_reference_lufs = mastering::common::measure_lufs(reference_left);
  const float mono_source_lufs = mastering::common::measure_lufs(source_left);

  // A 0.5*(L+R) implementation would turn both exact antiphase pairs into
  // silence. BS.1770 channel summing keeps their energy and returns the
  // expected +3.01 dB dual-channel offset.
  REQUIRE(std::isfinite(matched.reference_lufs));
  REQUIRE(std::isfinite(matched.source_lufs));
  REQUIRE_THAT(matched.reference_lufs, WithinAbs(mono_reference_lufs + kStereoToMonoDb, 0.05f));
  REQUIRE_THAT(matched.source_lufs, WithinAbs(mono_source_lufs + kStereoToMonoDb, 0.05f));
  REQUIRE(matched.applied_gain_db > 10.0f);
  REQUIRE_THAT(matched.applied_gain_db,
               WithinAbs(matched.reference_lufs - matched.source_lufs, 0.01f));

  const std::size_t sample_index = 1000;
  REQUIRE(matched.b.left[sample_index] != 0.0f);
  REQUIRE_THAT(matched.b.right[sample_index], WithinAbs(-matched.b.left[sample_index], 1.0e-6f));

  // Dual-mono is the control case: the same channel summing must apply when
  // both planes are in phase, and the shared gain must agree with mono A/B.
  const auto dual_mono =
      ab_match_loudness_stereo({reference_left, reference_left}, {source_left, source_left});
  const auto mono_match = ab_match_loudness(reference_left, source_left);
  REQUIRE_THAT(dual_mono.reference_lufs, WithinAbs(mono_reference_lufs + kStereoToMonoDb, 0.05f));
  REQUIRE_THAT(dual_mono.source_lufs, WithinAbs(mono_source_lufs + kStereoToMonoDb, 0.05f));
  REQUIRE_THAT(dual_mono.applied_gain_db, WithinAbs(mono_match.applied_gain_db, 0.05f));
}

TEST_CASE("ABMatchLoudnessStereo reports the dominant channel true peak",
          "[mastering][match][ab-match][stereo]") {
  constexpr int kSampleRate = 48000;
  std::vector<float> source_left(48000, 0.01f);
  std::vector<float> source_right(48000, 0.4f);
  std::vector<float> reference_left(48000, 0.03f);
  std::vector<float> reference_right(48000, 0.03f);
  const StereoAudioPair reference{Audio::from_vector(std::move(reference_left), kSampleRate),
                                  Audio::from_vector(std::move(reference_right), kSampleRate)};
  const StereoAudioPair source{Audio::from_vector(std::move(source_left), kSampleRate),
                               Audio::from_vector(std::move(source_right), kSampleRate)};

  const auto matched = ab_match_loudness_stereo(reference, source);
  const float measured = mastering::common::measure_true_peak_dbtp_stereo_planar(
      matched.b.left.data(), matched.b.right.data(), matched.b.left.size());
  REQUIRE_THAT(matched.matched_true_peak_dbtp, WithinAbs(measured, 1.0e-5f));
  const float left_peak = mastering::common::measure_true_peak_dbtp(matched.b.left);
  REQUIRE(matched.matched_true_peak_dbtp > left_peak + 10.0f);
}

TEST_CASE("ABMatchLoudnessStereo keeps silent pairs at unity",
          "[mastering][match][ab-match][stereo]") {
  constexpr int kSampleRate = 48000;
  const StereoAudioPair silence{Audio::from_vector(std::vector<float>(48000, 0.0f), kSampleRate),
                                Audio::from_vector(std::vector<float>(48000, 0.0f), kSampleRate)};
  const auto matched = ab_match_loudness_stereo(silence, silence);
  REQUIRE(matched.applied_gain_db == 0.0f);
  REQUIRE(matched.matched_true_peak_dbtp == sonare::constants::kFloorDb);
  REQUIRE(std::equal(matched.b.left.begin(), matched.b.left.end(), silence.left.begin()));
  REQUIRE(std::equal(matched.b.right.begin(), matched.b.right.end(), silence.right.begin()));
}

TEST_CASE("ABMatchLoudnessStereo validates pair shape and sample rate",
          "[mastering][match][ab-match][stereo]") {
  const auto left = generate_sine_audio(440.0f, 48000, 1.0f, 0.5f);
  const auto right = generate_sine_audio(440.0f, 48000, 1.0f, 0.5f);
  const auto short_right = generate_sine_audio(440.0f, 48000, 0.25f, 1.0f);
  REQUIRE_THROWS(ab_match_loudness_stereo({left, short_right}, {left, right}));
  const auto right_44k = generate_sine_audio(440.0f, 44100, 1.0f, 0.5f);
  REQUIRE_THROWS(ab_match_loudness_stereo({left, right}, {left, right_44k}));
}

TEST_CASE("ABMatchLoudnessStereo rejects nonfinite samples and unsupported rates",
          "[mastering][match][ab-match][stereo]") {
  constexpr int kSampleRate = 48000;
  const auto left = generate_sine_audio(440.0f, kSampleRate, 1.0f, 0.5f);
  const auto right = generate_sine_audio(660.0f, kSampleRate, 1.0f, 0.5f);

  std::vector<float> nan_left(left.begin(), left.end());
  nan_left[1000] = std::numeric_limits<float>::quiet_NaN();
  REQUIRE_THROWS(ab_match_loudness_stereo(
      {Audio::from_vector(std::move(nan_left), kSampleRate), right}, {left, right}));

  std::vector<float> inf_right(right.begin(), right.end());
  inf_right[1000] = std::numeric_limits<float>::infinity();
  REQUIRE_THROWS(ab_match_loudness_stereo(
      {left, Audio::from_vector(std::move(inf_right), kSampleRate)}, {left, right}));

  const Audio low_rate_left = Audio::from_vector(std::vector<float>(48000, 0.1f), 7999);
  const Audio low_rate_right = Audio::from_vector(std::vector<float>(48000, 0.1f), 7999);
  REQUIRE_THROWS(
      ab_match_loudness_stereo({low_rate_left, low_rate_right}, {low_rate_left, low_rate_right}));
}

TEST_CASE("ABCrossfadeStereo applies one mix to both planes and uses the shorter pair",
          "[mastering][match][ab-match][stereo]") {
  constexpr int kSampleRate = 48000;
  const StereoAudioPair a{Audio::from_vector(std::vector<float>{1.0f, 2.0f, 3.0f}, kSampleRate),
                          Audio::from_vector(std::vector<float>{-1.0f, -2.0f, -3.0f}, kSampleRate)};
  const StereoAudioPair b{Audio::from_vector(std::vector<float>{5.0f, 6.0f}, kSampleRate),
                          Audio::from_vector(std::vector<float>{-5.0f, -6.0f}, kSampleRate)};

  const auto mixed = ab_crossfade_stereo(a, b, 0.25f);
  REQUIRE(mixed.left.size() == b.left.size());
  REQUIRE(mixed.right.size() == b.right.size());
  REQUIRE_THAT(mixed.left[0], WithinAbs(2.0f, 1.0e-6f));
  REQUIRE_THAT(mixed.left[1], WithinAbs(3.0f, 1.0e-6f));
  REQUIRE_THAT(mixed.right[0], WithinAbs(-2.0f, 1.0e-6f));
  REQUIRE_THAT(mixed.right[1], WithinAbs(-3.0f, 1.0e-6f));
}

TEST_CASE("Named stereo A/B crossfade is linear and uses the shorter pair",
          "[mastering][match][ab-match][stereo]") {
  constexpr int kSampleRate = 48000;
  const std::vector<float> source_left(8, 1.0f);
  const std::vector<float> source_right(8, -1.0f);
  const std::vector<float> reference_left(5, 3.0f);
  const std::vector<float> reference_right(5, -3.0f);
  const auto result = apply_named_pair_processor_stereo(
      "match.abCrossfade", source_left.data(), source_right.data(), source_left.size(),
      reference_left.data(), reference_right.data(), reference_left.size(), kSampleRate,
      {Param{"mix", 0.25}});
  REQUIRE(result.left.size() == reference_left.size());
  REQUIRE_THAT(result.left[0], WithinAbs(1.5f, 1.0e-6f));
  REQUIRE_THAT(result.right[0], WithinAbs(-1.5f, 1.0e-6f));

  const auto at_a = apply_named_pair_processor_stereo(
      "match.abCrossfade", source_left.data(), source_right.data(), source_left.size(),
      reference_left.data(), reference_right.data(), reference_left.size(), kSampleRate,
      {Param{"mix", 0.0}});
  const auto at_b = apply_named_pair_processor_stereo(
      "match.abCrossfade", source_left.data(), source_right.data(), source_left.size(),
      reference_left.data(), reference_right.data(), reference_left.size(), kSampleRate,
      {Param{"mix", 1.0}});
  REQUIRE(at_a.left[0] == source_left[0]);
  REQUIRE(at_b.left[0] == reference_left[0]);
}

TEST_CASE("Named stereo A/B crossfade validates every parameter like its mono sibling",
          "[mastering][match][ab-match][stereo]") {
  constexpr int kSampleRate = 48000;
  const std::vector<float> samples(4800, 0.1f);
  // An unread key still has to be finite: the mono pair entry point refuses it.
  const std::vector<Param> params{Param{"mix", 0.5},
                                  Param{"unread", std::numeric_limits<double>::quiet_NaN()}};
  REQUIRE_THROWS_AS(apply_named_pair_processor("match.abCrossfade", samples.data(), samples.data(),
                                               samples.size(), kSampleRate, params),
                    SonareException);
  REQUIRE_THROWS_AS(apply_named_pair_processor_stereo(
                        "match.abCrossfade", samples.data(), samples.data(), samples.size(),
                        samples.data(), samples.data(), samples.size(), kSampleRate, params),
                    SonareException);
}

TEST_CASE("Named stereo A/B crossfade rejects other pairs and bad parameters",
          "[mastering][match][ab-match][stereo]") {
  constexpr int kSampleRate = 48000;
  const std::vector<float> samples(48000, 0.1f);
  REQUIRE_THROWS(apply_named_pair_processor_stereo("match.abSwitch", samples.data(), samples.data(),
                                                   samples.size(), samples.data(), samples.data(),
                                                   samples.size(), kSampleRate));
  REQUIRE_THROWS(apply_named_pair_processor_stereo("unknown", samples.data(), samples.data(),
                                                   samples.size(), samples.data(), samples.data(),
                                                   samples.size(), kSampleRate));
  REQUIRE_THROWS(apply_named_pair_processor_stereo(
      "match.abCrossfade", samples.data(), samples.data(), samples.size(), samples.data(),
      samples.data(), samples.size(), kSampleRate, {Param{"mix", -0.01}}));
  REQUIRE_THROWS(apply_named_pair_processor_stereo(
      "match.abCrossfade", samples.data(), samples.data(), samples.size(), samples.data(),
      samples.data(), samples.size(), kSampleRate, {Param{"mix", 1.01}}));
}

TEST_CASE("Named stereo A/B crossfade reads parameters like the mono pair processor",
          "[mastering][match][ab-match][stereo]") {
  constexpr int kSampleRate = 48000;
  const std::vector<float> source(8, 1.0f);
  const std::vector<float> reference(8, 3.0f);
  const std::vector<std::vector<Param>> param_sets = {
      {Param{"unexpected", 0.5}},
      {Param{"mix", 0.2}, Param{"mix", 0.8}},
  };
  for (const auto& params : param_sets) {
    const auto mono = apply_named_pair_processor(
        "match.abCrossfade", source.data(), reference.data(), source.size(), kSampleRate, params);
    const auto stereo = apply_named_pair_processor_stereo(
        "match.abCrossfade", source.data(), source.data(), reference.data(), reference.data(),
        source.size(), kSampleRate, params);
    REQUIRE(stereo.left == mono.samples);
    REQUIRE(stereo.right == mono.samples);
  }
}

TEST_CASE("Named stereo pair rejects oversized buffers before copying",
          "[mastering][match][ab-match][stereo]") {
  constexpr int kSampleRate = 48000;
  const float sample = 0.25f;
  const std::size_t oversized = sonare::kMaxAudioBufferSize + 1;
  bool invalid_parameter = false;
  bool bad_alloc = false;
  {
    sonare::test::AllocationFailureGuard guard(1024);
    try {
      static_cast<void>(apply_named_pair_processor_stereo("match.abCrossfade", &sample, &sample,
                                                          oversized, &sample, &sample, oversized,
                                                          kSampleRate));
    } catch (const SonareException& error) {
      invalid_parameter = error.code() == ErrorCode::InvalidParameter;
    } catch (const std::bad_alloc&) {
      bad_alloc = true;
    }
  }
  REQUIRE(invalid_parameter);
  REQUIRE_FALSE(bad_alloc);
}
