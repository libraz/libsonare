#include "mir/tempo_estimator_bridge.h"

#include <algorithm>
#include <cmath>
#include <vector>

#include "analysis/tempo_curve.h"
#include "util/constants.h"

namespace sonare::mir {
namespace {

using sonare::constants::kEpsilon;

// Period (in seconds) per beat for a given BPM.
double period_for_bpm(double bpm) { return 60.0 / std::max(bpm, 1.0e-3); }

// The tempo-state grid, the interval observations and the Viterbi decode behind
// them live in analysis/tempo_curve.h. They are shared rather than local so the
// per-beat curve an analysis reports and the segments this bridge writes into a
// project are the same numbers grouped differently.
TempoCurveConfig curve_config(const TempoEstimatorConfig& config) {
  TempoCurveConfig curve;
  curve.bpm_min = config.bpm_min;
  curve.bpm_max = config.bpm_max;
  curve.tempo_state_count = config.tempo_state_count;
  curve.transition_weight = config.transition_weight;
  return curve;
}

std::vector<BeatIntervalObservation> build_observations(const BeatAnalysisInput& input) {
  return build_beat_interval_observations(input.beats, input.onset_strength, input.sample_rate,
                                          input.hop_length);
}

double activation_at(const BeatAnalysisInput& input, double time_s) {
  return onset_activation_at(input.onset_strength, input.sample_rate, input.hop_length, time_s);
}

double downbeat_origin_ppq(const BeatAnalysisInput& input, const TempoEstimatorConfig& config,
                           double bpm_scale) {
  const double ppq_per_detected_beat =
      (config.ppq_per_beat > 0.0 ? config.ppq_per_beat : 1.0) * bpm_scale;
  for (int index : input.downbeat_indices) {
    if (index >= 0 && static_cast<size_t>(index) < input.beats.size()) {
      return static_cast<double>(index) * ppq_per_detected_beat;
    }
  }
  return 0.0;
}

// Builds time-signature segments. A single segment is emitted at the detected
// downbeat origin when available; TempoMap uses the segment start as the bar
// phase anchor. Without downbeats the origin remains ppq 0.
std::vector<transport::TimeSignatureSegment> build_time_sigs(const BeatAnalysisInput& input,
                                                             const TempoEstimatorConfig& config,
                                                             double bpm_scale) {
  transport::TimeSignatureSegment seg;
  seg.start_ppq = downbeat_origin_ppq(input, config, bpm_scale);
  seg.time_sig.numerator = std::max(input.time_signature.numerator, 1);
  seg.time_sig.denominator = std::max(input.time_signature.denominator, 1);
  return {seg};
}

// Converts the decoded per-beat BPM curve into piecewise tempo segments anchored
// at integer beat ppq positions.
//
// The decoded grid BPM is used only to GROUP beats: a run of consecutive beats
// whose decoded grid tempo stays within ramp_threshold of the run's start is
// folded into one constant segment; a larger change opens a new segment and
// ramps the previous one toward it. Crucially the EMITTED bpm of each segment is
// derived from the actually observed inter-beat intervals over the whole run
// (60 / mean-IBI), not from the coarse grid value. This keeps the DP's role
// (octave/phase stabilization, grouping) while giving the segment continuous
// timing accuracy, so reconstructed beat positions do not drift over long spans.
//
// `obs[i].ibi` is the interval leading INTO beat (i+1); decoded_bpm has the same
// indexing, so decoded_bpm[i] is the tempo of the interval that STARTS at beat i.
std::vector<transport::TempoSegment> build_segments(const BeatAnalysisInput& input,
                                                    const std::vector<BeatIntervalObservation>& obs,
                                                    const std::vector<double>& decoded_bpm,
                                                    double bpm_scale,
                                                    const TempoEstimatorConfig& config) {
  std::vector<transport::TempoSegment> segments;
  const double ppq_per_detected_beat =
      (config.ppq_per_beat > 0.0 ? config.ppq_per_beat : 1.0) * bpm_scale;

  // Fallback: no usable beat grid -> a single constant segment from global BPM.
  if (decoded_bpm.empty() || decoded_bpm.size() != obs.size()) {
    transport::TempoSegment seg;
    seg.start_ppq = 0.0;
    seg.bpm = std::max(static_cast<double>(input.bpm) * bpm_scale, 1.0);
    seg.start_sample = 0.0;
    segments.push_back(seg);
    return segments;
  }

  const double thresh = std::max(static_cast<double>(config.ramp_threshold), 0.0);

  // Mean BPM of the run obs[run_start .. run_end) from total elapsed time, so a
  // constant segment exactly preserves the total duration of the beats it spans.
  auto run_bpm = [&](size_t run_start, size_t run_end) {
    double total_ibi = 0.0;
    for (size_t k = run_start; k < run_end; ++k) total_ibi += obs[k].ibi;
    const size_t count = run_end - run_start;
    if (count == 0 || total_ibi <= 0.0) return std::max(decoded_bpm[run_start] * bpm_scale, 1.0);
    const double mean_ibi = total_ibi / static_cast<double>(count);
    return std::max((60.0 / mean_ibi) * bpm_scale, 1.0);
  };

  // The duration-preserving constant BPM of each emitted segment, kept so a
  // segment that later becomes a ramp can be re-anchored without drifting.
  std::vector<double> calibrated_bpm;

  size_t run_start = 0;
  for (size_t i = 1; i <= decoded_bpm.size(); ++i) {
    // Close the run when the decoded grid tempo departs from the run's start, or
    // at the end of the sequence.
    const bool at_end = (i == decoded_bpm.size());
    bool boundary = at_end;
    if (!at_end) {
      const double base = decoded_bpm[run_start];
      const double rel = std::abs(decoded_bpm[i] - base) / std::max(base, 1.0);
      boundary = rel >= thresh;
    }
    if (!boundary) continue;

    transport::TempoSegment seg;
    seg.start_ppq = static_cast<double>(run_start) * ppq_per_detected_beat;
    seg.bpm = run_bpm(run_start, i);
    seg.start_sample = 0.0;  // re-derived by TempoMap normalization from start_ppq
    if (!segments.empty()) {
      // Ramp the previous segment toward this one for a smooth tempo change.
      segments.back().end_bpm = seg.bpm;
    }
    segments.push_back(seg);
    calibrated_bpm.push_back(seg.bpm);
    run_start = i;
  }

  // Re-anchor each ramped segment's START bpm so the ramp spans the SAME elapsed
  // time as its duration-calibrated constant. A linear-BPM ramp from b0 to b1
  // over a fixed ppq span elapses the time of a constant at the logarithmic mean
  // L(b0,b1) = (b1 - b0) / ln(b1/b0), NOT at b0. Leaving the segment's stored
  // `bpm` as the constant therefore over/under-runs the segment at the boundary,
  // shifting every later beat (the "ramp boundary" quantization defect). We hold
  // end_bpm = b1 (continuity with the next segment) and solve for the b0 whose
  // logarithmic mean with b1 equals the calibrated constant M, by bisection
  // (L is monotonic in b0). Deterministic and bounded-iteration.
  auto log_mean = [](double a, double b) {
    if (std::abs(a - b) < 1.0e-9) return 0.5 * (a + b);
    return (b - a) / (std::log(b) - std::log(a));
  };
  for (size_t i = 0; i < segments.size(); ++i) {
    transport::TempoSegment& seg = segments[i];
    const double m = calibrated_bpm[i];                      // duration-calibrated mean
    const double b1 = seg.end_bpm;                           // ramp target (next seg start)
    if (!(b1 > 0.0) || std::abs(b1 - m) < 1.0e-9) continue;  // constant segment
    // Bisect b0 so that log_mean(b0, b1) == m. log_mean(., b1) is continuous and
    // strictly increasing in b0 (from 0 as b0->0+ to +inf), so a wide bracket
    // always contains the unique root.
    double lo_b = 1.0e-3;
    double hi_b = std::max(m, b1) * 8.0;
    for (int it = 0; it < 80; ++it) {
      const double mid = 0.5 * (lo_b + hi_b);
      if (log_mean(mid, b1) < m) {
        lo_b = mid;
      } else {
        hi_b = mid;
      }
    }
    seg.bpm = std::max(0.5 * (lo_b + hi_b), 1.0);
  }

  // Re-anchoring changes the next segment's start BPM. Refresh every preceding
  // ramp target afterwards so the two segments meet at exactly the same tempo.
  for (size_t i = 0; i + 1 < segments.size(); ++i) {
    if (segments[i].end_bpm > 0.0) {
      segments[i].end_bpm = segments[i + 1].bpm;
    }
  }

  if (!segments.empty()) segments.front().start_sample = 0.0;
  return segments;
}

float clamp01(double v) { return static_cast<float>(std::clamp(v, 0.0, 1.0)); }

// Confidence from how tightly the decoded BPM tracks the observed IBIs:
// 1 / (1 + mean squared log-ratio). Deterministic.
float estimate_confidence(const std::vector<BeatIntervalObservation>& obs,
                          const std::vector<double>& decoded_bpm) {
  if (obs.empty() || decoded_bpm.size() != obs.size()) return 0.0f;
  double acc = 0.0;
  for (size_t i = 0; i < obs.size(); ++i) {
    const double r = std::log(period_for_bpm(decoded_bpm[i])) - std::log(obs[i].ibi);
    acc += r * r;
  }
  const double mean = acc / static_cast<double>(obs.size());
  return clamp01(1.0 / (1.0 + mean));
}

double normalized_activation_at(const BeatAnalysisInput& input, double time_s,
                                double max_activation) {
  if (input.onset_strength.empty() || max_activation <= kEpsilon || input.sample_rate <= 0 ||
      input.hop_length <= 0) {
    return 0.0;
  }
  return std::clamp(activation_at(input, time_s) / max_activation, 0.0, 1.0);
}

double max_activation(const BeatAnalysisInput& input) {
  double max_act = 0.0;
  for (float value : input.onset_strength) {
    max_act = std::max(max_act, static_cast<double>(value));
  }
  return max_act;
}

float octave_evidence(const BeatAnalysisInput& input, double bpm_scale) {
  if (input.beats.size() < 3 || input.onset_strength.empty()) return 0.5f;
  const double max_act = max_activation(input);
  if (max_act <= kEpsilon) return 0.5f;

  if (bpm_scale > 1.0) {
    // Double-tempo plausibility: strong activations between detected beats mean
    // the analyzer may have skipped every other beat.
    double midpoint_sum = 0.0;
    size_t midpoint_count = 0;
    for (size_t i = 1; i < input.beats.size(); ++i) {
      const double a = input.beats[i - 1].time;
      const double b = input.beats[i].time;
      if (b <= a) continue;
      midpoint_sum += normalized_activation_at(input, 0.5 * (a + b), max_act);
      ++midpoint_count;
    }
    if (midpoint_count == 0) return 0.5f;
    return clamp01(midpoint_sum / static_cast<double>(midpoint_count));
  }

  if (bpm_scale < 1.0) {
    // Half-tempo plausibility: one alternating phase is strong while the other
    // is weak, suggesting the detector may have included subdivisions.
    double phase_sum[2] = {0.0, 0.0};
    size_t phase_count[2] = {0, 0};
    for (size_t i = 0; i < input.beats.size(); ++i) {
      const size_t phase = i & 1u;
      phase_sum[phase] += normalized_activation_at(input, input.beats[i].time, max_act);
      ++phase_count[phase];
    }
    if (phase_count[0] == 0 || phase_count[1] == 0) return 0.5f;
    const double a = phase_sum[0] / static_cast<double>(phase_count[0]);
    const double b = phase_sum[1] / static_cast<double>(phase_count[1]);
    const double strong = std::max(a, b);
    const double weak = std::min(a, b);
    return clamp01(strong * (1.0 - weak));
  }

  return 1.0f;
}

float scaled_octave_confidence(float primary_conf, const BeatAnalysisInput& input,
                               double bpm_scale) {
  const float evidence = octave_evidence(input, bpm_scale);
  return clamp01(static_cast<double>(primary_conf) * static_cast<double>(evidence));
}

}  // namespace

BeatAnalysisInput make_input_from_analyzer(const BeatAnalyzer& analyzer) {
  BeatAnalysisInput in;
  in.beats = analyzer.beats();
  in.bpm = analyzer.bpm();
  in.downbeat_indices = analyzer.downbeat_indices();
  in.time_signature = analyzer.time_signature();
  in.onset_strength = analyzer.onset_strength();
  in.sample_rate = analyzer.sample_rate();
  in.hop_length = analyzer.hop_length();
  return in;
}

std::vector<TempoEstimate> estimate_tempo(const BeatAnalysisInput& input,
                                          const TempoEstimatorConfig& config) {
  std::vector<TempoEstimate> candidates;

  const std::vector<BeatIntervalObservation> obs = build_observations(input);
  const std::vector<double> decoded = decode_beat_tempo_curve(obs, curve_config(config));

  // Primary estimate (scale = 1).
  {
    TempoEstimate est;
    est.segments = build_segments(input, obs, decoded, 1.0, config);
    est.time_sigs = build_time_sigs(input, config, 1.0);
    est.confidence = estimate_confidence(obs, decoded);
    est.label = "primary";
    candidates.push_back(std::move(est));
  }

  if (config.include_octave_candidates && !decoded.empty()) {
    // Double-tempo variant: bpm *2, bar length halved in quarters is preserved
    // by keeping the same numerator/denominator (the listener-relative meter is
    // a separate choice; we keep the analysis meter). Discount confidence.
    const float primary_conf = candidates.front().confidence;
    {
      TempoEstimate est;
      est.segments = build_segments(input, obs, decoded, 2.0, config);
      est.time_sigs = build_time_sigs(input, config, 2.0);
      est.confidence = scaled_octave_confidence(primary_conf, input, 2.0);
      est.label = "double";
      candidates.push_back(std::move(est));
    }
    {
      TempoEstimate est;
      est.segments = build_segments(input, obs, decoded, 0.5, config);
      est.time_sigs = build_time_sigs(input, config, 0.5);
      est.confidence = scaled_octave_confidence(primary_conf, input, 0.5);
      est.label = "half";
      candidates.push_back(std::move(est));
    }
  }

  // Stable sort by confidence descending; the primary stays first on ties
  // (std::stable_sort preserves insertion order, keeping output deterministic).
  std::stable_sort(
      candidates.begin(), candidates.end(),
      [](const TempoEstimate& a, const TempoEstimate& b) { return a.confidence > b.confidence; });

  return candidates;
}

}  // namespace sonare::mir
