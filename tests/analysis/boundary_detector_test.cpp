/// @file boundary_detector_test.cpp
/// @brief Tests for boundary detector.

#include "analysis/boundary_detector.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <cmath>
#include <cstddef>
#include <vector>

#include "util/constants.h"

using namespace sonare;
using Catch::Matchers::WithinAbs;

namespace {

/// @brief Creates a sine wave at given frequency.
Audio create_sine(float freq, int sr = 22050, float duration = 1.0f) {
  int n_samples = static_cast<int>(sr * duration);
  std::vector<float> samples(n_samples);

  for (int i = 0; i < n_samples; ++i) {
    float t = static_cast<float>(i) / static_cast<float>(sr);
    samples[i] = 0.5f * std::sin(2.0f * sonare::constants::kPiD * freq * t);
  }

  return Audio::from_vector(std::move(samples), sr);
}

/// @brief Creates audio with two distinct sections.
Audio create_two_sections(int sr = 22050, float section_duration = 2.0f) {
  int section_samples = static_cast<int>(sr * section_duration);
  int total_samples = section_samples * 2;
  std::vector<float> samples(total_samples);

  // Section 1: Low frequency tone with harmonics
  for (int i = 0; i < section_samples; ++i) {
    float t = static_cast<float>(i) / static_cast<float>(sr);
    samples[i] = 0.5f * std::sin(2.0f * sonare::constants::kPiD * 220.0f * t);
    samples[i] += 0.25f * std::sin(2.0f * sonare::constants::kPiD * 440.0f * t);
    samples[i] += 0.125f * std::sin(2.0f * sonare::constants::kPiD * 660.0f * t);
  }

  // Section 2: Higher frequency tone with different timbre
  for (int i = section_samples; i < total_samples; ++i) {
    float t = static_cast<float>(i) / static_cast<float>(sr);
    samples[i] = 0.5f * std::sin(2.0f * sonare::constants::kPiD * 880.0f * t);
    samples[i] += 0.3f * std::sin(2.0f * sonare::constants::kPiD * 1760.0f * t);
  }

  return Audio::from_vector(std::move(samples), sr);
}

/// @brief Creates audio with multiple sections.
Audio create_multi_sections(int sr = 22050, float section_duration = 1.5f) {
  int section_samples = static_cast<int>(sr * section_duration);
  int total_samples = section_samples * 4;
  std::vector<float> samples(total_samples);

  float freqs[] = {220.0f, 440.0f, 330.0f, 550.0f};

  for (int s = 0; s < 4; ++s) {
    int start = s * section_samples;
    float freq = freqs[s];

    for (int i = 0; i < section_samples; ++i) {
      float t = static_cast<float>(i) / static_cast<float>(sr);
      samples[start + i] = 0.5f * std::sin(2.0f * sonare::constants::kPiD * freq * t);
      samples[start + i] += 0.25f * std::sin(2.0f * sonare::constants::kPiD * freq * 2.0f * t);
    }
  }

  return Audio::from_vector(std::move(samples), sr);
}

}  // namespace

TEST_CASE("BoundaryDetector basic", "[boundary_detector]") {
  Audio audio = create_two_sections();

  BoundaryConfig config;
  BoundaryDetector detector(audio, config);

  // Should detect at least the novelty curve
  REQUIRE(!detector.novelty_curve().empty());
  REQUIRE(detector.sample_rate() == audio.sample_rate());
  REQUIRE(detector.hop_length() > 0);
}

TEST_CASE("BoundaryDetector two sections", "[boundary_detector]") {
  Audio audio = create_two_sections(22050, 2.0f);

  BoundaryConfig config;
  config.threshold = 0.2f;
  config.peak_distance = 1.0f;

  BoundaryDetector detector(audio, config);

  // May detect a boundary near the section change
  const auto& novelty = detector.novelty_curve();
  REQUIRE(!novelty.empty());

  // Novelty curve should have values
  float max_novelty = 0.0f;
  for (float val : novelty) {
    max_novelty = std::max(max_novelty, val);
  }
  REQUIRE(max_novelty >= 0.0f);
}

TEST_CASE("BoundaryDetector boundary times", "[boundary_detector]") {
  Audio audio = create_two_sections();

  BoundaryConfig config;
  config.threshold = 0.1f;

  BoundaryDetector detector(audio, config);

  auto times = detector.boundary_times();
  REQUIRE(times.size() == detector.count());

  // Times should be non-negative and sorted
  for (size_t i = 0; i < times.size(); ++i) {
    REQUIRE(times[i] >= 0.0f);
    if (i > 0) {
      REQUIRE(times[i] > times[i - 1]);
    }
  }
}

