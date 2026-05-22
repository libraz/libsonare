#include "mastering/common/noise_tracker.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace sonare::mastering::common {
namespace {

constexpr float kFloor = 1.0e-12f;
constexpr float kMcraSignalThreshold = 3.0f;
constexpr float kImcraSignalThreshold = 4.6f;

float clamp_probability(float value) { return std::clamp(value, 0.0f, 1.0f); }

}  // namespace

NoiseTracker::NoiseTracker(int n_bins, int sample_rate, Mode mode)
    : n_bins_(n_bins), sample_rate_(sample_rate), mode_(mode) {
  if (n_bins_ <= 0) {
    throw std::invalid_argument("n_bins must be positive");
  }
  if (sample_rate_ <= 0) {
    throw std::invalid_argument("sample_rate must be positive");
  }
  noise_psd_.assign(static_cast<size_t>(n_bins_), kFloor);
  speech_presence_.assign(static_cast<size_t>(n_bins_), 0.0f);
  smoothed_power_.assign(static_cast<size_t>(n_bins_), kFloor);
  local_min_.assign(static_cast<size_t>(n_bins_), kFloor);
  previous_min_.assign(static_cast<size_t>(n_bins_), kFloor);
  candidate_min_.assign(static_cast<size_t>(n_bins_), kFloor);
}

void NoiseTracker::update(const float* power_spectrum) {
  validate_power(power_spectrum);
  if (!initialized_) {
    initialize(power_spectrum);
    return;
  }

  constexpr float kSmooth = 0.95f;
  for (int bin = 0; bin < n_bins_; ++bin) {
    const size_t idx = static_cast<size_t>(bin);
    const float power = std::max(power_spectrum[idx], kFloor);
    smoothed_power_[idx] = kSmooth * smoothed_power_[idx] + (1.0f - kSmooth) * power;
    candidate_min_[idx] = std::min(candidate_min_[idx], smoothed_power_[idx]);
  }
  ++frame_index_;
  update_minima();

  for (int bin = 0; bin < n_bins_; ++bin) {
    const size_t idx = static_cast<size_t>(bin);
    const float min_psd = std::max(std::min(local_min_[idx], previous_min_[idx]), kFloor);
    const float ratio = smoothed_power_[idx] / min_psd;

    float speech_probability = 0.0f;
    float alpha_noise = 0.8f;
    switch (mode_) {
      case Mode::Static:
        speech_probability = ratio > kMcraSignalThreshold ? 1.0f : 0.0f;
        alpha_noise = speech_probability > 0.0f ? 0.995f : 0.92f;
        noise_psd_[idx] =
            std::min(alpha_noise * noise_psd_[idx] + (1.0f - alpha_noise) * power_spectrum[idx],
                     std::max(noise_psd_[idx] * 1.02f, min_psd));
        break;
      case Mode::Mcra:
        speech_probability = clamp_probability((ratio - 1.5f) / (kMcraSignalThreshold - 1.5f));
        alpha_noise = 0.85f + 0.14f * speech_probability;
        noise_psd_[idx] =
            alpha_noise * noise_psd_[idx] + (1.0f - alpha_noise) * smoothed_power_[idx];
        noise_psd_[idx] = std::min(noise_psd_[idx], std::max(min_psd * 1.47f, kFloor));
        break;
      case Mode::Imcra: {
        const bool strong_speech = ratio > kImcraSignalThreshold;
        const float second_min = strong_speech ? std::max(previous_min_[idx], kFloor) : min_psd;
        const float second_ratio = smoothed_power_[idx] / std::max(second_min, kFloor);
        speech_probability =
            clamp_probability((second_ratio - 1.67f) / (kImcraSignalThreshold - 1.67f));
        alpha_noise = strong_speech ? 0.995f : 0.82f + 0.16f * speech_probability;
        const float target = strong_speech ? second_min * 1.47f : smoothed_power_[idx];
        noise_psd_[idx] = alpha_noise * noise_psd_[idx] + (1.0f - alpha_noise) * target;
        noise_psd_[idx] = std::min(noise_psd_[idx], std::max(second_min * 1.47f, kFloor));
        break;
      }
    }

    noise_psd_[idx] = std::max(noise_psd_[idx], kFloor);
    speech_presence_[idx] = speech_probability;
  }
}

void NoiseTracker::reset() {
  frame_index_ = 0;
  initialized_ = false;
  std::fill(noise_psd_.begin(), noise_psd_.end(), kFloor);
  std::fill(speech_presence_.begin(), speech_presence_.end(), 0.0f);
  std::fill(smoothed_power_.begin(), smoothed_power_.end(), kFloor);
  std::fill(local_min_.begin(), local_min_.end(), kFloor);
  std::fill(previous_min_.begin(), previous_min_.end(), kFloor);
  std::fill(candidate_min_.begin(), candidate_min_.end(), kFloor);
}

void NoiseTracker::validate_power(const float* power_spectrum) const {
  if (power_spectrum == nullptr) {
    throw std::invalid_argument("power_spectrum must not be null");
  }
}

void NoiseTracker::initialize(const float* power_spectrum) {
  for (int bin = 0; bin < n_bins_; ++bin) {
    const size_t idx = static_cast<size_t>(bin);
    const float power = std::max(power_spectrum[idx], kFloor);
    noise_psd_[idx] = power;
    smoothed_power_[idx] = power;
    local_min_[idx] = power;
    previous_min_[idx] = power;
    candidate_min_[idx] = power;
    speech_presence_[idx] = 0.0f;
  }
  frame_index_ = 1;
  initialized_ = true;
}

void NoiseTracker::update_minima() {
  if (frame_index_ % min_window_frames_ != 0) {
    for (int bin = 0; bin < n_bins_; ++bin) {
      const size_t idx = static_cast<size_t>(bin);
      local_min_[idx] = std::min(local_min_[idx], candidate_min_[idx]);
    }
    return;
  }

  previous_min_ = local_min_;
  local_min_ = candidate_min_;
  candidate_min_ = smoothed_power_;
}

}  // namespace sonare::mastering::common
