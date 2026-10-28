#include "voicelab/metrics.h"

#include <Eigen/Dense>
#include <Eigen/Eigenvalues>
#include <algorithm>
#include <cmath>
#include <complex>

#include "core/fft.h"
#include "feature/pitch.h"
#include "util/constants.h"

namespace sonare::voicelab {

using constants::kEpsilon;
using constants::kFloorDb;
using constants::kPiD;
using constants::kTwoPiD;

namespace {

// YIN search range (D19).
constexpr double kF0MinHz = 40.0;
constexpr double kF0MaxHz = 1600.0;
// The fine pass integrates over this many periods of the coarse estimate (D19).
constexpr int kF0WindowPeriods = 4;
constexpr float kYinThreshold = 0.1f;
constexpr float kYinVoicedMax = 0.2f;
// Frames quieter than -100 dBFS RMS carry no pitch.
constexpr double kF0MinFrameRms = 1e-5;

// Formant / envelope analysis (D32): discrete all-pole fit to harmonic amplitudes read up to
// 5.5 kHz, on the 11.025 kHz-rate frequency axis, order 12.
constexpr double kDapRate = 11025.0;
constexpr double kDapMaxHz = 5500.0;
constexpr int kDapOrder = 12;
constexpr int kDapMaxIterations = 50;
constexpr double kDapTolerance = 1e-9;
// Damping of the fixed-point update, which oscillates when applied undamped.
constexpr double kDapStep = 0.5;
constexpr double kFormantMaxBandwidthHz = 400.0;
constexpr double kFormantMinHz = 50.0;
constexpr double kLsdMaxHz = 5000.0;
// Hop of the f0 track whose voiced median is the evaluator's f0.
constexpr double kEvaluatorHopS = 0.010;
// Bins either side of k * f0 searched for a harmonic's spectral peak.
constexpr int kHarmonicSearchBins = 2;

// S4: 8192-point 4-term Blackman-Harris, +-4 bins around each multiple of f0.
constexpr int kInharmonicFft = 8192;
constexpr double kInharmonicGuardBins = 4.0;
constexpr std::array<double, 4> kBlackmanHarris4 = {0.35875, 0.48829, 0.14128, 0.01168};

// Envelope-mode latency: Hann smoothing length of the squared signal, in samples.
constexpr int kEnvelopeSmoothing = 1024;
// Lags around the FFT peak that are re-evaluated exactly in double precision.
constexpr int kLatencyRefineRadius = 8;

// S5 click rule (D33): centred +-10 ms reference window without its central +-1 ms.
constexpr double kClickWindowS = 0.010;
constexpr double kClickExcludeS = 0.001;
constexpr double kClickLocalFactor = 8.0;
constexpr double kClickGlobalFactor = 0.8;

int next_pow2(std::size_t n) {
  int p = 1;
  while (static_cast<std::size_t>(p) < n) p <<= 1;
  return p;
}

/// Direct YIN difference d(tau) over w samples from x, in double precision.
double yin_diff(const float* x, int w, int tau) {
  double d = 0.0;
  for (int j = 0; j < w; ++j) {
    const double e = static_cast<double>(x[j]) - x[j + tau];
    d += e * e;
  }
  return d;
}

/// Coarse YIN period of a frame starting at x: CMNDF via FFT over w = tau_max samples.
/// @return Integer period in samples, or 0 when the frame is unvoiced.
double coarse_period(const float* x, int tau_min, int tau_max, FFT& fft, std::vector<float>& a,
                     std::vector<float>& b, std::vector<std::complex<float>>& fa,
                     std::vector<std::complex<float>>& fb, std::vector<float>& diff,
                     std::vector<float>& cmndf) {
  const int w = tau_max;
  const int len = w + tau_max + 1;
  std::fill(a.begin(), a.end(), 0.0f);
  std::fill(b.begin(), b.end(), 0.0f);
  std::copy(x, x + w, a.begin());
  std::copy(x, x + len, b.begin());
  fft.forward(a.data(), fa.data());
  fft.forward(b.data(), fb.data());
  for (int i = 0; i < fft.n_bins(); ++i) fa[static_cast<size_t>(i)] = std::conj(fa[i]) * fb[i];
  fft.inverse(fa.data(), a.data());

  std::vector<double> energy(static_cast<size_t>(len) + 1, 0.0);
  for (int i = 0; i < len; ++i) energy[i + 1] = energy[i] + static_cast<double>(x[i]) * x[i];
  const double e0 = energy[static_cast<size_t>(w)];
  if (std::sqrt(e0 / w) < kF0MinFrameRms) return 0.0;
  diff.assign(static_cast<size_t>(tau_max) + 1, 0.0f);
  for (int tau = 0; tau <= tau_max; ++tau) {
    const double et = energy[static_cast<size_t>(tau + w)] - energy[static_cast<size_t>(tau)];
    diff[static_cast<size_t>(tau)] = static_cast<float>(std::max(0.0, e0 + et - 2.0 * a[tau]));
  }
  yin_cmndf_into(diff, cmndf);

  int best = -1;
  for (int tau = tau_min; tau <= tau_max; ++tau) {
    if (cmndf[static_cast<size_t>(tau)] < kYinThreshold) {
      while (tau + 1 <= tau_max && cmndf[tau + 1] < cmndf[static_cast<size_t>(tau)]) ++tau;
      best = tau;
      break;
    }
  }
  if (best < 0) {
    best = tau_min;
    for (int tau = tau_min; tau <= tau_max; ++tau) {
      if (cmndf[static_cast<size_t>(tau)] < cmndf[static_cast<size_t>(best)]) best = tau;
    }
  }
  if (cmndf[static_cast<size_t>(best)] > kYinVoicedMax) return 0.0;
  return static_cast<double>(best);
}

/// Frame-averaged power spectrum under an 8192-point 4-term Blackman-Harris window, 50% hop.
std::vector<double> blackman_harris_power(const float* x, std::size_t n) {
  const int nfft = kInharmonicFft;
  std::vector<float> window(static_cast<size_t>(nfft));
  for (int i = 0; i < nfft; ++i) {
    const double p = kTwoPiD * i / nfft;
    window[static_cast<size_t>(i)] = static_cast<float>(
        kBlackmanHarris4[0] - kBlackmanHarris4[1] * std::cos(p) +
        kBlackmanHarris4[2] * std::cos(2.0 * p) - kBlackmanHarris4[3] * std::cos(3.0 * p));
  }
  FFT fft(nfft);
  std::vector<float> buf(static_cast<size_t>(nfft));
  std::vector<std::complex<float>> spec(static_cast<size_t>(fft.n_bins()));
  std::vector<double> power(static_cast<size_t>(fft.n_bins()), 0.0);
  const std::size_t hop = static_cast<std::size_t>(nfft / 2);
  std::size_t start = 0;
  do {
    for (int i = 0; i < nfft; ++i) {
      const std::size_t j = start + static_cast<std::size_t>(i);
      buf[static_cast<size_t>(i)] = j < n ? x[j] * window[static_cast<size_t>(i)] : 0.0f;
    }
    fft.forward(buf.data(), spec.data());
    for (int b = 0; b < fft.n_bins(); ++b) power[static_cast<size_t>(b)] += std::norm(spec[b]);
    start += hop;
  } while (start + static_cast<std::size_t>(nfft) <= n);
  return power;
}

double median(std::vector<double> v) {
  if (v.empty()) return 0.0;
  const std::size_t mid = v.size() / 2;
  std::nth_element(v.begin(), v.begin() + static_cast<std::ptrdiff_t>(mid), v.end());
  const double hi = v[mid];
  if (v.size() % 2 == 1) return hi;
  return 0.5 * (hi + *std::max_element(v.begin(), v.begin() + static_cast<std::ptrdiff_t>(mid)));
}

/// Harmonic frequencies and powers read from the spectral peaks at multiples of f0.
struct Harmonics {
  std::vector<double> hz;
  std::vector<double> power;
};

Harmonics read_harmonics(const float* x, std::size_t n, double sr, double f0, double max_hz) {
  Harmonics h;
  if (!(f0 > 0.0)) return h;
  const std::vector<double> power = blackman_harris_power(x, n);
  const auto bins = static_cast<int>(power.size());
  const auto db = [&](int b) {
    return 10.0 * std::log10(power[static_cast<size_t>(b)] + kEpsilon);
  };
  for (int k = 1; k * f0 <= max_hz; ++k) {
    const int centre = static_cast<int>(std::lround(k * f0 * kInharmonicFft / sr));
    int peak = std::max(1, centre - kHarmonicSearchBins);
    for (int b = peak; b <= centre + kHarmonicSearchBins && b < bins - 1; ++b) {
      if (power[static_cast<size_t>(b)] > power[static_cast<size_t>(peak)]) peak = b;
    }
    if (peak + 1 >= bins) break;
    // Parabolic interpolation of the peak level in dB.
    const double l = db(peak - 1);
    const double c = db(peak);
    const double r = db(peak + 1);
    const double curvature = l - 2.0 * c + r;
    const double shift = curvature < 0.0 ? 0.5 * (l - r) / curvature : 0.0;
    const double level = c - 0.25 * (l - r) * shift;
    h.hz.push_back(k * f0);
    h.power.push_back(std::pow(10.0, level / 10.0));
  }
  return h;
}

/// Median of the voiced frames of the evaluator's f0 track, or 0 when none is voiced.
double evaluator_f0(const float* x, std::size_t n, double sr) {
  const std::vector<float> track = estimate_f0_track(x, n, sr, kEvaluatorHopS);
  std::vector<double> voiced;
  for (float f : track) {
    if (f > 0.0f) voiced.push_back(f);
  }
  return median(voiced);
}

/// A(e^{jw}) = sum_k a_k e^{-jkw} of a (non-monic) all-pole denominator.
std::complex<double> denominator(const Eigen::VectorXd& a, double w) {
  std::complex<double> s = 0.0;
  for (Eigen::Index k = 0; k < a.size(); ++k)
    s += a[k] * std::polar(1.0, -w * static_cast<double>(k));
  return s;
}

/// Discrete all-pole fit (El-Jaroudi & Makhoul 1991): minimises the Itakura-Saito distance between
/// 1/|A|^2 and the given powers at the given frequencies. @return empty when underdetermined.
Eigen::VectorXd discrete_all_pole(const Harmonics& h) {
  const auto count = static_cast<Eigen::Index>(h.hz.size());
  const int p = kDapOrder;
  if (count <= p) return {};
  double mean_power = 0.0;
  for (double v : h.power) mean_power += v;
  mean_power /= static_cast<double>(count);
  if (!(mean_power > 0.0)) return {};
  std::vector<double> w(h.hz.size());
  for (std::size_t m = 0; m < w.size(); ++m) w[m] = kTwoPiD * h.hz[m] / kDapRate;

  Eigen::VectorXd r = Eigen::VectorXd::Zero(p + 1);
  for (int i = 0; i <= p; ++i) {
    for (std::size_t m = 0; m < w.size(); ++m) r[i] += h.power[m] / mean_power * std::cos(i * w[m]);
    r[i] /= static_cast<double>(count);
  }
  Eigen::MatrixXd toeplitz(p + 1, p + 1);
  for (int i = 0; i <= p; ++i) {
    for (int j = 0; j <= p; ++j) toeplitz(i, j) = r[std::abs(i - j)];
  }
  const Eigen::LDLT<Eigen::MatrixXd> solver(toeplitz);
  // Start from the autocorrelation-method solution with its gain folded in.
  Eigen::VectorXd a = solver.solve(Eigen::VectorXd::Unit(p + 1, 0));
  if (!(a[0] > 0.0)) return {};
  a /= std::sqrt(a[0]);
  // basis(k, m) = e^{-jk w_m}, shared by A(e^{jw_m}) and the right-hand side.
  Eigen::MatrixXcd basis(p + 1, count);
  for (Eigen::Index m = 0; m < count; ++m) {
    for (int k = 0; k <= p; ++k) basis(k, m) = std::polar(1.0, -k * w[static_cast<size_t>(m)]);
  }
  Eigen::VectorXd rhs(p + 1);
  for (int iter = 0; iter < kDapMaxIterations; ++iter) {
    const Eigen::VectorXcd inv =
        (basis.transpose() * a.cast<std::complex<double>>()).cwiseInverse();
    rhs = (basis * inv).real() / static_cast<double>(count);
    const Eigen::VectorXd next = a + kDapStep * (solver.solve(rhs) - a);
    const double change = (next - a).norm() / a.norm();
    a = next;
    if (change < kDapTolerance) break;
  }
  return a;
}

/// Envelope 10 log10(1/|A|^2) in dB at f_hz.
double dap_envelope_db(const Eigen::VectorXd& a, double f_hz) {
  return -10.0 * std::log10(std::norm(denominator(a, kTwoPiD * f_hz / kDapRate)) + kEpsilon);
}

/// Formant candidates of an all-pole model: complex pole pairs with narrow bandwidth, ascending.
std::vector<double> formant_candidates(const Eigen::VectorXd& a) {
  const auto p = static_cast<int>(a.size()) - 1;
  Eigen::MatrixXd companion = Eigen::MatrixXd::Zero(p, p);
  for (int k = 0; k < p; ++k) companion(0, k) = -a[k + 1] / a[0];
  for (int k = 1; k < p; ++k) companion(k, k - 1) = 1.0;
  const Eigen::EigenSolver<Eigen::MatrixXd> solver(companion, false);
  std::vector<double> out;
  const double nyquist = 0.5 * kDapRate;
  for (int k = 0; k < p; ++k) {
    std::complex<double> z = solver.eigenvalues()[k];
    if (z.imag() <= 0.0) continue;
    // A pole outside the unit circle has the same magnitude response as its reflection.
    if (std::abs(z) > 1.0) z = 1.0 / std::conj(z);
    const double f = std::arg(z) * kDapRate / kTwoPiD;
    const double bw = -std::log(std::abs(z)) * kDapRate / kPiD;
    if (bw < kFormantMaxBandwidthHz && f > kFormantMinHz && f < nyquist - kFormantMinHz) {
      out.push_back(f);
    }
  }
  std::sort(out.begin(), out.end());
  return out;
}

/// Centred Hann-weighted moving average of x^2, by FFT convolution.
std::vector<float> energy_envelope(const float* x, std::size_t n) {
  const int half = kEnvelopeSmoothing / 2;
  FFT fft(next_pow2(n + static_cast<std::size_t>(kEnvelopeSmoothing)));
  const auto nfft = static_cast<std::size_t>(fft.n_fft());
  std::vector<float> sq(nfft, 0.0f);
  for (std::size_t i = 0; i < n; ++i) sq[i] = x[i] * x[i];
  std::vector<float> kernel(nfft, 0.0f);
  double ksum = 0.0;
  for (int i = 0; i <= kEnvelopeSmoothing; ++i) {
    const double v = 0.5 - 0.5 * std::cos(kTwoPiD * i / kEnvelopeSmoothing);
    kernel[static_cast<size_t>(i)] = static_cast<float>(v);
    ksum += v;
  }
  std::vector<std::complex<float>> fs(static_cast<size_t>(fft.n_bins()));
  std::vector<std::complex<float>> fk(static_cast<size_t>(fft.n_bins()));
  fft.forward(sq.data(), fs.data());
  fft.forward(kernel.data(), fk.data());
  for (std::size_t i = 0; i < fs.size(); ++i) fs[i] *= fk[i];
  fft.inverse(fs.data(), sq.data());
  // The kernel starts at index 0, so the centred value sits half a kernel later.
  std::vector<float> out(n);
  for (std::size_t i = 0; i < n; ++i) {
    out[i] = static_cast<float>(sq[i + static_cast<std::size_t>(half)] / ksum);
  }
  return out;
}

}  // namespace

std::vector<float> estimate_f0_track(const float* x, std::size_t n, double sr, double hop_s,
                                     std::vector<bool>* voiced) {
  const std::size_t hop =
      std::max<std::size_t>(1, static_cast<std::size_t>(std::lround(hop_s * sr)));
  const std::size_t frames = n == 0 ? 0 : (n - 1) / hop + 1;
  std::vector<float> f0(frames, 0.0f);
  if (voiced != nullptr) voiced->assign(frames, false);

  const int tau_min = static_cast<int>(std::floor(sr / kF0MaxHz));
  const int tau_max = static_cast<int>(std::ceil(sr / kF0MinHz));
  const std::size_t coarse_len = 2 * static_cast<std::size_t>(tau_max) + 1;
  if (n < coarse_len) return f0;

  FFT fft(next_pow2(coarse_len));
  std::vector<float> a(static_cast<size_t>(fft.n_fft()));
  std::vector<float> b(static_cast<size_t>(fft.n_fft()));
  std::vector<std::complex<float>> fa(static_cast<size_t>(fft.n_bins()));
  std::vector<std::complex<float>> fb(static_cast<size_t>(fft.n_bins()));
  std::vector<float> diff;
  std::vector<float> cmndf;
  const auto frame_start = [n](std::size_t centre, std::size_t len) {
    const std::ptrdiff_t s =
        static_cast<std::ptrdiff_t>(centre) - static_cast<std::ptrdiff_t>(len / 2);
    return static_cast<std::size_t>(
        std::clamp<std::ptrdiff_t>(s, 0, static_cast<std::ptrdiff_t>(n - len)));
  };

  for (std::size_t i = 0; i < frames; ++i) {
    const std::size_t centre = i * hop;
    const double coarse = coarse_period(x + frame_start(centre, coarse_len), tau_min, tau_max, fft,
                                        a, b, fa, fb, diff, cmndf);
    if (coarse <= 0.0) continue;

    // Fine pass: direct d(tau) over four coarse periods, parabolic vertex on the raw difference.
    const int t0 = static_cast<int>(std::lround(coarse));
    const int w = kF0WindowPeriods * t0;
    const std::size_t fine_len = static_cast<std::size_t>(w + t0 + 3);
    if (fine_len > n || t0 - 2 < 1) continue;
    const float* frame = x + frame_start(centre, fine_len);
    std::array<double, 5> d{};
    for (int k = 0; k < 5; ++k) d[static_cast<size_t>(k)] = yin_diff(frame, w, t0 - 2 + k);
    int m = 1;
    for (int k = 2; k <= 3; ++k) {
      if (d[static_cast<size_t>(k)] < d[static_cast<size_t>(m)]) m = k;
    }
    const double dl = d[static_cast<size_t>(m - 1)];
    const double dc = d[static_cast<size_t>(m)];
    const double dr = d[static_cast<size_t>(m + 1)];
    const double curvature = dl - 2.0 * dc + dr;
    const double shift = curvature > 0.0 ? 0.5 * (dl - dr) / curvature : 0.0;
    const double period = (t0 - 2 + m) + std::clamp(shift, -1.0, 1.0);
    f0[i] = static_cast<float>(sr / period);
    if (voiced != nullptr) (*voiced)[i] = true;
  }
  return f0;
}

std::array<float, 3> estimate_formants(const float* x, std::size_t n, double sr) {
  std::array<float, 3> out{};
  const Harmonics h = read_harmonics(x, n, sr, evaluator_f0(x, n, sr), kDapMaxHz);
  const Eigen::VectorXd a = discrete_all_pole(h);
  if (a.size() == 0) return out;
  const std::vector<double> c = formant_candidates(a);
  for (std::size_t k = 0; k < 3 && k < c.size(); ++k) out[k] = static_cast<float>(c[k]);
  return out;
}

float log_spectral_distance_db(const float* reference, const float* test, std::size_t n, double sr,
                               float reference_warp) {
  if (!(reference_warp > 0.0f)) return -kFloorDb;
  const Harmonics ref = read_harmonics(reference, n, sr, evaluator_f0(reference, n, sr), kDapMaxHz);
  const Eigen::VectorXd a = discrete_all_pole(ref);
  const Harmonics tst = read_harmonics(test, n, sr, evaluator_f0(test, n, sr), kLsdMaxHz);
  if (a.size() == 0) return -kFloorDb;
  // Test harmonics are kept where the warped reference is read inside its own fitted band.
  std::vector<double> d;
  for (std::size_t m = 0; m < tst.hz.size(); ++m) {
    const double f_ref = tst.hz[m] / reference_warp;
    if (f_ref > kDapMaxHz) continue;
    d.push_back(10.0 * std::log10(tst.power[m] + kEpsilon) - dap_envelope_db(a, f_ref));
  }
  if (d.size() < 2) return -kFloorDb;
  // Level is removed: the distance compares envelope shape only.
  double mean = 0.0;
  for (double v : d) mean += v;
  mean /= static_cast<double>(d.size());
  double acc = 0.0;
  for (double v : d) acc += (v - mean) * (v - mean);
  return static_cast<float>(std::sqrt(acc / static_cast<double>(d.size())));
}

float inharmonic_energy_db(const float* x, std::size_t n, double sr, float f0_hz) {
  if (!(f0_hz > 0.0f)) return 0.0f;
  const std::vector<double> power = blackman_harris_power(x, n);
  const double bins_per_harmonic = static_cast<double>(f0_hz) * kInharmonicFft / sr;
  double total = 0.0;
  double off = 0.0;
  for (std::size_t b = 0; b < power.size(); ++b) {
    total += power[b];
    const double k = std::round(static_cast<double>(b) / bins_per_harmonic);
    if (std::abs(static_cast<double>(b) - k * bins_per_harmonic) > kInharmonicGuardBins) {
      off += power[b];
    }
  }
  if (total <= 0.0) return kFloorDb;
  return static_cast<float>(10.0 * std::log10(off / total + kEpsilon));
}

int estimate_latency_samples(const float* in, const float* out, std::size_t n, int max_lag,
                             bool use_envelope) {
  // Envelope mode keeps the compared span half a kernel clear of the end, where `out` is cut.
  const std::size_t guard = use_envelope ? static_cast<std::size_t>(kEnvelopeSmoothing / 2) : 0;
  if (n < 2 + guard || max_lag <= 0) return 0;
  const std::size_t lag_cap = std::min(static_cast<std::size_t>(max_lag), n - 1 - guard);
  std::vector<float> a;
  std::vector<float> b;
  if (use_envelope) {
    a = energy_envelope(in, n);
    b = energy_envelope(out, n);
  } else {
    a.assign(in, in + n);
    b.assign(out, out + n);
  }
  // Pearson correlation ignores offsets; removing them keeps the float FFT stage well conditioned.
  for (std::vector<float>* v : {&a, &b}) {
    double mean = 0.0;
    for (float s : *v) mean += s;
    mean /= static_cast<double>(n);
    for (float& s : *v) s = static_cast<float>(s - mean);
  }
  // Pearson correlation of a[0, m) against b[L, L + m): the same span of `in` at every lag.
  const std::size_t m = n - lag_cap - guard;
  double sum_a = 0.0;
  double sum_aa = 0.0;
  for (std::size_t t = 0; t < m; ++t) {
    sum_a += a[t];
    sum_aa += static_cast<double>(a[t]) * a[t];
  }
  const double mean_a = sum_a / static_cast<double>(m);
  const double sd_a = std::sqrt(std::max(0.0, sum_aa / static_cast<double>(m) - mean_a * mean_a));
  std::vector<double> sum_b(n + 1, 0.0);
  std::vector<double> sum_bb(n + 1, 0.0);
  for (std::size_t t = 0; t < n; ++t) {
    sum_b[t + 1] = sum_b[t] + b[t];
    sum_bb[t + 1] = sum_bb[t] + static_cast<double>(b[t]) * b[t];
  }
  const auto pearson = [&](std::size_t lag, double cross) {
    const double mb = (sum_b[lag + m] - sum_b[lag]) / static_cast<double>(m);
    const double vb = (sum_bb[lag + m] - sum_bb[lag]) / static_cast<double>(m) - mb * mb;
    if (!(vb > 0.0) || !(sd_a > 0.0)) return -2.0;
    return (cross / static_cast<double>(m) - mean_a * mb) / (sd_a * std::sqrt(vb));
  };

  FFT fft(next_pow2(n));
  const auto nfft = static_cast<std::size_t>(fft.n_fft());
  std::vector<float> pa(nfft, 0.0f);
  std::vector<float> pb(nfft, 0.0f);
  std::copy(a.begin(), a.begin() + static_cast<std::ptrdiff_t>(m), pa.begin());
  std::copy(b.begin(), b.end(), pb.begin());
  std::vector<std::complex<float>> fa(static_cast<size_t>(fft.n_bins()));
  std::vector<std::complex<float>> fb(static_cast<size_t>(fft.n_bins()));
  fft.forward(pa.data(), fa.data());
  fft.forward(pb.data(), fb.data());
  for (std::size_t i = 0; i < fa.size(); ++i) fa[i] = std::conj(fa[i]) * fb[i];
  fft.inverse(fa.data(), pa.data());
  std::size_t coarse = 0;
  double coarse_value = pearson(0, pa[0]);
  for (std::size_t lag = 1; lag <= lag_cap; ++lag) {
    const double r = pearson(lag, pa[lag]);
    if (r > coarse_value) {
      coarse_value = r;
      coarse = lag;
    }
  }
  // Exact double-precision correlation around the FFT peak.
  const std::size_t lo =
      coarse > static_cast<std::size_t>(kLatencyRefineRadius) ? coarse - kLatencyRefineRadius : 0;
  const std::size_t hi = std::min(lag_cap, coarse + kLatencyRefineRadius);
  std::size_t best = coarse;
  double best_value = -3.0;
  for (std::size_t lag = lo; lag <= hi; ++lag) {
    double cross = 0.0;
    for (std::size_t t = 0; t < m; ++t) cross += static_cast<double>(a[t]) * b[t + lag];
    const double r = pearson(lag, cross);
    if (r > best_value) {
      best_value = r;
      best = lag;
    }
  }
  return static_cast<int>(best);
}

int count_clicks(const float* x, std::size_t n, double sr) {
  if (n < 2) return 0;
  const auto w = static_cast<std::size_t>(std::max(1L, std::lround(kClickWindowS * sr)));
  const auto e = static_cast<std::size_t>(std::lround(kClickExcludeS * sr));
  // prefix[i] = sum of squared differences dx[1..i-1], dx[j] = x[j] - x[j-1].
  std::vector<double> prefix(n + 1, 0.0);
  for (std::size_t j = 1; j < n; ++j) {
    const double d = static_cast<double>(x[j]) - x[j - 1];
    prefix[j + 1] = prefix[j] + d * d;
  }
  const double global_floor =
      kClickGlobalFactor * std::sqrt(prefix[n] / static_cast<double>(n - 1));
  // Sum and count of dx over [lo, hi] clipped to the valid range 1..n-1.
  const auto span = [&](std::ptrdiff_t lo, std::ptrdiff_t hi, double* sum, double* count) {
    lo = std::max<std::ptrdiff_t>(lo, 1);
    hi = std::min<std::ptrdiff_t>(hi, static_cast<std::ptrdiff_t>(n) - 1);
    if (hi < lo) return;
    *sum += prefix[static_cast<size_t>(hi) + 1] - prefix[static_cast<size_t>(lo)];
    *count += static_cast<double>(hi - lo + 1);
  };
  int clicks = 0;
  for (std::size_t i = w; i < n; ++i) {
    const auto c = static_cast<std::ptrdiff_t>(i);
    const auto pw = static_cast<std::ptrdiff_t>(w);
    const auto pe = static_cast<std::ptrdiff_t>(e);
    double sum = 0.0;
    double count = 0.0;
    span(c - pw, c - pe - 1, &sum, &count);
    span(c + pe + 1, c + pw, &sum, &count);
    const double local_rms = count > 0.0 ? std::sqrt(sum / count) : 0.0;
    const double d = static_cast<double>(x[i]) - x[i - 1];
    if (std::abs(d) > std::max(kClickLocalFactor * local_rms, global_floor)) ++clicks;
  }
  return clicks;
}

}  // namespace sonare::voicelab
