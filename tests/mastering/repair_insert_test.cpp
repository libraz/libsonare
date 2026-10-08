#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <cfloat>
#include <cmath>
#include <cstddef>
#include <memory>
#include <random>
#include <string>
#include <vector>

#include "core/audio.h"
#include "mastering/api/insert_factory.h"
#include "mastering/repair/decrackle.h"
#include "mastering/repair/decrackle_streaming.h"
#include "mastering/repair/dehum.h"
#include "mastering/repair/dehum_streaming.h"
#include "mastering/repair/denoise_classical.h"
#include "mastering/repair/denoise_streaming.h"
#include "mastering/repair/dereverb_classical.h"
#include "mastering/repair/dereverb_streaming.h"
#include "rt/processor_base.h"
#include "util/constants.h"
#include "util/exception.h"

using namespace sonare;
namespace repair = sonare::mastering::repair;

namespace {

constexpr int kSr = 48000;
constexpr int kBlock = 333;

/// A tone, a hum series and a little noise, with sparse crackle on top.
std::vector<float> material(std::size_t length, unsigned seed) {
  std::mt19937 rng(seed);
  std::normal_distribution<float> noise(0.0f, 0.01f);
  std::uniform_int_distribution<int> crackle(0, 199);
  std::vector<float> out(length);
  for (std::size_t i = 0; i < length; ++i) {
    const double t = static_cast<double>(i) / kSr;
    out[i] = static_cast<float>(0.25 * std::sin(constants::kTwoPiD * 440.0 * t) +
                                0.05 * std::sin(constants::kTwoPiD * 50.0 * t) +
                                0.03 * std::sin(constants::kTwoPiD * 100.0 * t)) +
             noise(rng);
    if (crackle(rng) == 0) out[i] += 0.6f;
  }
  return out;
}

/// Runs @p processor over @p channels in blocks of @p block, appending @p latency zeros so the
/// delayed output covers the whole input.
std::vector<std::vector<float>> run_insert(rt::ProcessorBase& processor,
                                           std::vector<std::vector<float>> channels,
                                           int block = kBlock) {
  const int count = static_cast<int>(channels.size());
  const int latency = processor.latency_samples();
  for (auto& channel : channels) channel.resize(channel.size() + static_cast<std::size_t>(latency));
  processor.prepare(kSr, block, count);
  std::vector<float*> pointers(channels.size());
  for (std::size_t start = 0; start < channels[0].size();
       start += static_cast<std::size_t>(block)) {
    const int n = static_cast<int>(
        std::min<std::size_t>(static_cast<std::size_t>(block), channels[0].size() - start));
    for (int c = 0; c < count; ++c)
      pointers[static_cast<std::size_t>(c)] = channels[c].data() + start;
    processor.process(pointers.data(), count, n);
  }
  return channels;
}

float peak(const std::vector<float>& samples) {
  float out = 0.0f;
  for (float s : samples) out = std::max(out, std::abs(s));
  return out;
}

/// Largest difference between @p input and @p offline: the control that a match is not a match
/// of two unprocessed signals.
float processed_by(const std::vector<float>& input, const Audio& offline) {
  float worst = 0.0f;
  for (std::size_t i = 0; i < input.size(); ++i)
    worst = std::max(worst, std::abs(input[i] - offline[i]));
  return worst;
}

/// Largest difference between the insert's output, read @p latency late, and @p offline over
/// [first, last).
float delayed_difference(const std::vector<float>& insert, const Audio& offline, int latency,
                         std::size_t first, std::size_t last) {
  float worst = 0.0f;
  for (std::size_t i = first; i < last; ++i) {
    worst = std::max(worst, std::abs(insert[i + static_cast<std::size_t>(latency)] - offline[i]));
  }
  return worst;
}

bool refuses(const std::string& name, const std::string& json) {
  try {
    (void)mastering::api::make_insert(name, json);
  } catch (const SonareException& e) {
    return e.code() == ErrorCode::InvalidParameter;
  }
  return false;
}

}  // namespace

TEST_CASE("decrackle insert reproduces the offline median one sample late",
          "[mastering][repair][repair-insert]") {
  const std::vector<float> input = material(kSr / 2, 1);
  const repair::DecrackleConfig config;
  const Audio offline =
      repair::decrackle(Audio::from_buffer(input.data(), input.size(), kSr), config);
  REQUIRE(processed_by(input, offline) > 0.1f);
  repair::StreamingDecrackle insert(config);
  REQUIRE(insert.latency_samples() == 1);
  const auto out = run_insert(insert, {input, input});
  // The offline pass leaves its last sample alone because nothing follows it; a stream never
  // learns which sample is the last, so that one sample is not compared.
  for (const auto& channel : out) {
    CHECK(delayed_difference(channel, offline, 1, 0, input.size() - 1) == 0.0f);
  }
}

TEST_CASE("dereverb insert reproduces the offline mask n_fft - 1 samples late",
          "[mastering][repair][repair-insert]") {
  std::vector<float> left = material(kSr / 2, 2);
  std::vector<float> right = material(kSr / 2, 3);
  repair::DereverbClassicalConfig config;
  config.t60_sec = 1.2f;
  const Audio left_audio = Audio::from_buffer(left.data(), left.size(), kSr);
  const Audio right_audio = Audio::from_buffer(right.data(), right.size(), kSr);
  const Audio* pair[2] = {&left_audio, &right_audio};
  std::vector<Audio> offline;
  repair::dereverb_classical_linked(pair, 2, &offline, config);

  REQUIRE(processed_by(left, offline[0]) > 0.01f);
  repair::StreamingDereverb insert(config);
  REQUIRE(insert.latency_samples() == config.n_fft - 1);
  const auto out = run_insert(insert, {left, right});
  // Both sides sum float spectra in a different order; the difference is rounding at the
  // signal's scale.
  const float tolerance = 64.0f * FLT_EPSILON * std::max(peak(left), peak(right));
  // The offline frames past the end see the zero padding a stream has not reached.
  const std::size_t compared = left.size() - static_cast<std::size_t>(config.n_fft);
  for (int c = 0; c < 2; ++c) {
    CAPTURE(c);
    CHECK(delayed_difference(out[static_cast<std::size_t>(c)], offline[static_cast<std::size_t>(c)],
                             insert.latency_samples(), 0, compared) <= tolerance);
  }
}

