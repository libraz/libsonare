#include <algorithm>
#include <array>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <cmath>
#include <complex>
#include <cstddef>
#include <functional>
#include <random>
#include <utility>
#include <vector>

#include "core/spectrum.h"
#include "mastering/common/noise_tracker.h"
#include "mastering/repair/declick.h"
#include "mastering/repair/declip.h"
#include "mastering/repair/decrackle.h"
#include "mastering/repair/dehum.h"
#include "mastering/repair/denoise_classical.h"
#include "mastering/repair/denoise_internal.h"
#include "mastering/repair/dereverb_classical.h"
#include "mastering/repair/trim_silence.h"
#include "support/audio_fixtures.h"
#include "util/constants.h"

using Catch::Matchers::WithinAbs;
using namespace sonare;
using namespace sonare::mastering::repair;

namespace {
using sonare::test::rms;

Audio make_audio(const std::vector<float>& samples) {
  return Audio::from_buffer(samples.data(), samples.size(), 48000);
}

}  // namespace

TEST_CASE("TrimSilence removes leading and trailing quiet samples", "[mastering][repair]") {
  const auto result = trim_silence(make_audio({0.0f, 0.001f, 0.2f, -0.1f, 0.0f}), {0.01f, 0});

  REQUIRE(result.size() == 2);
  REQUIRE_THAT(result[0], WithinAbs(0.2f, 0.001f));
}

TEST_CASE("TrimSilence supports LUFS-gated trimming", "[mastering][repair]") {
  std::vector<float> samples(100, 0.005f);
  samples.insert(samples.end(), 20, 0.1f);
  samples.insert(samples.end(), 100, 0.005f);

  const auto result =
      trim_silence(make_audio(samples), {0.001f, 0, TrimSilenceMode::LufsGated, -35.0f, 0.1f});

  REQUIRE(result.size() < samples.size());
  REQUIRE(result.size() >= 20);
  REQUIRE(rms(result) > 0.05f);
}

TEST_CASE("TrimSilence shares enum and range validation across direct entrypoints",
          "[mastering][repair]") {
  const std::vector<float> samples = {0.0f, 0.2f, 0.0f};
  TrimSilenceConfig invalid_mode;
  invalid_mode.mode = static_cast<TrimSilenceMode>(2);
  REQUIRE_THROWS(trim_silence(make_audio(samples), invalid_mode));
  REQUIRE_THROWS(detect_trim_range(samples.data(), samples.size(), 48000, invalid_mode));

  TrimSilenceConfig invalid_window;
  invalid_window.window_ms = -1.0f;
  REQUIRE_THROWS(trim_silence(make_audio(samples), invalid_window));
  REQUIRE_THROWS(detect_trim_range(samples.data(), samples.size(), 48000, invalid_window));
}

TEST_CASE("Declick interpolates isolated spikes", "[mastering][repair]") {
  const auto result = declick(make_audio({0.1f, 1.0f, 0.1f}), {0.8f, 4.0f});

  REQUIRE_THAT(result[1], WithinAbs(0.1f, 0.001f));
}

TEST_CASE("Declick interpolates short click clusters", "[mastering][repair]") {
  const auto result = declick(make_audio({0.1f, 1.0f, 1.0f, 0.2f}), {0.8f, 4.0f, 4});

  REQUIRE_THAT(result[1], WithinAbs(0.133333f, 0.001f));
  REQUIRE_THAT(result[2], WithinAbs(0.166667f, 0.001f));
}

TEST_CASE("Declick detects sub-threshold impulses with LPC residuals", "[mastering][repair]") {
  std::vector<float> samples(128, 0.0f);
  for (size_t i = 1; i < samples.size(); ++i) {
    samples[i] = 0.92f * samples[i - 1] + (i == 1 ? 0.1f : 0.0f);
  }
  samples[64] = 0.7f;

  const auto result = declick(Audio::from_vector(samples, 48000), {0.8f, 4.0f, 4, 12, 6.0f});

  REQUIRE(std::abs(result[64]) < 0.3f);
  REQUIRE(std::abs(result[63] - samples[63]) < 0.0001f);
  REQUIRE(std::abs(result[65] - samples[65]) < 0.0001f);
}

TEST_CASE("Decrackle median-filters small impulses", "[mastering][repair]") {
  const auto result = decrackle(make_audio({0.1f, 0.8f, 0.12f}), {0.2f});

  REQUIRE_THAT(result[1], WithinAbs(0.12f, 0.001f));
}

TEST_CASE("Decrackle wavelet shrinkage reduces crackle energy", "[mastering][repair]") {
  std::vector<float> samples(256, 0.0f);
  for (size_t i = 0; i < samples.size(); ++i) {
    samples[i] = 0.1f * static_cast<float>(std::sin(sonare::constants::kTwoPiD * i / 64.0));
  }
  samples[32] += 0.35f;
  samples[96] -= 0.32f;
  samples[160] += 0.30f;
  const auto input = make_audio(samples);

  const auto result = decrackle(input, {0.08f, DecrackleMode::WaveletShrinkage, 4});

  REQUIRE(rms(result) < rms(input));
  REQUIRE(std::abs(result[32]) < std::abs(input[32]));
}

TEST_CASE("Clip detection survives the gain change that hides clipping from the threshold",
          "[mastering][repair]") {
  // The threshold fields answer "what is at the ceiling now", which is the wrong
  // question for a file clipped in one tool and turned down in the next: the
  // flat tops are all that is left of the clipping, and they are still there.
  constexpr int kRate = 48000;
  constexpr size_t kLength = 48000;
  std::vector<float> clipped(kLength);
  for (size_t i = 0; i < kLength; ++i) {
    const float t = static_cast<float>(i) / static_cast<float>(kRate);
    clipped[i] = std::clamp(1.8f * std::sin(constants::kTwoPi * 220.0f * t), -1.0f, 1.0f);
  }

  const auto full_scale = detect_clipping(clipped.data(), clipped.size(), kRate);
  REQUIRE(full_scale.sample_count > 0);
  REQUIRE(full_scale.flat_run_count > 0);

  std::vector<float> attenuated(clipped.size());
  std::transform(clipped.begin(), clipped.end(), attenuated.begin(),
                 [](float value) { return value * 0.25f; });
  const auto quiet = detect_clipping(attenuated.data(), attenuated.size(), kRate);

  CAPTURE(quiet.sample_count, quiet.flat_run_count, quiet.flat_level);
  REQUIRE(quiet.sample_count == 0);
  REQUIRE(quiet.run_count == 0);
  REQUIRE(quiet.flat_run_count == full_scale.flat_run_count);
  REQUIRE(quiet.flat_sample_count == full_scale.flat_sample_count);
  REQUIRE(quiet.longest_flat_run_samples == full_scale.longest_flat_run_samples);
  REQUIRE_THAT(quiet.flat_level, WithinAbs(0.25f, 1e-6f));
}

TEST_CASE("Clip detection does not read an unclipped peak as a flat top", "[mastering][repair]") {
  // A full-scale sine reaches the default threshold every cycle without ever
  // having been clipped, which is the false positive the counts alone produce.
  // The apex of a sine never repeats a sample, so no run forms.
  constexpr int kRate = 48000;
  std::vector<float> sine(48000);
  for (size_t i = 0; i < sine.size(); ++i) {
    const float t = static_cast<float>(i) / static_cast<float>(kRate);
    sine[i] = std::sin(constants::kTwoPi * 220.0f * t);
  }

  const auto detected = detect_clipping(sine.data(), sine.size(), kRate);

  CAPTURE(detected.sample_count, detected.flat_run_count);
  REQUIRE(detected.sample_count > 0);
  REQUIRE(detected.flat_run_count == 0);
  REQUIRE(detected.flat_sample_count == 0);
  REQUIRE(detected.longest_flat_run_samples == 0);
  REQUIRE(detected.flat_level == 0.0f);
}

TEST_CASE("Declip reconstructs clipped samples from neighbors", "[mastering][repair]") {
  const auto result = declip(make_audio({0.2f, 1.0f, 0.4f}), {0.98f});

  REQUIRE_THAT(result[1], WithinAbs(0.3f, 0.001f));
}

TEST_CASE("Declip reconstructs clipped runs as a continuous segment", "[mastering][repair]") {
  const auto result = declip(make_audio({0.4f, 1.0f, 1.0f, 0.7f}), {0.98f});

  REQUIRE_THAT(result[1], WithinAbs(0.5f, 0.001f));
  REQUIRE_THAT(result[2], WithinAbs(0.6f, 0.001f));
}

TEST_CASE("Declip uses cubic context for smoother clipped run reconstruction",
          "[mastering][repair]") {
  const auto result = declip(make_audio({0.1f, 0.2f, 1.0f, 1.0f, 0.7f, 0.9f}), {0.98f});

  REQUIRE(result[2] > 0.34f);
  REQUIRE(result[2] < 0.45f);
  REQUIRE(result[3] > 0.52f);
  REQUIRE(result[3] < 0.62f);
  REQUIRE(result[2] < result[3]);
}

TEST_CASE("Declip allows reconstructed samples beyond clip threshold", "[mastering][repair]") {
  const auto result = declip(make_audio({0.0f, 0.49f, 1.0f, 0.49f, 0.0f}), {0.5f});

  REQUIRE(result[2] > 0.5f);
  REQUIRE_THAT(result[2], WithinAbs(0.55125f, 0.0001f));
}

