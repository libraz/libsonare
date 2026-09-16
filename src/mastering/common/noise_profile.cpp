#include "mastering/common/noise_profile.h"

#include <algorithm>
#include <cmath>
#include <vector>

#include "util/exception.h"

namespace sonare::mastering::common {
namespace {

using sonare::constants::kFloorDb;

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

/// The answer for an input nothing could be measured from: every band at the floor.
NoiseFloorDbfs floor_result() {
  NoiseFloorDbfs result;
  std::fill(std::begin(result.bands), std::end(result.bands), kFloorDb);
  return result;
}

float power_to_dbfs(double power) {
  if (!(power > 0.0)) return kFloorDb;
  return std::max(kFloorDb, static_cast<float>(10.0 * std::log10(power)));
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

NoiseFloorDbfs noise_floor_dbfs(const double* noise_psd, const float* power, int bins, int frames,
                                double signal_mean_square, int sample_rate) {
  if (noise_psd == nullptr || power == nullptr || bins <= 0 || frames <= 0) return floor_result();

  std::vector<double> noise_sum(static_cast<size_t>(bins), 0.0);
  std::vector<double> power_sum(static_cast<size_t>(bins), 0.0);
  for (int b = 0; b < bins; ++b) {
    const size_t row = static_cast<size_t>(b) * static_cast<size_t>(frames);
    double noise = 0.0;
    double observed = 0.0;
    for (int t = 0; t < frames; ++t) {
      noise += noise_psd[row + static_cast<size_t>(t)];
      observed += static_cast<double>(power[row + static_cast<size_t>(t)]);
    }
    noise_sum[static_cast<size_t>(b)] = noise;
    power_sum[static_cast<size_t>(b)] = observed;
  }
  return noise_floor_dbfs_from_sums(noise_sum.data(), power_sum.data(), bins, frames,
                                    signal_mean_square, sample_rate);
}

NoiseFloorDbfs noise_floor_dbfs_from_sums(const double* noise_psd_sum, const double* power_sum,
                                          int bins, int frames, double signal_mean_square,
                                          int sample_rate) {
  NoiseFloorDbfs result = floor_result();
  if (noise_psd_sum == nullptr || power_sum == nullptr || bins <= 0 || frames <= 0) return result;

  std::vector<double> per_bin(static_cast<size_t>(bins), 0.0);
  double observed = 0.0;
  for (int b = 0; b < bins; ++b) {
    per_bin[static_cast<size_t>(b)] =
        noise_psd_sum[static_cast<size_t>(b)] / static_cast<double>(frames);
    observed += bin_weight(b, bins) * power_sum[static_cast<size_t>(b)];
  }
  if (!(observed > 0.0) || !(signal_mean_square > 0.0)) return result;

  // The transform's unknown constant cancels in this ratio, so the dBFS below is
  // anchored on a level measured in the time domain rather than on a window sum.
  const double scale = signal_mean_square / observed * static_cast<double>(frames);
  const std::vector<double> smoothed = median_across_bins(per_bin);

  int edges[kRepairNoiseBandCount + 1] = {};
  repair_noise_band_bins(2 * (bins - 1), sample_rate, edges);
  double broadband = 0.0;
  for (size_t k = 0; k < kRepairNoiseBandCount; ++k) {
    double band = 0.0;
    for (int b = edges[k]; b < edges[k + 1] && b < bins; ++b) {
      band += bin_weight(b, bins) * smoothed[static_cast<size_t>(b)];
    }
    if (edges[k + 1] > edges[k]) result.bands[k] = power_to_dbfs(band * scale);
    broadband += band;
  }
  result.broadband = power_to_dbfs(broadband * scale);
  return result;
}

LinkedSpectra LinkedSpectra::compute(const Audio* const* channels, std::size_t channel_count,
                                     const StftConfig& config) {
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

  LinkedSpectra linked;
  linked.spectra_.reserve(channel_count);
  for (size_t c = 0; c < channel_count; ++c) {
    linked.spectra_.push_back(Spectrogram::compute(*channels[c], config));
  }
  if (linked.spectra_.front().empty()) return linked;

  const size_t cells = linked.spectra_.front().power().size();
  linked.summed_power_.assign(cells, 0.0f);
  for (const Spectrogram& spectrum : linked.spectra_) {
    const std::vector<float>& channel_power = spectrum.power();
    for (size_t i = 0; i < cells; ++i) linked.summed_power_[i] += channel_power[i];
  }
  return linked;
}

bool LinkedSpectra::empty() const { return spectra_.empty() || spectra_.front().empty(); }

int LinkedSpectra::n_bins() const { return spectra_.empty() ? 0 : spectra_.front().n_bins(); }

int LinkedSpectra::n_frames() const { return spectra_.empty() ? 0 : spectra_.front().n_frames(); }

std::vector<double> LinkedSpectra::magnitude_power_sum() const {
  if (empty()) return {};
  const size_t cells = summed_power_.size();
  std::vector<double> sum(cells, 0.0);
  for (const Spectrogram& spectrum : spectra_) {
    const std::complex<float>* data = spectrum.complex_data();
    for (size_t i = 0; i < cells; ++i) {
      const double magnitude = std::abs(data[i]);
      sum[i] += magnitude * magnitude;
    }
  }
  return sum;
}

std::vector<std::vector<std::complex<float>>> LinkedSpectra::masked(const double* gains) const {
  std::vector<std::vector<std::complex<float>>> out;
  if (empty() || gains == nullptr) return out;
  const size_t cells = summed_power_.size();
  out.reserve(spectra_.size());
  for (const Spectrogram& spectrum : spectra_) {
    const std::complex<float>* data = spectrum.complex_data();
    std::vector<std::complex<float>> channel(cells);
    for (size_t i = 0; i < cells; ++i) {
      channel[i] = {static_cast<float>(data[i].real() * gains[i]),
                    static_cast<float>(data[i].imag() * gains[i])};
    }
    out.push_back(std::move(channel));
  }
  return out;
}

std::vector<Audio> LinkedSpectra::resynthesize(
    const std::vector<std::vector<std::complex<float>>>& spectra, int out_length) const {
  std::vector<Audio> out;
  out.reserve(spectra.size());
  for (size_t c = 0; c < spectra.size(); ++c) {
    const Spectrogram& reference = spectra_[c];
    const Spectrogram clean = Spectrogram::from_complex(
        spectra[c].data(), reference.n_bins(), reference.n_frames(), reference.n_fft(),
        reference.hop_length(), reference.sample_rate(), reference.window(), reference.center(),
        reference.win_length());
    out.push_back(clean.to_audio(out_length));
  }
  return out;
}

}  // namespace sonare::mastering::common
