#include "analysis/meter/spectrum.h"

#include <algorithm>
#include <cmath>

#include "core/fft.h"
#include "util/constants.h"
#include "util/exception.h"
#include "util/math_utils.h"

namespace sonare::analysis::meter {
namespace {

std::vector<float> bin_frequencies(int n_bins, int sample_rate, int n_fft) {
  std::vector<float> frequencies(n_bins);
  const float bin_width = static_cast<float>(sample_rate) / static_cast<float>(n_fft);
  for (int i = 0; i < n_bins; ++i) {
    frequencies[i] = static_cast<float>(i) * bin_width;
  }
  return frequencies;
}

}  // namespace

SpectrumResult spectrum(const Audio& audio, const SpectrumConfig& config) {
  SONARE_CHECK(config.n_fft > 0, ErrorCode::InvalidParameter);
  SONARE_CHECK(config.octave_fraction > 0, ErrorCode::InvalidParameter);
  SONARE_CHECK(config.db_ref > 0.0f && config.db_amin > 0.0f, ErrorCode::InvalidParameter);

  SpectrumResult result;
  result.n_fft = config.n_fft;
  result.sample_rate = audio.sample_rate();
  const int n_bins = config.n_fft / 2 + 1;
  result.frequencies = bin_frequencies(n_bins, audio.sample_rate(), config.n_fft);
  result.magnitude.assign(n_bins, 0.0f);
  result.power.assign(n_bins, 0.0f);
  result.db.assign(n_bins, sonare::constants::kFloorDb);
  if (audio.empty()) return result;

  std::vector<float> frame(static_cast<size_t>(config.n_fft), 0.0f);
  const size_t copy_count = std::min(audio.size(), static_cast<size_t>(config.n_fft));
  std::copy(audio.data(), audio.data() + copy_count, frame.begin());

  std::vector<std::complex<float>> bins(n_bins);
  FFT fft(config.n_fft);
  fft.forward(frame.data(), bins.data());

  for (int i = 0; i < n_bins; ++i) {
    result.magnitude[i] = std::abs(bins[i]);
    result.power[i] = result.magnitude[i] * result.magnitude[i];
  }

  if (config.apply_octave_smoothing) {
    result.magnitude =
        smooth_fractional_octave(result.magnitude, result.frequencies, config.octave_fraction);
    for (int i = 0; i < n_bins; ++i) {
      result.power[i] = result.magnitude[i] * result.magnitude[i];
    }
  }

  for (int i = 0; i < n_bins; ++i) {
    const float amplitude = std::max(config.db_amin, result.magnitude[i]);
    result.db[i] = 20.0f * std::log10(amplitude / config.db_ref);
  }
  return result;
}

std::vector<float> smooth_fractional_octave(const std::vector<float>& values,
                                            const std::vector<float>& frequencies,
                                            int octave_fraction) {
  SONARE_CHECK(octave_fraction > 0, ErrorCode::InvalidParameter);
  SONARE_CHECK(values.size() == frequencies.size(), ErrorCode::InvalidParameter);

  std::vector<float> smoothed(values.size(), 0.0f);
  if (values.empty()) return smoothed;

  smoothed[0] = values[0];
  const float ratio = std::pow(2.0f, 1.0f / (2.0f * static_cast<float>(octave_fraction)));
  for (size_t i = 1; i < values.size(); ++i) {
    const float center = frequencies[i];
    const float low = center / ratio;
    const float high = center * ratio;
    float sum = 0.0f;
    size_t count = 0;
    for (size_t j = 1; j < values.size(); ++j) {
      if (frequencies[j] >= low && frequencies[j] <= high) {
        sum += values[j];
        ++count;
      }
    }
    smoothed[i] = count == 0 ? values[i] : sum / static_cast<float>(count);
  }
  return smoothed;
}

}  // namespace sonare::analysis::meter
