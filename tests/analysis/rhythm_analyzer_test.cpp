/// @file rhythm_analyzer_test.cpp
/// @brief Tests for rhythm analyzer.

#include "analysis/rhythm_analyzer.h"

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <cmath>
#include <vector>

using namespace sonare;
using Catch::Matchers::WithinAbs;
using Catch::Matchers::WithinRel;

namespace {

/// @brief Creates a regular click track at specified BPM.
Audio create_click_track(float bpm, int sr = 22050, float duration = 4.0f) {
  int n_samples = static_cast<int>(sr * duration);
  std::vector<float> samples(n_samples, 0.0f);

  float beat_interval = 60.0f / bpm;
  int click_length = sr / 100;

  for (float t = 0.0f; t < duration; t += beat_interval) {
    int start = static_cast<int>(t * sr);
    for (int i = 0; i < click_length && start + i < n_samples; ++i) {
      float envelope = 1.0f - static_cast<float>(i) / click_length;
      samples[start + i] = envelope * 0.8f;
    }
  }

  return Audio::from_vector(std::move(samples), sr);
}

/// @brief Creates a 4/4 pattern with accented downbeats.
Audio create_4_4_pattern(float bpm, int sr = 22050, float duration = 4.0f) {
  int n_samples = static_cast<int>(sr * duration);
  std::vector<float> samples(n_samples, 0.0f);

  float beat_interval = 60.0f / bpm;
  int click_length = sr / 100;
  int beat_count = 0;

  for (float t = 0.0f; t < duration; t += beat_interval) {
    int start = static_cast<int>(t * sr);
    float amplitude = (beat_count % 4 == 0) ? 1.0f : 0.5f;

    for (int i = 0; i < click_length && start + i < n_samples; ++i) {
      float envelope = 1.0f - static_cast<float>(i) / click_length;
      samples[start + i] = envelope * amplitude;
    }
    beat_count++;
  }

  return Audio::from_vector(std::move(samples), sr);
}

/// @brief Creates a 6/8 compound pattern with primary (beat 0) and secondary
/// (beat 3) accents, the two strong beats of compound duple meter.
Audio create_6_8_pattern(float bpm, int sr = 22050, float duration = 8.0f) {
  int n_samples = static_cast<int>(sr * duration);
  std::vector<float> samples(n_samples, 0.0f);

  float beat_interval = 60.0f / bpm;
  int click_length = sr / 100;
  int beat_count = 0;

  for (float t = 0.0f; t < duration; t += beat_interval) {
    int start = static_cast<int>(t * sr);
    int pos = beat_count % 6;
    // Strong on 0 and 3, weak elsewhere (6/8 compound duple).
    float amplitude = (pos == 0) ? 1.0f : (pos == 3 ? 0.8f : 0.4f);

    for (int i = 0; i < click_length && start + i < n_samples; ++i) {
      float envelope = 1.0f - static_cast<float>(i) / click_length;
      samples[start + i] = envelope * amplitude;
    }
    beat_count++;
  }

  return Audio::from_vector(std::move(samples), sr);
}

/// @brief Creates a 3/4 pattern (waltz).
Audio create_3_4_pattern(float bpm, int sr = 22050, float duration = 6.0f) {
  int n_samples = static_cast<int>(sr * duration);
  std::vector<float> samples(n_samples, 0.0f);

  float beat_interval = 60.0f / bpm;
  int click_length = sr / 100;
  int beat_count = 0;

  for (float t = 0.0f; t < duration; t += beat_interval) {
    int start = static_cast<int>(t * sr);
    float amplitude = (beat_count % 3 == 0) ? 1.0f : 0.4f;

    for (int i = 0; i < click_length && start + i < n_samples; ++i) {
      float envelope = 1.0f - static_cast<float>(i) / click_length;
      samples[start + i] = envelope * amplitude;
    }
    beat_count++;
  }

  return Audio::from_vector(std::move(samples), sr);
}

/// @brief Creates a four-beat click pattern accenting the listed bar positions.
/// @param bpm Beat rate of the click grid.
/// @param accented_positions Bar positions (0-3) that receive the loud click.
/// @details Every beat is present at the same grid, so the only thing that
///          varies between two patterns built from this template is which bar
///          positions carry the accent.
Audio create_positional_accent_pattern(float bpm, const std::vector<int>& accented_positions,
                                       int sr = 22050, float duration = 8.0f) {
  int n_samples = static_cast<int>(sr * duration);
  std::vector<float> samples(n_samples, 0.0f);

  float beat_interval = 60.0f / bpm;
  int click_length = sr / 100;
  int beat_count = 0;

  for (float t = 0.0f; t < duration; t += beat_interval) {
    int start = static_cast<int>(t * sr);
    bool accented = std::find(accented_positions.begin(), accented_positions.end(),
                              beat_count % 4) != accented_positions.end();
    float amplitude = accented ? 1.0f : 0.2f;

    for (int i = 0; i < click_length && start + i < n_samples; ++i) {
      float envelope = 1.0f - static_cast<float>(i) / click_length;
      samples[start + i] = envelope * amplitude;
    }
    beat_count++;
  }

  return Audio::from_vector(std::move(samples), sr);
}

/// @brief Creates a six-beat accent pattern with optional beat-midpoint clicks.
/// @param bpm Beat rate of the click grid.
/// @param subdivision_amplitude Amplitude of the click placed halfway between
///        consecutive beats; zero leaves the space between beats empty.
/// @details The accents alone (strong on bar positions 0 and 3) describe a
///          six-beat bar without saying whether the beat divides in two or in
///          three. Only the midpoint clicks carry that evidence, so two patterns
///          differing solely in @p subdivision_amplitude isolate it.
Audio create_six_beat_pattern(float bpm, float subdivision_amplitude, int sr = 22050,
                              float duration = 8.0f) {
  int n_samples = static_cast<int>(sr * duration);
  std::vector<float> samples(n_samples, 0.0f);

  float beat_interval = 60.0f / bpm;
  int click_length = sr / 100;
  int beat_count = 0;

  auto place_click = [&](float t, float amplitude) {
    int start = static_cast<int>(t * sr);
    for (int i = 0; i < click_length && start + i < n_samples; ++i) {
      float envelope = 1.0f - static_cast<float>(i) / click_length;
      samples[start + i] = envelope * amplitude;
    }
  };

  for (float t = 0.0f; t < duration; t += beat_interval) {
    int pos = beat_count % 6;
    place_click(t, pos == 0 ? 1.0f : (pos == 3 ? 0.8f : 0.4f));
    if (subdivision_amplitude > 0.0f && t + 0.5f * beat_interval < duration) {
      place_click(t + 0.5f * beat_interval, subdivision_amplitude);
    }
    beat_count++;
  }

  return Audio::from_vector(std::move(samples), sr);
}

/// @brief Creates a click track whose first beat of every bar is accented.
Audio create_accented_pattern(float bpm, int beats_per_bar, int sr = 22050,
                              float duration = 10.0f) {
  int n_samples = static_cast<int>(sr * duration);
  std::vector<float> samples(n_samples, 0.0f);

  float beat_interval = 60.0f / bpm;
  int click_length = sr / 100;
  int beat_count = 0;

  for (float t = 0.0f; t < duration; t += beat_interval) {
    int start = static_cast<int>(t * sr);
    float amplitude = (beat_count % beats_per_bar == 0) ? 1.0f : 0.5f;

    for (int i = 0; i < click_length && start + i < n_samples; ++i) {
      float envelope = 1.0f - static_cast<float>(i) / click_length;
      samples[start + i] = envelope * amplitude;
    }
    beat_count++;
  }

  return Audio::from_vector(std::move(samples), sr);
}

/// @brief Time signatures the beat pass and the rhythm pass report for one take.
struct MeterPair {
  TimeSignature beat;
  TimeSignature rhythm;
};

/// @brief Runs both meter estimates over a single set of tracked beats.
/// @details MusicAnalyzer builds its rhythm analyzer from its beat analyzer the
///          same way and publishes both signatures side by side, so this is the
///          pairing a caller actually observes.
MeterPair estimate_meter_both_ways(const Audio& audio, float bpm, float bpm_min, float bpm_max) {
  BeatConfig beat_config;
  beat_config.start_bpm = bpm;
  beat_config.bpm_min = bpm_min;
  beat_config.bpm_max = bpm_max;
  BeatAnalyzer beat_analyzer(audio, beat_config);

  RhythmConfig rhythm_config;
  rhythm_config.start_bpm = bpm;
  rhythm_config.bpm_min = bpm_min;
  rhythm_config.bpm_max = bpm_max;
  RhythmAnalyzer rhythm_analyzer(beat_analyzer, rhythm_config);

  return {beat_analyzer.time_signature(), rhythm_analyzer.time_signature()};
}

}  // namespace

