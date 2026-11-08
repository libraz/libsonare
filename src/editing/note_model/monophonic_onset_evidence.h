#pragma once

/// @file monophonic_onset_evidence.h
/// @brief Private spectral-flux onset evidence for monophonic note grouping.

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <vector>

#include "core/audio.h"
#include "core/fft.h"
#include "util/constants.h"
#include "util/peak.h"

namespace sonare::editing::note_model::detail {

struct MonophonicOnsetEvidence {
  bool available = false;
  std::vector<int> frames;
};

namespace monophonic_onset_detail {

constexpr double kFramesPerSecond = 200.0;
constexpr double kReferenceFftSize = 2048.0;
constexpr double kReferenceSampleRate = 44100.0;
constexpr double kMinimumFilterFrequency = 30.0;
constexpr double kReferenceFilterFrequency = sonare::constants::kA4Hz;
constexpr double kMaximumFilterFrequency = 17000.0;
constexpr double kBandsPerOctave = 24.0;
constexpr int kPeakLeftMaximum = 2;
constexpr int kPeakRightMaximumExclusive = 11;
constexpr int kPeakLeftAverage = 30;
constexpr int kPeakRightAverageExclusive = 1;
constexpr float kPeakMeanDelta = 1.1f;
constexpr int kPeakPadding = kPeakLeftAverage;
constexpr int kPeakRightPadding = kPeakRightMaximumExclusive - 1;

struct Triangle {
  int left;
  int center;
  int right;
};

inline bool nearest_even_bin(double frequency, int fft_size, int sample_rate, int& bin) {
  const double exact = frequency * static_cast<double>(fft_size) / sample_rate;
  if (!std::isfinite(exact) || exact < 0.0 ||
      exact > static_cast<double>(std::numeric_limits<int>::max())) {
    return false;
  }
  const double lower_exact = std::floor(exact);
  const double fraction = exact - lower_exact;
  int rounded = static_cast<int>(lower_exact);
  if (fraction > 0.5 || (fraction == 0.5 && rounded % 2 != 0)) ++rounded;
  bin = rounded;
  return true;
}

inline bool make_filterbank(int fft_size, int sample_rate, std::vector<Triangle>& triangles) {
  const double upper = std::min(kMaximumFilterFrequency, 0.5 * sample_rate);
  const double ratio = std::pow(2.0, 1.0 / kBandsPerOctave);
  std::vector<double> frequencies;
  frequencies.reserve(320);
  double frequency = kReferenceFilterFrequency;
  frequencies.push_back(frequency);
  while (frequency <= upper) {
    frequency *= ratio;
    if (!std::isfinite(frequency)) return false;
    frequencies.push_back(frequency);
  }
  frequency = kReferenceFilterFrequency;
  while (frequency >= kMinimumFilterFrequency) {
    frequency /= ratio;
    if (!std::isfinite(frequency) || !(frequency > 0.0)) return false;
    frequencies.push_back(frequency);
  }
  std::sort(frequencies.begin(), frequencies.end());

  std::vector<int> bins;
  bins.reserve(frequencies.size());
  for (const double center_hz : frequencies) {
    int bin = 0;
    if (!nearest_even_bin(center_hz, fft_size, sample_rate, bin)) return false;
    if (bin < fft_size / 2) bins.push_back(bin);
  }
  std::sort(bins.begin(), bins.end());
  bins.erase(std::unique(bins.begin(), bins.end()), bins.end());
  if (bins.size() < 3) return false;

  triangles.clear();
  triangles.reserve(bins.size() - 2);
  for (size_t i = 0; i + 2 < bins.size(); ++i) {
    if (!(bins[i] < bins[i + 1] && bins[i + 1] < bins[i + 2])) return false;
    triangles.push_back({bins[i], bins[i + 1], bins[i + 2]});
  }
  return !triangles.empty();
}

inline bool make_log_spectrum(const Audio& audio, int64_t frame_begin, int fft_size,
                              const std::vector<float>& window, FFT& fft,
                              const std::vector<Triangle>& triangles, std::vector<float>& frame,
                              std::vector<std::complex<float>>& bins,
                              std::vector<float>& log_spectrum) {
  const int64_t sample_count = static_cast<int64_t>(audio.size());
  for (int i = 0; i < fft_size; ++i) {
    const int64_t sample_index = frame_begin + i;
    float sample = 0.0f;
    if (sample_index >= 0 && sample_index < sample_count) {
      sample = audio[static_cast<size_t>(sample_index)];
      if (!std::isfinite(sample)) return false;
    }
    frame[static_cast<size_t>(i)] = sample * window[static_cast<size_t>(i)];
  }
  fft.forward(frame.data(), bins.data());

  const double magnitude_scale = kReferenceFftSize / fft_size;
  log_spectrum.resize(triangles.size());
  for (size_t band = 0; band < triangles.size(); ++band) {
    const Triangle triangle = triangles[band];
    double magnitude_sum = 0.0;
    for (int bin = triangle.left; bin < triangle.center; ++bin) {
      const double magnitude = std::abs(bins[static_cast<size_t>(bin)]);
      if (!std::isfinite(magnitude)) return false;
      const double weight = static_cast<double>(bin - triangle.left) /
                            static_cast<double>(triangle.center - triangle.left);
      magnitude_sum += magnitude * weight;
    }
    for (int bin = triangle.center; bin < triangle.right; ++bin) {
      const double magnitude = std::abs(bins[static_cast<size_t>(bin)]);
      if (!std::isfinite(magnitude)) return false;
      const double weight = 1.0 - static_cast<double>(bin - triangle.center) /
                                      static_cast<double>(triangle.right - triangle.center);
      magnitude_sum += magnitude * weight;
    }
    if (!std::isfinite(magnitude_sum)) return false;
    const double log_magnitude = std::log10(1.0 + magnitude_scale * magnitude_sum);
    if (!std::isfinite(log_magnitude)) return false;
    log_spectrum[band] = static_cast<float>(log_magnitude);
  }
  return true;
}

}  // namespace monophonic_onset_detail

/// Computes 200 Hz SuperFlux onset evidence without retaining a full spectrogram.
/// `available == false` means the geometry or samples could not be analyzed;
/// an available result with no frames is a valid empty onset list.
inline MonophonicOnsetEvidence monophonic_onset_evidence(const Audio& audio) {
  MonophonicOnsetEvidence result;
  const int sample_rate = audio.sample_rate();
  if (audio.empty() || sample_rate <= 0 ||
      audio.size() > static_cast<size_t>(std::numeric_limits<int64_t>::max())) {
    return result;
  }

  const double requested_fft = monophonic_onset_detail::kReferenceFftSize * sample_rate /
                               monophonic_onset_detail::kReferenceSampleRate;
  if (!std::isfinite(requested_fft) || requested_fft > std::numeric_limits<int>::max()) {
    return result;
  }
  const double half_fft = requested_fft / 2.0;
  if (!std::isfinite(half_fft) || half_fft > std::numeric_limits<int>::max() / 2.0) return result;
  const int fft_size = std::max(2, 2 * static_cast<int>(std::floor(half_fft + 0.5)));

  const double requested_frames = std::ceil(
      static_cast<double>(audio.size()) * monophonic_onset_detail::kFramesPerSecond / sample_rate);
  if (!std::isfinite(requested_frames) || requested_frames < 1.0 ||
      requested_frames > static_cast<double>(std::numeric_limits<int>::max() -
                                             monophonic_onset_detail::kPeakPadding -
                                             monophonic_onset_detail::kPeakRightPadding)) {
    return result;
  }
  const int frame_count = static_cast<int>(requested_frames);
  std::vector<monophonic_onset_detail::Triangle> triangles;
  if (!monophonic_onset_detail::make_filterbank(fft_size, sample_rate, triangles)) return result;

  std::vector<float> window(static_cast<size_t>(fft_size));
  const double window_denominator = static_cast<double>(fft_size - 1);
  for (int i = 0; i < fft_size; ++i) {
    window[static_cast<size_t>(i)] = static_cast<float>(
        0.5 - 0.5 * std::cos(sonare::constants::kTwoPiD * i / window_denominator));
  }

  FFT fft(fft_size);
  std::vector<float> frame(static_cast<size_t>(fft_size));
  std::vector<std::complex<float>> bins(static_cast<size_t>(fft.n_bins()));
  std::vector<float> two_frames_ago(triangles.size(), 0.0f);
  std::vector<float> previous_frame(triangles.size(), 0.0f);
  std::vector<float> current_frame(triangles.size());
  std::vector<float> activation(static_cast<size_t>(frame_count), 0.0f);

  for (int i = 0; i < frame_count; ++i) {
    const double begin_exact =
        static_cast<double>(i) * sample_rate / monophonic_onset_detail::kFramesPerSecond -
        static_cast<double>(fft_size) / 2.0;
    if (!std::isfinite(begin_exact) ||
        begin_exact < static_cast<double>(std::numeric_limits<int64_t>::min()) ||
        begin_exact > static_cast<double>(std::numeric_limits<int64_t>::max())) {
      return MonophonicOnsetEvidence{};
    }
    const int64_t frame_begin = static_cast<int64_t>(begin_exact);
    if (!monophonic_onset_detail::make_log_spectrum(audio, frame_begin, fft_size, window, fft,
                                                    triangles, frame, bins, current_frame)) {
      return MonophonicOnsetEvidence{};
    }
    if (i >= 2) {
      double sum = 0.0;
      for (size_t band = 0; band < triangles.size(); ++band) {
        const size_t left = band == 0 ? band : band - 1;
        const size_t right = std::min(triangles.size() - 1, band + 1);
        const float reference =
            std::max({two_frames_ago[left], two_frames_ago[band], two_frames_ago[right]});
        sum += std::max(0.0, static_cast<double>(current_frame[band]) - reference);
      }
      activation[static_cast<size_t>(i)] = static_cast<float>(sum);
    }
    two_frames_ago.swap(previous_frame);
    previous_frame.swap(current_frame);
  }

  std::vector<float> padded(
      static_cast<size_t>(monophonic_onset_detail::kPeakPadding + frame_count +
                          monophonic_onset_detail::kPeakRightPadding),
      0.0f);
  std::copy(activation.begin(), activation.end(),
            padded.begin() + monophonic_onset_detail::kPeakPadding);
  const std::vector<int> picked = peak_pick(padded, monophonic_onset_detail::kPeakLeftMaximum,
                                            monophonic_onset_detail::kPeakRightMaximumExclusive,
                                            monophonic_onset_detail::kPeakLeftAverage,
                                            monophonic_onset_detail::kPeakRightAverageExclusive,
                                            monophonic_onset_detail::kPeakMeanDelta, 0);
  result.frames.reserve(picked.size());
  for (const int peak : picked) {
    if (peak >= monophonic_onset_detail::kPeakPadding &&
        peak < monophonic_onset_detail::kPeakPadding + frame_count) {
      result.frames.push_back(peak - monophonic_onset_detail::kPeakPadding);
    }
  }
  result.available = true;
  return result;
}

}  // namespace sonare::editing::note_model::detail
