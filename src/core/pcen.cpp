#include "core/pcen.h"

#include <cmath>

#include "util/exception.h"

namespace sonare {

namespace {

float derive_b(float time_constant, int sr, int hop_length) {
  // librosa: T = time_constant * sr / hop_length, b = (sqrt(1 + 4 T^2) - 1) / (2 T^2)
  if (time_constant <= 0.0f || sr <= 0 || hop_length <= 0) {
    // Signal the bad parameters rather than substituting a magic coefficient
    // that happens to pass the (0,1] range check and silently mis-normalizes.
    throw SonareException(ErrorCode::InvalidParameter,
                          "pcen: time_constant, sr, and hop_length must be positive");
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
  if (!std::isfinite(config.time_constant) || !std::isfinite(config.gain) ||
      !std::isfinite(config.bias) || !std::isfinite(config.power) || !std::isfinite(config.eps)) {
    throw SonareException(ErrorCode::InvalidParameter,
                          "pcen: scalar configuration values must be finite");
  }
  if (config.b.size() > 1) {
    throw SonareException(ErrorCode::InvalidParameter, "pcen: b must have length 0 or 1");
  }
  for (float value : config.b) {
    if (!std::isfinite(value)) {
      throw SonareException(ErrorCode::InvalidParameter, "pcen: b values must be finite");
    }
  }
  for (float value : config.zi) {
    if (!std::isfinite(value)) {
      throw SonareException(ErrorCode::InvalidParameter, "pcen: zi values must be finite");
    }
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
  if (!std::isfinite(b) || b <= 0.0f || b > 1.0f) {
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

  // Loop-invariant across every cell; `config` and `b` do not change below.
  const float one_minus_b = 1.0f - b;
  const bool logarithmic = config.power == 0.0f;
  const float bias_term = logarithmic ? 0.0f : std::pow(config.bias, config.power);

  std::vector<float> out(static_cast<size_t>(n_bins) * n_frames);
  // Bin-major: S and out are row-major [n_bins x n_frames], so both sides of a bin are one
  // contiguous row. The recursion runs along t within a bin and bins are independent, so
  // keeping t ascending per bin replays the identical operation sequence on each chain and
  // collapses the delay state to a scalar.
  for (int k = 0; k < n_bins; ++k) {
    const float* row = S + static_cast<size_t>(k) * n_frames;
    float* out_row = out.data() + static_cast<size_t>(k) * n_frames;
    float state = d[static_cast<size_t>(k)];
    for (int t = 0; t < n_frames; ++t) {
      const float s = row[t];
      // Direct-Form II Transposed AR(1) step (matches scipy.signal.lfilter).
      const float y = b * s + state;
      state = one_minus_b * y;
      const float smooth = std::pow(y + config.eps, -config.gain);
      // librosa special-cases power==0 as logarithmic compression
      // (S_out = log1p(S * smooth)); the power law would yield 1 - 1 = 0.
      out_row[t] = logarithmic ? std::log1p(s * smooth)
                               : std::pow(s * smooth + config.bias, config.power) - bias_term;
    }
  }
  return out;
}

std::vector<float> pcen(const std::vector<float>& S, int n_bins, int n_frames,
                        const PcenConfig& config) {
  return pcen(S.data(), n_bins, n_frames, config);
}

}  // namespace sonare
