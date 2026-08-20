#include "analysis/tempo_curve.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <vector>

#include "util/constants.h"

namespace sonare {
namespace {

using sonare::constants::kEpsilon;

// Period (in seconds) per beat for a given BPM.
double period_for_bpm(double bpm) { return 60.0 / std::max(bpm, 1.0e-3); }

// Log-spaced tempo-state grid (BPM). Log spacing makes the transition cost
// scale-invariant: a half/double jump costs the same anywhere in the grid.
std::vector<double> build_tempo_grid(const TempoCurveConfig& config) {
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

}  // namespace

double onset_activation_at(const std::vector<float>& onset_strength, int sample_rate,
                           int hop_length, double time_s) {
  if (onset_strength.empty() || sample_rate <= 0 || hop_length <= 0) {
    return 1.0;
  }
  const double frames_per_sec = static_cast<double>(sample_rate) / static_cast<double>(hop_length);
  const long frame = std::lround(time_s * frames_per_sec);
  if (frame < 0 || frame >= static_cast<long>(onset_strength.size())) return 0.0;
  return static_cast<double>(onset_strength[static_cast<size_t>(frame)]);
}

std::vector<BeatIntervalObservation> build_beat_interval_observations(
    const std::vector<Beat>& beats, const std::vector<float>& onset_strength, int sample_rate,
    int hop_length) {
  std::vector<BeatIntervalObservation> obs;
  const size_t n = beats.size();
  if (n < 2) return obs;
  obs.reserve(n - 1);
  // Normalize activation by the max so weights are comparable across inputs.
  double max_act = kEpsilon;
  for (const Beat& b : beats) {
    max_act =
        std::max(max_act, onset_activation_at(onset_strength, sample_rate, hop_length, b.time));
  }
  for (size_t i = 1; i < n; ++i) {
    const double ibi = static_cast<double>(beats[i].time - beats[i - 1].time);
    const double act =
        onset_activation_at(onset_strength, sample_rate, hop_length, beats[i].time) / max_act;
    obs.push_back({std::max(ibi, 1.0e-4), std::clamp(act, 0.0, 1.0)});
  }
  return obs;
}

std::vector<double> decode_beat_tempo_curve(
    const std::vector<BeatIntervalObservation>& observations, const TempoCurveConfig& config) {
  const std::vector<double> grid = build_tempo_grid(config);
  const size_t t_count = observations.size();
  const size_t s_count = grid.size();
  std::vector<double> decoded;
  if (t_count == 0 || s_count == 0) return decoded;

  const double trans_w = std::max(0.0, static_cast<double>(config.transition_weight));
  constexpr double kInf = std::numeric_limits<double>::infinity();

  std::vector<double> log_grid(s_count);
  for (size_t s = 0; s < s_count; ++s) log_grid[s] = std::log(grid[s]);

  auto obs_cost = [&](size_t t, size_t s) {
    const double state_period = period_for_bpm(grid[s]);
    const double r = std::log(state_period) - std::log(observations[t].ibi);
    // Trust the observation in proportion to its activation weight; an
    // (near-)silent beat contributes little, letting the transition prior carry.
    return observations[t].weight * r * r;
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

  // Backtrace from the terminal state. back[t][s] holds the predecessor state at
  // t-1 for current state s at t, and is only written for t in [1, t_count); row
  // 0 carries no predecessor. Walk t = t_count-1 .. 1 stepping through back[t],
  // then write the first frame's state explicitly so the t==0 row (which has no
  // valid predecessor pointer) is never dereferenced.
  decoded.assign(t_count, 0.0);
  size_t s = end_state;
  for (size_t t = t_count - 1; t >= 1; --t) {
    decoded[t] = grid[s];
    s = back[t][s];
  }
  decoded[0] = grid[s];
  return decoded;
}

std::vector<float> estimate_beat_local_bpm(const std::vector<Beat>& beats,
                                           const std::vector<float>& onset_strength,
                                           int sample_rate, int hop_length,
                                           const TempoCurveConfig& config) {
  const std::vector<BeatIntervalObservation> obs =
      build_beat_interval_observations(beats, onset_strength, sample_rate, hop_length);
  const std::vector<double> decoded = decode_beat_tempo_curve(obs, config);
  if (decoded.empty()) return {};

  std::vector<float> curve(beats.size());
  for (size_t i = 0; i < decoded.size(); ++i) {
    curve[i] = static_cast<float>(decoded[i]);
  }
  // The final beat closes the last interval rather than opening one, so it has
  // no tempo of its own and carries the tempo that led into it.
  curve.back() = static_cast<float>(decoded.back());
  return curve;
}

}  // namespace sonare
