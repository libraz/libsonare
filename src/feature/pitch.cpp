#include "feature/pitch.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <numeric>

#include "util/exception.h"

namespace sonare {

namespace {

/// @brief Parabolic interpolation for sub-sample precision.
/// @param y Array of 3 values (y[-1], y[0], y[1])
/// @return Fractional offset from center sample
float parabolic_interp(float ym1, float y0, float yp1) {
  float denom = ym1 - 2.0f * y0 + yp1;
  if (std::abs(denom) < 1e-10f) {
    return 0.0f;
  }
  return 0.5f * (ym1 - yp1) / denom;
}

/// @brief Beta distribution PDF for pYIN.
float beta_pdf(float x, float alpha, float beta_param) {
  if (x <= 0.0f || x >= 1.0f) {
    return 0.0f;
  }
  // Simplified beta PDF (unnormalized for relative comparison)
  return std::pow(x, alpha - 1.0f) * std::pow(1.0f - x, beta_param - 1.0f);
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

  // d(tau) = sum_{j=0}^{W-1} (x[j] - x[j+tau])^2
  for (int tau = 0; tau < max_lag; ++tau) {
    float sum = 0.0f;
    int window = frame_length - max_lag;
    for (int j = 0; j < window; ++j) {
      float delta = frame[j] - frame[j + tau];
      sum += delta * delta;
    }
    diff[tau] = sum;
  }

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
    if (running_sum > 1e-10f) {
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
  int n_samples = static_cast<int>(audio.size());
  int n_frames = 1 + (n_samples - config.frame_length) / config.hop_length;

  if (n_frames <= 0) {
    return PitchResult();
  }

  PitchResult result;
  result.f0.resize(n_frames);
  result.voiced_prob.resize(n_frames);
  result.voiced_flag.resize(n_frames);

  const float* data = audio.data();

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
  int n_samples = static_cast<int>(audio.size());
  int n_frames = 1 + (n_samples - config.frame_length) / config.hop_length;

  if (n_frames <= 0) {
    return PitchResult();
  }

  // Convert frequency to period
  int min_period = static_cast<int>(std::floor(static_cast<float>(sr) / config.fmax));
  int max_period = static_cast<int>(std::ceil(static_cast<float>(sr) / config.fmin));
  max_period = std::min(max_period, config.frame_length / 2);

  if (min_period >= max_period) {
    return PitchResult();
  }

  // Number of pitch candidates per frame
  constexpr int kMaxCandidates = 20;

  // Beta distribution parameters for pYIN
  constexpr float kBetaAlpha = 1.0f;
  constexpr float kBetaBeta = 18.0f;

  // Transition probability parameters
  constexpr float kSelfTransition = 0.99f;
  constexpr float kVoicedToUnvoiced = 0.01f;
  constexpr float kUnvoicedToVoiced = 0.01f;

  const float* data = audio.data();

  // Step 1: Extract pitch candidates for each frame
  struct Candidate {
    float period;
    float probability;
  };

  std::vector<std::vector<Candidate>> frame_candidates(n_frames);

  for (int i = 0; i < n_frames; ++i) {
    int start = i * config.hop_length;

    // Compute CMNDF
    std::vector<float> diff = yin_difference(data + start, config.frame_length, max_period + 1);
    std::vector<float> cmndf = yin_cmndf(diff);

    // Find all local minima as pitch candidates
    std::vector<Candidate> candidates;

    for (int tau = min_period + 1; tau < max_period - 1; ++tau) {
      // Local minimum check
      if (cmndf[tau] < cmndf[tau - 1] && cmndf[tau] <= cmndf[tau + 1]) {
        // Calculate probability using beta distribution
        float prob = beta_pdf(cmndf[tau], kBetaAlpha, kBetaBeta);
        if (prob > 1e-6f) {
          // Parabolic interpolation
          float offset = parabolic_interp(cmndf[tau - 1], cmndf[tau], cmndf[tau + 1]);
          float period = static_cast<float>(tau) + offset;

          candidates.push_back({period, prob});
        }
      }
    }

    // Add unvoiced candidate
    candidates.push_back({0.0f, 0.01f});

    // Sort by probability and keep top candidates
    std::sort(candidates.begin(), candidates.end(),
              [](const Candidate& a, const Candidate& b) { return a.probability > b.probability; });

    if (candidates.size() > kMaxCandidates) {
      candidates.resize(kMaxCandidates);
    }

    // Normalize probabilities
    float sum = 0.0f;
    for (const auto& c : candidates) {
      sum += c.probability;
    }
    if (sum > 0.0f) {
      for (auto& c : candidates) {
        c.probability /= sum;
      }
    }

    frame_candidates[i] = std::move(candidates);
  }

  // Step 2: Viterbi decoding for optimal path
  // State: candidate index for each frame
  std::vector<std::vector<float>> viterbi_prob(n_frames);
  std::vector<std::vector<int>> backtrack(n_frames);

  // Initialize first frame
  viterbi_prob[0].resize(frame_candidates[0].size());
  backtrack[0].resize(frame_candidates[0].size(), -1);
  for (size_t j = 0; j < frame_candidates[0].size(); ++j) {
    viterbi_prob[0][j] = std::log(frame_candidates[0][j].probability + 1e-10f);
  }

  // Forward pass
  for (int i = 1; i < n_frames; ++i) {
    size_t n_curr = frame_candidates[i].size();
    size_t n_prev = frame_candidates[i - 1].size();

    viterbi_prob[i].resize(n_curr);
    backtrack[i].resize(n_curr);

    for (size_t j = 0; j < n_curr; ++j) {
      float best_prob = -std::numeric_limits<float>::infinity();
      int best_prev = 0;

      bool curr_voiced = (frame_candidates[i][j].period > 0.0f);

      for (size_t k = 0; k < n_prev; ++k) {
        bool prev_voiced = (frame_candidates[i - 1][k].period > 0.0f);

        // Transition probability
        float trans_prob;
        if (prev_voiced && curr_voiced) {
          // Voiced to voiced: penalize large pitch jumps
          float prev_freq = static_cast<float>(sr) / frame_candidates[i - 1][k].period;
          float curr_freq = static_cast<float>(sr) / frame_candidates[i][j].period;
          float ratio = curr_freq / prev_freq;
          float cents = 1200.0f * std::log2(ratio);
          float jump_penalty = std::exp(-cents * cents / (2.0f * 50.0f * 50.0f));
          trans_prob = kSelfTransition * jump_penalty;
        } else if (!prev_voiced && !curr_voiced) {
          trans_prob = kSelfTransition;
        } else if (prev_voiced && !curr_voiced) {
          trans_prob = kVoicedToUnvoiced;
        } else {
          trans_prob = kUnvoicedToVoiced;
        }

        float prob = viterbi_prob[i - 1][k] + std::log(trans_prob + 1e-10f);
        if (prob > best_prob) {
          best_prob = prob;
          best_prev = static_cast<int>(k);
        }
      }

      viterbi_prob[i][j] = best_prob + std::log(frame_candidates[i][j].probability + 1e-10f);
      backtrack[i][j] = best_prev;
    }
  }

  // Backtrack to find best path
  std::vector<int> best_path(n_frames);

  // Find best final state
  float best_final = -std::numeric_limits<float>::infinity();
  int best_final_idx = 0;
  for (size_t j = 0; j < viterbi_prob[n_frames - 1].size(); ++j) {
    if (viterbi_prob[n_frames - 1][j] > best_final) {
      best_final = viterbi_prob[n_frames - 1][j];
      best_final_idx = static_cast<int>(j);
    }
  }
  best_path[n_frames - 1] = best_final_idx;

  // Backtrack
  for (int i = n_frames - 2; i >= 0; --i) {
    best_path[i] = backtrack[i + 1][best_path[i + 1]];
  }

  // Step 3: Extract results
  PitchResult result;
  result.f0.resize(n_frames);
  result.voiced_prob.resize(n_frames);
  result.voiced_flag.resize(n_frames);

  for (int i = 0; i < n_frames; ++i) {
    int idx = best_path[i];
    float period = frame_candidates[i][idx].period;
    float prob = frame_candidates[i][idx].probability;

    if (period > 0.0f) {
      result.f0[i] = static_cast<float>(sr) / period;
      result.voiced_flag[i] = true;
    } else {
      result.f0[i] = config.fill_na ? 0.0f : std::numeric_limits<float>::quiet_NaN();
      result.voiced_flag[i] = false;
    }
    result.voiced_prob[i] = prob;
  }

  return result;
}

float freq_to_midi(float freq) {
  if (freq <= 0.0f) {
    return 0.0f;
  }
  return 12.0f * std::log2(freq / 440.0f) + 69.0f;
}

float midi_to_freq(float midi) { return 440.0f * std::pow(2.0f, (midi - 69.0f) / 12.0f); }

}  // namespace sonare