TEST_CASE("Declip uses LPC reconstruction when enough context is available",
          "[mastering][repair]") {
  std::vector<float> samples(96, 0.0f);
  samples[0] = 1.0f;
  for (size_t i = 1; i < samples.size(); ++i) samples[i] = 0.95f * samples[i - 1];
  samples[20] = 1.0f;
  samples[21] = 1.0f;
  samples[22] = 1.0f;

  const auto result = declip(Audio::from_vector(samples, 48000), {0.9f, 12, 2});

  REQUIRE(std::isfinite(result[20]));
  REQUIRE(std::isfinite(result[21]));
  REQUIRE(std::isfinite(result[22]));
  REQUIRE(result[20] > result[21]);
  REQUIRE(result[21] > result[22]);
  REQUIRE(result[20] < 0.9f);
}

TEST_CASE("Declip lpc_blend interpolates between LPC and fallback", "[mastering][repair]") {
  // Long enough decaying signal with a clipped run so the LPC refinement path
  // actually runs (n >= 32, order >= 4) and produces an estimate that differs
  // from the cubic / linear interpolation baseline. The decaying baseline starts
  // BELOW the 0.9 clip threshold so only the inserted run [20,22] is detected as
  // clipped (a leading near-1.0 baseline would itself be flagged/reconstructed).
  std::vector<float> samples(96, 0.0f);
  samples[0] = 0.8f;
  for (size_t i = 1; i < samples.size(); ++i) samples[i] = 0.95f * samples[i - 1];
  samples[20] = 1.0f;
  samples[21] = 1.0f;
  samples[22] = 1.0f;
  const auto audio = Audio::from_vector(samples, 48000);

  const auto pure_lpc = declip(audio, {0.9f, 12, 2, 1.0f});
  const auto pure_interp = declip(audio, {0.9f, 12, 2, 0.0f});

  for (size_t i = 20; i <= 22; ++i) {
    REQUIRE(std::isfinite(pure_interp[i]));
    REQUIRE(std::isfinite(pure_lpc[i]));
  }

  // The two extremes must differ at the reconstructed samples: if lpc_blend was
  // ignored the outputs would be bit-identical (the pre-fix bug).
  bool extremes_differ = false;
  for (size_t i = 20; i <= 22; ++i) {
    if (std::abs(pure_lpc[i] - pure_interp[i]) > 1e-5f) extremes_differ = true;
  }
  REQUIRE(extremes_differ);

  // Samples outside the clipped run must be untouched by any blend setting.
  for (size_t i = 0; i < samples.size(); ++i) {
    if (i < 20 || i > 22) {
      REQUIRE(pure_lpc[i] == pure_interp[i]);
    }
  }
}

TEST_CASE("Declip lpc_blend=1.0 reproduces unblended LPC behaviour", "[mastering][repair]") {
  // Guard that blend == 1.0 is a true no-op relative to the historical full-LPC
  // overwrite: blend 1.0 writes 1.0 * x_u + 0.0 * baseline == x_u, so it must
  // match the existing "uses LPC reconstruction" expectations exactly.
  std::vector<float> samples(96, 0.0f);
  samples[0] = 1.0f;
  for (size_t i = 1; i < samples.size(); ++i) samples[i] = 0.95f * samples[i - 1];
  samples[20] = 1.0f;
  samples[21] = 1.0f;
  samples[22] = 1.0f;

  const auto result = declip(Audio::from_vector(samples, 48000), {0.9f, 12, 2, 1.0f});

  REQUIRE(result[20] > result[21]);
  REQUIRE(result[21] > result[22]);
  REQUIRE(result[20] < 0.9f);
}

namespace {

// Generate a sine wave + low-amplitude noise, then hard-clip at +/- threshold.
// Returns the original (unclipped) and clipped signals separately so tests can
// measure SDR improvements without re-deriving the reference.
struct ClippedFixture {
  std::vector<float> original;
  std::vector<float> clipped;
};

ClippedFixture make_clipped_sine(size_t n, float freq_hz, float amp, float sample_rate,
                                 float clip_thresh, uint32_t seed) {
  ClippedFixture fx;
  fx.original.resize(n);
  fx.clipped.resize(n);
  std::mt19937 rng(seed);
  std::uniform_real_distribution<float> noise(-0.005f, 0.005f);
  for (size_t i = 0; i < n; ++i) {
    const float t = static_cast<float>(i) / sample_rate;
    const float x =
        amp * static_cast<float>(std::sin(sonare::constants::kTwoPiD * freq_hz * t)) + noise(rng);
    fx.original[i] = x;
    fx.clipped[i] = std::clamp(x, -clip_thresh, clip_thresh);
  }
  return fx;
}

}  // namespace

TEST_CASE("Declip preserves all unclipped samples exactly", "[mastering][repair]") {
  // Regression test for the bug where Burg was retrained on filled-in samples:
  // even after the LPC step, samples outside any clipped region must remain
  // bit-identical to the input.
  const auto fx = make_clipped_sine(2048, 700.0f, 0.92f, 48000.0f, 0.5f, 0xFACE);
  const auto clipped_audio = Audio::from_buffer(fx.clipped.data(), fx.clipped.size(), 48000);
  const auto result = declip(clipped_audio, {0.5f, 24, 2, 1.0f});

  REQUIRE(result.size() == fx.clipped.size());
  for (size_t i = 0; i < fx.clipped.size(); ++i) {
    if (std::abs(fx.clipped[i]) < 0.5f) {
      REQUIRE(result[i] == fx.clipped[i]);
    }
  }
}

namespace {

// The gap and context caps are the whole bound: the solver's dense working set is
// a function of them alone, so a cap raise that would put a mastering pass back
// into gigabyte territory has to trip here first.
static_assert(kDeclipMaxLpcWorkingSetBytes < 32u * 1024u * 1024u,
              "declip LPC solver working set must stay inside a mastering-pass memory budget");

/// @brief Longest run of samples at or above @p threshold.
size_t longest_clipped_run(const std::vector<float>& samples, float threshold) {
  size_t longest = 0;
  size_t current = 0;
  for (const float value : samples) {
    current = std::abs(value) >= threshold ? current + 1 : 0;
    longest = std::max(longest, current);
  }
  return longest;
}

}  // namespace

TEST_CASE("Declip bounds a full-scale run instead of scaling with its length",
          "[mastering][repair]") {
  // A sustained full-scale passage is a single clipped run with no length limit of
  // its own, so sizing the solver from the gap asked for ~46 GB for one second of
  // 48 kHz material. Runs past the cap must take the interpolation fallback: no
  // solver matrix at all, and a finite, smooth, no-longer-clipped result.
  constexpr int kSampleRate = 48000;
  constexpr size_t kFrames = static_cast<size_t>(kSampleRate) * 3;
  constexpr size_t kRunStart = static_cast<size_t>(kSampleRate);
  constexpr size_t kRunLength = static_cast<size_t>(kSampleRate);  // one full second
  static_assert(kRunLength > kDeclipMaxLpcGapSamples, "run must exceed the LPC gap cap");

  std::vector<float> samples(kFrames);
  for (size_t i = 0; i < kFrames; ++i) {
    samples[i] = 0.5f * static_cast<float>(
                            std::sin(sonare::constants::kTwoPiD * 220.0 * static_cast<double>(i) /
                                     static_cast<double>(kSampleRate)));
  }
  std::fill(samples.begin() + static_cast<std::ptrdiff_t>(kRunStart),
            samples.begin() + static_cast<std::ptrdiff_t>(kRunStart + kRunLength), 1.0f);

  const auto result =
      declip(Audio::from_buffer(samples.data(), samples.size(), kSampleRate), DeclipConfig{});

  REQUIRE(result.size() == kFrames);
  for (size_t i = 0; i < kFrames; ++i) {
    REQUIRE(std::isfinite(result[i]));
  }

  // The run is no longer at full scale, and it joins its neighbours without a
  // step: an interpolation spanning the run has a per-sample slope on the order of
  // one over its length.
  for (size_t i = kRunStart; i < kRunStart + kRunLength; ++i) {
    REQUIRE(std::abs(result[i]) < 1.0f);
    REQUIRE(std::abs(result[i] - result[i - 1]) < 0.01f);
  }

  // Everything outside the run is untouched.
  for (size_t i = 0; i < kRunStart; ++i) {
    REQUIRE(result[i] == samples[i]);
  }
  for (size_t i = kRunStart + kRunLength; i < kFrames; ++i) {
    REQUIRE(result[i] == samples[i]);
  }
}

TEST_CASE("Declip short-gap repair does not depend on the input length", "[mastering][repair]") {
  // The context window is capped independently of the input, so a short clipped
  // run's reconstruction is a function of its bounded neighbourhood only. Repairing
  // the same prefix inside a four-times-longer buffer must therefore reproduce it
  // bit for bit -- the property that makes the working set input-length independent.
  constexpr size_t kShort = 4096;
  constexpr size_t kLong = 16384;
  constexpr float kThreshold = 0.5f;
  const DeclipConfig config{kThreshold, 24, 2, 1.0f};

  const auto fx_short = make_clipped_sine(kShort, 700.0f, 0.92f, 48000.0f, kThreshold, 0xC0FFEE);
  const auto fx_long = make_clipped_sine(kLong, 700.0f, 0.92f, 48000.0f, kThreshold, 0xC0FFEE);
  // Same generator and seed, so the long buffer starts with the short one.
  for (size_t i = 0; i < kShort; ++i) {
    REQUIRE(fx_long.clipped[i] == fx_short.clipped[i]);
  }
  // These runs go through the solver, not the fallback: this compares the LPC path.
  // The upper bound also pins the context window (at most 8 * run) well inside the
  // margin kCompareEnd leaves at the end of the short buffer.
  const size_t longest_run = longest_clipped_run(fx_short.clipped, kThreshold);
  REQUIRE(longest_run > 0);
  REQUIRE(longest_run <= 64);
  REQUIRE(longest_run < kDeclipMaxLpcGapSamples);

  const auto repaired_short =
      declip(Audio::from_buffer(fx_short.clipped.data(), kShort, 48000), config);
  const auto repaired_long =
      declip(Audio::from_buffer(fx_long.clipped.data(), kLong, 48000), config);

  // Compare only where the capped context window fits inside the short buffer; past
  // that the short buffer's context is truncated by its own end.
  constexpr size_t kCompareEnd = kShort - 1024;
  bool repaired_anything = false;
  for (size_t i = 0; i < kCompareEnd; ++i) {
    REQUIRE(std::isfinite(repaired_short[i]));
    REQUIRE(repaired_short[i] == repaired_long[i]);
    if (std::abs(fx_short.clipped[i]) >= kThreshold && repaired_short[i] != fx_short.clipped[i]) {
      repaired_anything = true;
    }
  }
  REQUIRE(repaired_anything);
}

