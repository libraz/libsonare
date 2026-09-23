#include "analysis/tempo_curve.h"

#include <algorithm>
#include <cmath>
#include <vector>

#include "util/constants.h"

namespace sonare {
namespace {

using sonare::constants::kEpsilon;

// Floor on an observation's weight, so a run of silent beats still has a
// unique solution: the prior carries across it instead of the system going singular.
constexpr double kMinObservationWeight = 1.0e-6;

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
  const size_t n = observations.size();
  std::vector<double> decoded;
  if (n == 0) return decoded;

  const double lam = std::max(0.0, static_cast<double>(config.transition_weight));
  const double lo = std::log(std::max(config.bpm_min, 1.0f));
  const double hi = std::log(std::max(config.bpm_max, config.bpm_min + 1.0f));

  // Minimise sum_t w_t (x_t - y_t)^2 + lam * sum_t (x_t - x_{t-1})^2 over the
  // log-tempo x; y_t is the log tempo of interval t. The normal equations are a
  // symmetric, diagonally dominant tridiagonal system, solved by the Thomas algorithm.
  std::vector<double> diag(n);
  std::vector<double> rhs(n);
  for (size_t t = 0; t < n; ++t) {
    const double w = std::max(observations[t].weight, kMinObservationWeight);
    const double y = std::log(60.0 / observations[t].ibi);
    const double neighbours = (t > 0 ? 1.0 : 0.0) + (t + 1 < n ? 1.0 : 0.0);
    diag[t] = w + lam * neighbours;
    rhs[t] = w * y;
  }
  // Forward sweep; the off-diagonal is -lam everywhere.
  std::vector<double> upper(n, 0.0);
  for (size_t t = 0; t < n; ++t) {
    double d = diag[t];
    if (t > 0) {
      d -= lam * upper[t - 1];
      rhs[t] += lam * rhs[t - 1];
    }
    upper[t] = (t + 1 < n) ? lam / d : 0.0;
    rhs[t] /= d;
  }
  decoded.assign(n, 0.0);
  for (size_t k = n; k-- > 0;) {
    const double x = rhs[k] + (k + 1 < n ? upper[k] * decoded[k + 1] : 0.0);
    decoded[k] = x;
  }
  for (double& x : decoded) x = std::exp(std::clamp(x, lo, hi));
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
