#pragma once

/// @file envelope_follower.h
/// @brief Attack/release envelope detector.

namespace sonare::rt {

class EnvelopeFollower {
 public:
  void prepare(double sample_rate, float attack_ms = 10.0f, float release_ms = 100.0f);
  void reset(float value = 0.0f);
  float process(float input);
  float smooth_bidirectional(float target, bool attack_when_decreasing = true);
  float smooth_bidirectional(float target, float release_coeff, bool attack_when_decreasing);
  float value() const { return envelope_; }
  /// @brief Returns a non-finite envelope to rest (see util/non_finite_state.h).
  /// @details The envelope is recursive, so an owner calls this once per block;
  ///          it cannot be reached through @ref reset, which a live stream has
  ///          no reason to call.
  /// @return true when the envelope was discarded.
  [[nodiscard]] bool discard_if_non_finite() noexcept;

 private:
  double sample_rate_ = 48000.0;
  float attack_coeff_ = 1.0f;
  float release_coeff_ = 1.0f;
  float envelope_ = 0.0f;
};

}  // namespace sonare::rt
