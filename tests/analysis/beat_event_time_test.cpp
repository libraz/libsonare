/// @file beat_event_time_test.cpp
/// @brief Beat and onset times sit at the steepest energy rise inside their evidence frame.

#include <algorithm>
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <cstdint>
#include <vector>

#include "analysis/analysis_rate.h"
#include "analysis/beat_analyzer.h"
#include "analysis/onset_analyzer.h"
#include "core/audio.h"
#include "core/convert.h"
#include "feature/onset.h"
#include "util/constants.h"

using namespace sonare;

namespace {

constexpr int kSampleRate = 22050;
constexpr double kBpm = 120.0;
constexpr double kLeadInSeconds = 0.5;
constexpr int kEventCount = 24;

/// @brief Event onsets at @ref kBpm after @ref kLeadInSeconds of silence.
std::vector<double> event_times() {
  std::vector<double> times;
  for (int i = 0; i < kEventCount; ++i) {
    times.push_back(kLeadInSeconds + i * 60.0 / kBpm);
  }
  return times;
}

/// @brief Short broadband clicks: 10 ms of decaying deterministic noise per event.
Audio click_train() {
  const std::vector<double> times = event_times();
  const double duration = times.back() + 1.0;
  std::vector<float> samples(static_cast<size_t>(duration * kSampleRate), 0.0f);
  uint32_t state = 12345u;
  const int length = kSampleRate / 100;
  for (double t : times) {
    const size_t start = static_cast<size_t>(std::lround(t * kSampleRate));
    for (int i = 0; i < length && start + i < samples.size(); ++i) {
      state = state * 1664525u + 1013904223u;
      const float noise =
          static_cast<float>(state >> 8) / static_cast<float>(1u << 24) * 2.0f - 1.0f;
      samples[start + i] = 0.8f * noise * std::exp(-static_cast<float>(i) / (0.25f * length));
    }
  }
  return Audio::from_vector(std::move(samples), kSampleRate);
}

/// @brief Plucked-style notes: an instant attack, then a 120 ms exponential decay.
Audio decaying_notes() {
  const std::vector<double> times = event_times();
  const double duration = times.back() + 1.0;
  std::vector<float> samples(static_cast<size_t>(duration * kSampleRate), 0.0f);
  const double pitches[] = {220.0, 277.18, 329.63, 440.0};
  const int length = static_cast<int>(0.45 * kSampleRate);
  for (size_t n = 0; n < times.size(); ++n) {
    const size_t start = static_cast<size_t>(std::lround(times[n] * kSampleRate));
    const double f = pitches[n % 4];
    for (int i = 0; i < length && start + i < samples.size(); ++i) {
      const double t = static_cast<double>(i) / kSampleRate;
      samples[start + i] +=
          static_cast<float>(0.6 * std::exp(-t / 0.12) * std::sin(constants::kTwoPiD * f * t));
    }
  }
  return Audio::from_vector(std::move(samples), kSampleRate);
}

BeatConfig beat_config() {
  BeatConfig config;
  config.start_bpm = static_cast<float>(kBpm);
  config.bpm_min = 60.0f;
  config.bpm_max = 180.0f;
  return config;
}

/// @brief Fine hop of the refinement at @p sr: 1.25 ms rounded to samples.
double fine_hop_s(int sr) { return std::lround(1.25e-3 * sr) / static_cast<double>(sr); }

/// @brief Checks every beat against the nearest true onset.
/// @details The signed median sits within two fine hops. A single beat may sit up to one envelope
///          hop off, since the spacing stays the tracker's and each beat frame may fall on either
///          side of its event.
void require_beats_on_onsets(const BeatAnalyzer& analyzer) {
  const int sr = analyzer.sample_rate();
  const double median_bound = 2.0 * fine_hop_s(sr);
  const double beat_bound = analyzer.hop_length() / static_cast<double>(sr);
  const std::vector<double> truth = event_times();
  REQUIRE(analyzer.count() >= static_cast<size_t>(kEventCount / 2));
  std::vector<double> errors;
  for (const Beat& beat : analyzer.beats()) {
    double best = 1.0e9;
    for (double t : truth) {
      if (std::abs(beat.time - t) < std::abs(best)) best = beat.time - t;
    }
    CAPTURE(beat.frame, beat.time, best, beat_bound);
    REQUIRE(std::abs(best) <= beat_bound);
    errors.push_back(best);
  }
  std::nth_element(errors.begin(), errors.begin() + errors.size() / 2, errors.end());
  CAPTURE(errors[errors.size() / 2], median_bound);
  REQUIRE(std::abs(errors[errors.size() / 2]) <= median_bound);
}

/// @brief Every beat is its shifted frame time moved by one offset; the BPM is the frame span's.
/// @return The offset in seconds.
double require_one_offset(const BeatAnalyzer& analyzer) {
  const int sr = analyzer.sample_rate();
  const int hop = analyzer.hop_length();
  const int shift = analyzer.onset_shift_frames();
  const std::vector<Beat>& beats = analyzer.beats();
  REQUIRE(beats.size() >= 2);
  const double offset = beats.back().time - envelope_frame_time(beats.back().frame, shift, sr, hop);
  for (const Beat& beat : beats) {
    const float frame_time = envelope_frame_time(beat.frame, shift, sr, hop);
    if (frame_time + offset < 0.0) continue;  // clamped at the start of the signal
    CAPTURE(beat.frame, beat.time, frame_time, offset);
    REQUIRE(static_cast<double>(beat.time) - frame_time == Catch::Approx(offset).margin(1.0e-5));
  }
  REQUIRE(beats.size() >= 2);
  const double expected_bpm = 60.0 * sr / hop * static_cast<double>(beats.size() - 1) /
                              static_cast<double>(beats.back().frame - beats.front().frame);
  REQUIRE(static_cast<double>(analyzer.bpm()) == Catch::Approx(expected_bpm).epsilon(1.0e-5));
  return offset;
}

}  // namespace

