#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <cmath>
#include <cstddef>
#include <vector>

#include "core/audio.h"
#include "core/resample.h"
#include "mastering/api/named_processor.h"
#include "mastering/match/reference_spectrum.h"
#include "metering/lufs.h"
#include "util/constants.h"
#include "util/db.h"
#include "util/json.h"

namespace {

namespace json = sonare::util::json;
using Catch::Matchers::WithinAbs;
using sonare::mastering::api::analyze_named_pair;
using Samples = std::vector<float>;

constexpr int kSampleRate = static_cast<int>(sonare::constants::kDefaultDawSampleRate);
constexpr double kResultTolerance = 1.0e-6;
constexpr double kLoudnessTolerance = 0.01;

Samples signal(float frequency, float amplitude, int sample_rate = kSampleRate) {
  Samples samples(sample_rate);
  for (std::size_t i = 0; i < samples.size(); ++i) {
    const double phase = sonare::constants::kTwoPiD * frequency * i / sample_rate;
    samples[i] = amplitude * static_cast<float>(std::sin(phase));
  }
  return samples;
}

json::Value analyze_mono(const char* name, const Samples& source, const Samples& reference) {
  return json::parse(analyze_named_pair(name, source.data(), reference.data(), source.size(),
                                        reference.size(), kSampleRate, kSampleRate, {}));
}

json::Value analyze_stereo(const char* name, const Samples& source_left,
                           const Samples& source_right, const Samples& reference_left,
                           const Samples& reference_right,
                           int reference_sample_rate = kSampleRate) {
  REQUIRE(source_left.size() == source_right.size());
  REQUIRE(reference_left.size() == reference_right.size());
  return json::parse(analyze_named_pair(
      name, source_left.data(), source_right.data(), source_left.size(), reference_left.data(),
      reference_right.data(), reference_left.size(), kSampleRate, reference_sample_rate, {}));
}

float stereo_lufs(const Samples& left, const Samples& right) {
  std::vector<float> interleaved;
  interleaved.reserve(left.size() * 2);
  for (std::size_t i = 0; i < left.size(); ++i) {
    interleaved.push_back(left[i]);
    interleaved.push_back(right[i]);
  }
  return sonare::metering::lufs_interleaved(interleaved.data(), left.size(), 2, kSampleRate)
      .integrated_lufs;
}

void check_field(const json::Value& actual, const json::Value& expected) {
  if (expected.is_null()) {
    CHECK(actual.is_null());
  } else {
    REQUIRE(actual.is_number());
    CHECK_THAT(actual.as_number(), WithinAbs(expected.as_number(), kResultTolerance));
  }
}

}  // namespace

TEST_CASE("Pair loudness agrees with the mono and stereo meters", "[pair_stereo]") {
  const Samples source_left = signal(440.0f, 0.12f);
  const Samples source_right = signal(800.0f, 0.28f);
  const Samples reference_left = signal(500.0f, 0.24f);
  const Samples reference_right = signal(1200.0f, 0.32f);
  const auto mono = analyze_mono("match.referenceLoudness", source_left, reference_left);
  const auto stereo = analyze_stereo("match.referenceLoudness", source_left, source_right,
                                     reference_left, reference_right);

  CHECK_THAT(mono["sourceLufs"].as_number(),
             WithinAbs(sonare::metering::lufs_interleaved(source_left.data(), source_left.size(), 1,
                                                          kSampleRate)
                           .integrated_lufs,
                       kLoudnessTolerance));
  CHECK_THAT(stereo["sourceLufs"].as_number(),
             WithinAbs(stereo_lufs(source_left, source_right), kLoudnessTolerance));
  CHECK_THAT(stereo["referenceLufs"].as_number(),
             WithinAbs(stereo_lufs(reference_left, reference_right), kLoudnessTolerance));

  const float gain = sonare::db_to_linear(stereo["gainToMatchDb"].as_number());
  Samples scaled_left = source_left;
  Samples scaled_right = source_right;
  for (float& value : scaled_left) value *= gain;
  for (float& value : scaled_right) value *= gain;
  CHECK_THAT(stereo_lufs(scaled_left, scaled_right),
             WithinAbs(stereo["referenceLufs"].as_number(), kLoudnessTolerance));
}

TEST_CASE("Pair spectra retain the power of duplicate and antiphase channels", "[pair_stereo]") {
  const char* name =
      GENERATE("match.tonalBalance", "match.tonalBalanceLogBands", "match.matchEqCurve");
  const float polarity = GENERATE(1.0f, -1.0f);
  const Samples source_left = signal(440.0f, 0.12f);
  const Samples reference_left = signal(880.0f, 0.28f);
  Samples source_right = source_left;
  Samples reference_right = reference_left;
  for (float& sample : source_right) sample *= polarity;
  for (float& sample : reference_right) sample *= polarity;

  const auto mono = analyze_mono(name, source_left, reference_left);
  const auto stereo =
      analyze_stereo(name, source_left, source_right, reference_left, reference_right);
  if (mono.find("bands")) {
    const auto& mono_bands = mono["bands"].as_array();
    const auto& stereo_bands = stereo["bands"].as_array();
    REQUIRE_FALSE(mono_bands.empty());
    REQUIRE(stereo_bands.size() == mono_bands.size());
    for (std::size_t i = 0; i < mono_bands.size(); ++i) {
      CAPTURE(i);
      for (const char* key : {"lowHz", "highHz", "sourceDb", "referenceDb", "deviationDb"}) {
        CAPTURE(key);
        const auto* actual = stereo_bands[i].find(key);
        const auto* expected = mono_bands[i].find(key);
        REQUIRE((actual == nullptr) == (expected == nullptr));
        if (expected) check_field(*actual, *expected);
      }
    }
  } else {
    for (const char* key : {"frequencies", "gainDb"}) {
      CAPTURE(key);
      const auto& mono_values = mono[key].as_array();
      const auto& stereo_values = stereo[key].as_array();
      REQUIRE_FALSE(mono_values.empty());
      REQUIRE(stereo_values.size() == mono_values.size());
      for (std::size_t i = 0; i < mono_values.size(); ++i) {
        CAPTURE(i);
        check_field(stereo_values[i], mono_values[i]);
      }
    }
  }
}