TEST_CASE("BoundaryDetector novelty curve", "[boundary_detector]") {
  Audio audio = create_sine(440.0f, 22050, 3.0f);

  BoundaryDetector detector(audio);

  const auto& novelty = detector.novelty_curve();

  REQUIRE(!novelty.empty());

  // All values should be in [0, 1] range
  for (float val : novelty) {
    REQUIRE(val >= 0.0f);
    REQUIRE(val <= 1.0f);
  }
}

TEST_CASE("BoundaryDetector config options", "[boundary_detector]") {
  Audio audio = create_two_sections();

  // MFCC only
  BoundaryConfig config1;
  config1.use_mfcc = true;
  config1.use_chroma = false;
  BoundaryDetector detector1(audio, config1);
  REQUIRE(!detector1.novelty_curve().empty());

  // Chroma only
  BoundaryConfig config2;
  config2.use_mfcc = false;
  config2.use_chroma = true;
  BoundaryDetector detector2(audio, config2);
  REQUIRE(!detector2.novelty_curve().empty());

  // Both
  BoundaryConfig config3;
  config3.use_mfcc = true;
  config3.use_chroma = true;
  BoundaryDetector detector3(audio, config3);
  REQUIRE(!detector3.novelty_curve().empty());
}

TEST_CASE("BoundaryDetector boundaries struct", "[boundary_detector]") {
  Audio audio = create_multi_sections();

  BoundaryConfig config;
  config.threshold = 0.1f;
  config.peak_distance = 1.0f;

  BoundaryDetector detector(audio, config);

  const auto& boundaries = detector.boundaries();

  for (const auto& b : boundaries) {
    REQUIRE(b.time >= 0.0f);
    REQUIRE(b.frame >= 0);
    REQUIRE(b.strength >= 0.0f);
    REQUIRE(b.strength <= 1.0f);
  }
}

TEST_CASE("detect_boundaries quick function", "[boundary_detector]") {
  Audio audio = create_two_sections();

  auto times = detect_boundaries(audio);

  // Should return vector (possibly empty)
  // Times should be sorted
  for (size_t i = 1; i < times.size(); ++i) {
    REQUIRE(times[i] > times[i - 1]);
  }
}

TEST_CASE("BoundaryDetector short audio", "[boundary_detector]") {
  // Very short audio (0.5 seconds)
  Audio audio = create_sine(440.0f, 22050, 0.5f);

  BoundaryConfig config;
  BoundaryDetector detector(audio, config);

  // Should still work without crashing
  REQUIRE(detector.sample_rate() == 22050);
}

TEST_CASE("BoundaryDetector kernel size", "[boundary_detector]") {
  Audio audio = create_two_sections();

  BoundaryConfig config1;
  config1.kernel_size = 32;
  BoundaryDetector detector1(audio, config1);

  BoundaryConfig config2;
  config2.kernel_size = 128;
  BoundaryDetector detector2(audio, config2);

  // Both should produce novelty curves
  REQUIRE(!detector1.novelty_curve().empty());
  REQUIRE(!detector2.novelty_curve().empty());
}

TEST_CASE("BoundaryDetector peak distance", "[boundary_detector]") {
  Audio audio = create_multi_sections();

  BoundaryConfig config;
  config.threshold = 0.1f;
  config.peak_distance = 0.5f;

  BoundaryDetector detector(audio, config);

  auto times = detector.boundary_times();

  // Check minimum distance between boundaries
  for (size_t i = 1; i < times.size(); ++i) {
    float distance = times[i] - times[i - 1];
    REQUIRE(distance >= config.peak_distance * 0.9f);  // Allow small tolerance
  }
}

namespace {

/// @brief Similarity-band allocation implied by a frame count, in bytes.
size_t band_bytes(int ssm_frames, int kernel_size) {
  return static_cast<size_t>(ssm_frames) * boundary_ssm_band_width(kernel_size) * sizeof(float);
}

/// @brief Analysis frame count of a 60-minute input at the analysis rate.
int frames_60min() { return 60 * 60 * 22050 / 512; }

}  // namespace

TEST_CASE("Boundary similarity band is bounded by its memory budget, not by the input length",
          "[boundary_detector]") {
  // Only the diagonal band is stored, so the working set is linear in the frame
  // count. Assert the decision directly instead of allocating: for any raw frame
  // count, including absurd ones, the realized band fits the budget.
  const int kernel = BoundaryConfig{}.kernel_size;
  const int target = boundary_pooled_target_frames(kernel);
  const int candidates[] = {
      1, 2, 1000, 46340, frames_60min(), target, target + 1, 100 * frames_60min()};

  for (int raw : candidates) {
    const int frames = boundary_ssm_frames(raw, kernel);
    INFO("raw frames = " << raw);
    REQUIRE(frames >= 1);
    REQUIRE(frames <= target);
    REQUIRE(band_bytes(frames, kernel) <= kBoundarySsmBudgetBytes);
    REQUIRE(boundary_pooling_stride(raw, kernel) >= 1);
  }

  // Inputs that fit the budget keep full per-hop time resolution.
  REQUIRE(boundary_pooling_stride(target, kernel) == 1);
  REQUIRE(boundary_ssm_frames(target, kernel) == target);

  // One frame past the budget pools rather than allocating past it: the backstop
  // is still wired even though nothing realistic reaches it.
  REQUIRE(boundary_pooling_stride(target + 1, kernel) == 2);
  REQUIRE(boundary_ssm_frames(target + 1, kernel) <= target);

  // A wider kernel stores more diagonals per frame, so it pools sooner.
  REQUIRE(boundary_ssm_band_width(128) > boundary_ssm_band_width(kernel));
  REQUIRE(boundary_pooled_target_frames(128) < target);
}