TEST_CASE("beat analyzer reports the front end's centering shift", "[beat][event_time]") {
  const Audio audio = click_train();
  const BeatConfig config = beat_config();
  const BeatAnalyzer analyzer(audio, config);
  const int expected = onset_center_shift_frames(
      stft_config_at_rate(config.n_fft, config.hop_length, audio.sample_rate()), true);
  REQUIRE(expected > 0);
  REQUIRE(analyzer.onset_shift_frames() == expected);
}

TEST_CASE("beats on a click train out of silence sit on the clicks", "[beat][event_time]") {
  const BeatAnalyzer analyzer(click_train(), beat_config());
  require_beats_on_onsets(analyzer);
  require_one_offset(analyzer);
}

TEST_CASE("beats on decaying notes out of silence sit on the attacks", "[beat][event_time]") {
  const BeatAnalyzer analyzer(decaying_notes(), beat_config());
  require_beats_on_onsets(analyzer);
  require_one_offset(analyzer);
}

TEST_CASE("an envelope without a stated shift reports frame times", "[beat][event_time]") {
  const BeatAnalyzer from_audio(click_train(), beat_config());
  const BeatAnalyzer from_envelope(from_audio.onset_strength(), from_audio.sample_rate(),
                                   from_audio.hop_length(), beat_config());
  REQUIRE(from_envelope.onset_shift_frames() == 0);
  REQUIRE(from_envelope.beat_frames() == from_audio.beat_frames());
  for (const Beat& beat : from_envelope.beats()) {
    REQUIRE(beat.time ==
            frames_to_time(beat.frame, from_envelope.sample_rate(), from_envelope.hop_length()));
  }
  // Without audio there is nothing to refine against.
  REQUIRE(require_one_offset(from_envelope) == 0.0);
}

