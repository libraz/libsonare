/// @file spectrogram_reuse_test.cpp
/// @brief Pins the analyzers that share one STFT across their stages to the results of computing
///        each stage's STFT separately.

#include <algorithm>
#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <utility>
#include <vector>

#include "analysis/boundary_detector.h"
#include "analysis/key_analyzer.h"
#include "analysis/section_analyzer.h"
#include "core/audio.h"
#include "core/spectrum.h"
#include "feature/chroma.h"
#include "feature/mel_spectrogram.h"
#include "util/constants.h"

using namespace sonare;

namespace {

constexpr int kSampleRate = 22050;

/// @brief Three one-second segments of different pitch and brightness, with a click per half
/// second.
Audio make_segmented_signal() {
  const float freqs[3] = {220.0f, 330.0f, 494.0f};
  const int partials[3] = {1, 3, 4};
  std::vector<float> samples(3 * kSampleRate, 0.0f);
  for (size_t i = 0; i < samples.size(); ++i) {
    const int segment = static_cast<int>(i / kSampleRate);
    const float t = static_cast<float>(i) / kSampleRate;
    float v = 0.0f;
    for (int h = 1; h <= partials[segment]; ++h) {
      v += std::sin(constants::kTwoPi * freqs[segment] * static_cast<float>(h) * t) /
           static_cast<float>(h);
    }
    samples[i] = 0.3f * v;
    if (i % (kSampleRate / 2) < 64) samples[i] += 0.5f;
  }
  return Audio::from_vector(std::move(samples), kSampleRate);
}

/// @brief Two seconds of a C major triad with a percussive click train.
Audio make_triad_signal() {
  const float freqs[3] = {261.63f, 329.63f, 392.0f};
  std::vector<float> samples(2 * kSampleRate, 0.0f);
  for (size_t i = 0; i < samples.size(); ++i) {
    const float t = static_cast<float>(i) / kSampleRate;
    float v = 0.0f;
    for (float f : freqs) {
      v += std::sin(constants::kTwoPi * f * t) + 0.4f * std::sin(constants::kTwoPi * 2.0f * f * t);
    }
    samples[i] = 0.15f * v;
    if (i % (kSampleRate / 4) < 32) samples[i] += 0.4f;
  }
  return Audio::from_vector(std::move(samples), kSampleRate);
}

SectionConfig make_section_config() {
  SectionConfig config;
  config.kernel_size = 16;
  config.min_section_sec = 0.3f;
  return config;
}

/// @brief The boundary configuration SectionAnalyzer derives from its own.
BoundaryConfig boundary_config_for(const SectionConfig& config) {
  BoundaryConfig boundary_config;
  boundary_config.n_fft = config.n_fft;
  boundary_config.hop_length = config.hop_length;
  boundary_config.threshold = config.boundary_threshold;
  boundary_config.kernel_size = config.kernel_size;
  boundary_config.peak_distance =
      std::min(std::max(0.0f, config.min_section_sec), BoundaryConfig{}.peak_distance);
  return boundary_config;
}

}  // namespace

TEST_CASE("SectionAnalyzer matches boundaries and an STFT computed separately",
          "[analysis][spectrogram_reuse]") {
  const Audio audio = make_segmented_signal();
  const SectionConfig config = make_section_config();

  // The separate-STFT route: the detector computes its own mel and chroma, and the section
  // analyzer is handed an STFT taken apart from both.
  BoundaryDetector detector(audio, boundary_config_for(config));
  ChromaConfig chroma_config;
  chroma_config.n_fft = config.n_fft;
  chroma_config.hop_length = config.hop_length;
  const Spectrogram spec = Spectrogram::compute(audio, chroma_config.to_stft_config());
  const SectionAnalyzer reference(audio, detector.boundaries(), spec, config);

  const SectionAnalyzer actual(audio, config);

  REQUIRE(reference.count() >= 1);
  REQUIRE(actual.count() == reference.count());
  for (size_t i = 0; i < reference.count(); ++i) {
    const Section& a = actual.sections()[i];
    const Section& r = reference.sections()[i];
    CHECK(a.type == r.type);
    CHECK(a.start == r.start);
    CHECK(a.end == r.end);
    CHECK(a.energy_level == r.energy_level);
    CHECK(a.confidence == r.confidence);
  }
}

TEST_CASE("BoundaryDetector from a shared STFT equals the audio constructor",
          "[analysis][spectrogram_reuse]") {
  const Audio audio = make_segmented_signal();
  const BoundaryConfig config = boundary_config_for(make_section_config());

  const BoundaryDetector reference(audio, config);

  ChromaConfig chroma_config;
  chroma_config.n_fft = config.n_fft;
  chroma_config.hop_length = config.hop_length;
  chroma_config.n_chroma = config.n_chroma;
  MelConfig mel_config;
  mel_config.n_mels = constants::kDefaultNMels;
  const Spectrogram spec = Spectrogram::compute(audio, chroma_config.to_stft_config());
  const MelSpectrogram mel =
      MelSpectrogram::from_spectrogram(spec, kSampleRate, mel_config.to_mel_filter_config());
  const Chroma chroma =
      Chroma::from_spectrogram(spec, kSampleRate, chroma_config.to_chroma_filter_config());
  const BoundaryDetector actual(mel, chroma, kSampleRate, config);

  REQUIRE(actual.boundaries().size() == reference.boundaries().size());
  for (size_t i = 0; i < reference.boundaries().size(); ++i) {
    CHECK(actual.boundaries()[i].time == reference.boundaries()[i].time);
    CHECK(actual.boundaries()[i].frame == reference.boundaries()[i].frame);
    CHECK(actual.boundaries()[i].strength == reference.boundaries()[i].strength);
  }
  CHECK(actual.novelty_curve() == reference.novelty_curve());
}

TEST_CASE("KeyAnalyzer auto path matches the per-candidate chroma means",
          "[analysis][spectrogram_reuse]") {
  const Audio audio = make_triad_signal();

  // Each candidate through the non-auto path computes its own STFTs from the audio.
  struct Candidate {
    bool use_hpss;
    bool loudness_weighted;
    float high_pass_hz;
  };
  const Candidate candidates[] = {
      {false, false, 0.0f}, {true, false, 0.0f},  {false, true, 0.0f},
      {true, true, 0.0f},   {true, false, 60.0f},
  };
  std::vector<std::array<float, 12>> reference_means;
  for (const Candidate& c : candidates) {
    KeyConfig config;
    config.genre_hint = "";
    config.use_hpss = c.use_hpss;
    config.loudness_weighted = c.loudness_weighted;
    config.high_pass_hz = c.high_pass_hz;
    reference_means.push_back(KeyAnalyzer(audio, config).mean_chroma());
  }

  const KeyAnalyzer actual(audio, KeyConfig());

  bool matched = false;
  for (const auto& mean : reference_means) {
    matched = matched || mean == actual.mean_chroma();
  }
  CHECK(matched);
}
