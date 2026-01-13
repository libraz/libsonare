#include "effects/normalize.h"

#include <algorithm>
#include <cmath>
#include <vector>

#include "util/exception.h"

namespace sonare {

float peak_db(const Audio& audio) {
  if (audio.empty()) return -std::numeric_limits<float>::infinity();

  float peak = 0.0f;
  const float* data = audio.data();
  for (size_t i = 0; i < audio.size(); ++i) {
    peak = std::max(peak, std::abs(data[i]));
  }

  if (peak < 1e-10f) {
    return -std::numeric_limits<float>::infinity();
  }

  return 20.0f * std::log10(peak);
}

float rms_db(const Audio& audio) {
  if (audio.empty()) return -std::numeric_limits<float>::infinity();

  double sum_sq = 0.0;
  const float* data = audio.data();
  for (size_t i = 0; i < audio.size(); ++i) {
    sum_sq += static_cast<double>(data[i]) * static_cast<double>(data[i]);
  }

  double rms = std::sqrt(sum_sq / static_cast<double>(audio.size()));

  if (rms < 1e-10) {
    return -std::numeric_limits<float>::infinity();
  }

  return 20.0f * std::log10(static_cast<float>(rms));
}

Audio apply_gain(const Audio& audio, float gain_db) {
  if (audio.empty()) return audio;

  float gain_linear = std::pow(10.0f, gain_db / 20.0f);

  std::vector<float> samples(audio.size());
  const float* data = audio.data();

  for (size_t i = 0; i < audio.size(); ++i) {
    samples[i] = data[i] * gain_linear;
    // Clip to [-1, 1]
    samples[i] = std::max(-1.0f, std::min(1.0f, samples[i]));
  }

  return Audio::from_vector(std::move(samples), audio.sample_rate());
}

Audio normalize(const Audio& audio, float target_db) {
  if (audio.empty()) return audio;

  float current_peak = peak_db(audio);

  if (std::isinf(current_peak)) {
    return audio;  // Silent audio, nothing to normalize
  }

  float gain = target_db - current_peak;
  return apply_gain(audio, gain);
}

Audio normalize_rms(const Audio& audio, float target_db) {
  if (audio.empty()) return audio;

  float current_rms = rms_db(audio);

  if (std::isinf(current_rms)) {
    return audio;  // Silent audio
  }

  float gain = target_db - current_rms;
  return apply_gain(audio, gain);
}

std::pair<size_t, size_t> detect_silence_boundaries(const Audio& audio, float threshold_db,
                                                    int frame_length, int hop_length) {
  if (audio.empty()) return {0, 0};

  float threshold_linear = std::pow(10.0f, threshold_db / 20.0f);
  const float* data = audio.data();
  size_t n_samples = audio.size();

  // Find start (first frame above threshold)
  size_t start = 0;
  for (size_t pos = 0; pos + frame_length <= n_samples; pos += hop_length) {
    float rms = 0.0f;
    for (int i = 0; i < frame_length; ++i) {
      rms += data[pos + i] * data[pos + i];
    }
    rms = std::sqrt(rms / static_cast<float>(frame_length));

    if (rms > threshold_linear) {
      start = pos;
      break;
    }
  }

  // Find end (last frame above threshold)
  size_t end = n_samples;
  for (size_t pos = n_samples - frame_length; pos > 0; pos -= hop_length) {
    float rms = 0.0f;
    for (int i = 0; i < frame_length; ++i) {
      rms += data[pos + i] * data[pos + i];
    }
    rms = std::sqrt(rms / static_cast<float>(frame_length));

    if (rms > threshold_linear) {
      end = pos + frame_length;
      break;
    }

    if (pos < static_cast<size_t>(hop_length)) break;
  }

  if (start >= end) {
    return {0, n_samples};  // All silent or invalid, return original bounds
  }

  return {start, end};
}

Audio trim(const Audio& audio, float threshold_db, int frame_length, int hop_length) {
  if (audio.empty()) return audio;

  auto [start, end] = detect_silence_boundaries(audio, threshold_db, frame_length, hop_length);

  if (start == 0 && end == audio.size()) {
    return audio;  // No trimming needed
  }

  return audio.slice_samples(start, end);
}

Audio fade_in(const Audio& audio, float duration_sec) {
  if (audio.empty()) return audio;

  size_t fade_samples = static_cast<size_t>(duration_sec * audio.sample_rate());
  fade_samples = std::min(fade_samples, audio.size());

  std::vector<float> samples(audio.size());
  const float* data = audio.data();

  for (size_t i = 0; i < audio.size(); ++i) {
    float gain = 1.0f;
    if (i < fade_samples) {
      // Cosine fade for smooth transition
      float t = static_cast<float>(i) / static_cast<float>(fade_samples);
      gain = 0.5f * (1.0f - std::cos(M_PI * t));
    }
    samples[i] = data[i] * gain;
  }

  return Audio::from_vector(std::move(samples), audio.sample_rate());
}

Audio fade_out(const Audio& audio, float duration_sec) {
  if (audio.empty()) return audio;

  size_t fade_samples = static_cast<size_t>(duration_sec * audio.sample_rate());
  fade_samples = std::min(fade_samples, audio.size());

  std::vector<float> samples(audio.size());
  const float* data = audio.data();

  size_t fade_start = audio.size() - fade_samples;

  for (size_t i = 0; i < audio.size(); ++i) {
    float gain = 1.0f;
    if (i >= fade_start) {
      size_t fade_pos = i - fade_start;
      float t = static_cast<float>(fade_pos) / static_cast<float>(fade_samples);
      gain = 0.5f * (1.0f + std::cos(M_PI * t));
    }
    samples[i] = data[i] * gain;
  }

  return Audio::from_vector(std::move(samples), audio.sample_rate());
}

}  // namespace sonare