TEST_CASE("Declip routes over-cap runs to the interpolation fallback", "[mastering][repair]") {
  // The fallback has no LPC estimate to blend, so it ignores lpc_blend entirely: a
  // run one sample past the cap must produce bit-identical output at both blend
  // extremes, where a solver-reconstructed run does not (see the lpc_blend cases).
  constexpr int kSampleRate = 48000;
  constexpr size_t kFrames = 4096;
  constexpr size_t kRunStart = 1024;
  constexpr size_t kRunLength = kDeclipMaxLpcGapSamples + 1;

  std::vector<float> samples(kFrames);
  for (size_t i = 0; i < kFrames; ++i) {
    samples[i] = 0.4f * static_cast<float>(
                            std::sin(sonare::constants::kTwoPiD * 440.0 * static_cast<double>(i) /
                                     static_cast<double>(kSampleRate)));
  }
  std::fill(samples.begin() + static_cast<std::ptrdiff_t>(kRunStart),
            samples.begin() + static_cast<std::ptrdiff_t>(kRunStart + kRunLength), 1.0f);
  const auto audio = Audio::from_buffer(samples.data(), samples.size(), kSampleRate);

  const auto pure_interp = declip(audio, {0.98f, 36, 2, 0.0f});
  const auto pure_lpc = declip(audio, {0.98f, 36, 2, 1.0f});

  for (size_t i = 0; i < kFrames; ++i) {
    REQUIRE(pure_lpc[i] == pure_interp[i]);
  }
}

TEST_CASE("Declip and Declick repair a three-minute recording with many short defects",
          "[.][slow][mastering][repair]") {
  constexpr int kSampleRate = 48000;
  constexpr size_t kFrames = static_cast<size_t>(3 * 60 * kSampleRate);
  constexpr size_t kDefectCount = 1000;
  std::vector<float> damaged(kFrames);
  for (size_t i = 0; i < damaged.size(); ++i) {
    damaged[i] = 0.2f * static_cast<float>(
                            std::sin(sonare::constants::kTwoPiD * 440.0 * static_cast<double>(i) /
                                     static_cast<double>(kSampleRate)));
  }

  std::vector<size_t> defects;
  defects.reserve(kDefectCount);
  const size_t stride = (kFrames - 2) / kDefectCount;
  for (size_t i = 0; i < kDefectCount; ++i) {
    const size_t index = 1 + i * stride;
    damaged[index] = 1.0f;
    defects.push_back(index);
  }

  const Audio input = Audio::from_buffer(damaged.data(), damaged.size(), kSampleRate);
  const Audio declipped = declip(input, {0.9f, 8, 2, 1.0f});
  const Audio declicked = declick(input, {0.8f, 2.0f, 1, 8, 8.0f});

  REQUIRE(declipped.size() == damaged.size());
  REQUIRE(declicked.size() == damaged.size());
  for (const size_t index : defects) {
    REQUIRE(std::isfinite(declipped[index]));
    REQUIRE(std::isfinite(declicked[index]));
    CHECK(std::abs(declipped[index]) < 0.8f);
    CHECK(std::abs(declicked[index]) < 0.8f);
  }
}

TEST_CASE("Dehum notch filter reduces fundamental tone", "[mastering][repair]") {
  std::vector<float> samples(4800);
  for (size_t i = 0; i < samples.size(); ++i) {
    samples[i] = 0.5f * static_cast<float>(std::sin(sonare::constants::kTwoPiD * 50.0 *
                                                    static_cast<double>(i) / 48000.0));
  }
  const auto input = make_audio(samples);
  const auto result = dehum(input, {50.0f, 1, 10.0f});

  REQUIRE(rms(result) < rms(input));
}

TEST_CASE("Dehum adaptive notch follows drifting fundamental", "[mastering][repair]") {
  std::vector<float> samples(8192);
  for (size_t i = 0; i < samples.size(); ++i) {
    const float hz = 49.0f + 2.0f * static_cast<float>(i) / static_cast<float>(samples.size());
    samples[i] = 0.35f * static_cast<float>(std::sin(sonare::constants::kTwoPiD * hz *
                                                     static_cast<double>(i) / 48000.0));
  }
  const auto input = make_audio(samples);
  const auto result = dehum(input, {50.0f, 1, 6.0f, true, 2.5f, 0.8f, 1024});

  REQUIRE(rms(result) < rms(input) * 0.8f);
}

TEST_CASE("Dehum adaptive tracking recovers after out-of-band content", "[mastering][repair]") {
  // First half: programme content well above the search window and no hum, so a
  // search re-centred on its own previous estimate climbs toward that content
  // one search range per frame. Second half: the configured hum appears. The
  // walk is irreversible -- once the target is outside the window the next
  // window is centred on it, never on the fundamental -- so the drifted target
  // pins the PLL against its clamp and the hum is never notched. A search
  // anchored on fundamental_hz cannot leave, so the hum is removed.
  // The programme tone sits close enough above the window that a search
  // re-centred on its own previous estimate reaches it within the first half,
  // and far enough that a window anchored on the fundamental never does. A tone
  // several octaves up leaves the drifted target still inside the window, which
  // is why a 400 Hz fixture here produced identical output either way and
  // asserted nothing.
  constexpr int kSampleRate = 48000;
  constexpr double kHumHz = 50.0;
  constexpr double kProgrammeHz = 100.0;
  std::vector<float> samples(48000);
  for (size_t i = 0; i < samples.size(); ++i) {
    const double t = static_cast<double>(i) / kSampleRate;
    const bool second_half = i >= samples.size() / 2;
    samples[i] = 0.6f * static_cast<float>(std::sin(sonare::constants::kTwoPiD * kProgrammeHz * t));
    if (second_half) {
      samples[i] += 0.4f * static_cast<float>(std::sin(sonare::constants::kTwoPiD * kHumHz * t));
    }
  }
  const auto input = make_audio(samples);
  const auto result = dehum(input, {50.0f, 1, 6.0f, true, 10.0f, 1.0f, 1024});

  const auto energy_at = [](const float* data, size_t count, double hz) {
    const double w = sonare::constants::kTwoPiD * hz / kSampleRate;
    const double coeff = 2.0 * std::cos(w);
    double s1 = 0.0;
    double s2 = 0.0;
    for (size_t i = 0; i < count; ++i) {
      const double s0 = static_cast<double>(data[i]) + coeff * s1 - s2;
      s2 = s1;
      s1 = s0;
    }
    const double real = s1 - s2 * std::cos(w);
    const double imag = s2 * std::sin(w);
    return real * real + imag * imag;
  };

  const size_t half = samples.size() / 2;
  const double hum_in = energy_at(input.data() + half, half, kHumHz);
  const double hum_out = energy_at(result.data() + half, half, kHumHz);
  REQUIRE(hum_out < 0.5 * hum_in);
}

namespace {
Audio noisy_tone(int sample_rate, int samples, float tone_freq, float tone_amp, float noise_amp,
                 uint32_t seed) {
  std::vector<float> data(static_cast<size_t>(samples));
  std::mt19937 rng(seed);
  std::uniform_real_distribution<float> noise(-noise_amp, noise_amp);
  for (int i = 0; i < samples; ++i) {
    const double t = static_cast<double>(i) / sample_rate;
    data[static_cast<size_t>(i)] =
        static_cast<float>(tone_amp * std::sin(sonare::constants::kTwoPiD * tone_freq * t)) +
        noise(rng);
  }
  return Audio::from_vector(std::move(data), sample_rate);
}

float high_frequency_residual_rms(const Audio& a) {
  // First-difference RMS is dominated by broadband noise content; it is a
  // proxy for "how noisy this signal sounds" that is largely insensitive to
  // the low-frequency tone.
  double sum = 0.0;
  for (size_t i = 1; i < a.size(); ++i) {
    const float diff = a[i] - a[i - 1];
    sum += static_cast<double>(diff) * diff;
  }
  return static_cast<float>(std::sqrt(sum / static_cast<double>(a.size())));
}
}  // namespace

TEST_CASE("DenoiseClassical LogMmse reduces broadband noise", "[mastering][repair]") {
  const Audio input = noisy_tone(48000, 48000, 1000.0f, 0.5f, 0.05f, 12345);
  DenoiseClassicalConfig config{};
  config.mode = DenoiseMode::LogMmse;
  const Audio output = denoise_classical(input, config);

  REQUIRE(output.size() == input.size());
  REQUIRE(high_frequency_residual_rms(output) < high_frequency_residual_rms(input) * 0.85f);
}

TEST_CASE("DenoiseClassical MmseStsa reduces broadband noise", "[mastering][repair]") {
  const Audio input = noisy_tone(48000, 48000, 1000.0f, 0.5f, 0.05f, 23456);
  DenoiseClassicalConfig config{};
  config.mode = DenoiseMode::MmseStsa;
  const Audio output = denoise_classical(input, config);

  REQUIRE(high_frequency_residual_rms(output) < high_frequency_residual_rms(input) * 0.85f);
}

