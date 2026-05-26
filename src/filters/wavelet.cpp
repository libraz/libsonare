#include "filters/wavelet.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>

#include "rt/biquad_design.h"
#include "util/constants.h"

namespace sonare {

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
  // Edge bins reflect using a one-sided log difference.
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

  // Return raw fractional lengths to match librosa exactly. Callers that need
  // integer kernel sizes can floor/ceil as appropriate.
  std::vector<float> lengths(freqs.size(), 0.0f);
  for (size_t i = 0; i < freqs.size(); ++i) {
    if (freqs[i] <= 0.0f) {
      lengths[i] = 0.0f;
      continue;
    }
    if (use_Q) {
      lengths[i] = Q * static_cast<float>(sr) / freqs[i];
    } else if (!alpha.empty()) {
      // librosa: Q = filter_scale / alpha; length = Q * sr / freq.
      const float a = std::max(alpha[i], 1e-6f);
      lengths[i] = (window_param / a) * static_cast<float>(sr) / freqs[i];
    } else {
      lengths[i] = window_param * static_cast<float>(sr) / freqs[i];
    }
  }
  return lengths;
}

namespace {

int next_pow2(int n) {
  int p = 1;
  while (p < n) p <<= 1;
  return p;
}

// Replicates Python's floor-division semantics for floats: `floor(x / 2)`.
int python_floordiv_by2(float x) { return static_cast<int>(std::floor(x * 0.5f)); }

}  // namespace

std::vector<std::complex<float>> wavelet(const std::vector<float>& freqs, int sr,
                                         float window_param, bool is_cqt, bool pad_fft, float Q,
                                         int* n_fft_out) {
  std::vector<float> lengths = wavelet_lengths(freqs, sr, window_param, Q);
  const size_t n_filters = freqs.size();

  // Compute the effective integer length L_k of each kernel using librosa's
  // index range  n in [floor(-ilen/2), floor(ilen/2)).
  std::vector<int> L(n_filters, 0);
  std::vector<int> n_start(n_filters, 0);
  float max_ilen = 0.0f;
  for (size_t i = 0; i < n_filters; ++i) {
    const float ilen = lengths[i];
    if (ilen <= 0.0f) continue;
    const int s = python_floordiv_by2(-ilen);
    const int e = python_floordiv_by2(ilen);
    L[i] = e - s;
    n_start[i] = s;
    max_ilen = std::max(max_ilen, ilen);
  }

  size_t n_fft = 0;
  if (pad_fft) {
    const int max_ceil = std::max(1, static_cast<int>(std::ceil(max_ilen)));
    n_fft = static_cast<size_t>(next_pow2(max_ceil));
  }
  const size_t per_kernel = pad_fft ? n_fft : 0;

  size_t total = 0;
  if (pad_fft) {
    total = n_filters * n_fft;
  } else {
    for (int Lk : L) total += static_cast<size_t>(Lk);
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

    // Build windowed complex sinusoid:
    //   sig[i] = exp(j * 2π * f * (s + i) / sr) * hann(L)[i]
    // Hann is the scipy.signal.get_window default (periodic / fftbins=True):
    //   w[n] = 0.5 * (1 - cos(2π n / L)).
    float l1 = 0.0f;
    for (int n = 0; n < Lk; ++n) {
      const int idx = s + n;
      const float angle = constants::kTwoPi * f * static_cast<float>(idx) / static_cast<float>(sr);
      const float w =
          0.5f *
          (1.0f - std::cos(constants::kTwoPi * static_cast<float>(n) / static_cast<float>(Lk)));
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
    const auto coeffs = rt::rbj_bandpass_raw_d(fc, static_cast<double>(sr), 25.0);
    out[i * 6 + 0] = static_cast<float>(coeffs.b0);
    out[i * 6 + 1] = static_cast<float>(coeffs.b1);
    out[i * 6 + 2] = static_cast<float>(coeffs.b2);
    out[i * 6 + 3] = static_cast<float>(coeffs.a0);
    out[i * 6 + 4] = static_cast<float>(coeffs.a1);
    out[i * 6 + 5] = static_cast<float>(coeffs.a2);
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
