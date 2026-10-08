#pragma once

/// @file gain_pair_glide.h
/// @brief The gain pair a processor is applying, retargeted without ever stepping.

namespace sonare::rt {

/// @brief Holds the pair of gains in force and glides it onto whatever target it is given.
/// @details The applied pair is the only state: a retarget records its distance from the new
///   target as an offset and decays that offset linearly to zero, so a change that interrupts
///   another one starts from where the audio is rather than from either endpoint. The target
///   passed to advance() may itself move (a balance position riding a law curve), which keeps
///   the glide on that curve instead of on the straight line between its endpoints.
class GainPairGlide {
 public:
  struct Pair {
    float first = 1.0f;
    float second = 1.0f;
  };

  /// @brief Lands on @p target at once, with no glide pending.
  void snap(Pair target) noexcept {
    applied_ = target;
    remaining_ = 0;
    weight_ = 0.0f;
  }

  /// @brief Begins gliding from the applied pair onto @p target over @p samples.
  void retarget(Pair target, int samples) noexcept {
    if (samples <= 0) {
      snap(target);
      return;
    }
    offset_ = {applied_.first - target.first, applied_.second - target.second};
    remaining_ = samples;
    weight_ = 1.0f;
    step_ = 1.0f / static_cast<float>(samples);
  }

  /// @brief Advances one sample toward @p target and returns the pair to apply.
  Pair advance(Pair target) noexcept {
    if (remaining_ > 0) {
      weight_ = --remaining_ == 0 ? 0.0f : weight_ - step_;
    }
    applied_ = {target.first + weight_ * offset_.first, target.second + weight_ * offset_.second};
    return applied_;
  }

  /// @brief The pair applied by the latest snap() or advance().
  Pair applied() const noexcept { return applied_; }
  bool gliding() const noexcept { return remaining_ > 0; }

 private:
  Pair applied_{};
  Pair offset_{0.0f, 0.0f};
  float weight_ = 0.0f;
  float step_ = 0.0f;
  int remaining_ = 0;
};

}  // namespace sonare::rt
