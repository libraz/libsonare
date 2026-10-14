#pragma once

/// @file gain_pair_glide.h
/// @brief The gain (or gain pair) a processor is applying, retargeted without ever stepping.

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
  /// @brief Number of samples left before the current target is reached.
  int remaining_samples() const noexcept { return remaining_; }
  bool gliding() const noexcept { return remaining_ > 0; }

 private:
  Pair applied_{};
  Pair offset_{0.0f, 0.0f};
  float weight_ = 0.0f;
  float step_ = 0.0f;
  int remaining_ = 0;
};

/// @brief Single-gain sibling of GainPairGlide with the same retarget contract.
/// @details Once settled, advance() returns its target exactly, so a stage whose gain is
///   unchanged renders bit-identically to one that multiplies by the target directly.
class GainGlide {
 public:
  /// @brief Lands on @p target at once, with no glide pending.
  void snap(float target) noexcept {
    applied_ = target;
    remaining_ = 0;
  }

  /// @brief Begins gliding from the applied gain onto @p target over @p samples.
  void retarget(float target, int samples) noexcept {
    if (samples <= 0) {
      snap(target);
      return;
    }
    offset_ = applied_ - target;
    remaining_ = samples;
    step_ = 1.0f / static_cast<float>(samples);
  }

  /// @brief Advances one sample toward @p target and returns the gain to apply.
  float advance(float target) noexcept {
    if (remaining_ == 0) {
      applied_ = target;
      return applied_;
    }
    // The weight is taken from the count, so a long glide accumulates no rounding.
    const float weight = static_cast<float>(--remaining_) * step_;
    applied_ = target + weight * offset_;
    return applied_;
  }

  /// @brief The gain applied by the latest snap() or advance().
  float applied() const noexcept { return applied_; }
  /// @brief Number of samples left before the current target is reached.
  int remaining_samples() const noexcept { return remaining_; }
  bool gliding() const noexcept { return remaining_ > 0; }

 private:
  float applied_ = 1.0f;
  float offset_ = 0.0f;
  float step_ = 0.0f;
  int remaining_ = 0;
};

}  // namespace sonare::rt
