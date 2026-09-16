#pragma once

/// @file noise_tracker.h
/// @brief Streaming noise PSD trackers for STFT-domain denoisers.

#include <vector>

namespace sonare::mastering::common {

class NoiseTracker {
 public:
  /// @brief How the noise PSD is tracked.
  /// @details Static, Mcra and Imcra follow a minimum over a sliding window, which
  ///   biases the estimate low and needs a compensation factor. Spp
  ///   (Gerkmann-Hendriks 2012) estimates the noise periodogram from a
  ///   speech-presence probability instead: no minimum tracking, no bias factor.
  enum class Mode { Static, Mcra, Imcra, Spp };

  NoiseTracker(int n_bins, int sample_rate, Mode mode = Mode::Imcra, int hop_length = 512);

  void update(const float* power_spectrum);
  const float* noise_psd() const noexcept { return noise_psd_.data(); }
  const float* speech_presence_probability() const noexcept { return speech_presence_.data(); }
  int n_bins() const noexcept { return n_bins_; }
  Mode mode() const noexcept { return mode_; }
  void reset();

  /// @brief Returns every tracked cell to its post-reset value when a non-finite
  ///        value has reached one, so the next frame reseeds instead of carrying
  ///        the poison forward.
  /// @details Not a free recovery, unlike a filter's: reseeding takes the floor
  ///   from whatever frame arrives next, and a minimum-tracking mode then holds
  ///   it for the half second its window spans. The caller owns the counting --
  ///   this reports rather than records, because a tracker is driven once per
  ///   STFT frame and its owner is driven once per block.
  /// @return true when the cells were discarded.
  [[nodiscard]] bool discard_non_finite_state() noexcept;

 private:
  void validate_power(const float* power_spectrum) const;
  void initialize(const float* power_spectrum);
  void update_minima();
  void update_spp(const float* power_spectrum);
  bool tracks_minima() const noexcept { return mode_ != Mode::Spp; }

  int n_bins_ = 0;
  int sample_rate_ = 48000;
  int hop_length_ = 512;
  Mode mode_ = Mode::Imcra;
  int frame_index_ = 0;
  int min_window_frames_ = 75;
  bool initialized_ = false;
  std::vector<float> noise_psd_;
  std::vector<float> speech_presence_;
  // Minimum-statistics state; left empty in Spp mode, which tracks no minimum.
  std::vector<float> smoothed_power_;
  std::vector<float> local_min_;
  std::vector<float> previous_min_;
  std::vector<float> candidate_min_;
  // Spp mode only: time-smoothed presence probability feeding the stagnation guard.
  std::vector<float> smoothed_presence_;
};

}  // namespace sonare::mastering::common
