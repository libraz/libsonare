#include "filters/wavelet.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <string>

#include "core/window.h"
#include "util/constants.h"
#include "util/exception.h"
#include "util/math_utils.h"
#include "util/numeric_validation.h"
#include "util/parameter_checks.h"

namespace sonare {

namespace detail {

// alpha is the bandwidth divisor in length = filter_scale * sr / (alpha * f + gamma).
namespace {

constexpr float kAlphaFloor = 1e-6f;

void validate_frequencies(const std::vector<float>& freqs) {
  for (size_t i = 0; i < freqs.size(); ++i) {
    if (std::isfinite(freqs[i]) && freqs[i] > 0.0f) continue;
    util::check::greater_than(freqs[i], 0.0, ("freqs[" + std::to_string(i) + "]").c_str());
  }
}

// The relative bandwidth of a bin whose local spacing is @p bpo bins per octave.
float bandwidth_at_spacing(float bpo) {
  const float t = std::pow(2.0f, 2.0f / bpo);
  return std::max((t - 1.0f) / (t + 1.0f), kAlphaFloor);
}

}  // namespace

float geometric_relative_bandwidth(int bins_per_octave) {
  return bandwidth_at_spacing(static_cast<float>(bins_per_octave));
}

std::vector<float> relative_bandwidths(const std::vector<float>& freqs) {
  constexpr float kLogGuard = 1e-9f;

  std::vector<float> alpha(freqs.size(), 1.0f);
  validate_frequencies(freqs);
  if (freqs.size() < 2) return alpha;

  std::vector<float> logf(freqs.size(), 0.0f);
  for (size_t i = 0; i < freqs.size(); ++i) {
    logf[i] = std::log2(std::max(freqs[i], kLogGuard));
  }

  std::vector<float> bpo(freqs.size(), 0.0f);
  bpo[0] = 1.0f / std::max(logf[1] - logf[0], kLogGuard);
  bpo[freqs.size() - 1] =
      1.0f / std::max(logf[freqs.size() - 1] - logf[freqs.size() - 2], kLogGuard);
  for (size_t k = 1; k + 1 < freqs.size(); ++k) {
    bpo[k] = 2.0f / std::max(logf[k + 1] - logf[k - 1], kLogGuard);
  }
  for (size_t k = 0; k < freqs.size(); ++k) alpha[k] = bandwidth_at_spacing(bpo[k]);
  return alpha;
}

}  // namespace detail

std::vector<float> wavelet_lengths(const std::vector<float>& freqs, int sr, float window_param,
                                   float Q) {
  util::check::integer_within<int>(sr, kWaveletSampleRateDomain, "sample_rate");
  util::check::greater_than(window_param, 0.0, "window_param");
  util::check::at_least(Q, 0.0, "Q");
  const bool use_Q = (Q > 0.0f);
  if (!use_Q && freqs.size() < 2) {
    util::check::refuse_relation({"freqs", "Q"},
                                 "{0} must hold at least two frequencies when {1} is 0");
  }
  detail::validate_frequencies(freqs);

  const std::vector<float> alpha =
      use_Q ? std::vector<float>() : detail::relative_bandwidths(freqs);

  // Return raw fractional lengths to match librosa exactly. Callers that need
  // integer kernel sizes can floor/ceil as appropriate.
  std::vector<float> lengths(freqs.size(), 0.0f);
  for (size_t i = 0; i < freqs.size(); ++i) {
    if (use_Q) {
      lengths[i] = Q * static_cast<float>(sr) / freqs[i];
    } else {
      // librosa: Q = filter_scale / alpha; length = Q * sr / freq.
      lengths[i] = (window_param / alpha[i]) * static_cast<float>(sr) / freqs[i];
    }
    if (!numeric::finite_positive(lengths[i])) {
      util::check::refuse_relation({"freqs", "sample_rate", "window_param", "Q"},
                                   "{0}, {1}, {2}, and {3} must give every filter a finite length");
    }
  }
  return lengths;
}

namespace {

// Replicates Python's floor-division semantics for floats: `floor(x / 2)`.
int python_floordiv_by2(float x) { return static_cast<int>(std::floor(x * 0.5f)); }

bool checked_wavelet_bounds(float length, int& start, int& end) {
  // A float and its half are exactly representable in double, including subnormals.
  const double start_wide = std::floor(-static_cast<double>(length) * 0.5);
  const double end_wide = std::floor(static_cast<double>(length) * 0.5);
  if (!std::isfinite(start_wide) || !std::isfinite(end_wide) ||
      start_wide < static_cast<double>(std::numeric_limits<int>::lowest()) ||
      start_wide > static_cast<double>(std::numeric_limits<int>::max()) ||
      end_wide < static_cast<double>(std::numeric_limits<int>::lowest()) ||
      end_wide > static_cast<double>(std::numeric_limits<int>::max())) {
    return false;
  }
  start = python_floordiv_by2(-length);
  end = python_floordiv_by2(length);
  return true;
}

}  // namespace

namespace detail {

std::vector<std::complex<float>> wavelet_with_window(const std::vector<float>& freqs, int sr,
                                                     float window_param, bool is_cqt, bool pad_fft,
                                                     float Q, WindowType window, int* n_fft_out) {
  std::vector<float> lengths = wavelet_lengths(freqs, sr, window_param, Q);
  const size_t n_filters = freqs.size();

  // Compute the effective integer length L_k of each kernel using librosa's
  // index range  n in [floor(-ilen/2), floor(ilen/2)).
  std::vector<int> L(n_filters, 0);
  std::vector<int> n_start(n_filters, 0);
  float max_ilen = 0.0f;
  for (size_t i = 0; i < n_filters; ++i) {
    const float ilen = lengths[i];
    int s = 0;
    int e = 0;
    if (!checked_wavelet_bounds(ilen, s, e)) {
      throw SonareException(ErrorCode::InvalidParameter,
                            "derived length is outside the integer range");
    }
    const long long effective = static_cast<long long>(e) - static_cast<long long>(s);
    if (effective <= 0 || effective > std::numeric_limits<int>::max()) {
      throw SonareException(ErrorCode::InvalidParameter,
                            "effective length is outside the integer range");
    }
    L[i] = static_cast<int>(effective);
    n_start[i] = s;
    max_ilen = std::max(max_ilen, ilen);
  }

  size_t n_fft = 0;
  if (pad_fft) {
    // Empty input still reports the minimum padded FFT length.
    const double max_ceil_wide = std::max(1.0, std::ceil(static_cast<double>(max_ilen)));
    constexpr int kMaxIntPowerOfTwo = 1 << 30;
    if (!std::isfinite(max_ceil_wide) || max_ceil_wide < 1.0 ||
        max_ceil_wide > static_cast<double>(kMaxIntPowerOfTwo)) {
      throw SonareException(ErrorCode::InvalidParameter,
                            "FFT length is outside the supported range");
    }
    const int max_ceil = static_cast<int>(max_ceil_wide);
    const int fft_length = next_power_of_2(max_ceil);
    if (fft_length <= 0) {
      throw SonareException(ErrorCode::InvalidParameter,
                            "FFT length is outside the supported range");
    }
    n_fft = static_cast<size_t>(fft_length);
  }
  const size_t per_kernel = pad_fft ? n_fft : 0;

  size_t total = 0;
  if (pad_fft) {
    if (!numeric::checked_size_product(n_filters, n_fft, std::numeric_limits<size_t>::max(),
                                       &total)) {
      throw SonareException(ErrorCode::InvalidParameter, "output element count overflows size_t");
    }
  } else {
    for (int Lk : L) {
      if (!numeric::checked_add(total, static_cast<size_t>(Lk), &total)) {
        throw SonareException(ErrorCode::InvalidParameter, "output element count overflows size_t");
      }
    }
  }
  std::vector<std::complex<float>> out(total, std::complex<float>(0.0f, 0.0f));

  size_t cursor = 0;
  for (size_t i = 0; i < n_filters; ++i) {
    const int Lk = L[i];
    if (Lk <= 0) {
      if (pad_fft) cursor += per_kernel;
      continue;
    }
    const float f = freqs[i];
    const int s = n_start[i];

    // librosa.util.pad_center: left_pad = (max_len - L) // 2.
    const size_t slot_offset =
        pad_fft ? (cursor + (per_kernel - static_cast<size_t>(Lk)) / 2) : cursor;

    std::vector<float> selected_window;
    if (window != WindowType::Hann) {
      selected_window = create_window(window, Lk, /*periodic=*/true);
    }

    // Build the selected periodic-windowed complex sinusoid:
    //   sig[i] = exp(j * 2π * f * (s + i) / sr) * window(L)[i]
    // Hann is the scipy.signal.get_window default (periodic / fftbins=True):
    //   w[n] = 0.5 * (1 - cos(2π n / L)).
    float l1 = 0.0f;
    for (int n = 0; n < Lk; ++n) {
      const int idx = s + n;
      const float angle = constants::kTwoPi * f * static_cast<float>(idx) / static_cast<float>(sr);
      // scipy.signal.get_window returns [1.0] for a length-1 window; the generic
      // periodic-Hann formula would instead give 0 here (cos(0) == 1), zeroing
      // the single-sample kernel before normalization. Special-case it.
      const float w =
          window == WindowType::Hann
              ? (Lk == 1 ? 1.0f
                         : 0.5f * (1.0f - std::cos(constants::kTwoPi * static_cast<float>(n) /
                                                   static_cast<float>(Lk))))
              : selected_window[static_cast<size_t>(n)];
      const std::complex<float> v(w * std::cos(angle), w * std::sin(angle));
      out[slot_offset + n] = v;
      l1 += std::abs(v);
    }
    // L1-normalize each filter (librosa.util.normalize with norm=1).
    if (l1 > 0.0f) {
      const float inv = 1.0f / l1;
      for (int n = 0; n < Lk; ++n) out[slot_offset + n] *= inv;
    }
    // The legacy `is_cqt=false` path historically rescaled by 1/sqrt(L); keep
    // that behaviour for callers that still rely on it.
    if (!is_cqt) {
      const float scale = 1.0f / std::sqrt(static_cast<float>(Lk));
      for (int n = 0; n < Lk; ++n) out[slot_offset + n] *= scale;
    }
    cursor += pad_fft ? per_kernel : static_cast<size_t>(Lk);
  }
  if (n_fft_out) *n_fft_out = pad_fft ? static_cast<int>(n_fft) : 0;
  return out;
}

}  // namespace detail

}  // namespace sonare
