#include "mastering/common/noise_profile.h"

#include <algorithm>
#include <cmath>
#include <vector>

#include "util/db.h"
#include "util/exception.h"

namespace sonare::mastering::common {
namespace {

// A Hann mainlobe is four bins wide, so a 17-bin median does not land on a tone.
// Measured over the evaluation corpus's noise items: an unsmoothed sum reported a
// 440 Hz sine as 12 dB of noise floor, this reports it within 0.2 dB, and a wider
// window costs a 1/f floor 4 dB by spanning too much of its slope.
constexpr int kMedianHalfBins = 8;

/// @brief One-sided energy weight: the interior bins stand for a conjugate pair.
double bin_weight(int bin, int bins) { return (bin == 0 || bin == bins - 1) ? 1.0 : 2.0; }

std::vector<double> median_across_bins(const std::vector<double>& psd) {
  const int bins = static_cast<int>(psd.size());
  std::vector<double> smoothed(psd.size(), 0.0);
  std::vector<double> window;
  window.reserve(static_cast<size_t>(2 * kMedianHalfBins + 1));
  const int width = std::min(bins, 2 * kMedianHalfBins + 1);
  for (int b = 0; b < bins; ++b) {
    // Slid inward at the edges rather than truncated: a half window at bin 0 is
    // narrow enough for the lowest tone in the signal to be its median.
    const int first = std::clamp(b - kMedianHalfBins, 0, bins - width);
    window.assign(psd.begin() + first, psd.begin() + first + width);
    std::nth_element(window.begin(), window.begin() + window.size() / 2, window.end());
    smoothed[static_cast<size_t>(b)] = window[window.size() / 2];
  }
  return smoothed;
}

/// The answer for an input nothing could be measured from: every band not measured.
NoiseFloorDbfs unmeasured_result() {
  NoiseFloorDbfs result;
  result.broadband = kNotMeasured;
  std::fill(std::begin(result.bands), std::end(result.bands), kNotMeasured);
  return result;
}

}  // namespace

void repair_noise_band_bins(int n_fft, int sample_rate, int* out) {
  if (n_fft <= 0 || sample_rate <= 0 || out == nullptr) {
    throw SonareException(ErrorCode::InvalidParameter, "invalid noise band grid geometry");
  }
  const int bins = n_fft / 2 + 1;
  const double nyquist = 0.5 * static_cast<double>(sample_rate);
  const double low = std::min(static_cast<double>(kRepairNoiseBandLowHz), nyquist);
  const double ratio = nyquist > low ? nyquist / low : 1.0;
  int previous = 0;
  for (size_t k = 0; k <= kRepairNoiseBandCount; ++k) {
    const double fraction = static_cast<double>(k) / static_cast<double>(kRepairNoiseBandCount);
    const double hz = low * std::pow(ratio, fraction);
    const int bin = static_cast<int>(std::lround(hz * n_fft / sample_rate));
    previous = std::clamp(std::max(bin, previous), 0, bins);
    out[k] = previous;
  }
  out[kRepairNoiseBandCount] = bins;
}

NoiseFloorDbfs noise_floor_dbfs_from_sums(const double* noise_psd_sum, const double* power_sum,
                                          int bins, int frames, double signal_mean_square,
                                          int sample_rate) {
  NoiseFloorDbfs result = unmeasured_result();
  if (noise_psd_sum == nullptr || power_sum == nullptr || bins <= 0 || frames <= 0) return result;

  std::vector<double> per_bin(static_cast<size_t>(bins), 0.0);
  double observed = 0.0;
  for (int b = 0; b < bins; ++b) {
    per_bin[static_cast<size_t>(b)] =
        noise_psd_sum[static_cast<size_t>(b)] / static_cast<double>(frames);
    observed += bin_weight(b, bins) * power_sum[static_cast<size_t>(b)];
  }
  if (!(observed >= 0.0) || !(signal_mean_square >= 0.0)) return result;

  // The transform's unknown constant cancels in this ratio, so the dBFS below is
  // anchored on a level measured in the time domain rather than on a window sum.
  // A signal of exact zeros has no power in any band: a zero scale reads -inf.
  const bool silent = observed == 0.0 || signal_mean_square == 0.0;
  const double scale = silent ? 0.0 : signal_mean_square / observed * static_cast<double>(frames);
  const std::vector<double> smoothed = median_across_bins(per_bin);

  int edges[kRepairNoiseBandCount + 1] = {};
  repair_noise_band_bins(2 * (bins - 1), sample_rate, edges);
  double broadband = 0.0;
  for (size_t k = 0; k < kRepairNoiseBandCount; ++k) {
    double band = 0.0;
    for (int b = edges[k]; b < edges[k + 1] && b < bins; ++b) {
      band += bin_weight(b, bins) * smoothed[static_cast<size_t>(b)];
    }
    if (edges[k + 1] > edges[k])
      result.bands[k] = static_cast<float>(power_to_level_db(band * scale));
    broadband += band;
  }
  result.broadband = static_cast<float>(power_to_level_db(broadband * scale));
  return result;
}

void validate_linked_channels(const Audio* const* channels, std::size_t channel_count) {
  if (channels == nullptr || channel_count == 0) {
    throw SonareException(ErrorCode::InvalidParameter,
                          "linked analysis needs at least one channel");
  }
  for (size_t c = 0; c < channel_count; ++c) {
    if (channels[c] == nullptr) {
      throw SonareException(ErrorCode::InvalidParameter,
                            "linked analysis channel must not be null");
    }
    if (channels[c]->size() != channels[0]->size()) {
      throw SonareException(ErrorCode::InvalidParameter,
                            "linked analysis channels must have the same length");
    }
    if (channels[c]->sample_rate() != channels[0]->sample_rate()) {
      throw SonareException(ErrorCode::InvalidParameter,
                            "linked analysis channels must share one sample rate");
    }
  }
}

LinkedSpectra LinkedSpectra::compute(const Audio* const* channels, std::size_t channel_count,
                                     const StftConfig& config) {
  validate_linked_channels(channels, channel_count);

  LinkedSpectra linked;
  linked.spectra_.reserve(channel_count);
  std::vector<float> channel_power;
  for (size_t c = 0; c < channel_count; ++c) {
    Spectrogram spectrum = Spectrogram::compute(*channels[c], config);
    if (c == 0) {
      if (spectrum.empty()) {
        linked.spectra_.resize(channel_count);
        return linked;
      }
      linked.n_bins_ = spectrum.n_bins();
      linked.n_frames_ = spectrum.n_frames();
      linked.n_fft_ = spectrum.n_fft();
      linked.hop_length_ = spectrum.hop_length();
      linked.sample_rate_ = spectrum.sample_rate();
      linked.win_length_ = spectrum.win_length();
      linked.center_ = spectrum.center();
      linked.window_ = spectrum.window();
    }
    linked.spectra_.push_back(std::move(spectrum).release_complex());
    // The same power the spectrogram would cache, summed and dropped rather than kept per channel.
    detail::fill_power_cache(linked.spectra_.back(), {}, channel_power);
    if (c == 0) linked.summed_power_.assign(channel_power.size(), 0.0f);
    for (size_t i = 0; i < channel_power.size(); ++i) linked.summed_power_[i] += channel_power[i];
  }
  return linked;
}

std::vector<std::vector<std::complex<float>>> LinkedSpectra::take_masked(const double* gains) {
  if (empty() || gains == nullptr) return {};
  for (std::vector<std::complex<float>>& channel : spectra_) {
    for (size_t i = 0; i < channel.size(); ++i) {
      channel[i] = {static_cast<float>(channel[i].real() * gains[i]),
                    static_cast<float>(channel[i].imag() * gains[i])};
    }
  }
  return std::move(spectra_);
}

std::vector<Audio> LinkedSpectra::resynthesize(
    std::vector<std::vector<std::complex<float>>>&& spectra, int out_length) const {
  std::vector<Audio> out;
  out.reserve(spectra.size());
  for (std::vector<std::complex<float>>& spectrum : spectra) {
    const Spectrogram clean =
        Spectrogram::from_complex(std::move(spectrum), n_bins_, n_frames_, n_fft_, hop_length_,
                                  sample_rate_, window_, center_, win_length_);
    out.push_back(clean.to_audio(out_length));
  }
  return out;
}

}  // namespace sonare::mastering::common
