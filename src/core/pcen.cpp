#include "core/pcen.h"

#include <cmath>

#include "util/exception.h"

namespace sonare {

namespace {

float derive_b(float time_constant, int sr, int hop_length) {
  // librosa: T = time_constant * sr / hop_length, b = (sqrt(1 + 4 T^2) - 1) / (2 T^2)
  if (time_constant <= 0.0f || sr <= 0 || hop_length <= 0) {
    return 0.025f;  // Fallback close to librosa's default at sr=22050.
  }
  double T = static_cast<double>(time_constant) * static_cast<double>(sr) /
             static_cast<double>(hop_length);
  double t2 = T * T;
  double b = (std::sqrt(1.0 + 4.0 * t2) - 1.0) / (2.0 * t2);
  return static_cast<float>(b);
}

}  // namespace

std::vector<float> pcen(const float* S, int n_bins, int n_frames, const PcenConfig& config) {
  if (S == nullptr) {
    throw SonareException(ErrorCode::InvalidParameter, "pcen: S is null");
  }
  if (n_bins <= 0 || n_frames <= 0) {
    return {};
  }
  if (config.eps <= 0.0f) {
    throw SonareException(ErrorCode::InvalidParameter, "pcen: eps must be strictly positive");
  }
  if (config.power < 0.0f) {
    throw SonareException(ErrorCode::InvalidParameter, "pcen: power must be non-negative");
  }
  if (config.bias < 0.0f) {
    throw SonareException(ErrorCode::InvalidParameter, "pcen: bias must be non-negative");
  }
  if (config.gain < 0.0f) {
    throw SonareException(ErrorCode::InvalidParameter, "pcen: gain must be non-negative");
  }

  float b =
      config.b.empty() ? derive_b(config.time_constant, config.sr, config.hop_length) : config.b[0];
  if (b <= 0.0f || b > 1.0f) {
    throw SonareException(ErrorCode::InvalidParameter,
                          "pcen: derived/explicit smoothing coefficient out of range (0,1]");
  }

  // Initial AR(1) delay state. librosa mirrors `scipy.signal.lfilter` with
  // `zi = scipy.signal.lfilter_zi([b], [1, b - 1])`, which evaluates to a
  // scalar `(1 - b)` broadcast across all bins. In Direct-Form II Transposed
  // the recursion is:
  //   y[n]   = b * x[n] + d[n],          with d[0] = zi (default: 1 - b)
  //   d[n+1] = (1 - b) * y[n]
  // This makes y[n] match `x[n]` for constant `x`, matching scipy semantics.
  std::vector<float> d(static_cast<size_t>(n_bins), 1.0f - b);
  if (!config.zi.empty()) {
    if (static_cast<int>(config.zi.size()) != n_bins) {
      throw SonareException(ErrorCode::InvalidParameter, "pcen: zi length must equal n_bins");
    }
    for (int k = 0; k < n_bins; ++k) d[k] = config.zi[k];
  }

  std::vector<float> out(static_cast<size_t>(n_bins) * n_frames);
  for (int t = 0; t < n_frames; ++t) {
    for (int k = 0; k < n_bins; ++k) {
      float s = S[k * n_frames + t];
      // Direct-Form II Transposed AR(1) step (matches scipy.signal.lfilter).
      float y = b * s + d[k];
      d[k] = (1.0f - b) * y;
      float smooth = std::pow(y + config.eps, -config.gain);
      float compressed =
          std::pow(s * smooth + config.bias, config.power) - std::pow(config.bias, config.power);
      out[k * n_frames + t] = compressed;
    }
  }
  return out;
}

std::vector<float> pcen(const std::vector<float>& S, int n_bins, int n_frames,
                        const PcenConfig& config) {
  return pcen(S.data(), n_bins, n_frames, config);
}

}  // namespace sonare
