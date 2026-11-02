/// @file spectrogram_reuse_test.cpp
/// @brief Pins the analyzers that share one STFT across their stages to the results of computing
///        each stage's STFT separately.

#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <utility>
#include <vector>

#include "analysis/beat_analyzer.h"
#include "analysis/boundary_detector.h"
#include "analysis/section_analyzer.h"
#include "analysis/section_structure.h"
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

SectionConfig make_section_config() {
  SectionConfig config;
  config.min_section_sec = 0.3f;
  return config;
}

/// @brief A short-kernel detector configuration on the section framing.
BoundaryConfig make_boundary_config() {
  const SectionConfig section_config = make_section_config();
  BoundaryConfig boundary_config;
  boundary_config.n_fft = section_config.n_fft;
  boundary_config.hop_length = section_config.hop_length;
  boundary_config.threshold = section_config.boundary_threshold;
  boundary_config.kernel_size = 16;
  boundary_config.peak_distance = section_config.min_section_sec;
  return boundary_config;
}

}  // namespace

TEST_CASE("SectionAnalyzer matches a beat grid and an STFT computed separately",
          "[analysis][spectrogram_reuse]") {
  const Audio audio = make_segmented_signal();
  const SectionConfig config = make_section_config();

  // The separate route: the beats are tracked on their own, and the section analyzer is handed
  // an STFT taken apart from them.
  BeatConfig beat_config;
  beat_config.n_fft = config.n_fft;
  beat_config.hop_length = config.hop_length;
  const SectionGrid grid = section_structure::make_section_grid(BeatAnalyzer(audio, beat_config));
  ChromaConfig chroma_config;
  chroma_config.n_fft = config.n_fft;
  chroma_config.hop_length = config.hop_length;
  const Spectrogram spec = Spectrogram::compute(audio, chroma_config.to_stft_config());
  const SectionAnalyzer reference(audio, grid, spec, config);

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
    CHECK(a.group == r.group);
  }
  CHECK(actual.section_self_similarity() == reference.section_self_similarity());
}

TEST_CASE("BoundaryDetector from a shared STFT equals the audio constructor",
          "[analysis][spectrogram_reuse]") {
  const Audio audio = make_segmented_signal();
  const BoundaryConfig config = make_boundary_config();

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
