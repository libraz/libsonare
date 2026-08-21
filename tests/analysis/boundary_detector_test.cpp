/// @file boundary_detector_test.cpp
/// @brief Tests for boundary detector.

#include "analysis/boundary_detector.h"

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>
#include <cmath>
#include <cstddef>
#include <string>
#include <vector>

#include "analysis/section_analyzer.h"
#include "feature/chroma.h"
#include "feature/mel_spectrogram.h"
#include "util/constants.h"
#include "util/exception.h"

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

/// @brief Renders one short pattern back to back with no variation between copies.
/// @param sr Sample rate in Hz.
/// @param loop_seconds Duration of the repeated pattern in seconds.
/// @param repeats Number of consecutive, bit-identical copies.
/// @details Each copy is a four-note arpeggio with a decaying envelope, so the
/// feature sequence fluctuates inside a loop yet never changes from one loop to
/// the next -- the stationary case a boundary detector must not segment.
Audio create_looped_pattern(int sr, float loop_seconds, int repeats) {
  const int loop_samples = static_cast<int>(static_cast<float>(sr) * loop_seconds);
  const float note_seconds = loop_seconds / 4.0f;
  const float notes[] = {220.0f, 277.18f, 329.63f, 440.0f};

  std::vector<float> loop(static_cast<size_t>(loop_samples), 0.0f);
  for (int i = 0; i < loop_samples; ++i) {
    const float t = static_cast<float>(i) / static_cast<float>(sr);
    const int note = std::min(3, static_cast<int>(t / note_seconds));
    const float env = std::exp(-6.0f * (t - static_cast<float>(note) * note_seconds));
    loop[static_cast<size_t>(i)] =
        0.5f * env * std::sin(2.0f * sonare::constants::kPiD * notes[note] * t) +
        0.2f * env * std::sin(2.0f * sonare::constants::kPiD * notes[note] * 2.0f * t);
  }

  std::vector<float> samples;
  samples.reserve(static_cast<size_t>(loop_samples) * static_cast<size_t>(repeats));
  for (int r = 0; r < repeats; ++r) {
    samples.insert(samples.end(), loop.begin(), loop.end());
  }
  return Audio::from_vector(std::move(samples), sr);
}

/// @brief Renders steady white noise from a fixed seed.
/// @param sr Sample rate in Hz.
/// @param seconds Duration in seconds.
/// @details Stationary in content yet varying frame to frame, which makes its
/// self-similarity matrix the noisiest of the stationary fixtures and its
/// checkerboard response the largest -- the case an absolute floor must clear.
Audio create_steady_noise(int sr, float seconds) {
  std::vector<float> samples(static_cast<size_t>(static_cast<float>(sr) * seconds));
  unsigned state = 7u;
  for (float& sample : samples) {
    state = state * 1664525u + 1013904223u;
    sample = 0.4f * (static_cast<float>(state >> 8) / 8388608.0f - 1.0f);
  }
  return Audio::from_vector(std::move(samples), sr);
}

/// @brief Largest absolute difference between two boundary-time vectors.
float max_time_delta(const std::vector<float>& a, const std::vector<float>& b) {
  float worst = 0.0f;
  const size_t n = std::min(a.size(), b.size());
  for (size_t i = 0; i < n; ++i) {
    worst = std::max(worst, std::fabs(a[i] - b[i]));
  }
  return worst;
}

/// @brief One hop of the 22.05 kHz analysis grid, in seconds.
/// @details Boundary times are frame indices scaled by the hop duration, so a
/// hop is the finest difference two runs can express. Resampling 44.1 kHz and
/// 48 kHz to the analysis rate goes through different filter phases, which can
/// move a novelty peak onto the neighbouring frame but no further.
constexpr float kAnalysisHopSeconds = static_cast<float>(constants::kDefaultHopLength) /
                                      static_cast<float>(constants::kDefaultSampleRate);

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

