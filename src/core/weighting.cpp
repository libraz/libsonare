/// @file weighting.cpp
/// @brief Implementation of perceptual frequency weighting curves.

#include "core/weighting.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <stdexcept>

#include "core/db_convert.h"
#include "util/constants.h"

namespace sonare {

using sonare::constants::kEpsilon;

namespace {

// Clamp inputs to a tiny positive floor so that the DC bin (f = 0) yields a
// very large negative but finite weight instead of -inf. The floor is well
// below any realistic squared-frequency value the weighting curves operate on
// (smallest meaningful f_sq ≈ (1 Hz)² = 1.0), so it never affects audible
// bins.
inline double log10_safe(double v) { return std::log10(std::max(v, 1e-300)); }

double a_weight_one(double f_sq) {
  static const double c0 = 12194.217 * 12194.217;
  static const double c1 = 20.598997 * 20.598997;
  static const double c2 = 107.65265 * 107.65265;
  static const double c3 = 737.86223 * 737.86223;
  return 2.0 +
         20.0 * (log10_safe(c0) + 2.0 * log10_safe(f_sq) - log10_safe(f_sq + c0) -
                 log10_safe(f_sq + c1) - 0.5 * log10_safe(f_sq + c2) - 0.5 * log10_safe(f_sq + c3));
}

double b_weight_one(double f_sq) {
  static const double c0 = 12194.217 * 12194.217;
  static const double c1 = 20.598997 * 20.598997;
  static const double c2 = 158.48932 * 158.48932;
  return 0.17 + 20.0 * (log10_safe(c0) + 1.5 * log10_safe(f_sq) - log10_safe(f_sq + c0) -
                        log10_safe(f_sq + c1) - 0.5 * log10_safe(f_sq + c2));
}

double c_weight_one(double f_sq) {
  static const double c0 = 12194.217 * 12194.217;
  static const double c1 = 20.598997 * 20.598997;
  return 0.062 +
         20.0 * (log10_safe(c0) + log10_safe(f_sq) - log10_safe(f_sq + c0) - log10_safe(f_sq + c1));
}

double d_weight_one(double f_sq) {
  // librosa: const = [8.3046305e-3, 1018.7, 1039.6, 3136.5, 3424, 282.7, 1160]**2
  static const double c0 = 8.3046305e-3 * 8.3046305e-3;
  static const double c1 = 1018.7 * 1018.7;
  static const double c2 = 1039.6 * 1039.6;
  static const double c3 = 3136.5 * 3136.5;
  static const double c4 = 3424.0 * 3424.0;
  static const double c5 = 282.7 * 282.7;
  static const double c6 = 1160.0 * 1160.0;

  const double diff1 = (c1 - f_sq);
  const double diff2 = (c3 - f_sq);
  return 20.0 *
         (0.5 * log10_safe(f_sq) - log10_safe(c0) +
          0.5 * (log10_safe(diff1 * diff1 + c2 * f_sq) - log10_safe(diff2 * diff2 + c4 * f_sq) -
                 log10_safe(c5 + f_sq) - log10_safe(c6 + f_sq)));
}

std::vector<float> apply_curve(const std::vector<float>& freqs, double (*fn)(double),
                               float min_db) {
  std::vector<float> out(freqs.size());
  for (size_t i = 0; i < freqs.size(); ++i) {
    double f = static_cast<double>(freqs[i]);
    double w = fn(f * f);
    if (w < static_cast<double>(min_db)) w = static_cast<double>(min_db);
    out[i] = static_cast<float>(w);
  }
  return out;
}

std::string to_upper(const std::string& s) {
  std::string u(s);
  for (auto& c : u) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
  return u;
}

}  // namespace

std::vector<float> A_weighting(const std::vector<float>& freqs, float min_db) {
  return apply_curve(freqs, &a_weight_one, min_db);
}

std::vector<float> B_weighting(const std::vector<float>& freqs, float min_db) {
  return apply_curve(freqs, &b_weight_one, min_db);
}

std::vector<float> C_weighting(const std::vector<float>& freqs, float min_db) {
  return apply_curve(freqs, &c_weight_one, min_db);
}

std::vector<float> D_weighting(const std::vector<float>& freqs, float min_db) {
  return apply_curve(freqs, &d_weight_one, min_db);
}

std::vector<float> frequency_weighting(const std::vector<float>& freqs, const std::string& kind,
                                       float min_db) {
  const std::string k = to_upper(kind);
  if (k == "A") return A_weighting(freqs, min_db);
  if (k == "B") return B_weighting(freqs, min_db);
  if (k == "C") return C_weighting(freqs, min_db);
  if (k == "D") return D_weighting(freqs, min_db);
  if (k == "Z") return std::vector<float>(freqs.size(), 0.0f);
  throw std::invalid_argument("frequency_weighting: unknown kind '" + kind + "'");
}

std::vector<float> perceptual_weighting(const float* S, int n_bins, int n_frames,
                                        const std::vector<float>& freqs, const std::string& kind) {
  if (n_bins < 0 || n_frames < 0) {
    throw std::invalid_argument("perceptual_weighting: negative shape");
  }
  if (static_cast<int>(freqs.size()) != n_bins) {
    throw std::invalid_argument("perceptual_weighting: freqs size must equal n_bins");
  }
  const size_t total = static_cast<size_t>(n_bins) * static_cast<size_t>(n_frames);
  if (total > 0 && S == nullptr) {
    throw std::invalid_argument("perceptual_weighting: null spectrogram with non-zero shape");
  }

  std::vector<float> weights = frequency_weighting(freqs, kind);
  std::vector<float> S_db = power_to_db(S, total, 1.0f, constants::kEpsilon, 80.0f);

  std::vector<float> out(total);
  for (int k = 0; k < n_bins; ++k) {
    const float w = weights[k];
    const size_t row = static_cast<size_t>(k) * static_cast<size_t>(n_frames);
    for (int t = 0; t < n_frames; ++t) {
      out[row + t] = S_db[row + t] + w;
    }
  }
  return out;
}

}  // namespace sonare
