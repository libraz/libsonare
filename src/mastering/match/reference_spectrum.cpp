#include "mastering/match/reference_spectrum.h"

#include <algorithm>
#include <cmath>
#include <complex>
#include <utility>

#include "core/fft.h"
#include "core/window.h"
#include "util/db.h"
#include "util/exception.h"
#include "util/fractional_octave.h"
#include "util/frequency_bins.h"

namespace sonare::mastering::match {

namespace {

ReferenceSpectrum reference_spectrum_channels(const Audio& left, const Audio* right,
                                              const ReferenceSpectrumConfig& config) {
  if (left.empty() || (right != nullptr && right->empty())) {
    throw SonareException(ErrorCode::InvalidParameter, "audio must not be empty");
  }
  if (right != nullptr &&
      (right->sample_rate() != left.sample_rate() || right->size() != left.size())) {
    throw SonareException(ErrorCode::InvalidParameter,
                          "stereo audio channels must have matching rates and lengths");
  }
  if (config.n_fft <= 0 || config.hop_length <= 0 || config.octave_fraction <= 0) {
    throw SonareException(ErrorCode::InvalidParameter, "invalid reference spectrum configuration");
  }

  FFT fft(config.n_fft);
  const int n_bins = fft.n_bins();
  std::vector<double> power_sum(static_cast<size_t>(n_bins), 0.0);
  std::vector<double> frame_power(static_cast<size_t>(n_bins), 0.0);
  std::vector<float> frame(static_cast<size_t>(config.n_fft), 0.0f);
  std::vector<std::complex<float>> bins(static_cast<size_t>(n_bins));

  size_t frame_count = 0;
  for (size_t start = 0; start < left.size(); start += static_cast<size_t>(config.hop_length)) {
    std::fill(frame.begin(), frame.end(), 0.0f);
    const size_t available = left.size() - start;
    const size_t copy_count = std::min(static_cast<size_t>(config.n_fft), available);
    for (size_t i = 0; i < copy_count; ++i) {
      frame[i] = left[start + i] * hann_value(static_cast<int>(i), config.n_fft);
    }
    fft.forward(frame.data(), bins.data());
    for (int bin = 0; bin < n_bins; ++bin) {
      const float mag = std::abs(bins[static_cast<size_t>(bin)]);
      frame_power[static_cast<size_t>(bin)] = static_cast<double>(mag) * mag;
    }
    if (right != nullptr) {
      std::fill(frame.begin(), frame.end(), 0.0f);
      for (size_t i = 0; i < copy_count; ++i) {
        frame[i] = (*right)[start + i] * hann_value(static_cast<int>(i), config.n_fft);
      }
      fft.forward(frame.data(), bins.data());
      for (int bin = 0; bin < n_bins; ++bin) {
        const float mag = std::abs(bins[static_cast<size_t>(bin)]);
        frame_power[static_cast<size_t>(bin)] =
            (frame_power[static_cast<size_t>(bin)] + static_cast<double>(mag) * mag) * 0.5;
      }
    }
    for (int bin = 0; bin < n_bins; ++bin) {
      power_sum[static_cast<size_t>(bin)] += frame_power[static_cast<size_t>(bin)];
    }
    ++frame_count;
    if (start + copy_count == left.size()) break;
  }

  std::vector<float> frequencies = util::bin_frequencies(n_bins, left.sample_rate(), config.n_fft);
  std::vector<float> magnitude(static_cast<size_t>(n_bins), 0.0f);
  for (int bin = 0; bin < n_bins; ++bin) {
    const double mean_power =
        power_sum[static_cast<size_t>(bin)] / static_cast<double>(std::max<size_t>(frame_count, 1));
    magnitude[static_cast<size_t>(bin)] = static_cast<float>(std::sqrt(mean_power));
  }
  if (config.apply_octave_smoothing) {
    magnitude = util::smooth_fractional_octave(magnitude, frequencies, config.octave_fraction);
  }

  // A bin of exact zero reads -inf; there is no floor.
  std::vector<float> db(static_cast<size_t>(n_bins), kNotMeasured);
  for (size_t bin = 0; bin < db.size(); ++bin) db[bin] = amplitude_to_level_db(magnitude[bin]);
  return {std::move(frequencies), std::move(db), left.sample_rate()};
}

}  // namespace

ReferenceSpectrum reference_spectrum(const Audio& audio, const ReferenceSpectrumConfig& config) {
  return reference_spectrum_channels(audio, nullptr, config);
}

ReferenceSpectrum reference_spectrum(const Audio& left, const Audio& right,
                                     const ReferenceSpectrumConfig& config) {
  return reference_spectrum_channels(left, &right, config);
}

}  // namespace sonare::mastering::match