TEST_CASE("BoundaryDetector honours a non-default chroma bin count", "[boundary_detector]") {
  // The chroma bin count has to reach the chromagram computation: the flatten
  // loop reads config.n_chroma bins per frame, so a chromagram built with the
  // 12-bin default cannot serve a configuration asking for more, and the bin
  // index leaves the chromagram's range.
  Audio audio = create_two_sections();

  BoundaryConfig config;
  config.use_mfcc = false;
  config.use_chroma = true;
  config.n_chroma = 24;
  config.threshold = 0.1f;

  REQUIRE_NOTHROW(BoundaryDetector{audio, config});

  BoundaryDetector detector(audio, config);
  REQUIRE(detector.n_frames() > 0);

  const auto& novelty = detector.novelty_curve();
  REQUIRE(novelty.size() == static_cast<size_t>(detector.n_frames()));
  for (float v : novelty) {
    REQUIRE(v >= 0.0f);
    REQUIRE(v <= 1.0f);
  }

  // The 24-bin analysis runs on the same time grid as the 12-bin one, so the two
  // frame counts agree; only the feature dimension differs.
  BoundaryConfig twelve = config;
  twelve.n_chroma = 12;
  BoundaryDetector reference(audio, twelve);
  REQUIRE(detector.n_frames() == reference.n_frames());

  const float duration = audio.duration();
  const auto times = detector.boundary_times();
  for (size_t i = 0; i < times.size(); ++i) {
    REQUIRE(times[i] >= 0.0f);
    REQUIRE(times[i] <= duration);
    if (i > 0) REQUIRE(times[i] > times[i - 1]);
  }
}

TEST_CASE("BoundaryDetector rejects a chromagram whose bin count disagrees with the config",
          "[boundary_detector]") {
  // The audio constructor enforces this structurally by computing the
  // chromagram with config.n_chroma bins. The precomputed constructor takes the
  // chromagram as given, and the flatten loop reads config.n_chroma bins per
  // frame regardless: fewer bins raised Chroma::at's bare SONARE_CHECK, which
  // names neither number, and more bins were silently truncated to the leading
  // config.n_chroma. The bin count is directly queryable, so both numbers are
  // named.
  Audio audio = create_two_sections();

  ChromaConfig chroma_config;
  chroma_config.n_chroma = 36;
  Chroma chroma_36 = Chroma::compute(audio, chroma_config);
  REQUIRE(chroma_36.n_chroma() == 36);

  BoundaryConfig config;
  config.use_mfcc = false;
  config.use_chroma = true;
  config.n_chroma = 12;

  REQUIRE_THROWS_AS(BoundaryDetector(MelSpectrogram(), chroma_36, audio.sample_rate(), config),
                    SonareException);
  // Both numbers, so the caller can see which side to change.
  REQUIRE_THROWS_WITH(
      BoundaryDetector(MelSpectrogram(), chroma_36, audio.sample_rate(), config),
      Catch::Matchers::ContainsSubstring("36") && Catch::Matchers::ContainsSubstring("12"));

  // A matching chromagram is accepted, so the guard rejects the mismatch and
  // not the constructor.
  ChromaConfig matching_config;
  matching_config.n_chroma = 12;
  Chroma chroma_12 = Chroma::compute(audio, matching_config);
  REQUIRE(chroma_12.n_chroma() == 12);
  REQUIRE_NOTHROW(BoundaryDetector(MelSpectrogram(), chroma_12, audio.sample_rate(), config));
}

TEST_CASE("BoundaryDetector tolerates a configuration with no feature dimensions",
          "[boundary_detector]") {
  // Contract pins, not proof of the guard: taking the address of the first
  // element of an empty feature grid never dereferences, so the released output
  // is identical with and without the guard and only a sanitizer run tells them
  // apart. What is asserted here is the documented shape of the degenerate
  // result: the analysis grid is still sized, and an all-zero similarity band
  // yields an all-zero novelty curve and no boundaries.
  Audio audio = create_two_sections();

  ChromaConfig chroma_config;
  Chroma chroma = Chroma::compute(audio, chroma_config);
  REQUIRE(chroma.n_frames() > 0);

  BoundaryConfig config;
  config.use_mfcc = false;
  config.use_chroma = true;
  config.n_chroma = 0;

  // Only the pre-computed constructor can reach this configuration: computing a
  // chromagram with no bins is rejected by the chroma filterbank.
  BoundaryDetector detector(MelSpectrogram(), chroma, audio.sample_rate(), config);

  REQUIRE(detector.n_frames() == chroma.n_frames());
  REQUIRE(detector.count() == 0);
  REQUIRE(detector.boundaries().empty());

  const auto& novelty = detector.novelty_curve();
  REQUIRE(novelty.size() == static_cast<size_t>(detector.n_frames()));
  for (float v : novelty) {
    REQUIRE_THAT(v, WithinAbs(0.0f, 0.0f));
  }
}