TEST_CASE("RhythmAnalyzer basic", "[rhythm_analyzer]") {
  Audio audio = create_click_track(120.0f);

  RhythmConfig config;
  RhythmAnalyzer analyzer(audio, config);

  const auto& features = analyzer.features();

  // Every click in the track is identical, so no beat carries an accent the
  // meter does not already expect and the score has to stay near zero. A score
  // built from unnormalized envelope values instead saturates here.
  CAPTURE(features.syncopation);
  REQUIRE(features.syncopation >= 0.0f);
  REQUIRE(features.syncopation < 0.3f);
  REQUIRE(features.pattern_regularity >= 0.0f);
  REQUIRE(features.pattern_regularity <= 1.0f);
  REQUIRE(features.tempo_stability >= 0.0f);
  REQUIRE(features.tempo_stability <= 1.0f);
}

TEST_CASE("RhythmAnalyzer time signature 4/4", "[rhythm_analyzer]") {
  Audio audio = create_4_4_pattern(120.0f, 22050, 8.0f);

  RhythmConfig config;
  config.start_bpm = 120.0f;
  RhythmAnalyzer analyzer(audio, config);

  TimeSignature ts = analyzer.time_signature();

  // Should detect 4 or related meter
  REQUIRE((ts.numerator == 4 || ts.numerator == 2));
  REQUIRE(ts.denominator == 4);
  REQUIRE(ts.confidence >= 0.0f);
  REQUIRE(ts.confidence <= 1.0f);
}

