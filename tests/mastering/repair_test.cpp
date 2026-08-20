#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <cmath>
#include <cstddef>
#include <random>
#include <vector>

#include "mastering/repair/declick.h"
#include "mastering/repair/declip.h"
#include "mastering/repair/decrackle.h"
#include "mastering/repair/dehum.h"
#include "mastering/repair/denoise_classical.h"
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

TEST_CASE("DenoiseClassical rejects inputs shorter than n_fft", "[mastering][repair]") {
  REQUIRE_THROWS_AS(denoise_classical(make_audio({0.03f, 0.05f})), SonareException);
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
  const auto output =
      dereverb_classical(input, {0.05f, 0.5f, 1024, 256, 0.25f, 20.0f, 2.0f, 0.02f});

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
