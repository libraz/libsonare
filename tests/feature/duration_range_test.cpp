#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <complex>
#include <utility>
#include <vector>

#include "analysis/chord_analyzer.h"
#include "core/spectrum.h"
#include "feature/chroma.h"
#include "feature/cqt.h"
#include "feature/mel_spectrogram.h"

namespace {
constexpr int kFrames = 2;
constexpr int kHop = 1'500'000'000;
constexpr int kSampleRate = 44100;
constexpr double kExpectedDuration = static_cast<double>(kFrames) * kHop / kSampleRate;
}  // namespace

TEST_CASE("Analysis result durations multiply frame counts without int overflow",
          "[feature][duration][edge]") {
  std::vector<std::complex<float>> spectrum_data(6);
  const auto spectrum = sonare::Spectrogram::from_complex(
      std::move(spectrum_data), 3, kFrames, 4, kHop, kSampleRate, sonare::WindowType::Hann, false);
  REQUIRE(spectrum.duration() == Catch::Approx(kExpectedDuration));

  sonare::MelFilterConfig mel_config;
  mel_config.n_mels = 1;
  mel_config.fmax = static_cast<float>(kSampleRate) / 2.0f;
  const auto mel = sonare::MelSpectrogram::from_spectrogram(spectrum, kSampleRate, mel_config);
  REQUIRE(mel.duration() == Catch::Approx(kExpectedDuration));

  sonare::ChromaFilterConfig chroma_config;
  chroma_config.n_chroma = 12;
  const auto chroma = sonare::Chroma::from_spectrogram(spectrum, kSampleRate, chroma_config);
  REQUIRE(chroma.duration() == Catch::Approx(kExpectedDuration));

  const sonare::CqtResult cqt(std::vector<std::complex<float>>(2), 1, kFrames, {440.0f}, kHop,
                              kSampleRate);
  REQUIRE(cqt.duration() == Catch::Approx(kExpectedDuration));
}

TEST_CASE("Chord analysis reuses overflow-safe chroma duration",
          "[analysis][chord][duration][edge]") {
  const sonare::Chroma chroma(std::vector<float>(12 * kFrames), 12, kFrames, kSampleRate, kHop);
  const sonare::ChordAnalyzer analyzer(chroma);
  REQUIRE(analyzer.chords().size() == 1);
  REQUIRE(analyzer.chords().front().end == Catch::Approx(kExpectedDuration));
}