TEST_CASE("onset times sit on the attacks, backtracked onsets at their frame",
          "[beat][event_time]") {
  const Audio audio = decaying_notes();
  const std::vector<double> truth = event_times();
  for (bool backtrack : {false, true}) {
    OnsetDetectConfig config;
    config.backtrack = backtrack;
    const OnsetAnalyzer analyzer(audio, config);
    CAPTURE(backtrack);
    REQUIRE(analyzer.onset_shift_frames() > 0);
    REQUIRE(analyzer.count() >= static_cast<size_t>(kEventCount / 2));
    const std::vector<int> frames = analyzer.onset_frames();
    for (size_t i = 0; i < analyzer.count(); ++i) {
      const Onset& onset = analyzer.onsets()[i];
      CAPTURE(i, onset.frame, onset.time);
      REQUIRE(frames[i] == onset.frame);
      const float frame_time = envelope_frame_time(onset.frame, analyzer.onset_shift_frames(),
                                                   analyzer.sample_rate(), analyzer.hop_length());
      if (backtrack || onset.time == frame_time) {
        // Backtracked, or a frame holding no rise (a second detection on one note's decay).
        REQUIRE(onset.time == frame_time);
      } else {
        // The first of two detections on one note is searched only up to the midpoint, so it may
        // take a fine frame that only overlaps the attack: one fine window plus one hop early.
        double best = 1.0e9;
        for (double t : truth) best = std::min(best, std::abs(onset.time - t));
        REQUIRE(best <= 5.0e-3 + fine_hop_s(kSampleRate));
      }
    }
  }
}

