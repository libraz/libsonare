#include "filters/wavelet.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>

#include "util/constants.h"

namespace sonare {

namespace {

constexpr float kBesselCorrection = 5.0f;  // librosa default cutoff in std devs.

}  // namespace

std::vector<float> wavelet_lengths(const std::vector<float>& freqs, int sr, float window_param) {
  if (sr <= 0) throw std::invalid_argument("wavelet_lengths: sr must be positive");
  if (window_param <= 0.0f) {
    throw std::invalid_argument("wavelet_lengths: window_param must be positive");
  }
  std::vector<float> lengths(freqs.size(), 0.0f);
  for (size_t i = 0; i < freqs.size(); ++i) {
    if (freqs[i] <= 0.0f) {
      lengths[i] = 0.0f;
      continue;
    }
    // librosa: N = ceil(window_param * sr / freq), rounded up to next odd.
    float n = window_param * static_cast<float>(sr) / freqs[i];
    int n_int = static_cast<int>(std::ceil(n));
    if (n_int % 2 == 0) ++n_int;
    lengths[i] = static_cast<float>(n_int);
  }
  return lengths;
}

std::vector<std::complex<float>> wavelet(const std::vector<float>& freqs, int sr,
                                         float window_param, bool is_cqt) {
  std::vector<float> lengths = wavelet_lengths(freqs, sr, window_param);
  size_t total = 0;
  for (float L : lengths) total += static_cast<size_t>(L);
  std::vector<std::complex<float>> out(total);
  size_t cursor = 0;
  for (size_t i = 0; i < freqs.size(); ++i) {
    int L = static_cast<int>(lengths[i]);
    if (L <= 0) continue;
    const float f = freqs[i];
    const float half = 0.5f * static_cast<float>(L - 1);
    const float sigma = static_cast<float>(L) / (kBesselCorrection * window_param);
    for (int n = 0; n < L; ++n) {
      float t = (static_cast<float>(n) - half) / static_cast<float>(sr);
      // Morlet wavelet: complex exponential with Gaussian envelope.
      float envelope =
          std::exp(-0.5f * (static_cast<float>(n) - half) *
                   (static_cast<float>(n) - half) / (sigma * sigma));
      float angle = constants::kTwoPi * f * t;
      out[cursor + n] =
          std::complex<float>(envelope * std::cos(angle), envelope * std::sin(angle));
      if (!is_cqt) {
        // scipy.signal.morlet2 normalises by 1/sqrt(L).
        out[cursor + n] /= std::sqrt(static_cast<float>(L));
      }
    }
    cursor += static_cast<size_t>(L);
  }
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
    const double fc = static_cast<double>(fmin) * std::pow(2.0, static_cast<double>(i) /
                                                                     bins_per_octave);
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
      const float d = (direction == 0) ? static_cast<float>(r - (n - 1 - c))
                                       : static_cast<float>(r - c);
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