TEST_CASE("denoise insert reproduces the offline linked mask at its reported latency",
          "[mastering][repair][repair-insert]") {
  std::vector<float> left = material(kSr / 2, 6);
  std::vector<float> right = material(kSr / 2, 7);
  const Audio left_audio = Audio::from_buffer(left.data(), left.size(), kSr);
  const Audio right_audio = Audio::from_buffer(right.data(), right.size(), kSr);
  const Audio* pair[2] = {&left_audio, &right_audio};
  for (const repair::DenoiseNoiseEstimator estimator :
       {repair::DenoiseNoiseEstimator::Spp, repair::DenoiseNoiseEstimator::Mcra}) {
    CAPTURE(static_cast<int>(estimator));
    repair::DenoiseClassicalConfig config;
    config.noise_estimator = estimator;
    std::vector<Audio> offline;
    repair::denoise_classical_linked(pair, 2, &offline, config);
    REQUIRE(processed_by(left, offline[0]) > 0.01f);

    repair::StreamingDenoise insert(config);
    const auto out = run_insert(insert, {left, right});
    const float tolerance = 64.0f * FLT_EPSILON * std::max(peak(left), peak(right));
    // The offline frames past the end see the zero padding a stream has not reached.
    const std::size_t compared = left.size() - static_cast<std::size_t>(insert.latency_samples());
    for (int c = 0; c < 2; ++c) {
      CAPTURE(c);
      CHECK(delayed_difference(out[static_cast<std::size_t>(c)],
                               offline[static_cast<std::size_t>(c)], insert.latency_samples(), 0,
                               compared) <= tolerance);
    }
  }
}

TEST_CASE("dehum insert reproduces the offline notch and the adaptive pass",
          "[mastering][repair][repair-insert]") {
  const std::vector<float> input = material(kSr / 2, 4);
  const Audio audio = Audio::from_buffer(input.data(), input.size(), kSr);
  const float tolerance = 64.0f * FLT_EPSILON * peak(input);

  for (const bool adaptive : {false, true}) {
    for (const repair::DehumMode mode : {repair::DehumMode::Notch, repair::DehumMode::Subtract}) {
      if (!adaptive && mode == repair::DehumMode::Subtract) continue;
      CAPTURE(adaptive, static_cast<int>(mode));
      repair::DehumConfig config;
      config.adaptive = adaptive;
      config.mode = mode;
      const Audio offline = repair::dehum(audio, config);
      REQUIRE(processed_by(input, offline) > 0.01f);
      repair::StreamingDehum insert(config);
      REQUIRE(insert.latency_samples() == (adaptive ? config.frame_size : 0));
      const auto out = run_insert(insert, {input});
      // The adaptive pass filters a short last block the stream has not completed.
      const std::size_t compared =
          input.size() -
          (adaptive ? input.size() % static_cast<std::size_t>(config.frame_size) : 0);
      CHECK(delayed_difference(out[0], offline, insert.latency_samples(), 0, compared) <=
            tolerance);
    }
  }
}

TEST_CASE("dehum insert subtract converges onto the seeded offline pass",
          "[mastering][repair][repair-insert]") {
  const std::vector<float> input = material(2 * kSr, 5);
  const Audio audio = Audio::from_buffer(input.data(), input.size(), kSr);
  repair::DehumConfig config;
  config.mode = repair::DehumMode::Subtract;
  const Audio offline = repair::dehum(audio, config);
  repair::StreamingDehum insert(config);
  const auto out = run_insert(insert, {input});
  // The cancellers start at zero rather than at the offline seed; by the second second the
  // difference is a small fraction of the hum they remove.
  CHECK(delayed_difference(out[0], offline, 0, kSr, input.size()) < 0.005f);
  CHECK(delayed_difference(out[0], offline, 0, 0, kSr / 10) > 0.01f);
}

TEST_CASE("repair inserts refuse an offline-only setting by name",
          "[mastering][repair][repair-insert]") {
  // Enums travel as their wire value; 1 is the wavelet mode.
  CHECK(refuses("repair.decrackle", R"({"mode":1})"));
  CHECK(refuses("repair.dereverbClassical", R"({"wpeEnabled":true})"));
  // 0 is the quantile estimator, which ranks a whole signal.
  CHECK(refuses("repair.denoiseClassical", R"({"noiseEstimator":0})"));
  CHECK_FALSE(refuses("repair.denoiseClassical", "{}"));
  CHECK_FALSE(refuses("repair.decrackle", R"({"threshold":0.3})"));
  CHECK_FALSE(refuses("repair.dehum", R"({"adaptive":true})"));
  CHECK(mastering::api::insert_timing("repair.dehum", R"({"adaptive":true,"frameSize":1024})", kSr)
            .latency_samples == 1024);
  CHECK(mastering::api::insert_timing("repair.dereverbClassical", R"({"nFft":512})", kSr)
            .latency_samples == 511);
}
