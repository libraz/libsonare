#include "mir/tempo_estimator_bridge.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <vector>

#include "util/constants.h"

namespace sonare::mir {
namespace {

using sonare::constants::kEpsilon;

// Period (in seconds) per beat for a given BPM.
double period_for_bpm(double bpm) { return 60.0 / std::max(bpm, 1.0e-3); }

// Log-spaced tempo-state grid (BPM). Log spacing makes the transition cost
// scale-invariant: a half/double jump costs the same anywhere in the grid.
std::vector<double> build_tempo_grid(const TempoEstimatorConfig& config) {
  const int n = std::max(config.tempo_state_count, 2);
  const double lo = std::log(std::max(config.bpm_min, 1.0f));
  const double hi = std::log(std::max(config.bpm_max, config.bpm_min + 1.0f));
  std::vector<double> grid(static_cast<size_t>(n));
  for (int i = 0; i < n; ++i) {
    const double t = static_cast<double>(i) / static_cast<double>(n - 1);
    grid[static_cast<size_t>(i)] = std::exp(lo + t * (hi - lo));
  }
  return grid;
}

// Per-beat observation: the inter-beat interval (seconds) leading INTO beat i,
// and an activation weight derived from the onset-strength curve at the beat.
struct BeatObservation {
  double ibi;     // seconds; the local period observed at this beat
  double weight;  // activation weight in [~0, 1]; higher => trust ibi more
};

// Samples the onset-strength envelope at a beat time (nearest frame). Returns 1
// when no envelope is available so every beat is trusted equally.
double activation_at(const BeatAnalysisInput& input, double beat_time_s) {
  if (input.onset_strength.empty() || input.sample_rate <= 0 || input.hop_length <= 0) {
    return 1.0;
  }
  const double frames_per_sec =
      static_cast<double>(input.sample_rate) / static_cast<double>(input.hop_length);
  const long frame = std::lround(beat_time_s * frames_per_sec);
  if (frame < 0 || frame >= static_cast<long>(input.onset_strength.size())) return 0.0;
  return static_cast<double>(input.onset_strength[static_cast<size_t>(frame)]);
}

std::vector<BeatObservation> build_observations(const BeatAnalysisInput& input) {
  std::vector<BeatObservation> obs;
  const size_t n = input.beats.size();
  if (n < 2) return obs;
  obs.reserve(n - 1);
  // Normalize activation by the max so weights are comparable across inputs.
  double max_act = kEpsilon;
  for (const Beat& b : input.beats) {
    max_act = std::max(max_act, static_cast<double>(activation_at(input, b.time)));
  }
  for (size_t i = 1; i < n; ++i) {
    const double ibi = static_cast<double>(input.beats[i].time - input.beats[i - 1].time);
    const double act = static_cast<double>(activation_at(input, input.beats[i].time)) / max_act;
    obs.push_back({std::max(ibi, 1.0e-4), std::clamp(act, 0.0, 1.0)});
  }
  return obs;
}

// Deterministic Viterbi over the tempo-state grid. Returns, for each
// observation, the decoded BPM. Observation cost: squared log-ratio between the
// state's period and the observed IBI, scaled by the activation weight.
// Transition cost: squared log-ratio between adjacent decoded BPMs, scaled by
// transition_weight. Pure DP, no randomness; ties broken toward the lower state
// index for reproducibility.
std::vector<double> viterbi_decode(const std::vector<BeatObservation>& obs,
                                   const std::vector<double>& grid,
                                   const TempoEstimatorConfig& config) {
  const size_t t_count = obs.size();
  const size_t s_count = grid.size();
  std::vector<double> decoded;
  if (t_count == 0 || s_count == 0) return decoded;

  const double trans_w = std::max(0.0, static_cast<double>(config.transition_weight));
  constexpr double kInf = std::numeric_limits<double>::infinity();

  std::vector<double> log_grid(s_count);
  for (size_t s = 0; s < s_count; ++s) log_grid[s] = std::log(grid[s]);

  auto obs_cost = [&](size_t t, size_t s) {
    const double state_period = period_for_bpm(grid[s]);
    const double r = std::log(state_period) - std::log(obs[t].ibi);
    // Trust the observation in proportion to its activation weight; an
    // (near-)silent beat contributes little, letting the transition prior carry.
    return obs[t].weight * r * r;
  };

  std::vector<double> prev(s_count);
  std::vector<double> cur(s_count);
  std::vector<std::vector<size_t>> back(t_count, std::vector<size_t>(s_count, 0));

  for (size_t s = 0; s < s_count; ++s) prev[s] = obs_cost(0, s);

  for (size_t t = 1; t < t_count; ++t) {
    for (size_t s = 0; s < s_count; ++s) {
      double best = kInf;
      size_t best_prev = 0;
      const double oc = obs_cost(t, s);
      for (size_t p = 0; p < s_count; ++p) {
        const double dr = log_grid[s] - log_grid[p];
        const double cost = prev[p] + trans_w * dr * dr;
        if (cost < best) {
          best = cost;
          best_prev = p;
        }
      }
      cur[s] = best + oc;
      back[t][s] = best_prev;
    }
    prev.swap(cur);
  }

  // Terminate at the minimum-cost state (lowest index on ties).
  size_t end_state = 0;
  double best = prev[0];
  for (size_t s = 1; s < s_count; ++s) {
    if (prev[s] < best) {
      best = prev[s];
      end_state = s;
    }
  }

  decoded.assign(t_count, 0.0);
  size_t s = end_state;
  for (size_t t = t_count; t-- > 0;) {
    decoded[t] = grid[s];
    s = back[t][s];
    if (t == 0) break;
  }
  return decoded;
}

// Builds time-signature segments. A single segment at ppq 0 is emitted (the
// analysis carries one meter); this is the shape TempoMap expects.
std::vector<transport::TimeSignatureSegment> build_time_sigs(const TimeSignature& sig) {
  transport::TimeSignatureSegment seg;
  seg.start_ppq = 0.0;
  seg.time_sig.numerator = std::max(sig.numerator, 1);
  seg.time_sig.denominator = std::max(sig.denominator, 1);
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
                                                    const std::vector<BeatObservation>& obs,
                                                    const std::vector<double>& decoded_bpm,
                                                    double bpm_scale,
                                                    const TempoEstimatorConfig& config) {
  std::vector<transport::TempoSegment> segments;
  const double ppq_per_beat = config.ppq_per_beat > 0.0 ? config.ppq_per_beat : 1.0;

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
    seg.start_ppq = static_cast<double>(run_start) * ppq_per_beat;
    seg.bpm = run_bpm(run_start, i);
    seg.start_sample = 0.0;  // re-derived by TempoMap normalization from start_ppq
    if (!segments.empty()) {
      // Ramp the previous segment toward this one for a smooth tempo change.
      segments.back().end_bpm = seg.bpm;
    }
    segments.push_back(seg);
    run_start = i;
  }

  if (!segments.empty()) segments.front().start_sample = 0.0;
  return segments;
}

float clamp01(double v) { return static_cast<float>(std::clamp(v, 0.0, 1.0)); }

// Confidence from how tightly the decoded BPM tracks the observed IBIs:
// 1 / (1 + mean squared log-ratio). Deterministic.
float estimate_confidence(const std::vector<BeatObservation>& obs,
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

  const std::vector<BeatObservation> obs = build_observations(input);
  const std::vector<double> grid = build_tempo_grid(config);
  const std::vector<double> decoded = viterbi_decode(obs, grid, config);

  const std::vector<transport::TimeSignatureSegment> time_sigs =
      build_time_sigs(input.time_signature);

  // Primary estimate (scale = 1).
  {
    TempoEstimate est;
    est.segments = build_segments(input, obs, decoded, 1.0, config);
    est.time_sigs = time_sigs;
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
      est.time_sigs = time_sigs;
      est.confidence = clamp01(primary_conf * 0.5);
      est.label = "double";
      candidates.push_back(std::move(est));
    }
    {
      TempoEstimate est;
      est.segments = build_segments(input, obs, decoded, 0.5, config);
      est.time_sigs = time_sigs;
      est.confidence = clamp01(primary_conf * 0.5);
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
