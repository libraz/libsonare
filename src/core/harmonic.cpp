#include "core/harmonic.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace sonare {

namespace {

/// @brief Linearly interpolate @p S along the frequency axis at @p target_freq.
/// @param S Spectrum [n_bins x n_frames] row-major.
/// @param freqs Bin center frequencies, monotonically increasing.
float interp_at(const float* S, int n_bins, int n_frames, const std::vector<float>& freqs,
                float target_freq, int frame, float fill_value) {
  if (target_freq < freqs.front() || target_freq > freqs.back()) return fill_value;
  // Binary-search the upper bound; clamp to [1, n_bins-1] for safe interpolation.
  auto it = std::lower_bound(freqs.begin(), freqs.end(), target_freq);
  int hi = static_cast<int>(it - freqs.begin());
  if (hi <= 0) hi = 1;
  if (hi >= n_bins) hi = n_bins - 1;
  int lo = hi - 1;
  float denom = freqs[hi] - freqs[lo];
  if (denom <= 0.0f) return S[lo * n_frames + frame];
  float t = (target_freq - freqs[lo]) / denom;
  float a = S[lo * n_frames + frame];
  float b = S[hi * n_frames + frame];
  return a + t * (b - a);
}

}  // namespace

std::vector<float> interp_harmonics(const float* x, int n_bins, int n_frames,
                                    const std::vector<float>& freqs,
                                    const std::vector<float>& harmonics) {
  if (x == nullptr) throw std::invalid_argument("interp_harmonics: x is null");
  if (n_bins <= 0 || n_frames <= 0 || harmonics.empty()) return {};
  if (static_cast<int>(freqs.size()) != n_bins) {
    throw std::invalid_argument("interp_harmonics: freqs size must equal n_bins");
  }
  const int H = static_cast<int>(harmonics.size());
  std::vector<float> out(static_cast<size_t>(H) * n_bins * n_frames, 0.0f);
  for (int h = 0; h < H; ++h) {
    float mult = harmonics[h];
    for (int k = 0; k < n_bins; ++k) {
      float target = mult * freqs[k];
      for (int t = 0; t < n_frames; ++t) {
        out[(static_cast<size_t>(h) * n_bins + k) * n_frames + t] =
            interp_at(x, n_bins, n_frames, freqs, target, t, 0.0f);
      }
    }
  }
  return out;
}

std::vector<float> interp_harmonics(const std::vector<float>& x, int n_bins, int n_frames,
                                    const std::vector<float>& freqs,
                                    const std::vector<float>& harmonics) {
  return interp_harmonics(x.data(), n_bins, n_frames, freqs, harmonics);
}

std::vector<float> salience(const float* S, int n_bins, int n_frames,
                            const std::vector<float>& freqs, const std::vector<float>& harmonics,
                            float fill_value) {
  if (S == nullptr) throw std::invalid_argument("salience: S is null");
  if (n_bins <= 0 || n_frames <= 0 || harmonics.empty()) return {};
  if (static_cast<int>(freqs.size()) != n_bins) {
    throw std::invalid_argument("salience: freqs size must equal n_bins");
  }
  std::vector<float> out(static_cast<size_t>(n_bins) * n_frames, 0.0f);
  const float weight = 1.0f / static_cast<float>(harmonics.size());
  for (int k = 0; k < n_bins; ++k) {
    for (int t = 0; t < n_frames; ++t) {
      float acc = 0.0f;
      for (float mult : harmonics) {
        float target = mult * freqs[k];
        acc += interp_at(S, n_bins, n_frames, freqs, target, t, fill_value);
      }
      out[k * n_frames + t] = acc * weight;
    }
  }
  return out;
}

std::vector<float> salience(const std::vector<float>& S, int n_bins, int n_frames,
                            const std::vector<float>& freqs, const std::vector<float>& harmonics,
                            float fill_value) {
  return salience(S.data(), n_bins, n_frames, freqs, harmonics, fill_value);
}

std::vector<float> f0_harmonics(const float* S, int n_bins, int n_frames,
                                const std::vector<float>& f0, const std::vector<float>& freqs,
                                const std::vector<float>& harmonics) {
  if (S == nullptr) throw std::invalid_argument("f0_harmonics: S is null");
  if (n_bins <= 0 || n_frames <= 0 || harmonics.empty()) return {};
  if (static_cast<int>(freqs.size()) != n_bins) {
    throw std::invalid_argument("f0_harmonics: freqs size must equal n_bins");
  }
  if (static_cast<int>(f0.size()) != n_frames) {
    throw std::invalid_argument("f0_harmonics: f0 size must equal n_frames");
  }
  const int H = static_cast<int>(harmonics.size());
  std::vector<float> out(static_cast<size_t>(H) * n_frames, 0.0f);
  for (int h = 0; h < H; ++h) {
    float mult = harmonics[h];
    for (int t = 0; t < n_frames; ++t) {
      float f = f0[t];
      if (!std::isfinite(f) || f <= 0.0f) continue;
      out[h * n_frames + t] = interp_at(S, n_bins, n_frames, freqs, mult * f, t, 0.0f);
    }
  }
  return out;
}

std::vector<float> f0_harmonics(const std::vector<float>& S, int n_bins, int n_frames,
                                const std::vector<float>& f0, const std::vector<float>& freqs,
                                const std::vector<float>& harmonics) {
  return f0_harmonics(S.data(), n_bins, n_frames, f0, freqs, harmonics);
}

}  // namespace sonare
