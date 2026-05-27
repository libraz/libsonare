#pragma once

/// @file noise_tracker.h
/// @brief Streaming noise PSD trackers for STFT-domain denoisers.

#include <vector>

namespace sonare::mastering::common {

class NoiseTracker {
 public:
  enum class Mode { Static, Mcra, Imcra };

  NoiseTracker(int n_bins, int sample_rate, Mode mode = Mode::Imcra, int hop_length = 512);

  void update(const float* power_spectrum);
  const float* noise_psd() const noexcept { return noise_psd_.data(); }
  const float* speech_presence_probability() const noexcept { return speech_presence_.data(); }
  int n_bins() const noexcept { return n_bins_; }
  Mode mode() const noexcept { return mode_; }
  void reset();

 private:
  void validate_power(const float* power_spectrum) const;
  void initialize(const float* power_spectrum);
  void update_minima();

  int n_bins_ = 0;
  int sample_rate_ = 48000;
  int hop_length_ = 512;
  Mode mode_ = Mode::Imcra;
  int frame_index_ = 0;
  int min_window_frames_ = 75;
  bool initialized_ = false;
  std::vector<float> noise_psd_;
  std::vector<float> speech_presence_;
  std::vector<float> smoothed_power_;
  std::vector<float> local_min_;
  std::vector<float> previous_min_;
  std::vector<float> candidate_min_;
};

}  // namespace sonare::mastering::common
