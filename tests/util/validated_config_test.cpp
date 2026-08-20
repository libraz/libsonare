/// @file validated_config_test.cpp
/// @brief Construction-time validation of the shared configuration structs.
///
/// These assert the guarantee at the layer that owns it: a rejected field never
/// reaches the DSP, whichever surface built the config. The per-binding tests
/// only confirm that the surface reports the resulting error.

#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <limits>
#include <vector>

#include "analysis/music_analyzer.h"
#include "core/audio.h"
#include "core/spectrum.h"
#include "effects/decompose.h"
#include "util/constants.h"
#include "util/exception.h"
#include "util/validated.h"

using namespace sonare;
using sonare::constants::kTwoPi;

namespace {

Audio make_tone(float seconds = 0.25f, int sample_rate = 22050) {
  const size_t n = static_cast<size_t>(seconds * static_cast<float>(sample_rate));
  std::vector<float> samples(n);
  for (size_t i = 0; i < n; ++i) {
    samples[i] =
        0.25f * std::sin(kTwoPi * 440.0f * static_cast<float>(i) / static_cast<float>(sample_rate));
  }
  return Audio::from_vector(std::move(samples), sample_rate);
}

void construct_analyzer(const Audio& audio, const MusicAnalyzerConfig& config) {
  MusicAnalyzer analyzer(audio, config);
  (void)analyzer;
}

}  // namespace

TEST_CASE("Validated<MusicAnalyzerConfig> accepts the defaults", "[util][validated]") {
  REQUIRE_NOTHROW(Validated<MusicAnalyzerConfig>::make(MusicAnalyzerConfig()));
}

TEST_CASE("Validated<MusicAnalyzerConfig> rejects out-of-range fields", "[util][validated]") {
  const auto rejects = [](void (*mutate)(MusicAnalyzerConfig&)) {
    MusicAnalyzerConfig config;
    mutate(config);
    REQUIRE_THROWS_AS(Validated<MusicAnalyzerConfig>::make(config), SonareException);
  };

  rejects([](MusicAnalyzerConfig& c) { c.bpm_min = 0.0f; });
  rejects([](MusicAnalyzerConfig& c) { c.bpm_max = c.bpm_min - 1.0f; });
  rejects([](MusicAnalyzerConfig& c) { c.chroma_highpass_hz = -1.0f; });
  rejects([](MusicAnalyzerConfig& c) { c.chord_hmm_beam_width = 0; });
  rejects([](MusicAnalyzerConfig& c) { c.n_fft = 2047; });
  rejects([](MusicAnalyzerConfig& c) { c.n_fft = 0; });
  rejects([](MusicAnalyzerConfig& c) { c.hop_length = 0; });
  rejects([](MusicAnalyzerConfig& c) { c.start_bpm = 0.0f; });
  rejects([](MusicAnalyzerConfig& c) { c.chroma_hop_multiplier = 0; });
  rejects([](MusicAnalyzerConfig& c) { c.bpm_min = std::nanf(""); });
  rejects([](MusicAnalyzerConfig& c) {
    c.chroma_highpass_hz = std::numeric_limits<float>::infinity();
  });
}

TEST_CASE("Validated<MusicAnalyzerConfig> rejects unusable meter and tempo-tracking fields",
          "[util][validated]") {
  const auto rejects = [](void (*mutate)(MusicAnalyzerConfig&)) {
    MusicAnalyzerConfig config;
    mutate(config);
    REQUIRE_THROWS_AS(Validated<MusicAnalyzerConfig>::make(config), SonareException);
  };
  const auto accepts = [](void (*mutate)(MusicAnalyzerConfig&)) {
    MusicAnalyzerConfig config;
    mutate(config);
    REQUIRE_NOTHROW(Validated<MusicAnalyzerConfig>::make(config));
  };

  rejects([](MusicAnalyzerConfig& c) { c.tempo_update_interval_beats = 0; });
  rejects([](MusicAnalyzerConfig& c) { c.tempo_update_interval_beats = -1; });
  // An empty candidate list would silently degrade to a fixed low-confidence
  // 4/4 that reads like a detection result, so it has to be rejected.
  rejects([](MusicAnalyzerConfig& c) { c.meter_candidate_numerators.clear(); });
  rejects([](MusicAnalyzerConfig& c) {
    c.meter_candidate_numerators.assign(kMaxMeterCandidateNumerators + 1, 4);
  });
  rejects([](MusicAnalyzerConfig& c) {
    c.meter_candidate_numerators = {kMinMeterCandidateNumerator - 1};
  });
  rejects([](MusicAnalyzerConfig& c) {
    c.meter_candidate_numerators = {kMaxMeterCandidateNumerator + 1};
  });
  // Every entry is checked, not just the first.
  rejects([](MusicAnalyzerConfig& c) { c.meter_candidate_numerators = {4, 1}; });
  rejects([](MusicAnalyzerConfig& c) { c.meter_candidate_numerators = {4, -3}; });
  // Only a power of two is a note value.
  rejects([](MusicAnalyzerConfig& c) { c.meter_denominator = 3; });
  rejects([](MusicAnalyzerConfig& c) { c.meter_denominator = 6; });
  rejects([](MusicAnalyzerConfig& c) { c.meter_denominator = 0; });
  rejects([](MusicAnalyzerConfig& c) { c.meter_denominator = -4; });
  rejects([](MusicAnalyzerConfig& c) { c.meter_denominator = kMaxMeterDenominator * 2; });

  // The inclusive ends of each range stay accepted, so the guards cannot be
  // tightened past the documented contract without a red test.
  accepts([](MusicAnalyzerConfig& c) { c.tempo_update_interval_beats = 1; });
  accepts([](MusicAnalyzerConfig& c) {
    c.meter_candidate_numerators.assign(kMaxMeterCandidateNumerators, 4);
  });
  accepts([](MusicAnalyzerConfig& c) {
    c.meter_candidate_numerators = {kMinMeterCandidateNumerator, kMaxMeterCandidateNumerator};
  });
  accepts([](MusicAnalyzerConfig& c) { c.meter_denominator = 1; });
  accepts([](MusicAnalyzerConfig& c) { c.meter_denominator = kMaxMeterDenominator; });
}