TEST_CASE("Boundary detection runs at the analysis rate whatever the source rate",
          "[boundary_detector]") {
  // boundaries() and sections() are two entry points onto one segmentation, and
  // section analysis has always resampled to 22.05 kHz. A detector that adopted
  // the source rate verbatim segmented the same material differently depending
  // on how it was delivered, and disagreed with the sections built on top of it.
  const Audio at_44100 = create_multi_sections(44100, 3.0f);
  const Audio at_48000 = create_multi_sections(48000, 3.0f);

  SectionConfig section_config;
  section_config.min_section_sec = 2.0f;
  section_config.boundary_threshold = 0.2f;

  // The configuration SectionAnalyzer derives for its own detector, so the two
  // entry points are compared on identical settings.
  BoundaryConfig boundary_config;
  boundary_config.n_fft = section_config.n_fft;
  boundary_config.hop_length = section_config.hop_length;
  boundary_config.threshold = section_config.boundary_threshold;
  boundary_config.kernel_size = section_config.kernel_size;
  boundary_config.peak_distance = section_config.min_section_sec;

  const BoundaryDetector detector_44100(at_44100, boundary_config);
  const BoundaryDetector detector_48000(at_48000, boundary_config);

  REQUIRE(detector_44100.sample_rate() == constants::kDefaultSampleRate);
  REQUIRE(detector_48000.sample_rate() == constants::kDefaultSampleRate);
  REQUIRE(detector_44100.n_frames() == detector_48000.n_frames());

  const std::vector<float> boundaries_44100 = detector_44100.boundary_times();
  const std::vector<float> boundaries_48000 = detector_48000.boundary_times();
  const std::vector<float> sections_44100 =
      SectionAnalyzer(at_44100, section_config).boundary_times();
  const std::vector<float> sections_48000 =
      SectionAnalyzer(at_48000, section_config).boundary_times();

  // Anti-vacuity: the material really does change, so agreement on an empty set
  // would not be agreement on a segmentation.
  REQUIRE(!boundaries_44100.empty());
  REQUIRE(boundaries_44100.size() == boundaries_48000.size());
  REQUIRE(boundaries_44100.size() == sections_44100.size());
  REQUIRE(boundaries_44100.size() == sections_48000.size());

  INFO("44.1 vs 48 kHz: " << max_time_delta(boundaries_44100, boundaries_48000));
  REQUIRE(max_time_delta(boundaries_44100, boundaries_48000) <= kAnalysisHopSeconds);
  REQUIRE(max_time_delta(sections_44100, sections_48000) <= kAnalysisHopSeconds);

  // Both entry points read the same grid built from the same resampled signal,
  // so at a matched configuration they agree exactly, not merely within the grid.
  REQUIRE_THAT(max_time_delta(boundaries_44100, sections_44100), WithinAbs(0.0f, 0.0f));
  REQUIRE_THAT(max_time_delta(boundaries_48000, sections_48000), WithinAbs(0.0f, 0.0f));
}

TEST_CASE("A repeated loop is not segmented", "[boundary_detector]") {
  // Self-normalizing the novelty curve rescales whatever fluctuation it finds to
  // full scale, so a relative threshold on its own reports boundaries in material
  // that never changes. Loop-based material is exactly where that happens.
  const Audio audio = create_looped_pattern(22050, 0.5f, 60);  // 30 s, no variation

  const BoundaryDetector detector(audio, BoundaryConfig{});
  INFO("boundaries " << detector.count());
  REQUIRE(detector.count() <= 2);

  const std::string form = SectionAnalyzer(audio).form();
  INFO("form " << form);
  REQUIRE(form.size() >= 1);
  REQUIRE(form.size() <= 2);
}

TEST_CASE("A repeated loop is not segmented over a full track length",
          "[boundary_detector][.][slow]") {
  // The 30 s case above pins the behaviour; this is the track-length version,
  // where a per-few-seconds false boundary accumulated into dozens of sections
  // and a form string of one letter repeated.
  const Audio audio = create_looped_pattern(22050, 0.5f, 720);  // 360 s, no variation

  const BoundaryDetector detector(audio, BoundaryConfig{});
  INFO("boundaries " << detector.count());
  REQUIRE(detector.count() <= 2);

  const std::string form = SectionAnalyzer(audio).form();
  INFO("form " << form);
  REQUIRE(form.size() >= 1);
  REQUIRE(form.size() <= 2);
}