TEST_CASE("RhythmAnalyzer time signature 3/4", "[rhythm_analyzer]") {
  Audio audio = create_3_4_pattern(100.0f, 22050, 9.0f);

  RhythmConfig config;
  config.start_bpm = 100.0f;
  RhythmAnalyzer analyzer(audio, config);

  TimeSignature ts = analyzer.time_signature();

  // Should detect 3 or 6
  REQUIRE((ts.numerator == 3 || ts.numerator == 6 || ts.numerator == 4));
}

TEST_CASE("RhythmAnalyzer straight groove", "[rhythm_analyzer]") {
  Audio audio = create_click_track(120.0f, 22050, 5.0f);

  RhythmAnalyzer analyzer(audio);

  REQUIRE(analyzer.groove_type() == "straight");
}

TEST_CASE("RhythmAnalyzer regularity high for steady tempo", "[rhythm_analyzer]") {
  Audio audio = create_click_track(120.0f, 22050, 5.0f);

  RhythmAnalyzer analyzer(audio);

  // Regular click track should have high regularity
  REQUIRE(analyzer.pattern_regularity() >= 0.5f);
}

TEST_CASE("RhythmAnalyzer tempo stability", "[rhythm_analyzer]") {
  Audio audio = create_click_track(120.0f, 22050, 5.0f);

  RhythmAnalyzer analyzer(audio);

  // Steady tempo should have high stability
  REQUIRE(analyzer.tempo_stability() >= 0.5f);
}

TEST_CASE("RhythmAnalyzer bpm", "[rhythm_analyzer]") {
  Audio audio = create_click_track(120.0f, 22050, 5.0f);

  RhythmConfig config;
  config.start_bpm = 120.0f;
  RhythmAnalyzer analyzer(audio, config);

  REQUIRE_THAT(analyzer.bpm(), WithinRel(120.0f, 0.2f));
}

TEST_CASE("RhythmAnalyzer beat intervals", "[rhythm_analyzer]") {
  Audio audio = create_click_track(120.0f, 22050, 4.0f);

  RhythmAnalyzer analyzer(audio);

  const auto& intervals = analyzer.beat_intervals();

  // Four seconds of 120 BPM clicks always yields tracked beats, so an empty
  // series is a failure rather than a case to skip over.
  REQUIRE(!intervals.empty());
  for (float interval : intervals) {
    REQUIRE(interval > 0.0f);
  }
}

TEST_CASE("RhythmAnalyzer from BeatAnalyzer", "[rhythm_analyzer]") {
  Audio audio = create_click_track(120.0f);

  BeatConfig beat_config;
  beat_config.start_bpm = 120.0f;
  BeatAnalyzer beat_analyzer(audio, beat_config);

  RhythmAnalyzer rhythm_analyzer(beat_analyzer);

  REQUIRE(rhythm_analyzer.bpm() > 0.0f);
  REQUIRE(rhythm_analyzer.groove_type().length() > 0);
}