TEST_CASE("MusicAnalyzer construction rejects an invalid config", "[util][validated]") {
  const Audio audio = make_tone();
  MusicAnalyzerConfig config;
  config.bpm_min = 0.0f;
  REQUIRE_THROWS_AS(construct_analyzer(audio, config), SonareException);

  config = MusicAnalyzerConfig();
  config.chord_hmm_beam_width = 0;
  REQUIRE_THROWS_AS(construct_analyzer(audio, config), SonareException);

  config = MusicAnalyzerConfig();
  config.meter_candidate_numerators.clear();
  REQUIRE_THROWS_AS(construct_analyzer(audio, config), SonareException);

  config = MusicAnalyzerConfig();
  config.meter_denominator = 3;
  REQUIRE_THROWS_AS(construct_analyzer(audio, config), SonareException);

  REQUIRE_NOTHROW(construct_analyzer(audio, MusicAnalyzerConfig()));
}

TEST_CASE("Validated<StftConfig> rejects unusable STFT sizes", "[util][validated]") {
  REQUIRE_NOTHROW(Validated<StftConfig>::make(StftConfig()));

  StftConfig config;
  config.n_fft = 0;
  REQUIRE_THROWS_AS(Validated<StftConfig>::make(config), SonareException);

  config = StftConfig();
  config.hop_length = 0;
  REQUIRE_THROWS_AS(Validated<StftConfig>::make(config), SonareException);

  config = StftConfig();
  config.win_length = -1;
  REQUIRE_THROWS_AS(Validated<StftConfig>::make(config), SonareException);

  config = StftConfig();
  config.win_length = config.n_fft + 1;
  REQUIRE_THROWS_AS(Validated<StftConfig>::make(config), SonareException);
}

TEST_CASE("Spectrogram::compute validates the config before the empty-audio shortcut",
          "[util][validated]") {
  StftConfig config;
  config.hop_length = 0;
  REQUIRE_THROWS_AS(Spectrogram::compute(Audio(), config), SonareException);
  REQUIRE_THROWS_AS(Spectrogram::compute(make_tone(), config), SonareException);
}

TEST_CASE("Validated<DecomposeStemsConfig> rejects out-of-range fields", "[util][validated]") {
  REQUIRE_NOTHROW(Validated<DecomposeStemsConfig>::make(DecomposeStemsConfig()));

  const auto rejects = [](void (*mutate)(DecomposeStemsConfig&)) {
    DecomposeStemsConfig config;
    mutate(config);
    REQUIRE_THROWS_AS(Validated<DecomposeStemsConfig>::make(config), SonareException);
  };

  rejects([](DecomposeStemsConfig& c) { c.n_components = 0; });
  rejects([](DecomposeStemsConfig& c) { c.n_fft = 0; });
  rejects([](DecomposeStemsConfig& c) { c.hop_length = 0; });
  rejects([](DecomposeStemsConfig& c) { c.n_iter = 0; });
  rejects([](DecomposeStemsConfig& c) { c.beta = std::nanf(""); });
  rejects([](DecomposeStemsConfig& c) { c.mask_power = 0.5f; });
}

TEST_CASE("decompose_stems rejects an invalid config before any DSP", "[util][validated]") {
  const Audio audio = make_tone();
  DecomposeStemsConfig config;
  config.mask_power = 0.5f;
  REQUIRE_THROWS_AS(decompose_stems(audio.data(), audio.size(), audio.sample_rate(), config),
                    SonareException);
}
