#pragma once

/// @file audio_fixtures.h
/// @brief Shared audio sample generators for tests.

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <vector>

#include "core/audio.h"
#include "rt/processor_base.h"
#include "util/constants.h"

namespace sonare::test {

inline std::vector<float> generate_sine(int samples, float frequency_hz, int sample_rate,
                                        float amplitude = 1.0f) {
  std::vector<float> result(static_cast<std::size_t>(std::max(0, samples)));
  for (int i = 0; i < samples; ++i) {
    result[static_cast<std::size_t>(i)] =
        amplitude * static_cast<float>(std::sin(constants::kTwoPiD * frequency_hz *
                                                static_cast<double>(i) / sample_rate));
  }
  return result;
}

inline std::vector<float> generate_sine_samples(float frequency_hz, int sample_rate, int samples,
                                                float amplitude = 1.0f) {
  return generate_sine(samples, frequency_hz, sample_rate, amplitude);
}

inline Audio generate_sine_audio(float frequency_hz, int sample_rate = 22050,
                                 float duration_sec = 0.5f, float amplitude = 1.0f) {
  const int samples = static_cast<int>(static_cast<float>(sample_rate) * duration_sec);
  return Audio::from_vector(generate_sine(samples, frequency_hz, sample_rate, amplitude),
                            sample_rate);
}

inline Audio generate_sine(float frequency_hz, float duration_sec, int sample_rate = 22050,
                           float amplitude = 1.0f) {
  return generate_sine_audio(frequency_hz, sample_rate, duration_sec, amplitude);
}

inline float peak_abs(const std::vector<float>& samples, std::size_t skip = 0) {
  float peak = 0.0f;
  for (std::size_t i = std::min(skip, samples.size()); i < samples.size(); ++i) {
    peak = std::max(peak, std::abs(samples[i]));
  }
  return peak;
}

inline float rms(const float* samples, std::size_t size) {
  if (size == 0) return 0.0f;
  double sum = 0.0;
  for (std::size_t i = 0; i < size; ++i) {
    sum += static_cast<double>(samples[i]) * samples[i];
  }
  return static_cast<float>(std::sqrt(sum / static_cast<double>(size)));
}

inline float rms(const std::vector<float>& samples, std::size_t skip = 0) {
  const std::size_t start = std::min(skip, samples.size());
  if (start == samples.size()) return 0.0f;
  return rms(samples.data() + start, samples.size() - start);
}

inline float rms(const Audio& audio, std::size_t skip = 0) {
  const std::size_t start = std::min(skip, audio.size());
  if (start == audio.size()) return 0.0f;
  return rms(audio.data() + start, audio.size() - start);
}

inline float rms_tail(const std::vector<float>& samples, std::size_t skip) {
  return rms(samples, skip);
}

inline float max_abs_difference(const std::vector<float>& lhs, const std::vector<float>& rhs) {
  const std::size_t count = std::min(lhs.size(), rhs.size());
  float peak = 0.0f;
  for (std::size_t i = 0; i < count; ++i) {
    peak = std::max(peak, std::abs(lhs[i] - rhs[i]));
  }
  return peak;
}

inline void process(rt::ProcessorBase& processor, std::vector<float>& mono) {
  float* channels[] = {mono.data()};
  processor.process(channels, 1, static_cast<int>(mono.size()));
}

inline void process_stereo(rt::ProcessorBase& processor, std::vector<float>& left,
                           std::vector<float>& right) {
  float* channels[] = {left.data(), right.data()};
  processor.process(channels, 2, static_cast<int>(std::min(left.size(), right.size())));
}

}  // namespace sonare::test