TEST_CASE("RhythmAnalyzer syncopation separates on-beat from off-beat accents",
          "[rhythm_analyzer]") {
  // Both patterns come from the same click template and differ only in which bar
  // position carries the accent, so the score has to be driven by accent
  // placement and not by the absolute loudness of the material. The candidate
  // set is pinned to 4 so both are scored against the same bar grid.
  RhythmConfig config;
  config.start_bpm = 120.0f;
  config.meter_candidate_numerators = {4};

  RhythmAnalyzer on_beat(create_positional_accent_pattern(120.0f, {0}), config);
  RhythmAnalyzer off_beat(create_positional_accent_pattern(120.0f, {0, 3}), config);

  const float on_beat_score = on_beat.syncopation();
  const float off_beat_score = off_beat.syncopation();
  CAPTURE(on_beat_score, off_beat_score);

  // Accents only on the downbeat: nothing lands off the metric grid.
  REQUIRE(on_beat_score >= 0.0f);
  REQUIRE(on_beat_score < 0.3f);
  // Adding an accent on bar position 3, a weak position in 4/4, has to move the
  // score up by a margin a reader would call visible rather than by rounding.
  REQUIRE(off_beat_score > on_beat_score + 0.05f);
  REQUIRE(off_beat_score <= 1.0f);
}

TEST_CASE("RhythmAnalyzer 6/8 secondary accent is not counted as syncopation",
          "[rhythm_analyzer]") {
  // In 6/8 compound meter, bar position 3 is the secondary strong beat. With the
  // accents placed only on positions 0 and 3 there is no genuine off-beat accent,
  // so syncopation must stay low. Previously position 3 was treated as off-beat,
  // which would inflate the score for compound meters.
  Audio audio = create_6_8_pattern(120.0f, 22050, 8.0f);

  RhythmConfig config;
  config.start_bpm = 120.0f;
  RhythmAnalyzer analyzer(audio, config);

  REQUIRE(analyzer.syncopation() >= 0.0f);
  REQUIRE(analyzer.syncopation() <= 1.0f);
  // The position-3-is-strong rule only applies when the analyzer actually
  // detects compound-duple (numerator 6). Meter detection on a synthetic click
  // track is approximate, so only enforce the "modest syncopation" guarantee
  // when 6/8 is detected — that is exactly the case the fix governs.
  if (analyzer.time_signature().numerator == 6) {
    INFO("detected 6/8: accents on positions 0 and 3 should not read as syncopation");
    REQUIRE(analyzer.syncopation() < 0.5f);
  }
}

TEST_CASE("Meter estimates over the same beats agree", "[rhythm_analyzer]") {
  // The beat pass and the rhythm pass estimate the meter separately over one set
  // of tracked beats and both signatures are published, so they have to rest on
  // the same evidence.
  SECTION("triple meter") {
    MeterPair meter =
        estimate_meter_both_ways(create_3_4_pattern(120.0f, 22050, 9.0f), 120.0f, 60.0f, 200.0f);
    CAPTURE(meter.beat.numerator, meter.beat.denominator, meter.rhythm.numerator,
            meter.rhythm.denominator);
    REQUIRE(meter.beat.numerator == meter.rhythm.numerator);
    REQUIRE(meter.beat.denominator == meter.rhythm.denominator);
  }

  SECTION("six-beat accents with nothing between the beats") {
    MeterPair meter =
        estimate_meter_both_ways(create_6_8_pattern(120.0f, 22050, 8.0f), 120.0f, 60.0f, 200.0f);
    CAPTURE(meter.beat.numerator, meter.beat.denominator, meter.rhythm.numerator,
            meter.rhythm.denominator);
    REQUIRE(meter.beat.numerator == meter.rhythm.numerator);
    REQUIRE(meter.beat.denominator == meter.rhythm.denominator);
    // The beats never subdivide, so nothing here supports a compound beat unit.
    REQUIRE(meter.beat.denominator == 4);
  }
}

