#include "feature/pitch.h"

#include <algorithm>
#include <cmath>
#include <complex>
#include <limits>

#include "core/convert.h"
#include "core/fft.h"
#include "core/spectrum.h"
#include "util/constants.h"
#include "util/exception.h"
#include "util/reflect_padding.h"

namespace sonare {

using sonare::constants::kEpsilon;

namespace {

/// @brief Parabolic interpolation for sub-sample precision.
/// @param y Array of 3 values (y[-1], y[0], y[1])
/// @return Fractional offset from center sample
float parabolic_interp(float ym1, float y0, float yp1) {
  float denom = ym1 - 2.0f * y0 + yp1;
  if (std::abs(denom) < constants::kEpsilon) {
    return 0.0f;
  }
  return 0.5f * (ym1 - yp1) / denom;
}

double beta_2_18_cdf(double x) {
  x = std::clamp(x, 0.0, 1.0);
  return 1.0 - std::pow(1.0 - x, 18.0) * (1.0 + 18.0 * x);
}

std::vector<float> librosa_yin_cmndf(const float* frame, int frame_length, int min_period,
                                     int max_period) {
  std::vector<double> acf(static_cast<size_t>(max_period) + 1, 0.0);
  for (int tau = 0; tau <= max_period; ++tau) {
    double sum = 0.0;
    for (int index = 0; index + tau < frame_length; ++index) {
      sum += static_cast<double>(frame[index]) * frame[index + tau];
    }
    acf[static_cast<size_t>(tau)] = sum;
  }

  std::vector<double> cumulative_square(static_cast<size_t>(frame_length), 0.0);
  double square_sum = 0.0;
  for (int index = 0; index < frame_length; ++index) {
    square_sum += static_cast<double>(frame[index]) * frame[index];
    cumulative_square[static_cast<size_t>(index)] = square_sum;
  }

  std::vector<double> difference(static_cast<size_t>(max_period) + 1, 0.0);
  for (int tau = 1; tau <= max_period; ++tau) {
    difference[static_cast<size_t>(tau)] = 2.0 * (acf[0] - acf[static_cast<size_t>(tau)]) -
                                           cumulative_square[static_cast<size_t>(tau - 1)];
  }

  std::vector<double> cumulative_mean(static_cast<size_t>(max_period) + 1, 1.0);
  double running = 0.0;
  for (int tau = 1; tau <= max_period; ++tau) {
    running += difference[static_cast<size_t>(tau)];
    cumulative_mean[static_cast<size_t>(tau)] = running / tau;
  }

  std::vector<float> cmndf(static_cast<size_t>(max_period - min_period + 1), 1.0f);
  for (int tau = min_period; tau <= max_period; ++tau) {
    const double denominator = cumulative_mean[static_cast<size_t>(tau)];
    cmndf[static_cast<size_t>(tau - min_period)] =
        denominator > 1.0e-300
            ? static_cast<float>(difference[static_cast<size_t>(tau)] / denominator)
            : 1.0f;
  }
  return cmndf;
}

int next_power_of_two(int value) {
  int power = 1;
  while (power < value) {
    power <<= 1;
  }
  return power;
}

}  // namespace

float PitchResult::median_f0() const {
  std::vector<float> voiced_f0;
  for (size_t i = 0; i < f0.size(); ++i) {
    if (voiced_flag[i] && f0[i] > 0.0f) {
      voiced_f0.push_back(f0[i]);
    }
  }
  if (voiced_f0.empty()) {
    return 0.0f;
  }
  std::sort(voiced_f0.begin(), voiced_f0.end());
  size_t mid = voiced_f0.size() / 2;
  if (voiced_f0.size() % 2 == 0) {
    return (voiced_f0[mid - 1] + voiced_f0[mid]) / 2.0f;
  }
  return voiced_f0[mid];
}

float PitchResult::mean_f0() const {
  float sum = 0.0f;
  int count = 0;
  for (size_t i = 0; i < f0.size(); ++i) {
    if (voiced_flag[i] && f0[i] > 0.0f) {
      sum += f0[i];
      ++count;
    }
  }
  return count > 0 ? sum / count : 0.0f;
}

std::vector<float> yin_difference(const float* frame, int frame_length, int max_lag) {
  std::vector<float> diff(max_lag, 0.0f);
  if (frame_length <= 0 || max_lag <= 0) {
    return diff;
  }

  // d(tau) = sum_{j=0}^{W-1} (x[j] - x[j+tau])^2
  // Per the YIN paper, the summation window W is constant (frame_length / 2)
  // for all tau values, ensuring consistent normalization.
  const int window = frame_length / 2;
  if (window <= 0) {
    return diff;
  }

  const int available_lags = std::min(max_lag, frame_length - window + 1);
  if (available_lags <= 0) {
    return diff;
  }

  const int comparison_length = std::min(frame_length, window + available_lags - 1);
  const int n_fft = next_power_of_two(window + comparison_length - 1);
  std::vector<float> reference(static_cast<size_t>(n_fft), 0.0f);
  std::vector<float> comparison(static_cast<size_t>(n_fft), 0.0f);

  // Cross-correlation r[tau] = sum_j x[j] * x[j + tau] via convolution of the
  // reversed reference window and the comparison span.
  double reference_energy = 0.0;
  for (int j = 0; j < window; ++j) {
    reference[static_cast<size_t>(window - 1 - j)] = frame[j];
    reference_energy += static_cast<double>(frame[j]) * frame[j];
  }
  for (int j = 0; j < comparison_length; ++j) {
    comparison[static_cast<size_t>(j)] = frame[j];
  }

  FFT fft(n_fft);
  std::vector<std::complex<float>> ref_spectrum(static_cast<size_t>(fft.n_bins()));
  std::vector<std::complex<float>> cmp_spectrum(static_cast<size_t>(fft.n_bins()));
  fft.forward(reference.data(), ref_spectrum.data());
  fft.forward(comparison.data(), cmp_spectrum.data());
  for (size_t bin = 0; bin < ref_spectrum.size(); ++bin) {
    ref_spectrum[bin] *= cmp_spectrum[bin];
  }
  std::vector<float> correlation(static_cast<size_t>(n_fft), 0.0f);
  fft.inverse(ref_spectrum.data(), correlation.data());

  std::vector<double> squared_prefix(static_cast<size_t>(comparison_length) + 1, 0.0);
  for (int j = 0; j < comparison_length; ++j) {
    squared_prefix[static_cast<size_t>(j + 1)] =
        squared_prefix[static_cast<size_t>(j)] + static_cast<double>(frame[j]) * frame[j];
  }

  for (int tau = 0; tau < available_lags; ++tau) {
    const double shifted_energy = squared_prefix[static_cast<size_t>(tau + window)] -
                                  squared_prefix[static_cast<size_t>(tau)];
    const double cross = correlation[static_cast<size_t>(window - 1 + tau)];
    const double value = reference_energy + shifted_energy - 2.0 * cross;
    diff[static_cast<size_t>(tau)] = value <= 0.0 ? 0.0f : static_cast<float>(value);
  }
  diff[0] = 0.0f;

  return diff;
}

std::vector<float> yin_cmndf(const std::vector<float>& diff) {
  std::vector<float> cmndf(diff.size());

  if (diff.empty()) {
    return cmndf;
  }

  cmndf[0] = 1.0f;  // By definition

  float running_sum = 0.0f;
  for (size_t tau = 1; tau < diff.size(); ++tau) {
    running_sum += diff[tau];
    if (running_sum > constants::kEpsilon) {
      cmndf[tau] = diff[tau] * tau / running_sum;
    } else {
      cmndf[tau] = 1.0f;
    }
  }

  return cmndf;
}

float yin_find_pitch(const std::vector<float>& cmndf, float threshold, int min_period,
                     int max_period) {
  int n = static_cast<int>(cmndf.size());
  min_period = std::max(1, min_period);
  max_period = std::min(max_period, n - 1);

  // Find first minimum below threshold
  int best_tau = -1;
  for (int tau = min_period; tau < max_period; ++tau) {
    if (cmndf[tau] < threshold) {
      // Check if it's a local minimum
      while (tau + 1 < max_period && cmndf[tau + 1] < cmndf[tau]) {
        ++tau;
      }
      best_tau = tau;
      break;
    }
  }

  if (best_tau < 0) {
    // No pitch found below threshold, find global minimum
    float min_val = std::numeric_limits<float>::max();
    for (int tau = min_period; tau < max_period; ++tau) {
      if (cmndf[tau] < min_val) {
        min_val = cmndf[tau];
        best_tau = tau;
      }
    }
    // Only accept if reasonably low
    if (min_val > 0.5f) {
      return 0.0f;  // Unvoiced
    }
  }

  if (best_tau <= 0 || best_tau >= n - 1) {
    return static_cast<float>(best_tau);
  }

  // Parabolic interpolation for sub-sample precision
  float offset = parabolic_interp(cmndf[best_tau - 1], cmndf[best_tau], cmndf[best_tau + 1]);
  return static_cast<float>(best_tau) + offset;
}

float yin(const float* frame, int frame_length, int sr, float fmin, float fmax, float threshold) {
  float confidence;
  return yin_with_confidence(frame, frame_length, sr, fmin, fmax, threshold, &confidence);
}

float yin_with_confidence(const float* frame, int frame_length, int sr, float fmin, float fmax,
                          float threshold, float* out_confidence) {
  // Convert frequency to period (in samples)
  int min_period = static_cast<int>(std::floor(static_cast<float>(sr) / fmax));
  int max_period = static_cast<int>(std::ceil(static_cast<float>(sr) / fmin));

  // Limit max_period to half frame length
  max_period = std::min(max_period, frame_length / 2);

  if (min_period >= max_period || max_period < 2) {
    *out_confidence = 0.0f;
    return 0.0f;
  }

  // Compute YIN
  std::vector<float> diff = yin_difference(frame, frame_length, max_period + 1);
  std::vector<float> cmndf = yin_cmndf(diff);

  float period = yin_find_pitch(cmndf, threshold, min_period, max_period);

  if (period <= 0.0f) {
    *out_confidence = 0.0f;
    return 0.0f;
  }

  // Confidence is 1 - cmndf at the detected period
  int tau = static_cast<int>(std::round(period));
  tau = std::max(0, std::min(tau, static_cast<int>(cmndf.size()) - 1));
  *out_confidence = 1.0f - cmndf[tau];
  *out_confidence = std::max(0.0f, std::min(1.0f, *out_confidence));

  // Convert period to frequency
  float freq = static_cast<float>(sr) / period;

  // Clamp to valid range
  if (freq < fmin || freq > fmax) {
    *out_confidence = 0.0f;
    return 0.0f;
  }

  return freq;
}

PitchResult yin_track(const Audio& audio, const PitchConfig& config) {
  SONARE_CHECK(!audio.empty(), ErrorCode::InvalidParameter);
  SONARE_CHECK(config.frame_length > 0, ErrorCode::InvalidParameter);
  SONARE_CHECK(config.hop_length > 0, ErrorCode::InvalidParameter);

  int sr = audio.sample_rate();
  std::vector<float> padded;
  const float* data = audio.data();
  int signal_samples = static_cast<int>(audio.size());
  if (config.center) {
    padded = reflect_center_pad(audio.data(), audio.size(), config.frame_length / 2);
    data = padded.data();
    signal_samples = static_cast<int>(padded.size());
  }
  int n_frames = 1 + (signal_samples - config.frame_length) / config.hop_length;

  if (n_frames <= 0) {
    return PitchResult();
  }

  PitchResult result;
  result.f0.resize(n_frames);
  result.voiced_prob.resize(n_frames);
  result.voiced_flag.resize(n_frames);

  for (int i = 0; i < n_frames; ++i) {
    int start = i * config.hop_length;
    float confidence = 0.0f;

    float freq = yin_with_confidence(data + start, config.frame_length, sr, config.fmin,
                                     config.fmax, config.threshold, &confidence);

    result.f0[i] = freq;
    result.voiced_prob[i] = confidence;
    result.voiced_flag[i] = (freq > 0.0f && confidence > 0.0f);

    if (!result.voiced_flag[i] && !config.fill_na) {
      result.f0[i] = std::numeric_limits<float>::quiet_NaN();
    }
  }

  return result;
}

PitchResult pyin(const Audio& audio, const PitchConfig& config) {
  SONARE_CHECK(!audio.empty(), ErrorCode::InvalidParameter);
  SONARE_CHECK(config.frame_length > 0, ErrorCode::InvalidParameter);
  SONARE_CHECK(config.hop_length > 0, ErrorCode::InvalidParameter);

  int sr = audio.sample_rate();
  std::vector<float> padded;
  const float* data = audio.data();
  int signal_samples = static_cast<int>(audio.size());
  if (config.center) {
    padded = reflect_center_pad(audio.data(), audio.size(), config.frame_length / 2);
    data = padded.data();
    signal_samples = static_cast<int>(padded.size());
  }
  int n_frames = 1 + (signal_samples - config.frame_length) / config.hop_length;

  if (n_frames <= 0) {
    return PitchResult();
  }

  // Convert frequency to period
  int min_period = static_cast<int>(std::floor(static_cast<float>(sr) / config.fmax));
  int max_period = static_cast<int>(std::ceil(static_cast<float>(sr) / config.fmin));
  max_period = std::min(max_period, config.frame_length - 1);

  if (min_period >= max_period) {
    return PitchResult();
  }

  constexpr int kThresholds = 100;
  constexpr double kBoltzmann = 2.0;
  constexpr double kResolution = 0.1;
  constexpr double kMaxTransitionRate = 35.92;
  constexpr double kSwitchProb = 0.01;
  constexpr double kNoTroughProb = 0.01;
  constexpr double kLogFloor = 1.0e-300;
  const int bins_per_semitone = static_cast<int>(std::ceil(1.0 / kResolution));
  const int n_pitch_bins =
      static_cast<int>(std::floor(static_cast<double>(constants::kSemitonesPerOctave) *
                                  bins_per_semitone * std::log2(config.fmax / config.fmin))) +
      1;
  const int n_states = 2 * n_pitch_bins;

  std::vector<double> thresholds(kThresholds + 1);
  for (int index = 0; index <= kThresholds; ++index) {
    thresholds[static_cast<size_t>(index)] = static_cast<double>(index) / kThresholds;
  }
  std::vector<double> beta_probs(kThresholds);
  for (int index = 0; index < kThresholds; ++index) {
    beta_probs[static_cast<size_t>(index)] =
        beta_2_18_cdf(thresholds[static_cast<size_t>(index + 1)]) -
        beta_2_18_cdf(thresholds[static_cast<size_t>(index)]);
  }

  // Flat row-major storage [n_frames x n_states]: observation_idx(t, s) = t * n_states + s.
  // Row-major matches the access pattern: every inner loop sweeps states for a fixed frame
  // (observation initialization, Viterbi update, Viterbi backtrack), keeping cache lines hot
  // and avoiding n_frames separate heap allocations / row-pointer chases.
  const auto obs_idx = [n_states](int t, int s) {
    return static_cast<size_t>(t) * static_cast<size_t>(n_states) + static_cast<size_t>(s);
  };
  std::vector<double> observation(static_cast<size_t>(n_frames) * static_cast<size_t>(n_states),
                                  0.0);
  std::vector<double> voiced_prob(static_cast<size_t>(n_frames), 0.0);

  for (int i = 0; i < n_frames; ++i) {
    int start = i * config.hop_length;

    std::vector<float> cmndf =
        librosa_yin_cmndf(data + start, config.frame_length, min_period, max_period);

    std::vector<int> troughs;
    for (int index = 0; index < static_cast<int>(cmndf.size()); ++index) {
      const bool is_left_edge = index == 0 && index + 1 < static_cast<int>(cmndf.size()) &&
                                cmndf[index] < cmndf[index + 1];
      const bool is_local_min = index > 0 && index + 1 < static_cast<int>(cmndf.size()) &&
                                cmndf[index] < cmndf[index - 1] && cmndf[index] <= cmndf[index + 1];
      if (is_left_edge || is_local_min) {
        troughs.push_back(index);
      }
    }
    if (troughs.empty()) {
      const double unvoiced = 1.0 / n_pitch_bins;
      for (int bin = 0; bin < n_pitch_bins; ++bin) {
        observation[obs_idx(i, n_pitch_bins + bin)] = unvoiced;
      }
      continue;
    }

    std::vector<double> trough_probs(troughs.size(), 0.0);
    for (int threshold_index = 0; threshold_index < kThresholds; ++threshold_index) {
      const double threshold = thresholds[static_cast<size_t>(threshold_index + 1)];
      int below_count = 0;
      for (int index : troughs) {
        below_count += cmndf[static_cast<size_t>(index)] < threshold ? 1 : 0;
      }
      if (below_count == 0) {
        continue;
      }
      double norm = 0.0;
      for (int position = 0; position < below_count; ++position) {
        norm += std::exp(-kBoltzmann * position);
      }
      int below_position = 0;
      for (size_t trough_index = 0; trough_index < troughs.size(); ++trough_index) {
        const int index = troughs[trough_index];
        if (cmndf[static_cast<size_t>(index)] < threshold) {
          const double prior = std::exp(-kBoltzmann * below_position) / norm;
          trough_probs[trough_index] += prior * beta_probs[static_cast<size_t>(threshold_index)];
          ++below_position;
        }
      }
    }

    auto global_min_it = std::min_element(troughs.begin(), troughs.end(), [&](int lhs, int rhs) {
      return cmndf[static_cast<size_t>(lhs)] < cmndf[static_cast<size_t>(rhs)];
    });
    const size_t global_min_index =
        static_cast<size_t>(std::distance(troughs.begin(), global_min_it));
    int thresholds_below_min = 0;
    const double global_min_height = cmndf[static_cast<size_t>(*global_min_it)];
    for (int threshold_index = 0; threshold_index < kThresholds; ++threshold_index) {
      if (global_min_height >= thresholds[static_cast<size_t>(threshold_index + 1)]) {
        ++thresholds_below_min;
      }
    }
    double no_trough_mass = 0.0;
    for (int threshold_index = 0; threshold_index < thresholds_below_min; ++threshold_index) {
      no_trough_mass += beta_probs[static_cast<size_t>(threshold_index)];
    }
    trough_probs[global_min_index] += kNoTroughProb * no_trough_mass;

    for (size_t trough_index = 0; trough_index < troughs.size(); ++trough_index) {
      const int index = troughs[trough_index];
      if (trough_probs[trough_index] <= 0.0) {
        continue;
      }
      float offset = 0.0f;
      if (index > 0 && index + 1 < static_cast<int>(cmndf.size())) {
        offset = parabolic_interp(cmndf[static_cast<size_t>(index - 1)],
                                  cmndf[static_cast<size_t>(index)],
                                  cmndf[static_cast<size_t>(index + 1)]);
      }
      const double period = static_cast<double>(min_period + index) + offset;
      const double f0 = static_cast<double>(sr) / period;
      int bin = static_cast<int>(std::llround(static_cast<double>(constants::kSemitonesPerOctave) *
                                              bins_per_semitone * std::log2(f0 / config.fmin)));
      bin = std::clamp(bin, 0, n_pitch_bins - 1);
      observation[obs_idx(i, bin)] += trough_probs[trough_index];
    }

    double voiced = 0.0;
    for (int bin = 0; bin < n_pitch_bins; ++bin) {
      voiced += observation[obs_idx(i, bin)];
    }
    voiced = std::clamp(voiced, 0.0, 1.0);
    voiced_prob[static_cast<size_t>(i)] = voiced;
    const double unvoiced = (1.0 - voiced) / n_pitch_bins;
    for (int bin = 0; bin < n_pitch_bins; ++bin) {
      observation[obs_idx(i, n_pitch_bins + bin)] = unvoiced;
    }
  }

  const int max_semitones_per_frame = static_cast<int>(
      std::llround(kMaxTransitionRate * static_cast<double>(constants::kSemitonesPerOctave) *
                   config.hop_length / sr));
  const int transition_width = max_semitones_per_frame * bins_per_semitone + 1;
  const int transition_half = transition_width / 2;
  std::vector<std::vector<std::pair<int, double>>> pitch_transitions(
      static_cast<size_t>(n_pitch_bins));
  for (int prev_bin = 0; prev_bin < n_pitch_bins; ++prev_bin) {
    double norm = 0.0;
    const int begin = std::max(0, prev_bin - transition_half);
    const int end = std::min(n_pitch_bins - 1, prev_bin + transition_half);
    for (int curr_bin = begin; curr_bin <= end; ++curr_bin) {
      norm += static_cast<double>(transition_half + 1 - std::abs(curr_bin - prev_bin));
    }
    for (int curr_bin = begin; curr_bin <= end; ++curr_bin) {
      const double weight =
          static_cast<double>(transition_half + 1 - std::abs(curr_bin - prev_bin));
      pitch_transitions[static_cast<size_t>(prev_bin)].push_back({curr_bin, weight / norm});
    }
  }

  // Flat row-major [n_frames x n_states] for Viterbi log-likelihoods and backpointers.
  // Row-major matches the forward update (sweeps curr_state for fixed i), the per-frame
  // log-observation add (sweeps state for fixed i), and the backtrack pass (reads frame i+1's
  // states then the row's argmax at n_frames-1) — all contiguous accesses.
  // Allocations drop from 3 * n_frames to 3 across observation / viterbi / backtrack.
  std::vector<double> viterbi(static_cast<size_t>(n_frames) * static_cast<size_t>(n_states),
                              -std::numeric_limits<double>::infinity());
  std::vector<int> backtrack(static_cast<size_t>(n_frames) * static_cast<size_t>(n_states), -1);
  const double log_init = -std::log(static_cast<double>(n_states));
  for (int state = 0; state < n_states; ++state) {
    viterbi[obs_idx(0, state)] =
        log_init + std::log(std::max(observation[obs_idx(0, state)], kLogFloor));
  }

  for (int i = 1; i < n_frames; ++i) {
    for (int prev_state = 0; prev_state < n_states; ++prev_state) {
      const double previous = viterbi[obs_idx(i - 1, prev_state)];
      if (!std::isfinite(previous)) {
        continue;
      }
      const bool prev_voiced = prev_state < n_pitch_bins;
      const int prev_bin = prev_state % n_pitch_bins;
      for (bool curr_voiced : {true, false}) {
        const double switch_prob = prev_voiced == curr_voiced ? (1.0 - kSwitchProb) : kSwitchProb;
        const int state_offset = curr_voiced ? 0 : n_pitch_bins;
        for (const auto& [curr_bin, pitch_prob] :
             pitch_transitions[static_cast<size_t>(prev_bin)]) {
          const int curr_state = state_offset + curr_bin;
          const double score = previous + std::log(switch_prob) + std::log(pitch_prob);
          const size_t curr_idx = obs_idx(i, curr_state);
          if (score > viterbi[curr_idx]) {
            viterbi[curr_idx] = score;
            backtrack[curr_idx] = prev_state;
          }
        }
      }
    }
    for (int state = 0; state < n_states; ++state) {
      viterbi[obs_idx(i, state)] += std::log(std::max(observation[obs_idx(i, state)], kLogFloor));
    }
  }

  std::vector<int> best_path(n_frames);
  {
    const size_t last_row_begin = obs_idx(n_frames - 1, 0);
    const auto row_begin = viterbi.begin() + static_cast<std::ptrdiff_t>(last_row_begin);
    const auto row_end = row_begin + n_states;
    best_path[static_cast<size_t>(n_frames - 1)] =
        static_cast<int>(std::distance(row_begin, std::max_element(row_begin, row_end)));
  }
  for (int i = n_frames - 2; i >= 0; --i) {
    const int next_state = best_path[static_cast<size_t>(i + 1)];
    const int previous = backtrack[obs_idx(i + 1, next_state)];
    best_path[static_cast<size_t>(i)] = previous < 0 ? next_state : previous;
  }

  std::vector<float> freqs(static_cast<size_t>(n_pitch_bins));
  for (int bin = 0; bin < n_pitch_bins; ++bin) {
    freqs[static_cast<size_t>(bin)] =
        config.fmin *
        std::pow(2.0f, static_cast<float>(bin) / (constants::kSemitonesPerOctave *
                                                  static_cast<float>(bins_per_semitone)));
  }

  PitchResult result;
  result.f0.resize(n_frames);
  result.voiced_prob.resize(n_frames);
  result.voiced_flag.resize(n_frames);
  for (int i = 0; i < n_frames; ++i) {
    const int state = best_path[static_cast<size_t>(i)];
    const int bin = state % n_pitch_bins;
    const bool voiced = state < n_pitch_bins;
    result.voiced_flag[static_cast<size_t>(i)] = voiced;
    result.voiced_prob[static_cast<size_t>(i)] =
        static_cast<float>(voiced_prob[static_cast<size_t>(i)]);
    if (voiced) {
      result.f0[static_cast<size_t>(i)] = freqs[static_cast<size_t>(bin)];
    } else if (config.fill_na) {
      result.f0[static_cast<size_t>(i)] = 0.0f;
    } else {
      result.f0[static_cast<size_t>(i)] = std::numeric_limits<float>::quiet_NaN();
    }
  }
  return result;
}

float freq_to_midi(float freq) { return hz_to_midi(freq); }

float midi_to_freq(float midi) { return midi_to_hz(midi); }

PiptrackResult piptrack(const Audio& audio, int n_fft, int hop_length, float fmin, float fmax,
                        float threshold) {
  SONARE_CHECK(!audio.empty(), ErrorCode::InvalidParameter);
  SONARE_CHECK(n_fft > 0 && hop_length > 0, ErrorCode::InvalidParameter);
  SONARE_CHECK(fmin > 0.0f && fmax > fmin, ErrorCode::InvalidParameter);
  SONARE_CHECK(threshold >= 0.0f, ErrorCode::InvalidParameter);

  StftConfig cfg;
  cfg.n_fft = n_fft;
  cfg.hop_length = hop_length;
  cfg.win_length = n_fft;
  cfg.center = true;
  Spectrogram spec = Spectrogram::compute(audio, cfg);

  const std::vector<float>& mag = spec.magnitude();
  int n_bins = spec.n_bins();
  int n_frames = spec.n_frames();
  int sr = audio.sample_rate();

  // Frequency at each FFT bin.
  std::vector<float> bin_freq(n_bins);
  for (int k = 0; k < n_bins; ++k) {
    bin_freq[k] = static_cast<float>(k) * static_cast<float>(sr) / static_cast<float>(n_fft);
  }

  PiptrackResult out;
  out.n_bins = n_bins;
  out.n_frames = n_frames;
  out.pitches.assign(static_cast<size_t>(n_bins) * n_frames, 0.0f);
  out.magnitudes.assign(static_cast<size_t>(n_bins) * n_frames, 0.0f);

  for (int t = 0; t < n_frames; ++t) {
    float maxm = 0.0f;
    for (int k = 0; k < n_bins; ++k) maxm = std::max(maxm, mag[k * n_frames + t]);
    float gate = threshold * maxm;

    for (int k = 1; k < n_bins - 1; ++k) {
      if (bin_freq[k] < fmin || bin_freq[k] > fmax) continue;
      float a = mag[(k - 1) * n_frames + t];
      float b = mag[k * n_frames + t];
      float c = mag[(k + 1) * n_frames + t];
      if (b <= a || b <= c || b < gate) continue;
      float shift = parabolic_interp(a, b, c);
      float freq =
          (static_cast<float>(k) + shift) * static_cast<float>(sr) / static_cast<float>(n_fft);
      out.pitches[k * n_frames + t] = freq;
      // Quadratic max value at vertex.
      float denom = a - 2.0f * b + c;
      float peak_mag = b;
      if (std::abs(denom) > constants::kEpsilon) {
        peak_mag = b - 0.25f * (a - c) * shift;
      }
      out.magnitudes[k * n_frames + t] = peak_mag;
    }
  }
  return out;
}

float pitch_tuning(const std::vector<float>& frequencies, float resolution, int bins_per_octave) {
  SONARE_CHECK(resolution > 0.0f && resolution < 1.0f, ErrorCode::InvalidParameter);
  SONARE_CHECK(bins_per_octave > 0, ErrorCode::InvalidParameter);

  // Convert to log-frequency bin offsets relative to A4.
  // librosa: residual = mod(bins_per_octave * log2(freq/440) + 0.5, 1.0) - 0.5
  std::vector<float> residuals;
  residuals.reserve(frequencies.size());
  for (float f : frequencies) {
    if (f <= 0.0f || !std::isfinite(f)) continue;
    float r = static_cast<float>(bins_per_octave) * std::log2(f / constants::kA4Hz);
    float frac = r - std::floor(r);
    if (frac >= 0.5f) frac -= 1.0f;
    residuals.push_back(frac);
  }
  if (residuals.empty()) return 0.0f;

  // Histogram with resolution bin width spanning (-0.5, 0.5].
  int n_bins = static_cast<int>(std::round(1.0f / resolution));
  if (n_bins <= 0) n_bins = 1;
  std::vector<int> hist(n_bins, 0);
  for (float v : residuals) {
    int idx = static_cast<int>(std::floor((v + 0.5f) * n_bins));
    if (idx < 0) idx = 0;
    if (idx >= n_bins) idx = n_bins - 1;
    hist[idx] += 1;
  }
  int peak = 0;
  int peak_count = hist[0];
  for (int i = 1; i < n_bins; ++i) {
    if (hist[i] > peak_count) {
      peak_count = hist[i];
      peak = i;
    }
  }
  return (static_cast<float>(peak) + 0.5f) / static_cast<float>(n_bins) - 0.5f;
}

float estimate_tuning(const Audio& audio, int n_fft, int hop_length, float resolution,
                      int bins_per_octave) {
  PiptrackResult pp = piptrack(audio, n_fft, hop_length);
  std::vector<float> freqs;
  freqs.reserve(pp.pitches.size());
  // Filter: keep peaks above the per-frame median magnitude (librosa default).
  for (int t = 0; t < pp.n_frames; ++t) {
    float thresh = 0.0f;
    std::vector<float> col_mags;
    col_mags.reserve(pp.n_bins);
    for (int k = 0; k < pp.n_bins; ++k) {
      float m = pp.magnitudes[k * pp.n_frames + t];
      if (m > 0.0f) col_mags.push_back(m);
    }
    if (!col_mags.empty()) {
      std::sort(col_mags.begin(), col_mags.end());
      thresh = col_mags[col_mags.size() / 2];
    }
    for (int k = 0; k < pp.n_bins; ++k) {
      float p = pp.pitches[k * pp.n_frames + t];
      float m = pp.magnitudes[k * pp.n_frames + t];
      if (p > 0.0f && m >= thresh) freqs.push_back(p);
    }
  }
  return pitch_tuning(freqs, resolution, bins_per_octave);
}

}  // namespace sonare
