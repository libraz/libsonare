#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <cmath>
#include <cstddef>
#include <vector>

#include "core/audio.h"
#include "mastering/maximizer/true_peak_limiter.h"
#include "metering/true_peak.h"
#include "util/constants.h"

namespace {

using sonare::constants::kPi;
using sonare::mastering::maximizer::TruePeakLimiter;
using sonare::mastering::maximizer::TruePeakLimiterConfig;

constexpr int kSampleRate = 24000;
constexpr int kSignalSamples = 18000;
// Reported latency of the default 4x polyphase configuration at this rate; the
// tail the offline runners flush through the limiter.
constexpr int kFlushSamples = 36;

// Sustained low tone plus periodic transients, driven hard enough that the
// limiter is working continuously and the decimation guard engages.
std::vector<float> program(float drive) {
  std::vector<float> samples(static_cast<std::size_t>(kSignalSamples + kFlushSamples), 0.0f);
  for (int i = 0; i < kSignalSamples; ++i) {
    const float t = static_cast<float>(i) / static_cast<float>(kSampleRate);
    float value = 0.18f * std::sin(2.0f * kPi * 110.0f * t);
    const int period = kSampleRate / 4;
    const int local = i % period;
    if (local < 96) {
      value += 0.75f * (1.0f - static_cast<float>(local) / 96.0f);
    }
    samples[static_cast<std::size_t>(i)] = value * drive;
  }
  return samples;
}

TruePeakLimiterConfig limiter_config() {
  TruePeakLimiterConfig config;
  config.ceiling_db = -0.3f;
  config.release_ms = 50.0f;
  config.oversample_factor = 4;
  return config;
}

// Runs one limiter instance over @p input, handing it @p block samples at a
// time (the final chunk is whatever is left).
std::vector<std::vector<float>> run_in_blocks(const std::vector<std::vector<float>>& input,
                                              int prepared_block_size,
                                              const std::vector<int>& block_sizes) {
  auto output = input;
  const int channels = static_cast<int>(output.size());
  TruePeakLimiter limiter(limiter_config());
  limiter.prepare(kSampleRate, prepared_block_size, channels);
  std::vector<float*> pointers(static_cast<std::size_t>(channels));
  int offset = 0;
  for (int count : block_sizes) {
    for (int ch = 0; ch < channels; ++ch) {
      pointers[static_cast<std::size_t>(ch)] = output[static_cast<std::size_t>(ch)].data() + offset;
    }
    limiter.process(pointers.data(), channels, count);
    offset += count;
  }
  return output;
}

std::vector<int> uniform_blocks(int total, int block) {
  std::vector<int> sizes;
  for (int offset = 0; offset < total; offset += block) {
    sizes.push_back(std::min(block, total - offset));
  }
  return sizes;
}

// Comparing the buffers with == would make Catch2 stringify every sample on a
// failure, which buries the result. Reduce to the two numbers that identify the
// disagreement instead.
struct Mismatch {
  double max_abs = 0.0;
  int first_index = -1;
};

Mismatch difference(const std::vector<std::vector<float>>& a,
                    const std::vector<std::vector<float>>& b) {
  Mismatch out;
  REQUIRE(a.size() == b.size());
  for (std::size_t ch = 0; ch < a.size(); ++ch) {
    REQUIRE(a[ch].size() == b[ch].size());
    for (std::size_t i = 0; i < a[ch].size(); ++i) {
      const double delta = std::abs(static_cast<double>(a[ch][i]) - static_cast<double>(b[ch][i]));
      if (delta > 0.0 && (out.first_index < 0 || static_cast<int>(i) < out.first_index)) {
        out.first_index = static_cast<int>(i);
      }
      out.max_abs = std::max(out.max_abs, delta);
    }
  }
  return out;
}

void require_identical(const std::vector<std::vector<float>>& actual,
                       const std::vector<std::vector<float>>& reference) {
  const Mismatch mismatch = difference(actual, reference);
  CAPTURE(mismatch.max_abs, mismatch.first_index);
  CHECK(mismatch.max_abs == 0.0);
}

}  // namespace

// A host's buffer size is an arbitrary choice: the same material rendered at
// 512 samples per block and in one offline block must come out identical. Every
// stage of TruePeakLimiter is sample-serial and carries its state across calls,
// so nothing here may be derived from a whole-block statistic. The decimation
// guard used to divide by the block's own maximum, which made the render level
// a function of the block size and made a streaming render disagree with an
// offline one.
TEST_CASE("TruePeakLimiter output does not depend on the caller's block size",
          "[mastering][maximizer][block_invariance]") {
  const float drive = GENERATE(1.0f, 2.0f, 5.0f, 15.0f, 60.0f);
  CAPTURE(drive);
  const auto mono_input = program(drive);
  const int total = static_cast<int>(mono_input.size());

  SECTION("mono") {
    const std::vector<std::vector<float>> input{mono_input};
    const auto reference = run_in_blocks(input, total, {total});

    const int block = GENERATE(256, 512, 1024, 4096, 16384);
    CAPTURE(block);
    require_identical(run_in_blocks(input, total, uniform_blocks(total, block)), reference);

    // The shape the offline runners used to produce: the whole input, then a
    // separate latency flush.
    require_identical(run_in_blocks(input, total, {kSignalSamples, kFlushSamples}), reference);
    // A ragged split that lands mid-transient.
    require_identical(run_in_blocks(input, total, {5000, 7001, total - 12001}), reference);
  }

  SECTION("stereo stays linked") {
    std::vector<float> right = mono_input;
    for (std::size_t i = 0; i < right.size(); ++i) {
      right[i] *= (i % 3 == 0) ? 0.5f : 1.0f;
    }
    const std::vector<std::vector<float>> input{mono_input, right};
    const auto reference = run_in_blocks(input, total, {total});

    const int block = GENERATE(512, 4096);
    CAPTURE(block);
    require_identical(run_in_blocks(input, total, uniform_blocks(total, block)), reference);
    require_identical(run_in_blocks(input, total, {kSignalSamples, kFlushSamples}), reference);
  }
}

// The per-sample decimation guard replaced a block-wide rescale that forced the
// block's measured true peak onto the ceiling exactly. Bounding each sample
// instead leaves a small inter-sample residue, so pin it: it must stay well
// inside the project's own true-peak tolerance and must not grow with drive.
TEST_CASE("TruePeakLimiter keeps the true peak at the ceiling within tolerance",
          "[mastering][maximizer][block_invariance]") {
  const float drive = GENERATE(1.0f, 2.0f, 5.0f, 15.0f, 60.0f);
  const int block = GENERATE(512, 18036);
  CAPTURE(drive, block);

  const auto input = program(drive);
  const int total = static_cast<int>(input.size());
  const auto limited = run_in_blocks({input}, total, uniform_blocks(total, block));

  const auto audio = sonare::Audio::from_buffer(limited[0].data(), limited[0].size(), kSampleRate);
  const float true_peak_db = sonare::metering::true_peak_db(audio, 8);
  CAPTURE(true_peak_db);
  // 0.1 dB: the measured residue is about +0.02 dB at an 8x meter (finer than
  // the 4x the limiter itself runs at) and is flat in drive.
  CHECK(true_peak_db <= limiter_config().ceiling_db + 0.1f);
}