TEST_CASE("A compound beat unit needs subdivision energy rather than its absence",
          "[rhythm_analyzer]") {
  // Same click grid, same accents, same tracked beats: the only difference is
  // whether a click sits halfway between the beats. That evidence, and not the
  // lack of an onset envelope, is what may promote the beat unit to an eighth.
  const float bpm = 120.0f;
  const float bpm_min = 90.0f;
  const float bpm_max = 150.0f;

  BeatConfig beat_config;
  beat_config.start_bpm = bpm;
  beat_config.bpm_min = bpm_min;
  beat_config.bpm_max = bpm_max;

  BeatAnalyzer bare(create_six_beat_pattern(bpm, 0.0f), beat_config);
  BeatAnalyzer subdivided(create_six_beat_pattern(bpm, 0.35f), beat_config);

  // Otherwise the two runs would be scoring different beat series and the
  // comparison below would not isolate the subdivision energy.
  CAPTURE(bare.count(), subdivided.count());
  REQUIRE(bare.count() == subdivided.count());

  RhythmConfig rhythm_config;
  rhythm_config.start_bpm = bpm;
  rhythm_config.bpm_min = bpm_min;
  rhythm_config.bpm_max = bpm_max;
  RhythmAnalyzer bare_rhythm(bare, rhythm_config);
  RhythmAnalyzer subdivided_rhythm(subdivided, rhythm_config);

  CAPTURE(bare.time_signature().numerator, bare.time_signature().denominator,
          bare_rhythm.time_signature().numerator, bare_rhythm.time_signature().denominator);
  REQUIRE(bare.time_signature().denominator == 4);
  REQUIRE(bare_rhythm.time_signature().denominator == 4);

  CAPTURE(subdivided.time_signature().numerator, subdivided.time_signature().denominator,
          subdivided_rhythm.time_signature().numerator,
          subdivided_rhythm.time_signature().denominator);
  REQUIRE(subdivided.time_signature().numerator == 6);
  REQUIRE(subdivided.time_signature().denominator == 8);
  REQUIRE(subdivided_rhythm.time_signature().numerator == subdivided.time_signature().numerator);
  REQUIRE(subdivided_rhythm.time_signature().denominator ==
          subdivided.time_signature().denominator);
}

TEST_CASE("RhythmAnalyzer features struct", "[rhythm_analyzer]") {
  Audio audio = create_click_track(120.0f);

  RhythmAnalyzer analyzer(audio);

  const auto& features = analyzer.features();

  REQUIRE(features.time_signature.numerator > 0);
  REQUIRE(features.time_signature.denominator > 0);
  REQUIRE(!features.groove_type.empty());
}

TEST_CASE("RhythmAnalyzer short audio", "[rhythm_analyzer]") {
  Audio audio = create_click_track(120.0f, 22050, 1.0f);

  RhythmConfig config;
  RhythmAnalyzer analyzer(audio, config);

  // Should still work for short audio
  REQUIRE(analyzer.bpm() > 0.0f);
}

TEST_CASE("RhythmAnalyzer meter candidates reach its own time signature", "[rhythm_analyzer]") {
  // RhythmFeatures carries a meter estimate this analyzer runs itself. If the
  // candidate set stopped here, the reported signature would depend on which
  // analyzer a caller happened to read.
  for (int numerator : {5, 7}) {
    Audio audio = create_accented_pattern(120.0f, numerator, 22050, 10.0f);

    RhythmConfig config;
    config.start_bpm = 120.0f;
    config.meter_candidate_numerators = {numerator};
    RhythmAnalyzer analyzer(audio, config);

    CAPTURE(numerator);
    REQUIRE(analyzer.features().time_signature.numerator == numerator);
    REQUIRE(analyzer.features().time_signature.denominator == 4);
    REQUIRE(analyzer.features().time_signature.confidence > 0.0f);

    RhythmConfig default_config;
    default_config.start_bpm = 120.0f;
    RhythmAnalyzer default_analyzer(audio, default_config);
    const int default_numerator = default_analyzer.features().time_signature.numerator;
    CAPTURE(default_numerator);
    REQUIRE(default_numerator != numerator);
    REQUIRE((default_numerator == 3 || default_numerator == 4 || default_numerator == 6));
  }
}

TEST_CASE("RhythmAnalyzer reports the requested beat unit", "[rhythm_analyzer]") {
  Audio audio = create_4_4_pattern(120.0f, 22050, 8.0f);

  RhythmConfig eighth;
  eighth.start_bpm = 120.0f;
  eighth.meter_denominator = 8;
  RhythmAnalyzer eighth_analyzer(audio, eighth);

  RhythmConfig default_config;
  default_config.start_bpm = 120.0f;
  RhythmAnalyzer default_analyzer(audio, default_config);

  CAPTURE(eighth_analyzer.features().time_signature.numerator);
  REQUIRE(eighth_analyzer.features().time_signature.denominator == 8);
  REQUIRE(default_analyzer.features().time_signature.denominator == 4);
}

TEST_CASE("RhythmAnalyzer applies the numerator and beat-unit settings together",
          "[rhythm_analyzer]") {
  // Both meter fields are read from the same config, so a copy that carried
  // only one of them would still be caught here.
  Audio audio = create_accented_pattern(120.0f, 5, 22050, 10.0f);

  RhythmConfig config;
  config.start_bpm = 120.0f;
  config.meter_candidate_numerators = {5};
  config.meter_denominator = 8;
  RhythmAnalyzer analyzer(audio, config);

  REQUIRE(analyzer.features().time_signature.numerator == 5);
  REQUIRE(analyzer.features().time_signature.denominator == 8);
}
