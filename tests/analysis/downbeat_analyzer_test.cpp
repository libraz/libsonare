/// @file downbeat_analyzer_test.cpp
/// @brief Tests for downbeat estimation.

#include "analysis/downbeat_analyzer.h"

#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <vector>

#include "analysis/chord_analyzer.h"
#include "util/constants.h"

using namespace sonare;

namespace {

std::vector<Beat> make_beats(int count) {
  std::vector<Beat> beats;
  beats.reserve(static_cast<size_t>(count));
  for (int i = 0; i < count; ++i) {
    beats.push_back({static_cast<float>(i) * 0.5f, i * 10, i % 4 == 1 ? 1.0f : 0.4f});
  }
  return beats;
}

std::vector<Beat> make_accelerating_beats(int count) {
  std::vector<Beat> beats;
  beats.reserve(static_cast<size_t>(count));
  float time = 0.0f;
  for (int i = 0; i < count; ++i) {
    beats.push_back({time, i * 10, 0.4f});
    const float interval = 0.55f - 0.012f * static_cast<float>(i);
    time += std::max(interval, 0.34f);
  }
  return beats;
}

Audio make_low_frequency_pulse_audio(const std::vector<Beat>& beats, int sample_rate = 8000) {
  const int n_samples = static_cast<int>((beats.back().time + 1.0f) * sample_rate);
  std::vector<float> samples(static_cast<size_t>(n_samples), 0.0f);
  for (size_t beat_index = 0; beat_index < beats.size(); ++beat_index) {
    if (beat_index % 4 != 2) {
      continue;
    }
    const int center = static_cast<int>(std::round(beats[beat_index].time * sample_rate));
    const int length = static_cast<int>(0.06f * sample_rate);
    for (int i = 0; i < length && center + i < n_samples; ++i) {
      const float t = static_cast<float>(i) / static_cast<float>(sample_rate);
      const float envelope = 1.0f - static_cast<float>(i) / static_cast<float>(length);
      samples[static_cast<size_t>(center + i)] +=
          0.8f * envelope *
          std::sin(2.0f * static_cast<float>(sonare::constants::kPiD) * 80.0f * t);
    }
  }
  return Audio::from_vector(std::move(samples), sample_rate);
}

}  // namespace

TEST_CASE("estimate_downbeats returns beats matching meter phase", "[downbeat_analyzer]") {
  const auto beats = make_beats(12);
  const TimeSignature time_signature{4, 4, 0.9f};

  const DownbeatResult result = estimate_downbeats(beats, time_signature, 1);

  REQUIRE(result.beat_indices == std::vector<int>{1, 5, 9});
  REQUIRE(result.downbeats.size() == 3);
  REQUIRE(result.downbeats[0].frame == 10);
  REQUIRE(result.time_signature.numerator == 4);
  REQUIRE(result.confidence == time_signature.confidence);
}

TEST_CASE("select_downbeat_phase follows low-frequency and harmonic-change evidence",
          "[downbeat_analyzer]") {
  const auto beats = make_beats(16);

  DownbeatObservations observations;
  observations.lf.assign(beats.size(), 0.0f);
  observations.harmonic_change.assign(beats.size(), 0.0f);
  for (int index : {2, 6, 10, 14}) {
    observations.lf[static_cast<size_t>(index)] = 1.0f;
    observations.harmonic_change[static_cast<size_t>(index)] = 1.0f;
  }

  const TimeSignature time_signature{4, 4, 0.8f};
  const DownbeatResult result = select_downbeat_phase(beats, time_signature, observations);

  REQUIRE(result.beat_indices == std::vector<int>{2, 6, 10, 14});
  REQUIRE(result.downbeats.size() == 4);
  REQUIRE(result.time_signature.numerator == 4);
  // A clear margin of more than one z unit is full margin.
  REQUIRE(result.confidence == 0.5f * time_signature.confidence + 0.5f);
}

TEST_CASE("select_downbeat_phase ignores the beat strength the onset envelope carries",
          "[downbeat_analyzer]") {
  // make_beats accents position 1 in Beat::strength; with flat evidence nothing marks a bar
  // start, so the tie goes to the earliest position.
  const auto beats = make_beats(16);

  DownbeatObservations observations;
  observations.lf.assign(beats.size(), 0.5f);
  observations.harmonic_change.assign(beats.size(), 0.0f);

  const TimeSignature time_signature{4, 4, 0.8f};
  const DownbeatResult result = select_downbeat_phase(beats, time_signature, observations);

  REQUIRE(result.beat_indices == std::vector<int>{0, 4, 8, 12});
  REQUIRE(result.confidence == 0.5f * time_signature.confidence);
}