TEST_CASE("DenoiseClassical SpectralSubtraction reduces broadband noise", "[mastering][repair]") {
  const Audio input = noisy_tone(48000, 48000, 1000.0f, 0.5f, 0.05f, 34567);
  DenoiseClassicalConfig config{};
  config.mode = DenoiseMode::SpectralSubtraction;
  const Audio output = denoise_classical(input, config);

  REQUIRE(high_frequency_residual_rms(output) < high_frequency_residual_rms(input));
}

TEST_CASE("DenoiseClassical can use IMCRA frame-adaptive noise tracking", "[mastering][repair]") {
  const Audio input = noisy_tone(48000, 48000, 1000.0f, 0.45f, 0.04f, 45678);
  DenoiseClassicalConfig config{};
  config.mode = DenoiseMode::LogMmse;
  config.noise_estimator = DenoiseNoiseEstimator::Imcra;
  const Audio output = denoise_classical(input, config);

  REQUIRE(output.size() == input.size());
  REQUIRE(high_frequency_residual_rms(output) < high_frequency_residual_rms(input) * 0.9f);

  config.noise_estimator = DenoiseNoiseEstimator::Mcra;
  const Audio mcra_output = denoise_classical(input, config);
  REQUIRE(mcra_output.size() == input.size());
}

TEST_CASE("DenoiseClassical SPP estimator is its own path, not a fallback", "[mastering][repair]") {
  // The estimator used to be selected by an `if (Mcra)` over an Imcra-initialized
  // variable, so any other value ran as Imcra under its own name. Checking that
  // the enum is accepted cannot see that; only the samples can.
  const Audio input = noisy_tone(48000, 48000, 1000.0f, 0.45f, 0.04f, 45678);
  DenoiseClassicalConfig config{};
  config.mode = DenoiseMode::LogMmse;

  auto run = [&](DenoiseNoiseEstimator estimator) {
    config.noise_estimator = estimator;
    return denoise_classical(input, config);
  };
  auto max_abs_diff = [](const Audio& a, const Audio& b) {
    REQUIRE(a.size() == b.size());
    float worst = 0.0f;
    for (size_t i = 0; i < a.size(); ++i) worst = std::max(worst, std::abs(a[i] - b[i]));
    return worst;
  };

  const Audio spp = run(DenoiseNoiseEstimator::Spp);
  REQUIRE(spp.size() == input.size());
  for (float sample : spp) REQUIRE(std::isfinite(sample));
  REQUIRE(high_frequency_residual_rms(spp) < high_frequency_residual_rms(input) * 0.9f);

  // Measured max |difference| on a signal peaking near 0.45: 0.045 against Imcra,
  // 0.045 against Mcra, 0.059 against Quantile. Restoring the Imcra fallback drops
  // the first to 0.
  REQUIRE(max_abs_diff(spp, run(DenoiseNoiseEstimator::Imcra)) > 0.005f);
  REQUIRE(max_abs_diff(spp, run(DenoiseNoiseEstimator::Mcra)) > 0.005f);
  REQUIRE(max_abs_diff(spp, run(DenoiseNoiseEstimator::Quantile)) > 0.005f);
}

namespace {

/// A tone that occupies its band half the time, over a stationary floor.
/// @details Returns the clean reference and the noisy input. The duty cycle is
///   what separates the estimators: a band that falls silent between bursts lets
///   an estimator see its own floor, and one that reads the floor while the band
///   is busy subtracts the programme instead of the noise.
struct IntermittentTone {
  Audio clean;
  Audio noisy;
};

IntermittentTone intermittent_tone(int sample_rate, int samples, float tone_freq, float tone_amp,
                                   float noise_amp, double burst_seconds, uint32_t seed) {
  std::vector<float> clean(static_cast<size_t>(samples));
  std::vector<float> noisy(static_cast<size_t>(samples));
  std::mt19937 rng(seed);
  std::normal_distribution<float> noise(0.0f, noise_amp);
  for (int i = 0; i < samples; ++i) {
    const double t = static_cast<double>(i) / sample_rate;
    const bool on = static_cast<int>(t / burst_seconds) % 2 == 1;
    const float value =
        on ? static_cast<float>(tone_amp * std::sin(sonare::constants::kTwoPiD * tone_freq * t))
           : 0.0f;
    clean[static_cast<size_t>(i)] = value;
    noisy[static_cast<size_t>(i)] = value + noise(rng);
  }
  return {Audio::from_vector(std::move(clean), sample_rate),
          Audio::from_vector(std::move(noisy), sample_rate)};
}

/// Segmental SNR against a reference, over the frames where the reference has energy.
/// @details Silent frames are excluded rather than floored: they hold only the
///   error term, so including them measures noise removal in the gaps and hides
///   what the processing did to the programme, which is the axis under test.
double segmental_snr_db(const Audio& reference, const Audio& test, size_t frame = 512) {
  REQUIRE(reference.size() == test.size());
  const size_t frames = reference.size() / frame;
  REQUIRE(frames > 0);
  std::vector<double> reference_power(frames, 0.0);
  std::vector<double> error_power(frames, 0.0);
  for (size_t f = 0; f < frames; ++f) {
    for (size_t i = 0; i < frame; ++i) {
      const double r = reference[f * frame + i];
      const double e = static_cast<double>(test[f * frame + i]) - r;
      reference_power[f] += r * r;
      error_power[f] += e * e;
    }
  }
  const double loudest = *std::max_element(reference_power.begin(), reference_power.end());
  double total = 0.0;
  size_t counted = 0;
  for (size_t f = 0; f < frames; ++f) {
    if (reference_power[f] <= loudest * 1e-6) continue;
    total += 10.0 * std::log10(reference_power[f] / std::max(error_power[f], 1e-30));
    ++counted;
  }
  REQUIRE(counted > 0);  // no live frames would make every comparison below vacuous
  return total / static_cast<double>(counted);
}

}  // namespace

TEST_CASE("DenoiseClassical keeps the floor under an intermittently occupied band",
          "[mastering][repair]") {
  // A minimum-tracking estimator needs the floor to become observable between
  // programme events. Its smoothed power carries a burst forward, so on a band
  // that is busy half the time the tracked minimum sits well above the real
  // floor and the gain rule subtracts the programme.
  const IntermittentTone signal =
      intermittent_tone(22050, 22050 * 4, 1000.0f, 0.2f, 0.01f, 0.5, 20260917);

  DenoiseClassicalConfig config{};
  config.mode = DenoiseMode::LogMmse;
  auto gain_db = [&](DenoiseNoiseEstimator estimator) {
    config.noise_estimator = estimator;
    return segmental_snr_db(signal.clean, denoise_classical(signal.noisy, config)) -
           segmental_snr_db(signal.clean, signal.noisy);
  };

  // Measured: quantile +15.1 dB, spp +14.1 dB. The two minimum-tracking modes sit
  // at +0.6 and +0.5 on the same signal and are deliberately not asserted here --
  // an assertion pinning that would have to be deleted by whoever repairs them,
  // and a test that goes red on a fix is worse than no test.
  REQUIRE(gain_db(DenoiseNoiseEstimator::Quantile) > 8.0);
  REQUIRE(gain_db(DenoiseNoiseEstimator::Spp) > 8.0);
}

TEST_CASE("DenoiseClassical rejects inputs shorter than n_fft", "[mastering][repair]") {
  REQUIRE_THROWS_AS(denoise_classical(make_audio({0.03f, 0.05f})), SonareException);
}

TEST_CASE("DenoiseClassical rejects an estimator outside the enumeration", "[mastering][repair]") {
  // validate_config directly, not through denoise_classical: the tracker dispatch
  // refuses an unnamed value too, so the end-to-end call throws either way and
  // cannot tell whether the validator still does its half.
  DenoiseClassicalConfig config{};
  config.noise_estimator = static_cast<DenoiseNoiseEstimator>(99);
  REQUIRE_THROWS_AS(validate_config(config), SonareException);

  config.noise_estimator = static_cast<DenoiseNoiseEstimator>(-1);
  REQUIRE_THROWS_AS(validate_config(config), SonareException);

  config.noise_estimator = DenoiseNoiseEstimator::Spp;
  REQUIRE_NOTHROW(validate_config(config));

  // Same guard shape on the mode: the switch cannot see a value the enumeration
  // never names, so the `false` after it is what refuses one.
  config.mode = static_cast<DenoiseMode>(99);
  REQUIRE_THROWS_AS(validate_config(config), SonareException);
  config.mode = static_cast<DenoiseMode>(-1);
  REQUIRE_THROWS_AS(validate_config(config), SonareException);
}

TEST_CASE("DereverbClassical zero-pads inputs shorter than n_fft", "[mastering][repair]") {
  const Audio input = make_audio({0.5f, 0.04f, 0.02f});
  const Audio result = dereverb_classical(input, {0.05f, 0.25f});
  REQUIRE(result.size() == input.size());
  for (float sample : result) REQUIRE(std::isfinite(sample));
}

TEST_CASE("DereverbClassical spectral subtraction reduces late decay", "[mastering][repair]") {
  std::vector<float> samples(48000, 0.0f);
  samples[0] = 1.0f;
  for (size_t i = 1; i < samples.size(); ++i) {
    samples[i] = 0.4f * std::exp(-static_cast<float>(i) / 8000.0f) *
                 std::sin(sonare::constants::kTwoPi * 1000.0f * static_cast<float>(i) / 48000.0f);
  }
  const Audio input = Audio::from_vector(samples, 48000);
  // attenuation is the full subtraction this case measures. It reads 1 rather
  // than 0.5 because the field reaches the DSP now: at 0.5 the module applies
  // half the suppression and the tail ratio rises from 0.756 to 0.874.
  const auto output =
      dereverb_classical(input, {0.05f, 1.0f, 1024, 256, 0.25f, 20.0f, 2.0f, 0.02f});

  REQUIRE(output.size() == input.size());
  double in_tail = 0.0;
  double out_tail = 0.0;
  for (size_t i = 12000; i < input.size(); ++i) {
    in_tail += static_cast<double>(input[i]) * input[i];
    out_tail += static_cast<double>(output[i]) * output[i];
  }
  REQUIRE(out_tail < in_tail * 0.8);
}

