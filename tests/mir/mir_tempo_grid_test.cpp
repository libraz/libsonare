/// @file mir_tempo_grid_test.cpp
/// @brief MIR tempo/grid: bridge determinism, tempo F-measure quality gate,
///        tempo-change ramp detection, half/double candidate exposure,
///        grid_snap correctness, and TempoMap integration.

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <string>
#include <vector>

#include "mir/grid_snap.h"
#include "mir/tempo_estimator_bridge.h"
#include "transport/tempo_map.h"

namespace {

using sonare::Beat;
using sonare::TimeSignature;
using sonare::mir::BeatAnalysisInput;
using sonare::mir::estimate_tempo;
using sonare::mir::SnapGrid;
using sonare::mir::TempoEstimate;
using sonare::mir::TempoEstimatorConfig;

// Builds a beat sequence at a constant BPM with a small deterministic jitter.
// jitter_idx makes the perturbation reproducible (no RNG).
BeatAnalysisInput make_constant_fixture(double bpm, int n_beats, double jitter_s = 0.0) {
  BeatAnalysisInput in;
  in.bpm = static_cast<float>(bpm);
  in.time_signature = TimeSignature{4, 4, 1.0f};
  in.sample_rate = 22050;
  in.hop_length = 512;
  const double period = 60.0 / bpm;
  for (int i = 0; i < n_beats; ++i) {
    // Deterministic triangle-wave jitter in [-jitter_s, +jitter_s].
    const double phase = (i % 4) / 4.0;  // 0,.25,.5,.75
    const double j = jitter_s * (2.0 * std::abs(phase - 0.5) - 0.5) * 2.0;
    Beat b;
    b.time = static_cast<float>(i * period + j);
    b.frame = static_cast<int>(b.time * in.sample_rate / in.hop_length);
    b.strength = 1.0f;
    in.beats.push_back(b);
  }
  return in;
}

// Beats that accelerate linearly from bpm0 to bpm1 over n_beats.
BeatAnalysisInput make_ramp_fixture(double bpm0, double bpm1, int n_beats) {
  BeatAnalysisInput in;
  in.bpm = static_cast<float>(bpm0);
  in.time_signature = TimeSignature{4, 4, 1.0f};
  in.sample_rate = 22050;
  in.hop_length = 512;
  double t = 0.0;
  for (int i = 0; i < n_beats; ++i) {
    Beat b;
    b.time = static_cast<float>(t);
    b.frame = static_cast<int>(b.time * in.sample_rate / in.hop_length);
    b.strength = 1.0f;
    in.beats.push_back(b);
    const double frac = static_cast<double>(i) / std::max(1, n_beats - 1);
    const double bpm = bpm0 + (bpm1 - bpm0) * frac;
    t += 60.0 / bpm;
  }
  return in;
}

// Reconstructs the beat times implied by a TempoEstimate's segments via a
// TempoMap (beats are at integer ppq positions for 1 ppq == 1 quarter note).
std::vector<double> reconstruct_beat_times(const TempoEstimate& est, int n_beats,
                                           double sample_rate) {
  sonare::transport::TempoMap map;
  map.prepare(sample_rate);
  map.set_segments(est.segments);
  map.set_time_signatures(est.time_sigs);
  std::vector<double> times;
  for (int i = 0; i < n_beats; ++i) {
    const int64_t s = map.ppq_to_sample(static_cast<double>(i));
    times.push_back(static_cast<double>(s) / sample_rate);
  }
  return times;
}

// F-measure with a +-window tolerance between detected and reference beat sets.
double beat_f_measure(const std::vector<double>& detected, const std::vector<double>& reference,
                      double window_s) {
  std::vector<bool> ref_used(reference.size(), false);
  int tp = 0;
  for (double d : detected) {
    for (size_t r = 0; r < reference.size(); ++r) {
      if (!ref_used[r] && std::abs(d - reference[r]) <= window_s) {
        ref_used[r] = true;
        ++tp;
        break;
      }
    }
  }
  const int fp = static_cast<int>(detected.size()) - tp;
  const int fn = static_cast<int>(reference.size()) - tp;
  const double precision = tp + fp > 0 ? static_cast<double>(tp) / (tp + fp) : 0.0;
  const double recall = tp + fn > 0 ? static_cast<double>(tp) / (tp + fn) : 0.0;
  if (precision + recall <= 0.0) return 0.0;
  return 2.0 * precision * recall / (precision + recall);
}

double dominant_bpm(const TempoEstimate& est) {
  // The longest-running segment's bpm is the dominant tempo.
  return est.segments.empty() ? 0.0 : est.segments.front().bpm;
}

}  // namespace

