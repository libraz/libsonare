#include "core/iirt.h"

#include <algorithm>
#include <cmath>
#include <complex>

#include "rt/biquad_design.h"
#include "util/constants.h"
#include "util/exception.h"

namespace sonare {

using sonare::constants::kPiD;

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

/// @brief Designs a true order-@p order Butterworth bandpass as a cascade of
///        @p order/2 distinct biquad sections (matching `scipy.signal.iirfilter`
///        with `ftype='butter'`, which `librosa.iirt` uses).
/// @details An order-@p order Butterworth bandpass has @p order/2 lowpass-prototype
///          poles. Each prototype pole maps, under the analog lowpass-to-bandpass
///          transform `s -> (s^2 + wo^2) / (BW * s)`, to a conjugate pair of bandpass
///          poles; that pair is realized as one real second-order section. The
///          resulting sections have *distinct* center frequencies and Q values whose
///          distribution follows the maximally-flat Butterworth prototype, so the
///          cascade has the correct passband shape and -6*order dB/oct skirts (unlike
///          a stack of identical RBJ sections, which over-narrows the passband and
///          gives the wrong rolloff for @p order >= 4).
/// @param f0 Center frequency in Hz.
/// @param band_q Overall band quality factor (f0 / bandwidth).
/// @param sr Sample rate in Hz.
/// @param order Even filter order (>= 2). @p order/2 sections are returned.
/// @note At @p order == 2 this reduces to a single bandpass section equivalent to
///       the historical RBJ design (constant-skirt-gain bandpass at @p f0, @p band_q).
std::vector<BiquadCoeffs> design_butterworth_bandpass(double f0, double band_q, double sr,
                                                      int order) {
  const int n_sections = order / 2;  // == N prototype poles == N output biquads
  const int N = n_sections;
  std::vector<BiquadCoeffs> sos;
  sos.reserve(static_cast<size_t>(N));

  // Analog band edges from center frequency and Q (bandwidth = f0 / band_q),
  // pre-warped for the bilinear transform so the digital band edges land correctly.
  const double bandwidth = f0 / band_q;
  const double f_lo = f0 - 0.5 * bandwidth;
  const double f_hi = f0 + 0.5 * bandwidth;
  const double fs2 = 2.0 * sr;  // bilinear pre-scale (2*fs)
  auto warp = [&](double f) { return fs2 * std::tan(kPiD * f / sr); };
  const double wlo = warp(f_lo);
  const double whi = warp(f_hi);
  const double wo2 = wlo * whi;  // analog center frequency squared
  const double bw = whi - wlo;   // analog bandwidth (rad/s)

  // Collect all 2*N analog bandpass poles. Each Butterworth lowpass-prototype pole
  //   s_k = exp(j * pi * (2k + 1 + N) / (2N)),  k = 0 .. N-1
  // maps under s -> (s^2 + wo^2)/(BW*s) to the two roots of
  //   s^2 - (BW * s_k) * s + wo^2 = 0.
  std::vector<std::complex<double>> poles;
  poles.reserve(static_cast<size_t>(2 * N));
  for (int k = 0; k < N; ++k) {
    const double theta = kPiD * (2.0 * k + 1.0 + N) / (2.0 * N);
    const std::complex<double> p_lp(std::cos(theta), std::sin(theta));
    const std::complex<double> half = 0.5 * bw * p_lp;
    const std::complex<double> disc = std::sqrt(half * half - wo2);
    poles.push_back(half + disc);
    poles.push_back(half - disc);
  }

  // Keep the N poles in the upper half plane (Im >= 0); each, with its conjugate,
  // forms one real second-order section. This is the standard SOS factorization of
  // a conjugate-symmetric pole set. Sort by |Im| so we deterministically pick one
  // representative per conjugate pair.
  std::sort(poles.begin(), poles.end(),
            [](const std::complex<double>& a, const std::complex<double>& b) {
              return a.imag() > b.imag();
            });

  // The bandpass numerator contributes one zero at s = 0 per section (N zeros at
  // DC, N at infinity across the cascade) — the Butterworth bandpass shape.
  for (int s = 0; s < N; ++s) {
    const std::complex<double>& p = poles[static_cast<size_t>(s)];
    // Real analog denominator from the conjugate pair {p, conj(p)}:
    //   (s - p)(s - conj(p)) = s^2 - 2*Re(p)*s + |p|^2.
    const double a_sum = -2.0 * p.real();
    const double a_prod = std::norm(p);  // |p|^2

    // Bilinear transform z = (fs2 + s)/(fs2 - s) of H(s) = s / (s^2 + a_sum*s + a_prod).
    const double az0 = fs2 * fs2 + a_sum * fs2 + a_prod;
    const double az1 = 2.0 * (a_prod - fs2 * fs2);
    const double az2 = fs2 * fs2 - a_sum * fs2 + a_prod;
    // numerator s -> fs2 * (z^2 - 1): b = [fs2, 0, -fs2].
    BiquadCoeffs c;
    c.b0 = fs2 / az0;
    c.b1 = 0.0;
    c.b2 = -fs2 / az0;
    c.a1 = az1 / az0;
    c.a2 = az2 / az0;
    sos.push_back(c);
  }

  // Normalize the cascade to unity gain at the center frequency f0, matching
  // scipy.signal.iirfilter (Butterworth bandpass peaks at 0 dB at the band center).
  const double w0 = constants::kTwoPiD * f0 / sr;
  const std::complex<double> z(std::cos(w0), std::sin(w0));
  const std::complex<double> z2 = z * z;
  std::complex<double> h(1.0, 0.0);
  for (const BiquadCoeffs& c : sos) {
    const std::complex<double> num = c.b0 * z2 + c.b1 * z + c.b2;
    const std::complex<double> den = z2 + c.a1 * z + c.a2;
    h *= num / den;
  }
  const double mag = std::abs(h);
  if (mag > 0.0 && std::isfinite(mag) && !sos.empty()) {
    const double scale = 1.0 / mag;
    sos[0].b0 *= scale;
    sos[0].b1 *= scale;
    sos[0].b2 *= scale;
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
  // Mirror frame_rms exactly: it pads win_length/2 on each side when centered,
  // so the total padded length is 2*(win_length/2), which differs from
  // win_length for odd win_length. Using the same expression here keeps this
  // predicted frame count identical to what frame_rms actually produces (no
  // off-by-one for odd win_length).
  const int half = win_length / 2;
  const int pad = center ? 2 * half : 0;
  const int total = n + pad;
  return (total >= win_length) ? 1 + (total - win_length) / hop_length : 1;
}

}  // namespace

std::vector<float> iirt(const float* y, size_t n_samples, const IirtConfig& config) {
  if (n_samples > 0 && y == nullptr)
    throw SonareException(ErrorCode::InvalidParameter, "iirt: y is null");
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

  const double tuning_factor =
      std::pow(2.0, static_cast<double>(config.tuning) /
                        static_cast<double>(constants::kSemitonesPerOctave));
  const double sr = static_cast<double>(config.sr);
  const double nyquist = 0.5 * sr;

  // Generate center frequencies based on MIDI -> Hz: f = 440 * 2^((m - 69)/12).
  std::vector<double> centers(config.n_filters);
  for (int i = 0; i < config.n_filters; ++i) {
    double midi = static_cast<double>(config.midi_start + i);
    centers[i] = static_cast<double>(constants::kA4Hz) *
                 std::pow(2.0, (midi - static_cast<double>(constants::kMidiA4)) /
                                   static_cast<double>(constants::kSemitonesPerOctave)) *
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