TEST_CASE("DereverbClassical WPE mode further suppresses predictable late reverb",
          "[mastering][repair]") {
  std::vector<float> samples(48000, 0.0f);
  for (size_t i = 0; i < samples.size(); ++i) {
    const float direct = 0.4f * static_cast<float>(std::sin(sonare::constants::kTwoPiD * 700.0 *
                                                            static_cast<double>(i) / 48000.0));
    const float late = i >= 2400 ? 0.25f * samples[i - 2400] : 0.0f;
    samples[i] = direct + late;
  }
  const Audio input = Audio::from_vector(samples, 48000);
  const auto spectral =
      dereverb_classical(input, {0.05f, 0.5f, 1024, 256, 0.4f, 50.0f, 0.8f, 0.05f});
  const auto wpe = dereverb_classical(
      input, {0.05f, 0.5f, 1024, 256, 0.4f, 50.0f, 0.8f, 0.05f, true, 2, 3, 0.5f});

  REQUIRE(wpe.size() == input.size());
  REQUIRE(rms(wpe) < rms(spectral));
}

namespace {

/// @brief denoise_classical's noise-PSD estimators as they walked the spectrogram before the
///        traversal changed, followed by the Berouti gain stage.
/// @details Only SpectralSubtraction is mirrored, because its gain stage is four expressions:
///          the oracle stays a reference for the traversal rather than a second copy of the
///          Ephraim-Malah estimators. The noise estimator is an independent config field, so
///          all three noise-PSD paths are still reachable through it.
Audio oracle_denoise_spectral_subtraction(const Audio& audio,
                                          const DenoiseClassicalConfig& config) {
  StftConfig stft_config;
  stft_config.n_fft = config.n_fft;
  stft_config.hop_length = config.hop_length;
  stft_config.window = WindowType::Hann;
  stft_config.center = true;
  const Spectrogram spec = Spectrogram::compute(audio, stft_config);

  const int bins = spec.n_bins();
  const int frames = spec.n_frames();
  const auto& power = spec.power();
  std::vector<double> noise_psd(static_cast<size_t>(bins * frames), 0.0);

  if (config.noise_estimator == DenoiseNoiseEstimator::Quantile) {
    std::vector<std::pair<double, int>> frame_energies(static_cast<size_t>(frames));
    for (int t = 0; t < frames; ++t) {
      double energy = 0.0;
      for (int b = 0; b < bins; ++b) {
        energy += power[b * frames + t];
      }
      frame_energies[static_cast<size_t>(t)] = {energy, t};
    }
    std::sort(frame_energies.begin(), frame_energies.end(),
              [](const auto& a, const auto& b) { return a.first < b.first; });

    const int noise_frames =
        std::max(1, static_cast<int>(
                        std::round(static_cast<float>(frames) * config.noise_estimation_quantile)));
    std::vector<double> stationary(static_cast<size_t>(bins), 0.0);
    for (int i = 0; i < noise_frames; ++i) {
      const int t = frame_energies[static_cast<size_t>(i)].second;
      for (int b = 0; b < bins; ++b) {
        stationary[static_cast<size_t>(b)] += power[b * frames + t];
      }
    }
    const double scale = 1.0 / static_cast<double>(noise_frames);
    for (auto& value : stationary) value *= scale;
    for (int b = 0; b < bins; ++b) {
      for (int t = 0; t < frames; ++t) {
        noise_psd[static_cast<size_t>(b * frames + t)] = stationary[static_cast<size_t>(b)];
      }
    }
  } else {
    auto mode = mastering::common::NoiseTracker::Mode::Imcra;
    if (config.noise_estimator == DenoiseNoiseEstimator::Mcra) {
      mode = mastering::common::NoiseTracker::Mode::Mcra;
    }
    mastering::common::NoiseTracker tracker(bins, spec.sample_rate(), mode, config.hop_length);
    std::vector<float> frame_power(static_cast<size_t>(bins), 0.0f);
    for (int t = 0; t < frames; ++t) {
      for (int b = 0; b < bins; ++b) {
        frame_power[static_cast<size_t>(b)] =
            static_cast<float>(std::max(power[b * frames + t], 0.0f));
      }
      tracker.update(frame_power.data());
      const float* tracked = tracker.noise_psd();
      for (int b = 0; b < bins; ++b) {
        noise_psd[static_cast<size_t>(b * frames + t)] = tracked[static_cast<size_t>(b)];
      }
    }
  }

  const auto* complex_data = spec.complex_data();
  std::vector<std::complex<float>> denoised(static_cast<size_t>(bins * frames));
  const double alpha = static_cast<double>(config.over_subtraction);
  const double beta = static_cast<double>(config.spectral_floor);
  for (int b = 0; b < bins; ++b) {
    for (int t = 0; t < frames; ++t) {
      const size_t idx = static_cast<size_t>(b * frames + t);
      const std::complex<float>& bin = complex_data[idx];
      const double mag = std::abs(bin);
      const double bin_power = mag * mag;
      const double noise_pow = std::max(noise_psd[idx], 1e-12);
      const double floor_pow = beta * noise_pow;
      const double clean_power = std::max(bin_power - alpha * noise_pow, floor_pow);
      const double gain = mag > 1e-12 ? std::sqrt(clean_power) / mag : 0.0;
      denoised[idx] = {static_cast<float>(bin.real() * gain),
                       static_cast<float>(bin.imag() * gain)};
    }
  }

  const Spectrogram clean = Spectrogram::from_complex(
      denoised.data(), bins, frames, spec.n_fft(), spec.hop_length(), spec.sample_rate(),
      spec.window(), spec.center(), spec.win_length());
  return clean.to_audio(static_cast<int>(audio.size()));
}

/// @brief dereverb_classical's linear solve, taking its working storage by value.
std::vector<std::complex<float>> oracle_solve_linear_system(
    std::vector<std::vector<std::complex<double>>> matrix, std::vector<std::complex<double>> rhs) {
  const size_t n = rhs.size();
  for (size_t col = 0; col < n; ++col) {
    size_t pivot = col;
    double best = std::abs(matrix[col][col]);
    for (size_t row = col + 1; row < n; ++row) {
      const double candidate = std::abs(matrix[row][col]);
      if (candidate > best) {
        best = candidate;
        pivot = row;
      }
    }
    if (best < 1.0e-18) {
      rhs[col] = {0.0, 0.0};
      continue;
    }
    if (pivot != col) {
      std::swap(matrix[pivot], matrix[col]);
      std::swap(rhs[pivot], rhs[col]);
    }
    const auto diagonal = matrix[col][col];
    for (size_t k = col; k < n; ++k) matrix[col][k] /= diagonal;
    rhs[col] /= diagonal;
    for (size_t row = 0; row < n; ++row) {
      if (row == col) continue;
      const auto factor = matrix[row][col];
      if (std::abs(factor) < 1.0e-18) continue;
      for (size_t k = col; k < n; ++k) matrix[row][k] -= factor * matrix[col][k];
      rhs[row] -= factor * rhs[col];
    }
  }

  std::vector<std::complex<float>> solution(n);
  for (size_t i = 0; i < n; ++i) solution[i] = static_cast<std::complex<float>>(rhs[i]);
  return solution;
}

/// @brief dereverb_classical with the WPE covariance and cross buffers allocated inside the
///        bin loop, as they were before they were hoisted out of it.
/// @details The short-input padding branch is not mirrored; the caller keeps the input longer
///          than n_fft.
Audio oracle_dereverb(const Audio& audio, const DereverbClassicalConfig& config) {
  constexpr double kRegularization = static_cast<double>(sonare::constants::kSpectrumEpsilon);

  StftConfig stft_config;
  stft_config.n_fft = config.n_fft;
  stft_config.hop_length = config.hop_length;
  stft_config.window = WindowType::Hann;
  stft_config.center = true;
  const Spectrogram spec = Spectrogram::compute(audio, stft_config);

  const int bins = spec.n_bins();
  const int frames = spec.n_frames();
  const auto* complex_data = spec.complex_data();
  const auto& power = spec.power();
  std::vector<std::complex<float>> dereverbed(static_cast<size_t>(bins * frames));

  const int delay_frames =
      std::max(1, static_cast<int>(std::round(config.late_delay_ms * 0.001f *
                                              static_cast<float>(audio.sample_rate()) /
                                              static_cast<float>(config.hop_length))));
  const float delay_sec = static_cast<float>(delay_frames * config.hop_length) /
                          static_cast<float>(audio.sample_rate());
  const double decay = std::exp(-2.0 * static_cast<double>(delay_sec) * 6.0 * std::log(10.0) /
                                static_cast<double>(config.t60_sec));

  for (int b = 0; b < bins; ++b) {
    for (int t = 0; t < frames; ++t) {
      const size_t idx = static_cast<size_t>(b * frames + t);
      const std::complex<float>& bin = complex_data[idx];
      const double current_power = std::max(static_cast<double>(power[idx]), 1e-18);
      const int late_frame = t - delay_frames;
      const double late_psd =
          late_frame >= 0
              ? static_cast<double>(power[static_cast<size_t>(b * frames + late_frame)]) * decay
              : 0.0;
      const double clean_power =
          std::max(current_power - static_cast<double>(config.over_subtraction) * late_psd,
                   static_cast<double>(config.spectral_floor) * current_power);
      const double gain = std::sqrt(clean_power / current_power);
      dereverbed[idx] = {static_cast<float>(bin.real() * gain),
                         static_cast<float>(bin.imag() * gain)};
    }
  }

  if (config.wpe_enabled) {
    std::vector<std::complex<float>> next = dereverbed;
    const int taps = std::max(1, config.wpe_taps);
    const int first_predictable = delay_frames + taps - 1;
    for (int iteration = 0; iteration < config.wpe_iterations; ++iteration) {
      for (int b = 0; b < bins; ++b) {
        std::vector<std::vector<std::complex<double>>> covariance(
            static_cast<size_t>(taps),
            std::vector<std::complex<double>>(static_cast<size_t>(taps), {0.0, 0.0}));
        std::vector<std::complex<double>> cross(static_cast<size_t>(taps), {0.0, 0.0});
        for (int t = first_predictable; t < frames; ++t) {
          const auto current =
              static_cast<std::complex<double>>(dereverbed[static_cast<size_t>(b * frames + t)]);
          for (int i = 0; i < taps; ++i) {
            const auto xi = static_cast<std::complex<double>>(
                dereverbed[static_cast<size_t>(b * frames + t - delay_frames - i)]);
            cross[static_cast<size_t>(i)] += current * std::conj(xi);
            for (int j = 0; j < taps; ++j) {
              const auto xj = static_cast<std::complex<double>>(
                  dereverbed[static_cast<size_t>(b * frames + t - delay_frames - j)]);
              covariance[static_cast<size_t>(i)][static_cast<size_t>(j)] += xi * std::conj(xj);
            }
          }
        }
        for (int i = 0; i < taps; ++i) {
          covariance[static_cast<size_t>(i)][static_cast<size_t>(i)] +=
              std::complex<double>{kRegularization, 0.0};
        }
        auto predictors = oracle_solve_linear_system(std::move(covariance), std::move(cross));
        double predictor_norm = 0.0;
        for (const auto& predictor : predictors) predictor_norm += std::abs(predictor);
        if (predictor_norm > 0.98) {
          const float scale = static_cast<float>(0.98 / predictor_norm);
          for (auto& predictor : predictors) predictor *= scale;
        }
        for (int t = 0; t < frames; ++t) {
          const size_t idx = static_cast<size_t>(b * frames + t);
          if (t < first_predictable) {
            next[idx] = dereverbed[idx];
            continue;
          }
          std::complex<float> predicted{0.0f, 0.0f};
          for (int tap = 0; tap < taps; ++tap) {
            predicted += predictors[static_cast<size_t>(tap)] *
                         dereverbed[static_cast<size_t>(b * frames + t - delay_frames - tap)];
          }
          next[idx] = dereverbed[idx] - config.wpe_strength * predicted;
        }
      }
      dereverbed.swap(next);
    }
  }

  const Spectrogram clean = Spectrogram::from_complex(
      dereverbed.data(), bins, frames, spec.n_fft(), spec.hop_length(), spec.sample_rate(),
      spec.window(), spec.center(), spec.win_length());
  return clean.to_audio(static_cast<int>(audio.size()));
}

/// @brief Index of the first sample where two results differ, or size() when they agree.
size_t first_differing_sample(const Audio& got, const Audio& want) {
  for (size_t i = 0; i < got.size(); ++i) {
    if (got[i] != want[i]) return i;
  }
  return got.size();
}

}  // namespace