TEST_CASE("mir bridge is deterministic across runs", "[mir]") {
  const BeatAnalysisInput in = make_constant_fixture(120.0, 32, 0.005);
  const std::vector<TempoEstimate> a = estimate_tempo(in);
  const std::vector<TempoEstimate> b = estimate_tempo(in);
  REQUIRE(a.size() == b.size());
  for (size_t k = 0; k < a.size(); ++k) {
    REQUIRE(a[k].segments.size() == b[k].segments.size());
    for (size_t i = 0; i < a[k].segments.size(); ++i) {
      REQUIRE(a[k].segments[i].start_ppq == b[k].segments[i].start_ppq);
      REQUIRE(a[k].segments[i].bpm == b[k].segments[i].bpm);
      REQUIRE(a[k].segments[i].start_sample == b[k].segments[i].start_sample);
      REQUIRE(a[k].segments[i].end_bpm == b[k].segments[i].end_bpm);
    }
    REQUIRE(a[k].confidence == b[k].confidence);
  }
}

TEST_CASE("mir bridge recovers constant tempo within F-measure tolerance", "[mir]") {
  const int n = 48;
  const BeatAnalysisInput in = make_constant_fixture(120.0, n, 0.008);
  const std::vector<TempoEstimate> cands = estimate_tempo(in);
  REQUIRE_FALSE(cands.empty());
  const TempoEstimate& primary = cands.front();

  // Dominant BPM correct within 3%.
  REQUIRE(dominant_bpm(primary) == Catch::Approx(120.0).epsilon(0.03));

  std::vector<double> ref;
  for (int i = 0; i < n; ++i) ref.push_back(i * 60.0 / 120.0);
  const std::vector<double> rec = reconstruct_beat_times(primary, n, in.sample_rate);
  const double f = beat_f_measure(rec, ref, 0.070);  // +-70 ms window
  REQUIRE(f > 0.9);
}

TEST_CASE("mir bridge emits a ramp boundary on a tempo change", "[mir]") {
  const BeatAnalysisInput in = make_ramp_fixture(120.0, 140.0, 48);
  TempoEstimatorConfig cfg;
  const std::vector<TempoEstimate> cands = estimate_tempo(in, cfg);
  REQUIRE_FALSE(cands.empty());
  const TempoEstimate& primary = cands.front();

  // More than one segment AND at least one ramp (end_bpm set) must appear.
  REQUIRE(primary.segments.size() > 1);
  bool has_ramp = false;
  double first_bpm = primary.segments.front().bpm;
  double last_bpm = primary.segments.back().bpm;
  for (const auto& seg : primary.segments) {
    if (seg.end_bpm > 0.0 && std::abs(seg.end_bpm - seg.bpm) > 1e-6) has_ramp = true;
  }
  REQUIRE(has_ramp);
  // Tempo increased overall.
  REQUIRE(last_bpm > first_bpm);
}

TEST_CASE("mir bridge exposes double-tempo candidate when analyzer reports half", "[mir]") {
  // Analyzer "reports" 60 BPM (half of the true 120). The double candidate must
  // be present so the caller can confirm the faster octave.
  const BeatAnalysisInput in = make_constant_fixture(60.0, 32);
  TempoEstimatorConfig cfg;
  cfg.include_octave_candidates = true;
  const std::vector<TempoEstimate> cands = estimate_tempo(in, cfg);

  bool has_double = false;
  bool has_half = false;
  double primary_bpm = 0.0;
  for (const auto& c : cands) {
    const std::string label = c.label;
    if (label == "double") has_double = true;
    if (label == "half") has_half = true;
    if (label == "primary") primary_bpm = c.segments.front().bpm;
  }
  REQUIRE(has_double);
  REQUIRE(has_half);
  REQUIRE(primary_bpm == Catch::Approx(60.0).epsilon(0.05));

  // The double candidate is ~120 BPM.
  for (const auto& c : cands) {
    if (std::string(c.label) == "double") {
      REQUIRE(c.segments.front().bpm == Catch::Approx(120.0).epsilon(0.05));
    }
  }
}

