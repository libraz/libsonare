#include "filters/wavelet.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>

#include "util/constants.h"

namespace sonare {

namespace {

constexpr float kBesselCorrection = 5.0f;  // librosa default cutoff in std devs.

}  // namespace

std::vector<float> wavelet_lengths(const std::vector<float>& freqs, int sr, float window_param,
                                   float Q) {
  if (sr <= 0) throw std::invalid_argument("wavelet_lengths: sr must be positive");
  if (window_param <= 0.0f) {
    throw std::invalid_argument("wavelet_lengths: window_param must be positive");
  }
  if (Q < 0.0f) throw std::invalid_argument("wavelet_lengths: Q must be non-negative");
  const bool use_Q = (Q > 0.0f);

  // Pre-compute alpha[k] from the local bins-per-octave (matches
  // librosa.filters._relative_bandwidth). For each interior k:
  //   bpo[k] = 2 / (log2(freqs[k+1]) - log2(freqs[k-1]))
  //   alpha[k] = (2^(2/bpo) - 1) / (2^(2/bpo) + 1)
  // Edge bins reflect using a one-sided log difference. When fewer than two
  // frequencies are supplied we fall back to the legacy
  // `filter_scale * sr / freq` rule.
  std::vector<float> alpha;
  if (!use_Q && freqs.size() >= 2) {
    alpha.assign(freqs.size(), 0.0f);
    std::vector<float> bpo(freqs.size(), 0.0f);
    std::vector<float> logf(freqs.size(), 0.0f);
    for (size_t i = 0; i < freqs.size(); ++i) {
      logf[i] = std::log2(std::max(freqs[i], 1e-9f));
    }
    bpo[0] = 1.0f / std::max(logf[1] - logf[0], 1e-9f);
    bpo[freqs.size() - 1] = 1.0f / std::max(logf[freqs.size() - 1] - logf[freqs.size() - 2], 1e-9f);
    for (size_t k = 1; k + 1 < freqs.size(); ++k) {
      bpo[k] = 2.0f / std::max(logf[k + 1] - logf[k - 1], 1e-9f);
    }
    for (size_t k = 0; k < freqs.size(); ++k) {
      const float t = std::pow(2.0f, 2.0f / bpo[k]);
      alpha[k] = (t - 1.0f) / (t + 1.0f);
    }
  }

  std::vector<float> lengths(freqs.size(), 0.0f);
  for (size_t i = 0; i < freqs.size(); ++i) {
    if (freqs[i] <= 0.0f) {
      lengths[i] = 0.0f;
      continue;
    }
    float n;
    if (use_Q) {
      n = Q * static_cast<float>(sr) / freqs[i];
    } else if (!alpha.empty()) {
      // librosa: Q = filter_scale / alpha; length = Q * sr / freq.
      const float a = std::max(alpha[i], 1e-6f);
      n = (window_param / a) * static_cast<float>(sr) / freqs[i];
    } else {
      // Single-frequency fallback (no alpha available).
      n = window_param * static_cast<float>(sr) / freqs[i];
    }
    int n_int = static_cast<int>(std::ceil(n));
    if (n_int % 2 == 0) ++n_int;
    lengths[i] = static_cast<float>(n_int);
  }
  return lengths;
}

namespace {

int next_pow2(int n) {
  int p = 1;
  while (p < n) p <<= 1;
  return p;
}

}  // namespace

std::vector<std::complex<float>> wavelet(const std::vector<float>& freqs, int sr,
                                         float window_param, bool is_cqt, bool pad_fft, float Q,
                                         int* n_fft_out) {
  std::vector<float> lengths = wavelet_lengths(freqs, sr, window_param, Q);

  const size_t n_filters = freqs.size();
  size_t n_fft = 0;
  if (pad_fft) {
    int max_len = 0;
    for (float L : lengths) max_len = std::max(max_len, static_cast<int>(L));
    n_fft = static_cast<size_t>(next_pow2(std::max(max_len, 1)));
  }
  const size_t per_kernel = pad_fft ? n_fft : 0;

  size_t total = 0;
  if (pad_fft) {
    total = n_filters * n_fft;
  } else {
    for (float L : lengths) total += static_cast<size_t>(L);
  }
  std::vector<std::complex<float>> out(total, std::complex<float>(0.0f, 0.0f));

  size_t cursor = 0;
  for (size_t i = 0; i < n_filters; ++i) {
    const int L = static_cast<int>(lengths[i]);
    if (L <= 0) {
      if (pad_fft) cursor += per_kernel;
      continue;
    }
    const float f = freqs[i];
    const float half = 0.5f * static_cast<float>(L - 1);
    const float sigma = static_cast<float>(L) / (kBesselCorrection * window_param);
    // When padding, center the kernel inside its n_fft slot.
    const size_t slot_offset =
        pad_fft ? (cursor + (per_kernel - static_cast<size_t>(L)) / 2) : cursor;
    for (int n = 0; n < L; ++n) {
      const float t = (static_cast<float>(n) - half) / static_cast<float>(sr);
      const float envelope = std::exp(-0.5f * (static_cast<float>(n) - half) *
                                      (static_cast<float>(n) - half) / (sigma * sigma));
      const float angle = constants::kTwoPi * f * t;
      std::complex<float> v(envelope * std::cos(angle), envelope * std::sin(angle));
      if (!is_cqt) v /= std::sqrt(static_cast<float>(L));
      out[slot_offset + n] = v;
    }
    cursor += pad_fft ? per_kernel : static_cast<size_t>(L);
  }
  if (n_fft_out) *n_fft_out = pad_fft ? static_cast<int>(n_fft) : 0;
  return out;
}

std::vector<float> semitone_filterbank(int n_octaves, int bins_per_octave, float fmin, int sr) {
  if (n_octaves <= 0 || bins_per_octave <= 0 || sr <= 0 || fmin <= 0.0f) {
    throw std::invalid_argument("semitone_filterbank: invalid parameters");
  }
  const int n_filters = n_octaves * bins_per_octave;
  std::vector<float> out(static_cast<size_t>(n_filters) * 6, 0.0f);
  const double nyquist = 0.5 * static_cast<double>(sr);
  for (int i = 0; i < n_filters; ++i) {
    const double fc =
        static_cast<double>(fmin) * std::pow(2.0, static_cast<double>(i) / bins_per_octave);
    if (fc <= 0.0 || fc >= nyquist) {
      // Identity biquad (passes signal through unchanged).
      out[i * 6 + 0] = 1.0f;
      out[i * 6 + 3] = 1.0f;
      continue;
    }
    // RBJ constant-skirt-gain bandpass with Q = 25 (matches our iirt default).
    const double Q = 25.0;
    const double w0 = constants::kTwoPiD * fc / sr;
    const double cos_w = std::cos(w0);
    const double sin_w = std::sin(w0);
    const double alpha = sin_w / (2.0 * Q);
    out[i * 6 + 0] = static_cast<float>(alpha);
    out[i * 6 + 1] = 0.0f;
    out[i * 6 + 2] = static_cast<float>(-alpha);
    out[i * 6 + 3] = static_cast<float>(1.0 + alpha);
    out[i * 6 + 4] = static_cast<float>(-2.0 * cos_w);
    out[i * 6 + 5] = static_cast<float>(1.0 - alpha);
  }
  return out;
}

std::vector<float> cq_to_chroma(int n_input, int bins_per_octave, int n_chroma, float fmin) {
  if (n_input <= 0 || bins_per_octave <= 0 || n_chroma <= 0) {
    throw std::invalid_argument("cq_to_chroma: invalid parameters");
  }
  (void)fmin;  // Not required for the index-based mapping (matches librosa default tuning=0).
  std::vector<float> out(static_cast<size_t>(n_chroma) * n_input, 0.0f);
  for (int i = 0; i < n_input; ++i) {
    int chroma_bin = ((i % bins_per_octave) * n_chroma) / bins_per_octave;
    if (chroma_bin < 0) chroma_bin += n_chroma;
    out[chroma_bin * n_input + i] = 1.0f;
  }
  // Normalize each chroma row by its sum so rows still average correctly.
  for (int c = 0; c < n_chroma; ++c) {
    float row_sum = 0.0f;
    for (int i = 0; i < n_input; ++i) row_sum += out[c * n_input + i];
    if (row_sum > 0.0f) {
      for (int i = 0; i < n_input; ++i) out[c * n_input + i] /= row_sum;
    }
  }
  return out;
}

std::vector<float> diagonal_filter(int n, int direction, float window_param) {
  if (n <= 0) throw std::invalid_argument("diagonal_filter: n must be positive");
  if (window_param <= 0.0f) {
    throw std::invalid_argument("diagonal_filter: window_param must be positive");
  }
  std::vector<float> out(static_cast<size_t>(n) * n, 0.0f);
  for (int r = 0; r < n; ++r) {
    for (int c = 0; c < n; ++c) {
      const float d =
          (direction == 0) ? static_cast<float>(r - (n - 1 - c)) : static_cast<float>(r - c);
      out[r * n + c] = std::exp(-0.5f * d * d / (window_param * window_param));
    }
  }
  return out;
}

float window_bandwidth(const std::vector<float>& window, int /*n*/) {
  // Equivalent Noise Bandwidth: N * sum(w^2) / (sum(w))^2.
  if (window.empty()) return 0.0f;
  double sum_sq = 0.0;
  double sum = 0.0;
  for (float w : window) {
    sum_sq += static_cast<double>(w) * w;
    sum += static_cast<double>(w);
  }
  if (sum == 0.0) return 0.0f;
  return static_cast<float>(static_cast<double>(window.size()) * sum_sq / (sum * sum));
}

std::vector<float> window_sumsquare(const std::vector<float>& window, int n_frames, int hop_length,
                                    int n_fft) {
  if (hop_length <= 0) {
    throw std::invalid_argument("window_sumsquare: hop_length must be positive");
  }
  const int win_len = static_cast<int>(window.size());
  if (n_fft <= 0) n_fft = win_len;
  if (n_frames <= 0) return {};
  const int total = n_fft + hop_length * (n_frames - 1);
  std::vector<float> out(total, 0.0f);
  std::vector<float> w_sq(win_len);
  for (int i = 0; i < win_len; ++i) w_sq[i] = window[i] * window[i];
  // Window is centered inside an n_fft-length frame.
  int pad = (n_fft - win_len) / 2;
  for (int t = 0; t < n_frames; ++t) {
    int start = t * hop_length + pad;
    for (int i = 0; i < win_len; ++i) {
      int idx = start + i;
      if (idx >= 0 && idx < total) out[idx] += w_sq[i];
    }
  }
  return out;
}

}  // namespace sonare