TEST_CASE("select_downbeat_phase keeps one evidence answer through gradual tempo change",
          "[downbeat_analyzer]") {
  const auto beats = make_accelerating_beats(16);

  DownbeatObservations observations;
  observations.lf.assign(beats.size(), 0.0f);
  observations.harmonic_change.assign(beats.size(), 0.0f);
  for (int index : {1, 5, 9, 13}) {
    observations.lf[static_cast<size_t>(index)] = 1.0f;
    observations.harmonic_change[static_cast<size_t>(index)] = 1.0f;
  }

  const TimeSignature time_signature{4, 4, 0.7f};
  const DownbeatResult result = select_downbeat_phase(beats, time_signature, observations);

  REQUIRE(result.beat_indices == std::vector<int>{1, 5, 9, 13});
  REQUIRE(result.confidence >= 0.4f);
}

TEST_CASE("select_downbeat_phase answers phase 0 when a position has no beat",
          "[downbeat_analyzer]") {
  // Three beats in 4/4: position 3 holds none.
  const auto beats = make_beats(3);
  DownbeatObservations observations;
  observations.lf = {0.0f, 0.0f, 1.0f};

  const DownbeatResult result =
      select_downbeat_phase(beats, TimeSignature{4, 4, 0.5f}, observations);

  REQUIRE(result.beat_indices == std::vector<int>{0});
}

TEST_CASE("select_downbeat_phase chooses among positions holding a single beat",
          "[downbeat_analyzer]") {
  // Seven beats in 4/4: position 3 holds only beat 3, and the evidence marks position 2.
  const auto beats = make_beats(7);
  DownbeatObservations observations;
  observations.lf = {0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 1.0f};

  const DownbeatResult result =
      select_downbeat_phase(beats, TimeSignature{4, 4, 0.5f}, observations);

  REQUIRE(result.beat_indices == std::vector<int>{2, 6});
}

TEST_CASE("select_downbeat_phase reports only the first beat for a one-beat bar",
          "[downbeat_analyzer]") {
  const auto beats = make_beats(8);
  DownbeatObservations observations;
  observations.lf = {0.0f, 1.0f, 0.0f, 1.0f, 0.0f, 1.0f, 0.0f, 1.0f};

  const DownbeatResult result =
      select_downbeat_phase(beats, TimeSignature{1, 4, 0.5f}, observations);

  REQUIRE(result.beat_indices == std::vector<int>{0});
  REQUIRE(result.confidence == 0.5f);
}

TEST_CASE("chord_change_observations maps chord starts to nearest beats", "[downbeat_analyzer]") {
  const auto beats = make_beats(8);
  const std::vector<Chord> chords = {
      {PitchClass::C, ChordQuality::Major, 0.0f, 1.0f, 1.0f},
      {PitchClass::F, ChordQuality::Major, 1.02f, 2.0f, 1.0f},
      {PitchClass::G, ChordQuality::Major, 2.49f, 3.5f, 1.0f},
  };

  const auto observations = chord_change_observations(beats, chords, 0.08f);

  REQUIRE(observations.size() == beats.size());
  REQUIRE(observations[0] == 1.0f);
  REQUIRE(observations[2] == 1.0f);
  REQUIRE(observations[5] == 1.0f);
}

TEST_CASE("low_frequency_energy_observations extracts beat-level bass energy",
          "[downbeat_analyzer]") {
  const auto beats = make_beats(12);
  const Audio audio = make_low_frequency_pulse_audio(beats);

  const auto observations = low_frequency_energy_observations(beats, audio, 200.0f, 0.12f);

  REQUIRE(observations.size() == beats.size());
  REQUIRE(observations[2] > observations[1] * 5.0f);
  REQUIRE(observations[6] > observations[5] * 5.0f);
  REQUIRE(observations[10] > observations[9] * 5.0f);
}

TEST_CASE("onset_strength_observations aggregates frame-level activation around beats",
          "[downbeat_analyzer]") {
  auto beats = make_beats(4);
  beats[0].frame = 10;
  beats[1].frame = 20;
  beats[2].frame = 30;
  beats[3].frame = 40;

  std::vector<float> onset_strength(64, 0.0f);
  onset_strength[9] = 0.2f;
  onset_strength[10] = 0.3f;
  onset_strength[19] = 0.9f;
  onset_strength[20] = 0.1f;
  onset_strength[31] = 0.6f;
  onset_strength[40] = 0.05f;

  const auto observations = onset_strength_observations(beats, onset_strength, 1000, 10, 0.04f);

  REQUIRE(observations.size() == beats.size());
  REQUIRE(observations[1] > observations[0]);
  REQUIRE(observations[2] > observations[3]);
}
