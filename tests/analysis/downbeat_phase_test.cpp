/// @file downbeat_phase_test.cpp
/// @brief The bar phase comes from bar-start evidence, and every analyzer reports the same one.

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <cstdint>
#include <limits>
#include <vector>

#include "analysis/analysis_rate.h"
#include "analysis/beat_analyzer.h"
#include "analysis/downbeat_analyzer.h"
#include "analysis/music_analyzer.h"
#include "analysis/rhythm_analyzer.h"
#include "core/spectrum.h"
#include "util/constants.h"

using namespace sonare;

namespace {

constexpr int kSampleRate = 22050;
constexpr int kHop = 512;
constexpr int kNumerator = 4;
constexpr float kBeatSeconds = 0.5f;

/// @brief A 4/4 groove: kick on beat 1, a broadband snare louder than the kick on 2 and 4, a
///        hat on 3, and a triad pad that changes chord on every beat 1.
/// @param total_beats Beats rendered; the first is beat 1 of a bar.
Audio render_groove(int total_beats) {
  const size_t length = static_cast<size_t>((total_beats * kBeatSeconds + 0.5f) * kSampleRate);
  std::vector<float> out(length, 0.0f);
  uint32_t state = 0x5eed1234u;
  auto noise = [&state]() {
    state = state * 1664525u + 1013904223u;
    return static_cast<float>(state >> 8) / static_cast<float>(1u << 24) * 2.0f - 1.0f;
  };
  auto add_noise = [&](size_t start, float amp, float decay) {
    const size_t n = static_cast<size_t>(6.0f * decay * kSampleRate);
    for (size_t j = 0; j < n && start + j < length; ++j) {
      out[start + j] += amp * std::exp(-static_cast<float>(j) / kSampleRate / decay) * noise();
    }
  };
  auto add_kick = [&](size_t start, float amp) {
    constexpr float kDecay = 0.16f;
    const size_t n = static_cast<size_t>(6.0f * kDecay * kSampleRate);
    double phase = 0.0;
    for (size_t j = 0; j < n && start + j < length; ++j) {
      const float t = static_cast<float>(j) / kSampleRate;
      phase += constants::kTwoPiD * (55.0 + 90.0 * std::exp(-t / 0.03)) / kSampleRate;
      out[start + j] += amp * std::exp(-t / kDecay) * static_cast<float>(std::sin(phase));
    }
  };

  for (int k = 0; k < total_beats; ++k) {
    const size_t start = static_cast<size_t>(k * kBeatSeconds * kSampleRate);
    switch (k % kNumerator) {
      case 0:
        add_kick(start, 0.8f);
        break;
      case 1:
      case 3:
        add_noise(start, 0.9f, 0.06f);
        break;
      default:
        add_noise(start, 0.15f, 0.015f);
        break;
    }
  }

  static const float kTriads[4][3] = {{261.63f, 329.63f, 392.00f},
                                      {349.23f, 440.00f, 523.25f},
                                      {293.66f, 349.23f, 440.00f},
                                      {392.00f, 493.88f, 587.33f}};
  const float bar_seconds = kBeatSeconds * kNumerator;
  for (size_t i = 0; i < length; ++i) {
    const float t = static_cast<float>(i) / kSampleRate;
    const float* triad = kTriads[static_cast<int>(t / bar_seconds) % 4];
    for (int voice = 0; voice < 3; ++voice) {
      out[i] += 0.08f * static_cast<float>(std::sin(constants::kTwoPiD * triad[voice] * t));
    }
  }
  return Audio::from_vector(std::move(out), kSampleRate);
}

/// @brief Beats exactly on the rendered hits.
std::vector<Beat> grid_beats(int total_beats) {
  std::vector<Beat> beats;
  for (int k = 0; k < total_beats; ++k) {
    const float time = static_cast<float>(k) * kBeatSeconds;
    beats.push_back({time, static_cast<int>(std::lround(time * kSampleRate / kHop)), 1.0f});
  }
  return beats;
}

DownbeatObservations evidence_for(const Audio& audio, const std::vector<Beat>& beats) {
  const StftConfig stft = stft_config_at_rate(2048, kHop, audio.sample_rate());
  const Spectrogram spec = Spectrogram::compute(audio, stft);
  return downbeat_phase_evidence(spec.power(), spec.n_bins(), spec.n_frames(), audio.sample_rate(),
                                 stft.n_fft, kHop, beats);
}

}  // namespace

