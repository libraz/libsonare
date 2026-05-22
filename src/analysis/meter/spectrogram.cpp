#include "analysis/meter/spectrogram.h"

#include <algorithm>

#include "util/exception.h"

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

std::vector<float> frame_times(int n_frames, int hop_length, int sample_rate) {
  std::vector<float> times(n_frames);
  for (int i = 0; i < n_frames; ++i) {
    times[i] = static_cast<float>(i * hop_length) / static_cast<float>(sample_rate);
  }
  return times;
}

}  // namespace

MeterSpectrogramResult spectrogram(const Audio& audio, const MeterSpectrogramConfig& config) {
  SONARE_CHECK(config.db_ref > 0.0f && config.db_amin > 0.0f, ErrorCode::InvalidParameter);

  MeterSpectrogramResult result;
  const Spectrogram spec = Spectrogram::compute(audio, config.stft);
  if (spec.empty()) return result;

  result.n_bins = spec.n_bins();
  result.n_frames = spec.n_frames();
  result.n_fft = spec.n_fft();
  result.hop_length = spec.hop_length();
  result.sample_rate = spec.sample_rate();
  result.frequencies = bin_frequencies(result.n_bins, result.sample_rate, result.n_fft);
  result.times = frame_times(result.n_frames, result.hop_length, result.sample_rate);
  result.magnitude = spec.magnitude();
  result.power = spec.power();
  result.db = spec.to_db(config.db_ref, config.db_amin, config.top_db);
  return result;
}

}  // namespace sonare::analysis::meter