namespace {

/// @brief Event shapes of the material table, each starting at its onset.
enum class Material { ToneClick, NoiseClick, Pluck, Attack30ms, Kick, Attack80ms };

constexpr double kTableFirstOnset = 0.13;
constexpr double kTableDuration = 10.0;

std::vector<double> table_onsets() {
  std::vector<double> onsets;
  for (double t = kTableFirstOnset; t < kTableDuration - 0.6; t += 60.0 / kBpm) onsets.push_back(t);
  return onsets;
}

/// @brief Amplitude of @p material at @p u seconds after its onset; @p state drives the noise.
double table_event(Material material, double u, uint32_t& state) {
  using constants::kTwoPiD;
  switch (material) {
    case Material::ToneClick:
      return u < 0.01 ? 0.8 * std::exp(-u / 0.0027) * std::sin(kTwoPiD * 1000.0 * u) : 0.0;
    case Material::NoiseClick: {
      state = state * 1664525u + 1013904223u;
      const double noise = static_cast<double>(state >> 8) / static_cast<double>(1u << 23) - 1.0;
      return u < 0.01 ? 0.8 * std::exp(-u / 0.003) * noise : 0.0;
    }
    case Material::Pluck: {
      double s = 0.0;
      for (int h = 1; h <= 6; ++h) s += std::sin(kTwoPiD * 220.0 * h * u) / h;
      return 0.4 * std::exp(-u / 0.12) * s;
    }
    case Material::Attack30ms:
      return 0.6 * std::min(1.0, u / 0.03) * std::exp(-std::max(0.0, u - 0.03) / 0.2) *
             std::sin(kTwoPiD * 440.0 * u);
    case Material::Kick: {
      const double phase = kTwoPiD * (50.0 * u + 3.0 * (1.0 - std::exp(-u / 0.03)));
      return 0.9 * std::min(1.0, u / 0.002) * std::exp(-u / 0.15) * std::sin(phase);
    }
    case Material::Attack80ms:
      return 0.6 * std::min(1.0, u / 0.08) * std::exp(-std::max(0.0, u - 0.08) / 0.3) *
             std::sin(kTwoPiD * 330.0 * u);
  }
  return 0.0;
}

/// @brief One table row: @p material at @p level, optionally over a sustained three-note pad.
/// @details The pad's partials beat against each other, so its short-time level dips by tens of
///          dB several times per beat.
Audio table_signal(Material material, int sr, double level, bool pad) {
  std::vector<float> x(static_cast<size_t>(kTableDuration * sr), 0.0f);
  uint32_t state = 12345u;
  for (double t0 : table_onsets()) {
    const size_t start = static_cast<size_t>(std::ceil(t0 * sr));
    const size_t length = static_cast<size_t>(0.45 * sr);
    for (size_t i = start; i < x.size() && i < start + length; ++i) {
      const double u = static_cast<double>(i) / sr - t0;
      // A raised-cosine fade over the last 100 ms keeps the cut from reading as an event.
      const double fade =
          u < 0.35 ? 1.0 : 0.5 * (1.0 + std::cos(constants::kPiD * (u - 0.35) / 0.1));
      x[i] += static_cast<float>(level * fade * table_event(material, u, state));
    }
  }
  if (pad) {
    for (size_t i = 0; i < x.size(); ++i) {
      const double t = static_cast<double>(i) / sr;
      x[i] += static_cast<float>(0.05 * (std::sin(constants::kTwoPiD * 196.0 * t) +
                                         std::sin(constants::kTwoPiD * 246.94 * t) +
                                         std::sin(constants::kTwoPiD * 293.66 * t)));
    }
  }
  return Audio::from_vector(std::move(x), sr);
}

/// @brief Signed distance of each time to its nearest true onset, for times within 0.2 s of one.
std::vector<double> onset_errors(const std::vector<float>& times) {
  const std::vector<double> truth = table_onsets();
  std::vector<double> errors;
  for (float time : times) {
    double best = 1.0e9;
    for (double t : truth) {
      if (std::abs(time - t) < std::abs(best)) best = time - t;
    }
    if (std::abs(best) < 0.2) errors.push_back(best);
  }
  return errors;
}

double median_of(std::vector<double> values) {
  REQUIRE(!values.empty());
  std::nth_element(values.begin(), values.begin() + values.size() / 2, values.end());
  return values[values.size() / 2];
}

double max_abs(const std::vector<double>& values) {
  double worst = 0.0;
  for (double v : values) worst = std::max(worst, std::abs(v));
  return worst;
}

std::vector<float> times_of(const BeatAnalyzer& analyzer) { return analyzer.beat_times(); }

/// @brief Number of onsets whose time differs from their evidence-frame time.
size_t refined_count(const OnsetAnalyzer& analyzer) {
  size_t count = 0;
  for (const Onset& onset : analyzer.onsets()) {
    if (onset.time != envelope_frame_time(onset.frame, analyzer.onset_shift_frames(),
                                          analyzer.sample_rate(), analyzer.hop_length())) {
      ++count;
    }
  }
  return count;
}

struct RowResult {
  double beat_median = 0.0;
  double onset_median = 0.0;
};

/// @brief Checks one table row against bounds taken from the fine hop and the envelope hop.
RowResult check_row(Material material, int sr, double level, bool pad) {
  const Audio audio = table_signal(material, sr, level, pad);
  const BeatAnalyzer beats(audio, beat_config());
  const OnsetAnalyzer onsets(audio);
  const double hop = fine_hop_s(sr);
  const size_t events = table_onsets().size();

  const std::vector<double> beat_errors = onset_errors(times_of(beats));
  REQUIRE(beat_errors.size() >= events - 1);
  RowResult result;
  result.beat_median = median_of(beat_errors);
  const double beat_bound = beats.hop_length() / static_cast<double>(sr);
  CAPTURE(sr, level, pad, result.beat_median, max_abs(beat_errors), beat_bound);
  CHECK(std::abs(result.beat_median) <= 2.0 * hop);
  CHECK(max_abs(beat_errors) <= beat_bound);
  require_one_offset(beats);

  const std::vector<double> errors = onset_errors(onsets.onset_times());
  REQUIRE(errors.size() >= events - 1);
  result.onset_median = median_of(errors);
  CAPTURE(result.onset_median, max_abs(errors), refined_count(onsets), onsets.count());
  CHECK(std::abs(result.onset_median) <= 2.0 * hop);
  CHECK(max_abs(errors) <= 3.0 * hop);
  CHECK(refined_count(onsets) == onsets.count());
  return result;
}

}  // namespace