TEST_CASE("DenoiseClassical noise estimation does not depend on the traversal",
          "[mastering][repair][denoise]") {
  // Both estimators read a row-major [bins x frames] power spectrogram. Reading it bin-major,
  // and staging the recursive tracker's columns in blocks rather than gathering one at a time,
  // has to leave the denoised audio identical sample for sample -- the RMS-reduction cases
  // above pass through a small numerical move without noticing it.
  const int sr = 48000;
  const Audio input = noisy_tone(sr, 12800, 1000.0f, 0.5f, 0.05f, 98765);

  DenoiseClassicalConfig config{};
  config.mode = DenoiseMode::SpectralSubtraction;
  config.n_fft = 256;
  config.hop_length = 64;

  // What the block-staged tracker needs from this input: many blocks, and a last one that is
  // partial whatever block size the staging picks.
  {
    StftConfig stft_config;
    stft_config.n_fft = config.n_fft;
    stft_config.hop_length = config.hop_length;
    stft_config.window = WindowType::Hann;
    stft_config.center = true;
    const Spectrogram spec = Spectrogram::compute(input, stft_config);
    CAPTURE(spec.n_frames());
    REQUIRE(spec.n_frames() > 128);
    REQUIRE(spec.n_frames() % 2 == 1);
  }

  const std::vector<DenoiseNoiseEstimator> estimators = {
      DenoiseNoiseEstimator::Quantile, DenoiseNoiseEstimator::Mcra, DenoiseNoiseEstimator::Imcra};
  for (DenoiseNoiseEstimator estimator : estimators) {
    CAPTURE(static_cast<int>(estimator));
    config.noise_estimator = estimator;

    const Audio got = denoise_classical(input, config);
    const Audio want = oracle_denoise_spectral_subtraction(input, config);
    REQUIRE(got.size() == want.size());
    REQUIRE(got.size() == input.size());

    const size_t mismatch = first_differing_sample(got, want);
    if (mismatch != got.size()) {
      CAPTURE(mismatch);
      REQUIRE(got[mismatch] == want[mismatch]);
    }
    // A silent oracle would compare two zero buffers and agree for the wrong reason.
    REQUIRE(rms(want) > 0.0f);
  }
}

namespace {

/// @brief The gain smoother as a whole-plane median, which is what it replaced.
/// @details Reads the [bins x frames] plane directly and truncates the window at
///   every edge. The streaming smoother has to reproduce this exactly: it sees
///   one frame at a time and cannot look up how many are still coming, so the
///   last frame's truncation is the case a ring buffer gets wrong.
std::vector<double> oracle_smooth_gain_3x3(const std::vector<double>& gains, int bins, int frames) {
  std::vector<double> smoothed = gains;
  std::vector<double> window;
  window.reserve(9);
  for (int b = 0; b < bins; ++b) {
    for (int t = 0; t < frames; ++t) {
      window.clear();
      for (int db = -1; db <= 1; ++db) {
        const int bb = b + db;
        if (bb < 0 || bb >= bins) continue;
        for (int dt = -1; dt <= 1; ++dt) {
          const int tt = t + dt;
          if (tt < 0 || tt >= frames) continue;
          window.push_back(gains[static_cast<size_t>(bb * frames + tt)]);
        }
      }
      std::nth_element(window.begin(), window.begin() + window.size() / 2, window.end());
      smoothed[static_cast<size_t>(b * frames + t)] = window[window.size() / 2];
    }
  }
  return smoothed;
}

}  // namespace

TEST_CASE("Denoise gain smoothing reads the same window frame by frame as it did plane-wide",
          "[mastering][repair][denoise]") {
  // The only part of the denoiser that is not causal. A ring buffer that answers
  // the last frame as though a successor existed, or that indexes the ring by the
  // wrong offset, moves the mask on exactly the frames an averaged comparison
  // cannot see -- so this compares every cell with ==.
  std::mt19937 rng(20260917);
  std::uniform_real_distribution<double> dist(0.02, 1.0);

  struct Geometry {
    int bins;
    int frames;
  };
  // Every truncation corner: one bin, one frame, and both at once, alongside a
  // plane large enough to have interior cells that are truncated nowhere.
  const std::vector<Geometry> geometries = {{1, 1}, {1, 5},  {2, 2},  {3, 1},
                                            {5, 3}, {17, 9}, {33, 64}};

  for (const Geometry geometry : geometries) {
    CAPTURE(geometry.bins, geometry.frames);
    const int bins = geometry.bins;
    const int frames = geometry.frames;

    std::vector<double> raw(static_cast<size_t>(bins * frames), 0.0);
    for (double& value : raw) value = dist(rng);

    const std::vector<double> want = oracle_smooth_gain_3x3(raw, bins, frames);

    // A median that returned its input would agree with the oracle everywhere and
    // prove nothing. One frame of a 3x3 median over independent draws moves almost
    // every cell, so this is a demand rather than a hope -- but it is asserted.
    if (bins > 1 || frames > 1) {
      REQUIRE(want != raw);
    }

    mastering::repair::detail::MedianGainSmoother smoother(bins);
    std::vector<double> got(static_cast<size_t>(bins * frames), 0.0);
    std::vector<double> frame_in(static_cast<size_t>(bins), 0.0);
    int emitted = 0;
    const auto store = [&](const double* out) {
      for (int b = 0; b < bins; ++b) {
        got[static_cast<size_t>(b * frames + emitted)] = out[b];
      }
      ++emitted;
    };

    for (int t = 0; t < frames; ++t) {
      for (int b = 0; b < bins; ++b) {
        frame_in[static_cast<size_t>(b)] = raw[static_cast<size_t>(b * frames + t)];
      }
      const double* ready = smoother.push(frame_in.data());
      if (ready != nullptr) store(ready);
    }
    const double* tail = smoother.flush();
    if (tail != nullptr) store(tail);

    REQUIRE(emitted == frames);
    for (int b = 0; b < bins; ++b) {
      for (int t = 0; t < frames; ++t) {
        const size_t idx = static_cast<size_t>(b * frames + t);
        CAPTURE(b, t);
        REQUIRE(got[idx] == want[idx]);
      }
    }
  }
}

