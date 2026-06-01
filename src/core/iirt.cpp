#include "core/iirt.h"

#include <algorithm>
#include <cmath>

#include "rt/biquad_design.h"
#include "util/constants.h"
#include "util/exception.h"

namespace sonare {

namespace {

struct BiquadCoeffs {
  double b0, b1, b2, a1, a2;
};

/// @brief Per-section runtime state (Direct Form I delay elements).
struct BiquadState {
  double x1 = 0.0, x2 = 0.0, y1 = 0.0, y2 = 0.0;
};

/// @brief Reports whether a normalized biquad (a0 == 1) is stable, i.e. its poles lie
///        strictly inside the unit circle (Schur–Cohn condition for a 2nd-order section).
bool is_stable(const BiquadCoeffs& c) {
  return std::abs(c.a2) < 1.0 && std::abs(c.a1) < 1.0 + c.a2;
}

/// @brief Designs an order-@p order bandpass as a cascade of @p order/2 IDENTICAL RBJ
///        bandpass biquads centered on @p f0, each with the raw band quality factor
///        @p band_q (no per-section Q scaling).
/// @details Each section uses the shared RBJ constant-skirt-gain bandpass design;
///          cascading @p order/2 of
///          them yields a steeper, stable skirt while keeping the peak fixed at @p f0.
///          At @p order == 2 this reduces exactly to a single RBJ bandpass with the
///          requested Q, matching the historical default behaviour bit-for-bit.
/// @note This is not yet a true elliptic/Butterworth-bandpass match to `librosa.iirt`
///       (whose SOS chain has distinct per-section coefficients); a full match is
///       future work.
std::vector<BiquadCoeffs> design_butterworth_bandpass(double f0, double band_q, double sr,
                                                      int order) {
  const int n_sections = order / 2;
  std::vector<BiquadCoeffs> sos;
  sos.reserve(static_cast<size_t>(n_sections));
  const auto coeffs = rt::rbj_bandpass_d(f0, sr, band_q);
  const BiquadCoeffs section{coeffs.b0, coeffs.b1, coeffs.b2, coeffs.a1, coeffs.a2};
  for (int k = 0; k < n_sections; ++k) {
    sos.push_back(section);
  }
  return sos;
}

/// @brief Applies a cascade of biquad bandpass sections to @p y. Each section keeps its
///        own state so the chain is a true higher-order filter, not a repeated 2nd order.
std::vector<double> apply_cascade(const float* y, size_t n, const std::vector<BiquadCoeffs>& sos) {
  std::vector<double> out(n);
  std::vector<BiquadState> st(sos.size());
  for (size_t i = 0; i < n; ++i) {
    double v = static_cast<double>(y[i]);
    for (size_t s = 0; s < sos.size(); ++s) {
      const BiquadCoeffs& c = sos[s];
      BiquadState& z = st[s];
      const double x0 = v;
      const double y0 = c.b0 * x0 + c.b1 * z.x1 + c.b2 * z.x2 - c.a1 * z.y1 - c.a2 * z.y2;
      z.x2 = z.x1;
      z.x1 = x0;
      z.y2 = z.y1;
      z.y1 = y0;
      v = y0;
    }
    out[i] = v;
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

int frame_count_for_iirt(size_t n_samples, int win_length, int hop_length, bool center) {
  const int n = static_cast<int>(n_samples);
  const int pad = center ? win_length : 0;
  const int total = n + pad;
  return (total >= win_length) ? 1 + (total - win_length) / hop_length : 1;
}

}  // namespace

std::vector<float> iirt(const float* y, size_t n_samples, const IirtConfig& config) {
  if (y == nullptr) throw SonareException(ErrorCode::InvalidParameter, "iirt: y is null");
  if (config.sr <= 0 || config.hop_length <= 0 || config.win_length <= 0) {
    throw SonareException(ErrorCode::InvalidParameter,
                          "iirt: sr, hop_length, and win_length must be positive");
  }
  if (config.n_filters <= 0)
    throw SonareException(ErrorCode::InvalidParameter, "iirt: n_filters must be positive");
  if (config.Q <= 0.0f)
    throw SonareException(ErrorCode::InvalidParameter, "iirt: Q must be positive");
  if (config.filter_order < 2 || config.filter_order % 2 != 0) {
    throw SonareException(ErrorCode::InvalidParameter, "iirt: filter_order must be even and >= 2");
  }
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

  const int n_frames_global =
      frame_count_for_iirt(n_samples, config.win_length, config.hop_length, config.center);
  std::vector<float> out;
  std::vector<std::vector<float>> rows;
  rows.reserve(config.n_filters);
  for (int i = 0; i < config.n_filters; ++i) {
    double fc = centers[i];
    std::vector<float> row;
    std::vector<BiquadCoeffs> sos =
        (fc < nyquist && fc > 0.0) ? design_butterworth_bandpass(fc, static_cast<double>(config.Q),
                                                                 sr, config.filter_order)
                                   : std::vector<BiquadCoeffs>{};
    const bool stable =
        !sos.empty() &&
        std::all_of(sos.begin(), sos.end(), [](const BiquadCoeffs& c) { return is_stable(c); });
    if (!stable) {
      // Out of band or numerically unstable design — leave the band unused (zeros).
      row.assign(n_frames_global, 0.0f);
    } else {
      std::vector<double> band = apply_cascade(y, n_samples, sos);
      row = frame_rms(band, config.win_length, config.hop_length, config.center);
    }
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
  return out;
}

std::vector<float> iirt(const Audio& audio, const IirtConfig& config) {
  IirtConfig cfg = config;
  cfg.sr = audio.sample_rate();
  return iirt(audio.data(), audio.size(), cfg);
}

}  // namespace sonare