TEST_CASE("Boundary analysis keeps full time resolution for a 60-minute input",
          "[boundary_detector]") {
  // The regression this pins: with the full n_frames x n_frames matrix an hour of
  // audio needed ~96 GB, and capping that by pooling cost half the time resolution
  // for anything over ~3 minutes. Storing only the band the checkerboard kernel
  // reads makes an hour fit in tens of megabytes at stride 1, so the 23 ms
  // boundary grid and the 1.5 s kernel span of the default config are preserved.
  const int kernel = BoundaryConfig{}.kernel_size;

  REQUIRE(boundary_ssm_band_width(kernel) == 127u);  // 2 * (kernel - 1) + 1
  REQUIRE(boundary_pooling_stride(frames_60min(), kernel) == 1);
  REQUIRE(boundary_ssm_frames(frames_60min(), kernel) == frames_60min());
  REQUIRE(band_bytes(frames_60min(), kernel) < 128u * 1024u * 1024u);
  REQUIRE(boundary_pooled_target_frames(kernel) > frames_60min());
}

TEST_CASE("BoundaryDetector leaves normal inputs unpooled", "[boundary_detector]") {
  // The bound must not cost normal-length material any time resolution.
  Audio audio = create_two_sections();

  BoundaryDetector detector(audio);

  const int kernel = BoundaryConfig{}.kernel_size;
  REQUIRE(detector.frame_stride() == 1);
  REQUIRE(detector.n_frames() > 0);
  REQUIRE(detector.n_frames() <= boundary_pooled_target_frames(kernel));
  REQUIRE(band_bytes(detector.n_frames(), kernel) <= kBoundarySsmBudgetBytes);
}

TEST_CASE("BoundaryDetector analyzes long audio at full resolution within the band budget",
          "[boundary][.][slow]") {
  // A frame count that the full n_frames x n_frames matrix could not hold (48k
  // frames is ~9.2 GB square, and ~46340 frames is where its int index used to
  // overflow). Stored as a band it is tens of megabytes, so the detector must
  // handle it end to end without pooling and still return a usable,
  // internally-consistent result (no crash, no throw). Use a small n_fft/hop so
  // the frame count is reached with a modest buffer.
  BoundaryConfig config;
  config.n_fft = 512;
  config.hop_length = 128;
  config.threshold = 0.1f;
  const int sr = 22050;
  // Two distinct halves so the novelty curve still carries structure.
  const size_t half = static_cast<size_t>(24000) * 128;  // each half > 23k frames
  std::vector<float> samples(half * 2, 0.0f);
  for (size_t i = 0; i < half; ++i) {
    const float t = static_cast<float>(i) / static_cast<float>(sr);
    samples[i] = 0.5f * std::sin(2.0f * sonare::constants::kPiD * 220.0f * t);
    samples[half + i] = 0.5f * std::sin(2.0f * sonare::constants::kPiD * 880.0f * t);
  }
  Audio audio = Audio::from_vector(std::move(samples), sr);

  BoundaryDetector detector(audio, config);  // ~48k frames, well inside the band budget

  // No pooling at this length, and the realized band sits inside the budget.
  REQUIRE(detector.frame_stride() == 1);
  REQUIRE(detector.n_frames() > 46340);  // past the old square-matrix index bound
  REQUIRE(detector.n_frames() <= boundary_pooled_target_frames(config.kernel_size));
  REQUIRE(band_bytes(detector.n_frames(), config.kernel_size) <= kBoundarySsmBudgetBytes);

  // The analysis is not silenced: a novelty curve still exists and is normalized
  // to [0, 1], and boundary times stay non-negative, sorted, and within the audio
  // duration (proving the frame-to-time mapping is consistent).
  const auto& novelty = detector.novelty_curve();
  REQUIRE(!novelty.empty());
  for (float v : novelty) {
    REQUIRE(v >= 0.0f);
    REQUIRE(v <= 1.0f);
  }
  const float duration = audio.duration();
  const auto times = detector.boundary_times();
  for (size_t i = 0; i < times.size(); ++i) {
    REQUIRE(times[i] >= 0.0f);
    REQUIRE(times[i] <= duration);
    if (i > 0) REQUIRE(times[i] > times[i - 1]);
  }
}