TEST_CASE("DereverbClassical WPE does not depend on where its working buffers live",
          "[mastering][repair][dereverb]") {
  // The covariance rows and the cross vector are now allocated once for the whole WPE stage,
  // so every bin has to refill both levels: a row still holding the previous bin's values
  // solves a different system. WPE feeds its own output back into the next iteration, so a
  // move does not stay in the last bit.
  const int sr = 48000;
  std::vector<float> samples(24000, 0.0f);
  for (size_t i = 0; i < samples.size(); ++i) {
    const float direct = 0.4f * static_cast<float>(std::sin(sonare::constants::kTwoPiD * 700.0 *
                                                            static_cast<double>(i) / sr));
    const float late = i >= 1200 ? 0.25f * samples[i - 1200] : 0.0f;
    samples[i] = direct + late;
  }
  const Audio input = Audio::from_vector(std::move(samples), sr);

  struct WpeParams {
    int iterations;
    int taps;
  };
  const std::vector<WpeParams> sweep = {{1, 2}, {2, 3}};

  for (const WpeParams& params : sweep) {
    CAPTURE(params.iterations, params.taps);
    DereverbClassicalConfig config{};
    config.n_fft = 256;
    config.hop_length = 64;
    config.t60_sec = 0.4f;
    config.late_delay_ms = 20.0f;
    config.wpe_enabled = true;
    config.wpe_iterations = params.iterations;
    config.wpe_taps = params.taps;
    config.wpe_strength = 0.5f;

    const Audio got = dereverb_classical(input, config);
    const Audio want = oracle_dereverb(input, config);
    REQUIRE(got.size() == want.size());
    REQUIRE(got.size() == input.size());

    const size_t mismatch = first_differing_sample(got, want);
    if (mismatch != got.size()) {
      CAPTURE(mismatch);
      REQUIRE(got[mismatch] == want[mismatch]);
    }
    REQUIRE(rms(want) > 0.0f);
  }
}

namespace {

// `detail` alone is ambiguous here: the file opens both sonare and
// sonare::mastering::repair, and each has one.
namespace repair_detail = sonare::mastering::repair::detail;

std::vector<float> spin_probe_signal() {
  std::vector<float> samples(512, 0.0f);
  for (size_t i = 0; i < samples.size(); ++i) {
    samples[i] = 0.1f * static_cast<float>(std::sin(sonare::constants::kTwoPiD * i / 64.0));
  }
  for (size_t index : {37u, 101u, 163u, 229u, 331u, 419u}) {
    samples[index] += (index % 2 == 0) ? 0.3f : -0.3f;
  }
  return samples;
}

/// A step plus an isolated impulse over a small noise floor. The noise floor is
/// what makes the case non-vacuous: the MAD estimate of a noiseless step is zero,
/// which drives the BayesShrink threshold to zero and leaves the transform an
/// exact identity no matter how it is shifted.
std::vector<float> spin_step_signal() {
  std::vector<float> samples(512, 0.0f);
  std::mt19937 rng(20260916u);
  std::uniform_real_distribution<float> noise(-0.01f, 0.01f);
  for (size_t i = 0; i < samples.size(); ++i) samples[i] = noise(rng);
  for (size_t i = 251; i < samples.size(); ++i) samples[i] += 0.4f;
  samples[123] += 0.3f;
  return samples;
}

/// A smooth run from one polarity to the other, so a cyclic shift butts a large
/// negative sample against a large positive one: the biggest seam the spinning can
/// build out of a buffer the forward transform never wraps. Both ends carry signal
/// on purpose -- with a near-silent end, mishandling the wrapped region costs
/// almost nothing and reads as no defect at all.
std::vector<float> spin_seam_signal() {
  std::vector<float> samples(512, 0.0f);
  std::mt19937 rng(20260917u);
  std::uniform_real_distribution<float> noise(-0.01f, 0.01f);
  for (size_t i = 0; i < samples.size(); ++i) {
    const float ramp = static_cast<float>(i) / static_cast<float>(samples.size() - 1);
    samples[i] = noise(rng) + 0.45f * std::tanh(8.0f * (ramp - 0.5f)) +
                 0.05f * static_cast<float>(std::sin(sonare::constants::kTwoPiD * i / 16.0));
  }
  return samples;
}

/// The largest absolute difference over [first, last).
double max_difference(const std::vector<float>& a, const std::vector<float>& b, size_t first,
                      size_t last) {
  double worst = 0.0;
  for (size_t i = first; i < last && i < a.size(); ++i) {
    worst = std::max(worst, std::abs(static_cast<double>(a[i]) - static_cast<double>(b[i])));
  }
  return worst;
}

/// The share of the repair's error that is antisymmetric inside a Haar pair.
/// Shrinking one detail coefficient moves its pair by (-d, +d), so blocking
/// shows up here and nowhere else; a shift-invariant repair has no reason to
/// prefer that axis.
double pair_antisymmetry(const std::vector<float>& input, const std::vector<float>& output,
                         size_t margin) {
  double anti = 0.0;
  double total = 0.0;
  for (size_t i = margin; i + margin + 1 < input.size(); i += 2) {
    const double even = static_cast<double>(output[i]) - static_cast<double>(input[i]);
    const double odd = static_cast<double>(output[i + 1]) - static_cast<double>(input[i + 1]);
    const double half_difference = 0.5 * (even - odd);
    anti += half_difference * half_difference;
    total += 0.5 * (even * even + odd * odd);
  }
  return total <= 0.0 ? 0.0 : std::sqrt(anti / total);
}

std::vector<float> rotate_left(const std::vector<float>& samples, size_t shift) {
  std::vector<float> out(samples.size(), 0.0f);
  for (size_t i = 0; i < samples.size(); ++i) out[i] = samples[(i + shift) % samples.size()];
  return out;
}

std::vector<float> rotate_right(const std::vector<float>& samples, size_t shift) {
  std::vector<float> out(samples.size(), 0.0f);
  for (size_t i = 0; i < samples.size(); ++i) out[(i + shift) % samples.size()] = samples[i];
  return out;
}

double vec_rms(const std::vector<float>& samples, size_t margin) {
  double sum = 0.0;
  size_t count = 0;
  for (size_t i = margin; i + margin < samples.size(); ++i) {
    sum += static_cast<double>(samples[i]) * static_cast<double>(samples[i]);
    ++count;
  }
  return count == 0 ? 0.0 : std::sqrt(sum / static_cast<double>(count));
}

using Repair = std::function<std::vector<float>(const std::vector<float>&)>;

Repair spun_repair(const DecrackleConfig& config, int shifts) {
  return [config, shifts](const std::vector<float>& samples) {
    return repair_detail::wavelet_shrink_spun(samples, config, shifts);
  };
}

Repair public_repair(const DecrackleConfig& config) {
  return [config](const std::vector<float>& samples) {
    const auto processed = decrackle(make_audio(samples), config);
    return std::vector<float>(processed.data(), processed.data() + processed.size());
  };
}

/// The spread of a shift-and-unshift family. A shift-invariant operator makes
/// every member identical, so this is zero; the decimating Haar does not.
double shift_spread(const std::vector<float>& samples, const Repair& repair, size_t probe_shifts,
                    size_t margin) {
  std::vector<std::vector<float>> family;
  for (size_t shift = 0; shift < probe_shifts; ++shift) {
    family.push_back(rotate_right(repair(rotate_left(samples, shift)), shift));
  }
  std::vector<float> mean(samples.size(), 0.0f);
  for (size_t i = 0; i < samples.size(); ++i) {
    double sum = 0.0;
    for (const auto& member : family) sum += member[i];
    mean[i] = static_cast<float>(sum / static_cast<double>(family.size()));
  }
  double total = 0.0;
  for (const auto& member : family) {
    std::vector<float> deviation(samples.size(), 0.0f);
    for (size_t i = 0; i < samples.size(); ++i) deviation[i] = member[i] - mean[i];
    const double value = vec_rms(deviation, margin);
    total += value * value;
  }
  return std::sqrt(total / static_cast<double>(family.size())) / vec_rms(samples, 0);
}

}  // namespace

TEST_CASE("Decrackle wavelet cycle spinning leaves no artefact at the buffer ends",
          "[mastering][repair]") {
  const DecrackleConfig wavelet{0.02f, DecrackleMode::WaveletShrinkage, 4};
  // haar_forward never wraps the buffer, so a cyclic shift butts the tail against
  // the head and makes a discontinuity the unspun pass never saw. What the wrap can
  // reach is one 2^levels block at each end, and nothing else guards that.
  constexpr size_t kBlock = 16;

  // Swept over the seam itself rather than measured at one amplitude, because a
  // single amplitude cannot separate a bounded effect from an absent one.
  for (float scale : {0.01f, 0.1f, 1.0f, 5.0f, 25.0f}) {
    std::vector<float> input = spin_seam_signal();
    for (size_t i = 0; i < input.size(); ++i) {
      const float ramp = static_cast<float>(i) / static_cast<float>(input.size() - 1);
      input[i] += (scale - 1.0f) * 0.45f * std::tanh(8.0f * (ramp - 0.5f));
    }
    const size_t size = input.size();
    const auto unspun = repair_detail::wavelet_shrink_spun(input, wavelet, 1);
    const auto spun =
        repair_detail::wavelet_shrink_spun(input, wavelet, repair_detail::kCycleSpinShifts);
    INFO("seam " << std::abs(input[size - 1] - input[0]));

    // Non-vacuity: the spinning has to have moved the interior, or the bounds below
    // are satisfied by an output that never changed.
    const double moved = max_difference(spun, unspun, 2 * kBlock, size - 2 * kBlock);
    REQUIRE(moved > 1e-4);

    // No edge-localised excess: the block the wrap reaches is not a special place.
    REQUIRE(max_difference(spun, unspun, 0, kBlock) < 2.0 * moved);
    REQUIRE(max_difference(spun, unspun, size - kBlock, size) < 2.0 * moved);

    // And the repair at those ends is no worse than the pass this replaced.
    REQUIRE(max_difference(spun, input, 0, 2 * kBlock) <
            max_difference(unspun, input, 0, 2 * kBlock));
    REQUIRE(max_difference(spun, input, size - 2 * kBlock, size) <
            max_difference(unspun, input, size - 2 * kBlock, size));
  }
}