TEST_CASE("event times sit on the onsets across materials and rates", "[beat][event_time]") {
  for (Material material : {Material::ToneClick, Material::NoiseClick, Material::Pluck,
                            Material::Attack30ms, Material::Kick}) {
    std::vector<RowResult> rows;
    for (int sr : {22050, 44100, 48000}) {
      CAPTURE(static_cast<int>(material), sr);
      rows.push_back(check_row(material, sr, 1.0, false));
    }
    // Two fine hops at the coarsest rate: each rate may round its rise to either side.
    const double agree = 2.0 * fine_hop_s(22050);
    for (size_t i = 1; i < rows.size(); ++i) {
      CAPTURE(static_cast<int>(material), i, rows[0].beat_median, rows[i].beat_median);
      CHECK(std::abs(rows[i].beat_median - rows[0].beat_median) <= agree);
      CHECK(std::abs(rows[i].onset_median - rows[0].onset_median) <= agree);
    }
  }
}

TEST_CASE("event times do not depend on the level", "[beat][event_time]") {
  for (Material material : {Material::NoiseClick, Material::Pluck, Material::Attack30ms}) {
    CAPTURE(static_cast<int>(material));
    const RowResult loud = check_row(material, 44100, 1.0, false);
    const RowResult quiet = check_row(material, 44100, 0.06, false);
    CHECK(std::abs(quiet.beat_median - loud.beat_median) <= fine_hop_s(44100));
    CHECK(std::abs(quiet.onset_median - loud.onset_median) <= fine_hop_s(44100));
  }
}

TEST_CASE("attacks over a beating pad are placed above the pad's own dips", "[beat][event_time]") {
  for (Material material : {Material::ToneClick, Material::Pluck, Material::Kick}) {
    for (int sr : {22050, 48000}) {
      CAPTURE(static_cast<int>(material), sr);
      check_row(material, sr, 1.0, true);
    }
  }
}

TEST_CASE("slow attacks under a beating pad are placed inside the attack or not at all",
          "[beat][event_time]") {
  // A 30 ms ramp clears the pad's recent peaks by the 6 dB evidence margin only where the
  // pad happens to dip at the attack, so a minority of events have evidence; the rest keep
  // their evidence-frame times. Where a rise is found it lies inside the ramp.
  const int sr = 44100;
  const double attack = 0.03;
  const double hop = fine_hop_s(sr);
  const Audio audio = table_signal(Material::Attack30ms, sr, 1.0, true);
  const OnsetAnalyzer onsets(audio);
  REQUIRE(onsets.count() > 0);
  const size_t refined = refined_count(onsets);
  CAPTURE(refined, onsets.count());
  CHECK(refined > 0);
  CHECK(refined < onsets.count() / 2);
  for (const Onset& onset : onsets.onsets()) {
    const float frame_time = envelope_frame_time(onset.frame, onsets.onset_shift_frames(),
                                                 onsets.sample_rate(), onsets.hop_length());
    if (onset.time == frame_time) continue;
    const std::vector<double> error = onset_errors({onset.time});
    REQUIRE(error.size() == 1);
    CAPTURE(onset.frame, onset.time, error[0]);
    CHECK(error[0] >= -hop);
    CHECK(error[0] <= attack);
  }

  // The beats follow the same evidence: one offset, which lands them inside the ramp.
  const BeatAnalyzer beats(audio, beat_config());
  require_one_offset(beats);
  const double median = median_of(onset_errors(times_of(beats)));
  CAPTURE(median);
  CHECK(median >= -hop);
  CHECK(median <= attack);
}
