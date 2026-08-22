#include "mastering/stereo/mono_compat_check.h"

#include <algorithm>
#include <cmath>
#include <complex>
#include <utility>

#include "core/fft.h"
#include "util/constants.h"
#include "util/exception.h"
#include "util/stereo_metrics.h"

namespace sonare::mastering::stereo {
namespace {

using sonare::constants::kPiD;

/// Smallest and largest analysis transform for the log-band measurement. The
/// floor keeps a coarse request from measuring a band out of two bins; the
/// ceiling bounds both the per-frame cost and the resident spectra on a
/// pathological (very low `low_hz`, very high `bands_per_octave`) request.
constexpr size_t kMinBandFftSize = 256;
constexpr size_t kMaxBandFftSize = 65536;

/// Bins the narrowest band should span before the transform is considered fine
/// enough to separate it from its neighbours.
constexpr double kMinBinsPerBand = 4.0;

/// Transform size that resolves the narrowest band in the requested log split.
/// The narrowest band is always the lowest one: its width is `low_hz * (r - 1)`
/// and every band above it is wider by the same ratio.
size_t band_analysis_fft_size(double sample_rate, double low_hz, double ratio) {
  const double narrowest_band_hz = low_hz * (ratio - 1.0);
  const double target_bin_hz = narrowest_band_hz / kMinBinsPerBand;
  size_t n_fft = kMinBandFftSize;
  while (n_fft < kMaxBandFftSize && sample_rate / static_cast<double>(n_fft) > target_bin_hz) {
    n_fft <<= 1;
  }
  return n_fft;
}

/// Half-open bin span [begin, end) covering [low_hz, high_hz). A band too narrow
/// to hold a bin collapses onto the one nearest its log centre rather than
/// measuring as silent.
std::pair<size_t, size_t> band_bin_span(double low_hz, double high_hz, size_t n_fft,
                                        double sample_rate) {
  const double bins_per_hz = static_cast<double>(n_fft) / sample_rate;
  const size_t n_bins = n_fft / 2 + 1;
  const auto to_bin = [&](double hz) {
    const double bin = std::ceil(hz * bins_per_hz);
    if (bin < 0.0) return size_t{0};
    return std::min(n_bins, static_cast<size_t>(bin));
  };
  size_t begin = to_bin(low_hz);
  size_t end = to_bin(high_hz);
  if (begin >= end) {
    const double center = std::sqrt(low_hz * high_hz);
    begin = std::min(n_bins - 1, static_cast<size_t>(std::lround(center * bins_per_hz)));
    end = begin + 1;
  }
  return {begin, end};
}

}  // namespace

MonoCompatResult mono_compat_check(const float* left, const float* right, size_t length,
                                   float correlation_threshold) {
  if (length == 0 || left == nullptr || right == nullptr) {
    throw SonareException(ErrorCode::InvalidParameter,
                          "mono compatibility buffers must not be empty or null");
  }

  MonoCompatResult result;
  result.correlation = util::stereo_correlation(left, right, length);
  result.width = util::stereo_width(left, right, length);

  double side_sum = 0.0;
  for (size_t i = 0; i < length; ++i) {
    const float mono = 0.5f * (left[i] + right[i]);
    const float side = 0.5f * (left[i] - right[i]);
    result.mono_peak = std::max(result.mono_peak, std::abs(mono));
    side_sum += static_cast<double>(side) * side;
  }
  result.side_rms = static_cast<float>(std::sqrt(side_sum / static_cast<double>(length)));
  result.likely_mono_compatible = result.correlation >= correlation_threshold;
  return result;
}

std::vector<MonoCompatBandResult> mono_compat_check_log_bands(const float* left, const float* right,
                                                              size_t length, double sample_rate,
                                                              int bands_per_octave, float low_hz,
                                                              float high_hz) {
  if (length == 0 || left == nullptr || right == nullptr) {
    throw SonareException(ErrorCode::InvalidParameter,
                          "mono compatibility buffers must not be empty or null");
  }
  if (!(sample_rate > 0.0) || bands_per_octave <= 0 || !(low_hz > 0.0f) || !(high_hz > low_hz) ||
      high_hz >= static_cast<float>(sample_rate * 0.5)) {
    throw SonareException(ErrorCode::InvalidParameter,
                          "invalid mono compatibility band configuration");
  }

  const double ratio = std::pow(2.0, 1.0 / static_cast<double>(bands_per_octave));

  // Band edges first, so the transform is sized against the narrowest band the
  // caller asked for rather than against a fixed constant.
  std::vector<MonoCompatBandResult> result;
  for (double low = low_hz; low < high_hz; low *= ratio) {
    const double high = std::min(static_cast<double>(high_hz), low * ratio);
    result.push_back({static_cast<float>(low), static_cast<float>(high), 0.0f, 0.0f});
  }
  if (result.empty()) return result;

  const size_t n_fft = band_analysis_fft_size(sample_rate, low_hz, ratio);
  const size_t hop = n_fft / 2;
  const size_t n_bins = n_fft / 2 + 1;

  // The bin span of each band, resolved once. A band narrower than one bin
  // collapses onto the single bin nearest its log centre, so no band is ever
  // measured as empty just because the transform could not separate it.
  std::vector<std::pair<size_t, size_t>> band_bins;
  band_bins.reserve(result.size());
  for (const MonoCompatBandResult& band : result) {
    band_bins.push_back(band_bin_span(band.low_hz, band.high_hz, n_fft, sample_rate));
  }

  FFT fft(static_cast<int>(n_fft));
  std::vector<float> window(n_fft);
  for (size_t n = 0; n < n_fft; ++n) {
    window[n] = static_cast<float>(
        0.5 * (1.0 - std::cos(2.0 * kPiD * static_cast<double>(n) / static_cast<double>(n_fft))));
  }
  std::vector<float> frame_left(n_fft, 0.0f);
  std::vector<float> frame_right(n_fft, 0.0f);
  std::vector<std::complex<float>> spec_left(n_bins);
  std::vector<std::complex<float>> spec_right(n_bins);

  // Per band: the cross-spectrum and the two auto-spectra (which give the
  // band-limited correlation directly), plus the band's share of the side
  // spectrum.
  std::vector<double> sum_lr(result.size(), 0.0);
  std::vector<double> sum_ll(result.size(), 0.0);
  std::vector<double> sum_rr(result.size(), 0.0);
  std::vector<double> sum_side(result.size(), 0.0);
  double total_side_spectral = 0.0;

  const size_t frames = length <= n_fft ? 1 : 1 + (length - 1) / hop;
  for (size_t frame = 0; frame < frames; ++frame) {
    const size_t offset = frame * hop;
    for (size_t n = 0; n < n_fft; ++n) {
      const size_t i = offset + n;
      const float w = window[n];
      frame_left[n] = i < length ? left[i] * w : 0.0f;
      frame_right[n] = i < length ? right[i] * w : 0.0f;
    }
    fft.forward(frame_left.data(), spec_left.data());
    fft.forward(frame_right.data(), spec_right.data());

    // One-sided bins carry their negative-frequency twin's energy as well; DC
    // and Nyquist have none.
    for (size_t k = 0; k < n_bins; ++k) {
      const double weight = (k == 0 || k + 1 == n_bins) ? 1.0 : 2.0;
      const std::complex<double> l(spec_left[k].real(), spec_left[k].imag());
      const std::complex<double> r(spec_right[k].real(), spec_right[k].imag());
      // The side channel is a linear combination of the two, so its spectrum
      // comes for free rather than costing a third transform.
      const std::complex<double> s = 0.5 * (l - r);
      total_side_spectral += weight * std::norm(s);
    }
    for (size_t band = 0; band < result.size(); ++band) {
      const auto [begin, end] = band_bins[band];
      for (size_t k = begin; k < end; ++k) {
        const double weight = (k == 0 || k + 1 == n_bins) ? 1.0 : 2.0;
        const std::complex<double> l(spec_left[k].real(), spec_left[k].imag());
        const std::complex<double> r(spec_right[k].real(), spec_right[k].imag());
        const std::complex<double> s = 0.5 * (l - r);
        sum_ll[band] += weight * std::norm(l);
        sum_rr[band] += weight * std::norm(r);
        sum_lr[band] += weight * (l.real() * r.real() + l.imag() * r.imag());
        sum_side[band] += weight * std::norm(s);
      }
    }
  }

  // The band's side RMS is its SHARE of the spectrum applied to the exactly
  // known broadband side energy. Taking the ratio cancels the window and
  // overlap normalization, which an absolute Parseval reconstruction would have
  // to estimate and would get wrong on a buffer shorter than one frame.
  double total_side_energy = 0.0;
  for (size_t i = 0; i < length; ++i) {
    const double side = 0.5 * (static_cast<double>(left[i]) - static_cast<double>(right[i]));
    total_side_energy += side * side;
  }
  const double side_mean_square = total_side_energy / static_cast<double>(length);

  for (size_t band = 0; band < result.size(); ++band) {
    if (sum_ll[band] > 0.0 && sum_rr[band] > 0.0) {
      result[band].correlation =
          static_cast<float>(sum_lr[band] / std::sqrt(sum_ll[band] * sum_rr[band]));
    }
    if (total_side_spectral > 0.0) {
      const double share = sum_side[band] / total_side_spectral;
      result[band].side_rms = static_cast<float>(std::sqrt(share * side_mean_square));
    }
  }
  return result;
}

}  // namespace sonare::mastering::stereo