TEST_CASE("Genuine structural change stays detected", "[boundary_detector]") {
  // The counterweight to the stationary case: a floor high enough to silence a
  // loop must still leave the structured fixtures of this file segmented.
  BoundaryConfig config;
  config.threshold = 0.2f;
  config.peak_distance = 1.0f;

  REQUIRE(!detect_boundaries(create_two_sections(22050, 3.0f), config).empty());
  REQUIRE(!detect_boundaries(create_multi_sections(22050, 3.0f), config).empty());
  REQUIRE(!detect_boundaries(create_multi_sections(44100, 3.0f), config).empty());
}

TEST_CASE("The absolute novelty floor separates stationary from structured material",
          "[boundary_detector]") {
  // What justifies the floor is a gap between two measured populations, so this
  // asserts the gap rather than the constant. Raw checkerboard response with the
  // floor disabled -- strongest peak for stationary material, weakest reported
  // boundary for structured material, since that is the pair the floor must fit
  // between:
  //
  //   loop, 0.25 s period    0.00004      four sections, weakest   0.008
  //   loop, 0.5 s period     0.0007       four sections, strongest 0.020
  //   loop, 1 s period       0.0012       two sections             0.049
  //   steady tone            0.0003       noise into a tone        0.95
  //   steady white noise     0.003
  //
  // Steady white noise is the binding stationary case and a weak section change
  // the binding structural one; the default floor of 0.005 is close to their
  // geometric midpoint.
  //
  // Level-only structure lands inside the stationary population rather than above
  // it. Not because the features ignore level -- the first MFCC coefficient
  // tracks it, and per-frame normalization takes the vector's length but not that
  // coefficient's ratio to the rest -- but because the resulting turn is roughly
  // five times smaller than a comparable change of pitch, which leaves it quieter
  // than steady noise. No floor separates the two, so lowering this one admits
  // noise before it admits level structure.
  BoundaryConfig ungated;
  ungated.absolute_threshold = 0.0f;

  const auto raw_peak = [&ungated](const Audio& audio) {
    const BoundaryDetector detector(audio, ungated);
    float worst = 0.0f;
    for (float value : detector.novelty_curve()) {
      worst = std::max(worst, value * detector.novelty_peak());
    }
    return worst;
  };
  const auto weakest_boundary = [&ungated](const Audio& audio) {
    const BoundaryDetector detector(audio, ungated);
    float weakest = 0.0f;
    for (const auto& boundary : detector.boundaries()) {
      const float absolute = boundary.strength * detector.novelty_peak();
      weakest = (weakest == 0.0f) ? absolute : std::min(weakest, absolute);
    }
    return weakest;
  };

  const float floor = BoundaryConfig{}.absolute_threshold;
  REQUIRE(floor > 0.0f);

  // Every stationary fixture stays under the floor, including steady noise, whose
  // frame-to-frame variation is what makes it the worst of them.
  REQUIRE(raw_peak(create_looped_pattern(22050, 0.5f, 60)) < floor);
  REQUIRE(raw_peak(create_sine(440.0f, 22050, 10.0f)) < floor);
  REQUIRE(raw_peak(create_steady_noise(22050, 30.0f)) < floor);

  // A real section change clears it -- every one of them, not just the strongest.
  REQUIRE(weakest_boundary(create_multi_sections(22050, 3.0f)) > floor);
  REQUIRE(raw_peak(create_two_sections(22050, 3.0f)) > floor * 4.0f);
}

TEST_CASE("A loop that does change is still segmented at the change", "[boundary_detector]") {
  // The floor must not silence a boundary just because the material around it
  // repeats. Half of this signal is the stationary loop that must yield nothing;
  // the other half is a steady tone unrelated to it, so exactly one boundary is
  // genuine, and it is the only one that may be reported.
  const Audio loop = create_looped_pattern(22050, 0.5f, 60);
  std::vector<float> samples(loop.begin(), loop.end());
  const float change_time = 0.5f * static_cast<float>(samples.size()) / 22050.0f;
  for (size_t i = samples.size() / 2; i < samples.size(); ++i) {
    const float t = static_cast<float>(i) / 22050.0f;
    samples[i] = 0.5f * std::sin(2.0f * sonare::constants::kPiD * 1200.0f * t);
  }

  const BoundaryDetector detector(Audio::from_vector(std::move(samples), 22050), BoundaryConfig{});

  REQUIRE(detector.count() == 1);
  REQUIRE_THAT(detector.boundary_times()[0], WithinAbs(change_time, 1.0f));
}
