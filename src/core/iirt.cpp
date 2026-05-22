#include "core/iirt.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>

#include "util/constants.h"

namespace sonare {

namespace {

struct BiquadCoeffs {
  double b0, b1, b2, a1, a2;
};

/// @brief Designs a constant-skirt-gain biquad bandpass (RBJ cookbook).
BiquadCoeffs design_bandpass(double f0, double Q, double sr) {
  const double w0 = constants::kTwoPiD * f0 / sr;
  const double cos_w = std::cos(w0);
  const double sin_w = std::sin(w0);
  const double alpha = sin_w / (2.0 * Q);

  const double a0 = 1.0 + alpha;
  const double b0 = alpha;
  const double b1 = 0.0;
  const double b2 = -alpha;
  const double a1 = -2.0 * cos_w;
  const double a2 = 1.0 - alpha;

  return {b0 / a0, b1 / a0, b2 / a0, a1 / a0, a2 / a0};
}

/// @brief Applies a cascaded biquad bandpass to @p y. Returns filtered samples.
std::vector<double> apply_bandpass(const float* y, size_t n, const BiquadCoeffs& c) {
  std::vector<double> out(n, 0.0);
  double x1 = 0.0, x2 = 0.0, y1 = 0.0, y2 = 0.0;
  for (size_t i = 0; i < n; ++i) {
    double x0 = static_cast<double>(y[i]);
    double y0 = c.b0 * x0 + c.b1 * x1 + c.b2 * x2 - c.a1 * y1 - c.a2 * y2;
    out[i] = y0;
    x2 = x1;
    x1 = x0;
    y2 = y1;
    y1 = y0;
  }
  return out;
}

/// @brief Computes the RMS of @p band per frame (length @p win_length, hop @p hop_length,
///        optional center padding).
std::vector<float> frame_rms(const std::vector<double>& band, int win_length, int hop_length,
                             bool center) {
  if (band.empty()) return {};
  const int n = static_cast<int>(band.size());
  const int half = win_length / 2;
  const int pad_left = center ? half : 0;
  const int pad_right = center ? half : 0;
  const int total = n + pad_left + pad_right;
  int n_frames = (total >= win_length) ? 1 + (total - win_length) / hop_length : 1;

  auto sample = [&](int idx) -> double {
    int j = idx - pad_left;
    if (j < 0 || j >= n) return 0.0;
    return band[j];
  };

  std::vector<float> rms(n_frames, 0.0f);
  for (int f = 0; f < n_frames; ++f) {
    int start = f * hop_length;
    double acc = 0.0;
    for (int i = 0; i < win_length; ++i) {
      double v = sample(start + i);
      acc += v * v;
    }
    rms[f] = static_cast<float>(std::sqrt(acc / static_cast<double>(win_length)));
  }
  return rms;
}

}  // namespace

std::vector<float> iirt(const float* y, size_t n_samples, const IirtConfig& config) {
  if (y == nullptr) throw std::invalid_argument("iirt: y is null");
  if (config.sr <= 0 || config.hop_length <= 0 || config.win_length <= 0) {
    throw std::invalid_argument("iirt: sr, hop_length, and win_length must be positive");
  }
  if (config.n_filters <= 0) throw std::invalid_argument("iirt: n_filters must be positive");
  if (config.Q <= 0.0f) throw std::invalid_argument("iirt: Q must be positive");
  if (n_samples == 0 || config.n_filters == 0) return {};

  const double tuning_factor = std::pow(2.0, static_cast<double>(config.tuning) / 12.0);
  const double sr = static_cast<double>(config.sr);
  const double nyquist = 0.5 * sr;

  // Generate center frequencies based on MIDI -> Hz: f = 440 * 2^((m - 69)/12).
  std::vector<double> centers(config.n_filters);
  for (int i = 0; i < config.n_filters; ++i) {
    double midi = static_cast<double>(config.midi_start + i);
    centers[i] = static_cast<double>(constants::kA4Hz) *
                 std::pow(2.0, (midi - static_cast<double>(constants::kMidiA4)) / 12.0) *
                 tuning_factor;
  }

  // Determine number of frames from the first available band.
  std::vector<float> out;
  int n_frames_global = 0;
  std::vector<std::vector<float>> rows;
  rows.reserve(config.n_filters);
  for (int i = 0; i < config.n_filters; ++i) {
    double fc = centers[i];
    std::vector<float> row;
    if (fc >= nyquist || fc <= 0.0) {
      // Out of band — zeros (band stays unused).
      row.assign(n_frames_global > 0 ? n_frames_global : 1, 0.0f);
    } else {
      BiquadCoeffs c = design_bandpass(fc, config.Q, sr);
      std::vector<double> band = apply_bandpass(y, n_samples, c);
      row = frame_rms(band, config.win_length, config.hop_length, config.center);
    }
    if (n_frames_global == 0) n_frames_global = static_cast<int>(row.size());
    if (static_cast<int>(row.size()) != n_frames_global) {
      // Resize zero-filled rows to match (only happens for early skipped bands).
      row.resize(n_frames_global, 0.0f);
    }
    rows.push_back(std::move(row));
  }

  // Pack into row-major [n_filters x n_frames_global].
  out.assign(static_cast<size_t>(config.n_filters) * static_cast<size_t>(n_frames_global), 0.0f);
  for (int i = 0; i < config.n_filters; ++i) {
    const std::vector<float>& row = rows[i];
    for (int t = 0; t < n_frames_global; ++t) {
      out[static_cast<size_t>(i) * n_frames_global + t] = row[t];
    }
  }
  (void)config.filter_order;  // 2nd-order RBJ section per band is implemented.
  return out;
}

std::vector<float> iirt(const Audio& audio, const IirtConfig& config) {
  IirtConfig cfg = config;
  cfg.sr = audio.sample_rate();
  return iirt(audio.data(), audio.size(), cfg);
}

}  // namespace sonare
