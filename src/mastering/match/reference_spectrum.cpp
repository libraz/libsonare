#include "mastering/match/reference_spectrum.h"

#include <Eigen/Core>
#include <algorithm>
#include <cmath>
#include <complex>
#include <utility>

#include "core/fft.h"
#include "core/window.h"
#include "util/constants.h"
#include "util/exception.h"
#include "util/fractional_octave.h"

namespace sonare::mastering::match {

using sonare::constants::kEpsilon;
namespace {

std::vector<float> bin_frequencies(int n_bins, int sample_rate, int n_fft) {
  std::vector<float> frequencies(static_cast<size_t>(n_bins));
  const float bin_width = static_cast<float>(sample_rate) / static_cast<float>(n_fft);
  for (int i = 0; i < n_bins; ++i) frequencies[static_cast<size_t>(i)] = i * bin_width;
  return frequencies;
}

}  // namespace

ReferenceSpectrum reference_spectrum(const Audio& audio, const ReferenceSpectrumConfig& config) {
  if (audio.empty()) {
    throw SonareException(ErrorCode::InvalidParameter, "audio must not be empty");
  }
  if (config.n_fft <= 0 || config.hop_length <= 0 || config.octave_fraction <= 0) {
    throw SonareException(ErrorCode::InvalidParameter, "invalid reference spectrum configuration");
  }

  FFT fft(config.n_fft);
  const int n_bins = fft.n_bins();
  std::vector<double> power_sum(static_cast<size_t>(n_bins), 0.0);
  std::vector<float> frame(static_cast<size_t>(config.n_fft), 0.0f);
  std::vector<std::complex<float>> bins(static_cast<size_t>(n_bins));

  size_t frame_count = 0;
  for (size_t start = 0; start < audio.size(); start += static_cast<size_t>(config.hop_length)) {
    std::fill(frame.begin(), frame.end(), 0.0f);
    const size_t available = audio.size() - start;
    const size_t copy_count = std::min(static_cast<size_t>(config.n_fft), available);
    for (size_t i = 0; i < copy_count; ++i) {
      frame[i] = audio[start + i] * hann_value(static_cast<int>(i), config.n_fft);
    }
    fft.forward(frame.data(), bins.data());
    for (int bin = 0; bin < n_bins; ++bin) {
      const float mag = std::abs(bins[static_cast<size_t>(bin)]);
      power_sum[static_cast<size_t>(bin)] += static_cast<double>(mag) * mag;
    }
    ++frame_count;
    if (start + copy_count == audio.size()) break;
  }

  std::vector<float> frequencies = bin_frequencies(n_bins, audio.sample_rate(), config.n_fft);
  std::vector<float> magnitude(static_cast<size_t>(n_bins), 0.0f);
  for (int bin = 0; bin < n_bins; ++bin) {
    const double mean_power =
        power_sum[static_cast<size_t>(bin)] / static_cast<double>(std::max<size_t>(frame_count, 1));
    magnitude[static_cast<size_t>(bin)] = static_cast<float>(std::sqrt(mean_power));
  }
  if (config.apply_octave_smoothing) {
    magnitude = util::smooth_fractional_octave(magnitude, frequencies, config.octave_fraction);
  }

  std::vector<float> db(static_cast<size_t>(n_bins), sonare::constants::kFloorDb);
  Eigen::Map<const Eigen::ArrayXf> mag_map(magnitude.data(), n_bins);
  Eigen::Map<Eigen::ArrayXf> db_map(db.data(), n_bins);
  db_map = (mag_map.cwiseMax(sonare::constants::kEpsilon)).log10() * 20.0f;
  return {std::move(frequencies), std::move(db), audio.sample_rate()};
}

}  // namespace sonare::mastering::match