TEST_CASE("Decrackle wavelet cycle spinning cuts the mode's dependence on the input phase",
          "[mastering][repair]") {
  const DecrackleConfig wavelet{0.02f, DecrackleMode::WaveletShrinkage, 4};

  // The instrument's own floor. A noiseless step has a zero MAD estimate, which
  // drives the BayesShrink threshold to zero and leaves the pass an identity up
  // to the transform's own round-trip rounding -- so what the measurement reads
  // here is its reading for an operation that really is shift-invariant, and the
  // numbers below have to stand clear of it.
  std::vector<float> noiseless(512, 0.0f);
  for (size_t i = 251; i < noiseless.size(); ++i) noiseless[i] = 0.4f;
  DecrackleReport silent;
  const auto untouched = decrackle(make_audio(noiseless), wavelet, &silent);
  REQUIRE(silent.detail_coefficients > 0);
  REQUIRE(silent.noise_sigma == 0.0f);
  for (size_t i = 0; i < noiseless.size(); ++i) {
    REQUIRE(std::abs(untouched[i] - noiseless[i]) < 1e-6f);
  }
  REQUIRE(shift_spread(noiseless, spun_repair(wavelet, 1), 16, 32) < 1e-6);

  // Both signals: a tone carrying isolated impulses, and a step with an impulse
  // over a noise floor, which is where blocking and pseudo-Gibbs show.
  for (const auto& samples : {spin_probe_signal(), spin_step_signal()}) {
    const double unspun = shift_spread(samples, spun_repair(wavelet, 1), 16, 32);
    const double spun =
        shift_spread(samples, spun_repair(wavelet, repair_detail::kCycleSpinShifts), 16, 32);

    // Non-vacuity: the unspun shift dependence is orders above the floor measured
    // above, so there is something for the spinning to remove.
    INFO("unspun " << unspun << " spun " << spun);
    REQUIRE(unspun > 1e-3);
    REQUIRE(spun < 0.1 * unspun);
  }
}

TEST_CASE("Decrackle wavelet shift dependence falls with every doubling of the spin count",
          "[mastering][repair]") {
  const DecrackleConfig wavelet{0.02f, DecrackleMode::WaveletShrinkage, 4};
  const auto probe = spin_probe_signal();

  std::vector<double> spreads;
  for (int shifts : {1, 2, 4, 8}) {
    spreads.push_back(shift_spread(probe, spun_repair(wavelet, shifts), 16, 32));
  }
  for (size_t i = 1; i < spreads.size(); ++i) {
    INFO("spin count " << (1u << i));
    REQUIRE(spreads[i] < spreads[i - 1]);
  }

  // With three levels the pair grid repeats every eight samples, so eight shifts
  // cover every phase the transform has and the averaging is exact rather than
  // partial. Interior only: a cyclic shift wraps the far end of the buffer onto
  // the near one, and that seam is not a phase artefact.
  DecrackleConfig three_levels = wavelet;
  three_levels.levels = 3;
  const double covered =
      shift_spread(probe, spun_repair(three_levels, repair_detail::kCycleSpinShifts), 16, 32);
  REQUIRE(covered < 0.01 * shift_spread(probe, spun_repair(three_levels, 1), 16, 32));
}

TEST_CASE("Decrackle wavelet cycle spinning evens the repair across the Haar pair grid",
          "[mastering][repair]") {
  const DecrackleConfig wavelet{0.02f, DecrackleMode::WaveletShrinkage, 4};
  const auto probe = spin_probe_signal();

  const double unspun =
      pair_antisymmetry(probe, repair_detail::wavelet_shrink_spun(probe, wavelet, 1), 32);
  const double spun = pair_antisymmetry(
      probe, repair_detail::wavelet_shrink_spun(probe, wavelet, repair_detail::kCycleSpinShifts),
      32);

  // An error with no pair-phase preference sits at 1/sqrt(2); the unspun transform
  // is above it because shrinking a detail coefficient moves its pair by (-d, +d).
  REQUIRE(unspun > sonare::constants::kInvSqrt2);
  REQUIRE(spun < sonare::constants::kInvSqrt2);
}

TEST_CASE("Decrackle wavelet mode averages exactly kCycleSpinShifts phases",
          "[mastering][repair]") {
  const DecrackleConfig wavelet{0.02f, DecrackleMode::WaveletShrinkage, 4};
  const auto probe = spin_probe_signal();

  const auto through_facade = public_repair(wavelet)(probe);
  const auto spun =
      repair_detail::wavelet_shrink_spun(probe, wavelet, repair_detail::kCycleSpinShifts);
  for (size_t i = 0; i < probe.size(); ++i) REQUIRE(through_facade[i] == spun[i]);

  // A count below one, and one past the buffer length, both collapse to a single
  // pass rather than dividing by a weight no phase carried.
  const auto once = repair_detail::wavelet_shrink_spun(probe, wavelet, 1);
  for (int shifts : {0, -3}) {
    const auto clamped = repair_detail::wavelet_shrink_spun(probe, wavelet, shifts);
    for (size_t i = 0; i < probe.size(); ++i) REQUIRE(clamped[i] == once[i]);
  }
  const std::vector<float> tiny = {0.1f, 0.8f, 0.12f};
  const auto over = repair_detail::wavelet_shrink_spun(tiny, wavelet, 64);
  const auto exact = repair_detail::wavelet_shrink_spun(tiny, wavelet, 3);
  for (size_t i = 0; i < tiny.size(); ++i) REQUIRE(over[i] == exact[i]);
}

TEST_CASE("Decrackle at one spin reproduces the unspun transform", "[mastering][repair]") {
  const DecrackleConfig wavelet{0.02f, DecrackleMode::WaveletShrinkage, 4};
  const auto probe = spin_probe_signal();

  // Captured from the decimating implementation before cycle spinning was added.
  // Eight shifts move these samples by parts in a hundred, so the tolerance is
  // four orders tighter than the change it has to tell apart.
  const std::array<float, 8> unspun_golden = {0x1.2b59d4p-4f, 0x1.3becc8p-4f, 0x1.57387ep-4f,
                                              0x1.61772ep-4f, 0x1.80ee6ep-4f, 0x1.840d8ep-4f,
                                              0x1.8ee8dep-4f, 0x1.8ee8dep-4f};
  const auto once = repair_detail::wavelet_shrink_spun(probe, wavelet, 1);
  for (size_t i = 0; i < unspun_golden.size(); ++i) {
    INFO("sample " << (200 + i));
    REQUIRE_THAT(once[200 + i], WithinAbs(unspun_golden[i], 1e-6f));
  }

  const auto spun = public_repair(wavelet)(probe);
  bool spinning_moved_the_golden_window = false;
  for (size_t i = 0; i < unspun_golden.size(); ++i) {
    if (std::abs(spun[200 + i] - unspun_golden[i]) > 1e-3f) spinning_moved_the_golden_window = true;
  }
  REQUIRE(spinning_moved_the_golden_window);
}

TEST_CASE("Decrackle median mode is untouched by the wavelet cycle spinning",
          "[mastering][repair]") {
  // Captured from the same pre-spinning implementation. Median mode never
  // arithmetically combines samples -- it copies either the input or one of its
  // neighbours -- so this comparison is exact rather than toleranced.
  const std::array<float, 8> median_golden = {0x1.21a186p-4f, 0x1.3ca006p-4f, 0x1.5491e8p-4f,
                                              0x1.693c26p-4f, 0x1.7a6bcap-4f, 0x1.87f678p-4f,
                                              0x1.91bacap-4f, 0x1.97a0aep-4f};
  const auto probe = spin_probe_signal();
  const auto repaired = public_repair({0.02f, DecrackleMode::Median, 4})(probe);
  for (size_t i = 0; i < median_golden.size(); ++i) {
    INFO("sample " << (200 + i));
    REQUIRE(repaired[200 + i] == median_golden[i]);
  }

  DecrackleReport report;
  decrackle(make_audio(probe), {0.02f, DecrackleMode::Median, 4}, &report);
  REQUIRE(report.replaced_samples > 0);
  REQUIRE(report.detail_coefficients == 0);
}

TEST_CASE("Repair helpers validate inputs", "[mastering][repair]") {
  const Audio empty;
  REQUIRE_THROWS(trim_silence(empty));
  REQUIRE_THROWS(trim_silence(make_audio({0.0f}), {0.0f, 0, TrimSilenceMode::Peak, -60.0f, 0.0f}));
  REQUIRE_THROWS(declick(make_audio({0.0f}), {0.0f, 1.0f}));
  REQUIRE_THROWS(declick(make_audio({0.0f}), {0.8f, 1.0f, 1, -1, 8.0f}));
  REQUIRE_THROWS(decrackle(make_audio({0.0f}), {0.0f}));
  REQUIRE_THROWS(decrackle(make_audio({0.0f}), {0.1f, DecrackleMode::WaveletShrinkage, 0}));
  REQUIRE_THROWS(declip(make_audio({0.0f}), {2.0f}));
  REQUIRE_THROWS(declip(make_audio({0.0f}), {0.98f, -1, 2}));
  REQUIRE_THROWS(dereverb_classical(make_audio({0.0f, 0.1f}), {0.05f, 0.5f, 1024, 256, 0.4f, 50.0f,
                                                               0.8f, 0.05f, true, 0, 3, 0.5f}));
  REQUIRE_THROWS(dehum(make_audio({0.0f}), {0.0f, 1, 10.0f}));
  DenoiseClassicalConfig bad_config{};
  bad_config.n_fft = 0;
  REQUIRE_THROWS(denoise_classical(make_audio({0.0f}), bad_config));
  REQUIRE_THROWS(dereverb_classical(make_audio({0.0f}), {0.0f, 2.0f}));
  REQUIRE_THROWS(dereverb_classical(make_audio({0.0f}), {0.0f, 0.5f, 1000}));
}