TEST_CASE("mir grid_snap snaps to beat / bar / subdivision", "[mir]") {
  SnapGrid grid;
  grid.time_sig = sonare::transport::TimeSignature{4, 4};
  grid.origin_ppq = 0.0;
  // 4/4: beat = 1 ppq, bar = 4 ppq.
  REQUIRE(sonare::mir::beat_length_ppq(grid) == Catch::Approx(1.0));
  REQUIRE(sonare::mir::bar_length_ppq(grid) == Catch::Approx(4.0));

  // Beat snap: 1.4 -> 1.0, 1.6 -> 2.0.
  REQUIRE(sonare::mir::snap_to_beat(grid, 1.4) == Catch::Approx(1.0));
  REQUIRE(sonare::mir::snap_to_beat(grid, 1.6) == Catch::Approx(2.0));

  // Bar snap: 2.9 -> 4.0 (nearest bar line), 1.0 -> 0.0.
  REQUIRE(sonare::mir::snap_to_bar(grid, 2.9) == Catch::Approx(4.0));
  REQUIRE(sonare::mir::snap_to_bar(grid, 1.0) == Catch::Approx(0.0));

  // Sixteenth-note subdivision (4 per beat => step 0.25): 0.30 -> 0.25.
  REQUIRE(sonare::mir::snap_to_subdivision(grid, 0.30, 4) == Catch::Approx(0.25));
  // Triplet (3 per beat => step 1/3): 0.30 -> 0.3333...
  REQUIRE(sonare::mir::snap_to_subdivision(grid, 0.30, 3) == Catch::Approx(1.0 / 3.0));

  // Strength 0 = no move; strength 1 = full snap; 0.5 = halfway.
  REQUIRE(sonare::mir::snap_to_beat(grid, 1.4, 0.0) == Catch::Approx(1.4));
  REQUIRE(sonare::mir::snap_to_beat(grid, 1.4, 1.0) == Catch::Approx(1.0));
  REQUIRE(sonare::mir::snap_to_beat(grid, 1.4, 0.5) == Catch::Approx(1.2));
}

TEST_CASE("mir grid_snap vectorized + non-zero origin", "[mir]") {
  SnapGrid grid;
  grid.time_sig = sonare::transport::TimeSignature{3, 4};  // bar = 3 ppq
  grid.origin_ppq = 1.0;
  const std::vector<double> in = {1.1, 1.9, 4.2};
  const std::vector<double> out = sonare::mir::snap_to_beat(grid, in);
  REQUIRE(out.size() == in.size());
  REQUIRE(out[0] == Catch::Approx(1.0));
  REQUIRE(out[1] == Catch::Approx(2.0));
  REQUIRE(out[2] == Catch::Approx(4.0));
}

TEST_CASE("mir bridge segments feed TempoMap with sane conversions", "[mir]") {
  const int n = 32;
  const BeatAnalysisInput in = make_constant_fixture(120.0, n);
  const std::vector<TempoEstimate> cands = estimate_tempo(in);
  const TempoEstimate& primary = cands.front();

  sonare::transport::TempoMap map;
  map.prepare(in.sample_rate);
  map.set_segments(primary.segments);
  map.set_time_signatures(primary.time_sigs);

  // ppq 0 -> sample 0.
  REQUIRE(map.ppq_to_sample(0.0) == 0);
  // Round-trip ppq -> sample -> ppq is near-identity.
  const double ppq = 8.0;
  const int64_t s = map.ppq_to_sample(ppq);
  const double back = map.sample_to_ppq(s);
  REQUIRE(back == Catch::Approx(ppq).margin(0.01));
  // At 120 BPM, one quarter note (1 ppq) = 0.5 s = sr/2 samples.
  REQUIRE(static_cast<double>(map.ppq_to_sample(1.0)) ==
          Catch::Approx(in.sample_rate / 2.0).epsilon(0.02));
}