TEST_CASE("bar-start evidence peaks on the kick and on the chord change",
          "[downbeat][phase_evidence]") {
  constexpr int kBeats = 16;
  const Audio audio = render_groove(kBeats);
  const std::vector<Beat> beats = grid_beats(kBeats);

  const DownbeatObservations evidence = evidence_for(audio, beats);
  REQUIRE(evidence.lf.size() == beats.size());
  REQUIRE(evidence.harmonic_change.size() == beats.size());
  REQUIRE(evidence.harmonic_change[0] == 0.0f);

  float min_kick_lf = std::numeric_limits<float>::infinity();
  float max_other_lf = 0.0f;
  float min_change_hc = std::numeric_limits<float>::infinity();
  float max_other_hc = 0.0f;
  for (size_t i = 0; i < beats.size(); ++i) {
    const bool bar_start = i % kNumerator == 0;
    if (bar_start) {
      min_kick_lf = std::min(min_kick_lf, evidence.lf[i]);
    } else {
      max_other_lf = std::max(max_other_lf, evidence.lf[i]);
    }
    if (bar_start && i > 0) {
      min_change_hc = std::min(min_change_hc, evidence.harmonic_change[i]);
    } else if (!bar_start) {
      max_other_hc = std::max(max_other_hc, evidence.harmonic_change[i]);
    }
  }
  CAPTURE(evidence.lf, evidence.harmonic_change);
  REQUIRE(min_kick_lf > max_other_lf);
  REQUIRE(min_change_hc > max_other_hc);

  const DownbeatResult result =
      select_downbeat_phase(beats, TimeSignature{kNumerator, 4, 1.0f}, evidence);
  REQUIRE(result.beat_indices == std::vector<int>{0, 4, 8, 12});
}

TEST_CASE("bar-start evidence finds the kick when the clip opens mid-bar",
          "[downbeat][phase_evidence]") {
  // Drop the first beat: the kick now sits on beat index 3 of the clip's own count.
  constexpr int kBeats = 17;
  const Audio audio = render_groove(kBeats);
  std::vector<Beat> beats = grid_beats(kBeats);
  beats.erase(beats.begin());

  const DownbeatResult result =
      select_downbeat_phase(beats, TimeSignature{kNumerator, 4, 1.0f}, evidence_for(audio, beats));
  REQUIRE(result.beat_indices == std::vector<int>{3, 7, 11, 15});
}

TEST_CASE("one dominant off-beat observation keeps the bar phase", "[downbeat][phase_evidence]") {
  std::vector<Beat> beats = grid_beats(16);
  DownbeatObservations evidence;
  const float pattern[4] = {1.0f, 0.3f, 0.5f, 0.3f};
  for (size_t i = 0; i < beats.size(); ++i) {
    evidence.lf.push_back(pattern[i % 4]);
  }
  evidence.lf[5] = 20.0f;

  const DownbeatResult result =
      select_downbeat_phase(beats, TimeSignature{kNumerator, 4, 1.0f}, evidence);
  REQUIRE(result.beat_indices == std::vector<int>{0, 4, 8, 12});
}

TEST_CASE("every analyzer reports one bar phase for the same beats", "[downbeat][one_answer]") {
  const Audio audio = render_groove(20);

  BeatConfig beat_config;
  beat_config.start_bpm = 120.0f;
  beat_config.bpm_min = 60.0f;
  beat_config.bpm_max = 200.0f;
  const BeatAnalyzer direct(audio, beat_config);
  const RhythmAnalyzer rhythm(direct);

  MusicAnalyzerConfig music_config;
  music_config.start_bpm = beat_config.start_bpm;
  music_config.bpm_min = beat_config.bpm_min;
  music_config.bpm_max = beat_config.bpm_max;
  MusicAnalyzer music(audio, music_config);
  const AnalysisResult result = music.analyze();

  // Same beats in, so the comparison below is about the phase alone.
  REQUIRE(result.beats.size() == direct.beats().size());
  for (size_t i = 0; i < result.beats.size(); ++i) {
    CAPTURE(i);
    REQUIRE(result.beats[i].frame == direct.beats()[i].frame);
  }
  REQUIRE(direct.time_signature().numerator == kNumerator);

  CAPTURE(direct.downbeat_phase(), result.downbeat_phase, rhythm.downbeat_phase(),
          direct.downbeat_indices(), result.downbeat_indices);
  REQUIRE(result.downbeat_phase == direct.downbeat_phase());
  REQUIRE(result.downbeat_indices == direct.downbeat_indices());
  REQUIRE(rhythm.downbeat_phase() == direct.downbeat_phase());
  REQUIRE(music.rhythm_analyzer().downbeat_phase() == direct.downbeat_phase());

  // The phase is the kick beat: rendered hits sit on multiples of 0.5 s, kicks on multiples of 2.
  const float first_downbeat = direct.beats()[static_cast<size_t>(direct.downbeat_phase())].time;
  const float bars = first_downbeat / (kBeatSeconds * kNumerator);
  CAPTURE(first_downbeat);
  REQUIRE(std::abs(bars - std::round(bars)) * kBeatSeconds * kNumerator <= 0.05f);
}