TEST_CASE("Stereo reference spectrum averages power across channels", "[pair_stereo]") {
  using sonare::mastering::match::reference_spectrum;
  const Samples samples = signal(750.0f, 0.4f);
  const auto tone = sonare::Audio::from_buffer(samples.data(), samples.size(), kSampleRate);
  const auto silence = sonare::Audio::from_vector(Samples(samples.size()), kSampleRate);
  const auto mono = reference_spectrum(tone, {});
  const auto stereo = reference_spectrum(tone, silence);
  REQUIRE(stereo.db.size() == mono.db.size());
  const auto peak =
      static_cast<std::size_t>(std::max_element(mono.db.begin(), mono.db.end()) - mono.db.begin());
  CHECK_THAT(stereo.db[peak] - mono.db[peak], WithinAbs(sonare::power_to_db_scalar(0.5), 1.0e-4));
}

TEST_CASE("Pair delay follows the mid and survives antiphase cancellation", "[pair_stereo]") {
  const char* name = "match.estimateReferenceDelaySamples";
  constexpr std::size_t kFrames = 128;
  constexpr std::size_t kShift = 13;
  constexpr std::size_t kPeakFrame = 11;
  constexpr std::size_t kDecoyFrame = 60;
  const float polarity = GENERATE(1.0f, -1.0f, -0.999f);
  Samples source_left(kFrames), source_right(kFrames), source_mid(kFrames);
  Samples reference_left(kFrames + kShift), reference_right(kFrames + kShift);
  Samples reference_mid(kFrames + kShift);
  source_left[kPeakFrame] = 0.5f;
  source_left[26] = -0.25f;
  source_left[75] = 0.125f;
  for (std::size_t i = 0; i < kFrames; ++i) {
    source_right[i] = polarity * source_left[i];
  }
  if (polarity > 0.0f) {
    source_right.assign(kFrames, 0.0f);
    source_right[46] = 0.5f;
  }
  for (std::size_t i = 0; i < kFrames; ++i) {
    source_mid[i] = 0.5f * (source_left[i] + source_right[i]);
    reference_left[i + kShift] = polarity > 0.0f ? source_right[i] : source_left[i];
    reference_right[i + kShift] = polarity > 0.0f ? source_left[i] : source_right[i];
    reference_mid[i + kShift] = source_mid[i];
  }
  if (polarity < 0.0f) {
    // The louder common-mode transient cancels in the summed channel correlation.
    constexpr float kDecoyLevel = 0.9f;
    reference_left[kDecoyFrame] = kDecoyLevel;
    reference_right[kDecoyFrame] = kDecoyLevel;
    reference_mid[kDecoyFrame] = kDecoyLevel;
  }

  const auto reference_later =
      analyze_stereo(name, source_left, source_right, reference_left, reference_right);
  const auto reference_earlier =
      analyze_stereo(name, reference_left, reference_right, source_left, source_right);
  CHECK_THAT(reference_later["delaySamples"].as_number(), WithinAbs(kShift, kResultTolerance));
  CHECK_THAT(reference_earlier["delaySamples"].as_number(),
             WithinAbs(-static_cast<double>(kShift), kResultTolerance));

  const auto mono = analyze_mono(name, source_mid, reference_mid);
  if (polarity > 0.0f) {
    check_field(reference_later["delaySamples"], mono["delaySamples"]);
  } else if (polarity == -1.0f) {
    CHECK_THAT(mono["delaySamples"].as_number(), WithinAbs(0.0, kResultTolerance));
  } else {
    CHECK_THAT(mono["delaySamples"].as_number(),
               WithinAbs(kDecoyFrame - kPeakFrame, kResultTolerance));
  }
}

TEST_CASE("Pair analysis aligns both reference channels to the source rate", "[pair_stereo]") {
  constexpr int kReferenceRate = sonare::constants::kDefaultSampleRate;
  const auto resample = [](const Samples& samples) {
    const auto audio = sonare::resample(
        sonare::Audio::from_buffer(samples.data(), samples.size(), kReferenceRate), kSampleRate);
    return Samples(audio.data(), audio.data() + audio.size());
  };
  const Samples source_left = signal(440.0f, 0.12f);
  const Samples source_right = signal(800.0f, 0.28f);
  const Samples reference_left = signal(500.0f, 0.24f, kReferenceRate);
  const Samples reference_right = signal(1200.0f, 0.32f, kReferenceRate);

  const auto native = analyze_stereo("match.referenceLoudness", source_left, source_right,
                                     reference_left, reference_right, kReferenceRate);
  const auto resampled = analyze_stereo("match.referenceLoudness", source_left, source_right,
                                        resample(reference_left), resample(reference_right));
  for (const char* key : {"sourceLufs", "referenceLufs", "gainToMatchDb"}) {
    CAPTURE(key);
    check_field(native[key], resampled[key]);
  }
}
