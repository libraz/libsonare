#include "mastering/match/match_eq.h"

#include <algorithm>
#include <cmath>
#include <complex>
#include <limits>
#include <utility>
#include <vector>

#include "core/fft.h"
#include "core/window.h"
#include "rt/biquad_design.h"
#include "rt/partitioned_convolver.h"
#include "util/constants.h"
#include "util/db.h"
#include "util/exception.h"

namespace sonare::mastering::match {
namespace {

using sonare::constants::kDefaultDawSampleRate;
using sonare::constants::kPi;
using sonare::constants::kPiD;
using sonare::constants::kTwoPiD;

float interpolate_db(const ReferenceSpectrum& spectrum, float frequency_hz) {
  if (spectrum.frequencies.empty() || spectrum.db.empty()) {
    throw SonareException(ErrorCode::InvalidParameter, "spectrum must not be empty");
  }
  if (spectrum.frequencies.size() != spectrum.db.size()) {
    throw SonareException(ErrorCode::InvalidParameter, "spectrum size mismatch");
  }
  if (frequency_hz <= spectrum.frequencies.front()) {
    return spectrum.db.front();
  }
  if (frequency_hz >= spectrum.frequencies.back()) {
    return spectrum.db.back();
  }

  const auto upper =
      std::upper_bound(spectrum.frequencies.begin(), spectrum.frequencies.end(), frequency_hz);
  const size_t index = static_cast<size_t>(upper - spectrum.frequencies.begin());
  const float f0 = spectrum.frequencies[index - 1];
  const float f1 = spectrum.frequencies[index];
  // Interpolate dB linearly against log-frequency so spacing matches the
  // perceptual (octave) scale rather than raw Hz.
  float t;
  if (!(f0 > 0.0f) || f1 == f0) {
    t = (frequency_hz - f0) / std::max(f1 - f0, 1.0f);
  } else {
    t = static_cast<float>(std::log(static_cast<double>(frequency_hz) / f0) /
                           std::log(static_cast<double>(f1) / f0));
  }
  return spectrum.db[index - 1] + (spectrum.db[index] - spectrum.db[index - 1]) * t;
}

bool is_power_of_two(int value) { return value > 0 && (value & (value - 1)) == 0; }

float db_to_gain(float db) { return db_to_linear(db); }

/// Weight in [0, 1] applied to the curve's dB value at @p frequency_hz, so the
/// realized FIR response returns to unity outside the matched band instead of
/// extending the curve's endpoint gain to DC and Nyquist.
///
/// interpolate_db holds the endpoint value outside the curve's range, which is
/// right for reading a spectrum and wrong for realizing a filter: the parametric
/// realization places no band out there and returns to 0 dB, while the FIR path
/// would otherwise build a low-heavy reference's edge boost at DC.
///
/// One octave of raised cosine each side, narrowed on the high side when Nyquist
/// is closer so the weight reaches zero AT Nyquist — a wider taper left several
/// dB standing on the Nyquist bin. One octave rather than the whole remaining
/// span because a kernel resolves about `sample_rate / kernel_size`, and gain
/// surviving closer to DC than that is smeared onto the DC bin anyway.
///
/// The limit this cannot beat: with the band edge itself within a kernel
/// resolution of DC (the default 40 Hz edge at 48 kHz with 513 taps), no taper
/// below it is realizable and the DC bin carries roughly the curve's edge value.
/// Only a longer kernel moves that.
constexpr float kOutOfBandTaperOctaves = 1.0f;

float out_of_band_weight(const MatchEqCurve& curve, float frequency_hz, float nyquist_hz) {
  const float low = curve.frequencies.front();
  const float high = curve.frequencies.back();
  if (!(low > 0.0f) || !(high >= low)) return 1.0f;
  if (frequency_hz >= low && frequency_hz <= high) return 1.0f;
  const auto raised_cosine = [](float octaves, float width) {
    if (!(width > 0.0f) || octaves >= width) return 0.0f;
    return 0.5f * (1.0f + std::cos(kPi * octaves / width));
  };
  if (frequency_hz < low) {
    if (!(frequency_hz > 0.0f)) return 0.0f;
    return raised_cosine(std::log2(low / frequency_hz), kOutOfBandTaperOctaves);
  }
  if (!(nyquist_hz > high)) return 1.0f;  // the band already reaches Nyquist
  const float to_nyquist = std::log2(nyquist_hz / high);
  return raised_cosine(std::log2(frequency_hz / high),
                       std::min(kOutOfBandTaperOctaves, to_nyquist));
}

std::vector<float> smooth_log_frequency(const std::vector<float>& frequencies,
                                        const std::vector<float>& gain_db, int smoothing_bins) {
  if (smoothing_bins <= 0 || gain_db.size() <= 2) return gain_db;

  std::vector<float> smoothed(gain_db.size(), 0.0f);
  for (size_t i = 0; i < gain_db.size(); ++i) {
    const size_t begin =
        i > static_cast<size_t>(smoothing_bins) ? i - static_cast<size_t>(smoothing_bins) : 0;
    const size_t end = std::min(gain_db.size(), i + static_cast<size_t>(smoothing_bins) + 1);
    const double center = std::log(std::max(frequencies[i], 1.0f));
    const double radius =
        std::max(std::abs(std::log(std::max(frequencies[end - 1], 1.0f)) - center),
                 std::abs(center - std::log(std::max(frequencies[begin], 1.0f))));
    double weighted_sum = 0.0;
    double weight_sum = 0.0;
    for (size_t j = begin; j < end; ++j) {
      const double distance =
          radius > 0.0 ? std::abs(std::log(std::max(frequencies[j], 1.0f)) - center) / radius : 0.0;
      const double weight = 0.5 + 0.5 * std::cos(std::min(distance, 1.0) * kPiD);
      weighted_sum += static_cast<double>(gain_db[j]) * weight;
      weight_sum += weight;
    }
    smoothed[i] = static_cast<float>(weight_sum > 0.0 ? weighted_sum / weight_sum : gain_db[i]);
  }
  return smoothed;
}

std::vector<float> minimum_phase_kernel(const std::vector<float>& magnitude_bins, int kernel_size) {
  const size_t n_bins = magnitude_bins.size();
  const size_t n_fft = (n_bins - 1) * 2;
  FFT fft(static_cast<int>(n_fft));

  // Hermitian-symmetric, zero-phase log-magnitude spectrum (n_bins = n_fft/2+1).
  std::vector<std::complex<float>> log_magnitude_bins(n_bins);
  for (size_t k = 0; k < n_bins; ++k) {
    log_magnitude_bins[k] = {
        static_cast<float>(std::log(std::max(static_cast<double>(magnitude_bins[k]), 1e-9))), 0.0f};
  }

  // Real cepstrum: inverse of the real, symmetric log spectrum. FFT::inverse
  // applies the 1/n_fft scaling, matching the previous naive inverse DFT.
  std::vector<float> cepstrum(n_fft, 0.0f);
  fft.inverse(log_magnitude_bins.data(), cepstrum.data());

  // Fold the cepstrum so the reconstructed spectrum becomes minimum phase.
  std::vector<std::complex<float>> folded(n_fft, {0.0f, 0.0f});
  folded[0] = {cepstrum[0], 0.0f};
  for (size_t n = 1; n < n_fft / 2; ++n) {
    folded[n] = {cepstrum[n] * 2.0f, 0.0f};
  }
  folded[n_fft / 2] = {cepstrum[n_fft / 2], 0.0f};

  // Forward (unnormalized) transform of the real folded cepstrum, matching the
  // previous naive forward DFT, yields the complex (minimum-phase) log spectrum.
  std::vector<std::complex<float>> minimum_log_spectrum(n_fft);
  fft.forward_complex(folded.data(), minimum_log_spectrum.data());

  // Exponentiate. The folded cepstrum is real, so its spectrum is Hermitian
  // symmetric and the exponential preserves that symmetry; the inverse below
  // therefore produces a real impulse response.
  std::vector<std::complex<float>> minimum_spectrum_bins(n_bins);
  for (size_t k = 0; k < n_bins; ++k) {
    minimum_spectrum_bins[k] = std::exp(minimum_log_spectrum[k]);
  }
  std::vector<float> impulse(n_fft, 0.0f);
  fft.inverse(minimum_spectrum_bins.data(), impulse.data());

  std::vector<float> kernel(static_cast<size_t>(kernel_size), 0.0f);
  // Apply a decaying (half-Hann) tail taper. A minimum-phase impulse is causal
  // with its energy front-loaded, so a symmetric window (as used in the
  // zero-phase branch) would wrongly attenuate the leading energy. Tapering only
  // the tail toward zero suppresses the truncation discontinuity / ripple while
  // preserving the early response.
  for (int i = 0; i < kernel_size; ++i) {
    // Second half of a length-(2*kernel_size-1) Hann window: 1.0 at i=0 falling
    // to ~0 at i=kernel_size-1.
    const double phase = kPiD * static_cast<double>(i) / static_cast<double>(kernel_size);
    const float taper = static_cast<float>(0.5 * (1.0 + std::cos(phase)));
    kernel[static_cast<size_t>(i)] = impulse[static_cast<size_t>(i)] * taper;
  }
  return kernel;
}

int next_power_of_two(size_t value) {
  size_t result = 1;
  while (result < value) {
    result <<= 1;
  }
  return static_cast<int>(result);
}

std::vector<float> apply_fir_partitioned(const Audio& audio, const std::vector<float>& kernel,
                                         int partition_size, int latency_compensation) {
  const int block_size = partition_size > 0 ? partition_size : 256;
  sonare::rt::PartitionedConvolver convolver({block_size});
  convolver.set_impulse_response(kernel);

  const size_t padded_size = ((audio.size() + kernel.size() + static_cast<size_t>(block_size) - 1) /
                              static_cast<size_t>(block_size)) *
                             static_cast<size_t>(block_size);
  std::vector<float> input(padded_size, 0.0f);
  std::copy(audio.begin(), audio.end(), input.begin());
  std::vector<float> convolved(padded_size, 0.0f);
  for (size_t offset = 0; offset < padded_size; offset += static_cast<size_t>(block_size)) {
    convolver.process_block(input.data() + offset, convolved.data() + offset);
  }

  std::vector<float> output(audio.size(), 0.0f);
  for (size_t i = 0; i < output.size(); ++i) {
    const size_t source = i + static_cast<size_t>(std::max(latency_compensation, 0));
    output[i] = source < convolved.size() ? convolved[source] : 0.0f;
  }
  return output;
}

// Composite band-gain solver. Cascaded RBJ peaking sections add approximately in
// log magnitude, so bands packed closer than a couple of octaves reinforce each
// other: at the default 40 Hz - 18 kHz / 8 band layout the selector places bands
// 0.386 octaves apart, where two neighbours set to +6 dB already realise about
// +10.6 dB. Gains are therefore fitted to the summed response of the whole band
// set and the gain limit is applied to that summed response.

/// Symmetric probe used to measure each band's small-signal dB shape.
constexpr float kSolverProbeDb = 1.0f;
/// Upper bound on Gauss-Newton refinement passes. The regularized step converges
/// geometrically, so passes beyond this move the fit by well under 0.1 dB.
constexpr size_t kSolverMaxIterations = 8;
/// Weighted RMS residual below which the fit is considered converged.
constexpr double kSolverTargetRmsDb = 0.05;
/// Smallest residual improvement that still counts as progress.
constexpr double kSolverImprovementDb = 1.0e-4;
/// Tikhonov ridge, relative to the mean diagonal of the normal matrix.
constexpr double kSolverRidge = 1.0e-2;
/// First-difference (per squared octave) penalty, same relative scale.
constexpr double kSolverSmoothness = 3.0e-2;
/// Floor for the normal-matrix scale so a fully degenerate basis stays solvable.
constexpr double kSolverMatrixFloor = 1.0e-12;
/// Floor for the band spacing feeding the smoothness penalty, in octaves.
constexpr double kSolverMinSpacingOctaves = 1.0e-3;
/// Bisection passes used to bring the composite response under the gain limit.
/// Resolves the scale factor to 2^-16, i.e. under 0.001 dB of the largest
/// response that still fits.
constexpr int kSolverLimitSteps = 16;

/// @brief Designs one peaking section, matching ParametricEq's RBJ coefficients.
sonare::rt::BiquadCoeffs peak_section(float frequency_hz, float q, float gain_db,
                                      double sample_rate) {
  const double w0 =
      std::clamp(kTwoPiD * static_cast<double>(frequency_hz) / sample_rate, 0.0, kPiD);
  return sonare::rt::rbj_peak(static_cast<float>(w0), q, gain_db);
}

/// @brief Summed log magnitude of the cascaded sections at every grid point.
void composite_response_db(const std::vector<float>& omegas, const std::vector<float>& centres,
                           float q, const std::vector<float>& gains_db, double sample_rate,
                           std::vector<float>& response) {
  response.assign(omegas.size(), 0.0f);
  for (size_t band = 0; band < centres.size(); ++band) {
    // A 0 dB peaking section normalizes to an exact passthrough, so it can be
    // skipped without changing the sum.
    if (gains_db[band] == 0.0f) continue;
    const auto section = peak_section(centres[band], q, gains_db[band], sample_rate);
    for (size_t k = 0; k < omegas.size(); ++k) {
      response[k] += linear_to_db(sonare::rt::biquad_magnitude(section, omegas[k]));
    }
  }
}

/// @brief Small-signal dB sensitivity of every band at every grid point.
/// @details Row-major `n_points x n_bands`. RBJ peaking keeps a near
///          gain-independent shape (its Q is defined at the midpoint gain), so
///          one Jacobian measured around 0 dB serves the whole refinement.
std::vector<float> band_sensitivity(const std::vector<float>& omegas,
                                    const std::vector<float>& centres, float q,
                                    double sample_rate) {
  std::vector<float> sensitivity(omegas.size() * centres.size(), 0.0f);
  for (size_t band = 0; band < centres.size(); ++band) {
    const auto boosted = peak_section(centres[band], q, kSolverProbeDb, sample_rate);
    const auto cut = peak_section(centres[band], q, -kSolverProbeDb, sample_rate);
    for (size_t k = 0; k < omegas.size(); ++k) {
      const float up = linear_to_db(sonare::rt::biquad_magnitude(boosted, omegas[k]));
      const float down = linear_to_db(sonare::rt::biquad_magnitude(cut, omegas[k]));
      sensitivity[k * centres.size() + band] = (up - down) / (2.0f * kSolverProbeDb);
    }
  }
  return sensitivity;
}

/// @brief In-place Cholesky factorization of a symmetric positive-definite matrix.
/// @return false when the matrix is not positive definite.
bool factor_spd(std::vector<double>& matrix, size_t n) {
  for (size_t i = 0; i < n; ++i) {
    for (size_t j = 0; j <= i; ++j) {
      double sum = matrix[i * n + j];
      for (size_t k = 0; k < j; ++k) {
        sum -= matrix[i * n + k] * matrix[j * n + k];
      }
      if (i == j) {
        if (!(sum > 0.0)) return false;
        matrix[i * n + i] = std::sqrt(sum);
      } else {
        matrix[i * n + j] = sum / matrix[j * n + j];
      }
    }
  }
  return true;
}

/// @brief Forward/back substitution against a matrix already run through @ref factor_spd.
void solve_factored(const std::vector<double>& factor, size_t n, std::vector<double>& rhs) {
  for (size_t i = 0; i < n; ++i) {
    double sum = rhs[i];
    for (size_t k = 0; k < i; ++k) {
      sum -= factor[i * n + k] * rhs[k];
    }
    rhs[i] = sum / factor[i * n + i];
  }
  for (size_t i = n; i-- > 0;) {
    double sum = rhs[i];
    for (size_t k = i + 1; k < n; ++k) {
      sum -= factor[k * n + i] * rhs[k];
    }
    rhs[i] = sum / factor[i * n + i];
  }
}

/// @brief Fits band gains to the composite target curve and bounds the realised response.
/// @param frequencies Curve grid the fit and the gain limit are evaluated on.
/// @param target_db Desired composite response at each grid frequency.
/// @param centres Selected band centre frequencies, ascending.
/// @return One gain per band, in the order of @p centres.
std::vector<float> solve_band_gains(const std::vector<float>& frequencies,
                                    const std::vector<float>& target_db,
                                    const std::vector<float>& centres, float q, float max_gain_db,
                                    double sample_rate) {
  const size_t n_bands = centres.size();
  const size_t n_points = frequencies.size();
  std::vector<float> gains(n_bands, 0.0f);
  if (n_bands == 0 || n_points == 0 || !(max_gain_db > 0.0f)) {
    return gains;
  }

  std::vector<float> omegas(n_points, 0.0f);
  for (size_t k = 0; k < n_points; ++k) {
    omegas[k] = static_cast<float>(
        std::clamp(kTwoPiD * static_cast<double>(frequencies[k]) / sample_rate, 0.0, kPiD));
  }
  const std::vector<float> sensitivity = band_sensitivity(omegas, centres, q, sample_rate);

  // Weight each grid point by how much authority the band set has there. Without
  // it a target the bands cannot reach (a broad boost against a cluster of
  // low-frequency bands) drags every distant band upward to chase it.
  std::vector<double> weights(n_points, 0.0);
  double weight_sum = 0.0;
  for (size_t k = 0; k < n_points; ++k) {
    double coverage = 0.0;
    for (size_t i = 0; i < n_bands; ++i) {
      coverage += static_cast<double>(sensitivity[k * n_bands + i]);
    }
    weights[k] = std::clamp(coverage, 0.0, 1.0);
    weight_sum += weights[k];
  }
  if (!(weight_sum > 0.0)) {
    return gains;
  }

  std::vector<double> normal(n_bands * n_bands, 0.0);
  for (size_t k = 0; k < n_points; ++k) {
    if (weights[k] <= 0.0) continue;
    for (size_t i = 0; i < n_bands; ++i) {
      const double weighted = weights[k] * static_cast<double>(sensitivity[k * n_bands + i]);
      for (size_t j = 0; j < n_bands; ++j) {
        normal[i * n_bands + j] += weighted * static_cast<double>(sensitivity[k * n_bands + j]);
      }
    }
  }

  double trace = 0.0;
  for (size_t i = 0; i < n_bands; ++i) {
    trace += normal[i * n_bands + i];
  }
  const double scale = std::max(trace / static_cast<double>(n_bands), kSolverMatrixFloor);

  // Densely packed bands make the normal matrix ill conditioned. The ridge keeps
  // it invertible; the octave-normalized first-difference penalty stops the fit
  // answering with large alternating gains that cancel in magnitude while
  // wrecking the phase response. Widely spaced neighbours are barely penalized.
  for (size_t i = 0; i < n_bands; ++i) {
    normal[i * n_bands + i] += kSolverRidge * scale;
  }
  for (size_t i = 0; i + 1 < n_bands; ++i) {
    const double octaves = std::max(
        std::abs(std::log2(static_cast<double>(centres[i + 1]) / static_cast<double>(centres[i]))),
        kSolverMinSpacingOctaves);
    const double penalty = kSolverSmoothness * scale / (octaves * octaves);
    normal[i * n_bands + i] += penalty;
    normal[(i + 1) * n_bands + (i + 1)] += penalty;
    normal[i * n_bands + (i + 1)] -= penalty;
    normal[(i + 1) * n_bands + i] -= penalty;
  }

  std::vector<double> factor = normal;
  if (!factor_spd(factor, n_bands)) {
    return gains;
  }

  // Gauss-Newton against the exact composite response, reusing the single
  // small-signal Jacobian. The best iterate is kept, so a non-improving step
  // ends the refinement rather than degrading the result.
  std::vector<float> best = gains;
  std::vector<float> response;
  std::vector<double> residual(n_points, 0.0);
  std::vector<double> step(n_bands, 0.0);
  double best_error = std::numeric_limits<double>::max();
  for (size_t iteration = 0; iteration < kSolverMaxIterations; ++iteration) {
    composite_response_db(omegas, centres, q, gains, sample_rate, response);
    double error = 0.0;
    for (size_t k = 0; k < n_points; ++k) {
      residual[k] = static_cast<double>(target_db[k]) - static_cast<double>(response[k]);
      error += weights[k] * residual[k] * residual[k];
    }
    error = std::sqrt(error / weight_sum);
    if (error + kSolverImprovementDb >= best_error) break;
    best_error = error;
    best = gains;
    if (error < kSolverTargetRmsDb) break;

    std::fill(step.begin(), step.end(), 0.0);
    for (size_t k = 0; k < n_points; ++k) {
      const double weighted = weights[k] * residual[k];
      if (weighted == 0.0) continue;
      for (size_t i = 0; i < n_bands; ++i) {
        step[i] += weighted * static_cast<double>(sensitivity[k * n_bands + i]);
      }
    }
    solve_factored(factor, n_bands, step);
    for (size_t i = 0; i < n_bands; ++i) {
      gains[i] = std::clamp(gains[i] + static_cast<float>(step[i]), -max_gain_db, max_gain_db);
    }
  }
  gains = best;

  // Bound the realised response, not the coefficients. Scaling every gain by one
  // factor preserves the matched tonal shape, and bisecting that factor always
  // terminates on a feasible point because a zero scale is feasible by
  // construction.
  const auto peak_response_db = [&](const std::vector<float>& candidate) {
    composite_response_db(omegas, centres, q, candidate, sample_rate, response);
    double peak = 0.0;
    for (float value : response) {
      peak = std::max(peak, std::abs(static_cast<double>(value)));
    }
    return peak;
  };
  if (peak_response_db(gains) > static_cast<double>(max_gain_db)) {
    std::vector<float> scaled(n_bands, 0.0f);
    float low = 0.0f;
    float high = 1.0f;
    for (int pass = 0; pass < kSolverLimitSteps; ++pass) {
      const float mid = 0.5f * (low + high);
      for (size_t i = 0; i < n_bands; ++i) {
        scaled[i] = gains[i] * mid;
      }
      if (peak_response_db(scaled) <= static_cast<double>(max_gain_db)) {
        low = mid;
      } else {
        high = mid;
      }
    }
    for (size_t i = 0; i < n_bands; ++i) {
      gains[i] *= low;
    }
  }
  return gains;
}

}  // namespace

void validate_config(const MatchEqConfig& config) {
  if (config.max_bands == 0 || !std::isfinite(config.max_gain_db) || config.max_gain_db < 0.0f ||
      !std::isfinite(config.min_frequency_hz) || config.min_frequency_hz <= 0.0f ||
      !std::isfinite(config.max_frequency_hz) ||
      config.max_frequency_hz <= config.min_frequency_hz || !std::isfinite(config.q) ||
      config.q <= 0.0f || config.smoothing_bins < 0) {
    throw SonareException(ErrorCode::InvalidParameter, "invalid match EQ configuration");
  }
}

void validate_config(const MatchEqFirConfig& config) {
  if (!is_power_of_two(config.fft_size) || config.kernel_size <= 0 ||
      config.kernel_size > config.fft_size || (config.kernel_size % 2) == 0 ||
      config.partition_size < 0 ||
      (config.phase != MatchEqFirPhase::LinearPhase &&
       config.phase != MatchEqFirPhase::MinimumPhase)) {
    throw SonareException(ErrorCode::InvalidParameter, "invalid match EQ FIR configuration");
  }
}

MatchEqCurve match_eq_curve(const ReferenceSpectrum& source, const ReferenceSpectrum& reference,
                            const MatchEqConfig& config) {
  validate_config(config);
  if (source.sample_rate != reference.sample_rate) {
    throw SonareException(ErrorCode::InvalidParameter, "sample rates must match");
  }
  if (source.frequencies.empty() || source.frequencies.size() != source.db.size()) {
    throw SonareException(ErrorCode::InvalidParameter, "invalid source spectrum");
  }

  MatchEqCurve curve;
  curve.sample_rate = source.sample_rate;
  curve.frequencies.reserve(source.frequencies.size());
  curve.gain_db.reserve(source.frequencies.size());
  for (float frequency : source.frequencies) {
    if (frequency < config.min_frequency_hz || frequency > config.max_frequency_hz) continue;
    curve.frequencies.push_back(frequency);
    curve.gain_db.push_back(
        std::clamp(interpolate_db(reference, frequency) - interpolate_db(source, frequency),
                   -config.max_gain_db, config.max_gain_db));
  }

  curve.gain_db = smooth_log_frequency(curve.frequencies, curve.gain_db, config.smoothing_bins);
  return curve;
}

std::vector<float> match_eq_fir_kernel(const MatchEqCurve& curve, int sample_rate,
                                       const MatchEqFirConfig& config) {
  if (curve.frequencies.empty() || curve.frequencies.size() != curve.gain_db.size()) {
    throw SonareException(ErrorCode::InvalidParameter, "invalid match EQ curve");
  }
  if (sample_rate <= 0) {
    throw SonareException(ErrorCode::InvalidParameter, "sample_rate must be positive");
  }
  validate_config(config);

  FFT fft(config.fft_size);
  std::vector<std::complex<float>> spectrum(static_cast<size_t>(fft.n_bins()));
  std::vector<float> magnitude(static_cast<size_t>(fft.n_bins()), 1.0f);
  const ReferenceSpectrum curve_spectrum{curve.frequencies, curve.gain_db, sample_rate};
  const float nyquist = static_cast<float>(sample_rate) * 0.5f;
  for (int bin = 0; bin < fft.n_bins(); ++bin) {
    const float frequency = static_cast<float>(bin) * static_cast<float>(sample_rate) /
                            static_cast<float>(config.fft_size);
    const float gain_db =
        interpolate_db(curve_spectrum, frequency) * out_of_band_weight(curve, frequency, nyquist);
    magnitude[static_cast<size_t>(bin)] = db_to_gain(gain_db);
    spectrum[static_cast<size_t>(bin)] = {magnitude[static_cast<size_t>(bin)], 0.0f};
  }

  if (config.phase == MatchEqFirPhase::MinimumPhase) {
    return minimum_phase_kernel(magnitude, config.kernel_size);
  }

  std::vector<float> zero_phase(static_cast<size_t>(config.fft_size), 0.0f);
  fft.inverse(spectrum.data(), zero_phase.data());

  const size_t kernel_size = static_cast<size_t>(config.kernel_size);
  std::vector<float> kernel(kernel_size, 0.0f);
  const int half = config.kernel_size / 2;
  for (int i = 0; i < config.kernel_size; ++i) {
    const int source = (i - half + config.fft_size) % config.fft_size;
    // Symmetric Hann (periodic=false): FIR match-EQ taps need a symmetric window
    // for linear phase and unity DC gain.
    kernel[static_cast<size_t>(i)] =
        zero_phase[static_cast<size_t>(source)] * hann_value(i, config.kernel_size, false);
  }

  return kernel;
}

Audio apply_match_eq(const Audio& audio, const ReferenceSpectrum& source,
                     const ReferenceSpectrum& reference, const MatchEqConfig& match_config,
                     const MatchEqFirConfig& fir_config) {
  if (audio.empty()) throw SonareException(ErrorCode::InvalidParameter, "audio must not be empty");
  if (audio.sample_rate() != source.sample_rate || source.sample_rate != reference.sample_rate) {
    throw SonareException(ErrorCode::InvalidParameter, "sample rates must match");
  }

  const MatchEqCurve curve = match_eq_curve(source, reference, match_config);
  const std::vector<float> kernel = match_eq_fir_kernel(curve, audio.sample_rate(), fir_config);
  const int latency_compensation =
      fir_config.phase == MatchEqFirPhase::LinearPhase ? static_cast<int>(kernel.size() / 2) : 0;
  std::vector<float> output =
      apply_fir_partitioned(audio, kernel, fir_config.partition_size, latency_compensation);
  return Audio::from_vector(std::move(output), audio.sample_rate());
}

float estimate_reference_delay_samples(const Audio& source, const Audio& reference,
                                       int max_abs_delay) {
  if (source.empty() || reference.empty()) {
    throw SonareException(ErrorCode::InvalidParameter, "audio must not be empty");
  }
  if (source.sample_rate() != reference.sample_rate()) {
    throw SonareException(ErrorCode::InvalidParameter, "sample rates must match");
  }
  if (max_abs_delay < 0) {
    throw SonareException(ErrorCode::InvalidParameter, "max_abs_delay must be non-negative");
  }

  const size_t length = std::min(source.size(), reference.size());
  const int clamped_max_delay = static_cast<int>(
      std::min<size_t>(static_cast<size_t>(max_abs_delay), length == 0 ? 0 : length - 1));
  if (clamped_max_delay <= 0) {
    return 0.0f;
  }

  // FFT-based cross-correlation (O(N log N)) replacing the previous
  // O(N * max_delay) brute force. With s, r zero-padded to a common power-of-two
  // size, ifft(conj(FFT(s)) * FFT(r))[m] equals sum_i s[i] * r[i + m] (mod n),
  // i.e. the unnormalized cross-correlation. We map circular lags to signed lags
  // and normalize by the global signal energies so the score is comparable to
  // the previous Pearson-style correlation.
  const int n_fft = next_power_of_two(length + static_cast<size_t>(clamped_max_delay) + 1);
  FFT fft(n_fft);

  std::vector<float> source_padded(static_cast<size_t>(n_fft), 0.0f);
  std::vector<float> reference_padded(static_cast<size_t>(n_fft), 0.0f);
  double source_energy = 0.0;
  double reference_energy = 0.0;
  for (size_t i = 0; i < length; ++i) {
    const float s = source[i];
    const float r = reference[i];
    source_padded[i] = s;
    reference_padded[i] = r;
    source_energy += static_cast<double>(s) * s;
    reference_energy += static_cast<double>(r) * r;
  }
  if (source_energy <= 0.0 || reference_energy <= 0.0) {
    return 0.0f;
  }

  const int n_bins = fft.n_bins();
  std::vector<std::complex<float>> source_spectrum(static_cast<size_t>(n_bins));
  std::vector<std::complex<float>> reference_spectrum(static_cast<size_t>(n_bins));
  fft.forward(source_padded.data(), source_spectrum.data());
  fft.forward(reference_padded.data(), reference_spectrum.data());

  std::vector<std::complex<float>> product(static_cast<size_t>(n_bins));
  for (int bin = 0; bin < n_bins; ++bin) {
    product[static_cast<size_t>(bin)] = std::conj(source_spectrum[static_cast<size_t>(bin)]) *
                                        reference_spectrum[static_cast<size_t>(bin)];
  }
  std::vector<float> correlation(static_cast<size_t>(n_fft), 0.0f);
  fft.inverse(product.data(), correlation.data());

  const float inv_norm = static_cast<float>(1.0 / std::sqrt(source_energy * reference_energy));
  int best_lag = 0;
  float best_score = -1.0f;
  for (int lag = -clamped_max_delay; lag <= clamped_max_delay; ++lag) {
    // Positive lag => reference leads (index n_fft + ... wraps for negatives).
    const int index = lag >= 0 ? lag : n_fft + lag;
    const float score = correlation[static_cast<size_t>(index)] * inv_norm;
    if (score > best_score) {
      best_score = score;
      best_lag = lag;
    }
  }
  return static_cast<float>(best_lag);
}

Audio align_reference_to_source(const Audio& source, const Audio& reference, int max_abs_delay) {
  const int delay = static_cast<int>(
      std::round(estimate_reference_delay_samples(source, reference, max_abs_delay)));
  std::vector<float> aligned(source.size(), 0.0f);
  for (size_t i = 0; i < aligned.size(); ++i) {
    const int reference_index = static_cast<int>(i) + delay;
    if (reference_index >= 0 && reference_index < static_cast<int>(reference.size())) {
      aligned[i] = reference[static_cast<size_t>(reference_index)];
    }
  }
  return Audio::from_vector(std::move(aligned), source.sample_rate());
}

std::vector<eq::EqBand> match_eq_bands(const ReferenceSpectrum& source,
                                       const ReferenceSpectrum& reference,
                                       const MatchEqConfig& config) {
  if (config.max_bands == 0 || config.max_bands > eq::ParametricEq::kMaxBands) {
    throw SonareException(ErrorCode::InvalidParameter, "invalid match EQ band count");
  }
  if (!(config.max_gain_db >= 0.0f) || !(config.min_frequency_hz > 0.0f) ||
      !(config.max_frequency_hz > config.min_frequency_hz) || !(config.q > 0.0f)) {
    throw SonareException(ErrorCode::InvalidParameter, "invalid match EQ configuration");
  }
  if (source.sample_rate != reference.sample_rate) {
    throw SonareException(ErrorCode::InvalidParameter, "sample rates must match");
  }

  const MatchEqCurve curve = match_eq_curve(source, reference, config);
  return match_eq_bands_from_curve(curve, config);
}

std::vector<eq::EqBand> match_eq_bands_from_curve(const MatchEqCurve& curve,
                                                  const MatchEqConfig& config) {
  if (config.max_bands == 0 || config.max_bands > eq::EqualizerProcessor::kMaxBands) {
    throw SonareException(ErrorCode::InvalidParameter, "invalid match EQ band count");
  }
  if (!(config.max_gain_db >= 0.0f) || !(config.min_frequency_hz > 0.0f) ||
      !(config.max_frequency_hz > config.min_frequency_hz) || !(config.q > 0.0f)) {
    throw SonareException(ErrorCode::InvalidParameter, "invalid match EQ configuration");
  }
  if (curve.frequencies.size() != curve.gain_db.size()) {
    throw SonareException(ErrorCode::InvalidParameter, "invalid match EQ curve");
  }
  if (curve.frequencies.empty()) {
    return {};
  }

  struct Candidate {
    size_t index = 0;
    float strength = 0.0f;
    bool extrema = false;
  };
  std::vector<Candidate> candidates;
  candidates.reserve(curve.frequencies.size());
  for (size_t i = 0; i < curve.frequencies.size(); ++i) {
    const float frequency = curve.frequencies[i];
    if (frequency < config.min_frequency_hz || frequency > config.max_frequency_hz) {
      continue;
    }
    const float gain = curve.gain_db[i];
    bool extrema = i == 0 || i + 1 == curve.gain_db.size();
    if (i > 0 && i + 1 < curve.gain_db.size()) {
      const float prev = curve.gain_db[i - 1];
      const float next = curve.gain_db[i + 1];
      extrema = (gain >= prev && gain > next) || (gain > prev && gain >= next) ||
                (gain <= prev && gain < next) || (gain < prev && gain <= next);
    }
    candidates.push_back({i, std::abs(gain), extrema});
  }

  std::stable_sort(candidates.begin(), candidates.end(),
                   [](const Candidate& a, const Candidate& b) {
                     if (a.extrema != b.extrema) return a.extrema > b.extrema;
                     return a.strength > b.strength;
                   });

  // Corrections below this are inaudible; placing a band there wastes a slot.
  constexpr float kMinCandidateStrengthDb = 0.05f;

  std::vector<size_t> selected;
  selected.reserve(config.max_bands);
  const float min_spacing_octaves =
      std::max(0.0f, std::log2(config.max_frequency_hz / config.min_frequency_hz) /
                         static_cast<float>(std::max<size_t>(config.max_bands, 1)) * 0.35f);

  // Octave distance from a candidate to the nearest already-selected band.
  // An empty selection is infinitely far from everything.
  const auto distance_to_selection = [&](size_t index) {
    float distance = std::numeric_limits<float>::max();
    const float frequency = curve.frequencies[index];
    for (size_t existing : selected) {
      distance = std::min(distance, std::abs(std::log2(frequency / curve.frequencies[existing])));
    }
    return distance;
  };

  // Pass 0 takes real extrema in strength order: a peak or dip in the correction
  // curve is where a band belongs, so nothing overrides that ranking.
  for (const auto& candidate : candidates) {
    if (selected.size() >= config.max_bands) {
      break;
    }
    if (!candidate.extrema || candidate.strength < kMinCandidateStrengthDb) {
      continue;
    }
    if (distance_to_selection(candidate.index) >= min_spacing_octaves) {
      selected.push_back(candidate.index);
    }
  }

  // Pass 1 fills the remaining budget with the candidate that sits farthest from
  // everything already placed. Scanning the strength-sorted list in order instead
  // leaves placement at the mercy of the scan order whenever the curve offers no
  // extrema to rank by: a flat correction gives every candidate the same
  // strength, so the sort degenerates into ascending frequency and the scan takes
  // the first few. min_spacing_octaves is no defence there, since consuming the
  // whole budget at the minimum spacing still covers only 35% of the range.
  // Ties keep the strength order, so an even curve still resolves deterministically.
  while (selected.size() < config.max_bands) {
    const Candidate* best = nullptr;
    float best_distance = -1.0f;
    for (const auto& candidate : candidates) {
      if (candidate.strength < kMinCandidateStrengthDb) {
        continue;
      }
      const float distance = distance_to_selection(candidate.index);
      if (distance < min_spacing_octaves) {
        continue;
      }
      if (distance > best_distance) {
        best = &candidate;
        best_distance = distance;
      }
    }
    if (best == nullptr) {
      break;
    }
    selected.push_back(best->index);
  }
  if (selected.empty()) {
    return {};
  }

  std::sort(selected.begin(), selected.end(),
            [&](size_t a, size_t b) { return curve.frequencies[a] < curve.frequencies[b]; });

  std::vector<float> centres;
  centres.reserve(selected.size());
  for (size_t index : selected) {
    centres.push_back(curve.frequencies[index]);
  }

  // Solve every gain at once against the composite response of the placed bands,
  // then bound that response by max_gain_db. Reading each band's gain straight
  // off the curve would let overlapping neighbours stack past the limit.
  const double sample_rate =
      curve.sample_rate > 0 ? static_cast<double>(curve.sample_rate) : kDefaultDawSampleRate;
  const std::vector<float> gains = solve_band_gains(curve.frequencies, curve.gain_db, centres,
                                                    config.q, config.max_gain_db, sample_rate);

  std::vector<eq::EqBand> bands;
  bands.reserve(centres.size());
  for (size_t i = 0; i < centres.size(); ++i) {
    bands.push_back({eq::EqBandType::Peak, centres[i], gains[i], config.q, true});
  }
  return bands;
}

void configure_equalizer_from_match(eq::EqualizerProcessor& equalizer,
                                    const ReferenceSpectrum& source,
                                    const ReferenceSpectrum& reference,
                                    const MatchEqConfig& config) {
  const auto bands = match_eq_bands(source, reference, config);
  equalizer.clear();
  for (size_t i = 0; i < bands.size(); ++i) {
    equalizer.set_band(i, bands[i]);
  }
}

}  // namespace sonare::mastering::match
