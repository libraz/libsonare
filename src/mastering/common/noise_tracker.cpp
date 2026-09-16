#include "mastering/common/noise_tracker.h"

#include <algorithm>
#include <cmath>

#include "util/exception.h"

namespace sonare::mastering::common {
namespace {

// PSD floor keeps recursive estimators positive and log/division safe.
constexpr float kFloor = 1.0e-12f;
constexpr float kMcraSignalThreshold = 3.0f;
constexpr float kImcraSignalThreshold = 4.6f;
// Static mode is a simplified soft-decision tracker; it gets its own threshold so a
// future tweak to the MCRA tuning cannot silently change Static-mode behavior.
constexpr float kStaticSignalThreshold = 3.0f;

// Gerkmann-Hendriks 2012, fixed a priori SNR under H1: 15 dB, where the estimator
// is unbiased. That is what lets the Spp path drop the minimum-statistics bias
// factor the MCRA/IMCRA paths multiply in.
constexpr float kSppPriorSnr = 31.6227766f;
constexpr float kSppSnrWeight = kSppPriorSnr / (1.0f + kSppPriorSnr);

// Equal speech/noise priors, so the likelihood ratio carries odds of 1 and the
// decision needs no fitted threshold.
constexpr float kSppSpeechPrior = 0.5f;
constexpr float kSppPriorOdds = (1.0f - kSppSpeechPrior) / kSppSpeechPrior;

// Noise-PSD smoothing and the paper's stagnation guard: a bin whose smoothed
// presence sits above the limit would freeze the estimate, so its probability is
// capped there for one frame rather than being allowed to reach 1.
constexpr float kSppSmoothing = 0.8f;
constexpr float kSppPresenceSmoothing = 0.9f;
constexpr float kSppStagnationLimit = 0.99f;

float clamp_probability(float value) { return std::clamp(value, 0.0f, 1.0f); }

}  // namespace

NoiseTracker::NoiseTracker(int n_bins, int sample_rate, Mode mode, int hop_length)
    : n_bins_(n_bins), sample_rate_(sample_rate), hop_length_(hop_length), mode_(mode) {
  if (n_bins_ <= 0) {
    throw SonareException(ErrorCode::InvalidParameter, "n_bins must be positive");
  }
  if (sample_rate_ <= 0) {
    throw SonareException(ErrorCode::InvalidParameter, "sample_rate must be positive");
  }
  if (hop_length_ <= 0) {
    throw SonareException(ErrorCode::InvalidParameter, "hop_length must be positive");
  }
  // Scale the minimum-tracking window to roughly 0.5 s of frames so the noise
  // floor adapts on a fixed time scale regardless of hop size / sample rate.
  constexpr double kMinWindowSeconds = 0.5;
  min_window_frames_ =
      std::max(10, static_cast<int>(std::lround(kMinWindowSeconds * sample_rate_ / hop_length_)));
  noise_psd_.assign(static_cast<size_t>(n_bins_), kFloor);
  speech_presence_.assign(static_cast<size_t>(n_bins_), 0.0f);
  if (tracks_minima()) {
    smoothed_power_.assign(static_cast<size_t>(n_bins_), kFloor);
    local_min_.assign(static_cast<size_t>(n_bins_), kFloor);
    previous_min_.assign(static_cast<size_t>(n_bins_), kFloor);
    candidate_min_.assign(static_cast<size_t>(n_bins_), kFloor);
  } else {
    smoothed_presence_.assign(static_cast<size_t>(n_bins_), 0.0f);
  }
}

void NoiseTracker::update(const float* power_spectrum) {
  validate_power(power_spectrum);
  if (!initialized_) {
    initialize(power_spectrum);
    return;
  }
  if (mode_ == Mode::Spp) {
    update_spp(power_spectrum);
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
        speech_probability = ratio > kStaticSignalThreshold ? 1.0f : 0.0f;
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
      case Mode::Spp:
        break;  // Returned above; listed so a new mode still trips -Wswitch.
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
  std::fill(smoothed_presence_.begin(), smoothed_presence_.end(), 0.0f);
}

void NoiseTracker::validate_power(const float* power_spectrum) const {
  if (power_spectrum == nullptr) {
    throw SonareException(ErrorCode::InvalidParameter, "power_spectrum must not be null");
  }
}

void NoiseTracker::initialize(const float* power_spectrum) {
  const bool minima = tracks_minima();
  for (int bin = 0; bin < n_bins_; ++bin) {
    const size_t idx = static_cast<size_t>(bin);
    const float power = std::max(power_spectrum[idx], kFloor);
    noise_psd_[idx] = power;
    speech_presence_[idx] = 0.0f;
    if (minima) {
      smoothed_power_[idx] = power;
      local_min_[idx] = power;
      previous_min_[idx] = power;
      candidate_min_[idx] = power;
    } else {
      smoothed_presence_[idx] = 0.0f;
    }
  }
  frame_index_ = 1;
  initialized_ = true;
}

/// Gerkmann-Hendriks 2012: an MMSE noise-periodogram estimate weighted by the
/// speech-presence probability. No minimum tracking, so no window length and no
/// bias compensation; every constant below is the paper's rather than fitted here.
void NoiseTracker::update_spp(const float* power_spectrum) {
  ++frame_index_;
  for (int bin = 0; bin < n_bins_; ++bin) {
    const size_t idx = static_cast<size_t>(bin);
    const float power = std::max(power_spectrum[idx], kFloor);
    const float posterior_snr = power / std::max(noise_psd_[idx], kFloor);

    // 1/(1+L') rather than L/(1+L): the exponent is never positive, so a loud bin
    // saturates at 1 instead of overflowing exp() and needing a clamp.
    const float presence = 1.0f / (1.0f + kSppPriorOdds * (1.0f + kSppPriorSnr) *
                                              std::exp(-posterior_snr * kSppSnrWeight));

    smoothed_presence_[idx] =
        kSppPresenceSmoothing * smoothed_presence_[idx] + (1.0f - kSppPresenceSmoothing) * presence;
    const float gated = smoothed_presence_[idx] > kSppStagnationLimit
                            ? std::min(presence, kSppStagnationLimit)
                            : presence;

    // The observation is noise under H0 and the running estimate is the best guess
    // under H1, so the posterior mean interpolates between them.
    const float periodogram = gated * noise_psd_[idx] + (1.0f - gated) * power;
    noise_psd_[idx] =
        std::max(kSppSmoothing * noise_psd_[idx] + (1.0f - kSppSmoothing) * periodogram, kFloor);
    speech_presence_[idx] = clamp_probability(gated);
  }
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
